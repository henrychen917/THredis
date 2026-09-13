# Opt-in epoch-MVCC atomics

`--atomic 1` makes the write groups for `MSET`, `MSETNX`, multi-key `DEL`, `UNLINK`, the STORE
family, and the cross-shard mover family atomic. `LMPOP` and `ZMPOP` also revalidate their selected
owner before mutating it. `MGET`, `EXISTS`, `TOUCH`, `KEYS`, and exact `DBSIZE` resolve one committed
cut while tracking is active. The default is `--atomic 0`; a shard with no pending group entry
follows the original store path, allocates nothing, and pays one predicted branch.
`--atomic-window` defaults to 256 admitted atomic write groups (zero is unlimited). Both knobs are
numeric and live through `CONFIG SET/GET`.

## ATOMICS V3: group-scoped pending entries

V3 deletes the V2 per-key record table and candidate-node chains. One atomic owner task now creates
one pooled `AtomicEntry` for its shard span in `ScatterState::key_order`. The entry retains the group
decision pointers, the span coordinates, one 64-bit hash-membership mask, and a trailing parallel
array of displaced `KvObj*` predecessors. Key bytes are not copied: installed values supply stable
keys, while DEL/UNLINK use arena-owned empty-string key anchors because their physical install is a
tombstone. The steady write loop is therefore one physical replacement/erase and one parked-pointer
store per key; group linking, decision references, and allocation are paid once per shard task.

Each shard has a normally empty intrusive pending-entry list and a 64-bucket intrusive connection
index used only for exact same-connection undecided hazards. With an empty list, point lookup and the
ordinary write handlers are unchanged. With entries present, the resolver rejects most entries by
the membership bit, checks the referenced span for exact key equality, and chooses the largest
committed epoch not newer than the read cut. The oldest matching parked pointer is the base; each
later parked pointer is the candidate immediately before that install, and the physical object is
the newest candidate. Epoch-zero, abandoned, and too-new installs are skipped. This computation is
order-independent for overlapping groups. Expired parked predecessors resolve absent.

An ordinary write touching no pending entry stays on its original path. One that does touch an entry
deep-clones the resolved value and installs a pooled one-key plain pseudo-entry with its own ticket,
so existing in-place collection handlers cannot mutate a protected predecessor. Same-owner
multi-key commands retain localfast and prepare every touched pseudo-entry before drawing tickets.

Cleanup removes the contiguous decided prefix strictly below the global read floor and no newer
than its pre-floor commit cutoff. Monotonic install/ticket order is proved in one linear pass and
then cleanup only frees parked predecessors and unlinks entries. If tickets invert, membership and
exact-key checks decide whether the same direct collapse is safe; an actual overlap uses a reusable
transient argmax table. An abandoned prefix reinstalls its predecessor, or splices it into the first
surviving occurrence of that key. This is the rollback-free abort. Output-materialized reads release
their epoch floor before ordered socket retirement; zero-copy string results retain their existing
store borrow instead. While atomic work is live, each IO admits at most eight unresolved snapshot
groups so a deep MGET pipeline cannot pin thousands of entries and turn list resolution quadratic.

No command path retains the V2 side map. The per-shard value and entry freelists remain because they
remove steady-state allocator traffic. `atomic_records_freed` is kept as a compatibility counter but
now counts retired group/plain entries; `atomic_entries` is the explicit cumulative entry gauge and
`atomic_pending_entries` is the current live gauge. `atomic_cleanup_fast/slow` expose the monotonic
and inversion/abort cleanup work.

### V3 instruction and throughput audit

The owner reference that motivated this lane was 504 cycles, 607 instructions, IPC 1.20, and 7.0
cache misses per written key at 1.64M commands/s OFF, versus V2's 920 cycles, 1,979 instructions,
IPC 2.15, and 15.8 misses at 859k commands/s ON. Thus V2 added 1,372 instructions/key, while the
10% goal allowed about 60.

