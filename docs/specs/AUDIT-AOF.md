# AUDIT-AOF — implementation audit governing an AOF/journal lane for TomoKV-cpp

Target tree: `/home/user/Projects/tomokv-cpp-perthread` (pure 2s, single-owner-per-shard).
References read in source: `/home/user/Projects/redis/src/{aof.c,rio.c,bio.c,server.c,server.h,config.c,expire.c,t_string.c,debug.c}`,
`/home/user/Projects/dragonfly/src/server/journal/*` plus `snapshot.{h,cc}`, `transaction.cc`, `replica.cc`,
`/home/user/Projects/garnet/libs/{server/AOF,storage/Tsavorite/cs/src/core}`.
Nothing was modified in any repository.

---

## 0. Two facts checked first, because they invalidate stale assumptions

**0.1 Redis no longer has a "fork + diff buffer" AOF rewrite.** The brief describes the pre-7.0 design.
In this tree the diff pipe is gone entirely — `grep -rn "aof_child_diff\|aof_pipe\|AOF_READ_DIFF" src/*.c src/*.h`
returns nothing. Multi-Part AOF replaced it: the parent opens a *new INCR file* before forking
(`aof.c:3216-3221`) and the child writes only a BASE snapshot of its forked memory
(`aof.c:3239-3253`). There is no parent→child diff channel and no accumulate-and-append step.
The audit below therefore compares against MP-AOF, not against the 6.x design.

**0.2 `DESIGN-SNAPSHOT.md` is stale about type coverage.** It says (lines 233-235) that
"Hash, list, set, and zset export explicit `Unsupported` stubs". That is no longer true:
`hash_snapshot_hooks()` at `src/cmd/t_hash.cc:1621`, `list_snapshot_hooks()` at `src/cmd/t_list.cc:1028`,
`set_snapshot_hooks()` at `src/cmd/t_set.cc:1274`, `zset_snapshot_hooks()` at `src/cmd/t_zset.cc:2412`
all return three real function pointers, and `tests/snap_typed_roundtrip.py` / `tests/snap_typed_race.py`
exist. **All five type lanes have a working resumable serializer and loader today.** This is the single
most consequential fact for the design in §2: an AOF base file and an AOF value record can both be
built out of machinery that already ships.

---

## (a) How each server actually does it

### A1. Redis — `src/aof.c` (4027 lines), multi-part AOF

**Append path.** One global `sds server.aof_buf` (`server.h:2344`). A command that mutates state calls
`propagateNow()` (`server.c:3766`) → `feedAppendOnlyFile(dbid, argv, argc)` (`aof.c:1661`), which
(i) optionally emits a `#TS:<unix>\r\n` annotation (`aof.c:1642-1651`), (ii) emits `SELECT <db>` when
`dictid != server.aof_selected_db` (`aof.c:1677-1684`), (iii) appends the command as plain RESP via
`catAppendOnlyGenericCommand()` (`aof.c:1609-1632`) — the AOF file *is* the RESP wire format — and
(iv) `sdscatlen`s it onto `aof_buf` (`aof.c:1696`). No syscall on the command path.

**Atomicity of multi-command effects.** `propagatePendingCommands()` (`server.c:3873`) wraps the
accumulated ops of one execution unit in `MULTI`/`EXEC` whenever `also_propagate.numops > 1`
(`server.c:3883`, `3897-3909`), with an explicit escape for `CMD_TOUCHES_ARBITRARY_KEYS`
(`server.c:3889-3893`). This is *the* mechanism that makes a multi-key effect replay all-or-nothing,
and it is deliberately paired with the loader's `valid_before_multi` (below).

**Determinism.** Redis never journals the received argv when it is nondeterministic; it journals the
*effect*. `EXPIRE`→`PEXPIREAT` rewrite at `expire.c:824-833`; `GETEX` propagated as
`PEXPIREAT`/`DEL`/`UNLINK`/`PERSIST`, never as itself (`t_string.c:520-535`); `SPOP`→`SREM`, active
expiry → `DEL`/`UNLINK`. `feedAppendOnlyFile` has no AOF-specific translation at all — the comment at
`aof.c:1686-1687` states the rule: "All commands should be propagated the same way in AOF as in
replication."

**Flush + fsync policies.** `flushAppendOnlyFile(force)` (`aof.c:1399`) is called from `beforeSleep`
(`server.c:2066`) **before** `handleClientsWithPendingWrites` — the comment at `server.c:2064-2066`
makes this ordering explicit. Consequence, and it matters for §2.3: *in every policy, including
`appendfsync no`, the `write(2)` happens before the reply is sent.* Policies (`server.h:666-668`):

| policy | behaviour | code |
|---|---|---|
| `always` | `redis_fsync(aof_fd)` inline; `exit(1)` on failure because the reply is already staged and the DB cannot be rolled back | `aof.c:1582-1598`, rationale at `aof.c:1531-1538` |
| `everysec` | once per second, hand the fd to a bio thread (`aof_background_fsync` → `bioCreateFsyncJob`, `aof.c:1233-1235`, `bio.c:252-258`); if an fsync is still in flight, postpone the *write* for up to 2s then force it and bump `aof_delayed_fsync` | `aof.c:1599-1605`, postpone logic `aof.c:1438-1458` |
| `no` | never fsync explicitly | enum only, `config.c:3381` |

The bio worker does the `redis_fsync` and publishes `fsynced_reploff_pending` for `WAITAOF`
(`bio.c:316-343`). Short writes are undone with `ftruncate(aof_fd, aof_last_incr_size)`
(`aof.c:1515-1526`). `no-appendfsync-on-rewrite` suppresses fsync while any child is active
(`aof.c:1578-1579`).

**Multi-part AOF: manifest + BASE + INCR.** Design comment at `aof.c:43-72`. Types
`AOF_FILE_TYPE_{BASE,HIST,INCR}` (`server.h:2005-2008`); `aofManifest` = one base + an INCR list + a
history list (`server.h:2019-2028`). Manifest lines are `file <name> seq <n> type <b|h|i>
[startoffset <o> [endoffset <o>]]` (`aof.c:119-141`). Persistence is
write-temp → `redis_fsync` → `rename` → `fsyncFileDir` (`aof.c:540-611`) — a crash-atomic pointer swap,
and the model to copy. Naming: `getNewBaseFileNameAndMarkPreAsHistory` (`aof.c:441`),
`getNewIncrAofName` (`aof.c:469`).

**Rewrite (AOFRW).** `rewriteAppendOnlyFileBackground()` (`aof.c:3197`) does, in order:
`aof_selected_db = -1` → `flushAppendOnlyFile(1)` → `openNewIncrAofForAppend()` (`aof.c:1054`, which
opens the new INCR *and synchronously persists the manifest*, rolling back on any failure) → `fork`.
The child writes a temp BASE (`rewriteAppendOnlyFile`, `aof.c:3117`) and exits. The parent's
`backgroundRewriteDoneHandler` (`aof.c:3847`) renames temp→BASE, marks old INCRs as HISTORY
(`markRewrittenIncrAofAsHistory`, `aof.c:506`), persists the manifest, then GCs history files
(`aofDelHistoryFiles`, `aof.c:3959`). **The boundary between BASE and INCR is purely positional —
"which file the command landed in" — never a value comparison.** Remember this for §2.6.

`aof-use-rdb-preamble` (default yes, `config.c:3313`) selects the BASE encoding: `rdbSaveRio(...)`
(`aof.c:3140-3145`) vs a command-stream rewrite `rewriteAppendOnlyFileRio` (`aof.c:3013`) that walks
the kvstore and emits `RPUSH`/`SADD`/`HSET`/`ZADD` batched at `AOF_REWRITE_ITEMS_PER_CMD = 64`
(`server.h:131`). During rewrite the child uses `rioSetAutoSync(&aof, REDIS_AUTOSYNC_BYTES /*4MB*/)`
(`aof.c:3133-3136`, `server.h:196`) so the page cache never accumulates more than 4MB
(`rio.c:98-145`, including `sync_file_range` + `reclaimFilePageCache`).

**Auto-rewrite trigger** (`server.c:1741-1753`): fires when AOF is on, no child is active,
`aof_rewrite_perc != 0`, `aof_current_size > aof_rewrite_min_size`, and
`growth = aof_current_size*100/aof_rewrite_base_size - 100 >= aof_rewrite_perc`.
`aofRewriteLimited()` (`aof.c:1186-1216`) adds exponential backoff (1,2,4…60 min) after
`AOF_REWRITE_LIMIT_THRESHOLD = 3` consecutive failures, so repeated failure cannot spray tiny INCRs.

**Load path — this is where torn-group handling lives.** `loadAppendOnlyFiles(am)` (`aof.c:2113`)
loads BASE then INCRs in manifest order. `loadSingleAppendOnlyFile` (`aof.c:1810`) sniffs the
`"REDIS"` magic to decide RDB-vs-RESP (`aof.c:1853-1888`), then replays commands through a fake
client. Two cursors are maintained:

```c
off_t valid_up_to = 0;        /* aof.c:1815  offset after the last well-formed command */
off_t valid_before_multi = 0; /* aof.c:1816  offset just before the last MULTI          */
...
if (cmd->proc == multiCommand) valid_before_multi = valid_up_to;   /* aof.c:1970 */
...
if (fakeClient->flags & CLIENT_MULTI) {                            /* aof.c:2022 */
    serverLog(LL_WARNING,"Revert incomplete MULTI/EXEC transaction in AOF file %s", filename);
    valid_up_to = valid_before_multi;                              /* aof.c:2025 */
    goto uxeof;
}
```

