# Redis Persistent Pub Sub

This Redis module (available in Redis 4.x) adds a new queue implementation different from the Redis Pub/Sub (i.e. SUBSCRIBE, PUBLISH etc).

This new implementation enables clients to disconnect from the server and retrieve messages whenever it reconnects. It also provides message ack:ing (and nack:ing), as well as retries for messages that are not acked within the timeout specified when fetching messages.

More documentation will follow.
