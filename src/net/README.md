# Network

Connections, RESP parsing/encoding, the per-connection reorder buffer, writeback,
io_uring/epoll, TLS, and Unix listeners live here. Socket and reply-buffer
lifetimes are separate from the shard owners that execute commands.
