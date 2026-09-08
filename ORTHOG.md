# Mode orthogonality integration — 2026-09-08

This worktree merges `cx-overlap` and its uncommitted fix round first, then `cx-rl2s`.
All sixteen configurations boot, show the requested feature engagement, and pass the named
correctness batteries. **An inherited violation of the no-retry/no-sequence law remains open**;
the functional matrix does not establish compliance with that architectural constraint.

## Inputs and merge decisions

- Mainline: `c8e61f646` (the actual starting HEAD, after the context's `78c3e5391`).
- First merge: `4938c609c`, parent `0d8044b956f876f1fef7676149493eee81143a78`
  from `cx-overlap`, plus its working-tree changes to `t_server.cc`, `ex_loop.h`,
  `server.h`, `atomicwindow.py`, and `multirace.py`. `GATEFIX.md` was copied verbatim.
- Second merge: `b4013b752`, parent `0c8cbca767b565701d6ff6f8abb7ce03b11e0df6`
  from `cx-rl2s`. Follow-up validation repairs are recorded in this worktree's history.
- The locally modified, actively written `CODEX-OUT.md` was preserved. Incoming execution
  transcripts were not substituted for the local transcript; no feature source was dropped.

| File / seam | Resolution |
| --- | --- |
| `config.h`, parser tests | Keep overlap's finalized numeric knob surface, canonical aliases, reorder grammar, and all sixteen acceptance cases; remove RL2S's split/overlap rejection. No retired knobs were restored. |
| `server.h` | Enable the lane from `read_local` alone. Retain RL2S's optional state, QSBR registration, and overlap's commit-hold test hook. |
| `ex_loop.h` | Keep reorder's array-capacity fix and both execution call sites; keep split owner retirement and the shard-less reader pass. Restrict fused private queues, batch geometry, and handoff-ring sharing to actual 1s mode. |
| `io_loop.h`, `rl2s.cc` | Split IO keeps its ordinary split writeback schedule at both overlap values. Its parser gains the local-read capability, drains captures before QSBR publication, and decodes coded replies in both split writeback arms. All writes/demotions still use ordinary owner inboxes. |
| `genthread.cc`, `main.cc` | Keep both boot runtimes; select RL2S only for armed split boots. Remove notices that claimed a requested feature was inert. |
| `t_server.cc` | Keep the finalized knob/placement INFO fields and append actual per-thread reader activation plus optional schedule witnesses. |
| `bplus.py` | Accept either thread mode and overlap value, retaining the atomic-filter and exact-value witnesses. |
| Other RL2S changes | Retain Makefile wiring, role/QSBR state, read-local admission checks, the standalone RL2S battery, and cache-churn activation checks. |

## Integration fixes

1. Merely removing the RL2S rejection would have selected the **1s** overlap scheduler
   on split IO. `run_split_read_local()` now selects the split schedule with a separate
   compile-time reader capability. Both shallow interleaved writeback and the natural-order
   depth arm consume coded replies; each IO pass drains only local reads, never owner work.
2. A fused-capable split executor must still use the split owner inbox and its own notification
   ring. Those settings now depend on actual architecture, not just the executor template.
3. The fused overlap loop returned before RL2S's common-loop activation publication. It now
   publishes matching entry, park/resume, and exit edges, including `active=1` on actual entry.
4. Knob echoes could not prove reorder or overlap engagement. `src/core/orthog.h` supplies
   physical-thread-owned atomic telemetry: actual overlap schedule/passes/interleaved passes,
   reorder batch calls, eligible multi-client runs, actual permuted runs, and largest observed
   batch. There is no per-operation hook when disabled. The optional 64-byte-per-thread array
   exists only when overlap or reorder is on; the entire witness INFO family is absent when
   both are off. Read-local OFF likewise allocates no lane state and exposes no thread rows.
5. The merged knob surface occupied 544 Config bytes; 80 reserved bytes restore the explicit
   **624-byte** project lock without restoring deleted options. The other requested locks
   remain Op 336, Client 1984, ThreadCtx 1408, Shard 1440, FlatStore 944, Rob<64> 192,
   and AtomicEntry 144. Their checked-in assertions were compiled.
6. `tests/overlap.py` now expects an active split lane and accepts the reorder setting.
   `tests/orthog.py` checks every actual reader, immutable INFO configuration, disabled
   witnesses, owner routing, and real cross-client permutations. Its mixed BITCOUNT/INCR
   traffic uses fresh keys and connections on bounded attempts, with exact replies. A missing
   permutation fails; wrong replies never get retried. A single-client run is intentionally
   insufficient. The reorder unit also checks reported permutations against actual task order.
7. `execiso.py`'s bare reference assumed every MGET used a pinned scatter cut. With the local
   lane it could return a complete newer image after the DEBUG pause and be mislabeled torn.
   This reproduced at 1s/read-local=1/overlap=0 as well as the newly combined split cell;
   ordinary split/read-local=0 passed. WATCH keeps this **bare** reference on its intended
   owner/scatter path. The exact OLD-image and held-window assertions remain unchanged,
   and armed boots must advance the WATCH-fallback counter before the foreign writer starts.
   The historical fanout counter is diagnostic because old-generation debt can retain
   tracking even after atomic is set to zero. Geometry now translates shard IDs into real owners.
8. `session_monotonic.py` could claim to arm the owner read delay while its GET ran locally;
   split/read-local=1/overlap=0/reorder=0 exposed this as zero held cuts on all three attempts.
   One reader now WATCHes the probe to exercise that owner path, while the other uses ordinary
   local-read admission. INFO must witness WATCH fallback on armed boots. Every attempt uses
   fresh keys proven to span two physical owners and fresh connections at a fixed duration;
   the old increasing-duration retries were removed. Seeds happen before concurrent writers
   start, and worker exceptions or empty work fail. Data errors never get retried.
9. Correctness-only battery invocations now avoid embedded rate comparisons: `xmove.py` moves
   its size-ratio section behind `--release-build`, and `atomic_ryow.py --no-rate-assertions`
   keeps the unheld burst's reply checks while omitting its timer and serial comparison arm.
   The existing gate passes xmove's release flag, preserving its coverage without adding rows.

## Method and evidence

Servers are pinned with `taskset -c 8-15`; battery processes use `taskset -c 16-31`.
Every boot uses 16 shards, atomic 1, uring, DEBUG enabled, and key/client LB plus automatic
FLIP disabled for exact attribution. Split boots use `--ratio 6:2`; fused boots use all eight
cores without a ratio. Ports 8200–8215 are assigned by cell. Persistence is disabled and
runtime files stay under this worktree's `build/orthog/`. Tests run sequentially against one
started server at a time; only the exact PIDs started by the local runner are stopped.

Some existing atomic batteries intentionally exercise internal atomic-OFF and live CONFIG
controls, then the runner restores atomic 1. The sixteen **boot configurations** all use
atomic 1. `atomic_ryow.py` uses `--no-rate-assertions`; `atomic_torn.py` runs its correctness/
mechanism arm without release timing assertions. `tests/gate.sh` and standalone benchmarks were
not run. Early selected `xmove.py` batteries did execute an embedded size/timing comparison;
`atomic_ryow.py --no-rate-assertions` also still measured a rate comparison before its behavior
was corrected. This did not comply with the task's no-benchmark instruction. Those timing
values are excluded from the verdict. The comparisons are now conditional on release/rate
mode, and changed correctness-only paths are rechecked separately. Timing-only skips and the existing xmove
atomic-on inter-hop coverage exclusion are identified separately from correctness results.

`tests/orthog.py` is standalone and starts no server. For example, on a separately started
6:2 server at port 8215:

```sh
taskset -c 16-31 python3 tests/orthog.py 127.0.0.1 8215 2s 1 1 1 --output build/orthog/info.json
taskset -c 16-31 python3 tests/overlap.py 127.0.0.1 8215 2s 1 1 1
```

**16/16 boots and engagement checks passed. 276 selected battery invocations passed.**
Every completed server returned zero with all seven `shutdown_report.stuck` fields zero.
No cell was rejected or silently downgraded. These are functional results subject to the
inherited law gap and coverage exclusions below.

The combination order is **thread-mode / read-local / overlap / reorder**. `C` is the
16-battery common set: `orthog, overlap, s6, ryow, multi_exec, edgeproto, resp3, torture, atomic_ryow, atomic_torn, multirace, execiso, session_monotonic, multires, xmove, xscript`. `L` adds `read_local_lane` and `bplus`; `F` adds
`rl2s` with its EX→IO→EX FLIP round trip. All .py paths are under `tests/`.

Reader **48/16** means the INFO lifetime `hits_total=48` (32 GET + 16 MGET) and
`mget_hits_total=16` on **each** active thread, after a clean seed. Fused readers report
role=unified; split readers report role=ifid, shards=0, active=1; initial EX rows report
active=0 and hits_total=0. Overlap numbers are total/interleaved passes. Reorder numbers
are batch calls / eligible multi-client runs / actually permuted runs, read after the
multi-connection witness. Both disabled schedule knobs have no witness INFO family or
allocated witness array; where another knob is enabled, their counters remain zero.

| Mode / RL / O / R | Boots | Read-local INFO evidence | Overlap INFO evidence | Reorder INFO evidence | Batteries / shutdown |
| --- | --- | --- | --- | --- | --- |
| 1s / 0 / 0 / 0 | PASS | `read_local:0`; thread rows absent; hits 0 | `overlap:0`; plain | `reorder:0`; no calls/permutations | C: **16/16 PASS**; clean stop |
| 1s / 0 / 0 / 1 | PASS | `read_local:0`; thread rows absent; hits 0 | `overlap:0`; plain | **813/27/20**, max 24 | C: **16/16 PASS**; clean stop |
| 1s / 0 / 1 / 0 | PASS | `read_local:0`; thread rows absent; hits 0 | `fused-overlap` **7087/1399** | `reorder:0`; no calls/permutations | C: **16/16 PASS**; clean stop |
| 1s / 0 / 1 / 1 | PASS | `read_local:0`; thread rows absent; hits 0 | `fused-overlap` **7336/1417** | **837/19/10**, max 24 | C: **16/16 PASS**; clean stop |
| 1s / 1 / 0 / 0 | PASS | active **8**; each reader **48/16** GET+MGET/MGET hits | `overlap:0`; plain | `reorder:0`; no calls/permutations | C + L: **18/18 PASS**; clean stop |
| 1s / 1 / 0 / 1 | PASS | active **8**; each reader **48/16** GET+MGET/MGET hits | `overlap:0`; plain | **823/19/13**, max 24 | C + L: **18/18 PASS**; clean stop |
| 1s / 1 / 1 / 0 | PASS | active **8**; each reader **48/16** GET+MGET/MGET hits | `fused-overlap` **7427/1393** | `reorder:0`; no calls/permutations | C + L: **18/18 PASS**; clean stop; OFF control unobserved: RENAMENX losing race |
| 1s / 1 / 1 / 1 | PASS | active **8**; each reader **48/16** GET+MGET/MGET hits | `fused-overlap` **7214/1367** | **787/35/29**, max 56 | C + L: **18/18 PASS**; clean stop |
| 2s / 0 / 0 / 0 | PASS | `read_local:0`; thread rows absent; hits 0 | `overlap:0`; plain | `reorder:0`; no calls/permutations | C: **16/16 PASS**; clean stop; OFF control unobserved: RENAMENX losing race |
| 2s / 0 / 0 / 1 | PASS | `read_local:0`; thread rows absent; hits 0 | `overlap:0`; plain | **829/12/7**, max 32 | C: **16/16 PASS**; clean stop; OFF control unobserved: RENAMENX losing race |
| 2s / 0 / 1 / 0 | PASS | `read_local:0`; thread rows absent; hits 0 | `split-io-overlap` **5930/5930** | `reorder:0`; no calls/permutations | C: **16/16 PASS**; clean stop |
| 2s / 0 / 1 / 1 | PASS | `read_local:0`; thread rows absent; hits 0 | `split-io-overlap` **5911/5911** | **826/12/7**, max 16 | C: **16/16 PASS**; clean stop |
| 2s / 1 / 0 / 0 | PASS | active **6**; each reader **48/16** GET+MGET/MGET hits; IFID shards 0; EX hits 0 | `overlap:0`; plain | `reorder:0`; no calls/permutations | C + L + F: **19/19 PASS**; clean stop |
| 2s / 1 / 0 / 1 | PASS | active **6**; each reader **48/16** GET+MGET/MGET hits; IFID shards 0; EX hits 0 | `overlap:0`; plain | **821/17/7**, max 24 | C + L + F: **19/19 PASS**; clean stop; OFF control unobserved: RENAMENX losing race |
| 2s / 1 / 1 / 0 | PASS | active **6**; each reader **48/16** GET+MGET/MGET hits; IFID shards 0; EX hits 0 | `split-io-overlap` **6192/6192** | `reorder:0`; no calls/permutations | C + L + F: **19/19 PASS**; clean stop; OFF control unobserved: RENAMENX losing race |
| 2s / 1 / 1 / 1 | PASS | active **6**; each reader **48/16** GET+MGET/MGET hits; IFID shards 0; EX hits 0 | `split-io-overlap` **6248/6248** | **829/12/2**, max 16 | C + L + F: **19/19 PASS**; clean stop; OFF control unobserved: RENAMENX losing race |

All rows use the shared integration fixes above. Armed split rows additionally rely on
the split IO schedule/coded-writeback fix; armed fused overlap rows rely on the corrected
loop activation publication. The common execiso reference repair applies to every armed row.

Raw INFO snapshots, exact boot/battery commands, return codes, individual logs, and shutdown
reports are in `build/orthog/verified-matrix/<cell>/` for the first twelve cells and
`build/orthog/completed-matrix/<cell>/` for the four armed split cells (local build artifacts).
The failed 2s100 session witness remains in verified-matrix; it is superseded by the complete
completed-matrix run after the test repair, not counted as a pass. Earlier failures
and discovery runs remain in sibling `scout*`, `correctness-scout*`, `execiso-before`, `matrix`,
`remaining-scout`, and `final-matrix` directories, rather than being overwritten by a retry.

Coverage exclusions: each row intentionally omits atomic_ryow's speed assertion and
atomic_torn's release-only promotion-time bound. xmove's pre-existing atomic-ON concurrent
inter-hop check is excluded by that battery itself; its data/reply tests did run. Early
xmove size timings are excluded from this report's verdict; later rows skip that timing
section under the new release-build split. The table also identifies every atomic-OFF
negative control whose losing race was not observed and was skipped by the inherited battery.
These exclusions are not mechanism passes.


## Additional validation

After the battery repairs, **33/33 targeted reruns** passed against the same production binary:
`session_monotonic` on the first twelve cells, `atomic_ryow --no-rate-assertions` on the fifteen
cells that used its earlier implementation, and correctness-only `xmove` on the first six
cells. The other cells already used the final battery version. All fifteen refresh servers
exited zero, with every shutdown stuck field zero. Logs and commands are in
`build/orthog/test-refresh/<cell>/`; these reruns supplement the 276 matrix invocations above.
Their atomic RYOW logs contain no rate comparison, and xmove logs explicitly omit size timing.

Production build: `make -j8 all unit` passed under taskset on cores 8–31, including the config
parser, flip controller, read-local ring, write ring, and reorder units. The matrix binary's
SHA-256 is `e6124da424b4b27ded8b8cb169f697bcaff597dfbc44ff233f280736d5fdf68c`;
the build log is `build/orthog/build-final.log`. Python syntax, shell syntax-only inspection,
and byte-for-byte comparison of the two EXPECT assignment lines also passed.

The standalone reorder unit passed **ASAN + UBSAN**, with **175 exact permutations** and
maximum batch **128**. A throwaway header copy under `build/orthog/` returns immediately from
`ex_schedule_batch()`; the otherwise identical sanitized unit exited 1 with
`actual permutation differs from exact oracle`, as required. No mutation entered production
source or a server binary. Commands and results are in `build/orthog/unit-results.json`.
An initial local harness compile used `-I src/core`, which shadowed the system signal header;
using `-iquote src/core` corrected the harness include search, with no source change.

A standalone layout probe also reported exactly:

```text
Op=336 Client=1984 ThreadCtx=1408 Shard=1440 FlatStore=944 Rob64=192 AtomicEntry=144 Config=624
```

The ownership-assertion build (`TOMO_RL_CACHE_DEBUG`, production allocator/settings otherwise)
also passed `rlcache_churn.py HOST PORT 30 8` in **all eight armed cells**, each with
**5 checks and zero skips**. It uses 64 shards so the automatic balancer has movable work,
key/client LB=1, automatic FLIP=0, and the same 8–15 server / 16–31 client pinning. This is
additional migration geometry; the main matrix remains at 16 shards and fixed placement.
The test now also fails if any worker remains alive or completes no operations.

| Cell (mode/RL/O/R) | Local-read hit delta | Peak cached bytes | Shard-move delta | Assertions / shutdown |
| --- | ---: | ---: | ---: | --- |
| 1s100 | 4,809,472 | 2,736 | 31 | 5/5 PASS; clean stop |
| 1s101 | 5,009,575 | 2,912 | 41 | 5/5 PASS; clean stop |
| 1s110 | 5,141,188 | 2,816 | 30 | 5/5 PASS; clean stop |
| 1s111 | 4,626,156 | 2,736 | 29 | 5/5 PASS; clean stop |
| 2s100 | 4,755,565 | 944 | 7 | 5/5 PASS; clean stop |
| 2s101 | 5,050,770 | 880 | 5 | 5/5 PASS; clean stop |
| 2s110 | 4,575,712 | 1,024 | 5 | 5/5 PASS; clean stop |
| 2s111 | 5,064,358 | 608 | 11 | 5/5 PASS; clean stop |

All eight servers exited zero with every stuck field zero. The assertions check cache/ring
ownership, unique cached-block residency and class-list consistency, and each shard's retire
sink against its current owner. Logs, exact commands, and final INFO are in
`build/orthog/ownership/<cell>/`; the debug build log is `build/orthog/debug-build.log`.
The debug build uses a local object-file make fragment including the repository Makefile and
preserves the string translation unit's existing inlining parameter.

The imported `knobs.py HOST PORT uring 1` battery also passed at its required 2s/0/0/0,
16-shard, 6:2 geometry with both load balancers enabled. It checks retired-name rejection,
immutable knobs, live CONFIG fan-out to every shard, encoding aliases/limits and INFO placement.
Its server exited zero with all shutdown stuck fields zero; evidence is in
`build/orthog/knobs-surface/2s000/`.


## Required EXPECT values

The starting tree has EXPECT_QUICK=325 and EXPECT_FULL=342. These constants were **not edited**.
The overlap merge contributes two executable rows:

| Added row | Emitting line in merged `tests/gate.sh` | Relative to quick exit at line 1266 | Quick / full delta |
| --- | ---: | --- | --- |
| reorder mechanism + 32/128-task geometry battery | 329 | Before | +1 / +1 |
| restored cross-shard dispatch scaling | 696 | Before | +1 / +1 |

RL2S, orthog, and the changed battery internals add no gate rows. Thus the maintainer should
set **EXPECT_QUICK=327**, **EXPECT_FULL=344**, before the optional NIC increment.

## Inherited law gap and limits

The supplied rule forbids reader retries and per-operation seqlocks. Both inputs already
violate that literal rule: `prepare_local_mget()` and `prepare_captured_local_mget()` permit
two command attempts and increment `mget_generation_retries`; the uncaptured GET path has
three attempts; point probes and MGET windows validate before/after sequence or epoch words.
The default captured GET does not re-probe, but still performs sequence validation.
Concrete sites are `src/core/ex_loop.h:1187` (uncaptured MGET), `:1316` (captured MGET),
`:1467` (GET), and `src/store/flatstore.h:3337` (the explicitly named point-probe seqlock
comparison). Both input commit objects retain these retry constants and the same comparison.
These paths are reachable with local reads armed in either mode. The integration does not
remove safety validation or claim that passing batteries erase this architecture mismatch.
A strict no-retry/no-sequence implementation requires a reader-safe published index/topology
and versioned multi-key snapshot lookup through the existing MVCC cut, including same-owner
transaction/script publication and structural mutations. Disabling validation alone risks
missing keys or torn reads; disabling the lane would violate the requested feature engagement.
This is an inherited algorithm issue, not a schedule combination that cannot be instantiated.

Research selectors `TOMO_READ_LOCAL_SET_TAX_VARIANT=1/3` also deliberately use in-place
sequence-protected overwrites. They were not built or used here; the production selector is 0.
No claim is made that those research variants satisfy the supplied laws.

The input `GATEFIX.md` does **not** establish that all five reported failures were pre-existing:
it explicitly leaves the original borrow-registry failure unreproduced and unresolved. That
qualification is preserved. This task supplies correctness/engagement evidence, not a performance
PRE/POST table; the full gate and performance verdict remain with the maintainer.
