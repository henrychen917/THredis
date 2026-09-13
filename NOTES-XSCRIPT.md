# Cross-shard script lane (`t-xscript`)

## Status

| | |
|---|---|
| **Stage 0** | Shipped (AOF groups for script writes, WATCH narrowed to declared KEYS, cross-owner differ generator) |
| **Stage 1** | **Built and gated** under AMENDMENT 1 — EVAL/EVALSHA/EVAL_RO/EVALSHA_RO/FCALL/FCALL_RO with N declared keys over M owners |
| **Stage 2** (EVAL inside MULTI) | Shelved behind Stage 1, still refused with today's CROSSSLOT element, asserted by the battery |
| **Stage 3** (hash-tag co-location) | Shelved |
| **Benches** | NOT run — see "What was not done" |

Any EVAL/EVALSHA/FCALL whose declared KEYS span owners now executes. Its writes land on their real
owners under one commit ticket, a concurrent reader sees all of them or none, a following command on
the same connection sees them, they survive an unclean stop through the AOF, and the reply bytes
match redis 7.4 over 11 randomized 5,000-op differ streams. The refusal that made the feature
"might as well not exist" is gone; `script-crossshard-max-bytes 0` restores it byte-for-byte.

---

## 1. The protocol, as built

Six waves over the existing two-hop scatter engine as `Kind::Script`. `xshard_complete_script()`
in `src/cmd/scatter_engine.inc` is the driver.

| Wave | Where | What it does |
|---|---|---|
| **PIN** | every declared key's owner | `atomic_script_pin()` installs an owner-local `AtomicScriptIntent`. **Nothing has been read yet and no cut exists yet.** |
| — | driver | all owners confirmed armed ⇒ **now** choose the cut, `server.atomic_snapshot()` |
| **READ** | every declared key's owner | read at the cut under the connection's RYOW overlay, serialize an `ObjectImage` into the group arena, accumulate staged bytes |
| **RUN** | coordinator = owner of `KEYS[1]` | load every image into a private thread-local workbench `Shard`, run the whole activation there with the 71 whitelisted handlers unmodified, record per-key read/write flags |
| **VALIDATE** | owners of keys the activation READ | any version newer than the cut ⇒ conflict ⇒ discard everything and restart from a fresh cut |
| **UNPIN** | every declared key's owner | drop the intents |
| **APPLY** | owners of keys the activation WROTE | install post-images, one `atomic_commit_group` ticket, one AOF group |

**The PIN wave is AMENDMENT 1's owner reservation sub-wave** and it is the whole point: `find()` at a
cut can only reconstruct a predecessor that some record preserved, and an ordinary key has no record.
Arming the key first is what forces a competing plain write to leave one.

---

## 2. What was inherited, and the three defects found in it

The working tree at handoff already contained a Stage 1 engine implementing the amended shape
(committed unmodified as `e2aaa6dc3` so the review diff for the fixes is exact). It built, and its
directed battery passed. Three defects were found underneath that.

### 2.1 The reservation armed nothing (`32dc59dd6`) — the amendment's own hole, still open

`begin_plain_version()` decided whether to materialize an MVCC version with `atomic_has_record()`
alone. A reserved ordinary key carries **no record**, so:

```
xshard_plain_prepare:  atomic_has_record(k) || atomic_has_script_intent(k)   -> proceed   ✔
begin_plain_version:   if (!atomic_has_record(k)) return WorkError::None;    -> BAIL OUT  ✘
```

Every call site the previous lead made intent-aware then called into a function that was not. A plain
`SET` landing between the cut and an owner's gather took the untracked physical path, drew no ticket,
and left validation nothing to find — **the exact deterministic hole AMENDMENT 1 describes, in a tree
that looked like it had closed it.** It is invisible from outside: same replies, same phase counters,
same commit.

Fixed by making `FlatStore::atomic_needs_version()` the single decision point for "this write must go
through MVCC" and routing every caller through it (`xshard_plain_prepare` both arms, the four scatter
phase-2 arms, blocking pops, `begin_plain_version` itself). Eviction victim selection asks it too: a
reserved key is not a legal victim, because the predecessor is still being resolved against.