A crash mid-transaction therefore rewinds to *before* the `MULTI`, and `truncateAppendOnlyFile`
physically truncates the file there (`aof.c:2046`). **All-or-nothing group replay in Redis is exactly
one saved offset plus a rewind.** Truncation is only permitted on the *last* file
(`aof.c:2159-2162`, `2192-2196`) — a truncated non-final file is fatal. Test surface for this is
`tests/integration/aof.tcl:25` ("Unfinished MULTI"), `:41` ("Short read"), `:77` ("Bad format").

`DEBUG LOADAOF` (`debug.c:660-676`) flushes, empties the DB, reloads the manifest and replays — the
in-process round-trip harness.

### A2. Dragonfly — `src/server/journal/*`

**Verdict up front: Dragonfly has no AOF.** `grep -rn "appendonly\|appendfsync\|auto-aof" src/server`
matches only a comment in `rdb_test.cc`. `grep -rn "fsync\|fdatasync" src/server` matches exactly one
non-test line, `server_family.cc:948` (pidfile). The journal is a **replication log**, not a
durability log. Everything below should be read as "how to structure a per-shard change stream",
not "how to make it survive power loss".

**Per-shard, thread-local, in-memory ring.** `thread_local JournalSlice journal_slice;`
(`journal.cc:20`) — one per shard thread, no shared structure, no lock on the append path.
`JournalSlice::AddLogRecord` (`journal_slice.cc:79-106`):

```cpp
FiberAtomicGuard fg;
item.journal_item.lsn = lsn_++;         // plain non-atomic increment: single owner
io::StringSink sink; JournalWriter writer{&sink}; writer.Write(entry);
std::move(sink).str().swap(item.journal_item.data);
```

then `CallOnChange` (`journal_slice.cc:108-142`) pushes into a
`boost::circular_buffer<JournalItem> ring_buffer_` sized by `--shard_repl_backlog_len` (default 8192,
`journal_slice.cc:20`) and fans out to registered `JournalConsumerInterface`s. **The LSN is per-shard
and monotone within the shard only.** There is no global sequence.

**Write hook.** `Transaction::LogAutoJournalOnShard` (`transaction.cc:1613-1649`) is the single
automatic tap. It gates on `cid_->IsJournaled()`, skips failed commands
(`result.status != OpStatus::OK`, `transaction.cc:1630-1632`), skips squasher hops, and — key detail —
**journals only this shard's slice of the argv** when the command spans shards:

```cpp
if (unique_shard_cnt_ == 1 || args_slices_.empty())
    entry_payload = journal::Entry::Payload(cmd, full_args_);
else
    entry_payload = journal::Entry::Payload(cmd, GetShardArgs(shard->shard_id()));
```

Commands that compute non-deterministic effects opt out with `CO::NO_AUTOJOURNAL` and hand-write
their effect instead — e.g. `RecordJournal(op_args, "DEL"sv, {key})` in `hset_family.cc:210`,
`"HDEL"` in `hset_family.cc:738`, `"XTRIM"` in `stream_family.cc:2624`. Same doctrine as Redis:
journal the effect, not the request.

**Serialization** (`serializer.cc:57-85`): `[opcode u8][txid packed][1u deprecated][argc][total-size][cmd][args…]`
with packed varints; `SELECT` is auto-emitted on db change by the *writer*, which keeps `cur_dbid_`
(`serializer.cc:58-63`). Opcodes `SELECT=6, COMMAND=10, PING=13, LSN=15` (`types.h:17`).

**Grouping.** `journal::DisableFlushGuard` (`journal.h:48-72`) + `SetFlushMode(false/true)`
(`journal_slice.cc:65-77`) let a caller emit several records with no preemption and no consumer
throttle between them — Dragonfly's equivalent of a MULTI bracket, but it only guarantees *no
interleaving on one shard*, not cross-shard atomicity.

**Replay ordering across shards — the important negative result.** `DflyShardReplica::ExecuteTx`
(`replica.cc:1241-1300`):

```cpp
if (!tx_data.IsGlobalCmd()) {
    // Execute cmd without sync between shards.
    return executor_->Execute(tx_data.dbid, tx_data.command) == DispatchResult::OK;
}
// global command only: InsertTxToSharedMap -> block->Wait() -> barrier.Wait() -> execute once -> barrier.Wait()
```

`IsGlobalCmd()` is true only for `FLUSHDB`/`FLUSHALL`/`DFLYCLUSTER FLUSHSLOTS`
(`tx_executor.cc:78-94`). **Every ordinary multi-shard transaction is applied by each shard's flow
independently, with no cross-shard barrier**, so a Dragonfly replica can transiently observe a
partial multi-shard transaction. The `shard_cnt` field that used to drive a general barrier is now
written as a hard-coded `1u` marked "deprecated field, kept for backward compatibility"
(`serializer.cc:78`). Dragonfly chose per-shard throughput over cross-shard replay atomicity.
That is precisely the trade TomoKV must *not* silently inherit, because TomoKV's atomic groups are a
user-visible promise (§2.4).

**Snapshot + journal composition — the part worth stealing.** `SliceSnapshot` is itself a
`journal::JournalConsumerInterface` (`snapshot.h:33`). `Start(stream_journal=true)` registers it
(`snapshot.cc:86-87`) and stamps `snapshot_version_`; `IterateBucketsFb` walks buckets and serializes
any bucket with version < `snapshot_version_`, bumping the bucket's version so it is emitted exactly
once (`snapshot.cc:152-160`). Mutations trigger `OnChange` (pre-image side-save) *before* the write;
journal records then flow through `ConsumeJournalChange` (`snapshot.cc:351-356`) into the same output
stream. At the end, `FinalizeJournalStream` unregisters and sends the final LSN
(`snapshot.cc:128-148`, `SendJournalOffset(journal::GetLsn())`). **One stream = full state then
incremental changes, with the handoff LSN written into the stream itself.** No fork.

Replica-side LSN accounting: `TransactionReader::NextTxData` increments a local LSN for every
non-`LSN` record and cross-checks against periodic `Op::LSN` markers (`tx_executor.cc:106-122`) — a
cheap continuity check that catches a dropped record.

### A3. Garnet / Tsavorite — checkpoint (FASTER CPR) + AOF

**CPR checkpoint state machine.** `libs/storage/Tsavorite/cs/src/core/Index/Checkpointing/StateTransitions.cs:27-52`
defines `Phase { REST, PREPARE, IN_PROGRESS, WAIT_INDEX_CHECKPOINT, WAIT_FLUSH, PERSISTENCE_CALLBACK, … }`;
`struct SystemState` (`:57-111`) packs phase (top 8 bits) and version (low 56) into **one 8-byte word**
so a thread reads phase+version atomically. `VersionChangeSM.cs:20-39` is the base cycle
`REST → PREPARE → IN_PROGRESS → REST`, with the version bump on the PREPARE→IN_PROGRESS edge (`:30`).
`HybridLogCheckpointSM.cs:19-37` extends it with `WAIT_FLUSH → PERSISTENCE_CALLBACK`.

The safety property is epoch-based, not lock-based: `StateMachineDriver.GlobalStateMachineStep`
(`StateMachineDriver.cs:215-253`) publishes the new phase word, then

```csharp
epoch.Resume();
epoch.BumpCurrentEpoch(() => MakeTransitionWorker(nextState));   // :246-247
```

so the transition action runs only once every thread that could still observe the old phase has left
its epoch (`LightEpoch.cs:383-425`, drain list at `:105/:139`).

Per-operation enforcement is `CheckCPRConsistencyRMW` (`Implementation/InternalRMW.cs:330-364`):
a thread in version V that sees a V+1 record during PREPARE returns `CPR_SHIFT_DETECTED` and retries;
a thread in V+1 that sees a V record during IN_PROGRESS is forced to RCU (create a new record) rather
than update in place. That is what makes the checkpoint a *consistent prefix* of each session's
operations without stopping the world.

Checkpoint boundaries are log addresses, captured in `HybridLogCheckpointSMTask.GlobalBeforeEnteringState`
(`:31-75`): PREPARE records `info.startLogicalAddress = hlogBase.GetTailAddress()`; WAIT_FLUSH records
`finalLogicalAddress`, `headAddress`, `nextVersion`. FoldOver simply shifts read-only to tail
(`FoldOverSMTask.cs:35-47`); Snapshot flushes the delta to separate devices
(`SnapshotCheckpointSMTask.cs:35-84`).

**Garnet's AOF.** `libs/server/AOF/AofEntryType.cs:6-104` is a byte-tagged, range-grouped opcode set —
main store `0x00-0x02` (`StoreUpsert/StoreRMW/StoreDelete`), object store `0x10-0x12`, transactions
`0x20-0x22` (`TxnStart/TxnCommit/TxnAbort`), checkpoint markers `0x30/0x32`, `FlushAll/FlushDb`
`0x60/0x61`. The record header (`AofHeader.cs:125-217`) is an explicit-layout struct carrying
`opType`, `databaseId`, **`storeVersion` (long)** and `sessionID`; the sharded variant
`AofShardedHeader` (`:54-67`) adds a **`sequenceNumber`**.

Enqueue sites are inside the store's own `IFunctions` callbacks —
`Storage/Functions/MainStore/PrivateMethods.cs:782-846` (`WriteLogUpsert` / `WriteLogRMW` /
`WriteLogDelete`), each guarded by `if (functionsState.appendOnlyFile != null)`. Note
`WriteLogRMW` sets `RespInputFlags.Deterministic` on the input before logging (`:816`) — the same
"journal the deterministic form" doctrine.

