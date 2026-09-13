# NOTES-EXPWIDE — one expiry cut per logical operation

Lane `t-expwide`. Oracle: vanilla redis 7.4.2 at `/tmp/claude-1000/redis74/src/redis-server`.
Resources used: cores 80-95 only, ports 7570-7579 only, every server `taskset`ed and stopped by the
pid resolved from its listening socket.

The brief handed this lane two items that `NOTES-EDGETIME.md` recorded as **REPRODUCED — SHELVED**,
and asked, for the first of them, which answer is actually CORRECT rather than assuming.

## Verdict table

| # | Item | Verdict | Evidence |
|---|---|---|---|
| W1 | a key's deadline landing inside a widened cross-shard fan-out | **REPRODUCED — FIXED**. Not a hook artifact: it also reproduces with **no hook at all**, 17 torn reads in 19997 | `tests/expwide.py` S1, `repro-natural` |
| W2 | a key expiring between queueing and EXEC, and *inside* EXEC | between-and-EXEC matches the oracle; **inside EXEC is REPRODUCED — FIXED**, 60/60 | `tests/expwide.py` S2/S3 |
| W3 | (found while fixing W1) a cross-shard **script** pins a CLOCK_MONOTONIC value as "now", so it never sees any key as expired | **REPRODUCED — FIXED**, deterministic, 100% | `tests/expwide.py` S4 |

The reference settles W1: **redis takes one expiry cut for the whole command**, so per-owner lazy
expiry inside one operation is not defensible. The argument and the measurement are below.

---

## What the reference does, measured rather than assumed

Redis expires lazily *per access*, which is the usual reason given for letting each owner decide.
That is true and irrelevant: the accesses inside ONE command all compare against the same instant,
because `commandTimeSnapshot()` returns `server.mstime`, and `server.mstime` is only refreshed at
execution-nesting depth zero. A command, a transaction and a script are each one such top-level
call, so none of them can straddle a deadline internally.

Measured, not recalled. A 200 000-key MGET takes ~200 ms on the oracle; a shared absolute deadline
was placed 46 ms, 94 ms and 143 ms into that execution:

```
ORACLE frac=0.25 N=200000 mget_duration=195.9ms -> deadline placed +46ms into it
ORACLE frac=0.25  PRESENT 200000/200000   -> SINGLE CUT (all present)   DBSIZE after=200000
ORACLE frac=0.5  N=200000 mget_duration=195.4ms -> deadline placed +94ms into it
ORACLE frac=0.5   PRESENT 200000/200000   -> SINGLE CUT (all present)   DBSIZE after=200000
ORACLE frac=0.75 N=200000 mget_duration=200.0ms -> deadline placed +143ms into it
ORACLE frac=0.75  PRESENT 200000/200000   -> SINGLE CUT (all present)   DBSIZE after=200000

ORACLE neg-control  deadline 93ms BEFORE the command started
ORACLE neg-control  PRESENT 0/200000      -> SINGLE CUT (all absent)    DBSIZE after=0
```

Not one key of 200 000 was hidden or reaped by a deadline crossed mid-command, and the negative
control shows the detector can report the other answer. A moving clock would have produced a
partial count in the three armed rows.

The same holds for a transaction and for a script, with the block stretched by real work
(`DEBUG SLEEP` for the transaction, a Lua busy loop for the script):

```
ORACLE B   MULTI / GET k / DEBUG SLEEP 0.20 / GET k / EXEC, deadline 100ms in -> [v, OK, v]
ORACLE     immediately after EXEC, outside it:  GET k                         -> nil
ORACLE B'  control, deadline an hour out                                      -> [v, OK, v]
ORACLE B'' control, deadline already past at EXEC                             -> [nil, OK, nil]
ORACLE C   EVAL: GET / 317ms busy loop / GET, deadline 158ms in               -> [v, v]
ORACLE     immediately after EVAL:  GET k                                     -> nil
ORACLE C'' control, deadline already past                                     -> [nil, nil]
```

So the reference answer is unambiguous: **the whole operation takes one cut**. A per-owner cut is
not "lazy expiry done differently" — it produces a reply describing a keyspace that existed at no
instant, which lazy expiry on one keyspace can never do.

## The argument, stated once

Two things could have been true, and only one of them is:

1. *Per-owner lazy expiry is legitimate, because redis expires per access.* Rejected. Redis's
   per-access expiry is evaluated against a per-COMMAND snapshot, so all accesses in one command
   agree. The property that survives is not "expiry is lazy" but "one command sees one instant".