Also in that commit: `atomic_script_changed()` dropped its `epoch < reserved_ticket` clause. A writer
whose ticket is newer still **read the pre-activation value** — the script has installed nothing at
validation time — so ordering it after the activation is sound only for a blind write and silently
loses a plain read-modify-write. Restart is unobservable and a fresh cut clears the conflict, so the
conservative test costs at most one retry and cannot livelock.

### 2.2 Two executors published the same wave (`5d42f9a1a`) — a live crash

`script_post_phase()` set a plain `bool script_publish_pending` at entry and cleared it *after* its
posting loop. Owner tasks become visible one participant at a time, so a task from the first
participant could finish, re-enter the driver, still see the flag set, and **re-publish the wave the
poster was mid-way through publishing**. Two executors then ran `reset_groups` + `build_groups` on one
arena concurrently.

Not theoretical: the first `tests/differ.py script` run killed the server. Core dump, four threads:

```
Thread 5  build_groups(count=8) <- script_post_phase <- xshard_complete_script
Thread 3  build_groups(count=8) <- script_post_phase <- xshard_complete_script
Thread 4  Shard::set_cached_now_ms(this=0x0) <- ExLoop::execute
Thread 2  Shard::set_cached_now_ms(this=0x0) <- ExLoop::execute
```

It had never been caught because nothing had ever run a cross-owner script stream against the engine.

`pending` now counts one reference per posted task **plus one held by the poster**, released only
after publication completes; whoever takes it to zero owns the next wave. The QueueFull path keeps
that reference across the requeue, which is also what makes resuming an interrupted publication safe.
The driver became a loop rather than recursing, so an OCC retry storm no longer recurses per restart.

### 2.3 The single-owner differ control could not be run (`2ca1e4f89`)

A one-shard boot aborted the script leg outright, so the control the Stage 1 gate asks for did not
exist. It is now the control it was meant to be. A stream that produces no proven cross-owner
activation on a *multi*-owner boot is still a hard error — that one really is a blind generator.

---

## 3. Proof that the reservation armed — the counters

The amendment is explicit that this failure is invisible without a counter, so three were added
(`INFO STATS`, all gate-asserted):

| Counter | Meaning |
|---|---|
| `script_keys_armed` | one per declared key per owner PIN task |
| `script_keys_released` | one per UNPIN; `armed - released` must be 0 at rest |
| `script_intents_live` | that difference, published directly |
| **`script_write_tickets_forced`** | **a plain write that would have taken the untracked physical path and materialized an MVCC version BECAUSE the key was reserved** |

The last one is the load-bearing one: `script_keys_armed` proves the reservation *happened*,
`script_write_tickets_forced` proves it *did something*. Defect 2.1 would have left the first
counter healthy and the second at zero forever.

The differ leg asserts `script_keys_armed == script_keys_released` and `script_intents_live == 0`
over each 5,000-op stream (15,192 keys armed on the atomic-0 run, 15,281 on atomic-1, 0 live).

---

## 4. The directed counterexample

`tests/xscript.py <host> <port> reserve`. `DEBUG SCRIPT-STAGE-DEFER <us>` parks every gather task
except the coordinator's own, **after** every key is reserved and the cut is chosen — that park is
precisely the interval AMENDMENT 1 is about. The racing plain writes are issued only once
`script_stage_owner_tasks` proves the coordinator's own gather completed (counter-polled, never
slept), so the two declared keys are necessarily read on opposite sides of the write.

Three independent things must then hold, and each is zero without the reservation.

### Passes with (`build/tomokv`)

```
  ok   zero control: uncontended activation reserves both keys and retries nothing reply=[b'pre', b'pre'] armed=+2 forced=+0 retries=+0 live=0
  ok   counterexample harness ran (park armed, racing writes accepted) armed=True alive=False raced=[b'OK', b'OK'] gathered=3
  ok   plain write into the reservation window is forced through MVCC forced=+2
  ok   that write is DETECTED by read-set validation retries=+1 giveups=+0
  ok   the activation observed a cut, not one key from each side of the write [b'post', b'post']
  ok   the racing writes survive and the activation composes on top of them [b'post/post', b'post']
  ok   every reservation was released armed=4 released=4
```

