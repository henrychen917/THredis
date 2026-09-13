# t-aofframe: the AOF writer produced a file its own loader refuses

**Verdict: the WRITER is wrong.** The loader's invariant is load-bearing, not over-strict. The fix
is one guard at the writer's single control-frame choke point.

| | before | after |
|---|---|---|
| AOF battery runs, `--persist-io normal` | **11 interleaves in 114 runs** | **0 in 120 runs** |
| AOF battery runs, `--persist-io uring` | 0 in 117 runs | 0 in 90 runs |
| directed battery `tests/aof_frame_order.py`, normal | **fails 6 of 6** (guard removed) | passes 16 of 16, window fired every run |
| server restart on a produced file | `AOF load plan failed: AOF control record interleaves a large record`, exit 1 | boots and replays |

---

## 1. What was observed, and what it actually is

A gate run's AOF replay boot exited before listening; the battery then got ConnectionRefused. The
server's own log said why:

    AOF load plan failed: AOF control record interleaves a large record

`src/persist/aof.cc:2207`. The loader walks physical frames holding an `active_large` flag; a
control frame (`sid == UINT32_MAX`, the physical control stream that carries GCMT group-commit
records) arriving while that flag is set rejects the whole file, and `src/main.cc:121` turns that
into `return 1`.

**Severity, stated exactly.** Writes were acknowledged to clients and are in the file. After a
restart the server does not load that file at all and does not listen: a refuse-to-start, not a
partial loss. It is not confined to an unclean stop — see §5, a clean `SIGTERM` stop with the
normal shutdown invariants printed produced an unloadable file in 2 of 27 runs, because the
offending frame is written mid-run and is simply still there when the file is closed. It does need
the ordering window, which is why it is intermittent.

## 2. Reproduction (before the fix)

Detector: `tests/aof_frames.py check FILE` re-implements the loader's frame walk and prints
`frames= large_records= control_frames= interleaved_control= other_violations=` on every file,
clean or not. The shape counts are printed always so that "0 interleaves" can never be vacuous,
and the clean runs are the detector's own negative control — it reported `interleaved_control=0`
on 220 of 231 pre-fix runs and non-zero on 11.

Loop: boot on cores 32-47 / port 7500, run the gate's own `tests/aof.py … populate` workload, stop
the server (pid resolved from the listening socket), walk the file. ~0.6 s per run.

| engine | atomic | stop | runs | interleaved |
|---|---|---|---|---|
| normal | 1 | SIGKILL | 60 | **5** |
| normal | 0 | SIGKILL | 27 | **4** |
| normal | 1 | SIGTERM (clean) | 27 | **2** |
| uring | 1 | SIGKILL | 102 | 0 |
| uring | 0 | SIGKILL | 15 | 0 |
| **total** | | | **231** | **11** |

No artificial load was needed: the first hit came on run 5 of a quiet box. Machine load widens the
window but is not required. Both `--atomic` modes hit, matching the original report that it landed
on different arms in different runs.

## 3. The frame dump that settles it

`tests/aof_frames.py dump <rejected file> --around 12457992`:

```
#391   @12221920   sid15   seq=351    flags=B-   len=65536   PUT key=x:expanded len=84968
#392   @12287496   sid15   seq=352    flags=-E   len=19482   (large-record continuation bytes)
#393   @12307018   sid15   seq=353    flags=B-   len=65536   PUT key=x:expanded len=85268
#394   @12372594   sid15   seq=354    flags=-E   len=19782   (large-record continuation bytes)
#395   @12392416   sid15   seq=355    flags=B-   len=65536   PUT key=x:expanded len=85568
#396   @12457992   CTRL    seq=0      flags=--   len=52      GCMT ticket=3 names=(sid3,seq5)
#397   @12458084   CTRL    seq=0      flags=--   len=60      GCMT ticket=5 names=(sid1,seq8),(sid3,seq6)
#398   @12458184   CTRL    seq=0      flags=--   len=52      GCMT ticket=2 names=(sid4,seq2)
#399   @12458276   sid15   seq=356    flags=-E   len=20082   (large-record continuation bytes)
#400   @12478398   sid15   seq=357    flags=B-   len=65536   PUT key=x:expanded len=85868
#401   @12543974   sid15   seq=358    flags=-E   len=20382   (large-record continuation bytes)
```