2. *A fan-out must take one cut.* Accepted, and it is also the internally consistent answer: this
   tree already pins a *commit* cut for a cross-shard read precisely so the reply cannot straddle a
   foreign write. A deadline is the one mutation with no command behind it, so it needs the same
   treatment through the only channel it has — the clock. The fix is that pairing, not a new
   concept: `ScatterState` already carried a pinned `script_now_ms` for exactly this purpose on the
   script path.

## The defects

### W1. A cross-shard fan-out had a pinned commit cut and no pinned time cut

Each fragment compared deadlines against its own executor's `cached_now_ms_`, refreshed once per
loop pass. The sibling lane's shelved reproducer, re-run here on the unfixed tree:

```
$ python3 tests/edgetime.py 127.0.0.1 7570 repro-hop        # --atomic 1, UNFIXED
geometry: 8 distinct shards [0, 1, 2, 3, 7, 9, 10, 14]
control (fan-out widened 500000us, deadline 1h out): present=8/8 elapsed=0.500s
armed   (fan-out widened 500000us, deadline inside): present=1/8 elapsed=0.500s
        reply=[None, None, None, None, None, None, None, b'v']
natural (no hook, deadline == now): torn 0/200
EDGETIME HOP EXPIRY: REPRODUCED
```

**The shelving argument rested on that last line, and it is wrong.** `deadline == now` makes every
fragment late, so it cannot tear; and 200 attempts cannot see a sub-millisecond window. Placing the
deadline one millisecond *ahead* and running 20 000 attempts reproduces it with **no debug hook
anywhere**, against ordinary pipelined load from a second connection:

`tests/expwide.py <host> <port> repro-natural`, the shipped form of that probe:

| server | armed (D=now+5ms, load) | ctrl-idle (D=now+5ms) | ctrl-far (D=now+1h, load) |
|---|---|---|---|
| tomokv UNFIXED `--atomic 1` | **57 / 6000 torn** | **30 / 6000 torn** | 0 / 1200 |
| tomokv FIXED `--atomic 1` | 0 / 6000 | 0 / 6000 | 0 / 1200 |
| redis 7.4.2 oracle | 0 / 6000 | 0 / 6000 | 0 / 1200 |

```
UNFIXED armed     example torn reply: [v, v, None, v, None, None, None, v]
UNFIXED ctrl-idle example torn reply: [v, v, v, None, v, None, v, None]
```

Every arm reported both all-present and all-absent outcomes (unfixed armed: 946 present, 4997
absent), so the deadline really was being crossed in all 6000 trials of each arm and the detector
was live; the fixed tree simply never observed a torn one. `ctrl-far` is the arm that matters for
attribution: identical load, identical timing, deadline an hour away, zero tears — so the load is
not what tears the reply, the deadline crossing is. An earlier, longer scratch run of the same
shape gave 17/19997 armed and 0/20000 idle on the unfixed tree; the shipped mode's higher rate
comes from its different filler geometry, not from a different mechanism.

That test carries an **arming guard** without which it would prove nothing. `PEXPIREAT` with a
deadline already in the past deletes the key on the spot on both servers, so an unguarded probe
cannot tell a torn read from keys that were never there — an early draft of this probe "reproduced"
tearing on the ORACLE, 95/20000, purely from that. The guard checks the client's own clock after
reading all sixteen arming replies: a reply already read was produced by a command the server had
already run, so a clean guard proves every `PEXPIREAT` ran before the deadline. With the guard the
oracle tears zero times.

The width of the natural window was measured directly. Under a sustained 3 000 000-op pipelined
burst aimed at one owner, an eight-owner MGET's worst latency was 1.27 ms against a 0.042 ms idle
median — so the natural straddle window is O(1 ms), and the tear rate follows from it. It is small
per operation and not small in aggregate: the number of in-flight cross-shard reads straddling any
given deadline instant is (rate x window), which at a million cross-shard reads per second is
thousands.

### W2. A key could expire in the middle of a transaction

`MULTI / MGET k1..k8 / <150 ms of real work> / MGET k1..k8 / EXEC`, deadline placed inside the
block, sixty consecutive transactions on the unfixed tree:

```
tomokv UNFIXED  deadline +12ms: (present_before, present_after) histogram = {(7,0): 34, (8,7): 26}
tomokv UNFIXED  deadline +40ms (outside the block, control): {(8,8): 60}
oracle          deadline +69ms (inside the block):           {(8,8): 60}
oracle          deadline +40ms (control):                    {(8,8): 60}
```

Two identical reads in one transaction disagreed in every single armed run, and the first of them
was itself torn. The oracle's two reads always agree. The brief's other half — a key expiring
*between* queueing and EXEC — matches the oracle on both trees (`[nil]` armed, `[v]` control) and is
not a defect; `NOTES-EDGETIME.md` had that right.