### Fails without (`make noreserve` ⇒ `build/tomokv-noreserve`, identical source except that `ScriptPhase::Pin` arms nothing)

```
  FAIL zero control: uncontended activation reserves both keys and retries nothing reply=[b'pre', b'pre'] armed=+0 forced=+0 retries=+0 live=0
  ok   counterexample harness ran (park armed, racing writes accepted) armed=True alive=False raced=[b'OK', b'OK'] gathered=3
  FAIL plain write into the reservation window is forced through MVCC forced=+0
  FAIL that write is DETECTED by read-set validation retries=+0 giveups=+0
  FAIL the activation observed a cut, not one key from each side of the write [b'pre', b'post']
  FAIL the racing writes survive and the activation composes on top of them [b'pre/post', b'post']
  ok   every reservation was released armed=0 released=0
  FAIL reservation released after: script raised a runtime error fired=True live=0 armed=+0 (want +2)
  FAIL reservation released after: script hit the instruction limit fired=True live=0 armed=+0 (want +2)
  FAIL reservation released after: script touched an undeclared key fired=True live=0 armed=+0 (want +2)
  ok   reservation released after: script failed to compile (refused before dispatch) fired=True live=0 armed=+0 (want +0)
  FAIL reservation released after: client vanished mid-activation fired=True live=0 armed=+0 (want +2)
9 xscript checks failed
```

`[b'pre', b'post']` is the failure in one line: the activation read `KEYS[1]` from before the racing
write and `KEYS[2]` from after it, then committed `pre/post` — a value that existed at no cut, on a
build whose phase counters and reply shape are otherwise indistinguishable. With the reservation it
reads `[post, post]` and commits `post/post`.

The negative control is a **build**, not a runtime switch, deliberately: a knob that can disable a
correctness mechanism does not belong in the shipping binary.

---

## 5. Release on every exit path

An armed key whose activation died would arm every later write to it for the life of the process. The
release wave is posted from every terminal arm of the driver; the battery drives each way to die and
asserts both that the arm actually reserved something and that nothing is left live.

```
  ok   reservation released after: script raised a runtime error fired=True live=0 armed=+2 (want +2)
  ok   reservation released after: script hit the instruction limit fired=True live=0 armed=+2 (want +2)
  ok   reservation released after: script touched an undeclared key fired=True live=0 armed=+2 (want +2)
  ok   reservation released after: script failed to compile (refused before dispatch) fired=True live=0 armed=+0 (want +0)
  ok   reservation released after: client vanished mid-activation fired=True live=0 armed=+2 (want +2)
  ok   armed == released across every exit path armed=12 released=12
  ok   a released key stops arming later plain writes forced=+0
```

Each expected arm count is stated **exactly**, not as a floor: a compile failure is refused at route
time before the group is admitted and must reserve nothing, and an arm that quietly stopped reserving
is the failure this function exists to catch. The connection-drop case aborts the socket with
`SO_LINGER 0` while the activation is held open by the park hook, so the client is genuinely gone
while its declared keys are reserved on their owners.

Release is by design **not** tied to retirement: retirement runs on the IO thread, which may not touch
an owner's shard. Every path through the driver therefore reaches the UNPIN wave, and the
`armed == released` invariant is what proves the enumeration is complete rather than merely plausible.

Verified from code (not inferred): a script group is never destroyed with pins live — `xshard_destroy`
is reachable only before the first task is posted (`io_loop.h:1148/1194`, the `xshard_prepare` error
arms) or from `xshard_retire` after the driver returned `Final`.

---

## 6. Zero-cost when off, measured

Address-normalized `objdump` of `build/tomokv` at HEAD against a from-scratch build of the pre-lane
base `dbef14d43`:

