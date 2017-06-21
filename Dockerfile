FROM alpine:3.6 AS builder

RUN set -ex; \
	\
	apk add --no-cache --virtual .build-deps \
		coreutils \
		gcc \
		linux-headers \
		make \
		musl-dev \
	;

RUN mkdir -p /usr/src/peps
COPY . /usr/src/peps
RUN make -C /usr/src/peps/

### COPIED FROM https://github.com/docker-library/redis/blob/f131897d1f6305b8f6ebae695344f19c523e2c65/3.2/alpine/Dockerfile
### Adapted slightly. Replace with redis:4 when it's available.

FROM alpine:3.6

# add our user and group first to make sure their IDs get assigned consistently, regardless of whatever dependencies get added
RUN addgroup -S redis && adduser -S -G redis redis

# grab su-exec for easy step-down from root
RUN apk add --no-cache 'su-exec>=0.2'

ENV REDIS_VERSION 4.0-RC3
ENV REDIS_DOWNLOAD_URL https://github.com/antirez/redis/archive/4.0-rc3.tar.gz
ENV REDIS_DOWNLOAD_SHA bc948bcb32dc4ba43412dd791b4bb48c64de9debb797346b9c9f1b2cb98f96f4

RUN apk update && apk add ca-certificates && update-ca-certificates && apk add openssl

# for redis-sentinel see: http://redis.io/topics/sentinel
RUN set -ex; \
	\
	apk add --no-cache --virtual .build-deps \
		coreutils \
		gcc \
		linux-headers \
		make \
		musl-dev \
        curl \
	; \
	\
	curl -L -o redis.tar.gz "$REDIS_DOWNLOAD_URL"; \
	echo "$REDIS_DOWNLOAD_SHA *redis.tar.gz" | sha256sum -c -; \
	mkdir -p /usr/src/redis; \
	tar -xzf redis.tar.gz -C /usr/src/redis --strip-components=1; \
	rm redis.tar.gz; \
	\
# disable Redis protected mode [1] as it is unnecessary in context of Docker
# (ports are not automatically exposed when running inside Docker, but rather explicitly by specifying -p / -P)
# [1]: https://github.com/antirez/redis/commit/edd4d555df57dc84265fdfb4ef59a4678832f6da
	grep -q '^#define CONFIG_DEFAULT_PROTECTED_MODE 1$' /usr/src/redis/src/server.h; \
	sed -ri 's!^(#define CONFIG_DEFAULT_PROTECTED_MODE) 1$!\1 0!' /usr/src/redis/src/server.h; \
	grep -q '^#define CONFIG_DEFAULT_PROTECTED_MODE 0$' /usr/src/redis/src/server.h; \
# for future reference, we modify this directly in the source instead of just supplying a default configuration flag because apparently "if you specify any argument to redis-server, [it assumes] you are going to specify everything"
# see also https://github.com/docker-library/redis/issues/4#issuecomment-50780840
# (more exactly, this makes sure the default behavior of "save on SIGTERM" stays functional by default)
	\
	make -C /usr/src/redis -j "$(nproc)"; \
	make -C /usr/src/redis install; \
	\
	rm -r /usr/src/redis; \
	\
	apk del .build-deps

RUN mkdir /data && chown redis:redis /data
VOLUME /data
WORKDIR /data

ADD https://raw.githubusercontent.com/docker-library/redis/master/3.2/alpine/docker-entrypoint.sh /usr/local/bin/
RUN chmod 755  /usr/local/bin/docker-entrypoint.sh
ENTRYPOINT ["docker-entrypoint.sh"]

EXPOSE 6379
CMD ["redis-server"]

### END COPIED


COPY --from=builder /usr/src/peps/peps.so /usr/local/bin/
CMD ["redis-server", "--loadmodule", "/usr/local/bin/peps.so"]