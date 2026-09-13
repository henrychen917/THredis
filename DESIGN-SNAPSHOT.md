# Snapshot design

This lane adds a fork-free point-in-time dump to the existing single-owner shard model.  It keeps
the executor rule intact: an executor performs serialization CPU work on objects it owns, but it
never opens, writes, syncs, renames, or reads a file.  One IO thread owns all save-side file I/O.
At boot the main thread reads and validates the file, after which the real executor threads decode
their owned shard sections in parallel before any listener is opened.

The direct reference is Dragonfly's `server/snapshot.cc` plus `server/serializer_base.{h,cc}` and
its `docs/shard-serialization.md` (a path in the Dragonfly tree, not a file here): version a
physical region, serialize that region from the mutation
callback before it changes, and let traversal skip regions already side-saved.  Dragonfly also has
to coordinate multiple fibers, DashTable displacement, tiered reads, journal ordering, overlapping
snapshots, and per-bucket dependency latches.  TomoKV has none of those ownership ambiguities.  One
owner freezes one stable FlatStore slot layout, so a cursor plus one temporary live-slot bit is the
complete equivalent.  That reduction—not a partial port of Dragonfly's fiber machinery—is the
intended paper result.

## Commands and configuration

- `BGSAVE` establishes the cut and returns `Background saving started`.  Executors then capture in
  bounded pieces while continuing to execute commands.
- `SAVE` uses the same cut machinery but stops command execution after the cut and captures shards
  in increasing shard-id order.  The command returns only after sync and rename complete.
- `--dir PATH` and `--dbfilename NAME` select the save target.  Defaults are `.` and `dump.tomo`.
- `--load PATH` loads an explicit file.  Load happens before the bind probe and before IO threads
  create listeners.
- `CONFIG GET dir`, `CONFIG GET dbfilename`, and `CONFIG GET *` return RESP2 key/value arrays.
- `INFO` includes `rdb_bgsave_in_progress` and `last_save_time` (Unix seconds, updated only after a
  successful durable rename).

Only one SAVE or BGSAVE may exist at once.  A second request gets `ERR Background save already in
progress`.

## The cut

The dump is the state at one logical epoch point, not one independently chosen time per shard.
Establishing epoch `E` has a preparation phase and a deliberately short stop-the-world mark phase:

1. The initiating IO thread opens a unique temporary file and sends a `SnapshotStart` ring message
   to every executor.
2. Each owner finishes any existing FlatStore rehash in its ordinary eight-slot steps, allocates a
   fresh post-cut slot table, and reports **prepared**.  It continues serving in this phase.  New
   resizes are suppressed so the prepared table cannot become stale; this does not scan keys.
3. After every owner is prepared, the coordinator publishes **Freeze**.  Each owner observes it at
   the head of a loop pass (an operation boundary), stops executing operations and reports frozen.
4. Once all owners are frozen, the coordinator defines cut `C = (E, realtime_ms)` and publishes
   **Mark**.  Every shard swaps its current table pointer into the frozen-table position and installs
   the already allocated empty table as its current/post-cut table.  The swap is O(shards), not
   O(keys).  Owners acknowledge the mark while still frozen.
5. After all marks, the coordinator publishes **Capture** immediately.  BGSAVE owners resume
   service; SAVE owners remain stopped until the file completes.  The initiating IO thread then
   writes the header before it can drain any queued frame, so disk latency is outside the barrier.

There are no store mutations between the last frozen acknowledgement, the definition of `C`, and
the pointer swaps.  Consequently every frozen table is exactly the whole-server state at `C`.
The only stop-the-world interval is Freeze through Mark; rehash completion and allocation happen
while service continues.

The timestamp in `C` gives TTLs one common interpretation.  A record whose absolute deadline is at
or before `C.realtime_ms` is absent from the dump.  A key expiring after `C` is included with its
absolute deadline, even if wall time passes that deadline before traversal reaches it.

### Cut proof sketch

For each shard `S`, let `F_S` be its table immediately before Mark and `P_S` the fresh table
installed at Mark.

1. All commands are single-shard and execute only on that shard's owner.  Freeze is observed between
   operation batches, so no operation is partially executed at the cut.
2. All owners acknowledge Freeze before `C` is defined.  No owner executes another command until
   after its Mark.  Therefore every `F_S` denotes `S` at the same `C`, rather than at a per-owner
   time.
3. Every key present at `C` occurs exactly once in `F_S`; `P_S` is empty.  Every key created after
   `C` is placed in `P_S` and is never traversed by this snapshot.
4. The serialize-before-mutate rule below emits the `C` value before a value in `F_S` can change.
   Traversal emits every remaining unmarked `F_S` slot.  Thus every key present at `C` is emitted
   once with its value at `C`.
5. Deletions, replacements, in-place string overwrites, TTL changes, and future collection
   mutations all pass the same Write-command gate.  There is no mutation path that can change an
   unvisited frozen value without first making its record independent of that value.

Items 1–5 establish that the union of all logical shard sections equals the keyspace at `C`.

## Mutations during BGSAVE

