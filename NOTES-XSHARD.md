# Cross-shard multi-key commands

Cross-shard commands use an owner-only scatter/gather core. They are deliberately not atomic across
shards: every `FlatStore` still has one executor owner, no store lock or store atomic exists, and a
concurrent client may observe a partially applied multi-shard write. Epoch MVCC is a later layer;
the arena header reserves its attachment point now.

## One-arena group state

IO validates the command, computes all registry-defined key positions and hashes, and resolves every
owner before allocating group state. If more than one owner is present, it knows both `nkeys` and the
number of touched shards (`nsub`), calculates the exact layout, and makes one allocation. Every
coordinator side array is then carved at an aligned, fixed offset:

```text
low address
+---------------------------+
| ScatterState header       | pending, phase, kind, owner_io
|                           | epoch (reserved MVCC seam)
+---------------------------+
| ShardGroup[nsub capacity] | shard + [begin,count] range + gate/scan cursor
+---------------------------+
| KeyRef[nkeys]             | argv index + full hash + shard
+---------------------------+
| key_order[nkeys]          | dense per-shard key-index lists
+---------------------------+
| ValueSlot[nkeys]          | MGET/MSETNX only: nil / inline / borrowed descriptor
+---------------------------+
| status[nkeys]             | apply/count/error byte, when required
+---------------------------+
| ObjectImage[nkeys]        | two-hop gather descriptors, except MSETNX
+---------------------------+
| ObjectImage[nkeys]        | two-hop apply descriptors
+---------------------------+
| hop2[nkeys]               | complete second-hop plan
+---------------------------+
high address
```

Arrays that a command does not use consume no arena bytes. Variable logical object payloads for the
existing two-hop image protocol remain privately owned by their `ObjectImage`; only their
descriptors and plans moved into the arena. Rare unbounded data (`KEYS` result chunks and the
capture-safe FLUSH key walk) is attached through a cold auxiliary object rather than putting vector
constructors in every group.

Requests up to the common size class use an 8 KiB block. Each dispatching IO loop owns a 64-entry
freelist of those blocks. After warmup the usual <=16-key command takes a block from that freelist
and performs no group-state allocation. A larger calculated layout gets an exact heap block and
still uses the same fixed-offset representation. Oversize routing uses temporary heap scratch;
<=32 route keys stay on the IO stack.

The arena is attached to the ROB `Op` by reusing the footprint-locked zero-copy descriptor with a
tagged shard value. This does not change `sizeof(Op) == 336`. Tasks carry the same arena pointer but
never own it. The last executor publishes `OpState::Done`; the connection-owning IO thread observes
that release at in-order ROB retirement, assembles the reply, detaches the arena, destroys cold
payloads, and returns the block to its own pool. An executor never deletes or recycles IO-owned
arena memory.

## Routing and execution shapes

The arena contains one dense `ShardGroup` per touched shard. A group names one range of
`key_order[]`, so publishing remains one `Task` per shard regardless of the number of keys owned by
that shard. IO checks the complete producer-to-owner SPSC capacity requirement before publishing the
ROB entry or any task.

MGET, MSET, multi-key DEL/UNLINK/EXISTS/TOUCH, and KEYS are single phase. MSET and the count/apply
families write only request-indexed status bytes. They do not build value images or copy strings.
KEYS retains its bounded walk: an executor examines at most 256 physical slots per pass and resumes
the same oldest task through its local retry deque.

MSETNX, RENAME, RENAMENX, COPY, SMOVE, LMOVE, RPOPLPUSH, SINTER/SUNION/SDIFF/SINTERCARD, and the SET
STORE variants retain their existing image-based semantics and connection barrier. The last hop-1
owner computes the verdict/result and fills the complete `hop2[]` plan. It rebuilds the arena's
dense groups, performs a full per-producer queue-capacity preflight, and only then publishes any
hop-2 task. The barrier is cleared only at ROB quiescence, so a following command cannot overtake a
continuation.

The non-atomic windows are unchanged:

- MSETNX checks every key before its apply wave, but another client may write between waves and
  individual owner applies become visible independently.
- RENAME/COPY and SMOVE/LMOVE gather logical images before applying replacements/deletes. Different
  owners can become visible in either order, and an admission failure may occur after another owner
  has applied its half.
- Set operations compute from gathered source images. STORE variants subsequently replace the
  destination and do not retry source changes.

Per-owner FIFO order still supplies sequential single-connection read-your-own-writes for one-phase
scatter: later work for key K enters K's owner queue behind the earlier group. Two-hop commands use
the stronger parse barrier above.

## Same-owner localfast

Routing examines every explicit command key, including STORE destinations. If they all resolve to
one shard, IO does not allocate an arena and does not publish scatter tasks. It marks the `Op` as a
local multi-key command and sends one ordinary `Task` to that owner. The owner runs the complete
multi-key loop directly and emits the same command reply as the previous semantics. This covers
reads, writes, moves, and set operations; one-key DEL/UNLINK/EXISTS/TOUCH continue through their
existing ordinary handlers. HLL is the intentional exception: multi-key PFCOUNT and every PFMERGE
always use their image-gather SCATTER-V2 shapes, even when routing finds only one owner; only
single-key PFCOUNT remains an ordinary owner operation.