The final V3 checkout was measured on 2026-08-25 with the same 8-core 6:2 placement: server CPUs
`240-243,248-251`, executor counters on `243,251`, and memtier on `244-247,252-255`. Every cell used
8 threads, 8 clients/thread, pipeline 32, 32-byte data, random command keys, and 30 seconds.

| Pure `MSET-8`, 100k keys | Commands/s | cycles/key | instr/key | IPC | misses/key |
| --- | ---: | ---: | ---: | ---: | ---: |
| V3 checkout, atomic OFF | 889,479 | 871 | 608 | 0.70 | 7.66 |
| V3 checkout, atomic ON | 431,588 | 1,798 | 1,594 | 0.89 | 14.27 |

V3 removes 385 instructions/key (19.5%) from the supplied V2 ON count, but the local ON tax is still
986 instructions/key and throughput is 48.5% of OFF (`-51.5%`). Therefore the requested 10-15%
pure-write target is **not met**. Sampling attributes the remaining executor instruction volume
primarily to physical exchange, entry-prefix collapse, owner-task execution/completion, and
same-connection hazard checks. Group scoping alone did not make cleanup and publication amortization
cheap enough for the roughly 60-instruction allowance.

| Final 30-second cell, 100k keys | OFF commands/s | ON commands/s | ON/OFF |
| --- | ---: | ---: | ---: |
| 1:1 `MSET-8:MGET-8` | 943,770 | 461,369 | 48.9% |
| populated pure `MGET-8` | 1,586,424 | 1,393,772 | 87.9% |
| single-key 1:1 `SET:GET` | 7,265,204 | 7,653,455 | 105.3% |
| populated `MSETNX-8` | - | 580,888 | - |

The mixed 20% target is also not met. Point reads and single-key writes do not create pending
entries; single-key parity holds, while the cross-shard pure-read snapshot plumbing costs 12.1% in
this run. A separate ON `MSET-8` 30-second sustained run with key maximum 100M completed at 418,484
commands/s and drained to `atomic_pending_entries=0` (observed `DBSIZE=2,933,667` under memtier's
shared random streams). The 100k final ON run likewise drained to zero and fired
`atomic_entries=83,545,907`, `atomic_promotions=103,593,240`, and `atomic_chain_max=359`; predecessor
reads are correctly zero in blind pure-write traffic.

### V3 validation record

The final quick gate reports `15 ok, 0 FAIL`; the release and ASAN builds enforce
`sizeof(Op)==336` and `sizeof(Client)==1984`. The atomic batteries also pass under ASAN with no
sanitizer report. Their non-vacuous observations include an OFF torn control, zero ON tears,
overlapping writers with an exact final group, DEL-vs-MSET all-or-nothing, abandoned MSETNX
invisibility, pipelined atomic/plain RYOW, 63 admission-window stalls, and connection churn ending at
`inflight=0,pending=0`. One ASAN run moved predecessor reads by 5,456, promotions by 173,456, and
retired 5,784 churn entries. Redis 8.9 differential runs for `string` (4,033 ops), `xshard` (4,276),
and `cgaps` (3,310) each produced zero diffs at both `--atomic 0` and `--atomic 1`.

The command templates remain `MSET __key__:0 __data__ ... __key__:7 __data__` and
`MGET __key__:0 ... __key__:7`, passed via `--command-key-pattern=R`. The sustained arms use
`--test-time 30 --key-minimum 1` with `--key-maximum 100000` and `100000000`, respectively.

## ATOMICS V4 write-path rework

V4 keeps the shard-owner contract literal: an IO thread touches only its connection, Op/argv,
ScatterState, admission lease, and fresh unlinked allocations. Every store probe, exchange,
pending-entry link, resolution, promotion, and drain-list mutation remains on the shard's owning
executor. The signed footprints remain `sizeof(Op)==336` and `sizeof(Client)==1984`; all atomic
owner write passes still call `atomic_prepare_capacity` before their first physical install.