**Sharded logs — directly relevant to per-shard journals.** `AOF/GarnetLog.cs:622-675`:
when `AofPhysicalSublogCount > 1`, `GetPhysicalSublogIdx(key)` (`:663`, hash-routed, helpers at
`:92-107`) picks the sublog and writes an `AofShardedHeader` stamped with a **global
`sequenceNumber`** (`:660`). Recovery then merges sublogs by that sequence number
(`AOF/Recover/AofRecover.cs:126-163 MultiLogRecover`, which first calls
`RecoverLatestSequenceNumber(out recoverUntilSequenceNumber)` at `:129`). **Garnet pays for a global
sequence counter precisely to buy cross-shard replay ordering.** Single-log mode
(`SingleLogRecover`, `:89-114`) needs no such counter.

**Commit.** `TsavoriteLog.CommitInternal` (`TsavoriteLog.cs:3159-3257`) assigns
`info.CommitNum = ++commitNum`, sets `info.UntilAddress = TailAddress` (`:3223`) or enqueues a real
commit record in fast-commit mode (`:3214`), queues `(commitTail, info)` on `ongoingCommitRequests`
(`:3231`), then `allocator.ShiftReadOnlyToTail(...)` (`:3248`) is what actually triggers the flush.
Completion runs `SerialCommitCallbackWorker` (`:2683-2758`), which drains every commit whose
`UntilAddress` is covered and writes commit metadata (`WriteCommitMetadata`, `:2655-2665`).

**Durability primitive — no fsync anywhere.** Garnet gets durability from the file *handle*:
`Device/LinuxFileExtensions.cs:38-39,75-88` opens with `O_DIRECT | O_CLOEXEC` and optionally
`O_DSYNC`, with a comment explaining .NET's `FileOptions.WriteThrough` gets neither on Linux;
`DeviceLogCommitCheckpointManager.WriteInto` (`:419-438`) issues the async write and then blocks on
`semaphore.Wait()` (`:437`). Windows uses `FILE_FLAG_NO_BUFFERING` (`LocalStorageDevice.cs:440`).

**Recovery = checkpoint, then AOF replay, bounded by log addresses.** `StoreWrapper.cs:388-390`:

```csharp
await RecoverCheckpointAsync();
await RecoverAOFAsync();
ReplayAOF(AofAddress.Create(length: serverOptions.AofPhysicalSublogCount, value: -1));
```

`-1` means "to each sublog's current tail", resolved at `AofRecover.cs:61`. `SingleLogRecover`
(`:89-114`) scans `[Log.BeginAddress[i], untilAddress[i])` — the checkpoint's own `BeginAddress`
truncation is what prevents re-applying pre-checkpoint records; there is no per-record version filter
on the hot path. Replay is constructed with **`recordToAof: false`**
(`Databases/SingleDatabaseManager.cs:254`) so replayed ops are not re-logged. Checkpoint boundaries
are also written *into* the AOF as `CheckpointStartCommit` / `CheckpointEndCommit` markers
(`DatabaseManagerBase.cs:288-292`, handled at `AofProcessor.cs:259/280`).

**Knobs** (`libs/host/Configuration/Options.cs`, `libs/server/Servers/GarnetServerOptions.cs`):
`EnableAOF` (`:197` / default false `:59`), `AofMemorySize` `--aof-memory` (`:200`, default 128m),
`AofPageSize` (`:205`, default 32m), `AofReplayTaskCount` (`:217`), **`CommitFrequencyMs`
`--aof-commit-freq`** (`:236-237`, default 0 = commit per operation, `-1` = manual via `COMMITAOF`),
`WaitForCommit` (`:241`), `AofSizeLimit` (`:245`) with enforce frequency (`:248`), `FastAofTruncate`
(`:438`). Cross-option validation at `libs/host/GarnetServer.cs:479-490`; size invariants
(`AofMemorySize >= 2*AofPageSize`, `AofPageSize >= 2*mainlog PageSize`) enforced with explicit
messages at `GarnetServerOptions.cs:978-1018`.

### A4. Side-by-side

| | Redis 8.x (MP-AOF) | Dragonfly | Garnet |
|---|---|---|---|
| log granularity | one global `aof_buf` → one INCR file | one ring per shard thread, memory only | 1..N physical sublogs, key-hashed |
| durable? | yes | **no** (replication only) | yes, via `O_DSYNC`/`O_DIRECT` + blocking wait |
| cross-shard order | trivial (one log) | **not preserved** except FLUSH* | global `sequenceNumber` in sharded headers |
| group atomicity on replay | `MULTI`/`EXEC` + `valid_before_multi` rewind | none | `TxnStart/TxnCommit/TxnAbort` opcodes |
| base image | fork + RDB (or command stream) | fork-free versioned-bucket snapshot | CPR checkpoint (FoldOver / Snapshot) |
| base↔incr boundary | positional (which file) | LSN sent in-stream | log address + in-AOF checkpoint markers |
| fsync knob | `always/everysec/no` + bio thread | — | `CommitFrequencyMs` (0 / N ms / -1 manual) |
| record content | RESP command (effect-rewritten) | serialized command (effect-rewritten) | typed op + key + value/input |

---

## (b) Design for TomoKV

### B0. The five repo laws that decide this design

These are not preferences; each is enforced by existing code or by an owner rule, and an AOF that
breaks one will be rejected.

1. **Executors never touch files.** `DESIGN-SNAPSHOT.md:3-6`: "an executor performs serialization CPU
   work on objects it owns, but it never opens, writes, syncs, renames, or reads a file. One IO
   thread owns all save-side file I/O." Enforced structurally: executors post
   `SnapshotChunk*` over `Channel<SnapshotChunk*,64>` (`src/snapshot/snapshot.h:81`) and the
   designated IO thread is the sole writer (`SnapshotManager::writer_pass`, `snapshot.cc:404`).
2. **Footprint locks.** `static_assert(sizeof(Op) == 336)` (`src/exec/op.h:240`) and
   `static_assert(sizeof(Client) == 1984)` (`src/net/conn.h:538`). Any per-op or per-connection AOF
   state must fit in existing padding or be paid for by removing something.
3. **A shard has exactly one owner at a time, and ownership can move.** `src/core/shard.h:1-6, 14-16`;
   the mutable map is `Server::shard_owner_[256]` (`src/core/server.h:547`), one acquire load per
   route. Reshard is O(1) and copies no keys.
4. **Knob philosophy.** `src/core/config.h:7-9`: numeric where possible; `0` = off *and off allocates
   nothing*; `-1` = auto; thresholds self-derive; a field nothing reads is a lie.
5. **`always-on machinery ≤ 3% or it doesn't ship`**, and every optimization lands with a PRE/POST
   table. AOF is opt-in, so the binding form is: **`appendonly no` must cost one predicted branch and
   zero allocation** — the same bar `--atomic 0` already meets (`FlatStore::find` branches on
   `atomic_pending_->live != 0`, `src/store/flatstore.h:497-499`).

### B1. Pressure-testing "a global log is a new bottleneck"

The hypothesis is right, but the usual reason given for it is wrong, and the wrong reason leads to
the wrong design.

*Wrong reason:* "serializing every write through one buffer costs too much CPU."
`catAppendOnlyGenericCommand` (`aof.c:1609`) is a handful of `memcpy`s; at ~60 bytes/record and
28.4M ops/s measured capacity (memory: `tomokv-cpp-scaling-truths`, 64c) that is ~1.7 GB/s of
`memcpy` — real, but not the wall.

*Right reason:* a single append buffer is a **single mutable cache line touched by every executor**.
Every write becomes an RMW on one `tail` word plus a shared destination region. This repo already
paid to avoid exactly that shape: `ExQueue` is deliberately N×M SPSC rings rather than one MPSC ring
"to avoid the MPSC atomic RMW per push" (`src/exec/exqueue.h:1-15`), and the measured comm-tax is
~400ns/handoff dominated by instruction volume (memory: `thredis-commtax-truth`). A global AOF buffer
reintroduces the MPSC contention the whole 2s design removed.

*But the converse is also constrained:* per-shard **files** (up to 256, `validate_config`,
`config.h:293`) would mean 256 fds, 256 fsyncs per `everysec` tick, 256 manifest entries, and — worse
— a **ragged recovery point** (each file truncated at its own offset, so the recovered state is a
product of per-shard prefixes, which need not be any prefix of the real history).

**Resolution: logical per-shard streams, one physical file, one IO writer.**
This is not a compromise; each half buys a distinct property:

- *logically per-shard* ⇒ the append path is a per-shard, owner-private buffer. No shared line, no
  atomic, no lock. Same shape as `SnapshotChunk` production.
- *physically one file, one writer* ⇒ the byte stream is a total order. A crash truncates one file at
  one offset, so the recovered state is a prefix of the writer's append order — a genuine global
  serialization. It also keeps the executors-never-touch-files law, one `fdatasync` per policy tick,
  one manifest, one rename.

The frame format that makes this work already exists and is already validated: `SnapshotChunk`'s
`{sid, sequence, flags(Begin|End)}` header (`src/snapshot/format.h:24-34`), with the comment at
`format.h:2-5` stating exactly the property we need — "Frames from different shards may be
interleaved, but `(sid, sequence)` identifies one ordered logical shard section."