Frame #395 opens an 85568-byte `x:expanded` PUT on shard 15 (`B-` = LargeBegin). Three GCMT control
frames land at #396-#398 — tickets 3, 5 and 2, naming fragments on shards 3, 1 and 4 that were all
written far earlier. Frame #399 closes the record (`-E` = LargeEnd). Three commits, written in one
go, inside one open large record.

A second dump, from a **clean SIGTERM** run, shows the same thing around `s:large` (70000 bytes,
2 frames) with four GCMTs at frames #436-#439 inside the record opened at #435.

## 4. Which side is wrong, and why

### The mechanism (writer side)

`AofManager::writer_pass` called `drain_pending_commits()` **before** consulting the physical-stream
lock, at two sites:

```cpp
consumed += drain_pending_commits(budget, ring, last_write);   // top of the pass, no lock check
if (locked_producer_ != UINT32_MAX) { drain_producer(locked_producer_, …); return …; }
…
if (budget) consumed += drain_pending_commits(budget, ring, last_write);  // lock may have been taken
```

`locked_producer_` is the lock a large record takes on the physical stream: while it is set the
writer drains **only** that producer, so no other shard's frames can interleave. Control frames
bypassed it entirely.

How the window opens: a producer emits a large record by sealing and posting 64 KiB chunks *as it
serializes* (`AofProducer::emit`), so the writer can write the LargeBegin frame and take the lock
while the rest of the record is still being built. `drain_producer(locked_producer_)` keeps running
during the lock and moves any group-commit chunk from that producer's channel into
`pending_commits_`; a group's dependencies can also become ready during the lock, because
`note_group_fragment` fires as that producer's fragment frames are written. Either way, at the top
of the next pass there is a ready GCMT in hand and a large record still open — and the pre-fix code
wrote it.

Producers are per **ex thread** (`chunk_in_[producer]`), while keys are placed per shard and a
thread owns several shards, so a thread holding a large record open is routinely the same thread
that commits a group. That is why the natural workload hits at ~10%.

### Why the loader is right

Re-assembly alone would tolerate the interleave: control frames go to `plan->control_section` and
shard frames to `plan->sections[sid]`, so the large record's bytes are contiguous *within its
section* regardless of what sits between its frames in the file. The invariant is not about
re-assembly. It is about **truncatability**, and every recovery path in the tree depends on it:

* loader, `aof_read_plan`: an unterminated large record rewinds to `valid = large_file_pos`, the
  large record's **first frame offset**, and truncates there.
* writer, `writer_shutdown`: with the lock still held it `ftruncate`s to `large_record_offset_` —
  again the large record's first byte.
* writer, `write_frame_normal` / `write_group_commit_normal`: a failed write rolls back to
  `last_good_offset_`, which is only advanced at points where no large record is open.

All three discard the file from the large record's first byte onward. A GCMT written inside that
range is discarded with it — and a GCMT is precisely the record that must not be discardable: it is
what makes an already-durable, already-acknowledged atomic group visible on replay. Dropping it
turns "group committed" into "group silently skipped".

Relaxing the loader is not available: to keep the GCMT while dropping a torn large record you would
have to remove bytes from the **middle** of the file, not truncate it. The single-`ftruncate`
recovery model is the format's contract, and the loader's check is the guard that enforces it.
Relaxing it would trade a loud refuse-to-start for a silent loss of an acknowledged commit — which
is worse, not better.

The writer was also already corrupting its own rollback anchor: both group-commit write paths set
`last_good_offset_ = file_offset_` unconditionally (`aof.cc` normal path, and the uring path's
`if (group_commit) last_good_offset_ = file_offset_;`). After an interleaved GCMT the "safe" rollback
point sat **inside** an open large record. With the guard in place those two lines are correct by
construction, so they are left as they are rather than re-tested at the point of use.

## 5. The fix

One guard, at the single choke point, so no future call site can reintroduce it
(`src/persist/aof.cc`, `AofManager::drain_pending_commits`):

```cpp
if (locked_producer_ != UINT32_MAX) {
    if (!pending_commits_.empty()) {
        for (const std::unique_ptr<AofChunk>& held : pending_commits_) {
            if (held->group && group_dependencies_ready(*held->group)) {
                control_defers_.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    }
    return 0;
}
```

The deferral is bounded and cannot deadlock: while the lock is held the writer keeps draining the
locked producer, and that producer is the only one that can close the record —
`AofProducer::record_bytes` always seals the record with `AofFrameLargeEnd` and never waits on the
AOF reply gate mid-record. GCMTs already tolerate arbitrary delay in `pending_commits_` (they wait
there for their dependencies), and `mark_post_written` already absorbs out-of-order post sequences,
so nothing downstream changes.

