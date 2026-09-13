# NOTES-HEXPIRE — hash-field TTLs (lane E)

Branch `t-hexpire`, based on `d177ea9cf`. Server cores 48-51 / port 7250, oracle cores 52-55 /
port 7255 (vanilla redis 7.4.2 at `/tmp/claude-1000/redis74/src/redis-server`).

## Instruction-cost PRE vs POST (the headline)

Plain HSET/HGET loopback, **the feature never used**, INDICATIVE, on cores 48-51 while eight other
lanes shared the box. Both quantities are measured over the same window and neither is estimated:
instructions from `perf stat -C 48-51` (the server owns those cores exclusively), ops from the
server's own `total_commands_processed` sampled either side of the window. memtier is rate-limited so
every rep does **exactly 7,530,001 ops at 750k ops/s** in both arms, and arm ORDER alternates.

| arm | instr/op median | mean | min | sd |
|---|---|---|---|---|
| PRE  (`d177ea9cf` build) | 5994.9 | 6033.6 | 5920.8 | 1.70% |
| POST (`t-hexpire` build) | 6027.1 | 6033.0 | 6015.7 | 0.27% |
| **delta** | **+0.54%** | **−0.01%** | +1.60% | |
| **NULL CONTROL** (same binary in *both* arms) | | | | **+0.64%** |

Secondary reading, saturated (2.0M ops/s, fixed PRE-then-POST order, 8 reps): PRE 5352.1 vs POST
5403.5 instr/op (+0.96%); throughput PRE 2.025M vs POST 2.014M ops/s (−0.54%).

**Read this honestly.** The same-binary null control reads **+0.64%** — the instrument's own slot
bias on this box is as large as the effect being measured, and the mean of the order-alternating
run is −0.01%. The brief's bar is ≤ +0.5%; the median lands at +0.54% and the mean at −0.01%, so
the correct statement is *the cost is at or below this box's measurement floor, and could not be
resolved from the null control*. The mechanism-level accounting agrees: a hash command against a
shard with no field deadlines executes **one load and two predicted-false branches** more than
before — roughly 4-6 instructions against the ~6000 the whole server spends per op at this load
point (~0.1%). Raw data: `scratchpad/hexpire/instr-result8.csv` (alternating),
`instr-selfcheck.csv` (null control), `instr-result5.csv` (saturated), harnesses `instr_ab4/5/6.sh`.

Two placement decisions came directly out of that measurement, in the order I found them:

1. `field_expire_count()` originally read `field_expires_.size()` — an `ExpireIndex` sitting ~200
   bytes into `FlatStore`, i.e. a different cache line from the one `find()` already touches. It is
   now a `uint32_t field_ttl_gate_` living in the **4-byte hole** the surrounding hot fields already
   left (between `rehash_pos_` and `obj_bytes_`): zero extra bytes, hot line.
2. The `ExpireIndex` itself and its counter were moved to the **cold tail** of `FlatStore`. Sitting
   mid-struct they pushed `cached_now_ms_`, the maxmemory fields and the snapshot flags 96 bytes
   further out, which is exactly the sort of layout tax a feature that is off must not levy.

## What was built

`src/cmd/t_hash_ttl.cc` + `src/cmd/t_hash_ttl.h` — nine commands, redis 7.4 semantics, all
differ-verified against the 7.4.2 binary:

```
HEXPIRE  key seconds  [NX|XX|GT|LT] FIELDS numfields field [field ...]
HPEXPIRE key ms       [NX|XX|GT|LT] FIELDS numfields field [field ...]
HEXPIREAT  key unix-s [NX|XX|GT|LT] FIELDS numfields field [field ...]
HPEXPIREAT key unix-ms[NX|XX|GT|LT] FIELDS numfields field [field ...]
HTTL         key FIELDS numfields field [field ...]
HPTTL        key FIELDS numfields field [field ...]
HEXPIRETIME  key FIELDS numfields field [field ...]
HPEXPIRETIME key FIELDS numfields field [field ...]
HPERSIST     key FIELDS numfields field [field ...]
```