Handoff cost check: chunks are 64 KiB (`format.h:22`), records ~60-200 bytes, so ~300-1000 records
amortize one 400ns handoff → sub-nanosecond per record. The writer does one `write(2)` per chunk.
This is not a throughput risk; the risk is entirely on the latency/ordering side (§B3, §f).

### B2. Concrete shape

```
executor (owner of shard S)                 designated IO writer thread
──────────────────────────                  ────────────────────────────
append record into per-shard                drain Channel<JournalChunk*,64> per producer
JournalChunk (owner-private,                append frames to <appenddirname>/<file>.<n>.incr.tomo
64 KiB, reuses the snapshot                 hold group commit records until deps satisfied (§B4)
chunk pool)                                 fsync per policy (§B5), io_uring IORING_OP_FSYNC
   │ full / batch end / gate                publish durable_seq_ (release store)
   └── post_chunk(...) ──────────────────▶  publish written_seq_  (release store)
```

- **Producer state** lives beside the existing snapshot state on `FlatStore` (`snapshot_pos_`,
  `snapshot_preimages_` at `flatstore.h:1441-1442`) — one `JournalChunk*` + one `uint32_t sequence`
  per shard. Zero when `appendonly no`.
- **Transport** reuses `SnapshotManager::post_chunk` / `writer_pass` verbatim in shape
  (`snapshot.h:74-75, 52`; `snapshot.cc:296-307, 404`), including the "full SPSC channel leaves the
  chunk owned by the executor and naturally backpressures" property (`DESIGN-SNAPSHOT.md:136`).
- **Writer role.** The snapshot writer is "the IO thread that accepted the save command"
  (`DESIGN-SNAPSHOT.md:139-141`). The journal writer must instead be a *fixed* IO thread chosen at
  boot (it runs for the process lifetime), bound the same way as `snapshot_bind_io`
  (`snapshot.h:130`). Because it is an `ifid` thread that also serves connections, **the fsync must
  not block it** — see B5.
- **Shard migration.** On an ownership flip (`Server::set_worker_of_shard`, `server.h:174`) the
  outgoing owner must close and post its partial chunk before the new owner appends, or the shard's
  `(sid, sequence)` stream forks. This is a hard barrier requirement and belongs in the migration
  handshake, not in the journal.

### B3. Replay ordering across shards — state the guarantee, then earn it

Define it precisely, because the interesting failures are all definitional.

> **Guarantee (target).** After recovery, the keyspace equals the result of applying some prefix
> *P* of the writer's append order, and every write acknowledged to a client before the crash is
> in *P*.

The first half is free from the one-file/one-writer choice. The second half is *not* free and is the
central cost decision. It holds only if a write's record reaches the file before its reply reaches
the socket. Redis gets this for free because `flushAppendOnlyFile` runs before
`handleClientsWithPendingWrites` on the same thread (`server.c:2064-2066`). TomoKV must buy it with a
cross-thread gate on the retire path (`IoLoop::collect_retire_work` / `flush_ready`,
`io_loop.h:798-840`).

**Proposed gate, sized to the footprint locks.** Do *not* add a per-`Op` or per-`Client` sequence
(both structs are locked, B0.2). Instead keep, per IO thread, one `uint64_t max_journal_seq_issued_`
in `ThreadCtx` (which already carries per-thread scratch), updated when the IO thread dispatches a
write, and compare against the writer's published watermark before staging bytes:

```
send allowed  ⟺  writer.written_seq_  >= self_->max_journal_seq_issued_    (everysec / rewrite)
send allowed  ⟺  writer.durable_seq_  >= self_->max_journal_seq_issued_    (always)
```

This is one acquire load per send batch, coarser than per-op (a read queued behind a write waits
too), but replies are already retired in ROB order per connection, and the cross-connection coupling
only bites on the same IO thread.

**Policy-tiered, because the p1 cost is real.** An extra EX→writer→IO hop is ~400ns against a p1 RTT
of ~11.8µs — about +3.4% on unpipelined single-connection latency, fully amortized at p32/p128.
Recommendation:

| policy | gate | recovered-state property |
|---|---|---|
| `always` | on `durable_seq_` (fsync) | acknowledged ⇒ on stable storage |
| `everysec` | on `written_seq_` (write(2) returned) | acknowledged ⇒ in page cache in log order; ≤1s lost to power failure; **survives process crash** — matches Redis |
| `no` | **no gate** | prefix of append order, but an acknowledged write may be missing |

The `no` row is a deliberate divergence from Redis (Redis still gates at `no`). It is defensible
because `no` promises nothing, and it keeps the zero-cost path zero-cost. It must be documented, not
silently shipped.

**What is *not* needed:** cross-shard ordering *between* independent single-key writes. Two writes to
the same key are on the same shard by construction — a key's bucket never changes, only which shard
owns the bucket (`shard.h:14-16, 36-38`) — so per-shard stream order is sufficient for per-key
correctness. This is the load-bearing simplification and it is why per-shard logical streams work at
all.

### B4. Atomic groups → one bracketed logical record

**The publish point is unambiguous and singular.** Verified in code:

```cpp
// src/cmd/scatter_engine.inc:1588-1591   (cross-shard atomic write groups)
if (!state.external_epoch) {
    const uint64_t ticket = server.atomic_commit();
    state.epoch.store(ticket, std::memory_order_release);   // ← every key on every shard flips here
}
// src/cmd/multi.inc:1155-1159            (EXEC; force-admitted even at --atomic 0)
if (!state.aborted.load(acquire) && state.final_reply == MultiFinalReply::Exec)
    state.epoch.store(server.atomic_commit(), std::memory_order_release);
// src/cmd/xshard_commands.inc:1071-1072  (broadened mover/store family)
// src/core/server.h:313-315
uint64_t atomic_commit() { return commit_seq_.fetch_add(1, std::memory_order_seq_cst) + 1; }
```

Before that store, every installed candidate sits at epoch 0 and is invisible to foreign readers
(`FlatStore::atomic_resolve_internal`, `flatstore_atomic.inc:558-590`, skip at `:580`). An abandoned
group simply never gets a ticket — the "rollback-free abort" (`NOTES-ATOMICS.md:39-40`).

**Design: bracketing, not coalescing.**

- Each participating owner writes its fragment into **its own shard stream**, tagged with the group
  ticket `G` and its own `(sid, seq)`.
- The finishing owner — the one that executes the publish above — enqueues **one `GCMT` commit record**
  carrying `G` and the participant vector `[(sid, seq), …]`.
- **Replay is two passes.** Pass 1 scans the file for `GCMT` records and builds the committed-ticket
  set. Pass 2 replays per-shard streams, applying any fragment whose ticket is 0 (plain write) or is
  in the committed set, and **discarding** every fragment whose ticket is absent.

This is Redis's `valid_before_multi` rewind (`aof.c:1970, 2022-2027`) generalized from one stream to
N, and it inherits its best property: a crash anywhere between the first fragment and the commit
record drops the whole group, with no undo log and no rollback.

**Why not coalesce the whole group into one record at the publish point?** Because the finishing
owner would have to serialize values owned by *other* shards, which violates B0.1's sibling rule
("an executor serializes objects it owns"), forces one shard's chunk stream to carry another shard's
bytes, and breaks the migration story in B2.

**The one hard ordering constraint the writer must enforce.**

> `GCMT(G)` must never be appended before every fragment `G` names.

Otherwise a truncation between them yields "committed but incomplete", which is unrecoverable.
Do *not* implement this by force-flushing a chunk per group (that destroys batching and turns every
`MSET` into a small write). Implement it in the writer:

- The writer tracks `appended_seq[sid]` — the highest sequence it has appended per shard.
- `GCMT(G)` records land in a small `pending_commits` table keyed by ticket, holding the participant
  vector. After each drain pass, every entry whose participants are all `<= appended_seq[sid]` is
  appended and erased.
- The table is bounded by the in-flight atomic window: `--atomic-window`, AUTO = `min(16*shards,1024)`
  (`config.h:80-87`, resolved at `server.h:63-65`). So the table is at most ~1024 entries.