| Symbol set | Result |
|---|---|
| 48 `cmd_get*` / `cmd_set*` / `cmd_incr*` / `cmd_append*` instantiations | **47 byte-identical.** `cmd_getex<false>` differs only by two 16-bit constant stores widened to 32-bit plus nop padding — one instruction *fewer* |
| `IoLoop::run_loop` (all four instantiations) | **byte-identical** |
| `ExLoop::execute` | +3 of 459 |

The +3 breaks down as: `atomic_has_records()` now tests `script_intent_count` as well as `live`, and
because those two `uint32_t` were deliberately placed adjacent (`0e3ba6abd`) GCC folds both into **one
8-byte `cmpq $0`** — the reservation test is free. The genuinely new instructions are a `cmp`/`je`
pair for `ScatterFinish::Retry`, on the `t.scatter` arm, which a plain GET/SET never enters.

---

## 7. Knobs

All in `src/core/config.h`, one parser for CLI and conf, documented in `tomokv.conf`. Redis and
Dragonfly have no equivalent (Dragonfly uses per-script shebang flags, which we deliberately do not
adopt), so these stay tomo-native names.

| Knob | Default | Semantics |
|---|---|---|
| `script-crossshard-max-bytes` | `-1` = auto: `max(4 MiB, min(maxmemory/shards/16, 64 MiB))` | Bytes gathered into one activation. **`0` = feature off: today's CROSSSLOT verbatim, no workbench and no intent state ever allocated** |
| `script-crossshard-workbench-bytes` | `-1` = auto: `2 x` the staging budget | Bytes the activation may produce. Exceeding it raises the redis OOM error |
| `script-crossshard-conflict-retries` | `-1` = auto (8) | OCC restarts before `-TRYAGAIN`. Never disables validation |
| `script-crossshard-cut-slots` | `-1` = auto (4) | Cut registrations scripts may hold per IO thread, so they cannot starve MGET/MSET/EXEC |

Counters in `INFO STATS`: `script_stage_owner_tasks`, `script_run_attempts`,
`script_validate_owner_tasks`, `script_apply_owner_tasks`, `script_crossshard_activations`,
`script_group_commits`, `script_group_occ_retries`, `script_group_occ_giveups`,
`script_staged_bytes_total`, `script_crossshard_window_refusals`, `script_group_aborts_oom`,
`script_keys_armed`, `script_keys_released`, `script_intents_live`, `script_write_tickets_forced`.

Test surface: `DEBUG SCRIPT-STAGE-DEFER <microseconds>` (needs `enable-debug-command`), zero in
production, read once per activation on the cold scatter path.

---

## 8. Refusals

| Case | Reply |
|---|---|
| Key not in declared KEYS | `ERR Script attempted to access an undeclared key` (unchanged; matches Dragonfly, diverges from vanilla standalone — pre-existing) |
| Staged bytes over the cap | `ERR cross-shard script staging limit exceeded` |
| Workbench bytes over the cap, or APPLY under maxmemory pressure | `OOM command not allowed when used memory > 'maxmemory'.` |
| OCC give-up after the retry budget | `TRYAGAIN Cross-shard script conflicted, retry` |
| Cut-slot window full on this IO thread | `BUSY Cross-shard script snapshot window is full` |
| `script-crossshard-max-bytes 0` | `CROSSSLOT Keys in request don't hash to the same slot` — byte-identical to today |
| Cross-owner script inside MULTI (Stage 1) | today's prebuilt CROSSSLOT array element |

---

## 9. Test evidence

Server cores 8-15, ports 7110-7119 only; every server `taskset`ed and stopped by the pid resolved
from its listening socket. Oracle: vanilla redis 7.4 on 7111, same cores.

### Directed battery — `tests/xscript.py HOST PORT {all,reserve,off,limit,window,stage0}`

`all` (17 checks) passes on `--shards 8` boots under **both** `--atomic 0` and `--atomic 1`:

```
XSCRIPT all directed battery passed          (atomic 0)
XSCRIPT all directed battery passed          (atomic 1)
```

