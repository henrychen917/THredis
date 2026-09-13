**FIXED (teardown UAF): a dead client now remains allocated through both send and recv CQEs, and a dead recv completion can only drop its pin; it cannot parse or dispatch.**
**FIXED (executor spin): park checks are role-specific, so an executor no longer treats an undrainable client/ready signal as perpetual inbound work.**

# Size-gated zero-copy GET replies

## Scope and switch

`--zc-min N` enables borrowed GET replies for string values whose length is at least `N` bytes.
`16384` is the default. `0` disables the feature. The disabled GET path is one cold
`zc_min != 0` branch. Integer-encoded values and values below the gate use the original contiguous
reply path.

"Zero-copy" here means no application-level copy of the value bytes. The executor formats only the
RESP bulk header, and the io thread gives `sendmsg` an iovec pointing directly at FlatStore value
memory. Headers, ordinary replies, and queue-boundary staging may still be copied. This does not use
kernel `MSG_ZEROCOPY`.

## Borrow lifetime

1. GET runs on the shard-owning executor. It reads the value pointer and length, writes only
   `$<len>\r\n` through `Op::Sink`, registers the pointer in that shard's FlatStore borrow registry,
   and publishes `{zc_ptr, zc_len, zc_shard}` with the normal `OpState::Done` release store.
2. The io thread observes `Done`, retires the op in ROB order, and moves the descriptor into a
   per-connection `BORROW` segment. The op may then be reset; the segment and store registry own the
   remaining protocol state.
3. A borrow ends only after its entire `BORROW` segment has completed on the socket, or after
   teardown determines that the segment will never be sent. The io thread copies `{shard, ptr}` into
   a release handoff; it never calls FlatStore itself.
4. The release travels through a per-producer SPSC `ReleaseChan` to the shard's owning executor.
   That executor calls `FlatStore::unborrow`. A full channel cannot drop a release: the io thread
   retains it in `pending_releases_` and retries until the channel accepts it.

FlatStore is still single-owner and contains no new atomics. Its registry is executor-local and
maps value pointer to a reference count plus an optional retired `KvObj`. Pointer identity cannot
have an ABA collision: the allocation named by a live pointer is either still in the table or held
on the pending-free path, so it cannot be reused before the final release.

Embedded and external values use the same retention rule. On overwrite or DEL, FlatStore retains
the whole old `KvObj`; that keeps embedded bytes in the object and keeps an external value block
owned by the object's existing destructor. The final unborrow frees both correctly. Logical store
size changes immediately, while `resident_estimate()` includes pending-free bytes until physical
reclamation.

Every replace/erase reaches `retire_obj`. With no outstanding borrows, its added hot-path cost is
one `outstanding_borrows_ == 0` branch and the object is freed immediately. Same-size in-place SET
has a separate guarded check: if its exact value pointer is borrowed, it declines the in-place path
so replacement can retain the old allocation. No borrowed bytes are ever mutated in place.

## Per-connection ordering

The ordinary double-buffered path is unchanged until the first borrowed reply. A connection's
segment queue is an inline-first ordered vector of:

- `BUF`: owned immutable bytes, used for sealed ordinary output and the borrowed reply's staged
  header;
- `BORROW`: a non-owning FlatStore value pointer, length, and return shard;
- `STATIC`: immortal immutable bytes; the GET trailer uses one shared two-byte CRLF object.

When a zero-copy op retires, any older fill-buffer bytes are first sealed into a `BUF` segment, then
the io thread appends `[header BUF][value BORROW][CRLF STATIC]`. If a legacy send buffer is already
in flight, it remains ahead of the queue. Once the queue is non-empty, every later non-zero-copy
reply is appended as a `BUF` segment until the queue drains. Therefore neither ROB completion order,
continued retirement during a send, nor a short write can place newer bytes before the borrow.

`BUF` payload blocks are independent of segment metadata and write buffers. Growing the inline
segment vector cannot move payload bytes named by the kernel. The `msghdr` and fixed 16-entry iovec
array live in `Client`, so they also remain valid until the CQE.

The direct-reply small path keeps its original eligibility and commit behavior for non-zero-copy
replies. A gated GET may use the direct region for its header; retirement copies that staged header
into its `BUF` segment instead of committing it into the legacy byte stream. `ReadyMask` signaling,
ROB retirement, and the existing connection quiescence predicate are unchanged.

## Partial sends