The implementation is Dragonfly's bucket-version/serialize-before-mutate idea reduced to the two
facts this store already guarantees: a shard has one owner, and a KvObj never moves.  FlatStore's
slot layout is frozen for capture; no mutex, atomic, refcount, or fork is needed.

FlatStore already has two physical table positions for incremental rehash.  During capture their
meanings are:

```text
t_[0]  post-cut table: new keys and replacements moved after C
t_[1]  frozen table:   the stable slot layout that existed at C
```

Traversal advances one monotonically increasing cursor through `t_[1]`.  Bit 48 of a live slot word
(the existing tombstone bit, which is otherwise unused when a pointer is present) is the temporary
`dumped-ahead` mark.  It adds no side allocation and does not reduce pointer or hash-tag bits.

Before executing a command marked `CmdFlags::Write`:

- a key in `t_[0]` is post-cut and needs no snapshot action;
- a frozen slot behind the cursor has already been emitted and may be changed;
- a frozen slot ahead of the cursor with `dumped-ahead` set already has its pre-image in the logical
  shard stream and may be changed;
- otherwise, the owner emits that slot's complete pre-image and then sets `dumped-ahead` before the
  command is allowed to run.

When traversal later reaches a marked slot it clears the mark and skips the slot.  A delete may have
turned the marked live word into the ordinary tombstone word; that also skips naturally.  A normal
traversal record advances the cursor only after its complete payload has been copied into owned
snapshot chunks.  Hence neither a duplicate record nor a post-cut value can enter the dump.

Large values do not break the loop budget.  Record serialization is resumable.  If a write needs a
large pre-image, that Task and later Tasks for the same shard stay in an executor-local FIFO while
up to 64 KiB is copied per pass.  Other owned shards continue to execute.  Once the complete
pre-image is in chunk-owned memory, the mutation runs.  This is the important distinction between
"serialize before mutate" and "write the whole large value in one command callback."

During capture, lazy expiry of a frozen key reports the key absent to the command but leaves its
physical object for traversal.  Active expiry is paused for capturing shards.  The dump uses the
cut timestamp as described above, so an after-cut expiry cannot erase the required pre-image.

The fresh table is allocated at twice the frozen table capacity.  Successful post-cut inserts are
capacity-gated against the complete logical live set, which guarantees the normal bounded rehash
can merge the frozen table after capture without losing a pointer.  Exhausting that reserve is
reported as the existing keyspace-insert failure; it never weakens snapshot consistency.

## Incremental work and handoff

An executor capture pass examines at most 256 frozen slots and copies at most 64 KiB for one shard.
A record cursor contains its fixed header position, key position, and the type lane's fixed-size
payload cursor.  A full chunk is handed off before more bytes are produced.  A full SPSC channel
leaves the chunk owned by the executor and naturally backpressures capture; it is never dropped.

The handoff is one `Channel<SnapshotChunk*, 64>` per executor producer, using the same SPSC queue,
notify mask, blocked flag, and ring wake protocol as request/reply channels.  Only the IO thread that
accepted the save command becomes the writer.  Its normal loop writes at most eight 64 KiB frames
per pass.  Executors perform no file operation.

SAVE uses the same channel path.  Because all command execution remains stopped, only the owner of
the current increasing shard id captures.  The writer advances and wakes the next owner after it
has appended the current shard's End frame.

## File format, version 1

All integers are little endian.  Readers reject unknown versions, wrong shard counts, invalid
sequence numbers, duplicate/missing Begin or End frames, checksum mismatches, bad lengths, unknown
types, duplicate keys, wrong-shard keys, and a missing completion footer.

### File header (80 bytes)

```text
magic[8] = "TOMOSNP\0"
u32 format_version = 1
u32 header_bytes = 80
u32 shard_count
u32 hash_kind
u64 snapshot_epoch
i64 cut_realtime_ms
u64 mix_hash_seed
u64 siphash_k0
u64 siphash_k1
u64 checksum_of_bytes_0_through_63
reserved[8]
```

Hash kind and keyed seed material are persistence metadata because routing consumes the keyed hash.
Load restores them before Server initialization; otherwise the same key could belong to a different
shard after restart.

### Physical frames and logical shard sections

Each physical frame has a 32-byte header followed by `payload_bytes`:

```text
u32 tag = "FRAM"
u32 shard_id
u32 per_shard_sequence
u32 flags                 (Begin=1, End=2)
u32 payload_bytes         (<= 65536)
u64 payload_checksum
reserved[4]
```

Frames may interleave across shards during BGSAVE.  `(shard_id, sequence)` forms one ordered logical
section per shard; Begin and End delimit it.  This lets one writer append immediately without
buffering whole out-of-order shards in executor memory.  SAVE naturally produces contiguous
sections because it captures shards sequentially.

### Key records

The concatenated payload of a logical shard section is a sequence of:

```text
u32 tag = "RECD"
u8  logical_type          (string/hash/list/set/zset)
u8  lane_encoding
u16 flags = 0
u32 key_bytes
u32 reserved = 0
u64 payload_bytes
i64 absolute_expire_at_ms (-1 means persistent)
key[key_bytes]
type_payload[payload_bytes]
```