Cost when the feature is off: `appendonly no` leaves `fd_ < 0` and `writer_pass` returns at its
first line, so this code is never reached. On the AOF path the added cost in the common case is one
predicted branch on `locked_producer_` plus, only while a large record is open, one
`pending_commits_.empty()` test.

**Counter.** `INFO Persistence` gains `aof_control_frames_deferred` — writer passes that had a ready
GCMT in hand and held it back because a large record was open. It is the near-miss counter: it
counts exactly the event that used to write an interleaved frame, so a test can prove the window
was *entered* rather than merely that nothing broke.

## 6. Verification

### Hit rate after (same detector, same cells, more runs than it took to catch it)

| engine | atomic | stop | runs | interleaved |
|---|---|---|---|---|
| normal | 1 | SIGKILL | 60 | 0 |
| normal | 0 | SIGKILL | 30 | 0 |
| normal | 1 | SIGTERM (clean) | 30 | 0 |
| uring | 1 | SIGKILL | 60 | 0 |
| uring | 0 | SIGKILL | 30 | 0 |
| **total** | | | **210** | **0** |

Every clean line carries its shape, e.g.
`frames=453 large_records=85 control_frames=5 interleaved_control=0 other_violations=0` — 85 large
records and 5 control frames per run, so the zero is not vacuous.

### Directed battery: `tests/aof_frame_order.py HOST PORT AOF_DIR`

Three phases, each of which can fail for its own reason:

1. **negative control** — 40 cross-owner script groups with no large record in flight. Asserts
   `aof_groups_committed` advanced (the workload really makes control frames) and that
   `aof_control_frames_deferred` did **not** move. This is the detector proving it can report zero.
2. **window** — eight pipelined connections writing large values against a ninth streaming two-key
   `EVAL` groups, with a key on every shard for both roles. Runs until
   `aof_control_frames_deferred` advances; **fails** if the window is never entered, because a
   clean frame walk would then prove nothing. Bounded by rounds, a 90 s deadline and a 512 MiB
   file budget, whichever comes first.
3. **walk** — every `*.incr.tomo` under the directory: must contain large records **and** control
   frames, and zero interleaves.

Two things had to be tuned by measurement, and both are recorded because they are the difference
between a real row and a decorative one:

* A cross-owner **script** is the group source rather than a cross-shard `MSET`, because `MSET`
  produces a GCMT only under `--atomic 1`. The first draft used `MSET` and its own negative control
  caught it: "negative control wrote no atomic groups (committed 0 -> 0)" at `--atomic 0`.
* The record size is **cycled** over 70000 / 120000 / 260000 / 520000 bytes rather than fixed. How
  long the lock outlives a writer pass depends on the record's frame count against how fast the
  producer posts, and a single size entered the window in only 3 of 4 thread geometries tried
  (a fixed 70000 missed one run at the gate's own 6:2 geometry; a fixed 20 MiB was worse still,
  because a long lock starves the very fragments whose completion arms the pending GCMT).

```
AOF FRAME ORDER PASS: negative-control groups=40 deferrals=0; window rounds=1 groups=83
  deferrals=3; segments=1 bytes=31122249 frames=1016 large_records=128 control_frames=168
  interleaves=0
```

16 of 16 runs on `persist-io normal` with the window entered every time: 6 at the lane geometry
(`--ratio 6:10`, 16 cores), 6 at the gate's own geometry (`--ratio 6:2`, 8 cores), 4 confirmation
runs — evenly split between `--atomic 0` and `--atomic 1`. Under ASAN, 2 of 2 (atomic 0 and 1),
window entered, no sanitizer report.

On `persist-io uring` the battery passes 5 of 6 but reaches the window only after tens of rounds,
writing up to 3.9 GB before it does. That is the second reason the gate row is normal-only.

### Fails before, passes after — same test, same workload

To make the "before" failure substantive rather than "the counter does not exist", a scratch build
was made from this tree with the **guard removed and the counter kept** (`return 0;` deleted, the
`control_defers_` count left in place). Nothing else differs.

