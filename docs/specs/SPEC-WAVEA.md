# SPEC — Wave A parity features (TomoKV-cpp, pure 2s)

**Target tree:** `/home/user/Projects/tomokv-cpp-perthread` (perthread fork = THE base)
**Reference tree:** `/home/user/Projects/redis` @ `REDIS_VERSION "8.9.241"` (`src/version.h:1`)
**Status:** build-ready. Each feature below is self-contained; an implementation lane should need
no further source archaeology.

---

## 0. Scope, sources, and three standing warnings

### 0.1 The four features

| # | Feature | Where it lives in our architecture | New knobs |
|---|---------|-----------------------------------|-----------|
| 1 | Keyspace notifications | EX records → IO publishes through the existing channel-home pub/sub fanout | `notify-keyspace-events` |
| 2 | Connection limits | IO accept path + IO cron beat + WB staging accounting | `maxclients`, `timeout`, `tcp-keepalive`, `tcp-backlog`, `client-output-buffer-limit` |
| 3 | requirepass / AUTH | Pre-dispatch conn gate in `parse_and_dispatch` | `requirepass`, `protected-mode` |
| 4 | Sharded pub/sub | Third index alongside `pubsub_channels_` at the same channel home | none |

### 0.2 WARNING 1 — the reference tree is a FORK, not upstream

`/home/user/Projects/redis` is **not** stock Redis. It carries fork-local notification classes,
commands, and source files that upstream does not have. Confirmed divergences that matter here:

- Extra notify classes: `o` NOTIFY_OVERWRITTEN (1<<15), `c` NOTIFY_TYPE_CHANGED (1<<16),
  `a` NOTIFY_ARRAY (1<<23), `r` NOTIFY_RATE_LIMIT (1<<24, `#ifdef ENABLE_GCRA`),
  and four subkey classes `S`/`T`/`I`/`V` (1<<19 … 1<<22) with their own
  `__subkeyspace@`/`__subkeyevent@`/`__subkeyspaceitem@`/`__subkeyspaceevent@` channels
  (`/home/user/Projects/redis/src/server.h:806-834`, `/home/user/Projects/redis/src/notify.c:189-268`).
- `NOTIFY_ALL` in the fork **includes `a`**: `NOTIFY_GENERIC|STRING|LIST|SET|HASH|ZSET|EXPIRED|EVICTED|STREAM|MODULE|ARRAY`
  (`/home/user/Projects/redis/src/server.h:834`). Upstream's `A` has no `a`.
- `signalModifiedKey()` does not exist; it is `keyModified(client*, redisDb*, robj*, robj*, int)`
  at `/home/user/Projects/redis/src/db.c:1209`.
- Fork-only commands with events: `DELEX`, `MSETEX`, `INCREX`, `HSETEX`, `HIMPORTSET`, `LMOVEM`,
  the `t_array.c` `ar*` family, `gcra.c`.
- `notifyKeyspaceEvent()` is a wrapper over `notifyKeyspaceEventImpl()`
  (`/home/user/Projects/redis/src/notify.c:275-284`).

**Ruling for this spec: implement the UPSTREAM surface only.** The grammar is
`K E A g $ l s h z x e t d m n` and nothing else. Fork classes are out of scope and MUST be
rejected as invalid characters, so a config that works here also works against stock Redis.

This also engages the **vanilla-oracle rule** (memory: `tomokv-xshard-phase`): differential tests
for feature 1 must run against a **vanilla** redis-server, not this fork, or the fork's extra
`overwritten`/`type_changed`/`new`-ordering events will produce false diffs.

### 0.3 WARNING 2 — we are RESP2-only, single-DB

- `cmd_hello` (`src/cmd/t_server.cc:367`) accepts protover 2 only and answers `-NOPROTO` for 3.
  There is no `c->resp`. Every reply shape below is the RESP2 shape. No `>` push frames.
- `cmd_select` (`src/cmd/t_server.cc:389`) accepts only `SELECT 0`; `databases` is reported as 1.
  **Therefore `<db>` in every notification channel is the literal `0`.** Build the channel prefix
  as a compile-time constant `"__keyspace@0__:"` / `"__keyevent@0__:"`. Do not call an integer
  formatter on the hot path. If multi-DB ever lands, the prefix becomes a per-db cached string,
  not a per-event `snprintf`.

### 0.4 WARNING 3 — footprint locks are law

`static_assert(sizeof(Op) == 336)` (`src/exec/op.h:240`) and
`static_assert(sizeof(Client) == 1984)` (`src/net/conn.h:538`) are owner law. Measured cost of
violating them: +16B on `Op` = **-3.7% at 64c p32**.

**Verified headroom — MEASURED, not estimated.** Two probes were compiled against this tree on
2026-08-26: one on the real `Client`, one on a declaration-order mirror struct that reproduces
`sizeof == 1984` exactly, so `offsetof` could be taken on the private fields.

```
sizeof(Client)=1984  alignof(Client)=64
sizeof(ReplySegment)=24   sizeof(SegmentQueue<8>)=216
sizeof(Rob<64>)=192       sizeof(SmallBuf<512>)=536

mirror struct sizeof = 1984  (matches -- the mirror is faithful)
H1: rpos_ ends at 12, rbuf_ starts at 16          -> HOLE = 4 bytes  @ offset 12
H2: watch_dirty_ ends at 1957, sizeof = 1984      -> HOLE = 27 bytes @ offset 1957
    (a third, 1-byte hole exists at offset 43, before send_requested_ -- too small to use)
```

Two usable holes exist in `Client` and this spec spends both:

| Hole | Offset | Size | Spent on | Fits? |
|------|--------|------|----------|-------|
| H1 | 12 (after `uint32_t rpos_`, before `char* rbuf_`) | **4 bytes** | `uint32_t last_interaction_s_` (feature 2, `timeout`) | 4/4 exactly |
| H2 | 1957 (after `std::atomic<bool> watch_dirty_`, to the `alignas(64)` round-up) | **27 bytes** | `uint64_t obuf_bytes_` (8, at 1960 after 3 pad) + `uint32_t obuf_soft_since_s_` (4) + `bool authenticated_` (1) = **13 of 27** | yes, 14 to spare |

H1 is in the io-hot packed run, which is where `last_interaction` belongs — it is written on every
recv and every send completion. H2 is in the cold executor-facing tail, which is where the output
accounting and the auth bit belong (touched at retire and at dispatch, not per byte).

Every lane MUST re-run the static_asserts. If a field does not fit, it does not ship — find a
different representation (see memory: `user-hardcode-or-delete`).

### 0.5 Architectural facts every feature depends on

Read these before writing code; they are the constraints that shaped every design below.

1. **Shards are touched only by their EX threads.** Owner rule, literal. Nothing in Wave A may
   route a connection or a socket to an executor.
2. **Pub/sub is IO-owned.** `src/core/pubsub.inc` is textually `#include`d inside `IoLoop`'s
   private section (`src/core/io_loop.h:153`). Channel home = `hash(channel) % n_io` over
   `srv_->placement().ifid_threads()` (`pubsub.inc:78-86`). The home IO exclusively owns that
   channel's subscriber index.
3. **The pub/sub transport is IO→IO.** `pubsub_post()` (`pubsub.inc:63-76`) pushes a heap
   `PubSubEvent*` onto the target's `pubsub_events_` deque and then spins pushing ONE `nullptr`
   marker onto the `client_in_[self_->id()]` SPSC lane. The load-bearing comment says: *"The
   producer channel is an IO channel, so ordinary executor completion traffic cannot fill this
   SPSC lane."* Feature 1 must preserve that invariant — see §1.5.
4. **Executors reach IO through exactly two paths**: the ready-mask bit
   (`ExLoop::notify_sender`, `src/core/ex_loop.h:651`) and the claimed `post_client` first-contact
   path (`notify_sender_to`, `src/core/ex_loop.h:676`). Both are per-connection.
5. **`Client` is single-owner.** One IO thread owns recv, parse, dispatch, retire, and send for the
   connection's whole life (pure-2s ruling, `src/net/conn.h:1-14`).
6. **There is no serverCron.** The IO loop's only periodic hook is `backstop_pass_`, one pass in
   `kFlushBackstopEvery == 64` (`src/core/io_loop.h:842-843, 999`). The EX loop has a
   time-gated beat, `blocking_beat_ms_`, cadence 10 ms (`src/core/ex_loop.h:85-90`). Feature 2
   needs a real IO-side beat; §2.3 specifies it.
7. **`store().erase(hash, key)` is the single choke point for key removal** across every type
   family (17 call sites; `t_list.cc:443,473,688,742,936`, `t_set.cc:889,972`,
   `t_string.cc:351,396,416,423,1031`, `t_zset.cc:1369,1444,1889,2309`, `t_hash.cc:991`). This is
   what makes the collection-emptied `del` event cheap to implement correctly.
8. **`FlatStore` already has a counter-binding pattern**: `bind_expired_counter()` /
   `bind_evicted_counter()` (`src/store/flatstore.h:585-586`), bound in `Shard::init`
   (`src/core/shard.h:60-61`). Feature 1 extends exactly this pattern rather than inventing a new
   one.
9. **The out-of-line integration pattern ("multi2 pattern").** `src/cmd/multi.h:41-48` states it:
   *"The IO loop deliberately contains only predicted-cold calls into these entries. Keeping the
   transaction dispatch and lifetime machinery in multi.inc preserves the layout of the ordinary
   parse/dispatch and retirement paths."* Concretely: a narrow header declares
   `X_entry(IoLoop&, ...)` free functions; the bodies live in an `.inc` textually included by
   `xshard.cc`; `IoLoop` befriends them; the hot loop contains one
   `__builtin_expect(cond, false)` guarded call. Features 1 and 2 both use it — see §5.2 for
   which and why.

### 0.6 Knob house rules (owner, `src/core/config.h:8`)

> numeric where possible; 0 = off and **off allocates nothing**; -1 = auto; thresholds self-derive.
> A field in `Config` that nothing reads is a lie — delete it.

And the always-on budget (memory: `thredis-lb-3pct-budget`): **always-on machinery ≤3% or it does
not ship.** Every feature here must be byte-identical-cost when its knob is off.

Three knob surfaces must be updated together or the knob drifts:
1. `struct Config` + the parser in `src/core/config.h` (CLI **and** conf file, one grammar).
2. `init_config()` in `src/cmd/t_server.cc:211` (the `CONFIG GET`/`SET` table).
3. `tomokv.conf` (the annotated reference).

Adding a knob without all three is the documented `knob_matrix` trap
(memory: `thredis-codex-fork-integration-traps`).

---

## 1. FEATURE 1 — Keyspace notifications

### 1.1 Command/reply semantics

Keyspace notifications add **no commands**. They add message traffic on the existing pub/sub
fanout. A client observes them by `SUBSCRIBE`/`PSUBSCRIBE` on the reserved channel names.

Redis emits up to two messages per event
(`/home/user/Projects/redis/src/notify.c:169-188`, verbatim):

```c
    /* __keyspace@<db>__:<key> <event> notifications. */
    if (server.notify_keyspace_events & NOTIFY_KEYSPACE) {
        chan = sdsnewlen("__keyspace@",11);
        chan = sdscatlen(chan, buf, len);
        chan = sdscatlen(chan, "__:", 3);
        chan = sdscatsds(chan, key->ptr);
        chanobj = createObject(OBJ_STRING, chan);
        pubsubPublishMessage(chanobj, eventobj, 0);
        decrRefCount(chanobj);
    }

    /* __keyevent@<db>__:<event> <key> notifications. */
    if (server.notify_keyspace_events & NOTIFY_KEYEVENT) {
        chan = sdsnewlen("__keyevent@",11);
        chan = sdscatlen(chan, buf, len);
        chan = sdscatlen(chan, "__:", 3);
        chan = sdscatsds(chan, eventobj->ptr);
        chanobj = createObject(OBJ_STRING, chan);
        pubsubPublishMessage(chanobj, key, 0);
        decrRefCount(chanobj);
    }
```

So, for our single-DB server:

| Gate | Channel | Payload |
|------|---------|---------|
| `K` (NOTIFY_KEYSPACE) | `__keyspace@0__:<key>` | `<event>` |
| `E` (NOTIFY_KEYEVENT) | `__keyevent@0__:<event>` | `<key>` |

Both are ordinary RESP2 `message` frames — identical wire shape to `PUBLISH`:

```
*3\r\n$7\r\nmessage\r\n$<n>\r\n__keyspace@0__:foo\r\n$3\r\nset\r\n
```

A `PSUBSCRIBE __key*@0__:*` subscriber gets the 4-element `pmessage` form. Both are produced by
the existing `pubsub_delivery_frame()` (`src/core/pubsub.inc:162-175`) with **zero changes**.

**Ordering contract (must hold):**
- `K` before `E` for the same event.
- Multiple events from one command fire in Redis's source order. Example `SET k v EX 10`:
  `$`/`set` then `g`/`expire` (`/home/user/Projects/redis/src/t_string.c:191, 209`).
- The `n`/`new` event fires **before** the type event for the same command, because it is emitted
  inside `dbAddInternal` (`/home/user/Projects/redis/src/db.c:465`) which runs during the store
  mutation.
- A collection-emptied `g`/`del` fires **after** the type event that emptied it (e.g. `l`/`lpop`
  then `g`/`del`, `/home/user/Projects/redis/src/t_list.c:798, 806`).

Redis's own two short-circuits, both of which we replicate (§1.6):
1. class filter — `if (!(server.notify_keyspace_events & type)) return;` (`notify.c:158`)
2. zero-subscriber skip — `if (dictSize(server.pubsub_patterns) == 0 && kvstoreSize(server.pubsub_channels) == 0) return;` (`notify.c:162-163`)

### 1.2 EXACT knob grammar — `notify-keyspace-events`

Upstream flag table. Ours must accept **exactly** these 15 characters and reject every other byte
with an error.

| Char | Constant | Bit | Meaning |
|:----:|----------|-----|---------|
| `K` | `NOTIFY_KEYSPACE` | `1<<0` | Keyspace events, published to `__keyspace@<db>__:<key>` |
| `E` | `NOTIFY_KEYEVENT` | `1<<1` | Keyevent events, published to `__keyevent@<db>__:<event>` |
| `g` | `NOTIFY_GENERIC` | `1<<2` | Generic, type-independent commands — `DEL`, `EXPIRE`, `RENAME`, … |
| `$` | `NOTIFY_STRING` | `1<<3` | String commands |
| `l` | `NOTIFY_LIST` | `1<<4` | List commands |
| `s` | `NOTIFY_SET` | `1<<5` | Set commands |
| `h` | `NOTIFY_HASH` | `1<<6` | Hash commands |
| `z` | `NOTIFY_ZSET` | `1<<7` | Sorted-set commands |
| `x` | `NOTIFY_EXPIRED` | `1<<8` | Expired events (key expiry, lazy or active) |
| `e` | `NOTIFY_EVICTED` | `1<<9` | Evicted events (maxmemory) |
| `t` | `NOTIFY_STREAM` | `1<<10` | Stream commands |
| `m` | `NOTIFY_KEY_MISS` | `1<<11` | Key-miss events. **NOT in `A`** — by design |
| `d` | `NOTIFY_MODULE` | `1<<13` | Module key-type events |
| `n` | `NOTIFY_NEW` | `1<<14` | New-key events. **NOT in `A`** — by design |
| `A` | `NOTIFY_ALL` | alias | `g$lshzxetd` — everything **except** `m` and `n` |

Bit values are verbatim from `/home/user/Projects/redis/src/server.h:806-834`. Note bit `1<<12`
(`NOTIFY_LOADED`) is module-only and has **no character**; leave the bit unallocated so our masks
stay numerically comparable with Redis.