### Lever accounting

1. **Atomic same-owner localfast.** A same-shard atomic write lowers to one ordinary owner task. With
   no pending entry on that shard it executes the original handler directly. With a recorded key it
   prepares every touched clone and entry, performs one capacity/admission preflight, draws one
   ticket, installs the complete local group, and then executes the handler. It creates no group
   pending entry and parks only behind an exact same-connection predecessor. `INFO STATS` exposes
   `atomic_localfast`; the new non-vacuous torn arm discovers a distinct-key same-shard MSET-2 pair
   under the randomized hash seed and requires both counter movement and zero tears.
2. **Atomic fan-out folding.** IO constructs the participant map once and publishes all shard tasks
   for one destination executor with one SPSC tail store; the existing parse-pass wake fold provides
   one notification per destination. Atomic completion decrements the shared group count once per
   participating executor, then preserves the single final commit ticket and release publication.
   Cleanup retirement posts no per-shard tasks. Non-atomic reads retain the compact V3 posting arm:
   using the write bundle arrays for MGET was measured and removed as read-only overhead.
3. **IO-side value prebuild.** Atomic MSET/MSETNX strings above the 192-byte embedded cutover are
   allocated and copied on the owning IO, carried unlinked in KeyRef, and transferred only when the
   shard owner installs them. Admission stays owner-side. Error and MSETNX-abandon paths free the
   never-linked object from IO retirement. Prebuilding every value was also built, but regressed the
   default MSET-8 cell by about 18% because it replaced the owner's warm value pool with generic
   allocation; V4 therefore moves only the allocation/copy work that was already heap-backed.
4. **Residual diet.** Membership bits are accumulated while the atomic participant map is built;
   each pending entry is linked once after its complete owner install; aborted state is hoisted out
   of the key loop; parked/installed byte accounting is folded to one gauge update per task. The
   same-connection hazard is captured by the connection-owning IO in an immutable Op bit before
   publish, so ordinary executor tasks never pull Client bookkeeping across cores. Per-shard reads
   branch once on `atomic_has_records`; MGET specializes the complete owner loop so the no-pending
   arm contains no per-key selector.
5. **Per-owner completion.** Owner-local remaining counts turn the former 6.45 shared decrements per
   random MSET-8 into at most the number of executors (two in the measured 6:2 placement), without
   changing last-completer decision or publication ordering.
6. **IO admission leases.** Each IO borrows up to eight window credits and accounts its active groups
   locally. An odd/even generation and borrow/return handshake makes CONFIG reconfiguration exact;
   shrink debt is repaid by retiring old-generation groups, idle IOs return unused batches, and a
   flip rebuilds the pool from published active counts. The live test drives window 31 -> 7 -> 19 ->
   3 under eight writers, checks the observed bound after shrink, and requires `pool=3,debt=0` after
   drain. INFO exposes `atomic_credit_pool` and `atomic_credit_debt`.
7. **Direct participant lookup.** Atomic writes have one direct shard-to-group map used by dispatch,
   install, hazard, and completion. A stronger prototype replaced all of an executor's shard tasks
   with one executable bundle and reduced task/shared-decrement count to two per group, but lost
   3-7% from the larger execution/fairness unit; it was removed. V4 retains batched queue publication
   plus per-owner completion, the positive part of that probe.
8. **Contention index probe.** The adaptive pending-key index was implemented with owner-local build,
   resolve, and teardown thresholds. Even inactive maintenance was measurable in pure write, while
   active rebuild churn produced far more builds than useful resolves in the tested spans. Because
   the binding requirement is zero cost in 9:1 and pure-write cells, the index was removed rather
   than shipped always-on or with an unearned threshold.
9. **Snapshot floor/cutoff fold.** Executor cleanup samples one cutoff/floor pair per owner cycle and
   reuses it for every owned shard. IO snapshot registration maintains an exact cached minimum plus
   reference count and O(1) state slots; completed reads behind the ROB are removed from the unresolved
   set, unchanged floors are not republished, and only IO floor publishers are scanned by cleanup.