- **Abandoned groups must reap.** A group that aborts never publishes a ticket, so it never produces
  a `GCMT`; its fragments are already in the file and are correctly skipped at replay. The leak risk
  runs the other way: a `GCMT` whose fragment never arrives (impossible if the participant
  posted before signalling, which the publish's release/acquire already orders) — assert it, with a
  bounded age-out that fails the AOF rather than silently committing.

**The invariant that keeps this honest:** *the journal must promise exactly the atomicity the live
path delivered — no more, no less.* At `--atomic 0`, cross-shard `MSET` is genuinely non-atomic
(`NOTES-XSHARD.md`, memory: `tomokv-xshard-phase` "cross-shard SHIPPED (non-atomic)"), so its
fragments are independent records with ticket 0 and no `GCMT`. `EXEC` force-admits through the atomic
window even at `--atomic 0` (`NOTES-MULTI.md`), so `EXEC` always produces a group. That mapping falls
out of tapping the publish point rather than the command name, which is the reason to tap there.

**A pleasant consequence:** replay does not need the MVCC lane at all. Boot-time replay is
single-owner-per-shard with no concurrent readers, so *visibility* atomicity is moot; only
*all-or-nothing application* matters, and bracketing gives that. The loader can use the plain store
path (`snapshot_load_owned`'s insert path, `snapshot.cc:548`). This removes an entire risk class.

### B5. fsync policy semantics mapped onto one file and one writer

There is only one file, so `everysec` is **not** a per-shard timer and **not** a fan-out sweep —
it is one timer on the writer thread. That is strictly simpler than Redis, and it deletes three
pieces of Redis machinery outright:

- **No bio thread.** Redis needs `BIO_AOF_FSYNC` (`bio.c:252-258, 316-343`) because its one thread
  cannot block. TomoKV's writer is already a separate thread from every executor — it *is* the bio
  thread. But it is also an `ifid` thread serving connections, so a blocking `fdatasync` would stall
  its clients.
- **No postponement state machine.** `aof_flush_postponed_start`, the 2-second grace, and
  `aof_delayed_fsync` (`aof.c:1438-1458`) exist only to cope with "write(2) blocks behind a
  background fsync on the same fd". Which brings us to:
- **Use `IORING_OP_FSYNC`.** The repo already links `liburing` (`Makefile:20`, `src/net/uring.h`) and
  the writer already runs a ring. An async fsync SQE with `IORING_FSYNC_DATASYNC` makes fsync just
  another completion: no blocking, no bio thread, no postponement heuristic. `durable_seq_` advances
  in the CQE handler. **This is the single largest structural simplification over Redis available
  here and it should be called out as a result, not buried as an implementation detail.**

| knob value | writer behaviour | notes |
|---|---|---|
| `always` | after every pass that appended bytes, submit an fsync SQE; `durable_seq_` advances on CQE; reply gate is on `durable_seq_` | batches naturally: one fsync per writer pass covers every record in that pass, exactly as Redis's one-fsync-per-event-loop does |
| `everysec` | submit an fsync SQE when `now_ms - last_fsync_ms >= 1000` **and** bytes have been appended since; also submit when idle-with-unsynced-bytes (Redis's `aof.c:1414-1423` case) | never blocks; no in-flight check needed because CQEs serialize |
| `no` | never submit | |

Failure handling copies Redis's asymmetry deliberately: an fsync error under `always` is
unrecoverable, because the reply has been (or is about to be) sent and the store cannot roll back —
Redis `exit(1)`s with that exact reasoning (`aof.c:1531-1538, 1588-1593`). Under `everysec`/`no`, set
a sticky error, refuse further writes with an OOM-style error (Redis's `DISK_ERROR_TYPE_AOF`,
`server.h:3635`), and clear it on a later success. Short writes get `ftruncate` back to the last good
offset (`aof.c:1515-1526`) — with one file and a known `appended_bytes`, this is exact.

### B6. Rewrite = snapshot + segment truncation. The fork is unnecessary — and so is the diff buffer, which Redis already deleted.

**The honest statement.** Redis already replaced diff-buffering with the BASE+INCR manifest (§0.1).
What TomoKV can additionally delete is the **fork**, because `DESIGN-SNAPSHOT.md`'s Freeze/Mark cut
is already a fork-free, whole-server, point-in-time consistent image, and §0.2 confirms every type
lane can serialize. Concretely:

```
AOFRW(t):
 1. writer closes segment N   (no fsync yet)
 2. SnapshotManager::start(..., blocking=false)     snapshot.cc:129
      Preparing → Freeze → Mark                     snapshot.cc:190-204, ex_loop.h:255-289
 3. AT THE MARK: writer opens segment N+1.
      Every write after Mark lands in the post-cut table t_[0] and in segment N+1.
      Every write before Mark is in the frozen table t_[1] and in segments ≤ N.
 4. Capture proceeds concurrently (serve-while-copy) → dump.tomo.tmp.<pid>.<epoch>
      fdatasync → close → rename → fsync(dir)       snapshot.cc:361-378
 5. manifest: BASE := <new snapshot>, segments ≤ N := HISTORY; persist manifest
      (write temp → fsync → rename → fsync dir, exactly Redis aof.c:540-611)
 6. unlink HISTORY segments
```

**Why the boundary is exact and needs no epoch comparison.** Step 3 is safe because the coordinator
(`SnapshotManager::start`) and the journal writer are both IO threads and can be made the *same*
thread, and because **no store mutation happens between the last frozen ack, the definition of the
cut, and the pointer swaps** (`DESIGN-SNAPSHOT.md:54-57`; the swap is
`FlatStore::snapshot_mark`, O(shards) not O(keys)). So the segment switch at the Mark is a clean
positional boundary — the same *kind* of boundary Redis uses, but established by a barrier we already
own rather than by a fork.

**Two traps here, both worth writing down:**

1. **`SnapshotManager::epoch_` is not `Server::commit_seq_`.** `epoch_` (`snapshot.h:96`, written into
   the file header per `DESIGN-SNAPSHOT.md:161`) is the snapshot lane's own counter;
   `commit_seq_` (`server.h:559`) is the atomic-group ticket source, advanced **only** by
   `atomic_commit()` and therefore **not advanced at all when `--atomic 0` and no `EXEC` runs**.
   The manifest must record both, and must never use one as the other. The BASE↔INCR boundary is
   *positional* (segment number), full stop.
2. **A group can straddle the Mark.** A group whose fragments installed before Freeze but whose
   `GCMT` is enqueued after the Mark would put fragments in segment N and the commit in N+1. Since
   the snapshot's frozen table already contains the installed candidates, and replay of segment N+1
   would re-apply them, the group must be resolved on one side. Cleanest rule: **the Freeze barrier
   must drain in-flight groups**, i.e. do not enter Freeze while `atomic_inflight() != 0`.
   The counter already exists and is already published in `INFO` (`t_server.cc`, `atomic_inflight`),
   and the admission window bounds the drain. This should be an explicit precondition on
   `SnapshotManager::start`, asserted, not assumed.

**Auto-rewrite trigger.** Copy Redis's shape (`server.c:1741-1753`) — percentage growth over the size
at last rewrite, floored by a minimum size, with `aofRewriteLimited()`'s exponential backoff
(`aof.c:1186-1216`) after repeated failure. Redis's own caveat applies verbatim: it does not persist
`aof_rewrite_base_size`, so it re-initializes to the BASE size on restart and may rewrite early
(`aof.c:2205-2214`). TomoKV *can* persist it in the manifest and should — it is one integer.

### B7. Record content: post-image vs command — the decision that sizes the whole lane

This is the highest-leverage choice, so state both options and pick.

**Option R1 — command-effect records (Redis/Dragonfly/Garnet all do this).** Journal the
deterministic rewritten command. Requires the whole determinism-rewrite surface: relative TTL →
absolute (`expire.c:824-833`), `GETEX` → `PEXPIREAT`/`DEL`/`PERSIST` (`t_string.c:520-535`),
`SPOP`→`SREM`, `SRANDMEMBER count`, `INCRBYFLOAT`, `RANDOMKEY`, `SORT BY` with a nondeterministic
pattern, Lua (`src/cmd/scripting.cc`, 873 lines — must journal effects, not the script), plus every
`CmdFlags::RandomShard` command (`command.h:27`). Redis's AOF bug history is concentrated here.

**Option R2 — post-image records.** Journal `{key, type, encoding, expire_at_ms, payload}` — i.e.
exactly the existing `RECD` snapshot record (`flatstore.h:917-939`) — produced by the already-shipping
`SnapshotBeginSaveHook`/`SnapshotReadSaveHook` (`format.h:53-56`), plus a `DEL` record. Replay = the
existing snapshot loader. **Determinism becomes structurally impossible to get wrong**: there is no
command to re-execute, so there is nothing to be nondeterministic. Lua, `SPOP`, `RANDOMKEY`, relative
TTLs and `INCRBYFLOAT` all disappear as problems. It also unifies BASE and INCR into one record
grammar and one loader.
Cost: write amplification. `HSET` on a 1 MB hash writes 1 MB. `RPUSH` on a long list ditto.

**Recommendation: R2 by default, with a size escape hatch, and R1 added only where measurement
demands it.** Rationale: (i) the serializers already exist and are already gate-tested
(`tests/snap_typed_roundtrip.py`, `snap_typed_race.py`); (ii) it deletes the single largest
correctness surface; (iii) string workloads — the ones this project benchmarks — have no
amplification at all, because a `SET` post-image *is* the command payload; (iv) the resumable
`SnapshotElementEmitter` (`format.h:107-130`) already handles values larger than a chunk without
blowing the loop budget, which is the same constraint AOF has.
The escape hatch is a numeric knob (`aof-image-max`, §c) above which a collection write emits an R1
command record instead. Per the hardcode-or-delete rule this starts as a measured threshold and
either earns its default or gets deleted.

**Silent mutations that must be journaled either way.** These have no command behind them and are the
classic source of replay divergence:

| source | code | required record |
|---|---|---|
| lazy expiry | `FlatStore::find` / `ExpireIndex` (`flatstore.h:89-146`) | `DEL` |
| active expiry | `ExLoop`, `kActiveExpireChecks = 20` (`ex_loop.h:41`) | `DEL` |
| eviction | `make_room_for` (`flatstore.h:1158`), `choose_victim` (`:1088`), `kEvictionsPerOp = 16` | `DEL` |
| `FLUSHALL/FLUSHDB` | `clear_during_snapshot()` (`flatstore.h:764`), `atomic_tombstone_all()` (`flatstore_atomic.inc:402`) | one `FLUSH` record per shard stream (Garnet uses opcodes `0x60/0x61`, `AofEntryType.cs`) |

**Snapshot interaction trap.** During capture, lazy expiry "reports the key absent to the command but
leaves its physical object for traversal", and active expiry is paused
(`DESIGN-SNAPSHOT.md:122-124`). So the *logical* expiry moment and the *physical* free are separated.
The journal must emit the `DEL` at the logical moment (when the command first observes absence), or
the BASE (which still contains the key) and the INCR (which never deleted it) will disagree and the
key resurrects on recovery. This is riskiest-part #3 in §f.

### B8. Where the code lands

| new/changed | what | est. lines |
|---|---|---|
| `src/journal/format.h` (new) | frame/record/`GCMT`/manifest encoding; mirrors `snapshot/format.h` | 200 |
| `src/journal/journal.h/.cc` (new) | `JournalWriter`: segments, manifest, `pending_commits`, io_uring fsync, `written_seq_`/`durable_seq_`, short-write truncate, sticky error | 1000 |
| `src/journal/replay.cc` (new) | two-pass loader; reuses `snapshot_load_owned`'s insert path | 450 |
| `src/store/flatstore.h` (+`journal.inc`) | per-shard producer chunk, `journal_emit_post_image()`, expiry/eviction/flush taps | 350 |
| `src/core/ex_loop.h` | emit at the existing write commit point — the `(CmdFlags::Write \| CmdFlags::SnapshotWrite)` test at `ex_loop.h:562`, immediately before `op.state.store(Done)` at `:568`. **Note the existing call there (`multi_plain_write_committed`) is additionally gated on `sh.has_watches()` at `:563`; the AOF emit must sit at the same place but *not* behind that guard.** Chunk post on batch end | 120 |
| `src/cmd/scatter_engine.inc`, `multi.inc`, `xshard_commands.inc` | `GCMT` enqueue at the three publish points | 120 |
| `src/core/io_loop.h` | the send gate (§B3) | 60 |
| `src/core/config.h`, `src/cmd/t_server.cc` | 7 Redis knobs + `INFO PERSISTENCE`/`STATS` counters; replace the `appendonly` stub (`t_server.cc:218`) | 250 |
| `src/snapshot/snapshot.cc` | Mark-time segment switch; Freeze precondition `atomic_inflight()==0` | 80 |
| `tests/` | `aof_replay.py`, `aof_torn_group.py`, `aof_fsync.py`, `aof_rewrite.py`, gate.sh section | 900 |

C++ total ≈ **2,600–3,200 lines**, plus ~900 lines of test. For calibration the snapshot lane is
`snapshot.cc` 602 + `format.h` 132 + `snapshot.h` 133 plus its `flatstore.h`/`ex_loop.h` hooks — call
it ~1,200. **AOF is ≈ 2.5× the snapshot lane.**

---

## (c) Exact knob formats to adopt

Compat rule (`NOTES-COMPAT.md:37-46`): Redis names, Redis value grammar, exposed through the same
`init_config` table. Note `appendonly` **already exists as an inert hardcoded `"no"`**
(`src/cmd/t_server.cc:218`) — this replaces a stub, it does not add a name. `redis-benchmark` probes
`CONFIG GET appendonly` before every run (`NOTES-COMPAT.md:20`), so the name must keep working.

Two surfaces must both be fed, from the one `Config` struct (`config.h:1-5`): the CLI/conf parser
(`parse_config_args`, `config.h:130`) and the `CONFIG SET/GET` table (`init_config`, `t_server.cc:211`).

| Redis name | CLI/conf | `ConfigKind` | default | meaning in this design | mutable at runtime |
|---|---|---|---|---|---|
| `appendonly` | `--appendonly yes\|no` | `Bool` | `no` | Master switch. `no` ⇒ **no producer chunk allocated, no writer thread role assigned, one predicted branch on the write path** (B0.4/B0.5). `no→yes` at runtime triggers an immediate AOFRW (Redis's `AOF_WAIT_REWRITE`, `aof.c:1288-1329`); until it completes, records accumulate into a temp segment. `yes→no` closes and fsyncs the current segment (`aof.c:1260-1284`). | yes |
| `appendfsync` | `--appendfsync always\|everysec\|no` | new `Enum` kind | `everysec` | Exactly Redis's three values (`config.c:3381`). Selects the writer's fsync trigger **and** the reply gate tier (§B3, §B5). Rejecting any other value is required — `redis-cli CONFIG SET appendfsync foo` must error. | yes |
| `appendfilename` | `--appendfilename NAME` | `String` | `"appendonly.aof"` | Basename for segment and manifest files. Must be a plain filename, not a path — Redis validates this (`config.c:2392-2396`, `isValidAOFfilename`); `validate_config` already does the identical check for `dbfilename` (`config.h:297-301`), reuse it. Immutable at runtime, like Redis (`IMMUTABLE_CONFIG`, `config.c:3354`). | no |
| `appenddirname` | `--appenddirname NAME` | `String` | `"appendonlydir"` | Directory under `dir` holding manifest + segments + the BASE snapshot. Must be a plain dirname (`config.c:2452-2456`). Created on demand (`aof.c:3207-3212`). Immutable (`config.c:3355`). | no |
| `auto-aof-rewrite-percentage` | `--auto-aof-rewrite-percentage N` | `Unsigned` | `100` | Growth percent over the size at last rewrite that triggers AOFRW. **`0` disables auto-rewrite entirely** and, per B0.4, must then run no sizing arithmetic at all. Redis semantics verbatim (`server.c:1742-1749`, `config.c:3398`). | yes |
| `auto-aof-rewrite-min-size` | `--auto-aof-rewrite-min-size BYTES` | `Bytes` | `64mb` | Floor below which growth is ignored (`server.c:1743`, `config.c:3505`). `parse_bytes` already accepts `K/KB/M/MB/G/GB` (`t_server.cc:245-260`). | yes |
| `aof-timestamp-enabled` | `--aof-timestamp-enabled yes\|no` | `Bool` | `no` | Redis writes `#TS:<unix>\r\n` annotations (`aof.c:1642-1651`), skipped on load (`aof.c:1914`), consumed by `redis-check-aof --truncate-to-timestamp`. TomoKV's stream is binary, so the equivalent is a `TSMP` record emitted into a shard stream when that owner's cached `now_ms` (`Shard::set_cached_now_ms`, `shard.h:74`) crosses a second boundary. `no` ⇒ zero bytes and no timestamp comparison (B0.4). | yes |

**`aof-use-rdb-preamble` — the one that must not lie.** In Redis this selects the BASE encoding
(`aof.c:3140-3148`). TomoKV has exactly one base encoding: the TOMOSNP snapshot. Options considered:
report `yes` and silently ignore `no` (violates "a field nothing reads is a lie"); reject the name
(breaks nothing measured, but is a gratuitous incompatibility). **Recommendation: expose it,
`CONFIG GET` returns `yes`, and `CONFIG SET aof-use-rdb-preamble no` returns
`ERR aof-use-rdb-preamble no is unsupported: the AOF base file is a TomoKV snapshot`.** Nothing in
the surveyed client/tool surface (`NOTES-COMPAT.md:14-21`) sets it, so the compat exposure is
sufficient and the error is honest.

**Knobs to *not* adopt**, with reasons: `no-appendfsync-on-rewrite` (Redis needs it because the
rewrite child contends with the parent's fd; TomoKV's rewrite is fork-free and the snapshot writes a
different file — nothing to suppress); `aof-rewrite-incremental-fsync` (the snapshot writer already
does bounded 64 KiB frames + one `fdatasync`, `snapshot.cc:368`; there is no 4 MB page-cache pile-up
to defend against); `aof-disable-auto-gc` (Redis-internal test hook, `config.c:3334`).

**New TomoKV-native numeric knobs** (Redis has no name, so use the house grammar — numeric, `0` = off,
`-1` = auto):

- `--aof-image-max N` (`Bytes`, default `-1` = auto): post-image records above `N` bytes fall back to
  a command record (§B7). Auto resolves from `kSnapshotChunkBytes` (`format.h:22`). Per
  hardcode-or-delete this must earn its default from a PRE/POST table or be deleted.
- `--aof-writer-tid N` (`Unsigned`, `-1` = auto): which IO thread owns the journal file, so a
  placement experiment can isolate it. Auto = the last `ifid` thread.

**INFO fields to add** under `# Persistence` (which already exists, `t_server.cc:720-731`), mirroring
Redis's names (`server.c:6650-6669`) so existing dashboards work: `aof_enabled`,
`aof_rewrite_in_progress`, `aof_rewrite_scheduled`, `aof_last_bgrewrite_status`,
`aof_last_write_status`, `aof_base_size`, `aof_current_size`, `aof_pending_rewrite`,
`aof_delayed_fsync`. Plus, per the vacuous-validation rule, **fired-counters that a gate can assert**:
`aof_records_written`, `aof_groups_committed`, `aof_groups_skipped_on_replay`, `aof_fsyncs`,
`aof_send_gate_waits`.

---

## (d) Steal candidates and avoid-list

### Steal

| from | what | why |
|---|---|---|
| Redis `aof.c:540-611` | manifest write: temp → `fsync` → `rename` → `fsyncFileDir` | The only crash-atomic way to swap a pointer file. `snapshot.cc:361-378` already does exactly this for the dump; reuse the code path. |
| Redis `aof.c:43-72, 119-141` | BASE/INCR/HISTORY manifest with `seq` + `startoffset`/`endoffset` | Makes "which files, in which order, from which offset" a data question rather than a naming convention. Also gives crash-safe rewrite for free (a failed rewrite leaves the old manifest). |
| Redis `aof.c:1970, 2022-2027` | `valid_before_multi` rewind | The all-or-nothing group idea in its cheapest possible form. §B4 is its N-stream generalization. |
| Redis `aof.c:2159-2162, 2192-2196` | only the **last** file may be truncated; a truncated interior file is fatal | Turns "silent data loss" into "loud refusal to boot". Non-negotiable. |
| Redis `aof.c:1186-1216` | `aofRewriteLimited()` exponential backoff after 3 consecutive failures | Prevents a failing rewrite from spraying tiny segments. Cheap: two statics. |
| Redis `aof.c:1515-1526` | `ftruncate` back to last good offset on short write | With one file and a tracked `appended_bytes`, exact. |
| Redis `debug.c:660-676` | `DEBUG LOADAOF` — flush, empty, reload in-process | The round-trip harness. TomoKV has **no `DEBUG` command at all** today; this lane should add exactly this one subcommand, because without it every replay test needs a process restart. |
| Redis `server.c:2064-2066` | write-before-reply ordering | The property that makes the recovered state a prefix of the acknowledged history (§B3). |
| Dragonfly `snapshot.cc:86-87, 128-148` | snapshot registers as a journal consumer; final LSN written *into* the stream at handoff | The composition pattern. TomoKV's analogue is the Mark-time segment switch (§B6) — same idea, positional instead of LSN-valued. |
| Dragonfly `journal_slice.cc:79-106` | thread-local slice, plain `lsn_++`, no atomics on the append path | Confirms the per-shard-producer shape is the right one for a shard-owned server. |
| Dragonfly `transaction.cc:1630-1632` | do not journal a command whose `result.status != OpStatus::OK` | Obvious in hindsight, easy to get wrong, and a silent corruption if missed. |
| Dragonfly `journal.h:48-72` | `DisableFlushGuard` — emit N records with no consumer interleaving | The shape for "one command produces several records" (e.g. a collection write plus its `DEL`). |
| Garnet `AofEntryType.cs:6-104` | byte opcodes grouped by numeric range, with `HasKey()` derived from the range | Range-grouped opcodes make "is this a control record?" one comparison. Better than Redis's `'#'` prefix sniffing (`aof.c:1914`). |
| Garnet `AofHeader.cs:125-217` | explicit-layout header carrying `storeVersion` per record | Cheap forward-compat: a record knows which format wrote it. |
| Garnet `StoreWrapper.cs:388-390` | `RecoverCheckpoint → RecoverAOF → ReplayAOF(-1 = to tail)` as three named, ordered steps | Clean recovery API shape. |
| Garnet `SingleDatabaseManager.cs:254` | `recordToAof: false` during replay | Explicit re-entrancy guard. Redis achieves the same by setting `server.aof_state = AOF_OFF` during load (`aof.c:1843`) — do one of them, deliberately. |
| Garnet `DatabaseManagerBase.cs:288-292` | checkpoint start/end markers written *into* the AOF | A useful cross-check even when the boundary is positional: replay can assert it landed where the manifest says. |
| Garnet `GarnetServerOptions.cs:978-1018` | size-invariant validation with explicit error text | `validate_config` (`config.h:292`) is the right home for the analogous checks. |

### Avoid

| what | why |
|---|---|
| **A global append buffer / MPSC log** | Reintroduces exactly the shared-line RMW that `ExQueue`'s N×M SPSC design was built to remove (`exqueue.h:1-15`). §B1. |
| **Per-shard output *files*** | 256 fds, 256 fsyncs/tick, and — fatally — a ragged recovery point that is not a prefix of any real history. §B1. |
| **fork for rewrite** | The Freeze/Mark cut already gives a fork-free consistent image (`DESIGN-SNAPSHOT.md:34-57`) and every type lane can serialize (§0.2). Forking a server whose data is in per-thread arenas also mis-prices COW badly. |
| **A bio thread for fsync** | `IORING_OP_FSYNC` on the writer's existing ring makes fsync a completion. Adopting Redis's `bio.c` here would import `aof_flush_postponed_start`, the 2s grace, and `aof_delayed_fsync` (`aof.c:1438-1458`) to solve a problem we would have created ourselves. §B5. |
| **Dragonfly's replay model** (per-shard flows applied independently, barrier only for `FLUSHALL`) | `replica.cc:1241-1300` + `tx_executor.cc:78-94`. Correct for an eventually-consistent replica; **wrong for recovery**, where TomoKV's atomic-group promise must survive. |
| **Dragonfly's deprecated `shard_cnt` field** | `serializer.cc:78` writes a hard-coded `1u`. A group record must carry a real participant vector, not a count that has drifted into a placeholder. |
| **Garnet's global `sequenceNumber` on every sharded record** | `GarnetLog.cs:660`. It buys cross-shard replay order — which the one-file/one-writer design already gets from byte position, for free. Paying a global atomic per record would recreate the §B1 bottleneck. (`Server::commit_seq_` is fine because it ticks **once per group**, not per record, and only when atomics are on.) |
| **Redis's `SELECT` records** | `aof.c:1677-1684`. TomoKV is single-database — `CONFIG GET databases` returns 1 (`t_server.cc:224`). Emitting `SELECT` would be pure ceremony. |
| **RESP-text record format** | Redis's AOF is RESP so `redis-check-aof` can parse it. TomoKV's snapshot format is binary, checksummed, and framed (`format.h`), and its loader is already written. Two grammars in one lane is a bug farm. |
| **`aof-use-rdb-preamble no` (command-stream BASE)** | `rewriteAppendOnlyFileRio` (`aof.c:3013`) is ~600 lines of per-type command emission that would duplicate the `SnapshotBeginSaveHook` lanes we already have and already gate-test. §c. |
| **A dirty counter** | Redis's `server.dirty` drives `save` points, which TomoKV does not implement (`CONFIG GET save` is an empty stub, `t_server.cc:214`). Adding one to serve AOF alone is a field nothing reads (B0.4). Auto-rewrite keys off file size, which is already tracked by the writer. |
| **Growing `Op` or `Client`** | `static_assert(sizeof(Op)==336)` / `sizeof(Client)==1984`. §B3's gate is designed around this. |

---

## (e) Validation plan

The governing rule is `tests/gate.sh:11-13` — *"every section proves its mechanism FIRED (counters,
accepts, direct>0), not merely that nothing crashed"* — reinforced by the recorded
vacuous-validation incident (two dead-tracer batteries, 2026-08-19). **Every battery below asserts a
counter moved.** And per the box-noise / one-server-one-bench rules, timing checks run alone.

**Prerequisite: add `DEBUG LOADAOF`.** TomoKV has no `DEBUG` command today. Without an in-process
reload (Redis `debug.c:660-676`) every replay assertion costs a process restart, which makes the
batteries slow enough that they will get skipped. This is a validation-infrastructure dependency,
not a nice-to-have.

### E1. `tests/aof_replay.py` — byte-exact crash-replay round trip

Shape follows `tests/flush_capture.py` (populate → act → restart with `--load` → verify) and
`tests/differ.py` (diff every reply against an oracle).

1. Populate a mixed workload across all five type lanes and all shards: strings around the
   `kEmbedThreshold = 192` boundary (`kvobj.h`), integer-encoded strings at signed limits, values
   larger than `kSnapshotChunkBytes = 64 KiB` (`format.h:22`), hashes/lists/sets/zsets on both sides
   of every `*-max-compact-{entries,value}` threshold, persistent keys, future TTLs, and keys that
   expire between save and load.
2. Snapshot the live state via a full `SCAN` + per-key `TYPE`/`OBJECT ENCODING`/`PTTL` + value read
   → the model.
3. `SIGKILL` the server (not `SIGTERM` — a clean shutdown flushes and proves nothing).
4. Restart with the same `--shards` and `--appenddirname`. Compare **every** key/value/encoding/TTL
   to the model.
5. **Fired assertions:** `aof_records_written > 0`, replayed record count > 0 in the boot log,
   and `aof_groups_skipped_on_replay` is *reported* (0 is legal here, non-zero is legal in E2).
6. **Negative control:** the same run with `appendonly no` must lose the data. A battery that passes
   with the feature off is testing nothing.

Byte-exactness harness: rather than compare files (segment boundaries legitimately differ), compare
**states**. Take a `BGSAVE` before the kill and a `BGSAVE` after recovery, and require the two
`dump.tomo` files to be **frame-payload-identical after normalizing the header's `epoch`/`cut_ms` and
the `(sid, sequence)` frame ordering** — i.e. per-shard concatenated `RECD` streams must match byte
for byte. This is a genuine byte-exactness test and it reuses the existing format, checksums, and
reader (`snapshot_read_plan`, `snapshot.cc:467`).

### E2. `tests/aof_torn_group.py` — kill mid-group-publish, replay must be all-or-nothing

Modelled on `tests/atomic_torn.py`, which already establishes the pattern of proving the anomaly
exists with the gate closed before asserting it is absent with the gate open.

1. Run with `--atomic 1`. Drive `MSET k0..k7` groups whose keys deliberately span shards (the
   `--command-key-pattern=R` template from `NOTES-ATOMICS.md:100-102`), plus `EXEC` groups, plus one
   arm per broadened family (`RENAME`, `SINTERSTORE`, `LMPOP`, `COPY`) per `NOTES-ATOMICS.md:265-272`.
2. `SIGKILL` at randomized points, and — this is the part that makes the test non-vacuous — **also
   at a directed point**: a debug-gated fault injector that kills after N fragments have been
   appended but before `GCMT` is written. Without the directed arm, the random arm will essentially
   never hit the window, and the battery will pass while testing nothing.
3. After replay, every group must be entirely present or entirely absent. For `MSET k0..k7`, the
   count of present keys must be 0 or 8, never 1..7.
4. **Fired assertions:** `aof_groups_committed > 0`; the directed-kill arm must produce
   `aof_groups_skipped_on_replay > 0` (proving the skip path executed, not merely that nothing tore);
   the `--atomic 0` control arm must show a partial group (1..7 keys) for cross-shard `MSET`,
   confirming the journal reflects real semantics and does not over-promise (§B4).
5. **Writer-ordering assertion:** a debug-only scan of the produced file must verify, for every
   `GCMT(G)`, that every fragment `G` names appears at a **lower byte offset**. This is the §B4
   invariant, checked directly rather than inferred.
6. Straddle case: trigger AOFRW concurrently with group traffic and assert no group has fragments in
   segment N and its `GCMT` in N+1 (the §B6 trap).

### E3. `tests/aof_fsync.py` — policy timing checks

Run alone (`boxguard`/one-server-one-bench rule); all three arms on the same box, same placement.

| check | method | assertion |
|---|---|---|
| `always` durability | write, kill -9 immediately after the reply is read, restart | **zero** acknowledged writes lost, over ≥10k iterations |
| `always` gate fired | `INFO` | `aof_fsyncs ≈ aof writer passes`, `aof_send_gate_waits > 0` |
| `everysec` bound | write continuously, kill -9, count losses | loss window ≤ ~1s of writes; **and** `aof_fsyncs ≈ elapsed_seconds ± 1` |
| `everysec` process-crash safety | `SIGKILL` the process (page cache survives) | **zero** acknowledged writes lost — this is the property the write-gate buys (§B3) |
| `no` | same | losses permitted; the point is that `aof_fsyncs == 0` |
| idle fsync | stop writing with unsynced bytes outstanding, wait 3s | `aof_fsyncs` still advances once (Redis's `aof.c:1414-1423` case, easy to miss) |
| cost | PRE/POST table, `appendonly no` vs each policy, at p1 and p32, GET/SET | `appendonly no` within box noise (±2% on 7700X, ±0.15% on EPYC) of the pre-lane binary — this is the B0.5 ≤3% gate |

The `appendonly no` row is the one that decides whether the lane ships at all, and per the
saturated-benching rule it needs both p1 and p32 cells, not just p1.

### E4. `tests/aof_rewrite.py` — AOFRW correctness

1. Continuous mixed write traffic; trigger AOFRW manually (`BGREWRITEAOF`) and via
   `auto-aof-rewrite-percentage`.
2. Kill at each stage: before the Mark, during capture, after capture but before the manifest
   rename, after the rename but before HISTORY deletion. **Every restart must recover a complete,
   correct state** — this mirrors `DESIGN-SNAPSHOT.md:292-300`'s kill matrix.
3. Manifest invariants after each recovery: exactly one BASE; INCR sequence numbers contiguous and
   increasing; no interior file truncated (the Redis rule, `aof.c:2159-2162`).
4. **Fired assertions:** `aof_rewrite_in_progress` observed as 1; `aof_base_size` changed;
   HISTORY files actually unlinked; `aof_current_size` reset.
5. Corruption matrix, following `DESIGN-SNAPSHOT.md:289`: corrupt manifest line, frame checksum,
   record length, `GCMT` participant vector, and a truncated *interior* segment — **each must fail
   the boot explicitly, before bind**, never silently load a partial keyspace.
6. Backoff: force three consecutive rewrite failures (read-only `appenddirname`) and assert the
   fourth is delayed, then that recovery clears the limiter (`aof.c:1190-1195`).

### E5. Differential and integration

- Extend `tests/differ.py` with an AOF mode: run all nine existing suites, then `DEBUG LOADAOF`, then
  re-diff the entire keyspace against the Redis oracle. Zero diffs required at `--atomic 0` and
  `--atomic 1`, matching the existing bar (`NOTES-ATOMICS.md:282-286`).
- Run the whole set under ASAN/UBSAN. Watch the `.make-settings` cached-`SANITIZER` trap
  (`ldd`-check the binary actually under test) and the "verify the validation binary IS the shipping
  binary" rule — a relaxed-RMW diagnostic counter can itself fix a Heisenbug.
- `tests/gate.sh quick` gains an AOF section: boot with `appendonly yes`, run `aof_replay.py` and the
  directed arm of `aof_torn_group.py`, assert the fired counters, assert the shutdown invariants
  still read `stuck: live_conns=0 rob_not_quiesced=0 unsent_bytes_pending=0`.

---

## (f) Build size and the riskiest parts

**Size: ≈2,600–3,200 lines of C++ and ~900 lines of Python/shell tests** — roughly 2.5× the existing
snapshot lane (§B8). Sequencing that keeps each step independently gateable:

1. Format + writer + `DEBUG LOADAOF` + replay, `appendonly` as a boot-only flag, `appendfsync no`,
   auto-rewrite off. Gate: E1.
2. Group bracketing at the three publish points. Gate: E2.
3. fsync policies + io_uring fsync + the send gate. Gate: E3 (including the ≤3% off-path table).
4. AOFRW on the snapshot cut + manifest + auto-rewrite. Gate: E4.
5. `CONFIG SET appendonly yes/no` at runtime (the `AOF_WAIT_REWRITE` dance) — genuinely the fiddliest
   part of Redis's AOF and the least valuable; do it last or not at all.

### Riskiest part 1 — the write-before-reply gate on the retire path

This touches the exact code that has repeatedly lost replies in this codebase. `ex_loop.h:661-666`
carries a comment marking a lost-wakeup bug as *"defect 5, third appearance"* (12 stranded
connections), and `io_loop.h:683-693` documents 3 lost replies in 87M traced to publishing an Op in
the wrong order relative to the ROB. Adding a **second condition** to "may I send now?" is precisely
the class of change that has caused those. Specific hazards:

- A gate that is checked but whose satisfaction produces no wake ⇒ a connection parks forever with a
  complete reply staged. The writer's `written_seq_`/`durable_seq_` advance **must** wake every IO
  thread that is blocked on it — and must do so via a mechanism that a lost notify-mask bit cannot
  defeat, because the mask is explicitly a hint here, not the authority (`ex_loop.h:104-106`,
  `thread.h:309-326`). The mask-independent parking sweep must include the gate.
- The footprint locks forbid the obvious fix (a per-op sequence), forcing the coarser per-thread
  watermark, which couples unrelated connections on one IO thread.
- Under `always` the gate is on the critical path of every write; a writer that stalls (slow disk)
  converts into head-of-line blocking for every connection on that IO thread. Redis's answer is
  `exit(1)`; ours must at minimum be an explicit, observable stall counter (`aof_send_gate_waits`)
  and a bounded wait.

### Riskiest part 2 — the group commit-record dependency in the writer

The invariant "`GCMT(G)` never precedes any fragment of `G`" is the whole of §B4's correctness, and
it lives in a place with no natural test: a table inside a single-threaded writer. Hazards:

- Chunk-granularity batching means a fragment can sit unposted in a producer chunk indefinitely on
  an idle shard, holding its `GCMT` hostage and stalling every later group behind it. Needs a
  bounded flush deadline on producer chunks, which interacts with the `everysec` timer.
- The `pending_commits` table is bounded by `--atomic-window` (AUTO = `min(16*shards,1024)`), but
  that window is a *credit* system with generation-based reconfiguration (`server.h:236-311, 486`).
  A window resize while commits are pending must not lose an entry.
- Aborted groups never produce a `GCMT`, so they cannot leak the table — but a `GCMT` whose fragment
  never arrives would, and would also be a silent correctness hole. This must be an assertion with a
  bounded age-out that **fails the AOF loudly**, not a best-effort skip.
- Straddling the AOFRW Mark (§B6 trap 2). The proposed precondition — do not enter Freeze while
  `atomic_inflight() != 0` — must be asserted, and its drain must be bounded or `BGREWRITEAOF` can
  hang under sustained group traffic.

### Riskiest part 3 — the three silent mutation sources, and their snapshot interaction

Lazy expiry, active expiry, and eviction mutate the store with no command behind them (§B7 table).
Each missing tap is a silent divergence that only shows up as a resurrected key after a restart,
which is the hardest possible failure to attribute. The snapshot interaction makes it worse: during
capture, lazy expiry deliberately reports a key absent while leaving the object for traversal, and
active expiry is paused (`DESIGN-SNAPSHOT.md:122-124`, `flatstore.h:632, 706-707`). So the logical
`DEL` and the physical free are separated in time, and journaling at the physical moment puts the BASE
and the INCR into disagreement. Eviction adds a second-order problem: it is driven by `maxmemory`,
which is *not* a deterministic function of the recovered state, so a replay must never re-derive
evictions — it must apply the journaled `DEL`s, which is exactly what makes the tap mandatory rather
than optional.

### Honourable mentions (not top-3, but do not discover these late)

- **Shard migration vs. an open producer chunk** (§B2). O(1) reshard means ownership can move
  mid-stream; without a flush barrier at the handoff, a shard's `(sid, sequence)` stream forks.
- **`appendonly no` must be free.** One predicted branch, no allocation, no writer role assigned.
  This is the B0.5 gate and it is the thing most likely to be quietly violated by, say, an
  unconditional `now_ms` comparison for the timestamp knob.
- **`aof-image-max` must earn its default** (hardcode-or-delete). If the post-image/command hybrid
  cannot show a consistent gain in a PRE/POST table, ship post-image only and delete the knob.