`NOTIFY_ALL` for us (upstream definition, fork's `|NOTIFY_ARRAY` dropped per §0.2):

```
NOTIFY_ALL = NOTIFY_GENERIC | NOTIFY_STRING | NOTIFY_LIST | NOTIFY_SET | NOTIFY_HASH |
             NOTIFY_ZSET | NOTIFY_EXPIRED | NOTIFY_EVICTED | NOTIFY_STREAM | NOTIFY_MODULE
           = 0x000027FC
```

**Parser** — mirror `keyspaceEventsStringToFlags()` (`/home/user/Projects/redis/src/notify.c:20-55`):
walk the string; `switch` each char to its bit; **any unknown char returns -1 and the whole SET
fails atomically, leaving the previous value untouched**. Empty string is valid and means `0`.

**Serializer** — mirror `keyspaceEventsFlagsToString()`
(`/home/user/Projects/redis/src/notify.c:61-94`). Emission order, verbatim upstream:

```
if ((flags & NOTIFY_ALL) == NOTIFY_ALL)  emit "A"
else  emit, in this order:  g  $  l  s  h  z  x  e  t  d
then, unconditionally:      n  K  E  m
```

> **PORT THIS CORRECTLY — the fork has a round-trip bug here.** In
> `/home/user/Projects/redis/src/notify.c:79`, the fork tests `NOTIFY_NEW` **inside** the `else`
> branch, so `CONFIG SET notify-keyspace-events "AKEn"` round-trips as `"AKE"` and `CONFIG REWRITE`
> permanently drops the `n`. Upstream emits `n` **outside** the else. We follow upstream: `n`, `K`,
> `E`, `m` are all emitted unconditionally after the class block. Our
> `CONFIG SET … "AKEn"` → `CONFIG GET` MUST return `"AKEn"`.
> This is a named test arm — see §1.9 arm G4.

**Normalization is observable and required.** The string is parsed to an int on SET and regenerated
from that int on GET; the input text is never stored. Consequences (all asserted in
`/home/user/Projects/redis/tests/unit/pubsub.tcl:673-682`):

| Input | `CONFIG GET` returns |
|-------|----------------------|
| `""` | `""` |
| `KA` | `AK` |
| `EA` | `AE` |
| `gKE` | `gKE` |
| `$lshzxeKE` | `$lshzxeKE` |
| `gg` | `g` |
| `g$lshzxetd` | `A` |
| `AKEn` | `AKEn` (upstream; fork wrongly gives `AKE`) |

**Grammar surfaces (all three, per §0.6):**

```
CLI / conf:   --notify-keyspace-events "KEA"      |   notify-keyspace-events KEA
CONFIG SET:   CONFIG SET notify-keyspace-events "KEA"
CONFIG GET:   CONFIG GET notify-keyspace-events   ->  1) "notify-keyspace-events" 2) "KEA"
```

Default: `""` (empty, all off) — matches `/home/user/Projects/redis/redis.conf:2174`.

Error on a bad character, matching this tree's wording minus the fork chars
(`/home/user/Projects/redis/src/config.c:3101-3105`):

```
ERR Invalid argument 'Qz' for CONFIG SET 'notify-keyspace-events'
```

(Our existing `normalize_config` failure path already produces exactly this shape —
`src/cmd/t_server.cc:314-319` — so returning `false` from the normalizer is sufficient. Do **not**
invent a new message.)

**`Config` field and knob type.** This is the one knob in Wave A that is genuinely a string, not
a number, so it takes a new `ConfigKind`:

```cpp
// src/core/config.h, in struct Config, ---- notifications (live via CONFIG SET) ----
// Keyspace-notification class mask (NOTIFY_* bits). 0 = off: no event is ever recorded, no
// allocation, and every write path is byte-identical to the pre-feature build.
uint32_t notify_events = 0;
```

and in `src/cmd/t_server.cc`, `enum class ConfigKind` gains `NotifyFlags`, whose `normalize_config`
case parses to a mask and re-serializes canonically (so the stored `ConfigValue::value` is already
the canonical string and `CONFIG GET` needs no extra work).

Live publication uses the **existing seqlock**, not a new one: extend
`Server::set_maxmemory_config`'s `begin_live_config_update()`/`end_live_config_update()` pair
(`src/core/server.h:523-537`) with a `live_notify_events_` atomic, and have `ExLoop` pick it up in
`refresh_maxmemory_config()` (rename → `refresh_live_config()`). That gives executors a
**per-pass** snapshot with zero per-op atomics — which is the whole point.

### 1.3 The event table, mapped onto OUR command table

Left column = our command (`src/cmd/*.cc` tables). Right = the class/event Redis emits, with the
upstream call site. `+g:del` means "additionally fire the generic `del` when the collection
becomes empty / the destination is emptied".

Absent from our tree, so **no producer exists** (spec them as reserved, do not invent):
`t` (no streams — there is no `t_stream.cc`), `d` (module events — no module system),
`MOVE`/`RESTORE`/`DUMP`/`BITFIELD`/`ZUNIONSTORE`/`ZINTERSTORE`/`ZDIFFSTORE`/`GEO*` (not in our
tables). The flag characters still parse and round-trip; they simply never fire. That is correct
parity behaviour and keeps a future stream lane from re-litigating the grammar.

#### `$` NOTIFY_STRING — file `src/cmd/t_string.cc`

| Our command | Handler | Event(s) | Redis site |
|---|---|---|---|
| `SET` | `cmd_set` | `$:set`; `+g:expire` if EX/PX/EXAT/PXAT; `+g:del` if the TTL is already in the past | `t_string.c:191, 209, 167` |
| `SETNX` | `cmd_setnx` | `$:set` (only when it actually set) | `t_string.c:191` |
| `SETEX` / `PSETEX` | `cmd_setex` / `cmd_psetex` | `$:set` then `g:expire` | `t_string.c:191, 209` |
| `GETSET` | `cmd_getset` | `$:set` — **note: no `getset` event exists** | `t_string.c:568` |
| `SETRANGE` | `cmd_setrange` | `$:setrange` | `t_string.c:637` |
| `APPEND` | `cmd_append` | `$:append` | `t_string.c:1401` |
| `INCR`/`DECR`/`INCRBY`/`DECRBY` | `cmd_incr`/`cmd_decr`/`cmd_incrby`/`cmd_decrby` | `$:incrby` — **always the literal `incrby`, for all four** | `t_string.c:932` |
| `INCRBYFLOAT` | `cmd_incrbyfloat` | `$:incrbyfloat` | `t_string.c:984` |
| `SETBIT` | `cmd_setbit` | `$:setbit` | `bitops.c:887` |
| `BITOP` | xshard (`cmd_xshard_only`) | `$:set` on dest; `g:del` when the result is empty | `bitops.c:1612, 1616` |
| `MSET` / `MSETNX` | xshard | `$:set` **once per key written** | `t_string.c:800` |
| `PFADD` | `cmd_pfadd` | `$:pfadd` (only when the register changed) | `hyperloglog.c:1689` |
| `PFMERGE` | xshard | `$:pfadd` on dest | `hyperloglog.c:1872` |

#### `g` NOTIFY_GENERIC

| Our command | Handler | Event(s) | Redis site |
|---|---|---|---|
| `DEL` / `UNLINK` | `cmd_del` | `g:del` **per key actually deleted** | `db.c:1468` |
| `GETDEL` | `cmd_getdel` | `g:del` — **not `getdel`** | `t_string.c:558` |
| `GETEX` | `cmd_getex` | `g:expire` (EX/PX/EXAT/PXAT), `g:persist` (PERSIST), `g:del` (past EXAT) | `t_string.c:540, 546, 530` |
| `EXPIRE`/`PEXPIRE`/`EXPIREAT`/`PEXPIREAT` | `cmd_expire` … `cmd_pexpireat` | `g:expire`; `g:del` when the deadline has already passed | `expire.c:838, 818` |
| `PERSIST` | `cmd_persist` | `g:persist` (only when a TTL was removed) | `expire.c:916` |
| `RENAME` / `RENAMENX` | xshard | `g:rename_from` (src) then `g:rename_to` (dst) | `db.c:2335-2336` |
| `COPY` | xshard | `g:copy_to` (dst only) | `db.c:2559` |
| `SORT … STORE` | xshard | `g:del` when the result is empty (the non-empty case is `l:sortstore`) | `sort.c:631` |
| *collection emptied* | every `store().erase()` site | `g:del` | see §1.4 |

#### `l` NOTIFY_LIST — file `src/cmd/t_list.cc`

| Our command | Handler | Event(s) | Redis site |
|---|---|---|---|
| `LPUSH`/`LPUSHX` | `cmd_lpush`/`cmd_lpushx` | `l:lpush` | `t_list.c:514, 522` |
| `RPUSH`/`RPUSHX` | `cmd_rpush`/`cmd_rpushx` | `l:rpush` | `t_list.c:514, 522` |
| `LPOP` | `cmd_lpop` | `l:lpop` `+g:del` | `t_list.c:795-806` |
| `RPOP` | `cmd_rpop` | `l:rpop` `+g:del` | `t_list.c:795-806` |
| `LINSERT` | `cmd_linsert` | `l:linsert` | `t_list.c:599` |
| `LSET` | `cmd_lset` | `l:lset` | `t_list.c:671` |
| `LREM` | `cmd_lrem` | `l:lrem` `+g:del` | `t_list.c:1155, 1161` |
| `LTRIM` | `cmd_ltrim` | `l:ltrim` `+g:del` | `t_list.c:980, 985` |
| `LMOVE`/`RPOPLPUSH` | xshard | src `l:lpop`\|`l:rpop` `+g:del`, then dst `l:lpush`\|`l:rpush` | `t_list.c:798, 806, 1194` |
| `LMPOP` | xshard | `l:lpop`\|`l:rpop` `+g:del` | `t_list.c:795-806` |
| `BLPOP`/`BRPOP`/`BLMPOP` | blocking xshard | `l:lpop`\|`l:rpop` `+g:del` — **fires when the pop actually happens**, i.e. possibly on the pusher's write, not on the blocked client's dispatch | `t_list.c:795-806` |
| `BLMOVE`/`BRPOPLPUSH` | blocking xshard | as `LMOVE` | `t_list.c:798, 1194` |
| `SORT … STORE` (non-empty) | xshard | `l:sortstore` — **classed LIST even for a zset/set input** | `sort.c:624` |

#### `s` NOTIFY_SET — file `src/cmd/t_set.cc`

| Our command | Handler | Event(s) | Redis site |
|---|---|---|---|
| `SADD` | `cmd_sadd` | `s:sadd` | `t_set.c:657` |
| `SREM` | `cmd_srem` | `s:srem` `+g:del` | `t_set.c:699, 701` |
| `SPOP` | `cmd_spop` | `s:spop` `+g:del` (both the count and no-count forms) | `t_set.c:863/1095`, `875/1109` |
| `SMOVE` | xshard | src `s:srem` `+g:del`, then dst `s:sadd` | `t_set.c:746, 755, 776` |
| `SINTERSTORE` | xshard | `s:sinterstore`, or `g:del` when the result is empty | `t_set.c:1555, 1563/1436` |
| `SUNIONSTORE` | xshard | `s:sunionstore`, or `g:del` when empty | `t_set.c:1927, 1936` |
| `SDIFFSTORE` | xshard | `s:sdiffstore`, or `g:del` when empty | `t_set.c:1927, 1936` |

#### `h` NOTIFY_HASH — file `src/cmd/t_hash.cc`

Upstream fires these with the plain `notifyKeyspaceEvent`. (The fork uses
`notifyKeyspaceEventWithSubkeys` — ignore that, per §0.2.)

| Our command | Handler | Event(s) | Redis site |
|---|---|---|---|
| `HSET` / `HMSET` | `cmd_hset` | `h:hset` — **one event for the whole command**, not one per field | `t_hash.c:4055` |
| `HSETNX` | `cmd_hsetnx` | `h:hset` (only when it set) | `t_hash.c:3939` |
| `HINCRBY` | `cmd_hincrby` | `h:hincrby` | `t_hash.c:4807` |
| `HINCRBYFLOAT` | `cmd_hincrbyfloat` | `h:hincrbyfloat` | `t_hash.c:4871` |
| `HDEL` | `cmd_hdel` | `h:hdel` `+g:del` | `t_hash.c:5296, 5301` |

We have no `HEXPIRE`/`HPERSIST`/`HGETEX`/`HGETDEL` and therefore no `hexpire`/`hpersist`/
`hexpired` producers. Nothing to build.

#### `z` NOTIFY_ZSET — file `src/cmd/t_zset.cc`

| Our command | Handler | Event(s) | Redis site |
|---|---|---|---|
| `ZADD` | `cmd_zadd` | `z:zadd`, or `z:zincr` when the `INCR` option is used | `t_zset.c:2094` |
| `ZINCRBY` | `cmd_zincrby` | `z:zincr` | `t_zset.c:2094` |
| `ZREM` | `cmd_zrem` | `z:zrem` `+g:del` | `t_zset.c:2140, 2142` |
| `ZREMRANGEBYRANK` | `cmd_zremrangebyrank` | `z:zremrangebyrank` `+g:del` | `t_zset.c:2173, 2269, 2271` |
| `ZREMRANGEBYSCORE` | `cmd_zremrangebyscore` | `z:zremrangebyscore` `+g:del` | `t_zset.c:2178, 2269` |
| `ZREMRANGEBYLEX` | `cmd_zremrangebylex` | `z:zremrangebylex` `+g:del` | `t_zset.c:2184, 2269` |
| `ZPOPMIN`/`BZPOPMIN` | `cmd_zpopmin` / blocking | `z:zpopmin` `+g:del` | `t_zset.c:4335, 4357` |
| `ZPOPMAX`/`BZPOPMAX` | `cmd_zpopmax` / blocking | `z:zpopmax` `+g:del` | `t_zset.c:4335, 4357` |
| `ZMPOP`/`BZMPOP` | xshard / blocking | `z:zpopmin`\|`z:zpopmax` `+g:del` | `t_zset.c:4335, 4357` |
| `ZRANGESTORE` | xshard | `z:zrangestore`, or `g:del` when the range is empty | `t_zset.c:3363, 3369` |

**Watch the spelling.** The range-removal event names are the full
`zremrangebyrank`/`zremrangebyscore`/`zremrangebylex` — *not* the abbreviated `zrembyrank` forms
that appear in some older documentation.

#### `x` / `e` / `m` / `n` — the four non-command classes

| Class | Event | Our trigger | Redis site |
|---|---|---|---|
| `x` | `expired` | `FlatStore` lazy expiry on lookup **and** `ExLoop::active_expire_cycle` (`src/core/ex_loop.h:157`). Both already funnel through `expired_counter_` (`flatstore.h:647, 715-716, 726, 731, 795, 824, 1234, 1260`). | `db.c:2865, 2897` (one shared emitter, `deleteKeyAndPropagate`) |
| `e` | `evicted` | Eviction under `maxmemory`; already funnels through `evicted_counter_` (`flatstore.h:674, 1171`). | `db.c:2865, 2897` — same emitter, different `notify_type` |
| `m` | `keymiss` | Read miss. Redis fires it from `lookupKey()` (**not** `lookupKeyReadWithFlags`) and **suppresses it for write lookups**: `if (!(flags & (LOOKUP_NONOTIFY \| LOOKUP_WRITE)))`. Our equivalent gate: fire only for commands whose spec has `CmdFlags::Readonly` and **not** `Write`. | `db.c:348-353` |
| `n` | `new` | Key creation. Redis emits it in `dbAddInternal` **immediately after `signalKeyAsReady` and before the type-specific event**. Our equivalent: the insert path in `FlatStore` that creates a slot for a key that did not exist. | `db.c:464-465` |

`x` and `e` share one emitter in Redis, with the class and name both variables:

```c
char *notify_name = notify_type == NOTIFY_EXPIRED ? "expired" : "evicted";
```
(`/home/user/Projects/redis/src/db.c:2865`, emitted at `:2897`)

Do the same here: one `flat_notify(class, event_id, key)` sink, two callers.

Two `m`/`n` gotchas that will otherwise cost a debugging night:
- **`m` and `n` are excluded from `A`.** `notify-keyspace-events KEA` will not deliver either. They
  must be requested explicitly (`KEAn`, `KEAm`). Comments say so verbatim at
  `/home/user/Projects/redis/src/server.h:819, 822`.
- **`n` carries no type** and fires *before* an aggregate key's contents exist. `LPUSH` on a new key
  emits `new` then `lpush`.

### 1.4 The `del`-on-empty idiom, and why our version is cleaner than Redis's

Redis has **no shared helper** for this. Each type re-implements `dbDelete` + notify, and the two
orderings are *inconsistent across files*: `t_list.c:805-806` deletes then notifies;
`t_hash.c:3730-3735` notifies then deletes. Observably they are the same because both happen inside
one command, but there is no invariant to lean on.

We have one. **Every** whole-key removal in our tree goes through
`FlatStore::erase(uint64_t h, Slice key)` (`src/store/flatstore.h:721`). That is the hook:

```cpp
// src/store/flatstore.h — extends the EXISTING bind_expired_counter/bind_evicted_counter pattern
void bind_notify_sink(FlatNotifySink* sink) { notify_sink_ = sink; }
```

with `notify_sink_ == nullptr` meaning off. Then:

- `erase()` → `g:del` (guard: the erase actually removed a live key).
- the lazy/active expiry branches that already bump `expired_counter_` → `x:expired`.
- the eviction branches that already bump `evicted_counter_` → `e:evicted`.
- the insert-creates-a-new-slot branch → `n:new`.
- read-miss in `find()` → `m:keymiss`, gated on the caller being a `Readonly` command.

**Consequence worth stating plainly:** hooking `erase()` gives us `g:del` for DEL/UNLINK/GETDEL
*and* every collection-emptied case *and* every store-emptied-destination case in one place, with
Redis's ordering (type event first, then `del`) falling out naturally because the handler emits its
own event before calling `erase()`. The lane does **not** need to touch 17 call sites.

**All five `t_string.cc` erase sites were checked against the real code and every one is a genuine
user-visible `del`:**

| Site | Command | Redis equivalent |
|---|---|---|
| `t_string.cc:351` | `SET` with an already-elapsed EX | `g:del`, `t_string.c:167` |
| `t_string.cc:396` | `GETEX` with a past EXAT | `g:del`, `t_string.c:530` |
| `t_string.cc:416` | `cmd_getdel` | `g:del`, `t_string.c:558` |
| `t_string.cc:423` | `cmd_del` loop (DEL/UNLINK, per key) | `g:del`, `db.c:1468` |
| `t_string.cc:1031` | `EXPIRE`-family with a deadline `<= now_ms()` | `g:del`, `expire.c:818` |

So `erase()` needs a **suppress flag only for the non-command paths**:
- expiry and eviction (they emit `x:expired` / `e:evicted` instead, never `del`);
- `FLUSHALL`/`FLUSHDB` — **Redis emits no notification at all** for a flush. Confirm the flush path
  (`cmd_flush`, `src/cmd/t_server.cc:915-916`, an `AllShards` scatter) does not route through the
  notifying `erase()`, or suppress it explicitly. Arm N1 tests this;
- snapshot/migration-internal removals, if any.

### 1.5 Where it hooks into OUR architecture

This is the design decision the whole feature turns on, so it is stated with its rejected
alternative.

#### The problem

A notification is *produced* on an **EX** thread (inside a write handler, or in
`active_expire_cycle`, or in the eviction path). It must be *delivered* through the pub/sub fanout,
which is **IO**-owned and homed by `hash(channel) % n_io`. EX threads must not touch connection
state, and the `client_in` SPSC lane carrying pub/sub wake markers is documented as an
**IO-producer** lane (`src/core/pubsub.inc:71-73`).

#### REJECTED — EX posts directly

Letting `ExLoop` call `pubsub_post()` breaks three things at once:
1. It puts executor traffic on the `client_in` lane that the pub/sub comment explicitly relies on
   being IO-only, so a burst of notifications can now fill the same lane executors use for
   first-contact `notify_sender_to` posts.
2. `pubsub_post` **spins unboundedly** (`while (!target.post_client(...)) __builtin_ia32_pause();`,
   `pubsub.inc:74-75`). Spinning on the scarce EX role under backpressure is exactly the shape the
   `ex` capacity numbers say we cannot afford (~750k ops/s/thread vs 2.85M for a send role —
   memory: `tomokv-true-ratio-and-role-scaling`).
3. It forces EX to allocate a heap `PubSubEvent` with three `std::string`s per event, on the hot
   path, per write.

#### ADOPTED — two lanes, split by whether a client exists

**Lane A — command-driven events (classes `g $ l s h z t d n m`).**

EX **records**, IO **publishes**.

1. EX appends a descriptor to an out-of-line batch attached to the `Op`:
   ```cpp
   struct NotifyRecord { uint8_t cls; uint8_t event_id; Slice key; };
   ```
   `Slice key` is a pointer into the connection's read buffer, which the ROB already pins until
   this op retires (`src/exec/op.h:8-12`) — so **there is no string copy and no allocation** for
   the common case where the key is an argv slice. `event_id` indexes a static table of the ~30
   event-name literals, so the name is not copied either.
2. The batch hangs off the `Op` via the existing marker discipline
   (`kScatterStateMarker`/`kBlockingStateMarker`/`kMultiStateMarker`, `src/exec/op.h:50-53`) —
   add `kNotifyStateMarker = -6`. **`sizeof(Op)` does not change.**
   *Caveat:* `zc_ptr` is already multiplexed. If a command needs both a zero-copy borrow and a
   notify batch (it cannot — borrows are GET-only and GET fires no event except `m:keymiss`, which
   is a read-miss and therefore has no borrow), the lane must assert the exclusion rather than
   assume it. Write the assert.
3. The owning IO thread converts the batch at retire, in the **existing** retire callback installed
   by `IoLoop::init` (`src/core/io_loop.h:79-92`) — the same hook that already dispatches
   `xshard_retire` / `blocking_retire` / `multi_retire_entry`:
   ```cpp
   else if (op.has_notify_state()) notify_retire_entry(*loop, client, op);
   ```
4. `notify_retire_entry` builds the two channel names, computes `pubsub_home_for()` for each, and
   calls the **existing** `pubsub_post()` — which is now legitimately being called from an IO
   thread, preserving the invariant in §0.5(3) exactly.

Properties this buys:
- Zero EX-side allocation and zero string copies in the common case.
- The pub/sub transport invariant is untouched — no new producer class.
- Ordering is free: the ROB retires in order, so notifications from op *N* are posted before those
  from op *N+1* on the same connection.
- A notification is emitted **after** the producing command's own reply bytes are staged. For any
  *other* subscriber that difference is unobservable. For the producing client it cannot arise:
  RESP2 subscriber mode forbids write commands (`src/core/io_loop.h:434-441`).

**Lane B — keyless events (`x:expired`, `e:evicted`).**

These fire in `active_expire_cycle` and the eviction path with **no `Op` and no client**. They need
their own carrier:

1. Each EX thread owns a `std::deque<NotifyOut>` where `NotifyOut` owns its key bytes (the key is
   about to be freed, so a copy is mandatory here — this is the one place a copy is unavoidable).
2. The `ExLoop` pass drains it behind a predicted-false flag test, posting to each channel home via
   `post_pubsub_event()` and a **non-blocking** marker post. On refusal, leave the item queued and
   retry next pass. **Never spin.** (The `pubsub_post` spin is only safe from IO; §1.5 rejected
   reason 2.)
3. The deque is allocated lazily: with `notify_events == 0` it is never touched and never
   constructed.

Alternatively — and preferably, if measurement supports it — route Lane B through the same
IO-side conversion by having EX enqueue and the *nearest* IO drain. Measure both; the deque-on-EX
version is simpler and the volume is low (expiry and eviction are already bounded per pass:
`kActiveExpireChecks == 20`, `src/core/ex_loop.h:41`).

#### Channel-home consequences worth designing around

- Each notification produces **two** publishes with **different homes**:
  `__keyspace@0__:<key>` hashes on the key (so it sprays across all IO threads — good),
  `__keyevent@0__:<event>` hashes on the event name, of which there are ~30 (so **all `set`
  keyevents land on one IO thread**). Under an `E`-only config with a hot write mix, that thread
  is a hotspot. Document it; do not fix it speculatively. If it bites, the fix is to salt the
  keyevent home with the key's hash *for routing only* and replicate the channel index the way
  patterns already are (`pubsub.inc:118-136`) — but that is a Wave B decision, not this one.
- Pattern subscriptions are already replicated to **every** home (`pubsub_start_modify`,
  `pubsub.inc:585-595`), so `PSUBSCRIBE __keyevent@0__:*` works with no new machinery. This is the
  common real-world usage and it costs nothing extra.

### 1.6 Hot-path budget

Three nested gates, cheapest first. This is Redis's structure (`notify.c:158, 162-163`) with our
per-pass snapshot substituted for its global load.

| Gate | Cost when it fails | Where |
|---|---|---|
| G0 — feature off | `if (__builtin_expect(!notify_mask_, true)) return;` — one **thread-local** load (latched per EX pass from the seqlock), one branch. No shared line, no atomic. | every candidate site |
| G1 — class off | `if (!(notify_mask_ & cls)) return;` — one AND, one branch on an already-loaded register | every candidate site |
| G2 — no subscribers | `if (!srv_->pubsub_any_subscribers()) return;` — one relaxed load of an **existing** counter | at record time |

G2 deserves a note: `Server` already publishes `pubsub_active_channels_` and
`pubsub_pattern_subscriptions_` as relaxed atomics (`src/core/server.h:570-575`). Redis's
equivalent check is `dictSize(pubsub_patterns) == 0 && kvstoreSize(pubsub_channels) == 0`
(`notify.c:162-163`). Ours is the sum of those two counters — a single relaxed load on a
rarely-written line. **Do not** add a new counter for this; use what exists.

**Off-state claim to prove, not assume:** with `notify-keyspace-events ""` the write path must be
byte-identical in instruction count to the pre-feature build. The instrument is
`instr/op` on a loopback p32 SET cell (memory: `tomokv-goodsize-nallocx-lesson` — instr/op is the
bisect instrument). See §5.3.

**On-state cost:** one `NotifyRecord` push per event (16 bytes, into an inline array), plus at
retire, two `std::string` channel constructions + two `PubSubEvent` heap allocations per event.
That is unapologetically expensive — it is Redis's cost too, and it is paid only when the operator
has asked for notifications. What must **not** happen is any of it leaking into the off path.

### 1.7 Out-of-line integration (the multi2 pattern)

Feature 1 uses it. New file `src/cmd/notify.h`, mirroring `src/cmd/multi.h:41-48`:

```cpp
// notify.h -- keyspace-notification integration at the retire seam.
// The implementation is textually included by xshard.cc (notify.inc). The IO loop contains only
// predicted-cold calls into these entries, so the ordinary parse/dispatch and retirement paths
// keep their layout.
namespace tomo {
class Client; class IoLoop; class Op; class Server; class Shard;
struct NotifyBatch;

// EX side -- recording. Returns false and drops the event if the batch is full (see §1.8).
bool notify_record(Shard& shard, Op& op, uint8_t cls, uint8_t event_id, Slice key);
bool notify_record_keyless(Shard& shard, uint8_t cls, uint8_t event_id, Slice key);

// IO side -- publication at retire. Cold-called from the WbEngine retire callback.
void notify_retire_entry(IoLoop& loop, Client& client, Op& op);
uint32_t notify_ex_pass_entry(Shard& shard);   // Lane B drain, one bounded pass
void notify_shutdown_entry(IoLoop& loop);
}
```

`IoLoop` befriends `notify_retire_entry` beside the existing `multi_*` friends
(`src/core/io_loop.h:147-152`). The bodies go in `src/cmd/notify.inc`, `#include`d by `xshard.cc`
next to `multi.inc`, `blocking.inc`, `scatter_engine.inc`, and `atomics_glue.inc`.

### 1.8 Bounded resources (the OOM lesson)

Memory (`tomokv-atomic-64c-convoy-oom`) records a 17 GB → 126 GB blowup that ended in a silent
SIGKILL. Notifications are an unbounded amplifier — one `DEL k1 k2 … k1000` produces 2000 heap
events — so bound them at design time:

1. **Per-op batch cap.** `NotifyBatch` holds an inline array of 8 records and spills to heap. A
   command producing more than *N* (suggest 1024) records **drops the overflow and bumps a
   counter**. Dropping is the correct failure: Redis's pub/sub is already at-most-once and drops on
   output-buffer overflow.
2. **Global in-flight cap.** Reuse `Server::pubsub_inflight_` (`src/core/server.h:570`) — it already
   counts heap-owned events. Add a ceiling; above it, `notify_retire_entry` drops and counts. This
   is a valve, not an ordering device.
3. **Counters, exposed in `INFO STATS`,** alongside the six existing `pubsub_*` gauges:
   `notify_events_fired`, `notify_events_dropped`. The second is what makes the valve visible
   instead of silent.
4. **The Vacuous-validation rule applies** (memory: `thredis-vacuous-validation-trap`): a test that
   passes with `notify_events_fired == 0` proves nothing. Every arm in §1.9 must assert the counter
   moved.

### 1.9 Test plan — the event-firing matrix

**Style:** `tests/notify.py HOST PORT`, modelled on `tests/pubsub.py` (raw socket, `encode()`,
`Conn.read()`, `expect()`, `info_stats()`) — reuse that file's `Conn` class verbatim.

**Harness shape.** One `admin` connection for `CONFIG SET`/`INFO`, one `listener` doing
`PSUBSCRIBE __keyspace@0__:* __keyevent@0__:*`, one `driver` issuing writes. After each write the
listener must read exactly the expected frames, in order, with a short deadline. A helper
`drain(listener, timeout)` returns everything that arrived, so an arm can assert both *presence*
and *absence*.

| Arm | What it proves |
|-----|----------------|
| **G1 grammar round-trip** | The table in §1.2: `""`, `KA`→`AK`, `EA`→`AE`, `gKE`→`gKE`, `$lshzxeKE` unchanged, `gg`→`g`, `g$lshzxetd`→`A` |
| **G2 grammar rejection** | `CONFIG SET notify-keyspace-events "Qz"` errors **and** leaves the previous value intact (read it back) |
| **G3 fork-char rejection** | `o`, `c`, `a`, `S`, `T`, `I`, `V`, `r` are each rejected — this is the §0.2 ruling, enforced |
| **G4 `n` survives `A`** | `CONFIG SET … "AKEn"` → `CONFIG GET` returns `"AKEn"`. **This is the fork's round-trip bug; we must not reproduce it.** |
| **M1 keyspace only** | `K` alone: `__keyspace@0__:k` fires, `__keyevent@0__:set` does **not** |
| **M2 keyevent only** | `E` alone: the mirror of M1 |
| **M3 both** | `KE`: exactly two frames, `__keyspace` **before** `__keyevent` |
| **M4 class gating** | `KE$` fires on `SET` but **not** on `LPUSH`; `KEl` the reverse |
| **M5 full matrix** | One arm per row of §1.3 — command in, exact `(channel, payload)` list out, in order. This is the bulk of the test and the reason the tables above are exhaustive |
| **M6 `A` excludes m/n** | With `KEA`: `GET missing` produces nothing and `SET brandnew v` produces `set` but **no** `new`. With `KEAmn`: both appear |
| **M7 multi-event commands** | `SET k v EX 10` → `set` then `expire`, in that order. `SET k v EX -1`(past) → `del` |
| **M8 empty-collection del** | `LPUSH`+`LPOP` → `lpush`, `lpop`, `del`. Repeat for `SREM`, `HDEL`, `ZREM`, `LTRIM`, `LREM`, `SPOP` |
| **M9 store-destination del** | `SINTERSTORE dst nonexistent` → `del` on `dst`; `ZRANGESTORE` empty → `del` |
| **M10 per-key fanout** | `DEL a b c` (all existing) → three `del` events; `MSET a 1 b 2` → two `set` events |
| **M11 cross-shard** | `RENAME` across shards → `rename_from` then `rename_to`; `SMOVE` → `srem` then `sadd`; `LMOVE` → `lpop`+`del`+`rpush`. Force the split by choosing keys whose `hash % nshards` differ |
| **X1 expired** | `SET k v PX 50`, subscribe `x`, wait, assert `__keyevent@0__:expired` with payload `k`. Cover **both** paths: a lazy expiry (touch the key after the deadline) and an active one (never touch it — `active_expire_cycle` must produce it) |
| **X2 evicted** | `maxmemory` small + `allkeys-lru`, drive inserts, assert `evicted` fires |
| **X3 keymiss** | `KEm`: `GET absent` fires `keymiss`; **`SET absent v` does NOT** (the `LOOKUP_WRITE` suppression, `db.c:349`) |
| **X4 new** | `KEn`: first `SET k` fires `new`; a second `SET k` does **not** (asserted upstream at `pubsub.tcl:684-695`). `LPUSH` on a fresh key fires `new` **before** `lpush` |
| **N1 no-flush** | `FLUSHALL` with `KEA` produces **zero** notifications |
| **N2 zero-subscriber skip** | With `KEA` and nobody subscribed, `notify_events_fired` must **not** advance (the G2 short-circuit is live) |
| **N3 live flip** | `CONFIG SET` from `""` to `KEA` mid-workload starts events; back to `""` stops them, with no lost or duplicated frames on either side of the flip |
| **D1 differential** | New `differ.py` suite `notify`: run an identical randomized write stream against us and a **vanilla** redis-server (§0.2), both `PSUBSCRIBE`d, and diff the normalized notification streams. This is the arm that catches an event name or class we got subtly wrong |
| **B1 bounded** | `DEL` of 5000 keys in one command: assert `notify_events_dropped` is either 0 or matches the cap, and that RSS does not blow up. Assert the server survives |
| **V1 non-vacuous** | Every arm above re-reads `INFO STATS`; any arm that expects events must show `notify_events_fired` strictly increasing. An arm that expects none must show it flat |

**ASAN arm.** The Lane B copy path is new heap traffic on the EX thread. Run `tests/notify.py`
against the ASAN build with X1/X2 emphasized (memory: `thredis-asan-repro-recipe`).

**Churn arm.** Reuse `pubsub.py`'s pattern (`pubsub.py:197-218`): 320 abrupt
subscribe/disconnect cycles concurrent with a write storm, then wait for all six `pubsub_*`
gauges **plus** `pubsub_inflight` to drain to zero. This is the arm that catches a leaked
`PubSubEvent` on the notification path.

---

## 2. FEATURE 2 — Connection limits

Five knobs, one theme: bound what a connection can consume. Three are enforced at accept, one on a
new IO cron beat, one on the reply-staging path.

### 2.1 `maxclients`

#### Redis semantics

The check is in `acceptCommonHandler`, **before `createClient()`**
(`/home/user/Projects/redis/src/networking.c:1741-1763`, verbatim):

```c
    if (listLength(server.clients) + getClusterConnectionsCount()
        >= server.maxclients)
    {
        char *err;
        if (server.cluster_enabled)
            err = "-ERR max number of clients + cluster "
                  "connections reached\r\n";
        else
            err = "-ERR max number of clients reached\r\n";

        /* That's a best effort error message, don't check write errors.
         * Note that for TLS connections, no handshake was done yet so nothing
         * is written and the connection will just drop. */
        if (connWrite(conn,err,strlen(err)) == -1) {
            /* Nothing to do, Just to avoid the warning... */
        }
        server.stat_rejected_conn++;
        connClose(conn);
        return;
    }
```

Points that are easy to get wrong:
- Comparison is **`>=`**, on a **pre-count** (the rejected client is never allocated or registered).
- The error is a **raw, non-blocking, best-effort** write on the freshly accepted fd — the fd is
  already `O_NONBLOCK` from `accept4(SOCK_NONBLOCK)`. The return value is deliberately ignored.
- The fd is closed **synchronously and immediately** after, with no linger, so delivery genuinely is
  best-effort.
- `server.stat_rejected_conn` is bumped — and is shared with the protected-mode rejection
  (`networking.c:1694`), so the counter is not maxclients-exclusive.

Config (`/home/user/Projects/redis/src/config.c:3442`):
```c
createUIntConfig("maxclients", NULL, MODIFIABLE_CONFIG, 1, UINT_MAX, server.maxclients, 10000, INTEGER_CONFIG, NULL, updateMaxclients),
```
Default **10000**, min 1, live.

`adjustOpenFilesLimit()` (`/home/user/Projects/redis/src/server.c:2698-2781`) relates it to the fd
limit: `maxfiles = server.maxclients + CONFIG_MIN_RESERVED_FDS`, where
`CONFIG_MIN_RESERVED_FDS == 32` (`server.h:145`) and `CONFIG_FDSET_INCR == 32+96 == 128`
(`server.h:209`). If `setrlimit` cannot reach `maxfiles`, Redis walks down in steps of 16 and then
**lowers `server.maxclients` to `bestlimit - 32`**, logging:

```
Current maximum open files is %llu. maxclients has been reduced to %d to compensate for low ulimit. If you need higher maxclients increase 'ulimit -n'.
```

and if `bestlimit <= 32`, exits:

```
Your current 'ulimit -n' of %llu is not enough for the server to start. Please increase your open file limit to at least %llu. Exiting.
```

#### Our implementation

**Counter.** `Server` gains one relaxed atomic:

```cpp
std::atomic<uint64_t> live_clients_{0};      // accepted, not yet released
std::atomic<uint64_t> rejected_conns_{0};    // maxclients + protected-mode, matching Redis
```

Accept is a rare event (memory: `epyc-scaling-and-perthread-truth` puts IO at ~85k ops/s/thread of
*command* work; accepts are orders of magnitude rarer), so one shared-line RMW per accept is
comfortably inside the ≤3% always-on budget. Do **not** reuse `g_clients.size()` from
`src/cmd/t_server.cc:171` — it is behind `g_clients_mu` and taking a process-wide mutex on the
accept path of every IO thread is a needless serialization point.

**Enforcement point.** `IoLoop::on_accept` (`src/core/io_loop.h:262`), immediately after
`int fd = cqe->res;` and **before** `new (std::nothrow) Client(fd)`:

```cpp
// Pre-count, >= , matching Redis. fetch_add-then-check would over-admit under a
// multi-IO-thread accept storm; load-check-then-add under-admits by at most nthreads.
if (srv_->live_clients() >= srv_->maxclients()) {
    static constexpr char kErr[] = "-ERR max number of clients reached\r\n";
    ::send(fd, kErr, sizeof(kErr) - 1, MSG_NOSIGNAL | MSG_DONTWAIT);  // best effort, ignore result
    ::close(fd);
    srv_->note_rejected_conn();
    self_->sig().accept_rejected++;
    if (!(cqe->flags & IORING_CQE_F_MORE)) { self_->sig().accept_rearm++; arm_accept(unix_socket); }
    return;
}
```

**Race note, stated rather than hidden.** With `n_io` independent SO_REUSEPORT listeners
(`src/core/io_loop.h:59-70`), a load-then-add admits up to `n_io - 1` connections over the limit
during a simultaneous burst. Redis is single-accept-threaded and has no such window. Two options:

- **(a) Accept the slop** — document that `maxclients` is enforced within `±n_io`. This is what the
  spec recommends: it costs one relaxed load, the overshoot is bounded and tiny, and `maxclients` is
  a safety valve, not an accounting boundary.
- **(b) CAS loop** — `compare_exchange_weak` on `live_clients_` to make it exact. Costs a contended
  RMW per accept under a storm. Only do this if a test demands exactness.

Pick (a); write the `±n_io` bound into the knob's documentation so nobody files it as a bug later.

**Increment/decrement pairing — three sites, and the third is the one that leaks.**

| Site | Action |
|------|--------|
| `IoLoop::on_accept`, after the check passes | `live_clients_.fetch_add(1)` |
| `IoLoop::close_client`, beside `command_client_disconnected(c)` (`src/core/io_loop.h:959`) | `fetch_sub(1)` |
| `IoLoop::~IoLoop` — `for (Client* c : pending_handoffs_) { ::close(c->fd()); delete c; }` (`src/core/io_loop.h:100`) | `fetch_sub(1)` **— this path never calls `close_client`.** Miss it and a shutdown-with-pending-unix-handoffs leaks the count |

**Boot-time fd check** (our `adjustOpenFilesLimit` analogue). At boot, in `Server::init`:
`getrlimit(RLIMIT_NOFILE)`; require `rlim_cur >= maxclients + 32 + n_io*2` (the `+n_io*2` covers
per-thread listeners and rings, which Redis does not have). If short, try `setrlimit`; if that
fails, **lower `maxclients` and log at warning level using Redis's exact wording**, and if the
result would be <= 32, fail the boot loudly. A silent clamp here is the documented shape of a whole
class of "why did my benchmark stop at 1017 connections" incidents.

**Knob.**
```
--maxclients N   |   maxclients N   |   CONFIG SET maxclients N
Config: uint32_t maxclients = 10000;   min 1.  Live.
```

### 2.2 `tcp-backlog` and `tcp-keepalive`

#### `tcp-backlog`

Redis: default **511**, `IMMUTABLE_CONFIG` (`/home/user/Projects/redis/src/config.c:3415`), passed
straight to `listen()` via `anetListen` (`/home/user/Projects/redis/src/anet.c:519`), and governs
the unix socket too. `checkTcpBacklogSettings()` (`/home/user/Projects/redis/src/server.c:2784-2828`)
reads `/proc/sys/net/core/somaxconn` and warns — advisory only, nothing clamps:

```
WARNING: The TCP backlog setting of %d cannot be enforced because /proc/sys/net/core/somaxconn is set to the lower value of %d.
```

Ours is currently **hardcoded to 16384** in both listeners
(`src/core/io_loop.h:116` and `:132`), with the comment *"Backlog must hold a benchmark's whole
opening burst; capped by net.core.somaxconn anyway."*

**Decision rule, not a decision.** Adding the knob is unambiguous. Changing the *default* from 16384
to Redis's 511 is a behaviour change to an existing constant, and our gate opens 2048 connections
(memory: `epyc-p1-truth-and-geometry` — the p1 crossover is at 2048 conns). With `n_io` independent
listeners the per-listener burst is roughly `2048/n_io`, so 511 is very likely fine — but "very
likely" is not a measurement.

So: **add the knob with Redis's default of 511, then run the 2048-conn p1 cell as a PRE/POST A/B**
(memory: `user-prepost-table-rule` — every optimization lands with a PRE vs POST perf table; this is
a behaviour change and gets the same treatment). If connect latency or accept errors regress, the
default reverts to 16384 and the divergence is documented in `tomokv.conf`. Either outcome is
acceptable; shipping without the measurement is not.

Also port `checkTcpBacklogSettings` — read `/proc/sys/net/core/somaxconn` at boot and emit Redis's
warning verbatim. It costs one file read at boot and saves a support round-trip.

```
--tcp-backlog N  |  tcp-backlog N     Boot-only (IMMUTABLE, matching Redis). Default 511 pending the A/B.
```

#### `tcp-keepalive`

Redis: default **300** seconds, live config, but **applied once in `createClient`**
(`/home/user/Projects/redis/src/networking.c:128-134`) — so changing it at runtime does not
retro-apply to existing connections. Match that; it is simpler and it is the documented behaviour.

The derivation, verbatim from `anetKeepAlive`
(`/home/user/Projects/redis/src/anet.c:210-252`):

| sockopt | value |
|---|---|
| `SO_KEEPALIVE` | 1 |
| `TCP_KEEPIDLE` | `interval` |
| `TCP_KEEPINTVL` | `interval/3`, floored to 1 (`if (intvl == 0) intvl = 1;`) |
| `TCP_KEEPCNT` | `3` (hardcoded) |

Worst-case dead-peer detection ≈ `2 * interval` (600 s at the default), which is exactly what
`redis.conf:163-177` means by *"to close the connection the double of the time is needed"*.

**Hook:** `IoLoop::adopt_client` (`src/core/io_loop.h:312`), right beside the existing
`TCP_NODELAY` setsockopt, inside the same `if (!unix_socket)` guard. Failures are ignored (Redis
passes `err = NULL` at `socket.c:455`, silently swallowing them).

```
--tcp-keepalive N  |  tcp-keepalive N  |  CONFIG SET tcp-keepalive N
Default 300. 0 = off (no setsockopt at all -- 0 allocates nothing, per the knob rules).
```

### 2.3 `timeout` — and the IO cron beat we do not currently have

#### Redis semantics

`clientsCronHandleTimeout()` (`/home/user/Projects/redis/src/timeout.c:33-59`, verbatim):

```c
int clientsCronHandleTimeout(client *c, mstime_t now_ms) {
    time_t now = now_ms/1000;

    if (server.maxidletime &&
        /* This handles the idle clients connection timeout if set. */
        !(c->flags & CLIENT_SLAVE) &&   /* No timeout for slaves and monitors */
        !mustObeyClient(c) &&         /* No timeout for masters and AOF */
        !(c->flags & CLIENT_BLOCKED) && /* No timeout for BLPOP */
        !(c->flags & CLIENT_PUBSUB) &&  /* No timeout for Pub/Sub clients */
        (now - c->lastinteraction > server.maxidletime))
    {
        serverLog(LL_VERBOSE,"Closing idle client");
        freeClient(c);
        return 1;
    }
    ...
```

| Client class | Exempt? | How |
|---|---|---|
| Replica / MONITOR | yes | `!(c->flags & CLIENT_SLAVE)` |
| Master / AOF fake client | yes | `mustObeyClient(c)` (`/home/user/Projects/redis/src/server.c:3731-3733`) |
| Blocked (BLPOP etc.) | yes | `!(c->flags & CLIENT_BLOCKED)` |
| RESP2 subscriber | yes | `!(c->flags & CLIENT_PUBSUB)` |
| **In MULTI** | **NO** | there is no `CLIENT_MULTI` test — an idle open transaction **is** reaped |

Comparison is **strictly `>`** on whole seconds, so the earliest possible reap is at `timeout + 1`
seconds of idleness, plus up to one sweep period.

Config `timeout`, default **0 = disabled**, range 0..INT_MAX, live
(`/home/user/Projects/redis/src/config.c:3413`).

Sweep rate — `clientsCron()` (`/home/user/Projects/redis/src/server.c:1259-1273`):

```c
    int numclients = listLength(server.clients);
    int iterations = numclients/server.hz;

    if (iterations < CLIENTS_CRON_MIN_ITERATIONS)
        iterations = (numclients < CLIENTS_CRON_MIN_ITERATIONS) ?
                     numclients : CLIENTS_CRON_MIN_ITERATIONS;
```

`CLIENTS_CRON_MIN_ITERATIONS == 5` (`server.h:148`), `hz` default **10** (`server.h:119`), clamped
to `[1, 500]`. `dynamic-hz` (default yes) doubles `hz` while
`clients/hz > MAX_CLIENTS_PER_CLOCK_TICK (200)`, capped at 500
(`/home/user/Projects/redis/src/server.c:1581-1595`). The contract: **every client is visited about
once per second**, and per-tick work stays bounded as the client count grows.

Iteration is a **rotating-head walk**, not a cursor
(`/home/user/Projects/redis/src/server.c:1288-1304`):

```c
        head = listFirst(server.clients);
        c = listNodeValue(head);
        listRotateHeadToTail(server.clients);
```

(For the record: `server.clients_index` is a rax keyed by client id, used only for
`CLIENT KILL ID` lookups — it plays no part in the timeout sweep.)

#### Our implementation

**`last_interaction_s_` goes in hole H1** (§0.4) — the 4-byte gap between `rpos_` and `rbuf_`, in
the io-hot packed run. `uint32_t` seconds since boot: 136 years of range, and it keeps the field in
the same cache line as `rlen_`/`rpos_`, which the io thread already writes on every pass.

Written at exactly two places, both io-thread-local, both already on the path:
- `IoLoop::on_recv` (`src/core/io_loop.h:342`) after `commit_read`
- `WbEngine::on_send_complete` (`src/net/wb.h:169`) on a successful send

Reading the clock per event would be a `clock_gettime` per recv, which is unacceptable. Instead the
IO loop caches `cached_now_s_` once per loop iteration — exactly the pattern `ExLoop` already uses
with `cached_now_ms_ = realtime_ms();` at `src/core/ex_loop.h:63`. **One `clock_gettime` per loop
pass, zero per event.**

**The cron beat.** `IoLoop` has no timer. Add one, modelled on `ExLoop::blocking_beat_ms_`
(`src/core/ex_loop.h:85-90`) — a time-gated call inside the `Span busy` block of `run_loop`:

```cpp
if (__builtin_expect(srv_->client_cron_armed(), false) && cached_now_ms_ >= client_cron_beat_ms_) {
    did += client_cron_pass();
    client_cron_beat_ms_ = cached_now_ms_ + 100;   // hz == 10 equivalent
}
```

`client_cron_armed()` is one relaxed load, true only when `timeout != 0` **or** any COBL class has a
nonzero limit. With every limit off, the beat never runs and the whole subsystem is one
predicted-false test per loop iteration — not per op. That is the zero-cost-when-off proof.

**`client_cron_pass()`** walks `self_->clients()` — the per-IO full client vector
(`src/core/thread.h`, used by `adopt_client` at `src/core/io_loop.h:320`) — **not** `active_`, which
by construction excludes idle connections and is therefore exactly the wrong set.

Rate math, mirroring Redis with our own beat:

```cpp
const size_t n = self_->clients().size();
size_t visits = n / kClientCronBeatsPerSecond;              // 10
if (visits < kClientCronMinVisits)                          // 5
    visits = std::min(n, static_cast<size_t>(kClientCronMinVisits));
```

Iteration uses a **persistent index cursor** into the vector rather than Redis's list rotation —
our `clients()` is a `std::vector` with swap-with-back removal, so rotation is not available and a
cursor is the natural analogue. Clamp the cursor when the vector shrinks. Per-client work:

```cpp
Client* c = self_->clients()[cursor_];
if (timeout_s_ && !c->blocked() && !c->subscriber_mode() && !c->closing() &&
    (cached_now_s_ - c->last_interaction_s()) > timeout_s_) {
    close_client(c);        // idempotent; handles the quiescence fence and in-flight ops
    continue;               // do NOT advance the cursor past a swapped-in element
}
client_obuf_check(c, /*async=*/false);   // §2.4, the cron (synchronous-close) arm
```

Three correctness notes for the lane:
1. **`close_client` is already idempotent and already correct** for a client with in-flight ops
   (`src/core/io_loop.h:928-971`): it marks closing, `shutdown(SHUT_RDWR)` so pinned kernel buffers
   release, and only frees at the quiescence fence. Do not invent a second teardown.
2. `close_client` may remove the client from `self_->clients()` by swap-with-back, which moves a
   different element into the current index. Handle the cursor accordingly or you skip a client per
   reap.
3. Exemptions map cleanly: we have no replicas, no master link, and no fake clients. `blocked()`
   (`src/net/conn.h:415`) is `CLIENT_BLOCKED`; `subscriber_mode()` (`:413`) is `CLIENT_PUBSUB`.
   **MULTI is deliberately NOT exempt**, matching Redis — a client with `multi_session() != nullptr`
   still times out.

The `timeout` entry **already exists** in our CONFIG table as a stub
(`add_config("timeout", ConfigKind::Unsigned, 0);`, `src/cmd/t_server.cc:223`). This feature makes
it real. Wire it to a `Config::timeout` field and to the live-config seqlock so `CONFIG SET timeout`
takes effect without a restart.

```
--timeout N  |  timeout N  |  CONFIG SET timeout N        seconds; 0 = off (default)
```

### 2.4 `client-output-buffer-limit`

#### EXACT grammar

Registered as a **multi-arg special config**
(`/home/user/Projects/redis/src/config.c:3535`). The parser is
`updateClientOutputBufferLimit()` (`/home/user/Projects/redis/src/config.c:390-444`). Grammar:

```
client-output-buffer-limit <class> <hard limit> <soft limit> <soft seconds>
                          [<class> <hard limit> <soft limit> <soft seconds> ...]
```

Rules, each traceable to a line in that function:

| Rule | Source |
|---|---|
| Arguments are consumed in **groups of 4**. `arg_len % 4 != 0` rejects the whole directive with `"Wrong number of arguments in buffer limit configuration."` | `config.c:404-408` |
| **Multiple class groups on one line are legal** — the loop is `for (j = 0; j < arg_len; j += 4)` | `config.c:413` |
| **Multiple lines are also legal and MERGE**: only classes named in an invocation are written back (`if (classes[j]) server.client_obuf_limits[j] = values[j];`). Unmentioned classes keep their previous values. **This is a partial update, not a replacement.** | `config.c:439-441` |
| `<class>` resolved by `getClientTypeByName`; **unknown AND `master`** both rejected with `"Invalid client class specified in buffer limit configuration."` | `config.c:415-420` |
| `<hard>`/`<soft>` go through `memtoull` → memory suffixes accepted (`256mb`, `8mb`, `1gb`, `1k`). `0` disables that limit independently | `config.c:422-423` |
| `<soft seconds>` is `strtoll` base 10, must be `>= 0` and fully consumed (`*soft_seconds_eptr != '\0'` rejects trailing garbage) | `config.c:424, 426-427` |
| Validation is **all-or-nothing**: everything lands in a scratch `values[]` and is committed only after the whole vector validates. Comment: *"so that we either refuse the whole configuration string or accept it all, even if a single error in a single client class is present."* | `config.c:410-412` |

Class names, `getClientTypeByName` (`/home/user/Projects/redis/src/networking.c:5439-5446`) —
case-insensitive:

| Accepted name | Class | Accepted by COBL? |
|---|---|---|
| `normal` | `CLIENT_TYPE_NORMAL` (0) | yes |
| `slave` | `CLIENT_TYPE_SLAVE` (1) | yes |
| `replica` | `CLIENT_TYPE_SLAVE` (1) — alias | yes |
| `pubsub` | `CLIENT_TYPE_PUBSUB` (2) | yes |
| `master` | `CLIENT_TYPE_MASTER` (3) | **rejected** |

`getClientTypeName` (`networking.c:5448-5456`) is **asymmetric** — it emits `slave`, never
`replica`. So `CONFIG GET` returns `slave`; only `CONFIG REWRITE` patches it to `replica`
(`config.c:1521-1522`). Match this exactly, including the asymmetry, or config-management tooling
that round-trips through us will diff against Redis.

`CONFIG GET client-output-buffer-limit` returns **all three classes on one line, raw bytes**
(`config.c:3016-3030`), e.g.:

```
normal 0 0 0 slave 268435456 67108864 60 pubsub 33554432 8388608 60
```

#### Defaults, verbatim (`/home/user/Projects/redis/src/config.c:171-176`)

```c
clientBufferLimitsConfig clientBufferLimitsDefaults[CLIENT_TYPE_OBUF_COUNT] = {
    {0, 0, 0}, /* normal */
    {1024*1024*256, 1024*1024*64, 60}, /* slave */
    {1024*1024*32, 1024*1024*8, 60}  /* pubsub */
};
```

| Class | hard | soft | soft seconds |
|---|---|---|---|
| normal | 0 (off) | 0 (off) | 0 |
| slave / replica | 268435456 (256mb) | 67108864 (64mb) | 60 |
| pubsub | 33554432 (32mb) | 8388608 (8mb) | 60 |

#### The check — `checkClientOutputBufferLimits()`

`/home/user/Projects/redis/src/networking.c:5458-5517`. The semantics that must be reproduced:

1. **Unauthenticated hard cap of 1024 bytes**, checked *before* class resolution and *before* any
   config lookup, and **not configurable**:
   ```c
   if (used_mem > 1024 && authRequired(c))
       return 1;
   ```
   This ties feature 2 to feature 3 — see §3.6.
2. **Master is remapped to NORMAL**, not exempted:
   `if (class == CLIENT_TYPE_MASTER) class = CLIENT_TYPE_NORMAL;`
3. **Replica hard limit is floored at `repl_backlog_size`** — but the *enable* test still reads the
   raw configured value, so hard `0` still means "off" regardless of backlog size. (Moot for us: no
   replication. Keep the class parseable; the floor is not implementable and is not needed.)
4. **Hard is immediate**: `used_mem >= hard_limit_bytes` → trigger, no grace.
5. **Soft requires a continuous window, and the boundary is strictly `>`:**
   - first observation at/over soft → stamp `obuf_soft_limit_reached_time = server.unixtime`, do
     **not** trigger this pass;
   - later observations → trigger only when `elapsed > soft_limit_seconds` (the code suppresses
     while `elapsed <= soft_limit_seconds`), so `soft seconds 60` needs **more than** 60 s;
   - **any single observation below soft resets the clock to 0.**
6. Each limit is disabled independently by `0`.

#### The close — `closeClientOnOutputBufferLimitReached()`

`/home/user/Projects/redis/src/networking.c:5519-5556`. Two modes and two log lines:

```
Client %s scheduled to be closed ASAP for overcoming of output buffer limits.     (async == 1)
Client %s closed for overcoming of output buffer limits.                          (async == 0)
```

Both `LL_WARNING`, both interpolating the full `catClientInfoString()` dump. Stat:
`server.stat_client_outbuf_limit_disconnections++`, exposed in `INFO stats` as
**`client_output_buffer_limit_disconnections`** (`/home/user/Projects/redis/src/server.c:6825`).

Call sites split cleanly:

| Context | async |
|---|---|
| every reply-append path (`_addReplyPayloadToList` ×2, bulk-by-reference, `setDeferredReply`, reply-list transfer, replication writer) | **1** |
| `clientsCronRunClient` (`/home/user/Projects/redis/src/server.c:1222`) | **0** |

So enforcement is **push-driven on every buffer growth** (which is what catches hard limits
immediately) **plus a once-per-second cron sweep** (which is what actually fires soft-limit
expirations for a client that has gone quiet).

#### Measurement — and where we deliberately differ

Redis uses the **logical** size for enforcement
(`getClientOutputBufferLogicalSize`, `/home/user/Projects/redis/src/networking.c:5350-5359`):

```c
static size_t getClientOutputBufferLogicalSize(client *c) {
    size_t mem = getClientOutputBufferAllocSize(c);
    if (!clientTypeIsSlave(c))
        mem += c->reply_bytes_shared;
    return mem;
}
```

where `getClientOutputBufferAllocSize` (`:5325-5348`) is, for a normal client,
`c->reply_bytes + (sizeof(listNode)+sizeof(clientReplyBlock)) * listLength(c->reply)` — reply-list
bytes **plus per-node bookkeeping**. The static `c->buf`/`c->bufpos` is **excluded**.

Our three staging areas (`src/net/conn.h`):

| Area | Bytes pending |
|---|---|
| fill buffer | `fill_buf().size()` |
| send buffer | `send_buf().size() - wsent_` |
| segment queue | sum of `ReplySegment::len` minus the head's `offset_` |

**Divergence, taken deliberately:** we **do** count the fill/send buffers, i.e. our analogue of
`c->buf`. Redis can exclude `c->buf` because it is a fixed `PROTO_REPLY_CHUNK_BYTES` allocation that
cannot grow. Ours is a `SmallBuf<512>` that **heap-grows without bound**
(`src/net/conn.h:318-319`, shed only at quiescence via `kWbufShed`). Excluding it would leave the
limit unenforceable for exactly the workload it exists to bound. Write this divergence into
`tomokv.conf` and into `NOTES-*` when the lane lands.

We **do** count `Borrow` segment bytes. They are zero-copy pointers into FlatStore
(`src/net/conn.h:124-129`), so no copy exists — but they pin store memory and they are pending
output, which is precisely what the limit is for. This is the same reasoning as Redis's
`reply_bytes_shared` inclusion, whose comment reads: *"we still account this shared memory towards
this client's output buffer usage."*

**Counter, not a walk.** Summing the segment queue is O(segments). Keep a running
`uint64_t obuf_bytes_` in hole H2 (§0.4), maintained at exactly the sites that already move bytes:

| Site | Delta |
|---|---|
| `Client::append_buf_segment` / `append_borrow_segment` / `append_static_segment` (`conn.h:344-351`) | `+= len` |
| `Client::consume_segments` (`conn.h:364`) | `-= bytes` |
| `Client::release_all_segments` (`conn.h:368`) | `= 0` after the release |
| `SmallBuf` appends into `fill_buf()` and `commit_raw` (via `WbEngine::serve`, `wb.h:105-113`) | `+= len` |
| `Client::commit_write` (`conn.h:323`) | `-= n` |
| `Client::swap_buffers` (`conn.h:328`) | no change — bytes move, they do not leave |

`obuf_soft_since_s_` (also H2) is the `obuf_soft_limit_reached_time` analogue; `0` means "not over
soft". Both live in the cold tail because they are touched at retire and at cron, not per byte.

#### Hook points in our architecture

| Redis context | Our equivalent | async |
|---|---|---|
| every reply-append path | `WbEngine::serve`, after the `rob().drain(...)` lambda completes and before `pump(c)` (`src/net/wb.h:115-116`) — one check per serve, not one per op | 1 → `c->mark_closing()` |
| `clientsCronRunClient` | `IoLoop::client_cron_pass()` (§2.3) | 0 → `close_client(c)` |

The async form must **not** free the client — our `close_client` already is the async form: it marks
closing, shuts the socket down, and defers the free to the quiescence fence
(`src/core/io_loop.h:928-971`). One teardown path, used from both contexts, differing only in
whether the caller then continues touching the client.

**Class resolution.** We have neither replicas nor a master link, so only two classes can ever be
selected: `subscriber_mode()` → `pubsub`, else `normal`. The `slave`/`replica` class must still
**parse, store, and round-trip** — an ops script or a config-management tool will set it, and
rejecting it would be a gratuitous incompatibility. It simply never selects.

**`CLIENT NO-EVICT` does not exempt a client from COBL.** That flag only gates `maxmemory-clients`
eviction (`clientEvictionAllowed`, `/home/user/Projects/redis/src/server.c:1099-1105`). This is a
frequent misconception; if we implement `CLIENT NO-EVICT` as a no-op (we already accept it —
`src/cmd/t_server.cc`, the `NO-EVICT` arm replies `+OK`), that stays correct.

**Out of scope: `maxmemory-clients`.** Redis 8 has it (default 0/off,
`/home/user/Projects/redis/src/config.c:3500`), but it is an orthogonal mechanism with a different
accounting basis (total client memory: query buffer + argv + reply buffers + pubsub/tracking/MULTI
overhead) and its own opt-out flag. Conflating it with COBL would be misleading. Not in Wave A.

```
--client-output-buffer-limit "normal 0 0 0 pubsub 32mb 8mb 60"
client-output-buffer-limit normal 0 0 0
client-output-buffer-limit pubsub 32mb 8mb 60
CONFIG SET client-output-buffer-limit "normal 0 0 0"
```

Note the conf-file loader (`load_conf_file`, `src/core/config.h:309-339`) turns `name v1 v2 ...`
into `--name v1 v2 ...`, so a multi-token value already flows through. The CLI parser must consume
**all** trailing tokens for this flag, not just one — it is the first multi-token knob in the file
and `next()` (`src/core/config.h:134`) takes exactly one. Extend it or the knob silently truncates.

### 2.5 Hot-path budget

| Mechanism | Cost when off | Cost when on |
|---|---|---|
| `maxclients` | one relaxed load per **accept** | + one `fetch_add`/`fetch_sub` per connection lifetime |
| `tcp-backlog` | zero — one boot-time `listen()` argument | zero |
| `tcp-keepalive` | zero (guarded by `!= 0`, no syscall) | 4 `setsockopt` per accept |
| `timeout` | one predicted-false test per **loop iteration** | one `clock_gettime` per loop iteration; `max(5, n/10)` client visits per 100 ms beat |
| COBL | one predicted-false test per **serve** | + counter maintenance at ~6 byte-moving sites |

**None of this needs the multi2 out-of-line pattern.** Every hook is either on a rare path (accept),
on a per-loop-iteration path (cron), or is a single compare on a path that already does substantial
work (serve). The counter maintenance is inline arithmetic on a line already being written. Compare
with feature 1, which lands on the per-op write path and therefore does need it (§5.2).

The single field that touches a genuinely hot line is `last_interaction_s_` — one store per recv
completion and one per send completion, into hole H1, which sits in the io-hot cache line that is
already dirty at both of those points. That is why H1 was chosen for it rather than the cold tail.

### 2.6 Test plan — limit-enforcement arms

`tests/limits.py HOST PORT`, same `Conn` harness as `tests/pubsub.py`. Several arms need a
purpose-booted server, so the driver should support `--boot` args the way `tests/gate.sh` does.

**maxclients**

| Arm | Assertion |
|---|---|
| L1 | Boot `--maxclients 20`. Open 20 connections and `PING` each successfully |
| L2 | The 21st connection receives exactly `-ERR max number of clients reached\r\n` and is then closed by the peer (read returns EOF) |
| L3 | `INFO stats` shows `rejected_connections` advanced by exactly the number of rejects (**the non-vacuous assertion** — memory: `thredis-vacuous-validation-trap`) |
| L4 | Close 5 connections, confirm 5 more are admitted — proves the decrement fires |
| L5 | Slop bound: with `n_io` IO threads, open 200 connections concurrently against `--maxclients 20`; assert admitted count is in `[20, 20 + n_io]`. This **tests the documented `±n_io` bound rather than pretending it does not exist** |
| L6 | `CONFIG SET maxclients 40` raises the ceiling live; `CONFIG GET` reflects it |
| L7 | Boot with an artificially low `ulimit -n`; assert the server either clamps `maxclients` **and logs Redis's exact wording**, or fails the boot loudly. Assert it never silently accepts fewer connections than it advertises |

**timeout**

| Arm | Assertion |
|---|---|
| T1 | `CONFIG SET timeout 2`; an idle connection is closed within `[2, 4]` seconds (the `>` boundary plus one 100 ms beat, with slack) |
| T2 | A connection PINGing every 500 ms is **never** closed over 10 s |
| T3 | `timeout 0` (default): an idle connection survives 10 s |
| T4 | A `SUBSCRIBE`d connection is **exempt** — idle for `3 * timeout`, still alive |
| T5 | A `BLPOP key 0` connection is **exempt** — same |
| T6 | A connection that sent `MULTI` and went idle **IS reaped** (this is the counter-intuitive Redis behaviour and the arm that catches an over-eager exemption) |
| T7 | Live flip: `CONFIG SET timeout 1` on a running server with idle connections starts reaping them |
| T8 | Scale: 2000 idle connections with `timeout 2` — all reaped within ~3 s, and the IO loop's `busy_ns` does not spike (the beat is budgeted, not a stampede). Assert `INFO` connected_clients returns to the expected floor |
| T9 | A timed-out connection with an in-flight op does not crash — drive a slow command, force the timeout, run under ASAN |

**tcp-keepalive / tcp-backlog**

| Arm | Assertion |
|---|---|
| K1 | With `--tcp-keepalive 60`, read back the socket options on the server side (`getsockopt` via a small helper, or `ss -o` on the connection) and assert `KEEPIDLE == 60`, `KEEPINTVL == 20`, `KEEPCNT == 3` |
| K2 | `--tcp-keepalive 1` → `KEEPINTVL == 1` (the `if (intvl == 0) intvl = 1` floor, not 0) |
| K3 | `--tcp-keepalive 0` → `SO_KEEPALIVE` is **off** (no syscall was made) |
| K4 | `CONFIG SET tcp-backlog` is **rejected** as immutable |
| K5 | Boot with `tcp-backlog` above `somaxconn`; assert the warning is logged verbatim |
| K6 | **The A/B**: 2048-conn p1 cell at backlog 511 vs 16384, PRE/POST table, connect-latency and `accept_err` compared. This arm decides the default (§2.2) |

**client-output-buffer-limit**

| Arm | Assertion |
|---|---|
| C1 grammar | `CONFIG SET client-output-buffer-limit "normal 0 0 0 pubsub 32mb 8mb 60"` (two groups, one directive) is accepted |
| C2 grammar | `arg_len % 4 != 0` is rejected: `"... normal 0 0"` errors |
| C3 grammar | `master` class rejected; unknown class rejected |
| C4 grammar | Suffixes: `256mb`, `8mb`, `1gb`, `1k` all parse to the right byte counts (read back via `CONFIG GET`) |
| C5 grammar | Trailing garbage in soft-seconds (`60x`) rejected; negative soft-seconds rejected |
| C6 grammar | **Merge semantics**: set `pubsub` alone, confirm `normal` retains its previous value |
| C7 grammar | **All-or-nothing**: a directive with a valid `normal` group and an invalid `pubsub` group leaves **both** unchanged |
| C8 grammar | `CONFIG GET` emits all three classes, raw bytes, and the class name is **`slave`** not `replica` |
| C9 defaults | A fresh boot reports exactly `normal 0 0 0 slave 268435456 67108864 60 pubsub 33554432 8388608 60` |
| C10 hard | `pubsub` hard 64kb; subscribe, stop reading, publish a flood; the subscriber is killed **promptly** (no soft-seconds grace) and `client_output_buffer_limit_disconnections` advances by 1 |
| C11 soft-no-trigger | soft 64kb / seconds 3; sit **just over** soft for 1 s then drain below; connection **survives** and the clock resets |
| C12 soft-trigger | soft 64kb / seconds 2; sit over soft for >2 s; connection is killed |
| C13 soft-boundary | soft seconds 2 with exactly 2 s over the limit → **survives** (the `>` boundary, not `>=`) |
| C14 disabled | `normal 0 0 0`: a normal client that never reads accumulates a large buffer and is **not** killed. Bound the arm so it does not OOM the box |
| C15 zc/borrow | With `zc-min` active, a flood of large `GET`s to a non-reading client trips the limit — proving **borrow bytes are counted** and that the kill path returns every outstanding borrow (assert no leak; run under ASAN) |
| C16 class | The same connection is `normal` before `SUBSCRIBE` and `pubsub` after — set wildly different limits per class and prove the effective limit changes at the subscribe boundary |
| C17 unauth | With `requirepass` set, an unauthenticated client that accumulates >1024 bytes of output is killed regardless of config (§3.6). Ties features 2 and 3 |
| C18 log | Both log lines appear verbatim, and the async form is used from the reply path while the sync form is used from cron |
| C19 accounting | A direct-reply (`op.direct`) path and a segment path both move `obuf_bytes_` correctly; assert it returns to **0** on a fully drained connection. This is the counter-drift arm and it is the one most likely to fail |

**Cross-cutting**

| Arm | Assertion |
|---|---|
| Z1 | `tests/gate.sh quick` still passes with every limit at its default |
| Z2 | Idle-CPU ceiling (gate.sh section 3) is unchanged with `timeout 0` — proves the beat is genuinely off |
| Z3 | Idle-CPU ceiling with `timeout 300` and 2000 connections stays under the gate's threshold — proves the beat is budgeted |
| Z4 | `sizeof(Client) == 1984` and `sizeof(Op) == 336` still hold |

---

## 3. FEATURE 3 — requirepass / AUTH / protected-mode / HELLO AUTH

### 3.1 What already exists in our tree

| Piece | Location | Current behaviour |
|---|---|---|
| `AUTH` command row | `src/cmd/t_server.cc:899` — `{"AUTH", 2, 3, CmdFlags::ConnLocal, cmd_auth, 0,0,0}` | arity is already correct (2 or 3) |
| `cmd_auth` | `src/cmd/t_server.cc:362-366` | **always** errors with the no-password-configured message |
| `HELLO` row | `src/cmd/t_server.cc:900` — `{"HELLO", 1, 2, ...}` | **max_arity 2 — must widen to `-1`** for `HELLO 2 AUTH u p` |
| `cmd_hello` | `src/cmd/t_server.cc:367-387` | protover 2 only, `-NOPROTO` otherwise; 14-element RESP2 array |
| `cmd_reset` | `src/cmd/t_server.cc:405-417` | clears name/lib/db, replies `+RESET` |
| `cmd_quit` | `src/cmd/t_server.cc:418-421` | `+OK`, `mark_closing()` |

**The existing `cmd_auth` error string is already byte-exact** with Redis 8
(`/home/user/Projects/redis/src/acl.c:3373-3375`). Keep it verbatim:

```
ERR AUTH <password> called without any password configured for the default user. Are you sure your configuration is correct?
```

> Do **not** use the Redis ≤5 wording `"Client sent AUTH, but no password is set..."`. A repo-wide
> grep for `Client sent AUTH` in the reference tree returns zero hits; that string was replaced in
> Redis 6.0.

### 3.2 Command/reply semantics

`authCommand` (`/home/user/Projects/redis/src/acl.c:3352-3403`). Our reduced form — we have no ACL
system, one user (`default`), one password:

| Input | Reply |
|---|---|
| `AUTH <pass>`, no `requirepass` set | `-ERR AUTH <password> called without any password configured for the default user. Are you sure your configuration is correct?` |
| `AUTH <pass>`, correct | `+OK`, and `c->authenticated_ = true` |
| `AUTH <pass>`, wrong | `-WRONGPASS invalid username-password pair or user is disabled.` |
| `AUTH default <pass>`, correct | `+OK` |
| `AUTH default <pass>`, wrong | `-WRONGPASS invalid username-password pair or user is disabled.` |
| `AUTH <other-user> <pass>` | `-WRONGPASS invalid username-password pair or user is disabled.` (never "no such user" — see 3.3) |
| `AUTH a b c` | arity error from the existing `command_arity_ok` path (`max_arity == 3`) |

The `WRONGPASS` string is verbatim from `addAuthErrReply`
(`/home/user/Projects/redis/src/acl.c:1508-1515`), trailing period included. It is written with a
leading `-` in Redis so `addReplyError` does not prepend `-ERR`; our `reply_err`
(`src/net/resp.h:144`) writes the message as given, so pass it **without** a leading `-` and the
wire bytes come out as `-WRONGPASS invalid username-password pair or user is disabled.\r\n`.

Two Redis behaviours deliberately **not** ported, each with a reason:
- `AUTH "internal connection" <secret>` (`acl.c:3386-3392`) is a cluster-only side channel. We have
  no cluster. Treat `internal connection` as an ordinary (failing) username.
- The `if (clientHasPendingReplies(c)) return;` suppression in `addAuthErrReply` (`acl.c:1509`)
  exists for module-auth reentrancy. We have no modules; always reply.

### 3.3 Password storage and the anti-enumeration property

Redis stores a **SHA-256 hex digest** (64 chars, `ACLHashPassword`,
`/home/user/Projects/redis/src/acl.c:210-225`) and compares with `time_independent_strcmp` over a
fixed 64-byte length — no early exit.

**Port both properties.** They are cheap and they are the whole security value:

1. Hash the candidate once, compare digests with a constant-time compare over a fixed length.
   Comparing plaintext with `memcmp` leaks the length and the matching prefix through timing.
2. **`ENOENT` (no such user) and `EINVAL` (bad password) collapse to the same `WRONGPASS` reply.**
   Redis sets `errno` to distinguish them (`ACLCheckUserCredentials`,
   `/home/user/Projects/redis/src/acl.c:1470-1504`) and the caller **discards it**. That is
   deliberate: it prevents username enumeration. Do not "improve" the error message.

**There is no rate limiting or auth-failure delay in Redis** — grep for `usleep|sleep|delay` across
`acl.c` returns nothing. A failed AUTH costs one SHA-256 and returns. Do not add throttling; it
would be a divergence, not a port. If we ever want it, it is a separate, named decision.

Counter to expose in `INFO stats`, mirroring
`server.acl_info.user_auth_failures` (`/home/user/Projects/redis/src/acl.c:2756-2759`,
INFO field `acl_access_denied_auth` at `server.c:6256`): `auth_failures`.

### 3.4 `requirepass` config

Registration (`/home/user/Projects/redis/src/config.c:3373`):
```c
createSDSConfig("requirepass", NULL, MODIFIABLE_CONFIG | SENSITIVE_CONFIG, EMPTY_STRING_IS_NULL, server.requirepass, NULL, NULL, updateRequirePass),
```

The state machine, from `ACLUpdateDefaultUserPassword`
(`/home/user/Projects/redis/src/acl.c:3405-3416`) plus the `EMPTY_STRING_IS_NULL` conversion at
`config.c:1940`:

| `requirepass` value | Effect |
|---|---|
| non-empty `"secret"` | password list = `{sha256(secret)}`, NOPASS cleared → **auth required** |
| `""` (empty) | converted to NULL → `nopass` → **auth disabled** |
| unset at startup | same as `""` |

Ours:

```cpp
// src/core/config.h, struct Config, ---- security (live via CONFIG SET) ----
// Empty or unset = no authentication, and off costs one predicted-false test per parsed command.
const char* requirepass = nullptr;
uint32_t    protected_mode = 1;      // numeric per the knob rules; 1 = on, matching Redis's default
```

`SENSITIVE_CONFIG` semantics matter: `requirepass` must **not** appear in any log line, in
`CLIENT INFO`/`CLIENT LIST`, or in a crash dump. `CONFIG GET requirepass` returns the cleartext in
Redis (it keeps `server.requirepass` for exactly that backward-compatibility reason,
`/home/user/Projects/redis/src/server.h:2683-2685`) — match that, but keep the value out of
everything else. Also redact it from any command-echo path we add later.

Live publication rides the **existing** seqlock (`Server::begin_live_config_update` /
`end_live_config_update`, `src/core/server.h:523-537`), same as `notify_events` in §1.2:
`live_requirepass_hash_` (32 bytes) plus `live_auth_required_` (a bool). The per-pass reader is the
IO loop, latching once per pass, so the parse path reads a **loop-local** bool.

### 3.5 The pre-dispatch gate — and why it is one branch when off

#### Redis

`processCommand` (`/home/user/Projects/redis/src/server.c:4583-4590`):

```c
    if (authRequired(c)) {
        /* AUTH and HELLO and no auth commands are valid even in
         * non-authenticated state. */
        if (!(c->cmd->flags & CMD_NO_AUTH)) {
            rejectCommand(c,shared.noautherr);
            return C_OK;
        }
    }
```

`shared.noautherr` is a preformatted object including the CRLF
(`/home/user/Projects/redis/src/server.c:2256-2257`):

```
-NOAUTH Authentication required.\r\n
```

`authRequired()` (`/home/user/Projects/redis/src/networking.c:111-120`) — **read the operand order,
it is the whole performance story**:

```c
int authRequired(client *c) {
    /* Check if the user is authenticated. This check is skipped in case
     * the default user is flagged as "nopass" and is active. */
    uint32_t default_flags;
    atomicGet(DefaultUser->flags, default_flags);
    int auth_required = (!(default_flags & USER_FLAG_NOPASS) ||
                          (default_flags & USER_FLAG_DISABLED)) &&
                        !c->authenticated;
    return auth_required;
}
```

With no password set, `!(flags & NOPASS)` is 0 and `(flags & DISABLED)` is 0, so the `||` yields 0
and C's `&&` **short-circuits before loading `c->authenticated` at all**. The disabled path never
touches the per-client cache line. Reordering these operands for readability destroys the property.

Commands allowed pre-auth carry `CMD_NO_AUTH` (`#define CMD_NO_AUTH (1ULL<<15)`,
`/home/user/Projects/redis/src/server.h:253`). A `grep -c` over `commands.def` returns **4** —
exactly **AUTH, HELLO, QUIT, RESET**, and nothing else.

#### Ours

Add the flag beside the existing ones in `src/cmd/command.h:21-48`:

```cpp
static constexpr uint32_t NoAuth = 1u << 15;   // legal before AUTH: AUTH, HELLO, QUIT, RESET
```

and set it on those four rows in `src/cmd/t_server.cc:899-902`.

`Client` gains `bool authenticated_` in **hole H2** (§0.4). The gate goes in
`IoLoop::parse_and_dispatch` (`src/core/io_loop.h:357-740`), immediately **after** the arity check
(`:389-402`) and **before** the MULTI branch (`:404`):

```cpp
// One predicted-false test when no password is configured. auth_required_ is latched per
// parse pass from the live-config seqlock, so this is a LOOP-LOCAL load -- no shared line,
// no atomic. The client bit is read second, exactly as redis orders it, so the disabled
// path never touches the Client's cold tail.
if (__builtin_expect(auth_required_ && !c->authenticated(), false)) {
    if (!(spec->flags & CmdFlags::NoAuth)) {
        conn.advance_parse(consumed);
        finish_locally(c, *op, "NOAUTH Authentication required.");
        continue;
    }
}
```

Placement matters for error precedence: an unauthenticated client must still get
`ERR unknown command` and the arity error **ahead of** `NOAUTH`, because that is what Redis does
(the gate sits after lookup and arity in `processCommand`). It is a minor information leak that
Redis accepts deliberately; diverging would show up in a differential test.

**Zero-cost-when-off claim:** with `requirepass` unset, `auth_required_` is a loop-local `false`, so
the branch is a register test that the predictor gets right every time and the client field is never
loaded. Prove it with `instr/op` on a p32 GET/SET cell (§5.3), do not assert it.

#### Latching, and the surprise it creates

`clientSetDefaultAuth` (`/home/user/Projects/redis/src/networking.c:103-109`) computes
`c->authenticated` **at connect time**:

```c
    c->authenticated = (c->user->flags & USER_FLAG_NOPASS) &&
                       !(c->user->flags & USER_FLAG_DISABLED);
```

So a client that connected while no password was set has `authenticated == 1` **latched**. If
`CONFIG SET requirepass` is then issued, that pre-existing connection is **not** retroactively
deauthenticated — only new connections are gated.

**Match this.** It is real Redis behaviour, it is a frequent source of confusion, and diverging
would break a differential test. Set `authenticated_` in `IoLoop::adopt_client`
(`src/core/io_loop.h:312`) from the current global state, and never revisit it except via AUTH,
RESET, and HELLO AUTH. Write the behaviour into `tomokv.conf` next to the knob.

### 3.6 Unauthenticated hardening

Redis sharply tightens limits for clients that have not authenticated. Each is one `authRequired(c)`
test on a path that already does work:

| Limit | Value | Site |
|---|---|---|
| output buffer | 1024 bytes, **not configurable** | `checkClientOutputBufferLimits`, `/home/user/Projects/redis/src/networking.c:5469-5470` |
| multibulk count | reject `> 10` | `/home/user/Projects/redis/src/networking.c:3397` |
| bulk length | reject `> 16384` | `/home/user/Projects/redis/src/networking.c:3480` |
| parser lookahead | forced to 1 | `/home/user/Projects/redis/src/networking.c:3792` |
| accumulated MULTI state | 1 MB cap | `/home/user/Projects/redis/src/networking.c:4124` |
| client memory | 1 KB cap | `/home/user/Projects/redis/src/networking.c:5470` |

**Ship at minimum the 1024-byte output-buffer cap** (it is the one that closes the
"connect, never authenticate, never read, make the server buffer" hole) and the multibulk/bulk
caps in `resp_parse` (`src/net/resp.h`) — those two bound an unauthenticated peer's ability to make
us allocate. Our read buffer already grows to `kRbufHardCap == 512MB + 64KB`
(`src/net/conn.h:67`) for a single oversized command, which an unauthenticated peer can drive today.
That is the strongest reason in this section to do the parser caps rather than defer them.

The remaining three are lower value for us and may be deferred; if deferred, **say so in the notes**
rather than leaving the gap implicit.

### 3.7 protected-mode

#### The condition is TWO clauses, not four

The check lives in `clientAcceptHandler`
(`/home/user/Projects/redis/src/networking.c:1663-1698`), **not** in `processCommand`, and it does
**not** test the `bind` config at all:

```c
    if (server.protected_mode &&
        DefaultUser->flags & USER_FLAG_NOPASS)
    {
        if (connIsLocal(conn) != 1) {
```

1. `protected-mode` is on (bool config, **default 1**,
   `/home/user/Projects/redis/src/config.c:3293`), **AND**
2. no password is set by any route, **AND**
3. the connection is not local.

The surrounding comment still claims *"nor a specific interface is bound"* — that is a **stale
comment**, an upstream documentation bug. Do not port the claim.

`connIsLocal` (`/home/user/Projects/redis/src/connection.h:322-328`) returns `-1` when the
connection type has no `is_local` handler, and the test is `!= 1`, so an unknown answer is treated
as **non-local** — fail closed. Unix sockets and loopback TCP return 1.

The rejection message, verbatim (`/home/user/Projects/redis/src/networking.c:1673-1692`), sent as a
single raw write:

```
-DENIED Redis is running in protected mode because protected mode is enabled and no password is set for the default user. In this mode connections are only accepted from the loopback interface. If you want to connect from external computers to Redis you may adopt one of the following solutions: 1) Just disable protected mode sending the command 'CONFIG SET protected-mode no' from the loopback interface by connecting to Redis from the same host the server is running, however MAKE SURE Redis is not publicly accessible from internet if you do so. Use CONFIG REWRITE to make this change permanent. 2) Alternatively you can just disable the protected mode by editing the Redis configuration file, and setting the protected mode option to 'no', and then restarting the server. 3) If you started the server manually just for testing, restart it with the '--protected-mode no' option. 4) Set up an authentication password for the default user. NOTE: You only need to do one of the above things in order for the server to start accepting connections from the outside.
```

followed by `\r\n`. Then `server.stat_rejected_conn++` and `freeClientAsync(c)`.

> The message says "Redis". Keep it. A client library or an operator runbook may match on it, and
> the point of a parity feature is that existing tooling works unchanged. (Our `HELLO` already
> answers `server: redis` — `src/cmd/t_server.cc:381`.)

#### Ours

Enforce in `IoLoop::on_accept`, **after** the `maxclients` check (§2.1) and before
`new Client(fd)` — same shape, same best-effort write, same close, same `rejected_conns_` counter:

```cpp
if (__builtin_expect(srv_->protected_mode() && !srv_->auth_required_global() && !unix_socket, false)) {
    if (!peer_is_loopback(fd)) {                 // getpeername + 127.0.0.0/8 test; ::1 too
        ::send(fd, kDeniedMsg, sizeof(kDeniedMsg) - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
        ::close(fd);
        srv_->note_rejected_conn();
        ...
        return;
    }
}
```

We already have `peer_address()` (`src/core/io_loop.h:300-310`) doing `getpeername` +
`inet_ntop`; factor the loopback test out of it rather than formatting a string on the reject path.
AF_UNIX is always local.

Note our default `bind_addr` is `"127.0.0.1"` (`src/core/config.h:52`), so protected-mode is
normally unreachable. It matters exactly when someone binds `0.0.0.0` — which is the case it exists
for.

```
--protected-mode 0|1   |   protected-mode 0|1   |   CONFIG SET protected-mode 0|1
Default 1. Numeric, per the knob rules (Redis spells it yes/no; accept both, emit the numeric form
via CONFIG GET only if that matches our existing Bool ConfigKind -- see the note below).
```

**Grammar decision:** our `ConfigKind::Bool` already normalizes to `yes`/`no`
(`src/cmd/t_server.cc:270-274`), and `appendonly` already uses it. Use `ConfigKind::Bool` for
`protected-mode` so `CONFIG GET` returns `yes`/`no` exactly as Redis does. The knob-rules preference
for numerics yields here to wire compatibility — and the CLI/conf grammar can accept `0|1|yes|no`.

### 3.8 HELLO with AUTH

`helloCommand` (`/home/user/Projects/redis/src/networking.c:5032-5129`). **The ordering is
load-bearing** and is the single most likely thing to get wrong.

```
1. parse protover (optional)
     non-integer         -> "ERR Protocol version is not an integer or out of range"; return
     out of range        -> "-NOPROTO unsupported protocol version"; return
                            (leading '-', so NO "-ERR" prefix, and NO trailing period)
2. parse options loop:  AUTH <user> <pass> | SETNAME <name>
     AUTH with < 2 remaining args  -> "ERR Syntax error in HELLO option 'AUTH'"  (NOT an arity error)
     unknown option                -> "ERR Syntax error in HELLO option '<opt>'"
     repeated AUTH                 -> last wins, silently
3. if (username && password): authenticate
     failure -> reply WRONGPASS and RETURN.  Do NOT apply protover.  Do NOT apply SETNAME.
4. if (!authenticated) -> reply the long HELLO-specific NOAUTH message and return
5. ONLY NOW apply protover and SETNAME, then emit the reply IN THE NEW PROTOCOL
```

Step 3's consequence, spelled out because it is a protocol-desync bug if missed: **a failed
`HELLO 3 AUTH bad creds` must leave the connection on RESP2.** In Redis `c->resp = ver` is at
`networking.c:5101`, after both guards. For us the analogue is: reject `HELLO 3` before or after,
but never partially apply anything on an auth failure.

Step 4's message is **distinct** from `shared.noautherr`
(`/home/user/Projects/redis/src/networking.c:5090-5094`), verbatim:

```
-NOAUTH HELLO must be called with the client already authenticated, otherwise the HELLO <proto> AUTH <user> <pass> option can be used to authenticate the client and select the RESP protocol version at the same time
```

Note it tests `c->authenticated` **directly**, not `authRequired(c)`.

**Changes required in our tree:**
- `{"HELLO", 1, 2, ...}` → `{"HELLO", 1, -1, ...}` (`src/cmd/t_server.cc:900`) plus
  `CmdFlags::NoAuth`. Without the arity widening `HELLO 2 AUTH u p` fails with an arity error before
  `cmd_hello` ever runs.
- `cmd_hello` grows the option loop. `SETNAME` should reuse the existing
  `valid_client_text()` validation from `CLIENT SETNAME` (`src/cmd/t_server.cc:435-441`) and write
  into the same `g_clients` metadata map, so the two paths cannot drift.
- We accept protover 2 only. `HELLO 3` continues to answer `-NOPROTO unsupported protocol version`
  — this is **deliberate and load-bearing**: `NOTES-COMPAT.md:17` records that go-redis probes with
  `HELLO 3` and uses the error to fall back to RESP2. Do not change it.

### 3.9 RESET

`resetCommand` (`/home/user/Projects/redis/src/networking.c:4448-4462`) delegates to
`clearClientConnectionState`, which calls `clientSetDefaultAuth(c)` at
`/home/user/Projects/redis/src/networking.c:2168`. So:

| requirepass | Effect of RESET on auth |
|---|---|
| set | `authenticated = 0` — the client must re-AUTH |
| unset | `authenticated = 1` — no-op |

Reply is `+RESET` (a simple status, not `+OK`) — which our `cmd_reset` already emits
(`src/cmd/t_server.cc:416`).

Also note for feature 4: `clearClientConnectionState` calls `clearClientPubSubState(c)`
(`/home/user/Projects/redis/src/networking.c:2166`) **before** flipping the ACL identity, and does
so with `notify == 0` — so RESET drops subscriptions **silently**, emitting no `unsubscribe` /
`sunsubscribe` pushes. Our `pubsub_start_reset` (`src/core/pubsub.inc:684-695`) already matches this
shape; §4 extends it to shard channels.

Distinct from `deauthenticateAndCloseClient` (`/home/user/Projects/redis/src/networking.c:2186-2207`),
which hard-clears `authenticated` and kills the connection — that is the `ACL DELUSER` path and we
have no ACL system.

### 3.10 Hot-path budget

| Path | Off (`requirepass` unset) | On |
|---|---|---|
| per parsed command | one loop-local register test, predicted-not-taken. **The client field is not loaded.** | + one cold-tail bool load per command until authenticated |
| accept | one relaxed load for protected-mode (default on, but the second clause is false when a password is set, and the whole test is predicted-false) | + one `getpeername` on the reject path only |
| AUTH itself | n/a | one SHA-256 over the supplied password + one 32-byte constant-time compare. Not a hot path |

**No multi2 out-of-line pattern needed.** The gate is a single conditional in
`parse_and_dispatch`; there is no state machine and no lifetime to keep out of the loop. Contrast
with `multi_dispatch_entry` (`src/core/io_loop.h:404-407`), which guards a whole transaction
subsystem — that is what the pattern is for, and auth is not that.

### 3.11 Test plan — auth state machine arms

`tests/auth.py HOST PORT`, plus purpose-booted arms.

**No password configured (the default)**

| Arm | Assertion |
|---|---|
| A1 | `AUTH secret` → the exact no-password-configured error |
| A2 | `AUTH default secret` → same error class per Redis's 3-arg path (it goes to `ACLAuthenticateUser` against a `nopass` user, which **succeeds**). **Verify against the vanilla oracle** — this arm is subtle and the differ should own it |
| A3 | `GET k`, `SET k v` etc. all work with no AUTH |
| A4 | `INFO`, `CONFIG GET`, `PING` all work |

**Password configured**

| Arm | Assertion |
|---|---|
| A5 | Boot `--requirepass hunter2`. A fresh connection issuing `GET k` gets exactly `-NOAUTH Authentication required.` |
| A6 | `AUTH hunter2` → `+OK`; the same connection can now `GET`/`SET` |
| A7 | `AUTH wrong` → exactly `-WRONGPASS invalid username-password pair or user is disabled.` and the connection remains **unauthenticated** and **open** |
| A8 | `AUTH default hunter2` → `+OK` |
| A9 | `AUTH nosuchuser hunter2` → `WRONGPASS` — **the same message as a bad password** (anti-enumeration) |
| A10 | Pre-auth allowlist: `AUTH`, `HELLO`, `QUIT`, `RESET` all work unauthenticated; **everything else** returns NOAUTH. Iterate the whole command registry to prove this rather than spot-checking four commands |
| A11 | Error precedence: unauthenticated `NOSUCHCOMMAND` → `ERR unknown command` (**not** NOAUTH); unauthenticated `GET` (arity 1) → the arity error (**not** NOAUTH) |
| A12 | `AUTH a b c` → arity error |
| A13 | After `AUTH hunter2`, `RESET` → `+RESET`, and the next `GET` returns **NOAUTH** |
| A14 | With no password, `RESET` does **not** deauthenticate |

**Live config**

| Arm | Assertion |
|---|---|
| A15 | `CONFIG SET requirepass s3cr3t` on a running server: a **new** connection is gated |
| A16 | **Latching**: an **existing**, already-working connection keeps working after A15 — matching Redis. This arm exists specifically to prevent an over-eager "fix" |
| A17 | `CONFIG SET requirepass ""` disables auth again; new connections are ungated |
| A18 | `CONFIG GET requirepass` returns the cleartext (Redis parity); assert it does **not** appear in `CLIENT LIST`, `CLIENT INFO`, or the server log |

**HELLO**

| Arm | Assertion |
|---|---|
| H1 | `HELLO` (bare) unauthenticated with a password set → the long HELLO-specific NOAUTH message, not `shared.noautherr` |
| H2 | `HELLO 2 AUTH default hunter2` → `+OK`-equivalent map reply, and the connection is authenticated |
| H3 | `HELLO 2 AUTH default wrong` → `WRONGPASS`, connection still unauthenticated |
| H4 | `HELLO 3 AUTH default wrong` → `WRONGPASS`, **and the connection is still RESP2** — issue a subsequent command and assert the reply framing. This is the protocol-desync arm |
| H5 | `HELLO 3` (valid creds) → `-NOPROTO unsupported protocol version`, verbatim, **no trailing period**. go-redis's fallback depends on it (`NOTES-COMPAT.md:17`) |
| H6 | `HELLO 2 SETNAME foo` → `CLIENT GETNAME` returns `foo` |
| H7 | `HELLO 2 AUTH default hunter2 SETNAME foo` → both applied |
| H8 | `HELLO 2 AUTH default` (one arg short) → `ERR Syntax error in HELLO option 'AUTH'`, **not** an arity error |
| H9 | `HELLO 2 BOGUS` → `ERR Syntax error in HELLO option 'BOGUS'` |
| H10 | `HELLO abc` → `ERR Protocol version is not an integer or out of range` |
| H11 | Repeated `AUTH` clauses: last wins, no error |

**protected-mode**

| Arm | Assertion |
|---|---|
| P1 | Boot `--bind 0.0.0.0` with no password, `protected-mode yes`. A connection from a **non-loopback** address gets the full DENIED message and is closed. (Needs the 25GbE netns rig or a second address — memory: `tomokv-nic-rig`) |
| P2 | Same boot, a **loopback** connection works |
| P3 | Same boot + `--requirepass x`: the non-loopback connection is accepted (and then NOAUTH-gated) |
| P4 | `--protected-mode no` with no password: the non-loopback connection is accepted |
| P5 | Unix socket is always treated as local |
| P6 | `rejected_connections` advances on each P1 rejection (**non-vacuous**) |

**Hardening**

| Arm | Assertion |
|---|---|
| U1 | Unauthenticated client accumulating >1024 bytes of output is killed (see C17 in §2.6) |
| U2 | Unauthenticated `*11\r\n...` (multibulk > 10) is rejected |
| U3 | Unauthenticated bulk > 16384 is rejected |
| U4 | The same three all **succeed** once authenticated |

**Differential**

| Arm | Assertion |
|---|---|
| A-D1 | New `differ.py` suite `auth`: identical AUTH/HELLO/RESET command streams against us and a **vanilla** redis-server with the same `requirepass`, diffing every reply byte |

---

## 4. FEATURE 4 — Sharded pub/sub

### 4.1 The simplification, stated up front

In Redis, shard channels exist to make pub/sub **cluster-shardable**: `SSUBSCRIBE`, `SUNSUBSCRIBE`,
and `SPUBLISH` carry a key spec so the channel name hashes to a slot and the cluster layer redirects
the client to the slot's owner. `SPUBLISH` then fans out only to nodes **in the same shard**, not
across the whole cluster bus.

**We are not a cluster.** There is no slot map, no MOVED/ASK, no cluster bus.

> **THE SIMPLIFICATION: shard channels bind to the channel-home IO thread exactly like regular
> channels — `pubsub_home_for(channel) = hash(channel) % n_io` (`src/core/pubsub.inc:78-86`).**
> The slot is replaced by the IO-thread home. Everything cluster-flavoured is dropped: no key spec,
> no `getKeySlot`, no `clusterRedirectClient`, no `CROSSSLOT`, no
> `cluster-allow-pubsubshard-when-down`, no `pubsubShardUnsubscribeAllChannelsInSlot` (there is no
> slot migration to trigger it).

What this buys the user is the **client-visible contract** — `ssubscribe`/`sunsubscribe`/`smessage`
frames, shard-only counts, no pattern matching — so a client written against Redis Cluster's sharded
pub/sub works against us unchanged. What it costs is nothing, because the routing it would have
needed is routing we already do.

**Multi-channel `SSUBSCRIBE a b c` therefore never errors with `CROSSSLOT`.** In Redis Cluster it
would, whenever the channels hash to different slots (the key spec is `{-1,1,0}` — all remaining
args are keys). For us the channels simply resolve to different IO homes, which the existing
multi-home fan-out in `pubsub_start_modify` (`src/core/pubsub.inc:596-609`) already handles. **This
is a deliberate superset**, not an omission: any command that works on Redis Cluster works here.
Document it.

### 4.2 Commands and exact replies

Registry rows to add to `server_command_table()` (`src/cmd/t_server.cc:894-925`), beside the
existing pub/sub rows at `:903-908`:

```cpp
{"SSUBSCRIBE",   2, -1, CmdFlags::ConnLocal | CmdFlags::PubSub,                 cmd_pubsub_only, 0,0,0},
{"SUNSUBSCRIBE", 1, -1, CmdFlags::ConnLocal | CmdFlags::PubSub,                 cmd_pubsub_only, 0,0,0},
{"SPUBLISH",     3,  3, CmdFlags::ConnLocal | CmdFlags::PubSub,                 cmd_pubsub_only, 0,0,0},
```

Arities match Redis (`/home/user/Projects/redis/src/commands.def:12947-12950`): SPUBLISH `3`,
SSUBSCRIBE `-2`, SUNSUBSCRIBE `-1`.

#### Message-type bulk strings — verbatim shared objects

`/home/user/Projects/redis/src/server.c:2298-2306`:

```c
    shared.messagebulk = createStringObject("$7\r\nmessage\r\n",13);
    shared.pmessagebulk = createStringObject("$8\r\npmessage\r\n",14);
    shared.subscribebulk = createStringObject("$9\r\nsubscribe\r\n",15);
    shared.unsubscribebulk = createStringObject("$11\r\nunsubscribe\r\n",18);
    shared.ssubscribebulk = createStringObject("$10\r\nssubscribe\r\n", 17);
    shared.sunsubscribebulk = createStringObject("$12\r\nsunsubscribe\r\n", 19);
    shared.smessagebulk = createStringObject("$8\r\nsmessage\r\n", 14);
    shared.psubscribebulk = createStringObject("$10\r\npsubscribe\r\n",17);
    shared.punsubscribebulk = createStringObject("$12\r\npunsubscribe\r\n",19);
```

So the three new labels are **`ssubscribe` (10)**, **`sunsubscribe` (12)**, **`smessage` (8)**.
Extend `pubsub_append_ack` (`src/core/pubsub.inc:341-357`) and `pubsub_delivery_frame`
(`:162-175`) with the shard variants — the label/length table there is the natural place.

#### SSUBSCRIBE

`[ssubscribe, <channel>, <count>]`, one frame per channel, RESP2 `*3`.

> **THE COUNT IS SHARD-CHANNELS ONLY.** This is the single most important semantic in the feature.
> `pubSubShardType.subscriptionCount == clientShardSubscriptionsCount`
> (`/home/user/Projects/redis/src/pubsub.c:389-391`) is `dictSize(c->pubsubshard_channels)` — it
> does **not** include regular channels or patterns. And symmetrically, the **regular**
> `SUBSCRIBE` count, `clientSubscriptionsCount` (`:384-386`), is
> `dictSize(c->pubsub_channels) + dictSize(c->pubsub_patterns)` — it does **not** include shard
> channels. **The two counters are disjoint namespaces.**

The only place the sum is used is `clientTotalPubSubSubscriptionCount()`
(`/home/user/Projects/redis/src/pubsub.c:401-405`), which drives subscribe-mode exit and never
appears in a reply.

In our code: `pubsub_subscription_count()` (`src/core/pubsub.inc:337-339`) is currently
`channels.size() + patterns.size()` — correct for the regular arm, and it must stay that way. Add a
sibling that returns `shard_channels.size()` for the shard arm.

A duplicate `SSUBSCRIBE` on an already-subscribed channel **still replies** — the notify call sits
outside the insert-success branch (`/home/user/Projects/redis/src/pubsub.c:504-506`). Our
`pubsub_finish_pending` Modify arm (`src/core/pubsub.inc:380-392`) already acks unconditionally per
item, so this falls out for free.

#### SUNSUBSCRIBE

`[sunsubscribe, <channel>, <remaining shard count>]`, one frame per channel, count decrementing.

With no arguments, `pubsubUnsubscribeShardAllChannels` (`/home/user/Projects/redis/src/pubsub.c:664-670`)
→ `pubsubUnsubscribeAllChannelsInternal` (`:633-654`), which ends with:

```c
    /* We were subscribed to nothing? Still reply to the client. */
    if (notify && count == 0) {
        addReplyPubsubUnsubscribed(c,NULL,type);
    }
```

so a client holding **zero** shard channels gets exactly one frame with a **nil** channel:

```
*3\r\n$12\r\nsunsubscribe\r\n$-1\r\n:0\r\n
```

Our `pubsub_start_modify` already implements exactly this shape for the regular arm
(`src/core/pubsub.inc:573-579` — `pubsub_append_ack(..., nullptr, ...)` → `reply_nil`). Reuse it.

Explicit `SUNSUBSCRIBE chan` on a channel the client is **not** subscribed to also replies, with the
channel echoed and the count unchanged (the `if (notify)` block at
`/home/user/Projects/redis/src/pubsub.c:540-543` is outside the `dictDelete` success branch).

#### SPUBLISH

`:<receivers>` — an integer.

```c
void spublishCommand(client *c) {
    int receivers = pubsubPublishMessageAndPropagateToCluster(c->argv[1],c->argv[2],1);
    if (!server.cluster_enabled)
        forceCommandPropagation(c,PROPAGATE_REPL);
    addReplyLongLong(c,receivers);
}
```
(`/home/user/Projects/redis/src/pubsub.c:948-954`)

**Cluster-forwarded receivers are NOT counted** — `receivers` is computed from the local walk before
`clusterPropagatePublish` runs, and the schema in
`/home/user/Projects/redis/src/commands/spublish.json` says so explicitly:

> *"the number of clients that received the message. Note that in a Redis Cluster, only clients that
> are connected to the same node as the publishing client are included in the count"*

Moot for us — we are one node — but it means our count (all local shard subscribers) is the correct
answer, not an approximation.

#### Delivered message

`[smessage, <channel>, <payload>]` — RESP2 `*3`. First element is **`smessage`**, not `message`.

**There is no `spmessage`.** `pubsubPublishMessageInternal` short-circuits before the pattern loop
(`/home/user/Projects/redis/src/pubsub.c:725-728`):

```c
    if (type.shard) {
        /* Shard pubsub ignores patterns. */
        return receivers;
    }
```

So a `PSUBSCRIBE foo*` subscriber receives **nothing** from `SPUBLISH foo bar`, and conversely
`SSUBSCRIBE ch` receives nothing from `PUBLISH ch msg`. **The two namespaces are completely
disjoint** — same channel string, different index, no crossover in either direction. This is the
second-most-important semantic and the easiest one to implement wrong by reusing one index.

#### PUBSUB SHARDCHANNELS / SHARDNUMSUB

`/home/user/Projects/redis/src/pubsub.c:898-914`:

```c
    } else if (!strcasecmp(c->argv[1]->ptr,"shardchannels") &&
        (c->argc == 2 || c->argc == 3)) 
    {
        /* PUBSUB SHARDCHANNELS */
        sds pat = (c->argc == 2) ? NULL : c->argv[2]->ptr;
        channelList(c,pat,server.pubsubshard_channels);
    } else if (!strcasecmp(c->argv[1]->ptr,"shardnumsub") && c->argc >= 2) {
        /* PUBSUB SHARDNUMSUB [ShardChannel_1 ... ShardChannel_N] */
        int j;
        addReplyArrayLen(c, (c->argc-2)*2);
        for (j = 2; j < c->argc; j++) {
            unsigned int slot = calculateKeySlot(c->argv[j]->ptr);
            dict *clients = kvstoreDictFetchValue(server.pubsubshard_channels, slot, c->argv[j]);

            addReplyBulk(c,c->argv[j]);
            addReplyLongLong(c, clients ? dictSize(clients) : 0);
        }
    }
```

- `SHARDCHANNELS [pattern]` → flat array of bulk strings. No pattern means `NULL`, which skips
  `stringmatchlen` entirely (**not** an implicit `*`). Maps onto our
  `pubsub_start_channels` (`src/core/pubsub.inc:625-643`) with the shard index substituted.
- `SHARDNUMSUB [ch ...]` → flat array of length `(argc-2)*2`, alternating `[chan, count, ...]`.
  Zero args → empty array. Maps onto `pubsub_start_numsub` (`:645-668`).
- **There is no `SHARDNUMPAT`.** A case-insensitive recursive grep for `shardnumpat` across the
  whole Redis source returns zero hits. Shard pub/sub has no pattern support at any layer — no
  `PSSUBSCRIBE`, no `server.pubsubshard_patterns`, and `pubsubTotalSubscriptions()`
  (`/home/user/Projects/redis/src/pubsub.c:995-999`) sums exactly three stores, confirming there is
  no fourth.

Help text to extend in `pubsub_start_command` (`src/core/pubsub.inc:710-712`), verbatim from
`/home/user/Projects/redis/src/pubsub.c:871-874`:

```
SHARDCHANNELS [<pattern>]
    Return the currently active shard level channels matching a <pattern> (default: '*').
SHARDNUMSUB [<shardchannel> ...]
    Return the number of subscribers for the specified shard level channel(s)
```

### 4.3 Subscriber-mode interaction

The RESP2 gate (`/home/user/Projects/redis/src/server.c:4759-4776`) whitelists **exactly nine
procs**: `PING`, `SUBSCRIBE`, `SSUBSCRIBE`, `UNSUBSCRIBE`, `SUNSUBSCRIBE`, `PSUBSCRIBE`,
`PUNSUBSCRIBE`, `QUIT`, `RESET`.

- **SSUBSCRIBE and SUNSUBSCRIBE ARE allowed** in subscriber mode.
- **SPUBLISH is NOT.** Neither are `PUBSUB SHARDCHANNELS`/`SHARDNUMSUB`.

Our existing restriction message (`src/core/pubsub.inc:723-731`) **already advertises the S
forms**:

```
ERR Can't execute '<cmd>': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / PING / QUIT / RESET are allowed in this context
```

byte-identical to Redis's `rejectCommandFormat` at `server.c:4771-4773`. **So the message is already
right and the code is already wrong** — `subscription_control` in `parse_and_dispatch`
(`src/core/io_loop.h:414-418`) currently tests only the four non-shard names. Extend it:

```cpp
const bool subscription_control =
    op->cmd_name().eq_icase("subscribe")   || op->cmd_name().eq_icase("unsubscribe")  ||
    op->cmd_name().eq_icase("psubscribe")  || op->cmd_name().eq_icase("punsubscribe") ||
    op->cmd_name().eq_icase("ssubscribe")  || op->cmd_name().eq_icase("sunsubscribe");
```

**Entering subscriber mode.** `markClientAsPubSub` (`/home/user/Projects/redis/src/pubsub.c:407-412`)
is called unconditionally at the end of `ssubscribeCommand`, so **SSUBSCRIBE enters subscriber mode
even for a duplicate/no-op subscribe**.

**Leaving it** is gated on the **total** count:
```c
    if (clientTotalPubSubSubscriptionCount(c) == 0) {
        unmarkClientAsPubSub(c);
    }
```
i.e. regular channels + patterns + shard channels. Our `pubsub_finish_pending`
(`src/core/pubsub.inc:391`) currently does
`client->set_subscriber_mode(pubsub_subscription_count(local) != 0)` — that must become the
**three-way total**, while the number placed in the **reply** stays namespace-scoped. Getting these
two confused is the most likely bug in this feature; they are different numbers computed at the
same instant.

**PING in subscriber mode** (`/home/user/Projects/redis/src/server.c:5303-5324`) returns the
2-element array form. Since shard subscriptions set the same mode bit, an SSUBSCRIBE-only client
gets `["pong", ""]`, not `+PONG`. Our `pubsub_reply_ping` (`src/core/pubsub.inc:715-721`) already
does this and needs no change — it keys off `c->subscriber_mode()`.

**SSUBSCRIBE in MULTI.** Redis's SUBSCRIBE/PSUBSCRIBE carve out MULTI explicitly
("because of backward compatibility"), but **`ssubscribeCommand` has no MULTI exemption**
(`/home/user/Projects/redis/src/pubsub.c:958-963` vs `:770-786`) — it rejects inside MULTI/EXEC.
Our `NOTES-MULTI.md:12-13` records that the current table has no subscription rows and that
disallowed commands get `ERR Command not allowed inside a transaction`. Adding the shard rows means
deciding this explicitly: **reject all six subscription commands inside MULTI**, which is a superset
of Redis's behaviour for the four regular ones. Our transaction machinery has no path to queue an
IO-owned async pub/sub command, so this is the only implementable answer — record it as a known,
intentional divergence for SUBSCRIBE/PSUBSCRIBE rather than letting it be discovered.

**RESET** drops shard subscriptions silently, with `notify == 0` — no `sunsubscribe` push, just
`+RESET` (`/home/user/Projects/redis/src/networking.c:2117-2133`, called from `:2166`). Our
`pubsub_start_reset` (`src/core/pubsub.inc:684-695`) fans a `ResetRequest` to every IO home, which
calls `pubsub_home_cleanup` — extend that to the shard index (§4.4) and the behaviour is correct
with no new machinery.

### 4.4 Implementation in our architecture

The shape is: **one more index at the same home, one more flag on the wire, one more set per
connection.** Nothing about routing, lifetime, or transport changes.

#### Home-side state (`src/core/pubsub.inc:766-771`)

```cpp
std::unordered_map<std::string, std::vector<PubSubRef>> pubsub_channels_;        // existing
std::unordered_map<std::string, std::vector<PubSubRef>> pubsub_shard_channels_;  // NEW
std::vector<PubSubPatternRef>                           pubsub_patterns_;        // existing
```

`pubsub_home_add_channel` / `pubsub_home_remove_channel` (`:92-116`) take a `bool shard` and select
the map. Everything else — the `PubSubRef` vector, the swap-with-back removal, the
`pubsub_active_channel_added/removed` accounting — is unchanged.

#### Per-connection state (`PubSubLocalConn`, `:23-31`)

```cpp
std::unordered_set<std::string> channels;         // existing
std::unordered_set<std::string> patterns;         // existing
std::unordered_set<std::string> shard_channels;   // NEW
```

Two count helpers, and the distinction is the feature:

```cpp
// The number that goes in a SUBSCRIBE/UNSUBSCRIBE/PSUBSCRIBE/PUNSUBSCRIBE reply.
static uint64_t pubsub_subscription_count(const PubSubLocalConn& l) {
    return l.channels.size() + l.patterns.size();
}
// The number that goes in an SSUBSCRIBE/SUNSUBSCRIBE reply.
static uint64_t pubsub_shard_subscription_count(const PubSubLocalConn& l) {
    return l.shard_channels.size();
}
// The number that decides subscriber mode. NEVER appears in a reply.
static uint64_t pubsub_total_subscription_count(const PubSubLocalConn& l) {
    return pubsub_subscription_count(l) + pubsub_shard_subscription_count(l);
}
```

#### Wire state (`src/core/pubsub_event.h:39-52`)

`PubSubEvent` gains `bool shard = false;` beside the existing `bool pattern`. It is a heap-owned
transport struct with no footprint lock, so this is free.

`PubSubPending` (`src/core/pubsub.inc:33-45`) gains the same.

#### Publish (`pubsub_home_publish`, `:212-245`)

```cpp
auto& index = request.shard ? pubsub_shard_channels_ : pubsub_channels_;
auto exact = index.find(request.channel);
... // unchanged exact fan-out, but delivery->shard = request.shard

// Shard pubsub ignores patterns -- redis pubsub.c:725-728. This early return is the whole
// namespace separation; without it a PSUBSCRIBE would receive SPUBLISH traffic.
if (!request.shard) {
    for (const PubSubPatternRef& entry : pubsub_patterns_) { ... }
}
```

#### Delivery frame (`pubsub_delivery_frame`, `:162-175`)

```cpp
if (event.pattern) { /* 4-elem pmessage -- unchanged */ }
else {
    reply_array_header(frame, 3);
    if (event.shard) reply_bulk(frame, Slice("smessage", 8));
    else             reply_bulk(frame, Slice("message", 7));
}
```

#### Cleanup (`pubsub_home_cleanup`, `:138-160`)

**Must sweep `pubsub_shard_channels_` too.** Factor the channel-map sweep into a helper called
twice. Missing this leaks a home entry per disconnected shard subscriber and the churn test's
`pubsub_home_entries` gauge will not drain to zero — which is exactly the assertion
`tests/pubsub.py:222-235` already makes, so the existing test catches it if the shard arm is added
to it.

Same for `pubsub_clear_local_subscriptions` (`:359-367`): clear `shard_channels` and account
`pubsub_subscription_removed` for each, and set subscriber mode from the **total**.

And `pubsub_disconnect_ready` (`:733-756`): the `!local.pending && local.channels.empty() && local.patterns.empty()`
early-out must also test `local.shard_channels.empty()`, or a shard-only subscriber skips cleanup
entirely and leaves stale refs at its homes.

#### Command entry (`pubsub_start_command`, `:697-713`)

```cpp
if (op.cmd_name().eq_icase("ssubscribe"))   return pubsub_start_modify(client, op, false, true,  /*shard=*/true);
if (op.cmd_name().eq_icase("sunsubscribe")) return pubsub_start_modify(client, op, false, false, /*shard=*/true);
if (op.cmd_name().eq_icase("spublish"))     return pubsub_start_publish(client, op, /*shard=*/true);
...
if (op.arg(1).eq_icase("shardchannels")) return pubsub_start_channels(client, op, /*shard=*/true);
if (op.arg(1).eq_icase("shardnumsub"))   return pubsub_start_numsub(client, op, /*shard=*/true);
```

`pubsub_start_modify` with `shard == true` uses the per-channel home fan-out branch
(`:596-609`) — **never** the all-homes pattern branch (`:585-595`), because shard channels have no
patterns.

#### Accounting

`Server` gains `pubsub_shard_channels_` and `pubsub_shard_subscriptions_` relaxed atomics beside the
six existing gauges (`src/core/server.h:570-575`), exposed in `INFO STATS`. Redis reports
`pubsub_channels` and `pubsubshard_channels` as separate counters
(`/home/user/Projects/redis/src/server.c:6799-6801`) — match those names.

Also worth matching: Redis adds a per-client `ssub=%i` field to `CLIENT INFO`/`CLIENT LIST`
(`/home/user/Projects/redis/src/networking.c:4289`). Our `append_client_line`
(`src/cmd/t_server.cc:191`) is where that goes.

### 4.5 Hot-path budget

**Zero.** Sharded pub/sub adds no code to the key-command path. It adds:

- one `bool` to a heap-allocated transport struct (`PubSubEvent`) — no footprint lock applies;
- one `unordered_set` per **connection that actually uses shard channels** — `PubSubLocalConn` is
  created lazily by `pubsub_track` (`src/core/pubsub.inc:524-528`) and an empty `unordered_set`
  allocates nothing;
- one `unordered_map` per IO thread, empty and never touched until the first SSUBSCRIBE;
- two `eq_icase` comparisons in the subscriber-mode `subscription_control` test — but that test only
  runs when `c->subscriber_mode()` is already true, which is itself
  `__builtin_expect(..., false)` (`src/core/io_loop.h:413`).

**No multi2 out-of-line pattern needed** — the entire feature lives inside `pubsub.inc`, which is
already an out-of-line include reached only through the `CmdFlags::PubSub` branch
(`src/core/io_loop.h:442`).

The one thing worth measuring is the **`pubsub_home_publish` cost when the shard index is empty**:
a `PUBLISH` on a server that has never seen an SSUBSCRIBE must not pay a lookup in the shard map.
It does not — the map is selected by `request.shard`, so a regular publish never touches it. Assert
that in review; it is the kind of thing a well-meaning refactor into "one map with a tagged key"
would silently destroy.

### 4.6 Test plan

Extend `tests/pubsub.py` rather than writing a new file — the lifecycle-gauge drain assertion at
`tests/pubsub.py:222-235` is exactly the invariant the shard arms need, and it should cover the new
gauges too.

| Arm | Assertion |
|---|---|
| S1 | `SSUBSCRIBE ch` → `[b"ssubscribe", ch, 1]`. Second channel → count 2 |
| S2 | `SPUBLISH ch msg` → integer 1; the subscriber reads `[b"smessage", ch, msg]` — **`smessage`, not `message`** |
| S3 | **Namespace disjointness, direction 1**: `SUBSCRIBE ch` + `SPUBLISH ch msg` → SPUBLISH returns **0** and the regular subscriber receives **nothing** |
| S4 | **Namespace disjointness, direction 2**: `SSUBSCRIBE ch` + `PUBLISH ch msg` → PUBLISH returns **0** and the shard subscriber receives **nothing** |
| S5 | **No pattern crossover**: `PSUBSCRIBE ch*` + `SPUBLISH channel msg` → the pattern subscriber receives **nothing**. This is the `if (type.shard) return receivers;` short-circuit |
| S6 | **Disjoint counts**: one connection does `SUBSCRIBE a` (count 1), `PSUBSCRIBE p*` (count 2), `SSUBSCRIBE s` (count **1**, not 3). Then `SUBSCRIBE b` → count **3** (channels+patterns), `SSUBSCRIBE t` → count **2** (shard only). **This is the arm that catches the single most likely bug** |
| S7 | `SUNSUBSCRIBE s` → `[b"sunsubscribe", s, <decremented shard count>]` |
| S8 | `SUNSUBSCRIBE` with no args unsubscribes all shard channels, one frame each, count decrementing to 0 |
| S9 | `SUNSUBSCRIBE` with no args on a client holding **zero** shard channels → exactly one frame `[b"sunsubscribe", None, 0]` (nil channel) |
| S10 | `SUNSUBSCRIBE notsubscribed` → still replies, channel echoed, count unchanged |
| S11 | Duplicate `SSUBSCRIBE ch` → still replies, count unchanged |
| S12 | **Subscriber-mode entry**: after `SSUBSCRIBE`, `SET k v` → the restricted error; `PING` → `[b"pong", b""]` |
| S13 | **SPUBLISH is NOT allowed in subscriber mode** → restricted error |
| S14 | **Subscriber-mode exit is on the TOTAL**: `SUBSCRIBE a` + `SSUBSCRIBE s`, then `UNSUBSCRIBE a` → **still** in subscriber mode; then `SUNSUBSCRIBE s` → mode exits and `SET k v` works |
| S15 | `RESET` from a shard-subscribed client → `+RESET`, **no `sunsubscribe` frame**, and `PUBSUB SHARDNUMSUB ch` then reports 0 |
| S16 | `PUBSUB SHARDCHANNELS` lists active shard channels; `PUBSUB SHARDCHANNELS <glob>` filters; empty pattern → `[]` |
| S17 | `PUBSUB SHARDCHANNELS` does **not** list regular channels, and `PUBSUB CHANNELS` does **not** list shard channels |
| S18 | `PUBSUB SHARDNUMSUB a b` → `[a, n, b, m]`; `PUBSUB SHARDNUMSUB` with no args → `[]` |
| S19 | `PUBSUB SHARDNUMPAT` → unknown-subcommand error (**it must not exist**) |
| S20 | **Multi-home fan-out**: `SSUBSCRIBE a b c` with channels chosen to hash to different IO homes → three acks with counts 1,2,3, and all three deliver. Proves the simplification works and that we do **not** emit CROSSSLOT |
| S21 | N-way fanout: 24 subscribers on one shard channel, 40 ordered messages each, mirroring `tests/pubsub.py:103-123` |
| S22 | **Churn**: 320 abrupt SSUBSCRIBE/disconnect cycles concurrent with 500 SPUBLISHes; then **all** lifecycle gauges — including the two new shard ones — drain to zero. This is the arm that catches a missed `pubsub_home_cleanup` sweep |
| S23 | SSUBSCRIBE inside MULTI is rejected (§4.3) |
| S24 | `CLIENT INFO` reports `ssub=N` |
| S25 | ASAN build, arms S20–S22 |
| S26 | Differential vs **vanilla** redis-server (non-cluster mode, where SSUBSCRIBE/SPUBLISH work exactly as here): identical command streams, diffed replies **and** diffed delivery frames |

---

## 5. CROSS-CUTTING

### 5.1 The complete knob matrix

Every row must land in **all three** surfaces or the knob drifts (§0.6). This table is the checklist.

| Knob | Type | Default | Live? | `Config` field | `ConfigKind` | Notes |
|---|---|---|---|---|---|---|
| `notify-keyspace-events` | string mask | `""` | yes | `uint32_t notify_events` | **new** `NotifyFlags` | canonicalized on GET; §1.2 |
| `maxclients` | uint32 | `10000` | yes | `uint32_t maxclients` | `Unsigned` | min 1; enforced ±`n_io` |
| `timeout` | uint32 s | `0` | yes | `uint32_t timeout` | `Unsigned` | **entry already exists as a stub** at `src/cmd/t_server.cc:223` |
| `tcp-keepalive` | uint32 s | `300` | yes* | `uint32_t tcp_keepalive` | `Unsigned` | *live for new conns only, matching Redis |
| `tcp-backlog` | uint32 | `511` (pending the A/B, §2.2) | **no** | `uint32_t tcp_backlog` | `Unsigned` | IMMUTABLE; `CONFIG SET` must be rejected |
| `client-output-buffer-limit` | 4-tuple × class | `normal 0 0 0` / `slave 256mb 64mb 60` / `pubsub 32mb 8mb 60` | yes | `ClientObufLimits obuf[3]` | **new** `ObufLimit` | multi-token; merge semantics; §2.4 |
| `requirepass` | string | `""` (off) | yes | `const char* requirepass` | `String` | SENSITIVE: keep out of logs and CLIENT LIST |
| `protected-mode` | bool | `yes` | yes | `uint32_t protected_mode` | `Bool` | emits `yes`/`no` on GET |

Two `CONFIG SET` mechanics that need attention:

1. **`tcp-backlog` must be rejected as immutable.** Our `collect_config_updates`
   (`src/cmd/t_server.cc:305-308`) already has an immutable list — `dir` and `dbfilename` — with the
   message `ERR parameter is immutable at runtime`. Add `tcp-backlog` there. (Redis's message
   differs; ours is already established, keep it consistent within our tree.)
2. **Multi-token values.** `client-output-buffer-limit` is the first knob whose value is more than
   one token. The CLI parser's `next()` helper (`src/core/config.h:134`) returns exactly one token.
   The conf-file loader (`:309-339`) already forwards all tokens on a line, so the *file* form works
   the moment the CLI parser consumes greedily — but the CLI form silently truncates until `next()`
   is extended. Fix the parser, then test **both** surfaces (arm C1 covers `CONFIG SET`; add the
   CLI and conf-file equivalents).

Also: several knobs need to reach executors or IO loops **live**. Reuse the existing seqlock
(`Server::begin_live_config_update` / `end_live_config_update`, `src/core/server.h:523-537`) and the
per-pass snapshot reader (`ExLoop::refresh_maxmemory_config`, `src/core/ex_loop.h:118-127`).
Rename that reader to `refresh_live_config` and extend the snapshot struct. **Do not add a second
version counter** — one seqlock, one snapshot, one per-pass read.

### 5.2 Which features need the multi2 out-of-line pattern

The rule (from `src/cmd/multi.h:41-48`): use it when a feature would otherwise put a **subsystem**
on a hot path, and the hot path's register allocation and code layout matter. Do not use it for a
single conditional — the indirection costs more than it saves and it hides the logic.

| Feature | Hot path touched | Pattern? | Why |
|---|---|---|---|
| **1 — keyspace notifications** | **per-op write path** (every `store().erase()`, every write handler, every insert) | **YES — required** | This is the only Wave A feature that lands inside the executor's per-command work. `notify.h` declares the entries; `notify.inc` holds the bodies and is included by `xshard.cc` beside `multi.inc`. The record path is a guard + a 16-byte push; the publish path is entirely out of line |
| **1 — retire-side publication** | `WbEngine` retire callback | **YES — reuses the existing seam** | `IoLoop::init` (`src/core/io_loop.h:79-92`) already dispatches `xshard_retire` / `blocking_retire` / `multi_retire_entry` from one cold callback. Add `notify_retire_entry` as a fourth arm; no new seam |
| **2 — maxclients** | accept only | no | Rare path. Inline the check |
| **2 — tcp-keepalive/backlog** | accept / boot | no | Rare path |
| **2 — timeout + COBL cron** | per-**loop-iteration** | no | One predicted-false test per loop pass. `client_cron_pass()` can be a private `IoLoop` method; it is not hot enough to earn a `.inc` |
| **2 — COBL counter maintenance** | reply staging | no | Inline arithmetic at ~6 sites that are already writing those cache lines. Out-of-lining it would add a call where a single `add` belongs |
| **3 — auth gate** | per parsed command | no | **One conditional**, and the operand order (§3.5) is the optimization. A call would defeat it |
| **4 — sharded pub/sub** | none | no | Already inside `pubsub.inc`, reached only via the `CmdFlags::PubSub` branch |

**Practical note for the feature-1 lane.** `xshard.cc` is already a large single translation unit
(`multi.inc` alone is 1443 lines, plus `blocking.inc`, `scatter_engine.inc`,
`xshard_commands.inc`, `atomics_glue.inc`). Adding `notify.inc` grows compile time and, more
importantly, grows the inlining surface the optimizer works over — which is precisely how an
unrelated hot function loses a register. **Measure `instr/op` on the p32 GET and SET cells before
and after adding the include, with the feature OFF.** If the include alone moves it, that is a real
regression and the fix is a separate TU, not a shrug. This is the `instr/op is the bisect
instrument` lesson (memory: `tomokv-goodsize-nallocx-lesson`) applied preemptively.

### 5.3 Validation: the standing rules this pack must satisfy

These are owner/user rules from memory, not suggestions. Each one has a concrete obligation here.

| Rule | Obligation for Wave A |
|---|---|
| **Pre/post table** (`user-prepost-table-rule`) | Every feature lands with a PRE vs POST perf table. For features whose knob is off by default, the table is the **off-state** proof: identical cells, feature absent vs feature present-but-off |
| **Report format** (`user-report-format-table-then-explain`) | Performance TABLE first, explanation after; publish as an HTML artifact |
| **Quick-check protocol** (`thredis-quickcheck-protocol`) | 8 cells first: p32/p1 × GET/SET × 2 statics, before any broader sweep |
| **Three-regime testing** (`thredis-three-regime-testing`) | A/B in benefit / neutral / deficit regimes; decide from the PATTERN, not one cell |
| **Saturated benching** (`thredis-saturated-benching-rule`) | single-conn never saturates — p1 **and** p128 memtier; investigate anything worse than −3% |
| **Sanity gate** (`thredis-sanity-gate-benching`) | every number gets a plausibility check; nonsense ⇒ STOP, fix, re-bench |
| **Parity bar** (`tomokv-parity-bar`) | every cell on par with or beating stable `730dc029f`; `stablecmp32` is the instrument; build stable from a CLEAN worktree |
| **One server, one bench** (`thredis-one-server-one-bench`) | `boxguard.sh` before every run; overlap corrupts verdicts |
| **Vacuous-validation trap** (`thredis-vacuous-validation-trap`) | "0 bugs" proves nothing unless the gate OPENED. **Every feature ships a counter and every test arm asserts it moved** |
| **Right-sized tests** (`thredis-right-sized-tests`) | shrink duration/breadth, NEVER discrimination |
| **Verify before implementing** (`thredis-verify-before-implementing`) | grep the CODE first; this spec's `file:line` citations were current on 2026-08-26 and must be re-checked, not trusted |
| **Hardcode-or-delete** (`user-hardcode-or-delete`) | any tuning constant introduced here (cron cadence, batch caps) either earns a consistent gain and gets hard-coded, or is deleted. **Do not ship a knob nobody will turn** |
| **LB 3% budget** (`thredis-lb-3pct-budget`) | always-on machinery ≤3% or it does not ship. The cron beat is the only always-on addition; Z2/Z3 in §2.6 are its budget test |

**The counters each feature must expose** (this is the non-vacuity contract):

| Feature | `INFO` field(s) |
|---|---|
| 1 | `notify_events_fired`, `notify_events_dropped` |
| 2 | `rejected_connections` (**already exists** — `src/cmd/t_server.cc:733`, currently fed from `sig().accept_err`; give it its own source), `client_output_buffer_limit_disconnections`, `idle_clients_closed` |
| 3 | `auth_failures` |
| 4 | `pubsubshard_channels`, `pubsubshard_subscriptions`, and `ssub=N` in `CLIENT INFO` |

**Gate additions** (`tests/gate.sh`, which is 164 lines and currently reports 17 checks in `quick`):

```sh
# --- Wave A ---------------------------------------------------------------------
boot ./build/tomokv --notify-keyspace-events KEAmn || bad "notify boot"
python3 tests/notify.py 127.0.0.1 $PORT   && ok "keyspace notification matrix" || bad "..."
stop
boot ./build/tomokv --maxclients 64 --timeout 2 || bad "limits boot"
python3 tests/limits.py 127.0.0.1 $PORT   && ok "limits battery"              || bad "..."
stop
boot ./build/tomokv --requirepass gatepass || bad "auth boot"
python3 tests/auth.py 127.0.0.1 $PORT gatepass && ok "auth state machine"     || bad "..."
stop
boot ./build/tomokv || bad "shard pubsub boot"
python3 tests/pubsub.py 127.0.0.1 $PORT   && ok "pub/sub incl. shard arms"    || bad "..."
stop
# off-state proof: every knob absent must leave the footprint locks and the idle ceiling intact
```

Note the existing boot-matrix section (`gate.sh` lines 30-37) asserts that **deleted** flags stay
dead. Add the mirror for Wave A: assert each new flag is **accepted**, and that a bad value for each
is **rejected**. A knob that silently ignores a typo is the same defect class as a flag that should
have died.

### 5.4 Suggested build order

The features are independent enough to parallelize, but two dependencies are real:

```
   [3 auth]  ──────────────┐
                           ├──► [2 COBL unauth 1024-byte cap]   (§3.6 / arm C17)
   [2 limits] ─────────────┘

   [4 sharded pub/sub]  ── independent, smallest, lowest risk ──► ship first for a quick win

   [1 notifications] ── largest, touches the per-op write path ──► ship last, after the
                        instr/op off-state proof is established as a baseline
```

Recommended order: **4 → 3 → 2 → 1**.

- **4** is contained entirely in `pubsub.inc`, has zero hot-path exposure, and its test arms
  strengthen an existing battery. It also front-loads the disjoint-counter lesson (arm S6), which is
  the kind of semantic detail that is cheaper to get right early.
- **3** is small and unblocks the COBL unauth cap.
- **2** is medium and introduces the IO cron beat, which nothing else depends on but which needs its
  own idle-CPU budget proof.
- **1** is the largest and the only one on the per-op write path. Doing it last means the
  `instr/op` baseline from the previous three lands is already trusted, so a regression is
  attributable.

Per the codex-first delegation rule (memory: `thredis-codex-first-delegation`), each of these is a
`codex exec` unit of work in its own worktree, with **testing done by us, not by Codex**. And per
`thredis-codex-fork-integration-traps`: **boot-test the merged binary FIRST**, and every new knob
goes into the knob matrix (§5.1) at merge time, not after.

### 5.5 Risk register

| # | Risk | Where | Mitigation |
|---|---|---|---|
| R1 | **The reference tree is a fork.** Implementing its extra notify classes (`o c a S T I V r`), its `hexpired` family, or its `A`-includes-`a` definition would make us incompatible with real Redis | §0.2, §1.2 | Ruling recorded: upstream surface only. Arm G3 enforces rejection of fork chars. **Differential tests use a VANILLA oracle** |
| R2 | **The `A` round-trip bug.** The fork drops `n` from `CONFIG GET` when `A` is complete | §1.2 | Arm G4 asserts we do **not** reproduce it |
| R3 | **EX→IO pub/sub posting** would break the documented IO-producer invariant and spin on the scarce EX role | §1.5 | Design rejects it explicitly. Lane A defers publication to the IO retire seam; Lane B never spins |
| R4 | **Footprint locks.** Three new `Client` fields | §0.4 | Holes H1/H2 identified and measured by a compiled probe. `static_assert` is the gate; arm Z4 re-checks |
| R5 | **`__keyevent@0__:<event>` home hotspot** — ~30 fixed channel names means all `set` keyevents land on one IO thread | §1.5 | Documented, not speculatively fixed. Measure under an `E`-only config before acting |
| R6 | **Notification amplification / OOM.** One `DEL k1..k1000` = 2000 heap events | §1.8 | Per-op cap, global in-flight cap reusing `pubsub_inflight_`, drop-and-count. Arm B1. (Memory: `tomokv-atomic-64c-convoy-oom` — 17→126 GB and a silent SIGKILL) |
| R7 | **`tcp-backlog` default change** could regress the 2048-conn opening burst | §2.2 | Decision rule, not a decision: ship 511, run arm K6 as a PRE/POST A/B, revert to 16384 if it regresses |
| R8 | **maxclients `±n_io` slop** from independent SO_REUSEPORT listeners | §2.1 | Documented bound, tested by arm L5. Option (b) CAS available if exactness is ever required |
| R9 | **`live_clients_` leak** via the `~IoLoop` `pending_handoffs_` path, which never calls `close_client` | §2.1 | Called out as the third decrement site. Arm L4 catches the common case; a shutdown-with-handoffs arm catches this one |
| R10 | **COBL counter drift.** `obuf_bytes_` maintained at ~6 sites; one missed site and the limit fires early or never | §2.4 | Arm C19 asserts it returns to **0** on a drained connection. This is the highest-probability bug in feature 2 |
| R11 | **Disjoint pub/sub counters.** Using the total in an `ssubscribe` reply, or the shard count for mode exit | §4.2, §4.3 | Three explicitly named helpers, and arm S6 is written specifically to catch it |
| R12 | **Namespace crossover.** Reusing one channel index for regular and shard would silently deliver `PUBLISH` to `SSUBSCRIBE`rs | §4.4 | Separate maps; arms S3/S4/S5 test both directions plus patterns |
| R13 | **HELLO partial application on auth failure** → RESP desync | §3.8 | Ordering spelled out; arm H4 tests it directly |
| R14 | **Auth latching surprise** — an existing connection keeps working after `CONFIG SET requirepass` | §3.5 | Matched to Redis deliberately; arm A16 exists to stop a future "fix" |
| R15 | **`instr/op` regression from the `notify.inc` include alone**, with the feature off | §5.2 | Measure the include's cost separately, before the feature's. Separate TU if it moves |
| R16 | **Cron beat idle-CPU cost** at 2000+ connections | §2.3 | Budgeted visits (`max(5, n/10)` per 100 ms); arms Z2/Z3 are the ≤3% proof |
| R17 | **`FLUSHALL` must emit no notifications**; if it routes through the notifying `erase()` it will emit one per key | §1.4 | Explicit suppression, arm N1 |
| R18 | **Stale `file:line` citations.** This pack was written against the tree as of 2026-08-26 | throughout | `thredis-verify-before-implementing`: grep the CODE first. Treat every citation as a pointer, not a promise |

### 5.6 What this pack deliberately does NOT cover

Named so the gaps are decisions rather than oversights:

- **`maxmemory-clients`** (client eviction). Redis 8 has it, default off. Orthogonal accounting
  basis, own opt-out flag. §2.4.
- **Streams (`t` class)** and **modules (`d` class)**. The flag characters parse and round-trip;
  there are no producers because there is no `t_stream.cc` and no module system. §1.3.
- **`MOVE`, `RESTORE`, `DUMP`, `BITFIELD`, `ZUNIONSTORE`/`ZINTERSTORE`/`ZDIFFSTORE`, `GEO*`** —
  not in our command tables, so their events have no producers.
- **Hash-field expiration** (`HEXPIRE`/`HPERSIST`/`HGETEX`/`HGETDEL` and the `hexpired` event).
  We have none of those commands.
- **ACL** beyond the single `default` user. `AUTH <other> <pass>` always fails.
- **RESP3.** `HELLO 3` deliberately answers `-NOPROTO`, and `NOTES-COMPAT.md:17` records that
  go-redis depends on that for its RESP2 fallback. Every reply shape in this pack is RESP2.
- **Multi-DB.** `SELECT 0` only, so notification channels hard-code `@0`.
- **Auth rate limiting.** Redis has none; adding it would be a divergence, not a port.
- **Cluster.** No slots, no MOVED/ASK/CROSSSLOT, no cluster bus. This is what makes feature 4's
  simplification possible.

---

## Appendix A — quick citation index

**Our tree** (`/home/user/Projects/tomokv-cpp-perthread`)

| Thing | Path:line |
|---|---|
| Pub/sub implementation (IO-owned, included into `IoLoop`) | `src/core/pubsub.inc` (770 lines), included at `src/core/io_loop.h:153` |
| Channel home | `src/core/pubsub.inc:78-86` |
| IO→IO event transport + the invariant comment | `src/core/pubsub.inc:63-76` |
| Delivery frame builder | `src/core/pubsub.inc:162-175` |
| Subscribe/unsubscribe ack builder | `src/core/pubsub.inc:341-357` |
| Per-connection pub/sub state | `src/core/pubsub.inc:23-31` |
| Home indexes | `src/core/pubsub.inc:766-771` |
| Disconnect fence | `src/core/pubsub.inc:733-756` |
| Pub/sub event transport struct | `src/core/pubsub_event.h:39-52` |
| `Client` (footprint lock at `:538`) | `src/net/conn.h` |
| Segment queue / reply staging | `src/net/conn.h:93-220`, `:340-370` |
| `Op` (footprint lock at `:240`), state markers | `src/exec/op.h:50-53`, `:240` |
| Accept path | `src/core/io_loop.h:262-298` |
| `adopt_client` (TCP_NODELAY, wb slot, client registry) | `src/core/io_loop.h:312-328` |
| Parse/dispatch loop | `src/core/io_loop.h:357-740` |
| Subscriber-mode gate | `src/core/io_loop.h:413-441` |
| Retire callback seam (xshard/blocking/multi) | `src/core/io_loop.h:79-92` |
| `close_client` (idempotent teardown + quiescence fence) | `src/core/io_loop.h:928-971` |
| Listener creation (hardcoded backlog 16384) | `src/core/io_loop.h:104-135` |
| `WbEngine::serve` / `pump` / `on_send_complete` | `src/net/wb.h:81-200` |
| EX loop, per-pass clock + beat pattern | `src/core/ex_loop.h:63`, `:85-90` |
| EX live-config snapshot reader | `src/core/ex_loop.h:118-127` |
| Active expire cycle | `src/core/ex_loop.h:157-175` |
| EX→IO notify (ready mask + claimed post) | `src/core/ex_loop.h:651-688` |
| `Server` live-config seqlock | `src/core/server.h:523-537` |
| `Server` pub/sub gauges | `src/core/server.h:408-449`, `:570-575` |
| `FlatStore::erase` (the single key-removal choke point) | `src/store/flatstore.h:721` |
| Counter-binding pattern to extend | `src/store/flatstore.h:585-586`, bound at `src/core/shard.h:60-61` |
| Command table + flags | `src/cmd/command.h:21-70` |
| Registry build/lookup | `src/cmd/commands.cc:52-128` |
| Server/admin command rows (AUTH/HELLO/RESET/QUIT/pubsub) | `src/cmd/t_server.cc:894-925` |
| `cmd_auth` / `cmd_hello` / `cmd_reset` / `cmd_quit` | `src/cmd/t_server.cc:362-421` |
| CONFIG table + normalizer | `src/cmd/t_server.cc:197-320` |
| `cmd_config` (GET/SET, shard fan-out) | `src/cmd/t_server.cc:569-...` |
| INFO stats assembly | `src/cmd/t_server.cc:658-790` |
| Client metadata registry | `src/cmd/t_server.cc:161-195`, `:946-958` |
| Knob grammar (CLI + conf, one parser) | `src/core/config.h:130-339` |
| Out-of-line integration pattern (the "multi2 pattern") | `src/cmd/multi.h:41-48` |
| Test harness style | `tests/pubsub.py` (esp. `Conn` at `:25-73`, gauge drain at `:222-235`) |
| Differential harness | `tests/differ.py` |
| Pre-push gate | `tests/gate.sh` |

**Reference tree** (`/home/user/Projects/redis`, 8.9.241 — **a fork**, see §0.2)

| Thing | Path:line |
|---|---|
| `keyspaceEventsStringToFlags` | `src/notify.c:20-55` |
| `keyspaceEventsFlagsToString` | `src/notify.c:61-94` |
| `notifyKeyspaceEventImpl` (the two classic channels) | `src/notify.c:142-188` |
| `NOTIFY_*` bit table + `NOTIFY_ALL` | `src/server.h:806-834` |
| `notify-keyspace-events` config registration / setter / getter / rewriter | `src/config.c:3537`, `:3094-3111`, `:3113-3116`, `:1493-1505` |
| `del` on empty list (`listElementsRemoved`) | `src/t_list.c:794-816` |
| `del` on empty hash (HDEL / field-expiry orderings) | `src/t_hash.c:5280-5303`, `:3730-3737` |
| `new` event (the only site) | `src/db.c:464-465` |
| `expired`/`evicted` shared emitter | `src/db.c:2860-2905` |
| `keymiss` (the only site, with `LOOKUP_WRITE` suppression) | `src/db.c:348-353` |
| `delGenericCommand` ordering | `src/db.c:1458-1474` |
| `setGenericCommand` ordering | `src/t_string.c:176-211` |
| maxclients check + error strings | `src/networking.c:1741-1763` |
| `adjustOpenFilesLimit` | `src/server.c:2698-2781` |
| `clientsCronHandleTimeout` | `src/timeout.c:33-59` |
| `clientsCron` rate math + rotating-head walk | `src/server.c:1259-1305` |
| `clientsCronRunClient` (the per-client chain) | `src/server.c:1202-1223` |
| `anetKeepAlive` (sockopt derivation) | `src/anet.c:113-253` |
| `anetListen` / `listenToPort` / `checkTcpBacklogSettings` | `src/anet.c:508-524`, `src/server.c:2877-2917`, `:2784-2828` |
| COBL parser | `src/config.c:390-444` |
| COBL defaults | `src/config.c:171-176` |
| COBL registration / getter / rewriter | `src/config.c:3535`, `:3016-3030`, `:1507-1533` |
| `getClientType` / `ByName` / `TypeName` | `src/networking.c:5410-5456` |
| `checkClientOutputBufferLimits` | `src/networking.c:5458-5517` |
| `closeClientOnOutputBufferLimitReached` | `src/networking.c:5519-5556` |
| Buffer-size trio (alloc / logical / memory-usage) | `src/networking.c:5325-5368` |
| `authCommand` | `src/acl.c:3352-3403` |
| `addAuthErrReply` (WRONGPASS) | `src/acl.c:1508-1515` |
| `ACLCheckUserCredentials` (the four-step ladder) | `src/acl.c:1470-1504` |
| `ACLUpdateDefaultUserPassword` | `src/acl.c:3405-3416` |
| `authRequired` (operand order matters) | `src/networking.c:111-120` |
| `clientSetDefaultAuth` (the latch) | `src/networking.c:103-109` |
| processCommand auth gate + `shared.noautherr` | `src/server.c:4583-4590`, `:2256-2257` |
| protected-mode block | `src/networking.c:1663-1698` |
| `helloCommand` | `src/networking.c:5032-5129` |
| `resetCommand` / `clearClientConnectionState` | `src/networking.c:4448-4462`, `:2136-2184` |
| `pubsubtype` struct + both instances | `src/pubsub.c:19-29`, `:236-247`, `:249-260` |
| `ssubscribe` / `sunsubscribe` / `spublish` | `src/pubsub.c:956-969`, `:971-983`, `:948-954` |
| Shard/regular subscription counters | `src/pubsub.c:383-405` |
| Shard-ignores-patterns short-circuit | `src/pubsub.c:725-728` |
| Subscribe/unsubscribe/message reply builders | `src/pubsub.c:266-333` |
| Shared message-type bulk strings | `src/server.c:2298-2306` |
| `PUBSUB SHARDCHANNELS` / `SHARDNUMSUB` + `channelList` | `src/pubsub.c:898-914`, `:920-946` |
| RESP2 subscriber-mode gate (the nine allowed procs) | `src/server.c:4759-4776` |
| `clearClientPubSubState` | `src/networking.c:2117-2133` |
| Shard command key specs (cluster routing — we drop these) | `src/commands.def:6048-6052`, `:6073-6077`, `:6119-6123` |