Local multi-key writes reuse the otherwise idle zero-copy length field as their per-key snapshot
gate cursor. A `Pending` pre-image resumes at exactly the same key without arena allocation and
without repeating already-ready keys. The marker and cursor are reset when the ROB slot is reused,
so `sizeof(Op)` is unchanged.

## Zero-copy MGET gather and IO-side assembly

Each MGET result slot is indexed by original request position and has three states:

```text
nil                       -> no value / wrong type
inline {len, bytes[24]}   -> integer decimal rendering or tiny raw string
borrow {ptr,len,shard}    -> FlatStore value bytes retained by the owner
```

An owner renders integer encoding into the inline buffer. Raw values up to 24 bytes are copied
inline. For larger values the owner calls `FlatStore::borrow(value_ptr)` and publishes only the
descriptor. The completion counter's acquire/release chain makes every disjoint slot visible to the
last owner and then to IO through `OpState::Done`.

At ROB retirement the IO owner emits the RESP array header in request order. Inline/nil runs become
ordinary buffered bytes. For a borrowed slot it emits a buffered bulk header, appends a BORROW
segment `{ptr,len,shard}`, and appends static CRLF. The existing `WbEngine`/`SegmentQueue` sendmsg
iovec path preserves ordering behind any older contiguous send and supports short writes without
copying the value bytes.

Borrow lifetime, including teardown, is:

```text
owner EX: find -> FlatStore::borrow(ptr) -> publish slot
     |
     v
owner IO: retire ROB -> header / BORROW / CRLF segments -> recycle arena
     |
     +-- successful/partial send: segment consumed exactly once
     |
     +-- disconnect/send error: WbEngine::teardown releases all unsent segments
     v
IO release queue -> shard's current owner EX -> FlatStore::unborrow(ptr)
                                              -> free retired object on last reference
```

The connection segment owns the copied borrow descriptor after retirement, so recycling the arena
does not shorten the value lifetime. If MGET terminates with an error before descriptors transfer to
segments, IO returns every gathered borrow immediately through the same per-shard release channel.
`FlatStore::borrow` increments the outstanding count only after a new registry entry is installed,
so allocation failure cannot create a phantom reference.

All non-local scatter reply serialization now happens in the IO retirement pass. Executors write
only slots, statuses, images, result descriptors, and completion state; they never issue sends.

## Snapshot and preserved seams

Scatter writes present every mutation key to `snapshot_prepare_write` before mutating any key in
that owner task. `ShardGroup::snapshot_pos` survives `Pending`. During capture, FLUSH still builds a
stable key-name walk and performs logical erases so the frozen table geometry remains valid; outside
capture it keeps the proportional `clear()` path. Bounded scans leave expired frozen entries in
place for the capture walker while reporting them absent to KEYS.

The `ScatterState::epoch` field remains a reserved `uint64_t` with the original comment and no
behavior. Validate/gather and apply remain separate so epoch assignment, read versions, validation,
and retry can attach without changing the scatter template.

Registry key positions, full pre-publish capacity checks, maxmemory insert-result admission,
capture-safe FLUSH, per-connection ROB order, owner-only stores, no executor sends, lowercase
`eq_icase` literals, `sizeof(Op) == 336`, and `sizeof(Client) == 1984` remain invariants.

## Fork mechanisms adopted, and Redis weight not carried

Adopted from the optimized ee451 layer:

- `csGroupNew`'s shape-derived one-region lifetime, translated to fixed-offset C++ arena arrays.
- `createPooledFakeClient`'s dispatch-owner recycling principle, translated to an IO-local arena
  size-class freelist.
- One sub per distinct owner and request-position maps, translated to dense `ShardGroup` ranges and
  `key_order[]`.
- Coalesced MGET retained references, translated to `FlatStore::borrow` descriptors and the existing
  per-shard release channel.
- `csReassemble` on the connection owner, translated to the ROB retirement callback and segmented
  `WbEngine` sends.
- `CS_LOCAL`, generalized here to every supported command whose complete key set has one owner.
- The last-sub completion barrier and complete-before-launch hop-2 plan/preflight.
- Bounded owner-side KEYS fan-out and coordinator concatenation.

Deliberately not carried:

- Redis real/fake `client` objects, subclient pools, copied argv arrays, reply-buffer splicing, CDB
  completion bytes, or fake-client lifecycle flags. Existing `Op`, `Task`, SPSC channels, and ROB do
  those jobs directly.
- `robj`/SDS value ownership, cross-thread refcount mutation, retained-object fences, and S8
  freeback records. FlatStore pointer borrows plus `{shard,ptr}` releases are the complete lifetime
  protocol.
- Redis dictionaries/kvstore, shared node databases, store locks, QSBR, node-wide borrowing, or
  migration locks. Per-shard `FlatStore` ownership remains literal.
- Atomic version-bag MSET machinery, commit records/timestamps, destination reservations, straddle
  memos, and read-restart waves. This phase carries only the explicit epoch seam for later MVCC.
- General Redis command/subcommand dispatch, Lua/module hooks, blocking-command readiness buses,
  commandstats plumbing, and the fork's many unrelated cross-shard pipeline types.
- Executor/WB send ownership. The connection's IO thread remains the only sender in the 2s
  architecture.

No benchmark belongs to this implementation pass; the owner runs performance cells.
