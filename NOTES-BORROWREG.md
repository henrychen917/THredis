# Borrow-registry battery: diagnosis and design

## Diagnosis

This is case 1: the product borrow path still stages borrowed replies in the connection's segment
queue, but the battery's inference from that queue is not sound.

`WriteBack::serve_impl` seals ordinary buffered output, appends the RESP header as a buffer segment,
appends the value with `append_borrow_segment`, and appends the trailing CRLF as a static segment.
Both the io_uring and epoll pumps build an iovec from that queue.  The send-completion paths consume
accepted segments and return each consumed borrow to its owner shard.  Therefore `CLIENT LIST oll`
does see borrowed replies while they are *unsent*, but it stops seeing them as soon as the kernel
accepts them.  Kernel acceptance can precede client consumption by the whole server send-buffer
capacity.  A small client `SO_RCVBUF`/`TCP_WINDOW_CLAMP` does not make one 400 x 8 KiB pipeline
larger than that capacity, so every holder can reach `oll=0` with its replies still buffered below
the application.

This rules out case 2: borrowed plaintext replies do pass through `segments_`; `oll` is simply the
wrong connection-side proxy for the shard-side lifetime being tested.

## Fix direction

`FlatStore` already owns the exact owner-thread counter, `outstanding_borrows_`, and exposes it as
`outstanding_borrows()`.  Add a boot-gated `DEBUG BORROWCOUNT` command that scatters one cold task to
every shard owner, reads that existing counter there, and returns the exact sum.  This introduces no
new counter and no ordinary borrow/reply-path work; the existing DEBUG permission gate remains the
only way to reach it.

The battery will use `DEBUG BORROWCOUNT` for both sides of a non-vacuity check:

- with no holders, wait for releases to settle and require exactly zero;
- pipeline 9,600 distinct 8 KiB values (about 75 MiB of replies) onto one unread connection to
  exceed a per-socket kernel send buffer, then require a substantial non-zero count both before
  and after the busy measurement while that holder remains parked;
- after closing holders, wait for teardown releases and require exactly zero again.

Distinct keys remain important.  Repeating one value raises reference count but does not grow the
registry's set of pointer entries, while the performance contract is about registry growth.

Validation geometry remains `--shards 1 --zc-min 64 --enable-debug-command yes`, with the 32-byte
plain probe below the threshold and the 128-byte borrowed probe above it.  The gate lane owns the
actual build/server run; this lane performs no build and starts no server or test script.

## Aside

`CLIENT LIST`'s `obl=` field is hardcoded to `0` in `src/cmd/t_server.cc` while `oll=` reads the real
segment-queue length.  INFO/CLIENT-field correctness belongs to its separate lane, so this change
does not alter `obl=`.
