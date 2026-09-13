# AUDIT-TESTS — stability of the test/gate instrument (night lane `t-night-tests`)

> Historical research record: configuration recipes and measurements below describe earlier
> revisions. Deleted controls are not accepted by the merged server, and the manual-map
> dispatch-scaling instrument is retired. For current controls and merge dispositions, see
> [DESIGN-KNOBS.md](DESIGN-KNOBS.md) and [MERGE.md](MERGE.md).

Base: mainline `775aeea48`. Scope: `tests/` (99 Python batteries, 10 shell drivers, 3 C++ unit
tests) and `tests/gate.sh` (the merge instrument, 239 quick / 250 full rows). Nothing here was RUN:
the box's cores and the server binary were off limits for this lane, so every finding is static
(read the code, cite the line) plus `py_compile` and a pinned compile of the two C++ unit tests.
Where a verdict needs a run, it says so.

Ledger recount (static, loop multipliers applied): quick = 4 (builds) + 7 (boot matrix) + 1
(cmdmeta) + 8 (release boot) + 3 (atomic) + 2x33 (feature loop) + 2x6 (fused) + 2 (SORT) + 2x12
(debug surface) + 3 (xscript controls) + 3 (efficiency) + 3 (DUMP/RESTORE) + 7 (auth/ACL/DEBUG) +
2x8 (snapshot) + 6 (notify) + 7 (flip) + 3 (mm floor) + 2x24 (AOF) + 2 (frame order) + 14 (TLS)
= **239**; full = 239 + 5 (ASAN) + 4 (zc) + 1 (differ) + 1 (globcase) = **250**. Both constants in
`tests/gate.sh:47-48` are correct. The comment history above them is not (see F13).

---------------------------------------------------------------------------------------------------

## A. Ranked findings

P0 = can hand the operator a WRONG verdict (green for the wrong reason, or red with no defect and
no way to tell). P1 = flake or instrument fragility. P2 = hygiene / wall time.