Entry representation, mainline reclaim cadence, and the existing pools are otherwise unchanged, as
required by the audit's KEEP-AS-IS results.

### V4 measurements

The final A/B was run on 2026-08-25 with server CPUs `240-243,248-251`, memtier CPUs
`244-247,252-255`, 16 shards, ratio 6:2, t8/c8, pipeline 32, a 100k key range, random command keys,
default 32-byte data, and 15 seconds per cell. V3 is the untouched b00458f1c split binary; V4 is this
checkout. The populated MGET arm used the same deterministic 100k MSET pass before each binary.
GSMM assigns equal ratios to GET, SET, MGET-8, and MSET-8.

| 15-second atomic-ON cell | V3 commands/s | V4 commands/s | V4/V3 |
| --- | ---: | ---: | ---: |
| populated MGET-8 | 1,390,973 | 1,423,925 | 102.4% |
| 9:1 MGET-8:MSET-8 | 685,224 | 737,730 | 107.7% |
| 1:1 MGET-8:MSET-8 | 448,345 | 453,950 | 101.3% |
| pure MSET-8 | 420,392 | 435,835 | 103.7% |
| equal GET/SET/MGET-8/MSET-8 (GSMM) | 753,556 | 772,530 | 102.5% |

The host was under different background load than the supplied verdict capture, so absolute rates
are not compared across those runs. Applying the same-run V4/V3 ratios to the supplied V3 anchors
gives 1.59M at 9:1, 867k at 1:1, 754k pure MSET-8, and 1.31M GSMM; the three write-heavy projections
remain above the supplied fork-ON floors (1.07M, 618k, and 382k respectively). The owner verdict run
remains authoritative for absolute acceptance.

| Atomic-ON pure write arm | 16 shards commands/s | 8 shards commands/s |
| --- | ---: | ---: |
| MSET-2 | 1,162,776 | 1,208,473 |
| MSET-4 | 755,552 | 770,722 |
| MSET-8 | 466,287 | 498,299 |

The 16-shard MSET-2/MSET-4 pass moved `atomic_localfast` by 1,103,073; the 8-shard MSET-2/4/8 pass
moved it by 2,288,581. Thus the small-N/shard-count measurements exercise the direct arm rather than
inferring it from total throughput.

Executor-only `perf stat` on CPUs 243 and 251, collected concurrently with a 15-second MSET-8 arm,
shows the following. V4's primary gain in this capture is lower cycles and cache misses rather than
lower retired instruction count; the instruction diet did not yet close the remaining architectural
gap to OFF.

| MSET-8 executor metric/key | V3 | V4 | Change |
| --- | ---: | ---: | ---: |
| instructions | 1,584.8 | 1,591.7 | +0.4% |
| cycles | 1,843.8 | 1,667.2 | -9.6% |
| cache misses | 15.13 | 13.74 | -9.2% |
| commands/s in the perf arm | 421,870 | 466,287 | +10.5% |

### V4 validation record

The full gate completed `27 ok, 0 FAIL` across release/ASAN correctness and both NIC regression
cells. After the final floor and duplicate-MSETNX fixes, atomic torn and RYOW passed again in release
and under ASAN/UBSAN with no sanitizer or leak report. Non-vacuous observations include OFF tears,
zero ON tears, same-owner localfast movement, predecessor resolution and promotion, overlapping and
abandoned writers, duplicate-key MSETNX last-value semantics, DEL-vs-MSET all-or-nothing, pipelined
atomic/plain RYOW, live window shrink/grow/flip, and final `inflight=0,pending=0,pool=window,debt=0`.

Zero-copy passed at atomic 0 and 1 in release and ASAN, with the send counter firing. The flush
capture battery completed run plus restart/load verification on a 95MB dump containing 100,000
950-byte values. Redis 8.9.241 differential runs produced zero diffs for string (4,033 ops), xshard
(4,276), and cgaps (3,310) at both atomic 0 and 1. The drain-list shutdown checks remained clean.