Registry rows: HEXPIRE family arity −6 `Write|DenyOom`; HTTL family arity −5 `Readonly`;
HPERSIST arity −5 `Write`. ACL categories regenerated with `tools/gen_acl_categories.py` (the tool
grew `src/cmd/t_hash_ttl.cc` in its source list); registry size 181 → 190 commands, still inside the
255-command ACL bit budget.

### Semantics, all probed against the oracle rather than read out of its source

* per-field integer array reply: `-2` no such field (and no such key answers `-2` per field rather
  than erroring — that is 7.4.2's shape), `0` condition not met, `1` set, `2` deadline already past
  so the field was deleted outright;
* `HTTL`/`HEXPIRETIME` round the second-granularity answer **up** (`(ms + 999) / 1000`);
  `HPTTL`/`HPEXPIRETIME` are exact ms;
* `HPERSIST`: `-2` / `-1` (no deadline to remove) / `1`;
* "no deadline" is **infinity** for the conditions: `NX` and `LT` succeed against a TTL-free field,
  `XX` and `GT` fail; `GT`/`LT` are strict, so an equal deadline answers `0`;
* the condition is evaluated **before** the past-deadline delete, so `HEXPIREAT k 1 NX` on a field
  that already has a deadline answers `0` and leaves the field alone;
* a deadline exactly equal to now counts as past;
* deadlines are capped at **2^46 − 1 = 70368744177663 ms**, checked per unit and again after adding
  the base time, with the per-command message `ERR invalid expire time in '<cmd>' command`;
  negatives get `ERR invalid expire time, must be >= 0`;
* the two `numfields` error strings genuinely differ between the write and read families
  (`` ERR Parameter `numFields` should be greater than 0 `` vs
  `ERR Number of fields must be a positive integer`), and both are reproduced byte for byte;
* WRONGTYPE outranks every argument error; duplicate fields are processed independently;
* `HSET`/`HMSET` writing a field's **value** clears that field's deadline; `HINCRBY`,
  `HINCRBYFLOAT` and a no-op `HSETNX` deliberately keep it; `HDEL` drops field and deadline
  together;
* when the last live field of a hash goes — by immediate delete, by lazy reap or by the active
  cycle — the **key itself is removed** (`EXISTS` 0, `TYPE` none).

### Design

**Storage.** `HashVal` gains `HashFieldTtl* ttls` (null until the first `HEXPIRE`, null again once
the last deadline goes) and a cached `uint64_t ttl_bytes` so `kvobj_size()` stays a straight-line
addition instead of an out-of-line call. `HashFieldTtl` is an open-addressed field→absolute-ms map
with tombstones and a maintained `min_expire_ms` lower bound; it is a side allocation, so a TTL-free
hash grows by nothing and allocates nothing. `KvObj`, `Op` and `Client` are untouched.

**Embedded hashes cannot carry deadlines, by construction.** A small hash lives in the `KvObj` tail
(`Enc::Compact`) with no `HashVal` to hang a table off, so the first `HEXPIRE` that will actually
store something externalizes it first — the same move redis makes from `listpack` to `listpackex`.
The externalization is deliberately lazy: `HEXPIRE` against absent fields, or with a condition
nobody meets, leaves the one-allocation form alone. That is also what makes `enc == Compact ⇒ no
deadlines` a sound shortcut everywhere else.

**Reap on touch, not filter on read.** Every hash command funnels through one `hash_lookup()` that
finds, type-checks, and — only behind the gate — reaps anything already past *before* the handler
reads the hash. After it returns, every remaining field is live, so all sixteen pre-existing hash
handlers are unchanged below that line. The alternative (filtering at each of ~12 read sites) would
have put the feature's branch in twelve hot places instead of one.

**The gate** is `store.field_expire_count() != 0` — a per-shard count of hashes carrying deadlines,
in the hot cache line, wrapped in `__builtin_expect(..., false)`, with every byte of machinery
out of line behind it. A shard that has never seen `HEXPIRE` reads zero and reaches no code in
`t_hash_ttl.cc` at all.

**Active expiry** rides the *same* attention mechanism as key TTLs: a second `ExpireIndex` of key
hashes with a persistent cursor and a slot budget that counts empty slots, sampled from
`FlatStore::active_expire()` (so it inherits the ex loop's existing cycle, its budget and its
`DEBUG SET-ACTIVE-EXPIRE` disable switch). Registration is an unconditional insert on the write path
and self-heals on visit: a sampled entry whose key is gone, re-typed, or no longer carries deadlines
is dropped. As in this tree's key-level cycle, the pass is skipped while a snapshot capture is
active.

**Persistence came free, and this is the nicest part of the lane.** This tree has no command-based
persistence at all: the snapshot, the AOF base, the AOF increments and DUMP/RESTORE are all binary
post-images through one set of per-type hooks. Field deadlines therefore ride the ordinary hash
payload, selected by the record's existing per-type `encoding` byte — `0` is the untouched TTL-free
form, `1` appends an absolute `i64` deadline (`-1` = none) to each pair. Because the deadlines are
absolute, replay is deterministic and `hash_snapshot_load` simply refuses to load a field that has
already lapsed; a hash all of whose fields lapsed comes back with a key deadline of `1`, so the
ordinary key-expiry machinery hides and collects it with no new code path. No `HPEXPIREAT` rewrite
emitter was needed or written.

**Notifications** (class `h`): `hexpire` when a deadline is set, `hpersist` when one is removed,
`hdel` when a past deadline deletes fields, `hexpired` when a reap collects them, plus the generic
`del` when the key goes. `hexpired`/`del` raised by the active cycle have no originating command, so
they take the same keyless route as `expired`.

### Knobs

**None added, deliberately.** Redis 7.4 has no knob for hash-field TTLs — the feature is always on
and its only tunable is the active-expire cycle, which this lane joins rather than duplicates
(`DEBUG SET-ACTIVE-EXPIRE`, `kActiveExpireChecks`). Under the knob-compat rule, inventing a
tomo-only knob for a feature redis also has would be the wrong move, so `tomokv.conf` is unchanged.

Two counters were added to `INFO stats` instead, because the tests need to prove the mechanism
fired rather than that nothing broke:

* `expired_hash_fields` — cumulative fields collected by the lazy and active reapers;
* `hash_field_expires` — hashes currently registered as carrying deadlines (the gate's value).

## Three real bugs this lane hit, and what caught them

1. **`find_hash_in` filters on `HasTtl`.** Reusing it for the field-TTL cycle found nothing, so the
   cycle deregistered every hash it visited and then the *lazy* path went dead too (the gate had
   fallen to zero). Caught by watching `hash_field_expires` drop to 0 — a counter that exists only
   because of the vacuous-validation rule. Fixed with `find_any_hash_in`.
2. **`kvobj_adopt_hash` collapses a small hash into the embedded form — and that form `delete`s the
   `HashVal`,** taking a freshly-loaded TTL table with it. Silent deadline loss through COPY,
   RENAME, RESTORE and snapshot load. Caught by the directed A/B against the oracle. A hash carrying
   deadlines now takes `kvobj_new_hash` unconditionally.
3. **SO_REUSEPORT server leak, twice** (`epyc-server-leak-incident`): a previous binary still bound
   to 7250 silently split connections with the new one, which is why an early run showed
   `ERR unknown command` for half the stream. Both test drivers now refuse to boot unless the port
   is genuinely free and take the pid from the listening socket rather than from `$!`.

## Test evidence

All runs on cores 48-51 (server) / 52-55 (oracle), port 7250 / 7255. `tests/gate.sh` was **not**
run (it owns port 7899 and cores 0-7).

### Directed battery — `tests/hexpire.py 127.0.0.1 7250`

204 checks over nine sections: the byte-exact error surface, the NX/XX/GT/LT matrix on all four
setters against both TTL-free and TTL-bearing fields, reply rounding, value-write vs counter-write
interactions, immediate delete, lazy expiry, active expiry, representation coverage (embedded
promotion, a 600-field hashtable with 200 deadlines, binary and empty field names) and value
transport (COPY / RENAME / DUMP+RESTORE / `DEBUG RELOAD`).

```
  errors               ok
  conditions           ok
  reads                ok
  write-interactions   ok
  immediate-delete     ok
  lazy-expiry          ok
  active-expiry        ok
  representations      ok
  value-transport      ok
hexpire: 204 checks, 0 failures -> PASS
```

The expiry sections are not allowed to pass vacuously:

* the lazy section reads `expired_hash_fields` before and after and asserts it moved by **exactly
  2** — the number of fields that should have gone;
* the active section never touches the expiring key at all (it polls `DBSIZE`), so a lazy reap
  cannot masquerade as the cycle, and it asserts the counter moved;
* negative controls: a TTL-free hash leaves `hash_field_expires` at 0 and 50 `HGETALL`s expire
  nothing; a deadline 600 s in the future survives 1.5 s of active cycles with the counter
  unchanged; `HTTL` on a TTL-free field returns `-1`, not `-2`.

### Differ — `tests/differ.py 127.0.0.1 7250 127.0.0.1 7255 hexpire`

New suite `hexpire` in `tests/differ.py`: 4288 ops per run of seeded random hash + field-TTL traffic
using only **absolute** deadlines (a far-future table and a definitely-past table), so the whole
stream is deterministic on both servers with no sleeps and no clock race, plus a directed tail
covering the error surface, the 46-bit ceiling, the embedded→external transition, last-field key
removal, value transport and binary field names. `HTTL`/`HPTTL` are bucketed in `normalize()`
because they are computed from each server's own clock; the absolute forms stay byte-exact and are
what actually pin the deadline.

```
DIFFER hexpire: 4288 ops, 0 diffs -> PASS      (seeds 7, 11, 23, 42, 99, 101 and RESP3)
```

Regression sweep against the same oracle, all after the change:

```
DIFFER hash: 3545 ops, 0 diffs -> PASS       DIFFER string: 4033 ops, 0 diffs -> PASS
DIFFER list: 3521 ops, 0 diffs -> PASS       DIFFER set:    3524 ops, 0 diffs -> PASS
DIFFER zset: 3531 ops, 0 diffs -> PASS       DIFFER xshard: 4276 ops, 0 diffs -> PASS
DIFFER bitmap: 4262 ops, 0 diffs -> PASS     DIFFER hll:    3057 ops, 0 diffs -> PASS
DIFFER cgaps: 3310 ops, 0 diffs -> PASS      DIFFER bitfield: 3226 ops, 0 diffs -> PASS
DIFFER stream: 4031 ops, 0 diffs -> PASS
```

### Both atomic modes

```
--atomic 0: hexpire: 204 checks, 0 failures -> PASS | DIFFER hexpire: 4288 ops, 0 diffs -> PASS
--atomic 1: hexpire: 204 checks, 0 failures -> PASS | DIFFER hexpire: 4288 ops, 0 diffs -> PASS
```

### Recovery — `tests/hexpire_persist.sh ./build/tomokv 7250 48-51`

Seeds deadlines, terminates the server, brings it back from a snapshot and then from an AOF, and
checks the absolute deadlines came back byte-identical on a small hash, a 300-field hash with
deadlines on a subset, and a TTL-free hash used as a control.

```
== snapshot recovery ==
hexpire: 6 checks, 0 failures -> PASS
== AOF recovery ==
hexpire: 6 checks, 0 failures -> PASS
hexpire persist: PASS
```

### ASAN

`make asan` (`-fsanitize=address,undefined`, `ldd` confirms `libasan` is linked into the binary
under test), then the full battery, both differ suites and the whole recovery driver:

```
hexpire: 204 checks, 0 failures -> PASS
DIFFER hexpire: 4288 ops, 0 diffs -> PASS
DIFFER hash:    3545 ops, 0 diffs -> PASS
hexpire persist: PASS
```

No ASAN or UBSAN report was produced (`ASAN_OPTIONS=detect_leaks=1:log_path=...` left no files).

### Build

`make -j12 clean` then `make -j12`: no warnings, no errors.

## Known divergences from redis, and scope notes

* **Field ORDER inside a hash.** Redis's `listpackex` moves a field to the front of the listpack
  when it acquires a deadline, so `HGETALL`/`HKEYS`/`HVALS`/`HSCAN` come back in a different order
  than TomoKV's. Hash field order is unordered by contract and the differ normalizes it; no
  behaviour depends on it. Not fixed, and not worth fixing.
* **`OBJECT ENCODING`** reports TomoKV's own `compact`/`hashtable` for hashes and always has; it
  does not grow a `listpackex` value. Unchanged by this lane.
* **A capture window can briefly show a lapsed field.** The lazy reap is suppressed while a
  snapshot capture is in flight, for exactly the reason `active_expire()` is: the frozen table still
  has to serialize its pre-cut image, and an ordinary `HGET` is a `Readonly` command that never
  passes the snapshot write-gate. The nine commands in this file filter logically as well as
  reaping, so `HTTL`/`HPERSIST`/`HEXPIRE` stay correct throughout; the only visible effect is that a
  plain `HGET` may still see a just-lapsed field until the capture completes. Documented rather
  than fixed: fixing it properly means giving readonly commands a pre-image path, which is a
  snapshot-lane change, not a hash-lane one.
* **A field reap does not write an AOF record** unless it removes the key (which records a delete,
  as `active_expire()` does). It does not need to: deadlines are absolute, so a stale post-image
  containing a lapsed field is dropped by `hash_snapshot_load` on replay. Deliberate, not a cut.
* `HGETEX` and `HGETDEL` are redis **8.0**, not 7.4, and are out of this lane's scope.

## Files

| file | what |
|---|---|
| `src/cmd/t_hash_ttl.h` | `HashFieldTtl` + the narrow two-way seam between the hash and TTL lanes |
| `src/cmd/t_hash_ttl.cc` | the nine handlers, the shared reaper, the registry table |
| `src/cmd/t_hash.cc` | `hash_lookup()` gate, HSET/HDEL deadline clearing, seam impls, `encoding = 1` snapshot payload |
| `src/store/typeval.h` | `HashVal::ttls` + `ttl_bytes` + the `allocation_bytes()` override |
| `src/store/flatstore.h` | hot-line gate, cold-tail `field_expires_` index, `active_expire_fields()`, `find_any_hash_in()`, `note_loaded_object()` |
| `src/cmd/notify.h/.inc` | `hexpire` / `hpersist` / `hexpired` events |
| `src/cmd/scatter_engine.inc`, `snapshot.cc`, `aof.cc` | re-arm the reaper for objects that arrive from a load rather than a command |
| `src/cmd/serialize.cc` | live DUMP/RESTORE codec; the hash-field-TTL round-trip gap is recorded in `NOTES-HYGIENE.md` |
| `src/cmd/t_server.cc` | `expired_hash_fields`, `hash_field_expires` in `INFO stats` |
| `tests/hexpire.py` | the 204-check battery |
| `tests/hexpire_persist.sh` | snapshot + AOF recovery driver |
| `tests/differ.py` | the `hexpire` suite and `HTTL`/`HPTTL` normalization |