| # | Sev | Where | Finding | Disposition |
|---|-----|-------|---------|-------------|
| F1 | P0 | `tests/spinprobe.py:12-18`, invoked at `tests/gate.sh:204,487` with PORT only | `find_srv()` takes the FIRST `/proc/*/comm` containing `tomokv`. On the shared box (other lanes' servers are always up) the probe measures a foreign process's jiffies and the row's verdict is about the wrong server. Also nothing proves the partial frame reached the parser: a server that dropped the connection on the half frame idles identically and PASSES. | **Fixed**: gate passes `$SRV`; without a pid the probe resolves the listener of PORT via `ss` and refuses ambiguity; after the idle window it completes the frame and requires `+OK` (mechanism proof: the parser held the partial frame). |
| F2 | P0 | `tests/gate.sh` (no `timeout` except `:492,:570`; no EXIT trap; memtier at `:499-515,:550-556`) | A hung battery hangs the whole ~45 min gate forever, and Ctrl-C leaves the server on 7899 (and any memtier) alive — the exact "leftover server" class the port guard then trips on next run. | **Fixed**: `py()` wrapper (bounded wall time, timeout/self-skip named in the row log), EXIT/INT/TERM cleanup of `$SRV`, oracle and memtier pids. |
| F3 | P0 | `tests/lbsignals.py:21-41` | The RESP "reader" is `sleep 0.05` + one `recv` + a "one burst" heuristic. A DEBUG LBSIGNALS dump (~10 KB at 8 threads, more at 64) that arrives in two segments is truncated; the parser then indexes missing fields and the row fails with no server defect — or a later reply is read as the current one. This is the one gate battery whose parser is timing-probabilistic by construction. | **Fixed**: real RESP parser from `tests/_lib.py`. |
| F4 | P0 | `tests/atomic_torn.py:137`, `tests/contarity.py:128` (`fields[2]==b"ex"`); `tests/lbsignals.py:94` ("both roles present"); `tests/sort.py:112-120,141,152,167` (owner := `shard % lb_ex_threads`); `tests/barrier.py:213-251` | Executor discovery keys on the `ex` role label. Under `--thread-mode 1s` every thread is `io` (`src/cmd/lbsignals.cc:156`), so these find zero executors and abort — correct for a 1s boot that cannot form the geometry, but the reason is wrong ("needs two executors" when it has eight owners), and `sort.py` divides by `lb_ex_threads`=0. The truth for BOTH modes is the shard rows: `shard <sid> <owner_tid>` (`lbsignals.cc:164-168`). | **Fixed** for atomic_torn/contarity (owners from shard rows via `_lib.topology`); lbsignals.py made mode-aware (ex legs skip WITH reason on 1s, io legs still assert conservation). sort.py/barrier.py: flagged, not converted (they also assume the default `sid % ex` shard-home map, which is a second assumption the LBSIGNALS owner map removes — see B.2). |
| F5 | P1 | `tests/execfix.py:164-175` (`two_owner_keys` → distinct SHARDS), `tests/xscript.py:96-110` ("eight distinct owners" = 8 shards), `tests/execiso.py:187-209`, `tests/multires.py:148-162`, `tests/expwide.py:133-147`, `tests/xmove.py:86-104`, `tests/edgetime.py:529-535`, `tests/scriptatomic.py:324-333`, `tests/session_monotonic.py:238-247` | "owner" means the integer DEBUG SHARD returns, i.e. a SHARD. On the gate boot (16 shards, 2 executors) two distinct shards share an executor half the time. Where the mechanism is the cross-SHARD group/scatter path this is exactly right; where the mechanism is CONCURRENCY between owners (a foreign commit landing between two fragments; a race two executors must lose/win) a "two-owner" arm can be a one-executor arm and the counter it asserts still advances. Each of these tests asserts a counter (`atomic_exec_read_cuts`, `script_keys_armed`, `atomic_groups`, ...) so none is vacuous today, but the label lies and the race-class arms are weaker than they claim. | **Flagged, not converted** (needs a run to confirm counters still fire when the pair is forced cross-OWNER). `_lib.cross_owner_keys` / `same_shard_pair` exist now; the conversion is a one-line swap per test — list in B.2. |
| F6 | P1 | `tests/atomic_torn.py:655-663` (MSET-8 OFF), `:733-748` (RENAME OFF, 4 rolls x 1s), `:762-786` (SINTERSTORE OFF, 4 rolls x 2.5s), `:826-853` (RENAMENX/COPY OFF) | The OFF "discovery" arms are timing-probabilistic by nature (they must OBSERVE a tear that atomic=0 permits). They already skip-with-reason on a clean run and the ON arms still run and assert `reads>0`, `commit_holds>0`, `predecessor_reads>0`, `promotions>0`, `localfast>0` — so the row is never vacuous. Cost of the new kernel: when no tear manifests the re-rolls burn 4x1 + 4x2.5 = 14 s per run for nothing. RENAMENX/COPY already have a deterministic widener (`DEBUG ATOMIC-CONDITIONAL-DEFER 100000`, `t_server.cc:1006-1015`); if those two still miss, the park is not covering the second contender's validation and that is a server-side question, not a test one. | Needs hook **`DEBUG ATOMIC-OFF-HOP-DELAY <us>`** (section C). Until then: unchanged except the topology fix; skip text now names the missing hook. |
| F7 | P1 | `tests/borrow_registry.py:152-156` (`measure()` = borrow x3 then plain x3), `:191-197` | The two arms are measured back-to-back but SEQUENTIALLY, so a CPU-state shift between the borrow rounds and the plain rounds lands on one arm only — exactly the 567→884 ns control flake. The control is the environment instrument; when it moves, the row is measuring the box, not the registry. | **Fixed**: arms interleaved per round (drift hits both); on control drift the busy pair is re-rolled (x3), then a POST baseline (holders released) is measured and used if it agrees; every original assertion kept, the FAIL text names "environment shifted" when nothing agrees. |
| F8 | P1 | `tests/flip_under_load.py:132-134,159-161` | Every exception collapses to `repr(e)`: a client-side 15 s `socket.timeout` under saturation and a server-side close both read "writer N: ..."; no INFO delta is captured, so "one writer conn drop in 20 flips once" cannot be attributed. | **Fixed**: exceptions classified (TIMEOUT/EOF/RST/EPIPE/ERROR), `connected_clients`, `rejected_connections`, `client_output_buffer_limit_disconnections`, `flip_completed`, `flip_clients_transferred` deltas printed always, and on a 1s boot the battery skips itself with the reason (FLIP is 2s-only: `t_server.cc:2637`). Hook wish in section C. |
| F9 | P1 | `tests/pubsub.py:400-462` (SSUBSCRIBE churn: `oob_frames_deferred` must move), `:481-556` (fan-out batching ratio) | Two occurrence gates already re-roll (x5, growing pressure) and skip-with-reason; every message/count assertion stays strict on each roll. Correct posture; the cost is 5 rolls (~10 s) when the kernel never opens the window. | Needs hooks **`DEBUG PUBSUB-ACK-DEFER`**, **`DEBUG PUBSUB-FANOUT-DEFER`** (section C). No test change. |
| F10 | P1 | `tests/gate.sh:129` (`stop(){ ...; sleep 5; }`, 43 calls) + 12 standalone `sleep 5` | ~55 x 5 s = ~4.6 min of dead time per gate run. Not load-bearing: the server sets `SO_REUSEADDR`+`SO_REUSEPORT` (`src/core/io_loop.h:74,167`) and `wait $SRV` already guarantees the listener is closed. `boot()` also spawns `./build/tomokv --help` on EVERY 200 ms poll (`:107,:121`) — a no-op costing up to 150 process spawns per boot. | **Fixed**: `stop()` polls the port closed then settles `GATE_STOP_SETTLE` (default 1 s; set 5 to restore the old timing if a row goes red — that would itself be a finding). `--help` loop removed; `boot`/`boot_fused` deduplicated. **Verify on the box.** |
| F11 | P1 | `tests/gate.sh:78-80` (QUICK tier), `tests/differ_gate.sh:12-14,140-151`, `tests/gate.sh:928-930` | Three rows depend on the `/tmp/claude-1000/redis74` symlink. Missing symlink today = a Python traceback in the ACL-categories row, `differ_gate.sh` exiting 2 with one stderr line, and a "target boot failed"/"see log" globcase row. | **Fixed**: `oracle_preflight` — quick tier fails the ACL row with the reason; full tier exits 2 up front with the expected layout spelled out. |
| F12 | P1 | `tests/gate_refs.txt` (refs pinned 2026-08-29/09-01), box kernel changed 2026-09-02 | Nothing records which kernel the refs were measured on, so a -3 % verdict after a kernel change is uninterpretable. | **Fixed**: `# kernel:` pin line (deliberately `pre-7.0.0-30`, NOT re-pinned) and a WARN row in the NIC tier when `uname -r` differs. Re-pin on the box. |
| F13 | P1 | `tests/gate.sh:27-46` | EXPECT history has "211 -> 213" twice and no 213 -> 224 step; constants are right but the audit trail is not. And the ledger is only a COUNT: when it drifts nothing names the row that vanished, and nothing records how long rows take (the question "what dominates 45 minutes" has no instrument). | **Fixed**: comments repaired; every row appends `label, seconds-since-previous-row, verdict` to `/tmp/gate-ledger-<tier>.txt`; a count mismatch prints the row-name diff against the previous run's ledger; the summary prints the 12 slowest rows. |
| F14 | P1 | `tests/gate.sh:69-74` | `make` and the ASAN build discard their output (`>/dev/null 2>&1`, `2>/dev/null`); a red "ASAN build" row has no reason. | **Fixed**: build logs kept and named in the row. |
| F15 | P1 | `tests/flipctl_unit.cc` (never built anywhere; `tests/config_parser_test.cc` only via the gate) | The flip-controller model unit test compiles clean today (pinned `g++` this lane) but is run by nothing, so a model regression would surface only in `flipctl.py`'s 2-5 min live row. | **Fixed**: static gate row (EXPECT 239→240 / 250→251). NOT executed this lane — its first gate run is the verification. |
| F16 | P2 | `tests/gate.sh:885,911` | "ASAN clean" = "no `ERROR: AddressSanitizer` in the log" — vacuously green if the ASAN server never booted (an empty log has no ASAN text either). | **Fixed**: also requires the server's shutdown line in the same log. |
| F17 | P2 | 24 files no driver invokes (census below) | Some are manual repros by design; the batteries among them (barrier, doubles, dumpttl, cmdgap2, cmdmeta, aof_frames, netio, replyschema, storeorder, maskflood, spscmask_flip, evict_battery, flatstore_alloc_fail, flush_capture) are coverage that exists on disk and nowhere else. `NOTES-HYGIENE.md` already reported this class once. | Flagged. Wiring changes the ledger and needs runs; not this lane. |
| F18 | P2 | ~60 copies of `encode`/`Conn`/`read_reply` (census B.1) | Divergent behaviours: error replies raised vs returned; RESP3 markers (`_ % ~ > , # (`) handled in some, not others (`pipeorder.py:11-19` has none); socket timeouts 10–120 s; some `makefile` buffered 1 MiB, some default. Each copy is a place for a parser bug to live alone. | `tests/_lib.py` shipped; five consumers migrated; migration list in B.1. |
| F19 | P2 | `tests/pipeorder.py` | Its premise (cross-shard MSET) is never asserted; the boot it runs on has DEBUG (`gate.sh:544`). Over 400 iterations with 16 shards it is overwhelmingly true, so this is labelling, not vacuity. | Flagged. |
| F20 | P2 | `tests/differ_gate.sh:24-110` vs `tests/gate.sh:100-129` | The full-tier driver already has the BETTER boot/stop/guard implementation (ss-based owner check, pid-owned stop, EXIT trap). The main gate reimplements a weaker one. | gate.sh adopts the ss guard + trap; unifying the two into one sourced helper is the natural next step. |
| F21 | P2 | `tests/session_monotonic.py:265-268`, `tests/edgeenc.py:650,669,692`, `tests/expwide.py:135-137`, `tests/multi_exec.py:218-220` | DEBUG-dependent arms SKIP silently when the hook is denied and the battery still prints PASS. On the gate's armed boots they run; on any other boot the deterministic arms vanish without a red. `hexpire.py` was given an executed-check floor for exactly this (NOTES-GATEFIX A8). | `_lib.Report` treats recorded skips as failures when `TOMO_GATE_STRICT=1` (exported by gate.sh). Existing tests keep their behaviour until migrated. |
| F22 | P2 | `tests/lbsignals.py:144,147` | `avg_depth >= 0.0` and `queue_delay_ewma_us >= 0.0` are tautologies on non-negative quantities. Not weakened, not strengthened (a real bound needs a run). | Flagged. |

---------------------------------------------------------------------------------------------------

## B. The four questions

### B.1 Q2 — copy-pasted helpers; what `tests/_lib.py` replaces

RESP client copies (file:line of the definition): `aclsel.py:28`, `acl.py:45`, `aof_fsync.py:35`,
`aof.py:98`, `aof_frame_order.py:65`, `aof_rewrite_triggers.py:32`, `aof_torn_group.py:34`,
`atomic_ryow.py:30`, `atomic_torn.py:44`, `auth.py:23`, `barrier.py:51`, `bitfield.py:23`,
`blocking.py:28`, `blockmulti.py:39`, `borrow_registry.py:68`, `climon.py:26`, `cmdgap.py:37`,
`cmdgap2.py:38`, `cmdmeta.py:57`, `contarity.py:39`, `debug.py:23`, `doubles.py:51`,
`dumprestore.py:26`, `edgetime.py:40`, `execatomic.py:83`, `execfix.py:71`, `execiso.py:89`,
`expireindex.py:73`, `flatstore_alloc_fail.py:23`, `flip.py:26`, `geo.py:21`, `globcase.py:25`,
`infofix.py:29`, `notify.py:25`, `pubsub.py:26`, `servertail.py:47`, `session_monotonic.py:71`,
`snap_typed_race.py:9`, `watchlive.py:64`, `zsetops.py:23`, `xshard_dispatch_scale.py:33`; plus
function-style readers in `arity.py:20`, `aof_rewrite.py:24`, `edgeproto.py:45`, `flip_ttl.py:42`,
`flip_under_load.py:63`, `lbsignals.py:21`, `maskflood.py:19`, `pipeorder.py:11`, `ryow.py:25`,
`storeorder.py:76`, `torn_mset.py:47`, `torture.py:22`, `zc.py`, `flush_capture.py:8`,
`snap_typed_roundtrip.py`, `ryow_sort_repro.py:10`, `differ.py:42`.

Geometry (DEBUG SHARD / LBSIGNALS) copies: `atomic_torn.py:125` (LBSIGNALS+SHARD, owner-correct),
`contarity.py:119,139` (owner-correct), `barrier.py:226`, `execfix.py:164`, `execiso.py:187`,
`expwide.py:133,150,259`, `multires.py:148`, `scriptatomic.py:324`, `sort.py:138,159,174`,
`storeorder.py:278`, `xmove.py:86`, `xscript.py:82,96`, `xshard_dispatch_scale.py:75`,
`edgetime.py:529`, `edgeenc.py:672`, `pushtear.py:556`, `aof.py:164`, `aof_frame_order.py:136`,
`aof_torn_group.py:98`, `snap_cut_battery.py:120`, `session_monotonic.py:238`.

Boot helpers (tests that spawn servers themselves): `servertail.py:315`, `wiredump.py:241`,
`writer_atomic_campaign.py:133-162`, `blocking.py:86`/`stream.py:84`/`streamgroups.py:76`
(`start_wait`, a thread helper, not a boot), plus the shell drivers `aof_rewrite_matrix.sh:39`,
`aof_rewrite_trigger_matrix.sh:37`, `xshard_dispatch_scale.sh:66`, `differ_gate.sh:53`,
`lru_slow.sh`, `gate.sh:100/114/789/889`.

`tests/_lib.py` (new) provides: `encode`, `RespError`, `Conn` (full RESP2/3 reader, errors
returned not raised, `must()` raises), `info()`, `thread_mode()`, `lbsignals()` +
`topology()` (owner map from SHARD ROWS — mode-agnostic), `shard_of()`, `keys_by_owner()`,
`cross_owner_pair()`, `same_shard_pair()`, `pipelined_rate()`, `wait_until()`, `armed()` (DEBUG
hook arm/disarm context), `Report` (ok/bad/skip/finish with the `TOMO_GATE_STRICT` rule),
`skip_all()` (exit 3 — the gate paints that red, see `py()`). Migrated this lane: `atomic_torn.py`
(topology), `contarity.py` (topology), `borrow_registry.py` (shard count), `xshard_dispatch_scale.py`
(owner map, thread count), `lbsignals.py` (whole client + parser), `flip_under_load.py` (mode probe,
INFO counters). `spinprobe.py` stays dependency-free (raw socket) by design: it is run on the
fused rows too and must not gain a failure mode of its own. Next candidates, in order of divergence risk: `pipeorder.py`
(RESP3-blind reader), the seven `Resp` classes that RAISE on `-ERR` (`atomic_*`, `aof_*`,
`blocking`, `execatomic`, `execiso`, `watchlive`) because a raised error inside a reader thread is
recorded as a generic exception and loses the server's text.

### B.2 Q2 — tests that assume 2s-only geometry

Break outright on `--thread-mode 1s`:
- FLIP callers: `flip.py`, `flip_ttl.py:175`, `flip_under_load.py:149`, `maskflood.py:29`,
  `spscmask_flip.py`, `flipctl.py` — FLIP answers `ERR FLIP is unavailable with --thread-mode 1s`
  (`t_server.cc:2637`). 2s-only by design; `flip_under_load.py` now skips itself with that reason,
  the others should adopt `_lib.skip_all` the same way.
- `sort.py:112-120` (`lb_ex_threads` must be 2; divides by it), `barrier.py:213-251` (shards ==
  executors), `lbsignals.py:94-98` (both rollups).
- `atomic_torn.py:137`, `contarity.py:128` — FIXED (owners from shard rows).
Assume the DEFAULT shard-home map (`sid % executors`) rather than asking LBSIGNALS: `sort.py:141,152,
167`, `barrier.py:227`. Both are gate-pinned to `--ratio 6:2 --shards 16` where the assumption holds;
`--shard-home` (used by `xshard_dispatch_scale.sh:63`) would silently break them.
Conflate shard with owner (F5): `execfix`, `xscript`, `execiso`, `multires`, `expwide`, `xmove`,
`edgetime`, `scriptatomic`, `session_monotonic`.

### B.3 Q3 — gate.sh

**Row runtime distribution.** There was no instrument (only a wall clock), so the first change is
the per-row elapsed column in the ledger file. Static lower bounds that no server speed can shrink:

| component | count | fixed seconds | note |
|---|---|---|---|
| `stop()` fixed `sleep 5` | 43 | 215 | F10 — now poll + 1 s settle |
| standalone `sleep 5` after kills/stops | 12 | 60 | F10 — replaced by `settle` |
| boot polls (`--help` spawn per 200 ms) | 45 boots | ~20-60 | removed |
| idle-CPU ceiling | 1 | 5 | measurement, keep |
| MM floor (memtier 20 s, sampled 8+6) | 1 | 14 | measurement, keep |
| saturated-flip memtier | 1 | 25+3 | measurement, keep |
| `flipctl.py --stable-seconds 30` (+ramp/idle/surge, timeout 300) | 1 | 60-120 est. | measurement, keep |
| Retired `xshard_dispatch_scale.sh` (historical 2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 historically, 0 now | 90-150 est. historically | manual ownership instrument removed; see `MERGE.md` |
| `atomic_torn.py` hammers (3+2.5+2+2.5 s) + OFF re-rolls (up to 14 s) + RENAMENX/COPY parks (2 x 64 x 100 ms = 12.8 s) + window/lease/churn (~6 s) | 1 quick, +1 ASAN | ~45-60 each | re-rolls are dead time on this kernel (F6) |
| `spinprobe.py` (6 s baseline + 6 s probe) | 3 | 37.5 | measurement, keep |
| Python batteries' literal sleeps (census: flipctl 17 s, limits 5.8, edgetime 4.5, atomic_ryow 4, hexpire 3.4 — most run x2) | | ~60 | keep |
| AOF matrices (5 boots, `sleep 5`, `sleep 2`) | 2 (per persist-io) | ~30 | own drivers |
| full tier: differ (2 targets x 2 seeds x 34 suites + 4 repeats = 140 legs), NIC (3 cells x 2M-key prefill + 15 s) | | dominant | own drivers |

So of the ~45 min: ~5 min was pure sleep (recovered), ~6-8 min are measurement rows that must stay
timed, ~2 min is atomic_torn discovery dead time (hook-dependent), and the remainder is ~180
battery invocations whose individual cost the ledger now records.

**Sharing boots without changing what rows prove** (candidates, NOT done — each changes a boot
line or a row label and needs one run to confirm):
- `gate.sh:166` (`--atomic 1 --enable-debug-command yes`) is byte-identical to the AT=1 feature boot
  at `:179`; atomic_torn/atomic_ryow could join that iteration (−1 boot, −1 row: the two shutdown
  rows merge).
- `gate.sh:226` debug-surface boots differ from the feature boots only by `--lb-age-sample-rate
  1024`; adding that flag to the feature boots merges 12 batteries in (−2 boots). lbsignals.py needs
  the flag (age samples must fire); no feature battery reads LB ages.
- `gate.sh:498` "flip battery reboot" exists to give the saturated-flip row a clean shutdown log; the
  row could run at the tail of the first flip boot (−1 boot). The flipctl boot cannot be shared
  (`--flip-auto 1` fights manual flips).
- Cannot share: SORT (6:2), xscript controls (three knob values), expire-index and borrow (`--shards
  1`, different zc/obuf knobs), auth/ACL/DEBUG-local, every persistence row (dir/file identity is the
  test), TLS.
Net: ~4 boots ≈ 30 s. Not where the time is.

**EXPECT bookkeeping** — F13. **Leftover hygiene** — F1, F2, plus the pre-boot guard now uses
`ss -ltnp` (names the owning pid; catches a listener that is bound but not yet accepting) in
addition to the connect probe, and a one-time WARN listing stray `tomokv` processes on the box
(other lanes legitimately run servers, so a WARN, not a FAIL). **Oracle** — F11. **Refs** — F12.

### B.4 Q1 — timing-probabilistic rows, and B.5 Q4 — vacuous criteria

Q1 is F3, F6, F7, F8, F9 above. Rows that already use the deterministic hooks and need nothing:
`session_monotonic.py` (ATOMIC-COMMIT-DELAY / READ-DELAY, asserts `atomic_read_cuts_held` /
`atomic_commit_holds`), `execatomic.py`/`execiso.py` (ATOMIC-FANOUT-DEFER, `atomic_exec_read_cuts`),
`xscript.py` (SCRIPT-STAGE-DEFER, `script_keys_armed`), `barrier.py` (BARRIER-HOLD,
`barrier_releases_held`), `blocking.py` (BLOCKING-TIMEOUT-REAP), `storeorder.py`
(ATOMIC-DIRECT-DEFER), `atomic_torn.py` ON arms (ATOMIC-COMMIT-DELAY) and RENAMENX/COPY OFF
(ATOMIC-CONDITIONAL-DEFER), `aof_torn_group.py` (AOF-STOP-AFTER-GROUP-FRAGMENTS),
`aof_rewrite_*` (AOF-REWRITE-PAUSE). Still probabilistic with no hook available: pubsub churn and
fan-out batching (F9), the four atomic OFF discovery controls (F6), `snap_typed_race.py` /
`snap_cut_battery.py` (a BGSAVE races a mutation storm; both assert `snapshot_preimages`/
`cuts_waited` fired so a miss is red, not green — correct posture, but a `DEBUG SNAPSHOT-CUT-HOLD`
would make it one roll), `aof_frame_order.py` (relies on entering the writer window "reliably enough"
on persist-io normal per `gate.sh:747-752`).

Q4 — would pass against a server that ignored the command: F1 (spinprobe), F16 (ASAN clean),
F21 (silent DEBUG skips), F22 (tautologies). Checked and NOT vacuous, for the record: `zc.py` (bytes
compared, and `gate.sh:907` requires `zc_sends>0`), `pipeorder.py` (400 rolls, 74 % pre-fix),
`torture.py` (`churn writes landed` is `>0` — a liveness probe, labelled as such), `ryow.py`,
`snap_typed_race.py` (PREIMAGE-FIRED), `flush_capture.py`, every AOF row (`records>0`,
`replayed>0`), `borrow_registry.py` (DEBUG BORROWCOUNT ≥ 1000 while parked, 0 before/after), the
xscript controls (byte-for-byte CROSSSLOT + zero counters), the AOF-off negative control.

---------------------------------------------------------------------------------------------------

## C. Server-side DEBUG hooks these tests need (do not exist yet)

Named to match the existing grammar (`DEBUG <NAME> <integer>`, 0 = disarm, production default 0,
behind `--enable-debug-command`; `src/cmd/t_server.cc:940-1030` is the pattern).

1. **`DEBUG ATOMIC-OFF-HOP-DELAY <us>`** — with `atomic 0`, park a multi-owner write (MSET, RENAME,
   SINTERSTORE/SMOVE, LMPOP) for N µs between its FIRST owner hop publishing and the remaining hops
   being dispatched. Makes `atomic_torn.py`'s four OFF discovery controls (F6) one deterministic roll
   each (a concurrent MGET/SMEMBERS lands in the park and must see the tear) and deletes 14 s of
   re-rolls. Must not touch the atomic-ON scheduling (same rule as ATOMIC-CONDITIONAL-DEFER).
2. **`DEBUG PUBSUB-ACK-DEFER <us>`** — park the retirement of an SSUBSCRIBE/SUBSCRIBE acknowledgement
   after the registration is published, so a concurrent SPUBLISH's delivery to that connection is
   necessarily deferred as an out-of-band frame (`oob_frames_deferred` must advance). Makes
   `pubsub.py:400-462` one roll.
3. **`DEBUG PUBSUB-FANOUT-DEFER <us>`** — hold the fan-out pass open for N µs after the first
   publish of a pass is staged so pipelined publishes coalesce into one delivery batch
   (`pubsub_delivery_batches < publishes`). Makes `pubsub.py:481-556` one roll.
4. **`DEBUG FLIP-HANDOFF-HOLD <us>`** — hold a connection between its detach from the old IO thread
   and its attach to the new one during FLIP, so `flip_under_load.py` can prove on demand that
   in-flight pipelined ops on a connection crossing threads retire in order and that no reply is
   lost while the socket has no owner. Today the row relies on the flip landing while a batch is
   in flight (it does, at 8 writers, but the single observed conn drop is exactly the case this
   would isolate).
5. **`DEBUG SNAPSHOT-CUT-HOLD <us>`** — hold the snapshot cut open after it is chosen so a typed
   overwrite storm deterministically lands preimages (`snap_typed_race.py`, `snap_cut_battery.py`).
6. **`DEBUG LBSIGNALS` in 1s** — not a new hook, a request: emit the role as `io` (as now) but ALSO
   emit `derived thread_mode 1s|2s` so a test can tell fused from split without a second `INFO
   server` round-trip (`_lib.thread_mode` uses INFO today).
7. **`read_local_*` totals must survive client close** (owner's finding, 2026-09-03 night). INFO
   folds `read_local` at read time from `g_server->thread(t).read_local_stats()`
   (`src/cmd/t_server.cc:1557-1561`), and those per-thread figures are summed from LIVE clients:
   after memtier disconnects every `read_local_*` reads 0. Any battery asserting on them must
   sample MID-RUN (while the load connections are open), or the server must fold each client's
   totals into its thread's cumulative counters on client close (the same shape as
   `client_output_buffer_limit_disconnections`: a monotonic per-thread total, not a live-set sum).
   No battery in this tree asserts `read_local_*` yet (grep is empty); `bplus.py` on its own branch
   does, and is the first consumer of the fold. Request: per-thread cumulative `read_local_*`
   folded on client close (or an explicit `read_local_*_total` alongside the live gauges).

Cross-check requested with the `bplus.py` fix (LBSIGNALS parsing accepting `io`-role threads in
1s): the same assumption existed in `atomic_torn.py:137` and `contarity.py:128` (fixed here, owners
from shard rows), `lbsignals.py:94-98` (fixed, mode-aware), and survives in `sort.py:112-120,141,
152,167` (owner := `shard % lb_ex_threads`, divides by zero in 1s) and `barrier.py:213-251`
(shards == executors) -- both gate-pinned to 2s boots and both listed in B.2 for conversion.

---------------------------------------------------------------------------------------------------

## D. ARCHITECTURAL / ALGORITHMIC IDEAS (not implemented)

Things noticed about the SERVER while reading the tests. Each is checked against the owner's rules:
single-owner writes; reads never obstructed by writes; immutable replacement + QSBR; numeric knobs
0=off/-1=auto; hard-code-or-delete; one file per feature.

1. **Geometry oracle as one verbatim dump.** Every cross-owner battery does 4 000-20 000 `DEBUG SHARD`
   round-trips to find keys on chosen owners (`atomic_torn.py:150`, `xscript.py:97`, `execiso.py:213`
   ...); at ~20 µs each that is 0.1-0.4 s per test and a whole tier of copy-pasted loops. A
   `DEBUG SHARD-OF <prefix> <count>` (or `DEBUG SHARDS key1 key2 ...`) answering `sid owner_tid` per
   key in one reply removes the loop from ~20 tests. Read-only, no hot-path code, no knob.
2. **Owner id in LBSIGNALS shard rows is the ground truth; the role label is derived.** Tests that
   said "executor" meant "thread that owns shards". Suggest the server's own tooling (flipctl,
   INFO LB) also report `owners` (count of distinct owner tids) so 1s and 2s share one vocabulary;
   `lb_ex_threads=0` in 1s mode is a true statement that reads as a bug.
3. **OFF-mode hop delay is the same mechanism as the ON-mode commit delay.** `set_debug_atomic_
   commit_delay` parks between ticket draw and publish; the requested OFF hook parks between hop 1
   and hop 2 of the non-atomic scatter. Both are "delay after first publication"; one `debug_hop_
   delay` field consulted at the scatter engine's hop boundary (0 = no code runs) serves both and
   keeps `--atomic 0/1` scheduling otherwise identical — which is also what makes the OFF control an
   honest control.
4. **Idle-CPU ceiling as a counter, not jiffies.** `gate.sh:146-155` samples `/proc/$SRV/stat` for
   5 s. The io loop already counts `spins`, `wakes_sent/recv`, `iterations` per thread
   (`lbsignals.cc:157-160`). "Parked connections cost zero iterations" is a delta of those counters
   over a quiet second: deterministic, per-thread, and it names WHICH thread spins. Same for
   `spinprobe.py`: `iterations` delta on the io thread owning the half-frame connection instead of
   process jiffies. No hot-path change (counters exist); the gate row becomes a 1 s row.
5. **Two roles, two rollups, one thread type.** In 1s every thread is `Role::Ifid` and executes; the
   `ex` rollup is empty. The controller/LB code paths that branch on role (`flipctl.cc:510-511`,
   `thread.h:817 task_consumer = role()==Role::Ex`) read as if 1s were a degenerate 2s. If 1s stays a
   first-class mode, a `Role::Fused` (or `owns_shards()`/`serves_clients()` predicates) would let
   LBSIGNALS, INFO LB and FLIP say the true thing (`FLIP: unavailable, threads are fused`) instead of
   `ex_threads:0`. Hard-code-or-delete: if 1s is the future base, the `ex` rollup is dead data.
6. **Borrow registry cost is a per-shard scan the tests prove flat; the same shape exists for
   pub/sub blobs.** `borrow_registry.py` guards O(live borrows) per GET; `pubsub_blobs` (encode-once
   frames retained until the last subscriber's send completes, `pubsub.py:527-530`) has the same
   lifetime pattern (immutable blob + last-reader release = QSBR-style). If the blob table is a
   linear scan on release, the borrow_registry test shape (holders parked, probe rate) is the test to
   write; the fix shape (indexed slots, O(1) release) is the same one NOTES-XPERF2 landed.
7. **AOF frame-order guard is a writer-pass invariant that could be a `static_assert`-style
   structural rule.** `gate.sh:747-752` says the control frame landed inside a large record because
   the writer flushed a ready GCMT "at the top of a pass without checking the large record still held
   the stream". A stream owner token (who holds the physical stream: large-record writer or control
   writer) makes the interleave unrepresentable rather than checked; single-owner writes already is
   the house rule for the store, this applies it to the AOF byte stream.
8. **`DEBUG SLEEP` is unbounded up to 86 400 s (`t_server.cc:846-860`).** It parks the EXECUTOR that
   runs it (`nanosleep` inside the command). A test that mistypes the unit freezes one owner for a
   day; the gate's `py()` timeout now bounds the test but not the server. Knob philosophy says the
   bound should be derived (e.g. ≤ the boot's client timeout) or the command should park the
   CONNECTION (defer the reply) rather than the thread — which also respects "reads never obstructed
   by writes" for the other clients on that owner.
9. **Snapshot preimage race needs the cut to be observable.** `snapshot_preimages` proves a preimage
   was TAKEN; nothing lets a test read the cut ticket and compare it with the commit tickets of its
   mutations. Exposing `snapshot_cut_ticket` in INFO (a plain read of an already-atomic word) turns
   the probabilistic race row into an exact assertion: every key mutated with ticket > cut must read
   its preimage from the file, every key with ticket ≤ cut its post-image.
10. **Test-visible thread mode belongs in the wire, not the boot line.** `gate.sh:193` greps the
    server log for `thread-mode=1s, overlap=0`; `INFO server` already has `thread_mode`. Log greps
    are the only assertions that break when a log line is reworded; the gate has ~14 of them
    (`stuck: live_conns=0 ...`, `direct=`, `dispatched=`, `tls: accepts=`, `wb: ... err=0`,
    `AOF warning: truncated AOF tail`). A `DEBUG SHUTDOWN-REPORT`/INFO-at-exit contract (the same
    fields, machine-readable, written as the last line) would let the gate parse one structured line.

---------------------------------------------------------------------------------------------------

## E. Census — files no driver invokes (`tests/gate.sh` + all sub-drivers + Makefile)

`aof_frames.py barrier.py benchfeat.py broaden_bench.cc cmdgap2.py cmdmeta.py doubles.py dumpttl.py
evict_battery.py flatstore_alloc_fail.py flipctl_unit.cc(now wired) flush_capture.py maskflood.py
netio.py push_tear_repro.py replyschema.py ryow_sort_repro.py spscmask_flip.py storeorder.py
torn_mset.py wiredump.py writer_atomic.py writer_atomic_campaign.py` (24; `config_parser_test.cc` is
built by the gate itself).

## F. Commits on `t-night-tests` (each keeps or adds a mechanism-fired check; none weakens one)

| commit | what it converts / fixes | finding |
|---|---|---|
| `AUDIT-TESTS` | this document | — |
| `WIP: helper dedup` | `tests/_lib.py` (RESP client, LBSIGNALS parser, owner-from-shard-rows topology, geometry helpers, `armed()`, `Report`, `skip_all`) + owner selection from shard rows in `atomic_torn.py`, `contarity.py`, `borrow_registry.py`, `xshard_dispatch_scale.py` | F4, F18 |
| `tests/lbsignals.py` | real RESP reader; ex legs skip WITH reason on 1s and the ex rollup's emptiness is asserted; fixed settle sleeps became bounded polls | F3, F4 |
| `tests/spinprobe.py` | pid from the caller or the port's listener (refuses ambiguity); frame completion must answer `+OK` | F1 |
| `tests/flip_under_load.py` | failure class (TIMEOUT/EOF/RST/EPIPE/ERROR-REPLY); server counter deltas printed every run; exit 3 self-skip on a 1s boot | F8 |
| `tests/borrow_registry.py` | arms interleaved per round; re-roll on control drift; post baseline when the pre one is stale; assertions unchanged, reference named | F7 |
| `tests/gate.sh` + `gate_refs.txt` | EXIT/INT/TERM cleanup; `ss` port guard naming the pid; `py()` timeout wrapper (timeout / exit 3 stay red); build logs kept; tool + oracle preflight with one clear message; per-row ledger with elapsed and name-diff on drift; `settle` replaces 55 x `sleep 5`; boot dedup, `--help` spawn loop gone; `flipctl_unit` static row (EXPECT 240/251); ASAN-clean requires the shutdown line; kernel pin WARN; `TOMO_GATE_STRICT=1` | F2, F10-F16 |

What this lane did NOT do, deliberately: run anything (the first gate run is the verification of
`flipctl_unit`, of the 1 s `settle`, and of the `py()` wrapper); convert the F5 shard-vs-owner
helpers (needs a run per test to confirm counters still fire on a forced cross-OWNER pair); wire
the 24 unwired files (changes the ledger); re-pin the NIC refs (owner action on the box).

Verification done: `python3 -m py_compile tests/*.py` clean; `bash -n tests/gate.sh` clean; both
C++ unit tests compile pinned (`taskset -c 40-47,168-175`); the static ledger recount is 240/251.