### W3. A cross-shard script compared deadlines against the machine's uptime

Found by reading the code while fixing W1, then reproduced. `ScatterState::script_now_ms` was
`now_ns() / 1000000`, and `now_ns()` is **CLOCK_MONOTONIC**. That value — milliseconds since boot,
order 1e7 — was installed as the owner's `cached_now_ms_` for the script's Read phase, where it is
compared against absolute deadlines of order 1.79e12. Every comparison therefore said "alive":

```
                         geometry          state          DBSIZE   EVAL {GET k1, GET k2}
tomokv UNFIXED --atomic 1  SAME owner       elapsed keys      2      [NIL, NIL]     <- correct
tomokv UNFIXED --atomic 1  TWO owners       elapsed keys      2      [v1, v2]       <- wrong
redis 7.4.2 oracle         one keyspace     elapsed keys      2      [NIL, NIL]
tomokv UNFIXED --atomic 1  TWO owners       live keys (ctl)   2      [v1, v2]
```

Deterministic and total, not a race: a cross-shard script could never observe any key as expired,
and could never reap one. The single-owner geometry on the same server and the same script is the
control that makes it attributable.

---

## The fix

One wall-clock cut per logical operation, stamped where the operation's program order already
stamps its commit cut, and installed on every owner before its fragment runs.

| file | what |
|---|---|
| `src/core/signal.h` | `now_realtime_ms()` — the wall clock, with a comment saying why `now_ns()` (CLOCK_MONOTONIC) must never reach a deadline comparison |
| `src/core/shard.h` | `Shard::pin_now_ms()` (installs a cut WITHOUT resetting the LRU clock, which `set_cached_now_ms`'s defaulted parameter would have done) and `PinnedNowMs`, the RAII guard that restores the executor's own per-pass clock on every exit |
| `src/cmd/scatter_engine.inc` | `ScatterState::now_cut_ms` replaces the script-only `script_now_ms`; stamped for **every** cross-shard group at prepare; installed in `xshard_execute`, which is the single entry point every fragment of every scatter kind passes through; the script workbench shard is pinned explicitly because it is a shard object the guard does not cover |
| `src/cmd/multi.inc` | `MultiExecState::now_cut_ms`, stamped before any fragment is posted and inherited by the transaction's lowered cross-shard children; installed in `multi_execute_task`, ahead of the WATCH phase (redis validates watches inside EXEC's own call) |
| `tests/expwide.py` | the 83-check battery plus the `repro-natural` mode |
| `tests/gate.sh` | `expwide` in the feature-battery loop; expected-check ledger 194 -> 196 |

86 lines added, 5 removed, across four source files. No new knob and no new source file, so
`tomokv.conf` and the Makefile are unchanged.

**Blast radius.** This does not change the MVCC resolver, the scatter engine's dispatch or gather,
or the single-owner law. It changes which *instant* a fragment's existing `live_or_expire`
comparison uses, and nothing else. No new knob: this is redis-parity behaviour, and the
knob-philosophy rule applies to features, not to which clock a comparison reads.

**Cost.** One `clock_gettime(CLOCK_REALTIME)` per cross-shard group prepare and per EXEC, and two
stores in / two stores out per fragment. `xshard_execute` is never called on a single-shard command,
so the GET/SET hot path executes byte-identical code; the A/B below is the check on that claim
rather than the argument for it.