## ATOMICS BROADENING

This lane changes only atomic wiring around the existing cross-shard computations. The MVCC store
and publication engine is unchanged. Before implementation, the Redis fork was audited at
`origin/2s-atomic-broaden` (`d85101b89`), including the broadening sequence `ab6c552d4`,
`799782f9a`, and `d85101b89`. Command history was read from the original RENAME/RENAMENX/COPY/SMOVE,
set-store, BITOP/PFMERGE, LMOVE/RPOPLPUSH, LMPOP/ZMPOP, ZRANGESTORE, and SORT STORE ports
(`4c478f7a9`, `8db12728a`, `e32a666cc`, `2f5b50324`, `41a5bc479`, `483299c3f`, `5d5fdf2aa`, and
`cc6acde5f`). The later SORT top-k and coalesced/reduced set-op work (`95b4aa476`, `7de0c1dff`,
`ffab83154`, and `59dcc51f9`) and the correctness repairs (`0c52842bc`, `1c8a7c660`) were checked as
well. No compute path was replaced or reverted.

### Publication mechanisms

1. **STORE family.** `BITOP`, `PFMERGE`, `ZRANGESTORE`, `SORT STORE`, `SINTERSTORE`, `SUNIONSTORE`,
   and `SDIFFSTORE` keep their existing one-cut source gather and compute. Hop 2 now installs the
   single destination through one pending group entry. An empty result installs the same null
   candidate/tombstone used by atomic DEL. Capacity and maxmemory admission complete before the
   first physical exchange.
2. **Movers.** `RENAMENX`, `COPY`, `SMOVE`, `LMOVE`, and `RPOPLPUSH` materialize every mutated image,
   preflight each owner pass, park both predecessors, and release-publish one shared ticket.
   RENAMENX and COPY-without-REPLACE decide destination existence on the destination owner just
   before install; a competing candidate is treated as a reservation and deferred, while a failed
   condition release-abandons every already-installed candidate. RENAME has a specialized one-pass
   composition: the source owner reads the current typed object, prepares the destination image,
   installs its source tombstone, and release-signals the destination owner. A destination task
   waiting for that image enters the existing owner-local deferred queue, so neither executor idles
   and source-before-destination ordering is preserved when both shards share an executor.
3. **Pops.** LMPOP/ZMPOP hop 2 performs the actual pop against the owner's latest value. If the
   selected container became empty after the probe, the group discards the stale decision, takes a
   fresh cut, and probes the original argument order again. It performs at most eight fresh-cut
   reselections before the terminal nil arm; owner work continues while retries are routed.

Same-owner commands retain the original localfast handlers, and `--atomic 0` retains the existing
scatter/two-hop paths. Every broadened write pass preserves the command's DenyOom gate and also uses
owner-side capacity plus maxmemory admission before installing a candidate. The signed
`sizeof(Op)==336` and `sizeof(Client)==1984` locks are unchanged.

### Concurrent and compatibility validation

`tests/atomic_torn.py` now has four non-serial broadening arms. A rotating cross-owner RENAME is
sampled through MGET and requires exactly one live image; SINTERSTORE races an atomically moving set
member and rejects any destination impossible at one source cut; 16 LMPOP racers account for all
2,048 list elements exactly once; and two-contender RENAMENX/COPY races prove the losing installed
candidate remains invisible. The RENAME, store, and conditional controls first require the
corresponding anomaly with atomic mode off, so the ON assertions cannot pass behind a closed gate.
Sets cannot be observed through GET/MGET without WRONGTYPE, so the store arm uses SMEMBERS plus the
type-correct SISMEMBER source observations.

