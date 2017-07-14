#define REDISMODULE_EXPERIMENTAL_API
#include "redismodule.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "list.h"

#define KEY_EXPIRE (7 * 24 * 3600)
#define MAX_TIMEOUT (3600)

struct blocked_subscriber {
    char *name;
    RedisModuleBlockedClient *bc;
    struct blocked_subscriber *prev, *next;
};

static struct blocked_subscriber *blocked;

static char* str(RedisModuleString *input) {
    size_t len;
    const char *s = RedisModule_StringPtrLen(input, &len);
    char *p = RedisModule_Alloc(len + 1);
    memcpy(p, s, len);
    *(p + len) = 0;
    return p;
}

static char* reply_str(RedisModuleCallReply *reply) {
    size_t len;
    const char *s = RedisModule_CallReplyStringPtr(reply, &len);
    char *p = RedisModule_Alloc(len + 1);
    memcpy(p, s, len);
    *(p + len) = 0;
    return p;
}

// SUBSCRIBE <subscription> <topic>
static int subscribe(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }
    RedisModule_AutoMemory(ctx);

    char *topic = str(argv[2]);
    RedisModuleString *key = RedisModule_CreateStringPrintf(ctx, "peps:topic:%s", topic);
    RedisModule_Free(topic);

    RedisModuleCallReply *reply = RedisModule_Call(ctx, "SADD", "ss", key, argv[1]);
    long long val = RedisModule_CallReplyInteger(reply);

    RedisModule_ReplyWithLongLong(ctx, val);
    return REDISMODULE_OK;
}

// UNSUBSCRIBE <subscription> <topic> 
static int unsubscribe(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }
    RedisModule_AutoMemory(ctx);

    char *topic = str(argv[2]);
    RedisModuleString *key = RedisModule_CreateStringPrintf(ctx, "peps:topic:%s", topic);
    RedisModule_Free(topic);

    RedisModuleCallReply *reply = RedisModule_Call(ctx, "SREM", "ss", key, argv[1]);
    long long val = RedisModule_CallReplyInteger(reply);

    RedisModule_ReplyWithLongLong(ctx, val);
    return REDISMODULE_OK;
}

static struct blocked_subscriber *get_blocked(RedisModuleCtx *ctx, char *subscriber) {
    struct blocked_subscriber *found;
    for (found = blocked; found; found = found->next) {
        if (strcmp(found->name, subscriber) == 0) {
            LIST_DELETE(blocked, found);
            return found;
        }
    }
    return NULL;
}

// PUBLISH <topic> <data>
static int publish(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }
    RedisModule_AutoMemory(ctx);

    char *topic = str(argv[1]);
    RedisModuleString *key = RedisModule_CreateStringPrintf(ctx, "peps:topic:%s", topic);

    RedisModuleCallReply *subscriptions = RedisModule_Call(ctx, "SMEMBERS", "s", key);
    size_t count = RedisModule_CallReplyLength(subscriptions);

    for (int idx = 0; idx < count; idx++) {
        RedisModuleCallReply *reply = RedisModule_CallReplyArrayElement(subscriptions, idx);
        char *subscription = reply_str(reply);
        
        long long message_id = RedisModule_CallReplyInteger(RedisModule_Call(ctx, "INCR", "c", "peps:msgid"));
        RedisModuleString *msg_key = RedisModule_CreateStringPrintf(ctx, "peps:msg:%d", message_id);
        RedisModule_Call(ctx, "SETEX", "sls", msg_key, KEY_EXPIRE, argv[2]);

        RedisModuleString *msg_ref = RedisModule_CreateStringPrintf(ctx, "%s:%s:%d", subscription, topic, message_id);
        RedisModuleString *queue = RedisModule_CreateStringPrintf(ctx, "peps:sub:%s:q", subscription);
        RedisModule_Call(ctx, "RPUSH", "ss!", queue, msg_ref);

        // Wake up anyone who is blocked on this subscription
        struct blocked_subscriber *blocked;
        while ((blocked = get_blocked(ctx, subscription))) {
            RedisModule_UnblockClient(blocked->bc, NULL);
            RedisModule_Free(blocked->name);
            RedisModule_Free(blocked);
        }
        RedisModule_Free(subscription);
    }
    RedisModule_Free(topic);

    RedisModule_ReplyWithLongLong(ctx, count);
    return REDISMODULE_OK;
}

static int Block_Reply(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return RedisModule_ReplyWithNull(ctx);
}

static void Block_FreeData(void *privdata) {
}

static void drain_inflight(RedisModuleCtx *ctx) {
    char timeoutbuf[40];
    snprintf(timeoutbuf, sizeof(timeoutbuf), "%f", (double)time(NULL));
    RedisModuleCallReply *expired = RedisModule_Call(ctx, "ZRANGEBYSCORE", "ccc", "peps:inflight", "0", timeoutbuf);
    size_t count = RedisModule_CallReplyLength(expired);
    for (int i = 0; i < count; i++) {
        RedisModuleCallReply *elem = RedisModule_CallReplyArrayElement(expired, i);
        RedisModuleString *msg_ref = RedisModule_CreateStringFromCallReply(elem);
        char *sub_ptr = str(msg_ref);
        char *saveptr;
        char *subscription = strtok_r(sub_ptr, ":", &saveptr);
        RedisModuleString *queue = RedisModule_CreateStringPrintf(ctx, "peps:sub:%s:q", subscription);
        RedisModule_Free(sub_ptr);

        RedisModule_Call(ctx, "RPUSH", "ss!", queue, msg_ref);

        // Wake up anyone who is blocked
        struct blocked_subscriber *blocked;
        while ((blocked = get_blocked(ctx, subscription))) {
            RedisModule_UnblockClient(blocked->bc, NULL);
            RedisModule_Free(blocked->name);
            RedisModule_Free(blocked);
        }
    }
    RedisModule_Call(ctx, "ZREMRANGEBYSCORE", "ccc", "peps:inflight", "0", timeoutbuf);
}