Only the connection-owning io thread submits sends, and `send_inflight` still permits exactly one
send per connection. Segment-mode `pump()` builds at most 16 iovecs beginning at the queue's saved
wire frontier. Submissions are also capped at Linux `MAX_RW_COUNT`, because CQE `res` is signed.

The frontier is `(head segment index, byte offset within head)`. A positive CQE consumes exactly
`res` bytes across segment boundaries. A fully consumed `BUF` is freed; a fully consumed `BORROW`
posts exactly one release; `STATIC` needs no action. A short CQE leaves the exact segment and offset
for the next `sendmsg`. Sending a full 16-iovec window with more queued segments advances to the
next window without being classified as a short write.

`zc_bytes` counts borrowed bytes actually covered by successful CQEs, not submitted bytes, so
retries cannot double-count them. `zc_sends` counts submissions whose current iovec window includes
a borrow, and `zc_releases` counts borrow segments handed to the release path.

Consequently the failing run's `zc_sends=24`, `short=16`, `zc_releases=8` does not describe 24
borrows with 16 missing releases. It describes eight borrow segments, each submitted three times on
average because of the 16 short completions, with all eight segments handed to release exactly once.

## Teardown and both release paths

Normal completion releases a borrow when its segment becomes fully sent. Teardown releases every
remaining borrow even if its bytes were never submitted.

If no send is in flight, `close_client` immediately transfers all queued borrow descriptors to the
release path. If `sendmsg` is in flight, its memory must remain pinned through the CQE. The client is
marked dead and enters the existing two-prologue deferred-free pipeline, but dead send CQEs are no
longer ignored: the completion consumes any sent prefix and releases all remaining segments. Two
prologues remain the minimum grace period; physical deletion is extended while either a send or recv
CQE is outstanding. A corpse is never freed while its `msghdr`, iovecs, borrowed value, read buffer,
or `Client*` CQE tag may still be used by the kernel.

The failing close path had guarded only `send_inflight`. A send error could close a client while a
recv was armed, and the deferred reaper could reuse the allocation before that recv CQE arrived. A
late positive recv then parsed and dispatched through the recycled object; a late error could close
the unrelated healthy connection that reused it. Dead recv completions now clear `recv_armed` and
return without touching protocol state, and the reaper requires both network pins to be clear.

The old generic park predicate also combined all channel families even though an executor drains
only task/release channels and an io loop drains only client/ready channels. Once corrupted teardown
routed a signal to the wrong role, that thread observed permanent inbound work it could never pop
and burned a core forever. Park rechecks now use the same role-specific channel set as their drains.

The original teardown quiescence conditions remain `ROB quiesced && no retire-channel claim`.
Segment cleanup happens after that fence and does not weaken or replace it.

`pending_releases_` is owned by the io loop, not by a connection, and is retried at the top of every
io pass plus the pre-park sweep. Closing or deleting the originating connection cannot strand a
queued release. A release leaves the deque only after the shard owner's SPSC channel accepts it.

The short-write frontier was not the fault: `consume_segments` charges bytes only against the
current head, releases a `BORROW` only when its offset reaches its exact length, pops it, and resumes
the next submission at offset zero of the next head. Teardown walks only the still-queued segments,
so a borrow popped by completion cannot be released again and a partial head cannot be skipped.

## Files touched

- `src/core/config.h`: strict `--zc-min` parsing, default, and help text.
- `src/core/server.h`: configuration storage and propagation of the gate to shards.
- `src/core/shard.h`: immutable per-shard zero-copy threshold.
- `src/exec/op.h`: borrow descriptor and reset behavior.
- `src/cmd/t_string.cc`: gated GET header-only formatting and borrow registration.
- `src/net/resp.h`: bulk-header-only formatter.
- `src/store/flatstore.h`: borrow registry, mutation retention, pending-free reclamation, and
  resident-byte accounting.
- `src/core/thread.h`: release item, per-producer release channels, notification, draining, pressure,
  and park checks.
- `src/core/ex_loop.h`: owner-thread release draining and `unborrow` calls.
- `src/net/conn.h`: inline segment queue, stable BUF ownership, iovec/msghdr storage, and partial-send
  frontier.
- `src/net/wb.h`: ordered retirement staging, send/sendmsg pumping, CQE consumption, release-on-send,
  release-on-teardown, and zero-copy statistics.
- `src/core/io_loop.h`: release routing/retry, dead-send completion handling, and deferred corpse
  retention through an outstanding send CQE.
- `DESIGN-ZC.md`: this invariant and ownership record.