The final release run passed atomic torn, atomic RYOW, torture, and plain RYOW at both knob values.
The broadened torn and RYOW batteries also passed under ASAN/UBSAN with no sanitizer or leak report.
The post-review repository quick gate completed `17 ok, 0 FAIL`.
Zero-copy passed at atomic 0 and 1 with `zc_sends=40`, `zc_bytes=83886080`, and `zc_releases=40` in
each run. The flush-capture restart/load protocol passed at both knob values: each run captured a
95MB image containing 100,000 950-byte values, observed the live database empty after FLUSH, then
restarted with `--load` and recovered exactly 100,000 keys with zero bad samples.

All nine Redis 8.6.2 differential suites produced zero diffs at atomic 0 (seed 11) and atomic 1
(seed 7): string 4,033, list 3,521, set 3,524, zset 3,531, hash 3,545, xshard 4,276, bitmap 4,262,
HLL 3,049/3,057, and command gaps 3,310 operations. A separate typed RENAME arm moved 64
list/set/hash/zset values with their TTLs intact and every source absent. Shutdown drain invariants
were clean in release and sanitizer runs.

### 15-second fork-floor cells

The C++ OFF/ON cells used 16 shards, an 8-core 6:2 placement on server CPUs
`240-243,248-251`, and load CPUs `244-247,252-255`. The driver used 64 connections, pipeline 32,
1,024 per-connection rotating key pairs, and 32-byte string values. LMPOP throughput counts only
LMPOP completions; each target has one companion RPUSH so the selected list remains live. The
reference is the requested `origin/2s-atomic-broaden` tip `d85101b89` with `tomokv-atomic yes`, using
the supplied `/tmp/claude-1000/f8.conf` pattern verbatim (eight cores, AUTO 4 IO / 4 executor) on the
same host. `tests/broaden_bench.cc` is the reproducible cell driver.

| p32 target command | C++ OFF/s | C++ ON/s | ON/OFF | fork ON/s | C++ ON/fork |
| --- | ---: | ---: | ---: | ---: | ---: |
| BITOP-2 | 662,359 | 505,722 | 76.4% | 291,162 | 173.7% |
| RENAME hammer | 701,312 | 622,438 | 88.8% | 576,926 | 107.9% |
| SINTERSTORE-2 | 526,219 | 412,855 | 78.5% | 239,548 | 172.3% |
| LMPOP-2 | 676,983 | 647,194 | 95.6% | 291,036 | 222.4% |

Every target completed with zero protocol or command errors. All four C++ ON cells clear their own
fork-ON floor; the narrowest margin is RENAME at +7.9%. These are local acceptance measurements;
the owner verdict run remains authoritative.

## ATOMICS V2 performance rework (historical)

The measured 8-core loopback decomposition that motivated V2 was:

| Cell (8 keys, t8/c8, pipeline 32, 100k keyspace) | Commands/s | Cost exposed |
| --- | ---: | --- |
| atomic OFF `MSET-8` | 1.67M | single-hop scatter ceiling |
| atomic OFF `MSETNX-8` | 637k | two hops plus connection parse barrier, no MVCC |
| atomic ON V1 `MSET-8` | 198k | two hops, barrier, and V1 MVCC allocation/install |

V2 changes the write path in four related places:

1. **Single-hop abandon-as-invisibility.** Each touched owner performs capacity/admission, record
   and value preparation, and the invisible version install in one task. A refusal release-marks the
   group abandoned. The last completer replies with the selected error and publishes nothing, or
   draws `T = ++commit_seq` and release-publishes the group epoch. `MSETNX` aggregates existence and
   replies 0 without publishing; `DEL`/`UNLINK` install tombstones and aggregate existed counts.
   This removes the second task round and the inter-hop barrier; rollback is unnecessary because an
   epoch-zero candidate is unreachable to every resolver and is reclaimed by owner cleanup.