```
########## COUNT-ONLY (guard removed, everything else identical) ##########
rep1 normal a1: VIOLATION interleaved_control … at frame #351 offset 13571422 (CTRL seq=0 flags=--) …
rep1 normal a0: VIOLATION interleaved_control … at frame #734 offset 26826856 (CTRL seq=0 flags=--) …
rep2 normal a1: VIOLATION interleaved_control … at frame #363 offset 14332091 …
rep2 normal a0: VIOLATION interleaved_control … at frame #609 offset 27507307 …
rep3 normal a1: VIOLATION interleaved_control … at frame #618 offset 25460995 …
rep3 normal a0: VIOLATION interleaved_control … at frame #449 offset 19327033 …
```

**6 of 6 fail with the guard removed; 16 of 16 pass with it.** The pre-fix binary (no counter at
all) additionally fails phase 1 immediately, and reproduces the physical interleave on this
workload in 2 of 4 runs.

### Gate row

`tests/gate.sh` gains two checks, after the AOF engine loop, on `persist-io normal` under both
atomic modes; `EXPECT_QUICK` 192 → 194 and `EXPECT_FULL` 202 → 204. Verified at the gate's own
thread geometry (`--shards 16 --ratio 6:2` on 8 cores, matching `GATE_CORES=0-7`): 6 of 6, window
entered in 1-5 rounds, ~31-155 MB of AOF per run. `persist-io normal` only: it is the engine the
defect was demonstrated on, and the engine where the window is entered cheaply enough for the row
to prove its mechanism fired. A row that cannot show its gate opened is worse than no row.

### No regression

The gate's whole AOF section replayed on lane resources (port 7500, cores 32-47), both engines:
`aof.py` populate/loadaof/verify/snapshot-byte-exact under both atomic modes, `aof_torn_group.py`
prepare/verify/scan, `aof_fsync.py` always/everysec/no with the truncated-tail recovery,
`aof_rewrite_matrix.sh` and `aof_rewrite_trigger_matrix.sh`: **32 checks, 32 pass, 0 fail.**
`tests/gate.sh` itself was not run — it owns port 7899 and cores 0-7, reserved for the mainline
operator.

`tests/differ.py` was not extended: this is a physical-framing invariant of the append-only file,
not a command surface, and vanilla redis has no comparable frame format to byte-compare against.

## 7. Adjacent findings (reported, not fixed)

* **A clean stop does not save you.** The interleaved frame is written mid-run, so a clean
  `SIGTERM` shutdown closes a file the next boot rejects — 2 of 27 clean-stop runs before the fix.
  The original brief noted no clean-shutdown path had been shown; this one has now been shown.
* **uring was never observed to hit** — 0 of 117 pre-fix runs of the natural workload — although
  the same two unguarded call sites were on its path. Its dependency marking happens on CQE
  (`on_io_complete` → `note_group_fragment`) rather than inline in `drain_producer`, so a GCMT
  rarely becomes ready while the lock is still held; the directed battery does reach the window
  there, but only after tens of rounds. The fix is engine-independent because it sits in the shared
  choke point; the exposure on uring is judged real but was demonstrated only as a near-miss
  (a non-zero `aof_control_frames_deferred`), never as an actual interleaved file.
* **`rewrite_mark` has the same shape of exposure, not demonstrated.** It drains with
  `for (pass…) while (pending_chunks() …)` and then switches files. `pending_chunks()` can read
  zero for an instant while `locked_producer_` is still set (the LargeBegin chunk written, the rest
  not yet posted), which would split a large record across two increments. I could not build a
  reproduction — the rewrite path also quiesces through the snapshot cut machinery, which may
  already prevent it — and per the standing rule I did not add a guard for a defect nobody has
  demonstrated. Worth a directed look with a debug pause at `AofRewriteDebugStage::BeforeMark`.
* **Slow `SIGTERM` under a rewrite-heavy burst, pre-existing.** Two servers did not release the
  listener within 20-60 s of `SIGTERM` while an AOF rewrite was in progress under a 500 KiB-value
  burst; one was the **pre-fix** binary, so it is not caused by this change. Not investigated.

## 8. Files

* `src/persist/aof.cc` — the guard in `AofManager::drain_pending_commits`.
* `src/persist/aof.h` — `control_defers_` + `control_defers()`.
* `src/cmd/t_server.cc` — `aof_control_frames_deferred` in `INFO Persistence`.
* `tests/aof_frames.py` — new. Frame-level `dump` and `check` for the AOF physical stream; `check`
  is the loader's frame walk, usable standalone as the detector for this class of defect.
* `tests/aof_frame_order.py` — new. The directed three-phase battery.
* `tests/gate.sh` — two rows + the ledger bump.