The file ends with a checksummed 32-byte `DONE` footer containing shard count, epoch, and physical
frame count.  A killed writer cannot produce a valid footer.

## Per-type hook contract

The contract is in `src/snapshot/format.h`:

```cpp
SnapshotHookStatus begin_save(const KvObj&, SnapshotSaveCursor&, uint8_t& encoding);
SnapshotHookStatus read_save(SnapshotSaveCursor&, uint8_t* dst,
                             size_t capacity, size_t& written);
SnapshotHookStatus load(Slice key, uint8_t encoding, int64_t expire_at_ms,
                        Slice payload, KvObj*& result);
```

`begin_save` must announce the exact logical payload length without scanning merely to compute a
threshold.  `read_save` is resumable and must write no more than `capacity`; the fixed `lane[4]`
cursor words may hold compact/expanded iterator state.  `load` returns a newly owned KvObj or a
precise status.  Hooks must serialize logical content, not allocator pointers or C++ container
layout, so compact/expanded representation changes remain file compatible.

The string lane is complete: raw/external strings serialize their bytes, integers serialize one
little-endian i64 and preserve integer encoding, and absolute TTL is in the common record header.
Hash, list, set, and zset export explicit `Unsupported` stubs with TODOs in their type files.  Until
those lanes provide logical iteration and reconstruction, encountering one aborts SAVE/BGSAVE and
never publishes a partial file as the target.

## Boot load

The main thread opens and reads the file, validates every physical frame/footer, restores hash key
material, and assembles the logical byte stream for each shard.  Server and executor rings are then
created.  The actual executor threads, pinned and arena-bound as usual, decode their owned sections
in parallel and insert directly into their single-owner FlatStores.  Expired records are skipped.

Main waits for every executor loader.  On any error it stops and joins them.  Only after all loaders
succeed does main create the bind-probe listener and start IO threads.  Thus no client can observe a
partly loaded keyspace, and no executor performs disk I/O.

## Crash safety

The writer never truncates the configured target.  It creates
`<dir>/<dbfilename>.tmp.<pid>.<epoch>` with `O_EXCL`, appends header/frames/footer, calls
`fdatasync`, closes, atomically renames over the target, and fsyncs the directory.  Before rename,
a crash leaves the previous target intact and at most an invalid temporary file.  After rename, the
new target has a validated completion footer and synced contents.  Any failure before rename closes
and unlinks the temporary file, leaving the old target untouched.  A directory-fsync error after a
successful rename is reported (and does not advance `last_save_time`); the visible target is still
the complete, data-synced new file rather than a partial stream.

## Test cases

These are runtime cases for the owner; this lane's implementation gate is compile-only.

### Mutate-heavy BGSAVE

1. Start with enough keys to force FlatStore growth/rehash; include embedded strings, external
   strings larger than several 64 KiB chunks, integer strings, persistent keys, and TTL keys on
   every shard.
2. Record the expected model, issue BGSAVE, and use its reply as confirmation that the epoch mark
   completed.
3. Until `rdb_bgsave_in_progress:0`, continuously SET/INCR/DEL/EXPIRE/PERSIST keys chosen both ahead
   of and behind traversal, delete then recreate keys, mutate every original key, and add new keys.
   Pipeline across many connections and shard owners.  Include repeated writes to the same large
   value while its first pre-image needs multiple passes.
4. Preserve the model at the BGSAVE cut, not at completion.  Boot a separate server with the same
   `--shards` and `--load` file, then compare every key/value/absolute TTL to that cut model.  Assert
   no post-cut-only key exists, no deleted-at-cut key resurrects, and every cut key occurs once.
5. Repeat while a FlatStore rehash is active when BGSAVE begins.  Verify service continues during
   preparation and that only the short Freeze/Mark barrier stops all owners.

### Load verification

1. Save raw strings around embed/extern thresholds, integers at signed limits, empty and binary
   keys/values, maximum accepted key/value sizes, persistent keys, future TTLs, and keys that expire
   before load.
2. Start with `--load PATH`; verify no listener accepts before load completion, DBSIZE excludes
   expired records, GET/TYPE/OBJECT ENCODING and TTL-family replies match the pre-save state, and a
   subsequent BGSAVE/load cycle is identical.
3. Corrupt magic, version, header checksum, frame checksum, sequence, shard id, record length/type,
   footer, and hash metadata separately.  Every boot must fail before bind.
4. Try a file with a different `--shards`; boot must fail explicitly.

### Kill during BGSAVE

1. Keep a known-good target, start a mutate-heavy BGSAVE, and SIGKILL at random points: before the
   first frame, mid-large-record, after several shard End frames, during fdatasync, and near rename.
2. After each kill, restart from the configured target.  It must load either the complete old file
   or the complete new file, never a prefix or mixture.  Temporary files may remain but must not be
   selected by `--load` and must never replace the target without a valid footer.
3. Inject ENOSPC/EIO/short writes and directory-sync failure.  INFO must leave `last_save_time`
   unchanged, the in-progress flag must eventually clear, and a later BGSAVE must be possible.