Separate boots for the control modes:

```
--script-crossshard-max-bytes 0      XSCRIPT off directed battery passed
    ok   feature-off cross-owner refusal is byte-exact RespError("CROSSSLOT Keys in request don't hash to the same slot")
    ok   feature-off control allocates/executes no cross engine {...'script_keys_armed': 0, 'script_write_tickets_forced': 0}
--script-crossshard-max-bytes 300    XSCRIPT limit directed battery passed
--script-crossshard-cut-slots 1      XSCRIPT window directed battery passed
    ok   zero control: unheld activations are never window-refused refusals=+0 bad=[]
    ok   cut-slot window refusal fires and is counted busy=21 counted=+21
    ok   refusal is a reply, not a stalled connection or a wrong answer ok=3 busy=21 other=[]
```

Every arm carries a control that must report zero, per the vacuous-validation rule: the window test's
control is the same 24 activations issued one at a time (0 refusals), the counterexample's control is
the same script with no racing write (0 forced, 0 retries), and the feature-off control asserts the
whole cross engine stayed idle.

### Differ vs redis 7.4 — `tests/differ.py 127.0.0.1 <port> 127.0.0.1 7111 script [seed]`

11 seeds x ~5,060 ops = **~55,700 ops, 0 diffs**, across `--atomic 0` and `--atomic 1` 8-shard boots.
Representative tail:

```
  script mechanism generated_cross=3249 deltas={'script_stage_owner_tasks': 11985, 'script_run_attempts': 3160,
    'script_validate_owner_tasks': 655, 'script_apply_owner_tasks': 576, 'script_crossshard_activations': 3160,
    'script_group_commits': 576, 'script_staged_bytes_total': 767322, 'script_keys_armed': 15281} live=0
DIFFER script: 5061 ops, 0 diffs -> PASS
```

Single-owner control, same stream on `--shards 1`:

```
  script mechanism SINGLE-OWNER CONTROL: cross engine idle, deltas all zero
DIFFER script: 5061 ops, 0 diffs -> PASS
```

The Stage 0 baseline on this same generator produced **3,305 CROSSSLOT diffs over 5,061 ops**, so the
gate is not vacuous.

### AOF process-restart over cross-owner activations

`tests/aof.py` populate → `DEBUG LOADAOF` → SIGKILL → reboot → verify, on both `--atomic 0` and
`--atomic 1`. `find_distinct_shard_keys` uses `DEBUG SHARD`, so the multi-key EVAL and the
failed-prefix EVAL are proven cross-owner, and both assert their post-images share **one** group
ticket.

```
atomic=0   populate+LOADAOF ok   process-restart replay ok
atomic=1   populate+LOADAOF ok   process-restart replay ok
```

### Neighbouring batteries (shared code was touched: `begin_plain_version`, eviction, blocking pops)

`--atomic 0` and `--atomic 1`, 8 shards: `torture`, `ryow`, `multi_exec`, `lua_scripting`,
`scriptsurf`, `blocking`, `limits`, `notify`, `scriptatomic`, `execatomic`, `session_monotonic`,
`atomfix`, `atomic_torn`, `atomic_ryow` — **all PASS**.

`evict_battery` sections `noev`/`lru`/`vlru`/`lfu` pass at the battery's designed `--shards 16`. At
`--shards 8` its `hot survives >= cold` LRU-sampling check reads 20 vs 21; that is a shard-count
geometry artifact of running the battery off-design (`MM` is documented as "1MB / 16 shards"), not a
lane regression — the run contained no scripts at all, and `atomic_needs_version` is bit-identical to
the old `atomic_has_record` when no intent exists.

`tests/config_parser_test.cc` passes, including the four new knobs' grammar and their four rejection
cases.

Neighbouring differ suites, same boot and oracle, all **0 diffs** — `xshard` (4276 ops) is the one
that most directly exercises the `begin_plain_version` change: `string` 4033, `list` 3521, `set`
3524, `hash` 3545, `zset` 3531, `cgaps` 3310, `xshard` 4276. `tests/debug.py` passes.