**What the pin deliberately does NOT do.** A group that retries across owner passes keeps its
original cut, so a key can be reported alive for the length of a retry loop after its deadline.
That is the same exposure the pinned MVCC read cut already carries and it is the redis answer as
well (a long command holds its snapshot). The reverse risk — a relative deadline set inside a
transaction being measured from the pinned instant rather than from wall-clock now — is real,
is what redis does for the same reason, and is pinned by an explicit check (S3, "a relative
deadline set inside a transaction is measured from its one instant", asserts exactly `60000`).

---

## Test evidence

### The battery: `tests/expwide.py`

Five sections, 81 checks. It runs **unchanged against vanilla redis** — the sections that need
TomoKV's own DEBUG hooks announce themselves as skipped — so the expectations are not this lane's
opinion of correct, they are the oracle's answers.

* **S1** bare cross-shard fan-out (MGET, EXISTS) widened by `DEBUG ATOMIC-FANOUT-DEFER`, with the
  hook's own measured elapsed time asserted so the window provably opened.
* **S2** the whole multi-key family across a deadline inside a transaction, with the window made of
  **real work** (a BITCOUNT over 32 MB queued on *every* participating owner) rather than a hook —
  which is what reaches the write half of the family, since the fan-out hook arms reads only.
  MGET, EXISTS, TOUCH, DEL, UNLINK, MSETNX, MSET-then-MGET, RENAME, SINTERSTORE, SUNIONSTORE,
  SDIFFSTORE; COPY and RENAMENX bare (see the recorded gap below).
* **S3** two identical reads either side of the deadline must agree; plus the relative-deadline
  check described above.
* **S4** the cross-shard script clock, with the single-owner geometry as the control.
* **S5** the cut has not become a licence to ignore deadlines: an ordinary cross-shard MGET over
  elapsed keys still hides all eight and moves `expired_keys` by exactly eight, and the same
  counter reports zero for TTL-free keys.

Every armed check is bracketed by a far-deadline control and an already-elapsed control, so no
section can pass because deadlines are ignored or because everything looks dead.

Keeping the window made of real work honest needed one non-obvious property: TomoKV's transactions
are ordered **per owner and not across owners**, so an owner with nothing else queued runs the
command under test immediately — before the deadline — and reports the pinned answer for the wrong
reason. The first draft of S2 passed on the *unfixed* tree for exactly that reason. The committed
version queues a BITCOUNT on every participating owner, and the fail-before numbers below are what
that changed.

```
                                          checks  failures
redis 7.4.2 oracle                          70       0   -> PASS
tomokv UNFIXED  --atomic 1                  83      10   -> FAIL
tomokv UNFIXED  --atomic 0                  83      10   -> FAIL
tomokv FIXED    --atomic 1                  83       0   -> PASS
tomokv FIXED    --atomic 0                  83       0   -> PASS
```

(The oracle runs fewer checks because S1 is skipped on a server with one keyspace.)

Unfixed failures, `--atomic 1`:

```
  FAIL S1 MGET across a deadline inside the fan-out: got [v,None,None,None,None,None,None,None]
  FAIL S1 EXISTS across a deadline inside the fan-out: got 1, want 8
  FAIL S2 MGET across a deadline inside the transaction: got [v,v,v,v,v,v,None,v]
  FAIL S2 EXISTS across a deadline inside the transaction: got 7, want 8
  FAIL S2 TOUCH across a deadline inside the transaction: got 7, want 8
  FAIL S2 DEL across a deadline inside the transaction: got 7, want 8
  FAIL S2 UNLINK across a deadline inside the transaction: got 7, want 8
  FAIL S2 RENAME across a deadline inside the transaction moved the value: got None, want b'v'
  FAIL S3 two reads either side of the deadline agree: got (8, 6), want (8, 8)
  FAIL S4 cross-shard script hides an elapsed key: got [v, v], want [NIL, NIL]
```

`--atomic 0` fails the same ten with SINTERSTORE in place of the S3 row. Both lists are stable
across runs; the fixed tree is 0 failures across every run taken.

### The gate row

`expwide` is appended to `tests/gate.sh`'s feature-battery loop (one row per atomic mode) and the
expected-check ledger goes 194 -> 196 with the reason recorded next to the others, as that ledger's
comment requires. The gate itself was **not run** — it owns port 7899 and cores 0-7, reserved for
the mainline operator — so the row was validated by reproducing the gate's exact boot geometry
(8 cores, `--shards 16 --ratio 6:2`, `--enable-debug-command yes`) on this lane's own cores:

```
                              --atomic 0            --atomic 1
tomokv FIXED     83 checks,  0 failures PASS   83 checks,  0 failures PASS    (~49s per mode)
tomokv UNFIXED   83 checks, 13 failures FAIL   83 checks, 11 failures FAIL
```

The battery needs nothing the feature loop's boot does not already provide, leaves active expiry
back on and the fan-out hook disarmed in its `finally`, and is appended last so its closing
`FLUSHALL` cannot disturb another battery.

### The sibling lane's shelved reproducer, after

```
$ python3 tests/edgetime.py 127.0.0.1 7570 repro-hop        # --atomic 1, FIXED
geometry: 8 distinct shards [2, 4, 5, 7, 9, 10, 12, 13]
control (fan-out widened 500000us, deadline 1h out): present=8/8 elapsed=0.500s
armed   (fan-out widened 500000us, deadline inside): present=8/8 elapsed=0.500s
natural (no hook, deadline == now): torn 0/200
EDGETIME HOP EXPIRY: NOT REPRODUCED
```

The control still shows the hook widening the fan-out to half a second, so the row did not start
passing because the window stopped opening.

### ASAN

Built with the `tests/gate.sh` section-1 line and `ldd`-confirmed to link `libasan.so.8` into the
binary that actually ran:

```
--atomic 1   expwide 83 checks 0 failures PASS | edgetime 155/0 PASS | multi_exec, execiso,
             execatomic, xscript, scriptatomic PASS | differ multi 4260 ops 0 diffs |
             differ xshard 4276 ops 0 diffs | sanitizer reports in the server log: 0
--atomic 0   identical, all PASS, 0 sanitizer reports
```

### Blast radius: batteries and differ, both atomic modes

74 legs, `--atomic 1` and `--atomic 0`, target on cores 80-91 against the pinned oracle:

```
batteries (each mode): atomic_torn atomfix atomic_ryow ryow concur execatomic execiso multi_exec
                       blockmulti multires scriptatomic xscript hexpire edgetime session_monotonic
                       debug xacct xmove expwide                             ALL PASS
differ 3 seeds  (7,19,23): multi  xshard  edgetime
differ 1 seed   (7):       string hash set scan script hexpire zsetops stream
```

Everything passes except two rows, and both were run against the **pre-fix binary** on the same
box, same oracle, to place them:

* `differ multi seed=19` — an *intermittent* divergence present identically on the unmodified
  mainline binary. Three runs each: FIXED gave 0, 2, 3 diffs; BASELINE gave 2, 0, 2 diffs, at the
  same ops (`op 924 EXEC`, `op 933 DEL`) with the same bytes. `gen_multi` sets no deadline on any
  key, so this lane's change cannot reach it.
* `differ multi seed=23` — a *deterministic* 1-diff at `op 2934 MGET`, byte-identical on the
  baseline and on the fixed tree across repeated runs. Also TTL-free, also pre-existing.

`tests/expireindex.py` needs a `--shards 1` boot (it says so, and it refuses otherwise); run that
way it is `EXPIREINDEX PASS` on both the baseline and the fixed binary. The `FAIL 1` seen in the
first sweep was this lane's driver booting 16 shards.

### Blast radius: interleaved A/B

Server on cores 80-87 (`--shards 16 --ratio 4:4`), load on 88-95, six rounds with the arm order
alternating each round, 12s per measurement after a populate pass and a 4s warm-up. INDICATIVE
(loopback, not the NIC rig).

| workload | arm | median ops/s | mean ops/s | stdev | FIX vs BASE (median / mean) |
|---|---|---|---|---|---|
| GET/SET p32, ratio 1:10 | BASE | 4 271 524 | 4 268 012 | 10 277 | |
| | FIX | 4 269 685 | 4 267 893 | 14 997 | **−0.04% / −0.00%** |
| 8-key MGET p32 | BASE | 1 079 139 | 1 077 548 | 6 182 | |
| | FIX | 1 080 588 | 1 079 986 | 4 232 | **+0.13% / +0.23%** |

The hot path is unchanged to three decimal places, which is what the code predicts: a single-shard
GET or SET never enters `xshard_execute`. The cross-shard arm — the only path that pays — is inside
noise too. That arm is a genuine fan-out: memtier substitutes a DISTINCT random key for each
`__key__`, confirmed on the wire with `MONITOR`:

```
"MGET" "memtier-89695" "memtier-66983" "memtier-65096" "memtier-14351" "memtier-75674" ...
```

A first attempt at this arm used a Python pipelined loadgen and produced +-13% round-to-round; it is
recorded here only to say why it was replaced rather than averaged.

## Recorded, out of scope, pre-existing

Two MULTI-lowering divergences surfaced while building S2. Both were confirmed on the **pre-fix**
binary, both are about how a cross-shard two-hop write is lowered into a transaction rather than
about expiry, and neither is touched by this lane:

* a cross-shard `COPY` or `RENAMENX` inside `MULTI` is refused at dispatch with `EXECABORT`
  (the oracle runs them and returns `0`);
* a cross-shard `RENAME` inside `MULTI` whose source does not exist answers `EXECABORT` where the
  oracle answers `ERR no such key` inside the EXEC array.

`tests/expwide.py` prints both as notes with the live answer rather than asserting them, so a lane
that fixes the lowering will see the note change and can move the two commands into the family
loop; and it checks the two commands bare so their expiry behaviour is still covered.

Also observed and not filed: `TIME` inside a TomoKV transaction returns the same value for every
occurrence regardless of how much work sits between them. Unrelated to expiry, and it is what led
to the per-owner ordering property documented above.

## Housekeeping

The pinned redis oracle was terminated three times during this lane with no entry in its own log
and no kernel OOM record — the signature of another lane selecting processes by name. Every oracle
measurement here was taken after re-booting it and re-confirming the listening pid.