static void mark_inflight(RedisModuleCtx *ctx, RedisModuleString *receipt, int timeout) {
    char timeoutbuf[40];
    snprintf(timeoutbuf, sizeof(timeoutbuf), "%f", (double)((unsigned long)time(NULL) + timeout));
    RedisModule_Call(ctx, "ZADD", "ccs", "peps:inflight", timeoutbuf, receipt);
}

// FETCH <subscription> [timeout = 10]
static int fetch(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) {
        return RedisModule_WrongArity(ctx);
    }
    long long timeout = 10;
    if (argc >= 3) {
        RedisModule_StringToLongLong(argv[2], &timeout);
        if (timeout <= 0 || timeout > MAX_TIMEOUT) {
            timeout = MAX_TIMEOUT;
        }
    }
    RedisModule_AutoMemory(ctx);

    drain_inflight(ctx);

    char *subscription = str(argv[1]);
    RedisModuleString *queue = RedisModule_CreateStringPrintf(ctx, "peps:sub:%s:q", subscription);

    RedisModuleCallReply *reply = RedisModule_Call(ctx, "LPOP", "s", queue);
    if (RedisModule_CallReplyType(reply) == REDISMODULE_REPLY_NULL) {
        // Already someone listening? Re-use
        struct blocked_subscriber *me = get_blocked(ctx, subscription);
        if (!me) {
            me = RedisModule_Alloc(sizeof(*me));
            me->name = str(argv[1]);
        }

        me->bc = RedisModule_BlockClient(ctx, Block_Reply, Block_Reply, Block_FreeData, 10000);
        LIST_APPEND(blocked, me);
    } else if (RedisModule_CallReplyType(reply) == REDISMODULE_REPLY_STRING) {
        RedisModuleString *msg_ref = RedisModule_CreateStringFromCallReply(reply);
        char *p = str(msg_ref);
        char *saveptr;
        char *subscription_ = strtok_r(p, ":", &saveptr);
        char *topic = strtok_r(NULL, ":", &saveptr);
        char *message_id = strtok_r(NULL, ":", &saveptr);

        RedisModuleString *msg_key = RedisModule_CreateStringPrintf(ctx, "peps:msg:%s", message_id);
        RedisModuleString *payload = RedisModule_CreateStringFromCallReply(RedisModule_Call(ctx, "GET", "s", msg_key));

        if (!payload) {
            // Expired or missing!
            RedisModule_ReplyWithNull(ctx);  
        } else {
            mark_inflight(ctx, msg_ref, timeout);
            RedisModule_ReplyWithArray(ctx, 3);
            RedisModule_ReplyWithString(ctx, msg_ref);
            RedisModule_ReplyWithString(ctx, payload);
            RedisModule_ReplyWithStringBuffer(ctx, topic, strlen(topic));
        }
        RedisModule_Free(p);
    } else {
        return RedisModule_ReplyWithError(ctx, "ERR queue has unknown value");
    }

    RedisModule_Free(subscription);
    return REDISMODULE_OK;
}

// ACK <message-receipt> []
static int ack(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) {
        return RedisModule_WrongArity(ctx);
    }
    RedisModule_AutoMemory(ctx);

    for (int i = 1; i < argc; i++) {
        char *msg_ref = str(argv[i]);
        const char *message_id = strrchr(msg_ref, ':');
        if (message_id != NULL) {
            RedisModuleString *msg_key = RedisModule_CreateStringPrintf(ctx, "peps:msg:%s", message_id + 1);
            RedisModule_Call(ctx, "UNLINK", "s", msg_key);
            RedisModule_Call(ctx, "ZREM", "cs", "peps:inflight", argv[i]);
        }
        RedisModule_Free(msg_ref);
    }
    return RedisModule_ReplyWithNull(ctx);
}

// NACK <message-receipt> []
static int nack(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) {
        return RedisModule_WrongArity(ctx);
    }
    RedisModule_AutoMemory(ctx);

    for (int i = 1; i < argc; i++) {
        char *sub_ptr = str(argv[i]);
        char *saveptr;
        char *subscription = strtok_r(sub_ptr, ":", &saveptr);
        RedisModuleString *dlq = RedisModule_CreateStringPrintf(ctx, "peps:sub:%s:dl", subscription);
        RedisModuleCallReply *reply = RedisModule_Call(ctx, "ZREM", "cs", "peps:inflight", argv[i]);
        if (RedisModule_CallReplyInteger(reply) != 0) {
            RedisModule_Call(ctx, "RPUSH", "ss!", dlq, argv[i]);
        }
        RedisModule_Free(sub_ptr);
    }
    return RedisModule_ReplyWithNull(ctx);
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (RedisModule_Init(ctx,"peps", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"peps.subscribe", subscribe, "write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"peps.unsubscribe", unsubscribe, "write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"peps.publish", publish, "write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"peps.fetch", fetch, "write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"peps.ack", ack, "write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"peps.nack", nack, "write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