2. **No atomic connection parse barrier.** Atomic writes pipeline freely. The owner parks only a
   task that touches the same connection's own undecided group node on the same key. Foreign nodes
   are skipped, cross-key operations proceed, and a separate resource-backpressure bit leaves an
   unconsumed frame parked only while the admission window is full. Whole-shard readers treat every
   own undecided node on that shard as a touched-key hazard. This recovers the concurrency hidden by
   the 1.67M-to-637k structural drop without importing an arrival-order frontier.
3. **MVCC allocation diet.** The side map is an owner-only open-address table keyed by hash with
   exact pooled-key comparison. Records have a 24-byte inline key, pooled long-key tails, an embedded
   first candidate, and a compact base predecessor. Additional nodes, records, and raw/int string
   values use per-shard intrusive freelists with arithmetic `good_size` classes. After warmup the
   common atomic string-write path performs no heap allocation. Group lifetime references are
   aggregated once per owner rather than contending once per key, and owner sweeps are batched on
   normal execution passes plus a low-frequency idle sweep instead of posting a cleanup task per
   group.
4. **Localfast under tracking.** Same-owner multi-key commands retain their original handler when
   touched keys have no records (one owner-only side-map probe per key). Reads with records resolve
   inline. Writes with records clone only those recorded keys into direct plain versions and then
   run the original handler against the clones; they never wait for an unrelated global read floor.

The structural wins are intentionally separable: point 1 removes an owner round, point 2 removes
connection-wide serialization, point 3 removes steady-state allocator/hash/reference traffic, and
point 4 restores the original same-owner fast path. The final combined result on the same 8-core
loopback shape was:

| Final V2 cell (30 seconds) | Commands/s | Result |
| --- | ---: | --- |
| atomic OFF `MSET-8` | 1.342M | local same-run ceiling |
| atomic ON `MSET-8` | 827k | 61.6% of local OFF; 4.18x the 198k V1 cell |
| atomic ON populated `MSETNX-8` | 1.218M | exceeds the 500k target |
| atomic ON 1:1 `MSET-8:MGET-8` | 907k combined | exceeds the 500k mixed target |
| atomic ON single-key 1:1 `SET:GET` | 6.534M combined | parity with OFF 6.477M (+0.9%) |

This host's final OFF cell was 19.6% below the 1.67M decomposition reference; the ON/OFF ratio is
above the 60% target and normalizes to 1.029M at that reference ceiling, although the locally
observed absolute ON cell was 827k. The individual point gains were not claimed from synthetic
ablations: points 1 and 2 remove the task round and parse barrier by construction (the overlap arm
measured a 2.18x pipelined/serial ratio), point 3 is what makes the resulting steady-state path
pool-only, and point 4 is directly evidenced by the single-key ON/OFF parity cell.

All cells used `memtier_benchmark -s 127.0.0.1 -p 7899 -t 8 -c 8 --pipeline 32
--test-time 30 --key-minimum 1 --key-maximum 100000` with default 32-byte data. The eight-key
templates were `MSET[ NX] __key__:0 __data__ ... __key__:7 __data__` and
`MGET __key__:0 ... __key__:7`, each with random command keys; the mixed cell assigned ratio 1 to
each template. The populated `MSETNX` cell followed an `MSET` population pass over the same range.
The single-key cells used the built-in `--ratio 1:1 --key-pattern=R:R` shape.

## V2 representation and publication (superseded)

`KvObj` and the command/connection ABI sizes remain unchanged. A shard lazily creates its MVCC map
on first tracked install. Each record owns a stable pooled key, a compact base value, and an
unsorted candidate chain. A candidate contains a `KvObj*` (null is a tombstone), its origin
connection ID, and either a group epoch/abandon pointer or a direct plain-write epoch.

Owners may install different groups in different physical orders. A present-to-present atomic
replacement may occupy the existing table slot while still invisible because all point reads
resolve through the record and cardinality is unchanged. Inserts and tombstones remain side-only
until promotion. Publication alone changes visibility; there is no publish frontier, group lock,
cross-client ordering wait, rollback, or physical-simultaneity requirement.

## V2 visibility and same-connection order (superseded)