### ASAN + UBSAN

`make asan` (`-fsanitize=address,undefined`), 8-shard `--atomic 1` boot on the lane cores:
`tests/xscript.py all` passed (with 172 OCC restarts forced by the contention arm — ASAN's slowdown
widens every window this feature has), then two differ legs, 5061 and 5053 ops, **0 diffs**.

```
hard ASAN errors: 0
  1 runtime error: load of misaligned address 0xADDR for type 'const uint32_t', which requires 4 byte alignment
      third_party/lua/lstring.c:87 murmur32  <- luaS_newlstr <- lua_pushlstring <- push_call_tables
```

The single UBSan report is the vendored Lua 5.1 string hash reading unaligned words. It fires on any
EVAL at all, single-owner included, and `git diff dbef14d43..HEAD -- third_party/` is empty — this
lane did not touch Lua. Pre-existing, reported here rather than filtered away.

---

## 10. What is knowingly left open

1. **APPLY-window plain writes.** UNPIN runs before APPLY, so a plain write landing between them is
   unarmed. This is deliberate and inherited, not an oversight: after APPLY the installed MVCC
   records point into the group's arena arrays, and running UNPIN afterwards would rebuild
   `state.groups` / `key_order` underneath them. A blind write in that window is legally superseded
   by the activation (serial order write→script). A plain **read-modify-write** in that window can be
   lost — but that is the same behaviour any cross-shard atomic group has had against a racing plain
   RMW since the engine shipped, because no group holds records on a participant between its read
   phase and its apply phase either. Narrowed, not widened, and not new.
2. **APPLY is globally serialized.** `script_try_certification()` is a single process-wide flag:
   activations STAGE and RUN concurrently but only one may hold a reservation through APPLY, so a
   loser re-stages. It is what keeps two activations from validating the same predecessor before
   either installs. Correct but a throughput ceiling; the obvious replacement is per-key or per-owner
   certification, which is Stage 1.5 work and wants the contention bench first.
3. **A15** — a group wholly on one side of the BGSAVE cut — remains the inherited open issue,
   explicitly not widened.
4. **A live reservation promotes every cross-shard group server-wide.**
   `script_intents_active()` is a global flag, so while any activation holds intents, an MSET or
   DEL on shards the script never named is still routed as an atomic group. Correct (it can only
   add ordering, never remove it) and it is what makes the plain-MSET-vs-script arm of the battery
   hold on an `--atomic 0` boot, but it is broader than it needs to be. Per-owner scoping is the
   obvious narrowing and wants the interference cell before it is worth doing.
5. **`script_cut_held_us_p99/max`** from the proposal's §6 counter list were not added; the cut-slot
   refusal counter and its directed test cover the starvation hazard those were for.

## 11. What was not done, and why

**No benches.** The proposal's §7 asks for cells 0-7 with a PRE/POST table. This lane was scoped to
correctness and reviewability, the box is shared with other lanes, and the one-server-one-bench rule
makes a trustworthy cell impossible while other lanes are running. What *is* measured here is the
zero-regression question that gates everything else, and it is measured at the instruction level
rather than through box noise (section 6): the plain GET/SET path is byte-identical to the pre-lane
base. **The cost of an activation itself is unmeasured**, so cells 1-7 — single-owner control,
nested-call flatness, key/owner sweep, value-size knee, the deciding contention cell, and the
interference cell — remain owed before this can carry a performance claim. Cell 5 in particular is
the one that decides whether item 10.2 above has to be fixed before this ships broadly.

## 12. Resource hygiene

Cores 8-15 and ports 7110-7112 only. Every server started with `taskset -c 8-15`; every stop resolved
the pid from `ss -lntpH | grep ':<port> '` and signalled that exact pid, then confirmed the listener
was released before the next boot. No process was ever selected by name or pattern. Nothing was
pushed. `tests/gate.sh` was not run (it owns port 7899 and cores 0-7), and no wire/NIC bench was run.