A tracked read selects the candidate with the largest nonzero committed epoch at or below its cut;
the base predecessor is eligible at epoch zero. Foreign undecided candidates are ignored. A
same-connection candidate that commits after a pipelined reader captured its cut is overlaid by
origin ID after the owner hazard has waited for the decision, providing RYOW without moving the
registered cut. Resolution and promotion always compute an epoch argmax and never infer order from
chain or physical-link order.

The deferred queue is owner-local and separate from normal work. When an op finds its own undecided
node on a touched key it is requeued; the owner continues normal inbox work. Later same-connection
tasks sharing that key inherit the earlier deferred dependency. Cross-key work on the same
connection and all foreign-connection work continue immediately.

Ordinary writes that encounter records create direct committed versions before invoking their
existing handlers. This preserves old snapshots while retaining in-place collection mutation.
Same-owner multi-key handlers reserve all required direct-version context before drawing tickets,
so activation cannot fail halfway through publication.

## V2 lifetime and promotion (superseded)

Each IO thread publishes the minimum **exclusive** read floor of its active scatter groups: for an
inclusive cut `S`, it publishes `S + 1` (or `UINT64_MAX` when inactive). The server floor is the
minimum publication. The freeing invariant is:

> A version is freed only after every version in its key record is decided and has an epoch
> strictly below the global floor. Consequently the promoted argmax is valid for every active and
> future reader, and no active reader can dereference a retired predecessor.

Publishing the successor lets the winner at the oldest inclusive cut become the sole physical
representation while preserving the strict inequality. Snapshot registration publishes its first
cut and then confirms `commit_seq`. Cleanup loads a commit cutoff before the floor and refuses to
promote anything newer than the cutoff. Those operations are sequentially consistent, closing the
publication/reclamation race without making resolution wait.

Promotion is owner-only and bounded. The next owner touch may promote its key; every fourth normal
owner batch runs a larger sweep and idle owners run a small low-frequency sweep. A bounded table
walker suppresses promotion only while its cursor is active, preventing a side-only insert from
moving into an already-scanned physical slot. Losers use the existing borrow retirement rules.
`ScatterState` arenas named by remaining nodes stay in their IO pool's deferred list until their
per-owner references reach zero.

Turning atomic mode off affects only newly admitted commands. In-flight groups decide, readers keep
consulting surviving maps, and the last record promotion drops the shard's tracking reference.

## V2 admission, diagnostics, and validation (superseded)

Admission reserves one memory-window slot before allocating the scatter arena. At the cap, prepare
returns backpressure without consuming the RESP frame; other clients and owners continue. Group
retirement releases the slot. The ticket is drawn only at successful publication, so the window is
a resource bound, never an ordering device.

`INFO STATS` exposes `atomic_groups`, `atomic_inflight`, `atomic_predecessor_reads`,
`atomic_chain_max`, `atomic_promotions`, `atomic_window_stalls`, and `atomic_records_freed`.
`tests/atomic_torn.py` requires predecessor reads and promotions to move. `tests/atomic_ryow.py`
covers pipelined atomic-write/read RYOW, atomic-then-plain same-key order, abandoned `MSETNX`
invisibility to own and foreign readers, and measured same-connection cross-key overlap.

Movers and store-producing commands remain outside the cross-shard atomic command set: `RENAME`,
`RENAMENX`, `COPY`, `SMOVE`, `LMOVE`, `RPOPLPUSH`, `SINTERSTORE`, `SUNIONSTORE`, `SDIFFSTORE`,
`BITOP`, `PFMERGE`, `LMPOP`, `ZMPOP`, `ZRANGESTORE`, and `SORT STORE`. They resolve recorded inputs
and stamp writes to recorded destinations but retain their prior command-level semantics. FLUSH
installs an owner-local ticketed tombstone cut before clearing plain slots. `SCAN`, random-key,
expiry, and eviction retain their existing non-snapshot command semantics while respecting record
ownership and predecessor lifetime.
