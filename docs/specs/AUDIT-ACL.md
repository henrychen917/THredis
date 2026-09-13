# AUDIT-ACL — the ACL family for TomoKV-cpp (perthread / pure 2s)

> **Where it landed (2026-09-04).** The feature shipped as `src/cmd/acl.{h,cc,inc}` (this document
> proposes `src/core/acl.h`). The probe `scratchpad/aclprobe/probe.cc` cited in §0.8/§2.9 is not
> in the tree (only `scratchpad/aclprobe/holes.cc` is). Everything else here is the design record.

Governing audit. Read before writing any ACL code in `/home/user/Projects/tomokv-cpp-perthread`.
No repository was modified in producing this. Two measurements below were made by compiling probes
against this tree rather than asserted from memory (§0.8, §2.9); sources and binaries are in
`scratchpad/aclprobe/`.

> **Standing caveat on line numbers.** The tomokv tree moved twice while this audit was being
> written (`fed25f4c6` → `c9e049088`, §0.6), and one of those commits added a whole command family.
> tomokv `file:line` citations are accurate at `c9e049088`; redis/valkey/dragonfly/garnet citations
> are against static checkouts and are stable. Before writing code, re-run
> `scratchpad/aclprobe/{probe,holes}.cc` at your branch point and re-locate the `src/` sites — the
> conclusions here are robust, the line numbers are not.

**Sources read (all first-hand):**

| Tree | Path | Version | Verified |
|---|---|---|---|
| **vanilla redis** (the surface we implement) | `/tmp/claude-1000/redis74/src/{acl.c,server.h,server.c,config.c,multi.c,script.c,script_lua.c,sort.c,blocked.c,db.c,networking.c,commands.def,redis.conf}` | `7.4.2` (`version.h:1`) | read |
| redis unstable | `/home/user/Projects/redis/src/acl.c` | `8.9.241` (`version.h:1`) | read + diffed |
| valkey | `/home/user/Projects/valkey/src/{acl.c,server.h,config.c}` | `255.255.255-dev`, HEAD `3f16ffa04` | read + diffed |
| **tomokv redis fork** | `/home/user/Projects/wt-round-mainline/src/{acl.c,server.c,multi.c,script.c,networking.c}` | redis `8.6.2` base, branch `mainline` | read + diffed against its own upstream point |
| dragonfly | `/home/user/Projects/dragonfly/src/server/acl/*` | local checkout | read |
| garnet | `/home/user/Projects/garnet/libs/server/{ACL,Auth,Resp}/*` | local checkout | read |
| tomokv-cpp | `/home/user/Projects/tomokv-cpp-perthread/src/{core,cmd,net,exec}`, `tests/` | branch `perthread-locality`, **`fed25f4c6` → `c9e049088` during the audit** (see §0.6) | read + probed, probes re-run at `c9e049088` |
| Wave-A spec | `scratchpad/specs/SPEC-WAVEA.md` §3 (requirepass / AUTH / protected-mode / HELLO) | — | read |

---

## 0. Corrections to the framing — read this first

Eight premises in the brief are wrong against the source. Six of them change the design, so they
lead.

### 0.1 — `/home/user/Projects/redis` is NOT a fork. The tomokv fork is `wt-round-mainline`.

The brief calls `/home/user/Projects/redis` "a redis FORK at 8.9.x (the tomokv fork's base)". It is
upstream `redis/redis` `unstable` at `8.9.241` (`src/version.h:1`), zero commits ahead of
`origin/unstable`, worktree clean. Every difference between it and vanilla 7.4 is **redis's own
7.4→8.9 evolution**, not a fork divergence. The table in §1.6 is therefore an *upstream drift* table
and is useful for a different reason than the brief assumed: it tells us which parts of the 7.4
surface redis is still actively changing (identity/provenance) and which are frozen (grammar,
subcommands, error strings).

The actual tomokv fork is `/home/user/Projects/wt-round-mainline` (redis `8.6.2` base). Its ACL
verdict is in §0.2 and it is the fork-floor answer the brief asked for.

### 0.2 — The fork kept ACL intact, so the fork floor is REAL and usable.

`wt-round-mainline`'s `acl.c` carries **8 hunks, +9/−41 lines** against its own upstream point, and
every one is mechanical:

| Site | Edit | Why |
|---|---|---|
| `acl.c:487` `ACLFreeUserAndKillClients` | `server.clients` → `server.clients[iotid]` | per-thread client lists |
| `acl.c:2002` `ACLKillPubsubClientsIfNeeded` | same | same |
| `acl.c:2428` `ACLLoadFromFile` | same | same |
| `acl.c:2692` `addACLLogEntry` | `server.current_client` → `server.current_client[iotid].p` | per-thread current client |
| `acl.c:1938-1971` `ACLShouldKillPubsubClient` | `c->pubsub_*` → `clientPubSubData(c)->…` | lazy cold client state |
| (upstream `acl.c:716`) | deleted `ACLCountCategoryBitsForCommands/ForSelector` (−34) | dead code, zero callers in *any* tree |

`ACLCheckAllPerm` at `server.c:10880-10887` was **relocated**, not modified. `multi.c:259,278` and
`script.c:451,453` have **zero** ACL delta. The one auth-adjacent tomokv invention is
`networking.c:5444-5451`: `AUTH` is treated as a deliberate one-command parse fence
(`authRequired(c)` forces `lookahead = 1`) so pipelined frames after `AUTH` are validated under the
new identity. **That trick is directly relevant to us** — see §2.11.

**Ruling: the fork's ACL-on throughput is a valid floor for ours.** Bench it (§5.6).

### 0.3 — The subcommand list in the brief is incomplete: `LOG` and `DRYRUN` exist in 7.4.

The brief enumerates "SETUSER GETUSER DELUSER USERS CAT GENPASS WHOAMI LIST LOAD SAVE HELP".
`aclCommand` (`redis74/src/acl.c:2844-3171`) dispatches **thirteen** branches: the eleven named plus
`ACL LOG [<count>|RESET]` (`:3034-3107`) and `ACL DRYRUN <user> <cmd> [args...]` (`:3108-3136`).
`ACL LOG` is not optional trivia — `addACLLogEntry` is called from **every** denial path
(`server.c:3989`, `multi.c:202`, `script.c:377`, `acl.c:1492`) and is the only observability the
feature has. Both are classified in §1.1.

### 0.4 — EXEC does **not** run under one ACL decision, and a denial does **not** abort the transaction.

The brief asks what vanilla does about a `SETUSER` between queue and `EXEC`. The answer is stronger
than "re-checks": `execCommand` re-checks **each queued command individually** at execution time
(`multi.c:183-184`), with the comment at `:181-182` stating exactly that. A failing element gets a
**distinct** error and the loop **continues** to the next element (`multi.c:186-206` vs the `else {
call(...) }` at `:207-213`):

```
-NOPERM ACLs rules changed between the moment the transaction was accumulated and the EXEC call. This command is no longer allowed for the following reason: %s
```

where `%s` ∈ {`no permission to execute the command or subcommand`, `no permission to touch the
specified keys`, `no permission to access one of the channels used as arguments`, `no permission`}.

So a `SETUSER` mid-transaction yields a **partially executed** transaction with per-element NOPERM
errors — not an `EXECABORT`, not a rollback. The queue-time check is the ordinary one
(`server.c:3986-3995`, which runs before `queueMultiCommand` at `server.c:4193-4201`) and produces
the ordinary `-NOPERM` string, which also sets `CLIENT_DIRTY_EXEC` via `rejectCommand`. Both checks
exist and they emit **different strings**. Any implementation that does one and not the other, or
uses one string for both, fails a differential test.

### 0.5 — A blocked client re-entering after a park **is** re-checked. The brief's premise is inverted.

The brief asks "BLPOP served later must not re-check as a different user?". It does re-check.
`unblockClientOnKey` (`blocked.c:631-680`) calls `processCommandAndResetClient(c)` at `:661` when
`CLIENT_PENDING_COMMAND` is set — the full `processCommand`, including the ACL gate at
`server.c:3986`. The identity used is whatever `c->user` points at *now*. Because `ACL SETUSER`
overwrites the user object **in place** (`ACLCopyUser(u, tempu)`, `acl.c:2124`) rather than
replacing the pointer, a permission change between the park and the serve is honoured. And if the
user was deleted, the client was already killed (`ACLFreeUserAndKillClients`, `acl.c:475-492`), so
the park never resumes.

**Consequence for us:** the tomokv blocking path (`io_loop.h:480-533`) dispatches once and the
owner replies later; there is no re-dispatch through `parse_and_dispatch`. Matching redis here
means either re-checking at `blocking_start`'s completion or accepting the divergence explicitly.
§2.8 rules on it.

### 0.6 — Shard channels DID NOT exist when this audit began. They landed mid-audit. Nine PubSub rows now.

**This section is a correction of my own correction, and it is the reason the "verify before
implementing" rule exists.** When the audit started (`fed25f4c6`),
`grep -rn "SPUBLISH\|SSUBSCRIBE\|SUNSUBSCRIBE" src/` returned **zero** hits and the registry had
six `PubSub` rows. Two commits landed while the audit was in progress:

```
3deccd690  blocking: per-waiter deadlines + tri-state not-ready policy (streams prep)
c9e049088  pubsub: add sharded namespace (816 diff checks)
```

All probes below were **re-run against `c9e049088`**. The registry is now **167** commands and
**nine** `PubSub` rows (`src/cmd/t_server.cc:903-914`):

```
SUBSCRIBE  UNSUBSCRIBE  PSUBSCRIBE  PUNSUBSCRIBE  SSUBSCRIBE  SUNSUBSCRIBE  PUBLISH  SPUBLISH  PUBSUB
```

So the brief's question about `SPUBLISH`/shard channels is **live, not moot**, and the channel table
in §2.7 carries nine rows. Vanilla treats shard channels identically to global ones — `db.c:2305`
(`ssubscribeCommand`, `CMD_CHANNEL_SUBSCRIBE`), `:2307` (`sunsubscribeCommand`,
`CMD_CHANNEL_UNSUBSCRIBE`), `:2311` (`spublishCommand`, `CMD_CHANNEL_PUBLISH`) — one `&pattern`
namespace covers both, and `ACLShouldKillPubsubClient` walks `pubsubshard_channels` alongside the
other two dicts (`acl.c:1969-1977`). **Do not give shard channels their own ACL namespace.**

Still true, and still worth stating: vanilla checks channels only for
`CMD_CHANNEL_PUBLISH | CMD_CHANNEL_SUBSCRIBE` (`acl.c:1727`). **`UNSUBSCRIBE`, `PUNSUBSCRIBE` and
`SUNSUBSCRIBE` are deliberately not ACL-checked** — you may always leave a channel.

**Standing instruction for the lane:** this tree moves under you. Re-run
`scratchpad/aclprobe/{probe,holes}.cc` at the commit you branch from and diff against the numbers
here before trusting any count in this document.

### 0.7 — Keyspace-notification channels are NOT `&pattern`-checked by anything. There is no gap to close.

The brief asks about "keyspace-notification channels under `&pattern` checks". Vanilla applies the
channel ACL at **`SUBSCRIBE`/`PSUBSCRIBE` command time only** (`acl.c:1728-1744` via
`getChannelsFromCommand`, `db.c:2344`). Notification *delivery* (`notifyKeyspaceEvent` →
`pubsubPublishMessage`) is never ACL-checked; the publisher is the server itself. A user with
`&__keyspace@*__:*` can hear notifications for keys they cannot read — that is upstream behaviour,
not an oversight to fix. **Do not invent a delivery-time check**; it would be a divergence, it would
be on the hot notify path, and it would violate the 3% always-on budget. The correct behaviour is
the existing one: gate at `SUBSCRIBE`, revoke by killing (`ACLKillPubsubClientsIfNeeded`, §2.7).

### 0.8 — MEASURED: our command-id space is 167 wide, so the ACL bitmap is 3 words / 24 bytes.

Probe compiled against this tree on 2026-08-26 (`scratchpad/aclprobe/probe.cc`, built and run pinned
to CPUs 32-47), re-run at `c9e049088` after the mid-audit commits of §0.6:

```
sizeof(CommandSpec)         = 40
command_registry_size()     = 167          (was 164 at fed25f4c6)
bitmap words @64b for 167   = 3   (24 bytes)
rows with first_key > 0     = 133
rows with last_key < 0      = 28
rows with key_step > 1      = 2      (MSET, MSETNX)
ConnLocal=28 PubSub=9 Blocking=8 Transaction=5 ScriptRoute=2 MultiShard=36
```

This is the single most load-bearing measurement in the audit. Redis carries
`USER_COMMAND_BITS_COUNT 1024` (`server.h:1067`) = **128 bytes per selector** because it must
accommodate module commands assigned ids at runtime. We have no modules
(`command_registry_init`, `src/cmd/commands.cc:52-111`, assigns dense ids at boot from seven static
family tables and the registry is frozen before any thread starts). **We can size the bitmap to the
real command count.** 256 ids → 4 words → **32 bytes, exactly half a cache line**, with 89 ids of
headroom. Use 256, not 167: a fixed power-of-two keeps the word index a shift, and the +3 rows this
audit watched land are exactly why the sizing must not be `n_commands`-derived.

**Bit 255 is reserved** and never assigned to a command — it is vanilla's "this rule set was built
from `+@all`" marker (`ACLGetCommandID`, `acl.c:1545`; consumed by
`ACLSelectorCanExecuteFutureCommands`, `:542-544`). §2.1 explains why we need it even without
modules: it is what decides whether `ACL SAVE` writes `+@all -x` or `-@all +x`.

### 0.9 — `%R~` / `%W~` are NOT implementable on today's key metadata. This is the biggest single finding.

Redis's read/write key split is driven by **per-key-position** keyspec flags, not by a per-command
flag. `ACLSelectorCheckKey` (`acl.c:1571-1595`) derives `key_flags` from
`CMD_KEY_ACCESS`/`INSERT`/`DELETE`/`UPDATE` **on the individual key reference**
(`resultidx[j].flags`, `acl.c:1717`), which `getKeysFromCommandWithSpecs` fills from the command's
`keySpec` array. Concretely (`commands.def`):

```c
keySpec SINTERSTORE_Keyspecs[2] = {
  {NULL, CMD_KEY_OW|CMD_KEY_UPDATE, KSPEC_BS_INDEX, .bs.index={1}, KSPEC_FK_RANGE, .fk.range={0,1,0}},   /* dest  = WRITE */
  {NULL, CMD_KEY_RO|CMD_KEY_ACCESS, KSPEC_BS_INDEX, .bs.index={2}, KSPEC_FK_RANGE, .fk.range={-1,1,0}}}; /* srcs  = READ  */
```

TomoKV's `CommandSpec` (`src/cmd/command.h:52-70`) carries **one** key range `(first_key, last_key,
key_step)` and **no per-position flags**. The registry dump shows the damage precisely:
`SINTERSTORE` is `keys=1..-1/1 W|OOM|MS` — destination and sources in one undifferentiated range,
all of them inheriting the command's `Write` bit. `ZRANGESTORE` is `keys=1..2/1 W` (dst write, src
read — same problem). `BITOP` is `keys=2..-1/1 W`. `COPY`/`SMOVE`/`LMOVE`/`RPOPLPUSH` likewise.

**A coarse fallback — "use the command's Write/Readonly flag for all its keys" — is not a
simplification, it is a security bug**: a user granted `%R~src:*` would be denied `SINTERSTORE` on
`src:*` (correct), but a user granted `%W~dst:*` and nothing else would be *granted* read of every
source key (wrong). §1.2 therefore classifies `%R~`/`%W~` as **LATER**, gated on a keyspec table,
and §2.10 specifies that table.

### 0.10 — Our conf-file loader will silently corrupt `#<sha256>` rules. P0 for the knob grammar.

`load_conf_file` (`src/core/config.h:309-339`) does, at `:320`:

```cpp
if (char* hash = std::strchr(line, '#')) *hash = '\0';
```

It truncates at **any** `#` in the line. Redis strips a comment only when `#` is the **first**
character of the trimmed line (`config.c:452`: `if (lines[i][0] == '#' || lines[i][0] == '\0')
continue;`). A conf line

```
user alice on #a3f1…64hex ~app:* +@read
```

would be silently truncated to `user alice on` — a user with a hashed password becomes a user with
**no password and no rules**, and nothing reports an error. The same loader also tokenizes with
`std::strtok(line, " \t\r\n")` (`:322`), so quoted rules (`>"pass phrase"`) are impossible, whereas
redis uses `sdssplitargs` (`config.c:455`) which handles quoting.

Both must be fixed **before** the `user` directive ships. §3.2.

### 0.11 — requirepass and ACL are the *same* state in redis. There is no second gate to build.

The brief's instruction — "ACL must extend that exact gate, not invent a second one" — is not just a
style preference, it is what the source does. `updateRequirePass` (`config.c:2565-2573`) calls
`ACLUpdateDefaultUserPassword(server.requirepass)` (`acl.c:3232-3241`), which is literally

```c
ACLSetUser(DefaultUser,"resetpass",-1);
if (password) { ACLSetUser(DefaultUser, ">"+password, …); } else { ACLSetUser(DefaultUser,"nopass",-1); }
```

`authRequired()` (`networking.c:111-120`) reads `DefaultUser->flags` — **the same object**. So
Wave-A's `requirepass` state, correctly implemented, *is* the default user's password list, and the
Wave-A gate *is* the ACL NOAUTH gate. ACL adds a **second, independent** gate after it (NOPERM), not
a replacement. §2.9 specifies the exact insertion point and proves the off-cost.

Two operational consequences worth writing into `tomokv.conf`, both citable:
- `ACL LOAD` and `aclfile` **override** `requirepass` (`redis.conf:1047-1048`: *"The requirepass is
  not compatible with aclfile option and the ACL LOAD command, these will cause requirepass to be
  ignored"*). Mechanically: `ACLLoadFromFile` replaces `DefaultUser`'s password list wholesale
  (`acl.c:2416` `ACLCopyUser(DefaultUser, new_default)`).
- After that, `CONFIG GET requirepass` still returns the **stale** `server.requirepass` string
  (`config.c:3126`), because nothing writes back. That is upstream behaviour. Match it; do not
  "fix" it.

---

### 0.12 — Dragonfly's per-connection cache has NO revision number. Invalidation is an eager push.

The brief says dragonfly "cache a categories/revision snapshot per connection ... how invalidation
on SETUSER works". Half right, and the wrong half is the interesting one.

The **cache is real**: `dfly::ConnectionContext` carries `std::vector<uint64_t> acl_commands`,
`AclKeys keys`, `AclPubSub pub_sub`, `size_t acl_db_idx`, `bool skip_acl_validation`
(`src/server/conn_context.h:377-389`), populated at connect (`main_service.cc:1897-1899`), at AUTH
(`server_family.cc:2045-2052`) and at RESET (`main_service.cc:1990-1995`).

There is **no revision/version counter anywhere**. `grep -rn "version" src/server/acl/` returns zero
hits. Invalidation is **eager push**: `AclFamily::StreamUpdatesToAllProactorConnections`
(`acl_family.cc:116-137`) walks every connection on the main listener via helio's
`TraverseConnections` and **overwrites** the four cached fields on any connection whose
`authed_username` matches — *while holding the registry write lock*
(`acl_family.cc:143` → `166-167`).

This matters for us in two directions:
- **The good idea to steal is the snapshot, not a version scheme.** Lazy revision-compare would put
  a load + compare on every command; the push puts the whole cost on the admin path.
- **The mechanism is illegal in our architecture.** A cross-thread write into per-connection state
  is exactly what pure-2s forbids (`Client` is single-owner, `conn.h:1-14`; SPEC-WAVEA §0.5 item 5).
  §2.9 gives the architecture-native equivalent: an immutable, atomically-published permission blob
  behind a stable per-user handle, so the *pointer on the Client never has to change*.

### 0.13 — Garnet is not a comparable ACL: it has no key ACLs and no channel ACLs at all.

Garnet's rule parser (`libs/server/ACL/ACLParser.cs:133-332`) accepts `~*`/`allkeys`/`resetkeys` as
**explicit no-ops** (`:257-267`) and rejects every other `~pattern` through the catch-all
`throw new ACLUnknownOperationException(op)` at `:270`. `&channel`, `allchannels`, `resetchannels`,
`%R~`, `%W~`, selectors and `allcommands`/`nocommands` produce **zero grep hits** in the tree and
all land on the same throw. The in-code comment at `ACLParser.cs:259-262` says why it is load-bearing
rather than merely unfinished:

> // NOTE: No-op, because only wildcard key patterns are currently supported. If per-key key
> // patterns are ever added, the GET scatter-gather fast path (NetworkGET_SG) must re-check
> // ACL per key: it serves GETs past the first without returning through the per-command ACL
> // check in ProcessMessages, which is only safe while key access is all-or-nothing.

**That is our situation exactly**, and it is the most directly transferable warning in any tree: a
batched/scatter fast path that skips the per-command gate is only sound while key ACLs do not exist.
See §2.6 — we have three such paths (`xshard`, `multi`, Lua).

So Garnet is a useful data point for *command-bitmap design* (§2.5) and useless as a key/channel
reference. Treat it as "the minimum viable ACL", and note it ships that way in production.

---

## 1. (a) SURFACE MATRIX

Classification is for a **single-node, RESP2-only, no-cluster, no-replication, no-modules,
SELECT-0-only** server. `SHOULD` = in the first ACL wave. `LATER` = correct to want, blocked on a
prerequisite that is named. `NEVER` = the mechanism it exists for does not exist here.

### 1.1 Subcommands (vanilla `aclCommand`, `redis74/src/acl.c:2844-3171`)

| Subcommand | Vanilla site | Verdict | Reason |
|---|---|---|---|
| `ACL WHOAMI` | `:2972-2977` | **SHOULD** | Two lines. `c->user->name`, or null. Every client library's ACL smoke test calls it. |
| `ACL LIST` | `:2948-2971` (`justnames=0`) | **SHOULD** | Config-file-format dump of every user. It is `ACLDescribeUser` + a `"user "` prefix — the same code `ACL SAVE` and `CONFIG REWRITE` need, so it is free once `ACLDescribeUser` exists. |
| `ACL USERS` | `:2948-2971` (`justnames=1`) | **SHOULD** | Same branch, one flag. |
| `ACL SETUSER` | `:2846-2872` | **SHOULD** | The feature. Note the staging discipline in `ACLStringSetUser` (`:2079-2134`) — all-or-nothing against a temp user; §2.4. |
| `ACL GETUSER` | `:2899-2947` | **SHOULD** | The round-trip oracle for every rule test. Reply shape §1.5. |
| `ACL DELUSER` | `:2873-2898` | **SHOULD** | Refuses `default` (`:2881-2884`), returns the delete count, and **kills the user's connections** (`ACLFreeUserAndKillClients`, `:2894`). The kill is the security-relevant half. |
| `ACL CAT` (bare) | `:2999-3004` | **SHOULD** | Lists the category names. Needs the category table (§1.3), which `+@cat` needs anyway. |
| `ACL CAT <category>` | `:3005-3014` | **SHOULD** | Lists commands in a category. Needs a per-command category word on `CommandSpec` — the same word `+@cat` needs. Garnet ships CAT-bare-only and its own docs are wrong about it (`ACLCommands.cs:114-118`); do not repeat that. |
| `ACL GENPASS [bits]` | `:3015-3033` | **SHOULD** | 20 lines, no state. Default 256 bits, max `GENPASS_MAX_BITS 4096`, output is `(bits+3)/4` hex chars (`:3031`). Error string at `:3024-3027` is verbatim-able. |
| `ACL HELP` | `:3137-3167` | **SHOULD** | We already have the `addReplyHelp` shape for other containers. Garnet's omission (`RespCommand.cs:339-349`) is a wart, not a precedent. |
| `ACL LOG [count\|RESET]` | `:3034-3107` | **SHOULD** | **Not optional.** `addACLLogEntry` is the only observability the feature has and it is called from all four denial paths (`server.c:3989`, `multi.c:202`, `script.c:377`, `acl.c:1492`). Without it, "why did my client get NOPERM" is unanswerable. See §2.12 for the per-io-thread design. |
| `ACL LOAD` | `:2983-2990` | **SHOULD** *(with `aclfile`)* | All-or-nothing; §2.4. Guarded by the "not configured to use an ACL file" error at `:2978-2982`. |
| `ACL SAVE` | `:2991-2998` | **SHOULD** *(with `aclfile`)* | Temp-file + `fsync` + `rename` + dir-fsync (`ACLSaveToFile`, `:2469-2544`). Garnet's version truncates in place (`AccessControlList.cs:235`) — do not copy that. |
| `ACL DRYRUN <user> <cmd> [args]` | `:3108-3136` | **LATER** | Pure convenience; it is `ACLCheckAllUserCommandPerm` against a named user with `verbose=1` messages. Ship once the check path is settled — it is ~25 lines then, and it is the cleanest directed-test hook we have. Dragonfly ships it (`acl_family.cc:1298`); Garnet does not. |

**Ruling: 12 SHOULD, 1 LATER, 0 NEVER.** The subcommand surface is not where the cost is.

### 1.2 Rule grammar

Parsed in two layers: `ACLSetUser` (`acl.c:1272-1372`, user-level) falls through to `ACLSetSelector`
(`acl.c:1025-1200`, selector-level) for anything it does not recognise (`:1366-1369`).

| Rule | Vanilla site | Verdict | Reason |
|---|---|---|---|
| `on` / `off` | `:1281-1286` | **SHOULD** | Two flag bits. `off` on `default` disables the whole server's unauthenticated path via `authRequired` (`networking.c:116-517`) — already Wave-A's operand. |
| `>password` | `:1299-1317` | **SHOULD** | SHA-256 hex, dedup via `listSearchKey`, clears `NOPASS`. Wave-A already brings the hash + constant-time compare (SPEC-WAVEA §3.3). |
| `#<sha256hex>` | `:1299-1317` via `ACLCheckPasswordHash` (`:220-235`) | **SHOULD** | Must be exactly 64 lowercase hex (error string at `:1395-1397`). **Dragonfly rejects `#hash` from `ACL SETUSER`** and accepts it only from the file loader (`acl_family.cc:146` passes `hashed=false`) — a gratuitous divergence; do not copy it. |
| `<password` / `!<hash>` (remove) | `:1318-1336` | **SHOULD** | Same code path, one branch. `ENODEV` → "The password you are trying to remove ... does not exist". |
| `nopass` | `:1293-1295` | **SHOULD** | This is *literally* `requirepass ""` for the default user (§0.11). Wave-A already implements the semantics; the rule is the spelling. |
| `resetpass` | `:1296-1298` | **SHOULD** | Clears list **and** the `NOPASS` flag. Garnet's `reset` fails to clear its passwordless flag (`User.cs:482-492` never touches `IsPasswordless`) — that is a Garnet bug; our `reset` must call `resetpass`. |
| `+cmd` / `-cmd` | `:1117-1125`, `:1181-1188` | **SHOULD** | One bit each. `ACLChangeSelectorPerm` (`:623-636`) also fans out to subcommands. |
| `+@cat` / `-@cat` | `:1189-1194` | **SHOULD** | Requires a per-command `acl_categories` word; §1.3. |
| `allcommands` / `nocommands` | `:1042-1055` | **SHOULD** | Aliases for `+@all` / `-@all`; three lines. Garnet rejects them (`ACLParser.cs:270`) — a real compat break, do not repeat. |
| `~pattern` | `:1056-1099` | **SHOULD** | The glob matcher already exists: `command_glob_match` (`src/cmd/command.h:111`), the redis-compatible implementation shared with `SCAN` and the pub/sub matcher. |
| `allkeys` / `~*` | `:1026-1030` | **SHOULD** | Sets `SELECTOR_FLAG_ALLKEYS` and **empties** the pattern list. It is the whole zero-cost story: `ACLSelectorCheckKey` returns at `:1573` before touching the list. |
| `resetkeys` | `:1031-1033` | **SHOULD** | Clears flag + list. |
| `&channel` | `:1100-1116` | **SHOULD** | Six PubSub rows (§0.6), a hand-written channel-arg table. |
| `allchannels` / `&*` | `:1034-1038` | **SHOULD** | |
| `resetchannels` | `:1039-1041` | **SHOULD** | And it is the **default for new users** in 7.4 (§3.3). |
| `reset` | `:1355-1364` | **SHOULD** | Composite: `resetpass` + `resetkeys` + `resetchannels` + conditional `allchannels` + `off` + `sanitize-payload` + `clearselectors` + `-@all`. Ship it as the composite; it is what `ACLLoadConfiguredUsers` uses for a re-declared `default` (`:2223`). |
| `%R~` / `%W~` / `%RW~` | `:1063-1084` | **LATER** | **Blocked on a keyspec table** (§0.9, §2.10). Shipping it against today's flat key range would grant reads it must deny. Note the round-trip asymmetry: `%RW~p` is stored as `ACL_ALL_PERMISSION` and re-emitted as plain `~p` (`sdsCatPatternString`, `:328-339`) — a GETUSER round-trip test must expect that. |
| `(selector ...)` | `:1337-1344`, `aclCreateSelectorFromOpSet` `:967-986` | **LATER** | Not cluster/module-specific — it is a real single-node feature (per-command-set key scoping). But it multiplies the check path by the selector count (`ACLCheckAllUserCommandPerm` loops selectors, `:1856-1870`) and forces the `aclKeyResultCache` (`:1655-1668`) to exist. Ship the root selector first; the data model must be *shaped* for selectors from day one (§2.9) so adding them is additive. Neither dragonfly (`acl_family.cc:1175` catch-all) nor garnet has them. |
| `clearselectors` | `:1345-1354` | **LATER** | Meaningless without selectors. |
| `sanitize-payload` / `skip-sanitize-payload` | `:1287-1292` | **NEVER** | They gate `RESTORE` deep-payload sanitization. We have no `RESTORE` (registry dump §0.8 — 167 rows, no `RESTORE`, no `DUMP`). Valkey deleted both flags outright (`server.h:1021-1023`) and kept the tokens as silent no-ops for file compatibility. **Do the same**: accept and ignore, so an ACL file written by redis loads. Document it. |
| `+cmd\|firstarg` (the `allowed_firstargs` form) | `:1167-1177` | **NEVER** | Redis itself logs a deprecation warning when you use it (`:1173-1175`: *"a misuse of ACL and may get disabled in the future"*), it exists to express `+select\|0` and `+debug\|object`, and it costs an `sds**` of `USER_COMMAND_BITS_COUNT` pointers on the selector. We are `SELECT 0`-only and `DEBUG` is not in the registry. Reject with `ECHILD`-equivalent wording. |
| `+cmd\|subcmd` (real subcommands) | `:1158-1166` | **LATER** | Vanilla resolves `CONFIG\|GET` to a distinct `redisCommand` with its own id. **We have no subcommand rows**: `CONFIG`, `CLIENT`, `COMMAND`, `OBJECT`, `PUBSUB`, `SCRIPT` are single registry entries (§0.8 dump) whose subcommand is `argv[1]` at runtime. Granting `+config\|get` requires either splitting those into rows (id-space cost: ~30 more ids, still inside 256) or a side table. **Do it by splitting rows** when it ships — it also fixes `ACL CAT` granularity and `COMMAND DOCS`. Dragonfly cannot do it at all: `command_registry.cc:303-310` makes every space-named command share its parent's bit, except `ACL` itself. |

**Ruling: 16 SHOULD, 4 LATER, 2 NEVER.** The two LATERs that matter are `%R~`/`%W~` (needs
keyspecs) and selectors (needs the loop). Both are *additive* to the design in §2.9 if the data
model is shaped for them now, and both are gratuitously expensive if it is not.

### 1.3 The category table

Vanilla 7.4 ships **21** categories, `ACLDefaultCommandCategories[]` (`acl.c:46-68`), bits 0-20
(`server.h:224-244`):

```
keyspace read write set sortedset list hash string bitmap hyperloglog geo
stream pubsub admin fast slow blocking dangerous connection transaction scripting
```

Cross-tree stability, which is the reason to adopt them verbatim:

| Tree | Count | Delta |
|---|---|---|
| vanilla 7.4.2 | 21 | baseline |
| valkey unstable | 21 | **identical names and bit positions** (`valkey/src/acl.c:71-95`) |
| tomokv fork (redis 8.6.2) | 21 | **byte-identical** (`wt-round-mainline/src/acl.c:51-74`) |
| redis unstable 8.9.241 | 22 (+1 build-gated) | `+array` bit 21 (`redis/src/acl.c:60`), `+ratelimit` bit 22 under `#ifdef ENABLE_GCRA` (`:74-76`) |
| dragonfly | 28, **UPPERCASE** | 21 + `CUCKOO_FILTER TOPK CMS BLOOM FT_SEARCH THROTTLE JSON` (`acl_commands_def.h:47-53`). **And `hyperloglog` is spelled `HYPERLOG`** (`acl_family.h:121`) — `+@hyperloglog` is an error there. |
| garnet | 25 | 21 + `garnet custom vector` + **`all` listed as a category** (`ACLParser.cs:23-50`). `+@stream` is advertised and **throws** (zero stream commands exist). |

**Ruling: adopt the 21 vanilla names, lowercase, bits 0-20, exactly.** Do not invent tomokv
categories. Do not follow dragonfly's uppercase or its `HYPERLOG` spelling. Of the 21, four have no
members in our 167-command registry today — `stream`, `geo`, `scripting` (we have `EVAL`/`EVALSHA`/
`SCRIPT`, so this one does have members), `blocking` (8 rows, has members). Concretely: **`stream`
and `geo` are empty**. An empty category must still be listed by `ACL CAT` and must still accept
`+@stream` as a legal no-op — Garnet's crash on `+@stream` (`User.cs:120-123`) is the anti-pattern.

Mechanically this is one new field, `uint64_t acl_categories`, on `CommandSpec`
(`src/cmd/command.h:52-70`; `sizeof` goes 40 → 48, and `CommandSpec` is **not** footprint-locked —
only `Op` and `Client` are, `config.h:17`). Populating it is 167 hand-audited literals in the seven
family tables. **The source of truth must be vanilla's own `commands.def`**, not our judgement — see
risk R1.

### 1.4 Error surface

| Situation | Exact bytes | Vanilla site |
|---|---|---|
| unauthenticated, command needs auth | `-NOAUTH Authentication required.` | `server.c:2256-2257` (shared obj), gate at `:3970-3977` |
| bad password / unknown user / disabled user | `-WRONGPASS invalid username-password pair or user is disabled.` | `acl.c:1474` (`addAuthErrReply`) |
| `AUTH <pass>` with `nopass` default | `-ERR AUTH <password> called without any password configured for the default user. Are you sure your configuration is correct?` | `acl.c:3206-3208` |
| command denied | `-NOPERM User <u> has no permissions to run the '<cmd>' command` | `server.c:3990-3991` + `getAclErrorMessage` `acl.c:2739-2740` |
| key denied (non-verbose — the wire form) | `-NOPERM No permissions to access a key` | `acl.c:2746` |
| channel denied (non-verbose) | `-NOPERM No permissions to access a channel` | `acl.c:2753` |
| key denied, verbose (`ACL DRYRUN` only) | `User <u> has no permissions to access the '<key>' key` | `acl.c:2743-2744` |
| channel denied, verbose (`ACL DRYRUN` only) | `User <u> has no permissions to access the '<chan>' channel` | `acl.c:2750-2751` |
| denied inside `EXEC` | `-NOPERM ACLs rules changed between the moment the transaction was accumulated and the EXEC call. This command is no longer allowed for the following reason: <reason>` | `multi.c:203-206` |
| denied inside Lua | `ACL failure in script: <non-verbose message>` | `script.c:378-379` |
| `SORT ... BY` without unrestricted read | `-ERR BY option of SORT denied due to insufficient ACL permissions.` | `sort.c:235` |
| `SORT ... GET` without unrestricted read | `-ERR GET option of SORT denied due to insufficient ACL permissions.` | `sort.c:256` |
| `SETUSER` rule errors | `ERR Error in ACL SETUSER modifier '<op>': <msg>` where `<msg>` ∈ the 8 strings in `ACLSetUserStringError` | `acl.c:2105-2107`, `:1376-1404` |
| unmatched selector paren | `ERR Unmatched parenthesis in acl selector starting at '<arg>'.` | `acl.c:2088-2090` |
| bad username | `ERR Usernames can't contain spaces or null characters` | `acl.c:2856` |
| `DELUSER default` | `ERR The 'default' user cannot be removed` | `acl.c:2882` |
| `LOAD`/`SAVE` with no `aclfile` | `ERR This Redis instance is not configured to use an ACL file. You may want to specify users via the ACL SETUSER command and then issue a CONFIG REWRITE (assuming you have a Redis configuration file set) in order to store users in the Redis configuration.` | `acl.c:2981` |
| `ACL CAT <bad>` | `ERR Unknown category '<name>'` (`%.128s`-truncated) | `acl.c:3008` |
| `GENPASS <bad bits>` | `ERR ACL GENPASS argument must be the number of bits for the output password, a positive number up to 4096` | `acl.c:3024-3027` |

**Two hard rules.**
1. The **non-verbose** key/channel messages are what goes on the wire for ordinary denials — they
   deliberately do **not** name the key. Emitting the verbose form outside `ACL DRYRUN` is an
   information leak *and* a differ failure.
2. `WRONGPASS` collapses "no such user" and "bad password" (`ACLCheckUserCredentials` sets `errno`
   at `acl.c:1436`/`:1465` and `authCommand` discards it). Wave-A already rules on this
   (SPEC-WAVEA §3.3); ACL must not regress it when real users exist. Garnet **does** regress it —
   it emits two different strings by arity (`BasicCommands.cs:1501-1510`).

Error-string stability across trees is total: redis 8.9, valkey and the tomokv fork all carry the
7.4 wording byte-for-byte. There is no version-sensitivity to design around.

### 1.5 `ACL GETUSER` / `ACL LIST` reply shapes (the round-trip contract)

`ACL GETUSER` (`acl.c:2899-2947`) is a map of **6** fields in this order: `flags` (a set),
`passwords` (array of 64-hex), then the root selector inlined for backwards compatibility as
`commands` / `keys` / `channels` (`aclAddReplySelectorDescription`, `:2789-2828`), then `selectors`
(array of maps, `listLength(u->selectors) - 1` entries, root excluded, `:2938-2946`).

`ACL LIST` and `ACL SAVE` share one serializer, `ACLDescribeUser` (`:846-891`), emitting

```
user <name> <flags...> <#hash...> <keys...> <channels...> <command-rules> [ (<selector>) ...]
```

with the channel part **always prefixed by `resetchannels`** when not `allchannels` (`:823`) so the
line is idempotent on reload, and the command part **always starting `+@all ` or `-@all `**
(`:761-767`). Sample of a nontrivial user, produced by this exact code:

```
user alice on #8c6976e5b5410415bde908bd4dee15dfb167a9c873fc4bb8a81f6f2ab448a918 ~app:* %R~cache:* resetchannels &news.* -@all +@read +get +set -del
```

Competitor divergence worth knowing: dragonfly's `ACL LIST` truncates password hashes to **15 hex
chars** (`PrettyPrintSha(pass,false)`, `acl_family.cc:834-835`) so its `LIST` output is **not**
round-trippable — only its `ACL SAVE` is. Garnet's `GETUSER` returns **3** fields (`flags`,
`passwords`, `commands` — `ACLCommands.cs:477`), omitting `keys`/`channels`/`selectors` entirely.
Match vanilla, not either of them.

### 1.6 Competitor comparison — one table

| Feature | vanilla 7.4 | redis 8.9 (upstream drift) | valkey | dragonfly | garnet | **tomokv verdict** |
|---|---|---|---|---|---|---|
| subcommands | 13 | 13 | 13 | 13 (`acl_family.cc:1283-1306`) | **10** (no `HELP`, no `LOG`, no `DRYRUN`) | 12 SHOULD + DRYRUN LATER |
| categories | 21 | 22 (+`array`, +`ratelimit` gated) | 21 | 28, **UPPERCASE**, `HYPERLOG` | 25 (+`all` as a category) | **21, vanilla names, lowercase** |
| `~pattern` | yes | yes | yes | yes | **no** (`~*` is a no-op) | SHOULD |
| `%R~`/`%W~` | yes | yes + `CMD_KEY_PREFIX` prefix-match (`acl.c:1623-1631`) | yes + `prefixmatchlen` (`acl.c:1742`) | yes, **uppercase-only** (`acl_family.cc:766-776`) | no | **LATER** (needs keyspecs) |
| `&channel` | yes | yes | yes | yes | **no** | SHOULD |
| selectors `(...)` | yes | yes | yes | **no** (`user.h:118-120` declares `Selector()`, no definition) | no | LATER |
| `+cmd\|subcmd` | yes | yes | yes | **no** (shared parent bit, `command_registry.cc:303-310`) | yes (subcommands are enum members) | LATER |
| `reset` (composite) | yes | yes | yes | **no** (TODO at `user.h:74-75`) | yes, **buggy** (leaves `nopass`) | SHOULD |
| bitmap width | 1024 bits / 128 B | 1024 | 1024 | 20 families × 64 bits = 160 B | 368 bits / 48 B | **256 bits / 32 B** (measured, §0.8) |
| category storage | eager into bitmap + ordered `command_rules` string for redisplay | same | same | eager + `cat_changes_`/`cmd_changes_` seq maps for redisplay | eager + rationalized `Description` string | **eager + ordered rule string** (vanilla's) |
| per-conn perm cache | no (`c->user` ptr) | no | no | **yes**, pushed eagerly under the write lock (`acl_family.cc:116-137`) | **yes**, immutable + CAS swap (`CommandPermissionSet.cs:12-13`) | **yes** — garnet's shape, §2.9 |
| `+@all` fast path | none in 7.4 | **yes**, root-selector short-circuit `acl.c:1910-1917` | no | `all_keys` skips the key loop (`validator.cc:155`) | **yes**, singleton pointer compare (`CommandPermissionSet.cs:60-63`) | **yes** — §2.9 |
| ACL check inside `EXEC` | **yes**, per element | yes | yes | **NO** (`InvokeCmd` has no check; `main_service.cc:1678-1754`) | yes (re-parse, `TxnRespCommands.cs:62-63`) | **yes** — §2.8 |
| ACL check inside Lua | yes, per `redis.call` | yes | yes | yes | yes | **yes** — §2.7 |
| `DELUSER` kills connections | yes | yes (widened) | yes | yes | **NO** (`AccessControlList.cs:97-104`) | **yes** |
| channel revocation on SETUSER | kills only if not a superset (`getUpcomingChannelList`) | per-subscription provenance | same as 7.4 | kills **only** if the rules contained `resetchannels` (`acl_family.cc:164`) | n/a | **vanilla's** — §2.7 |
| ACL file comments (`#` lines) | **rejected** (`redis74/src/acl.c:2329-2336`) | accepted, dropped on SAVE (`redis/src/acl.c:2394`) | rejected (`valkey/src/acl.c:2542-2543`) | not supported (`acl_family.cc:739-741`) | accepted (`AccessControlList.cs:271-275`) | **accept, and preserve nothing** — §3.4 |
| ACL LOG | yes, global, deduped | yes | yes + `script` context | yes, **per-thread**, 6 fields, no dedup | **no** | **per-io-thread, vanilla's 10 fields** — §2.12 |
| `ACL_DENIED_*` numbering | CMD 1, KEY 2, AUTH 3, CHANNEL 4 | same +`TLS_CERT 5` | **renumbered** (`DB 1, CMD 2, …`) | own enum | n/a | vanilla's — internal only |

---

## 2. (b) IMPLEMENTATION AUDIT

### 2.1 Vanilla data model — three levels, and the level that matters

```
user (server.h:1092-1100)                aclSelector (acl.c:144-171)          keyPattern (acl.c:290-293)
  sds       name                           uint32_t flags   SELECTOR_FLAG_*     int flags   ACL_READ/WRITE_PERMISSION
  uint32_t  flags   USER_FLAG_*             uint64_t  allowed_commands[16]      sds pattern
  list*     passwords  (sds, 64-hex)        sds**     allowed_firstargs
  list*     selectors  (>=1, root first)    list*     patterns  (keyPattern*)
  robj*     acl_string (memoised display)   list*     channels  (sds)
                                            sds       command_rules  (ORDERED TEXT)
```

`allowed_commands` is `USER_COMMAND_BITS_COUNT/64 == 16` words = **128 bytes per selector**
(`server.h:1067`). Word/bit derivation is `ACLGetCommandBitCoordinates` (`acl.c:519-524`):
`word = id/64`, `bit = 1ULL << (id%64)`; out-of-range ids return `C_ERR` and
`ACLGetSelectorCommandBit` (`:533-537`) then returns 0 — **fail closed on overflow**, deliberately
(`:530-532`).

The reserved top bit, id `USER_COMMAND_BITS_COUNT-1`, is never assigned to a command
(`ACLGetCommandID`, `:1545`: `if (nextid == USER_COMMAND_BITS_COUNT-1) nextid++;`). Its only job is
to record *how the rule set was built*: `+@all` sets it, so `ACLSelectorCanExecuteFutureCommands`
(`:542-544`) can decide whether `ACL SAVE` should write `+@all -x -y` or `-@all +x +y`
(`ACLDescribeSelectorCommandRules`, `:761-767`). **We need the same trick** — see §2.9 — and the
256-bit layout must reserve bit 255 for it.

### 2.2 How rules compile — eager into the bitmap, ordered text for redisplay

This is the question the brief asks ("ordered vs recomputed") and the answer is *both, for different
consumers*.

**Eager into the bitmap.** `ACLSetSelectorCategory` (`:697-706`) resolves the category name to a
flag, then `ACLSetSelectorCommandBitsForCategory` (`:644-657`) walks `server.orig_commands`
recursively and flips the bit of every command carrying that `acl_categories` bit. `+cmd` is one
`ACLChangeSelectorPerm` (`:623-636`), which also fans out to `cmd->subcommands_dict`. So at check
time there is **no category anywhere** — only bits.

**Ordered text for redisplay.** Every command/category rule also appends to
`selector->command_rules` via `ACLUpdateCommandRules` (`:611-619`), which first *removes* any
existing rule that matches verbatim or is a subcommand of the new one
(`ACLSelectorRemoveCommandRule`, `:565-607`, an in-place `memmove` compaction) and then appends
`+x`/`-x`. That string is what `ACL LIST`/`SAVE`/`GETUSER` replay to reproduce the ordering
(`ACLDescribeSelectorCommandRules`, `:748-801`).

The paranoia at `:786-798` is worth copying verbatim in spirit: after building the display string,
redis re-applies it to a *fake* selector and `memcmp`s the resulting bitmap against the real one,
`serverPanic`-ing on mismatch, with the comment *"aborting is better than a security risk in this
code path"*. **Ship that assertion.** It is the one place where a display bug becomes a privilege
bug.

`ACLRecomputeCommandBitsFromCommandRulesAllUsers` (`:662-695`) exists only for module load/unload —
it replays every user's `command_rules` from a `+@all`/`-@all` baseline. **We have no modules and a
boot-frozen registry, so this function has no analogue and must not be built.**

Dragonfly does the same split with better data structures: eager into `commands_`
(`user.cc:135-171`) plus `cat_changes_`/`cmd_changes_` hash maps keyed on the category/command with
a monotonic `seq_` (`user.h:200-208`), merged and sorted only for printing (`acl_family.cc:706-723`).
The hash-map form silently collapses `+@set -@set +@set` to one entry — fine for display, and
cheaper than redis's string surgery. Garnet goes further and *rationalizes* the description by
re-parsing prefixes and dropping no-op tokens (`User.cs:609-635`), self-described as "an expensive
method"; it is `SETUSER`-only, so the cost is in the right place.

**Ruling for us: vanilla's model, dragonfly's storage.** Bits eagerly; an ordered
`std::vector<RuleToken>` (sign + kind + name) rather than string surgery, replayed for
`LIST`/`SAVE`/`GETUSER`. Keep vanilla's post-build bitmap `memcmp` assertion.

### 2.3 The check path, and what it actually costs

```
processCommand (server.c:3986)
  └─ ACLCheckAllPerm(c, &idx)                         acl.c:1878-1880
      └─ ACLCheckAllUserCommandPerm(u,cmd,argv,argc)  acl.c:1837-1875
          ├─ if (u == NULL) return ACL_OK             :1842   ← the master/AOF fake-client escape
          ├─ initACLKeyResultCache(&cache)            :1852-1853
          └─ for each selector:                       :1856
              └─ ACLSelectorCheckCmd(...)             acl.c:1678-1746
                  ├─ [1] command bit                  :1681-1703
                  ├─ [2] key loop                     :1707-1723
                  └─ [3] channel loop                 :1728-1744
```

**[1] Command bit.** `if (!(selector->flags & SELECTOR_FLAG_ALLCOMMANDS) && !(cmd->flags &
CMD_NO_AUTH))` then one `ACLGetSelectorCommandBit`. So `+@all` users skip even the load. On a miss
it falls into the `allowed_firstargs` walk (`:1685-1702`) — a `strcasecmp` chain we are not
building (§1.2).

**[2] Key loop.** Gated by `if (!(selector->flags & SELECTOR_FLAG_ALLKEYS) &&
doesCommandHaveKeys(cmd))` (`:1707`). `doesCommandHaveKeys` (`db.c:2287-2291`) is a
`getkeys_proc`/keyspec-flags test, i.e. a per-command constant. When it runs, it calls
`getKeysFromCommandWithSpecs` **once**, memoised across selectors in `aclKeyResultCache`
(`:1708-1712`) — that cache exists purely because selectors made the loop re-entrant. Then per key:
derive `key_flags` from that key's `CMD_KEY_ACCESS/INSERT/DELETE/UPDATE`, and walk **every** pattern
calling `stringmatchlen` (`ACLSelectorCheckKey`, `:1586-1593`). It returns on the **first matching
pattern**, so the cost is `O(keys × patterns)` worst case, `O(keys)` when the first pattern hits.

**[3] Channel loop.** Gated by `!(flags & ALLCHANNELS) && doesCommandHaveChannelsWithFlags(cmd,
CMD_CHANNEL_PUBLISH|CMD_CHANNEL_SUBSCRIBE)` (`:1728`). Note the mask: **`UNSUBSCRIBE` and
`PUNSUBSCRIBE` are not checked** (`db.c:2306-2309` tag them `CMD_CHANNEL_UNSUBSCRIBE`) — leaving is
always allowed. `PSUBSCRIBE` patterns are matched **literally** (`strcmp`), not as globs
(`ACLCheckChannelAgainstList`, `:1646`), because a glob-vs-glob subsumption test is not a thing;
`SUBSCRIBE`/`PUBLISH` channel names are glob-matched against the ACL patterns.

**The honest cost summary for a default (`+@all ~* &*`) user in vanilla 7.4:** one flags load, three
predicted-taken branches, zero loops. **Redis 8.9 made it cheaper still** with a root-selector
short-circuit (`redis/src/acl.c:1910-1917`): if the first selector has all three ALL bits, return
`ACL_OK` before anything else. That is a straight steal.

**Where getKeysResult comes from, and why it matters to us.** `getKeysFromCommandWithSpecs` resolves
per-command `keySpec` arrays (or a `getkeys_proc` for the irregular ones: `SORT`, `GEORADIUS`,
`EVAL`, `ZADD`, `XREAD`, ...). Each returned `keyReference` is `{pos, flags}` — position **and**
access flags. It is the *flags* half we cannot reproduce today (§0.9).

**First-arg commands and `SORT`/`GEORADIUS STORE` — the honest answer.** These are the commands
whose key set is not statically derivable. Redis handles them in two different ways:
- `GEORADIUS ... STORE k` gets a `getkeys_proc` (`georadiusGetKeys`) so the STORE key **is** in
  `getKeysResult` and **is** ACL-checked normally. Keyspec 3 of `SORT` is likewise
  `CMD_KEY_OW|CMD_KEY_UPDATE, KSPEC_BS_UNKNOWN` (`commands.def:2480-2482`) with `sortGetKeys` doing
  the discovery.
- `SORT ... BY pat` / `GET pat` cannot be resolved at all — the keys derive from the *contents* of
  the sorted key. Redis's answer is a **capability escalation**: `sortCommand` calls
  `ACLUserCheckCmdWithUnrestrictedKeyAccess(c->user, c->cmd, c->argv, c->argc, CMD_KEY_ACCESS)`
  (`sort.c:192`) — which requires a selector to pass the command check **and** hold `~*` or `%R~*`
  (`ACLSelectorHasUnrestrictedKeyAccess`, `acl.c:1602-1626`, a literal `strcmp(pattern,"*")`) — and
  rejects `BY`/`GET` outright otherwise (`sort.c:233-237`, `:255-258`). **We must port this**: our
  `SORT` row is `keys=1..1/1` (§0.8 dump) so the BY/GET/STORE keys are invisible to any generic
  check. §2.6 row 8.

**Scripts touching undeclared keys under ACL — the answer the brief asks for.** In vanilla,
non-cluster scripts may touch **any** key; there is no undeclared-key restriction
(`grep -rn "undeclared" redis74/src/*.c` returns only `script.c:493`, a cluster-rehashing message).
The sole protection is that every `redis.call` re-enters the full ACL check under the caller's
identity: `scriptCall` sets `c->user = run_ctx->original_client->user` (`script.c:579`) and then
calls `scriptVerifyACL` (`:600` → `ACLCheckAllPerm`, `:375`), logging with `ACL_LOG_CTX_LUA`.
`redis.acl_check_cmd()` (`script_lua.c:1131`) exposes the same check to script authors.

**We are stricter, and it is a genuine advantage.** `key_declared` (`src/cmd/scripting.cc:365-370`,
enforced at `:488-491` with `ERR Script attempted to access an undeclared key`) means the declared
`KEYS` array is the **complete** key set a script can touch. §2.7 turns that into a hoisted check.

### 2.4 All-or-nothing staging — `SETUSER` and `LOAD`

**`ACL SETUSER`** never mutates the live user until every rule has parsed. `ACLStringSetUser`
(`acl.c:2079-2134`) creates an unlinked temp user, `ACLCopyUser`s the existing one into it
(`:2097-2100`), applies every rule to the temp (`:2102-2110`, bailing to `cleanup` on the first
error), runs `ACLKillPubsubClientsIfNeeded(tempu, u)` (`:2115`) **before** the swap, then
`ACLCopyUser(u, tempu)` (`:2124`). The swap is in place — the `user*` pointer every client holds is
unchanged — which is exactly why permission changes are visible to existing connections instantly
and why §0.5's blocking re-check works.

`ACLMergeSelectorArguments` (`:2032-2072`) runs first, gluing `( ... )` fragments that arrived as
separate argv entries; unmatched paren → `NULL` + `invalid_idx`.

**`ACL LOAD`** is all-or-nothing at the **file** level, and the mechanism is the interesting part
(`ACLLoadFromFile`, `:2272-2464`): it swaps `Users` for a **fresh empty rax** (`:2299-2300`), loads
every line into it accumulating an `errors` sds, and at the end either commits (`:2407-2457`) or
frees the new rax and restores `old_users` (`:2458-2463`) with the message
`WARNING: ACL errors detected, no change to the previously active ACL rules was performed`.

The per-line detail the brief asks about ("how vanilla handles a bad line mid-file"): a bad line
does **not** stop the loop. Each error is *appended* to the `errors` string and the loop `continue`s
(`:2319`, `:2335`, `:2344`, `:2353`, `:2398`), so the reply names every broken line at once. Within
a line, `ENOENT` (unknown command/category) prints per-op detail; every other error prints only the
**first** one, `syntax_error` gating the rest (`:2373-2386`) — deliberately, since later ops may be
garbage caused by the first. Comments (`#` at line start) are **errors** in 7.4 (`:2255-2257`
explains why: the file is rewritten, so comments would be lost); redis 8.9 relaxed this to skip them
(`redis/src/acl.c:2394`), valkey did not.

The commit half also does the identity fixup: `default` is copied into the *existing* `DefaultUser`
object rather than replaced (`:2408-2419`, so every `c->user == DefaultUser` stays valid), then
every client is re-pointed to its same-named new user or **killed** if the name vanished
(`:2430-2451`), with `getUpcomingChannelList` memoised per user name in a rax to avoid recomputing
it per client (`:2439-2444`).

Dragonfly's loader has the same shape (`acl_family.cc:294-361`: parse all, bail before mutating).
Garnet's too (`AccessControlList.cs:174-215`: parse into a temp ACL, swap by reference). **Three
independent implementations agree; adopt it without debate.**

### 2.5 Dragonfly and Garnet check paths, condensed

**Dragonfly** — `acl::IsUserAllowedToInvokeCommand` (`validator.cc:85-113`), the **last** step of
`Service::VerifyCommandState` (`main_service.cc:1487`), before any transaction scheduling.
Per command, against the **connection-local snapshot**:
1. `if (cntx.skip_acl_validation) return true;` (`validator.cc:87-89`) — admin port, UDS, replica
   apply, RDB load, journal executor.
2. `if (id.IsAlias()) return false;` (`:91-93`) — unconditional deny for `--command_alias` names.
3. `acl_commands[id.GetFamily()] & id.GetBitIndex()` (`:27-33`) — **one indexed load + AND**.
   Family/bit are plain members of `CommandId`, so no hashing, no lookup.
4. Key loop only when `!keys.all_keys && id.first_key_pos() != 0 && (read||write)` (`:154-165`).
   For the default user `all_keys == true` (`user_registry.cc:142`) so it never runs.
5. On denial only: thread-local `acl_log.Add` (`:107-110`) — the expensive `GetClientInfo()` is on
   the failure path.

Two costs worth naming because we must not repeat them: (a) a `GlobMatcher` is **constructed per
(glob, key) pair** (`validator.cc:22-25`), and (b) the key loop has **no early exit** —
`keys_allowed &= iterate_globs(key)` (`:164`) evaluates every key even after a denial. Also (c) two
`std::string` compares (`id.name() == "MOVE"`, `== "SELECT"`) run on the generic path for their
db-restriction extension (`:117-129`).

**Garnet** — `CheckACLPermissions` (`AdminCommands.cs:124-146`), `AggressiveInlining`, once per
parsed command in `ProcessMessages` (`RespServerSession.cs:653`). The whole check for an
unrestricted user is a **reference compare**: `+@all` installs the `CommandPermissionSet.All`
**singleton by reference** (`User.cs:157`) and `CanRunCommand` starts `if (this == All) return true`
(`CommandPermissionSet.cs:60-63`). Restricted users pay two field loads, a failed pointer compare,
an array load, a shift, an AND. The failure path is deliberately `[MethodImpl(NoInlining)]`
(`AdminCommands.cs:193`) with the comment *"Failing should be rare … hide this behind a method call
to keep icache pressure down"*.

Garnet's publication model is the one to copy: permissions are an **immutable object behind a
reference, swapped with `Interlocked.CompareExchange`** (`CommandPermissionSet.cs:12-13`, CAS loops
at `User.cs:174,229,303,358,400,441`), and the user object itself swaps through
`UserHandle.TrySetUser` (`UserHandle.cs:48-56`). Readers never lock and never see a torn set. That
is a lock-free RCU in all but name, and unlike dragonfly's push it needs **no cross-thread write to
per-connection state**.

### 2.6 The re-entry surface in OUR tree — the coverage matrix

Every path that can reach a command handler without passing the ordinary
`IoLoop::parse_and_dispatch` gate. **A missed row here is a privilege bypass, not a bug.**

| # | Path | Site | Reaches a handler? | Required ACL treatment |
|---|---|---|---|---|
| 1 | ordinary single-key route | `io_loop.h:677-727` | yes | the gate (§2.9). Key slice is `op->arg(spec->first_key)`, already materialised at `:678`. |
| 2 | scatter / multi-key (36 rows) | `io_loop.h:535-655` via `xshard_prepare` | yes | gate **before** `xshard_prepare`, iterating the declared range. All 36 MultiShard rows have `first_key > 0`. |
| 3 | `ConnLocal` (28 rows) | `io_loop.h:457-472` | yes | gate. `ECHO`/`PING` are cheap but `CLIENT`, `CONFIG`, `INFO`, `COMMAND`, `SCRIPT`, `SAVE`, `BGSAVE`, `LASTSAVE` are `Admin` and are exactly what `-@admin` must stop. |
| 4 | `PubSub` (9 rows) | `io_loop.h:442-453` | yes | gate **plus** the channel check, §2.7. |
| 5 | subscriber-mode early replies | `io_loop.h:419-441` | short-circuits **before** the PubSub branch | The `PING`/`RESET`/restricted replies at `:419-441` bypass any gate placed at `:442`. Gate must sit **above** `:413`. |
| 6 | `MULTI` queue + `EXEC` | `io_loop.h:404-407` → `multi.inc:908` | yes, twice | check at queue time **and** replay time, §2.8. |
| 7 | Lua `redis.call` | `scripting.cc:445-545` (`redis_dispatch`) | yes, on the **EX thread** | command-bit check per call; key check hoisted to route time, §2.7. |
| 8 | `SORT ... BY/GET/STORE` | `t_server.cc:920` (`keys=1..1/1`) + the scatter lowering | yes | vanilla's unrestricted-read escalation, §2.3. |
| 9 | blocking re-entry after park | `io_loop.h:480-533` + `blocking.inc` | owner replies directly; no re-dispatch | §2.8 rules. |
| 10 | `WATCH` | `t_server.cc:924` (`CL|TX`, `keys=1..-1/1`) | goes through `multi_dispatch_entry` | key check over its declared range. |
| 11 | keyspace-notification delivery | `pubsub.inc` fanout | no command dispatch | **none** — §0.7. |
| 12 | snapshot load (`--load`) / `SAVE` | `snapshot.cc` | no client | none. No fake-client identity exists in our tree — the `u == NULL` escape at `acl.c:1842` has no analogue and must not be invented. |

Rows 5, 7, 8 and 9 are the ones a naive implementation gets wrong.

### 2.7 Channels, revocation, and the Lua hoist

**Channel argument positions.** `CommandSpec` has no channel metadata and does not need it — nine
rows, hand-tabled, mirroring `db.c:2303-2313` exactly:

| Command | positions | mode | checked? |
|---|---|---|---|
| `SUBSCRIBE` | `argv[1..]` | plain name, glob-matched vs ACL patterns | **yes** |
| `SSUBSCRIBE` | `argv[1..]` | plain name, glob-matched | **yes** (`db.c:2305`) |
| `PSUBSCRIBE` | `argv[1..]` | pattern, **literal `strcmp`** vs ACL patterns | **yes** |
| `PUBLISH` | `argv[1]` only | plain name, glob-matched | **yes** |
| `SPUBLISH` | `argv[1]` only | plain name, glob-matched | **yes** (`db.c:2311`) |
| `UNSUBSCRIBE` / `PUNSUBSCRIBE` / `SUNSUBSCRIBE` | — | — | **no** (`acl.c:1727` mask; `db.c:2306-2309`) |
| `PUBSUB` | — | — | no (it is `@admin`; the command bit is the gate) |

**One `&pattern` namespace covers both global and shard channels** — vanilla has no separate shard
ACL vocabulary, and `ACLShouldKillPubsubClient` walks `pubsubshard_channels` next to
`pubsub_channels` and `pubsub_patterns` (`acl.c:1969-1977`). Do not invent one.

Channel names are already materialised as `Slice`s. Re-located at `c9e049088` after the
shard-namespace commit moved them: `pubsub.inc:604` (`op.arg(i)`, the subscribe family),
`pubsub.inc:698` (`op.arg(i)` from index 2, the shard/`PUBSUB`-family loop) and `pubsub.inc:662`
(`op.arg(1)`, publish). The check goes immediately before those loops, on the io thread that owns
the connection. **Three insertion points, not two** — a check written for the pre-shard tree would
cover only the first and the third.

**Revocation on `SETUSER` — what vanilla actually does.** Not "disconnect" and not "re-check":
`ACLKillPubsubClientsIfNeeded(new, original)` (`acl.c:1988-2014`) first asks
`getUpcomingChannelList` (`:1884-1937`) whether the new channel set is a **strict superset** of the
old — returning `NULL` (= no work) if any new selector has `ALLCHANNELS`, or if every old pattern is
present verbatim in the new union. Only when it is *not* a superset does it walk `server.clients`,
and for each client of that user check every `pubsub_patterns` (literal), `pubsub_channels` (glob)
and `pubsubshard_channels` (glob) entry (`ACLShouldKillPubsubClient`, `:1941-1984`), calling
`deauthenticateAndCloseClient` (`networking.c:1566-1577` — sets `c->user = DefaultUser`,
`authenticated = 0`, then `freeClientAsync`) on a violation. It also short-circuits entirely when
`pubsubTotalSubscriptions() == 0` (`:1990-1991`).

So: **kill, not unsubscribe, and only when narrowing.** Dragonfly is cruder — it kills *all* of a
user's connections if and only if the rule string contained `resetchannels`
(`acl_family.cc:164,168-172`), meaning a narrowing that does not use `resetchannels` leaves
subscriptions live forever. Garnet has no channels at all.

**Our shape.** Pub/sub is IO-owned (`pubsub.inc` is `#include`d inside `IoLoop`, `io_loop.h:153`)
and each channel has a **home io thread** (`pubsub_home_for`, `pubsub.inc:78-86`). A client's
subscription set is therefore distributed. The kill sweep must be an io→io broadcast, not a walk
of a global client list — which is a real cost, and the reason §5/§6 classify revocation as the
riskiest single sub-feature. Concretely: `ACL SETUSER` is a `ConnLocal` command on one io thread; it
publishes the new `AclPerm` (§2.9), then posts a `PubSubEvent`-shaped control message to every io
thread; each thread walks its **own** `clients()` list (`io_loop.h:320`), applies
`ACLShouldKillPubsubClient` locally against its own subscription view, and `mark_closing()`s
locally. No cross-thread `Client` write. Reuse `pubsub_post`'s transport (`pubsub.inc:63-76`) and
respect its invariant (SPEC-WAVEA §0.5 item 3): the producer must be an io channel so ordinary
executor completion traffic cannot fill the SPSC lane.

**The Lua hoist.** Because `key_declared` (`scripting.cc:365-370`) guarantees a script can only
touch keys in `KEYS`, the **key** half of the ACL check for `EVAL`/`EVALSHA` is complete at route
time over `argv[3 .. 3+numkeys-1]` — the same range `command_prepare_script_route` already walks
(`scripting.cc:486-495`). Nothing on the EX thread needs a key pattern.

The **command** half cannot be hoisted (script text is opaque), so `redis_dispatch`
(`scripting.cc:445`) must test the bitmap per `redis.call`, on the EX thread. That requires the
bitmap to be reachable from an executor. Two candidate carriers:
- **Reject:** a pointer on `Op`. `sizeof(Op) == 336` is owner law (`op.h:240`) and the tagged-union
  fields (`zc_ptr`/`zc_len`/`zc_shard`) are live for the plain path EVAL takes.
- **Adopt:** widen `ScriptContext` (`scripting.cc:297-304`, a stack local at `:697`, not
  footprint-locked) with `const AclPerm* perm`. It is constructed on the EX thread inside the
  script task; the pointer must therefore be handed across. Carry it in `Task` — but `Task` is a
  32-byte queue item and `multi.h:56-57` documents that its `scatter` field is already low-bit
  tagged. **Cleanest: stash the resolved `AclPerm*` in the `ScriptRoute` op's existing
  `mark_local_xshard()` slot is wrong (it stores a cursor). Use a per-shard "current script
  identity" slot** written by `xshard_execute`'s script arm from a value the IO thread placed in the
  scatter/local state, or — simpler and measurably free — **have the EX thread resolve the user by
  index**: put a `uint16_t acl_user_idx` on the Op's existing `route_flags_` neighbourhood only if a
  hole exists, otherwise pass the index through the already-allocated script state. **Decide this
  with an `offsetof` probe at implementation time, not now** (risk R5).

Fallback if no carrier fits without growing `Op`: since scripts are `ScriptRoute` and rare, gate the
whole feature — **a user without `+@all` may not run `EVAL`/`EVALSHA` in v1** (deny at route time
with the ordinary NOPERM). That is strictly safe, costs nothing, and is honest. Ship the fallback if
the carrier costs a byte of `Op`.

### 2.8 MULTI/EXEC and blocking

**MULTI/EXEC.** Vanilla checks twice (§0.4). Our queue-time site is `multi_handle_io`'s tail
(`multi.inc:885-905`) — the gate at §2.9 already runs before `multi_dispatch_entry`
(`io_loop.h:404-407`), so **queue-time is free**: a denied command never reaches the queue and
`session->queue_error = true` is set by the existing error path, giving the `EXECABORT` on `EXEC`
exactly as redis's `rejectCommand`→`CLIENT_DIRTY_EXEC` does.

Replay-time is the work. `multi_execute_task` (`multi.inc:1059`) runs on the **EX thread** from a
`MultiQueuedCommand` (`argv` as `std::vector<std::string>`, `spec` pointer, `:165-177`). Two
options:
- **(a) Re-check on IO at EXEC dispatch**, in `multi_handle_io`'s EXEC arm before building the
  shard list (`multi.inc:~840-883`). The queued argv and spec are both present there; the key
  enumerator `command_key_args` (`multi.inc:178-188`) already exists and yields exactly the
  positions to test. **This is the right answer**: it stays on the io thread, needs no cross-thread
  identity, and reproduces vanilla's *timing* (the re-check happens at EXEC, not at queue).
- (b) Re-check on the EX thread — needs the perm blob on the executor. Reject.

Divergence to accept and document: vanilla replies per element and **continues**; our EXEC lowering
builds a shard plan up front. Emitting the per-element NOPERM string while still executing the rest
requires the plan to carry per-element "denied" markers. That is ~40 lines in `multi.inc` and it is
the *only* faithful behaviour. If it is cut, the fallback is to fail the whole EXEC — and that
**must** be written down as a known divergence, because a differ will find it.

**Blocking.** Vanilla re-checks on serve (§0.5). We do not re-dispatch: `blocking_prepare` runs once
at `io_loop.h:486-487` and the owner replies later. Options:
- **(a) Check once at dispatch only.** Divergence window = the park duration, which for `BLPOP 0`
  is unbounded. A revoked user keeps being served.
- **(b) Check at dispatch, and again on the io thread at retire**, before the reply is staged. The
  retire path is io-side and has the `Client` and the `Op` (with argv still pinned by the ROB —
  `op.h:8-12`). Cost is one bitmap test + key loop on a path that runs once per *unblocked* command,
  not per command.
- **(c) Kill the blocked client on `SETUSER`,** matching what `DELUSER`/`LOAD` already do.

**Ruling: (b), with (c) for `DELUSER`.** (b) is cheap, sits on an already-cold path, and closes the
unbounded window. Note this makes us *stricter* than vanilla in one corner — vanilla re-checks at
serve, we check at both ends — which is safe in the right direction.

### 2.9 THE DESIGN — zero-cost when default

#### The publication model

```cpp
// src/core/acl.h  (new)
struct AclPerm {                       // IMMUTABLE once published. 32B bitmap + two small vectors.
    uint64_t allowed_commands[4];      // 256 ids; bit 255 reserved = "built from +@all" (§2.1)
    uint32_t flags;                    // ALLKEYS | ALLCOMMANDS | ALLCHANNELS
    std::vector<KeyPattern> patterns;  // {std::string glob; uint8_t rw;}  -- rw unused until %R~ ships
    std::vector<std::string> channels;
    std::vector<RuleToken>  rules;     // ordered, for LIST/SAVE/GETUSER replay only
};

struct AclUser {                       // STABLE for the username's lifetime. Never freed while a
    std::string name;                  // Client points at it (deleted users are killed first).
    std::atomic<const AclPerm*> perm;  // <-- the only mutable word
    std::vector<std::array<uint8_t,32>> passwords;   // SHA-256, admin-path only
    uint32_t user_flags;               // ENABLED | DISABLED | NOPASS   (admin-path only)
};
```

`Client` gains **one 8-byte `AclUser* user_`** (or a 4-byte index — decide by hole measurement,
below). `ACL SETUSER` builds a fresh `AclPerm`, `store(release)`s it into `perm`, and pushes the old
one onto a retire list. Readers do one `load(acquire)`.

**Why this and not dragonfly's push:** the push writes per-connection state from another thread,
which pure-2s forbids (`conn.h:1-14`). Here the *pointer on the Client never changes* — only the
word inside the stable `AclUser` does — so nothing cross-thread ever touches a `Client`.

**Why this and not "just mutate in place" (vanilla):** vanilla is single-threaded under a global
lock. We would be publishing a `std::vector` resize under a concurrent reader. The immutable-blob +
atomic-swap is Garnet's model (`CommandPermissionSet.cs:12-13`) and is the only lock-free shape that
survives our threading.

**Reclamation.** A retired `AclPerm` is freed once every io thread has completed one full parse pass
that began after the retirement. The tree already has the two halves: `Server::begin_live_config_
update`/`end_live_config_update` (`server.h:523-537`) for the publication seqlock, and the
deferred-destroy idiom (`ScatterArenaPool::defer_destroy`/`reap_deferred`, `xshard.h:70,73`;
`multi_reap_deferred`, `multi.h:62`) for the reap. **v1 may legitimately never free** — `ACL SETUSER`
is an admin op and an `AclPerm` is a few hundred bytes — but say so in the code rather than leaving
it implicit, and add a counter.

#### Where the check lives

One insertion point, in `IoLoop::parse_and_dispatch` (`io_loop.h:357-740`), placed **after** the
Wave-A NOAUTH gate and **before** the subscriber-mode branch at `:413` (matrix row 5):

```cpp
// --- Wave A: NOAUTH gate lands here (SPEC-WAVEA §3.5) ---

// ACL. acl_active_ is a LOOP-LOCAL bool latched once per parse pass from the live-config
// seqlock: false whenever no user has ever been created and no aclfile was loaded, i.e. the
// default server. One register test, predicted-false, and c->user_ is never loaded.
if (__builtin_expect(acl_active_, false)) {
    if (acl_check_entry(*this, conn, *op, spec, consumed)) continue;   // out-of-line, multi2 pattern
}
```

`acl_check_entry` is declared in a narrow `src/cmd/acl.h` and defined in an `.inc` textually included
by `xshard.cc`, exactly per the multi2 pattern (`multi.h:41-48`, SPEC-WAVEA §0.5 item 9). The hot
loop contains one predicted-cold call and no ACL data structures, so the layout of
`parse_and_dispatch` is unchanged when ACL is off.

Inside `acl_check_entry`, the ordering mirrors vanilla's cost model:

```
perm = c->user_->perm.load(acquire)
if (perm->flags & (ALLCOMMANDS|ALLKEYS|ALLCHANNELS)) == all three  -> OK       // redis 8.9 :1910-1917
if (!(perm->flags & ALLCOMMANDS) && !(spec->flags & NoAuth))
    if (!bit_test(perm->allowed_commands, spec->id)) -> NOPERM cmd             // 1 load + 1 AND
if (!(perm->flags & ALLKEYS) && spec->first_key > 0)
    for each key in the declared range: glob-walk patterns -> NOPERM key
if (!(perm->flags & ALLCHANNELS) && (spec->flags & PubSub))
    for each channel arg per the 6-row table -> NOPERM channel
```

**The key enumerator already exists and must be reused, not re-written:** `for_each_spec_key`
(`multi.inc:1248-1257`) and `command_key_args` (`multi.inc:178-188`) are the two existing spellings
of "walk `[first_key, last_key]` step `key_step`, clamping `last_key < 0` to `argc-1`". Promote one
into `command.h` and have `WATCH`, `MULTI` replay, and ACL all call it. Three copies of this loop is
how a key gets missed.

**Multi-key ops check every key**, because the enumerator walks the whole declared range —
`MGET`/`MSET`/`DEL`/`SINTERSTORE`/`BLPOP` are covered by construction (§0.8: 133 of 167 rows have
`first_key > 0`; 28 are open-ended; 2 are stepped, and the enumerator handles `key_step` because
`MSET` needs it). The check runs **before** `xshard_prepare` (`io_loop.h:536`), so a denied key
costs no scatter allocation.

#### The zero-cost claim, stated as something falsifiable

| Configuration | Added per-command work | Bar |
|---|---|---|
| no `aclfile`, no `user` lines, `requirepass` unset **or** set | **zero** — `acl_active_` is a loop-local `false`; `c->user_` is not loaded | **0 instructions/op** vs the Wave-A baseline (§5.5 cell a/b) |
| a custom user exists but the connection is `default` (`+@all ~* &*`) | one acquire load of `perm`, one flags load, one AND, one predicted-taken branch | ≤ **+4 instr/op** |
| custom user, `allkeys` | + one bitmap word load + AND + test | ≤ **+8 instr/op** |
| custom user, 1 `~pattern` | + `nkeys` × 1 `command_glob_match` | measure (§5.5 cell d) |
| custom user, 8 `~pattern`s, worst-case (match on the last) | + `nkeys` × 8 glob calls | measure — this is the pattern-tax curve |

`acl_active_` must be **false when no user other than `default` exists and `default` is
`+@all ~* &*`** — i.e. the Wave-A `requirepass` states do **not** turn ACL on, because a
password-only default user is still `+@all ~* &*`. That is what makes cell (b) free. Compute it once
at publication: `acl_active_ = (n_users > 1) || !default_is_unrestricted`.

#### Is a per-command result cache warranted?

**No.** Dragonfly's answer is instructive by omission: they cache the *permission set* per
connection but **not** the per-command decision, and their key loop rebuilds a `GlobMatcher` per
(glob, key) pair (`validator.cc:22-25`) — i.e. they did not think the decision was worth memoising
even at that cost. Vanilla's only cache, `aclKeyResultCache` (`acl.c:1655-1668`), memoises
`getKeysResult` **across selectors within one command**, not across commands — and it exists solely
because selectors made the key extraction re-entrant. With one root selector and key positions
already known from `CommandSpec`, we have nothing to memoise.

A decision cache would also be a correctness hazard: it must be invalidated on `SETUSER`, which
reintroduces exactly the cross-thread invalidation problem the design avoids. **Revisit only if cell
(e) shows the 8-pattern curve exceeding the 3% always-on budget** — and then the answer is a
first-pattern-hit reorder (move the last-matching pattern to the front, per connection), not a
decision cache.

### 2.10 The keyspec table (prerequisite for `%R~`/`%W~`, LATER)

`%R~`/`%W~` need per-key-position access flags. The minimal faithful form, added to `CommandSpec`:

```cpp
struct KeySpec { int16_t first; int16_t last; int16_t step; uint8_t flags; };  // ACCESS|INSERT|UPDATE|DELETE
// CommandSpec gains:  const KeySpec* key_specs; uint8_t n_key_specs;
```

Only the ~14 rows whose positions differ in access mode need more than one spec:
`SINTERSTORE SUNIONSTORE SDIFFSTORE ZRANGESTORE BITOP COPY SMOVE LMOVE RPOPLPUSH BLMOVE BRPOPLPUSH RENAME RENAMENX GETEX`
plus `SORT` (three specs, the last two `UNKNOWN`). Every other row is one spec derived mechanically
from its existing `(first_key,last_key,key_step)` plus `Write`/`Readonly`. **Derive the flags from
`redis74/src/commands.def`, row by row, not from judgement** — that is risk R1 and it is the same
discipline the category table needs.

Cost: ~180 lines of table, no hot-path change (the flags are read only inside the already-cold key
loop, and only when the user has a `%`-qualified pattern). Ship it as its own lane.

### 2.11 The fork's AUTH parse-fence — steal it

`wt-round-mainline/src/networking.c:5444-5451` makes `AUTH` a one-command parse fence: when
`authRequired(c)`, the parser lookahead is forced to 1 so pipelined frames arriving in the same
buffer as the `AUTH` are **parsed after** the identity flips, not before. Vanilla does the same for
a different reason (`redis74/src/networking.c:3792`, an unauthenticated-client hardening).

Our parse loop reads `spec` and dispatches within one pass (`io_loop.h:363-728`) and `AUTH` is
`ConnLocal`, executed synchronously at `:463` — so `c->user_` is already updated before the next
iteration reads it. **We get the property for free**, but only because `AUTH` is `ConnLocal`. Add a
comment at the `ConnLocal` arm saying so, and a test arm (§5.2 E9) that pipelines
`AUTH u p` + `GET restricted` in one TCP segment and asserts the second is evaluated under `u`.
This is the kind of invariant that a future "make AUTH async" change silently breaks.

### 2.12 `ACL LOG` — per-io-thread, merged on read

Vanilla keeps one global `list *ACLLog` with dedup: `ACLLogMatchEntry` (`acl.c:2602-2612`) merges an
entry with an existing one within `ACL_LOG_GROUPING_MAX_TIME_DELTA` (60000 ms) bumping `count` and
`ctime`, else prepends and trims to `acllog-max-len` (default 128, `config.c:3198`). Ten reply
fields (`acl.c:3066-3106`): `count reason context object username age-seconds client-info entry-id
timestamp-created timestamp-last-updated`.

We cannot have a global mutable list on the io hot path. **Dragonfly's shape is correct here**:
`ServerState::tlocal()->acl_log`, one bounded deque per thread (`acl_log.cc:24-47`), with `ACL LOG`
doing an n-way merge by timestamp across threads (`acl_family.cc:421-441`) and `RESET` fanning out.
Their knob help text is honest about the consequence — *"total number of entries are
acllog_max_len * threads"*.

**Ruling: dragonfly's storage, vanilla's reply shape and dedup.** Per-io-thread deque of
`acllog-max-len` entries, dedup within the thread's own deque (which is where the repeats will be,
since a connection is thread-pinned for life), merge-on-read, all ten fields. Do **not** ship
dragonfly's 6-field reply — a client library that parses `ACL LOG` will break on it.

---

### 2.13 Footprint — MEASURED, and the answer is a 4-byte index, not a pointer

The brief says a Wave-A lane is consuming H1 and ~13 bytes of H2 and instructs us not to bank on
those bytes. Correct instruction; here is the arithmetic, measured rather than assumed. Probe:
`scratchpad/aclprobe/holes.cc`, a declaration-order mirror of `Client`'s private section
(`src/net/conn.h:483-531`) whose `sizeof` match is the fidelity check. Built and run pinned to
CPUs 32-47 on 2026-08-26:

```
sizeof(Client) = 1984   alignof = 64
sizeof(Mirror) = 1984   (FAITHFUL)
H1: rpos_ ends at 12, rbuf_ starts at 16      -> HOLE =  4 B @ 12
H2: watch_dirty_ ends at 1957, sizeof = 1984  -> HOLE = 27 B @ 1957
  Wave-A projected: obj_bytes_@1960 obj_soft_since_s_@1968 authenticated_@1972 -> next free byte 1973
  ACL 8B AclUser*  would land @1976, ending 1984 -> FITS (0 B slack)
  ACL 4B user index would land @1976, ending 1980 -> FITS (4 B slack)
```

**An 8-byte `AclUser*` fits with exactly zero bytes to spare.** It ends on 1984 precisely. Any
reordering inside Wave A, any additional `bool`, any change to `watch_dirty_`'s alignment, and it
stops fitting — silently, until the `static_assert` fires at build time.

**Ruling: `uint32_t acl_user_idx_`, not `AclUser*`.** Three reasons:
1. It leaves **4 bytes of slack**, so the ACL lane does not become the thing that breaks when Wave A
   shifts by a byte.
2. It costs one extra indexed load from a global `AclUser* g_acl_users[kMaxAclUsers]` — a
   permanently-hot L1 line — and that load is on the **cold ACL path only**, never in the
   `acl_active_ == false` case where the field is not read at all.
3. It fails closed on `DELUSER`: a tombstoned slot yields a null and the check denies, whereas a
   dangling `AclUser*` is a use-after-free. `DELUSER` kills the user's connections first
   (`redis74/src/acl.c:2894`), so this is defence in depth, which is the right posture for the one
   subsystem whose bugs are privilege escalations.

`0` is the reserved index for `default`, so a freshly-adopted `Client` needs no initialisation
beyond the zeroing it already gets — and `acl_user_idx_ == 0` on every connection is what makes the
`acl_active_ == false` fast path provably correct.

**Mandate for the lane:** re-run `scratchpad/aclprobe/holes.cc` **after** the Wave-A merge, before
writing the field. If the slack is gone, the field does not ship as-is — find a different
representation (memory: `user-hardcode-or-delete`). Add
`static_assert(offsetof(Client, acl_user_idx_) + 4 <= sizeof(Client))` so the hole is load-bearing
by contract rather than by luck, and keep `static_assert(sizeof(Client) == 1984)` untouched.

`Op` is **not** touched by this design. `sizeof(Op) == 336` (`op.h:240`) stands.

---

## 3. (c) KNOBS

Per the knob-compat rule: shared features adopt the reference server's knob name, grammar and
semantics **exactly**. All four ACL knobs are shared features, so all four are byte-exact redis.

### 3.1 The knob table

| Knob | Type | Default | Redis source | Semantics to reproduce exactly |
|---|---|---|---|---|
| `aclfile <path>` | string | `""` (off) | `config.c:3098` — `createStringConfig("aclfile", NULL, IMMUTABLE_CONFIG, ALLOW_EMPTY_STRING, server.acl_filename, "", NULL, NULL)` | **IMMUTABLE**: boot-only, no `CONFIG SET`. Empty = no file. Non-empty enables `ACL LOAD`/`ACL SAVE`; empty makes both return the long "not configured to use an ACL file" error (`acl.c:2978-2982`). |
| `user <name> <rules...>` | conf-only directive | — | `config.c:546-554` → `ACLAppendUserForLoading` (`acl.c:2154-2200`) | **Not a config knob** — a bare directive handled before `lookupConfig` (`config.c:1159` lists it alongside `include`/`rename-command`/`loadmodule`/`sentinel`). Repeatable. Validated at parse into a fake user; applied at `ACLLoadUsersAtStartup`. Duplicate name → `EALREADY` → *"Duplicate user found. A user can only be defined once in config files"*. |
| `acl-pubsub-default <allchannels\|resetchannels>` | enum | **`resetchannels`** | `config.c:3136`, enum at `config.c:114-118` (`{"allchannels", SELECTOR_FLAG_ALLCHANNELS}, {"resetchannels", 0}`), default arg `0` | MODIFIABLE. Read at selector creation: `ACLCreateSelector` does `selector->flags = flags \| server.acl_pubsub_default` (`acl.c:345`), and `reset` re-applies it (`acl.c:1359-1360`). **The 7.4 default is `resetchannels`** — `redis.conf:1061` states it changed in Redis 7.0. |
| `acllog-max-len <n>` | ulong | **128** | `config.c:3198` — `createULongConfig("acllog-max-len", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.acllog_max_len, 128, INTEGER_CONFIG, NULL, NULL)` | MODIFIABLE, `0` = keep nothing. Per §2.12 ours is **per io thread**, so the effective total is `acllog-max-len × n_io`. Say so in `tomokv.conf`, as dragonfly does in its own flag help (`acl_log.cc:15-17`). |

**The startup mutual-exclusion error, verbatim** (`ACLLoadUsersAtStartup`, `acl.c:2551-2560`) — fires
when `aclfile` is set **and** at least one `user` line exists, and then `exit(1)`:

```
Configuring Redis with users defined in redis.conf and at the same setting an ACL file path is invalid. This setup is very likely to lead to configuration errors and security holes, please define either an ACL file or declare users directly in your redis.conf, but not both.
```

Keep the word "Redis" (same reasoning as the protected-mode message, SPEC-WAVEA §3.7).

**Not adopted, and why:**
- `acl-pubsub-default allchannels` is still accepted — it is the *value*, not a separate knob.
- Redis 8.9's `tls-auth-clients-user` (`redis/src/config.c:3515`) maps a TLS peer CN to an ACL user.
  **NEVER** here — TLS is a separate audit and the mechanism is `connGetPeerUsername`
  (`redis/src/networking.c:1700-1714`), which has no analogue.
- Dragonfly's `$<n>` db restriction (`acl_family.cc:814-829`) and `NAMESPACE:<ns>`
  (`acl_family.cc:1049-1055`), and valkey's `db=<id>`/`alldbs`/`resetdbs`
  (`valkey/src/acl.c:1154-1166`): **NEVER**. We are `SELECT 0` only. Note valkey's version renumbered
  `ACL_DENIED_*` to make room (`valkey/src/server.h:3345-3349`) — a good reminder that adopting a
  scope extension is not additive.
- Dragonfly's `--admin_port` / `skip_acl_validation` escape (`conn_context.cc:89-90`): **NEVER**.
  An ACL you can bypass by connecting to a different port is a different feature; we have one
  listener plus a unix socket, and the unix socket must **not** be exempt.

### 3.2 The conf-grammar work that must land FIRST (P0, §0.10)

Two defects in `load_conf_file` (`src/core/config.h:309-339`) block the `user` directive, and one of
them silently destroys credentials:

| # | Defect | Site | Fix |
|---|---|---|---|
| P0-a | strips from **any** `#`, not a leading `#` | `config.h:320` | trim first, then `if (line[0] == '#' \|\| line[0] == '\0') continue;` — redis's exact rule (`config.c:452`) |
| P0-b | `strtok` on whitespace: no quoting | `config.h:322` | a `sdssplitargs`-equivalent splitter honouring `"..."` and `'...'`, so `>"pass phrase"` and `~"key with space"` parse (redis: `config.c:455`). Note `ACLStringHasSpaces` (`acl.c:246-253`) still rejects spaces in *patterns* — quoting matters for **passwords**. |
| P0-c | every knob consumes exactly 0 or 1 value tokens | `config.h:334-335` + `parse_config_args` `:130-289` | `user` is variadic. Add a `--user` arm that consumes tokens until the next `--`-prefixed token, or (cleaner) special-case `user` in `load_conf_file` the way `pin` already is (`config.h:325-332`) and stash the whole line into a `std::vector<std::vector<std::string>>` for `ACLLoadUsersAtStartup`. |

P0-a is not hypothetical: `user alice on #<64hex> ~* +@all` currently becomes `user alice on`, i.e.
a **passwordless, unrestricted-looking** user declaration, with no error. Add a gate arm (§5.2 K3)
that asserts a `#hash` survives the conf round-trip, and a `tests/gate.sh` boot case for the
mutual-exclusion exit.

### 3.3 Interaction with existing tomokv knobs

| Existing knob | Interaction |
|---|---|
| `requirepass` (Wave A) | **Same state** (§0.11). `CONFIG SET requirepass x` = `ACL SETUSER default resetpass >x`; `""` = `nopass`. `aclfile` / `ACL LOAD` **override** it and leave `CONFIG GET requirepass` stale — upstream behaviour, match it. Must **not** flip `acl_active_` (§2.9), or the whole zero-cost claim evaporates for every password-protected deployment. |
| `protected-mode` (Wave A) | Second clause is *"no password set for the default user"* — in vanilla 7.4 the test is literally `server.protected_mode && DefaultUser->flags & USER_FLAG_NOPASS` (`redis74/src/networking.c:1279-1280`; SPEC-WAVEA §3.7 cites the same code at `redis/src/networking.c:1620-1621` in the 8.9 tree). Once ACL exists, that clause must read the **default user's** NOPASS flag, not a `requirepass != nullptr` shortcut, or `ACL SETUSER default >p` leaves protected-mode on. |
| `enable-debug-command` | Not present in this tree (`grep` over `src/` returns nothing; the registry dump has no `DEBUG` row). No interaction. If a `DEBUG` row ever lands it must be `@admin` + `@dangerous`. |
| `unixsocket` | Must be ACL-gated exactly like TCP. Dragonfly exempts UDS (`main_service.cc:1902-1904`); do not. |
| `--load` / snapshot | No client, no identity (matrix row 12). |
| `atomic`, `zc-min`, `maxmemory`, … | None. |

### 3.4 `ACL SAVE` file format, byte-exact

`ACLSaveToFile` (`acl.c:2469-2544`) writes, per user, one line:

```
user <name> <ACLDescribeUser(u)>\n
```

then `open(tmp, O_WRONLY|O_CREAT, 0644)` where tmp is `<filename>.tmp-<pid>-<mstime>`
(`:2496-2499`), a write loop retrying `EINTR` (`:2506-2516`), `redis_fsync` (`:2517`), `close`,
`rename` (`:2525`), `fsyncFileDir` (`:2530`). **Copy the whole dance** — Garnet truncates the real
file in place (`AccessControlList.cs:235`, `append: false`) and loses the ACL on a crash mid-write.

`ACLDescribeUser` (`:846-891`) emits, in order: user flags in `ACLUserFlags[]` declaration order
(`acl.c:122-130` — `on, off, nopass, skip-sanitize-payload, sanitize-payload`; the comment at `:123`
says the array order dictates the emitted order), then `#<hash>` per password, then per selector
`ACLDescribeSelector` (`:803-838`) = keys, then channels (always `resetchannels`-prefixed unless
`allchannels`), then command rules (always `+@all`/`-@all`-prefixed). Non-root selectors are wrapped
in ` (...)` (`:881`).

A nontrivial sample line, from that code path:

```
user alice on #8c6976e5b5410415bde908bd4dee15dfb167a9c873fc4bb8a81f6f2ab448a918 ~app:* %R~cache:* resetchannels &news.* -@all +@read +get +set -del
```

and the default user, unmodified:

```
user default on nopass sanitize-payload ~* &* +@all
```

**Reader** (`ACLLoadFromFile`, `:2272-2464`): `fgets` in 1024-byte chunks into one sds
(`:2286-2287`), split on `\n`, each line `sdstrim(" \t\r\n")`, blank skipped, **`#`-leading lines are
an error in 7.4** (`:2329-2336`, "should start with user keyword"). We adopt redis **8.9's**
relaxation here — skip `#` lines as comments (`redis/src/acl.c:2394`) — for one reason: our conf
loader will accept them after the P0-a fix, and having the ACL file be stricter than the conf file
about the same directive is a support trap. Document that comments are **not preserved** across
`ACL SAVE`, which is also 8.9's behaviour.

### 3.5 `CONFIG REWRITE` and `user` lines

`rewriteConfigUserOption` (`config.c:1406-1437`): if `aclfile` is set, mark `user` processed and emit
nothing (so existing `user` lines in the conf are commented out); otherwise emit one
`user <name> <ACLDescribeUser>` line per user from the `Users` rax. We have no `CONFIG REWRITE`
today; when it lands, this is the shape.

---

## 4. (d) STEAL / AVOID

### Steal

1. **The root-selector short-circuit** — redis 8.9 `acl.c:1910-1917`. If the first selector carries
   `ALLCOMMANDS|ALLKEYS|ALLCHANNELS`, return `ACL_OK` before touching anything. Upstream added this
   after 7.4 precisely because it is the common case. §2.9.
2. **Garnet's immutable-permission-object + atomic swap** — `CommandPermissionSet.cs:12-13`
   (*"Wraps up command permissions behind a reference so it can be atomically swapped"*), CAS loops
   at `User.cs:174,229,…`, handle swap at `UserHandle.cs:48-56`. The only publication model that is
   lock-free for readers **and** needs no cross-thread write to per-connection state. §2.9.
3. **Garnet's `+@all` singleton pointer compare** — `CommandPermissionSet.cs:60-63` + `User.cs:157`.
   The unrestricted check becomes one compare against a known constant. Our flags-word version is
   the same idea with one fewer indirection.
4. **Garnet's out-of-line failure path** — `[MethodImpl(NoInlining)] OnACLOrNoScriptFailure`,
   `AdminCommands.cs:186-193`, with the stated reason: *"Failing should be rare … hide this behind a
   method call to keep icache pressure down"*. This is the multi2 pattern arrived at independently.
5. **Garnet's scatter-path warning, as a standing invariant** — `ACLParser.cs:259-262`. A batched
   fast path that skips the per-command gate is sound **only while key ACLs do not exist**. We have
   three such paths; §2.6 rows 2, 6, 7. Put this comment, adapted, at `xshard_prepare`'s call site.
6. **Dragonfly's per-connection permission snapshot** — `conn_context.h:377-389`. The *snapshot* is
   the good idea; the eager cross-thread push that maintains it (`acl_family.cc:116-137`) is not
   available to us. §2.9 keeps the snapshot semantics with a stable-handle indirection.
7. **Dragonfly's per-thread ACL log with merge-on-read** — `acl_log.cc:24-47`,
   `acl_family.cc:406-441`, plus the honest knob help (*"total number of entries are
   acllog_max_len * threads"*). §2.12.
8. **Vanilla's post-build bitmap `memcmp` assertion** — `acl.c:786-798`, *"aborting is better than a
   security risk in this code path"*. The one place a display bug becomes a privilege bug.
9. **Vanilla's all-or-nothing staging, in both forms** — temp-user for `SETUSER`
   (`acl.c:2093-2124`), temp-rax for `LOAD` (`acl.c:2296-2463`). Three independent implementations
   converge on it (redis, dragonfly `acl_family.cc:294-361`, garnet `AccessControlList.cs:174-215`).
10. **Vanilla's superset test before killing pub/sub clients** — `getUpcomingChannelList`
    (`acl.c:1884-1937`) plus the `pubsubTotalSubscriptions() == 0` short-circuit (`:1990-1991`).
    Widening permissions must never disconnect anyone.
11. **Vanilla's `ACL SAVE` durability dance** — temp + fsync + rename + dir-fsync
    (`acl.c:2496-2534`).
12. **Vanilla's anti-enumeration `WRONGPASS` collapse** — `errno` set at `acl.c:1436`/`:1465`,
    discarded by the caller. Wave A already rules on it; ACL must not regress it.
13. **The fork's AUTH parse fence** — `wt-round-mainline/src/networking.c:5444-5451`. We get it free
    because `AUTH` is `ConnLocal`; §2.11 says to write that down and test it.
14. **`SORT`'s unrestricted-key-access escalation** — `sort.c:192` + the two refusals at `:233-237`,
    `:255-258`. The only correct answer for content-derived keys.

### Avoid

1. **A per-command decision cache.** §2.9. Nobody has one; it reintroduces cross-thread
   invalidation; and vanilla's only cache (`aclKeyResultCache`, `acl.c:1655-1668`) memoises key
   *extraction* across selectors, not decisions across commands.
2. **`USER_COMMAND_BITS_COUNT 1024` / 128-byte bitmaps** — `server.h:1067`. Sized for module
   commands assigned at runtime. Our registry is boot-frozen at 167 (§0.8), and grew by 3 during this audit — 256 bits leaves 89 of headroom.
3. **`ACLRecomputeCommandBitsFromCommandRulesAllUsers`** — `acl.c:662-695`. Exists only for module
   load/unload. No modules, no analogue, do not build it.
4. **`allowed_firstargs`** — `acl.c:933-958`, `:1685-1702`. Redis logs its own deprecation warning
   when you use it (`:1173-1175`). It costs an `sds**` of the full id space on the selector.
5. **Dragonfly's cross-thread write into connection state** — `acl_family.cc:116-137`. Illegal here.
6. **Dragonfly's write-lock-held cross-thread sweep** — the registry write lock is held across
   `TraverseConnections` (`acl_family.cc:143` → `166-167`), serialising every `AuthUser` for the
   duration.
7. **Dragonfly's per-(glob,key) matcher construction** — `validator.cc:22-25` builds a fresh
   `GlobMatcher` per pair. Our `command_glob_match` (`command.h:111`) is already a free function.
8. **Dragonfly's no-early-exit key loop** — `keys_allowed &= iterate_globs(key)` (`validator.cc:164`)
   evaluates every key after a denial.
9. **Dragonfly's string compares on the generic path** — `id.name() == "MOVE"` / `== "SELECT"`
   (`validator.cc:117-129`), for a db-restriction feature we do not have.
10. **Dragonfly's `ACL LOG` reply shape** — 6 fields (`acl_family.cc:920-951`) where redis has 10,
    and no dedup. A client library that parses `ACL LOG` breaks on it.
11. **Dragonfly's `ACL LIST` truncating hashes to 15 hex chars** — `acl_family.cc:834-835`. Makes
    `LIST` output non-round-trippable while `SAVE`'s is; and `LIST` is exactly what operators paste.
12. **Dragonfly's uppercase categories and `HYPERLOG` spelling** — `acl_family.h:121,148`.
    `+@hyperloglog` errors there.
13. **Dragonfly rejecting `#<hash>` from `ACL SETUSER`** — `acl_family.cc:146` passes `hashed=false`;
    only the file loader passes `true` (`:323`). Gratuitous.
14. **Dragonfly skipping the ACL check inside `EXEC`** — `InvokeCmd` (`main_service.cc:1678-1754`)
    has no check; the squasher has none either. A queued command whose permission was revoked
    **still executes**. This is the single largest correctness gap in any competitor tree.
15. **Dragonfly's admin-port / UDS `skip_acl_validation` escape** — `conn_context.cc:89-90`,
    `main_service.cc:1902-1904`.
16. **Dragonfly's unconditional alias deny** — `validator.cc:91-93`, a consequence of `family_`/
    `bit_index_` being uninitialised members on cloned `CommandId`s (`command_id.h:103-104`). A
    fail-closed patch over an initialisation bug.
17. **Dragonfly's `acl_categories_ ^= cat`** — `user.cc:157`. XOR, not `&= ~`. `-@string` on a user
    that never had `@string` **sets** the bit. Cosmetic today only because nothing reads it.
18. **Dragonfly dropping `NAMESPACE:` on save** — `RegistryToString` (`acl_family.cc:227-254`) never
    writes it, so it is silently lost across `SAVE`/`LOAD`. Whatever we store, we serialise.
19. **Garnet's `ACL DELUSER` not killing sessions** — `AccessControlList.cs:97-104`. Live sessions
    hold a `UserHandle` and keep full permissions after deletion. The docs claim otherwise.
20. **Garnet's `reset` not clearing `nopass`** — `User.cs:482-492`. `nopass` then `reset` leaves the
    user passwordless.
21. **Garnet's two different `WRONGPASS` strings by arity** — `BasicCommands.cs:1501-1510`.
    Defeats the anti-enumeration property.
22. **Garnet's `ACL SAVE` truncate-in-place** — `AccessControlList.cs:235`.
23. **Garnet advertising a category that throws** — `+@stream` is in `ACL CAT` output and raises
    `ERR Unable to obtain ACL information, this shouldn't be possible` (`User.cs:120-123`) because
    zero commands carry the tag. **We will have two empty categories (`stream`, `geo`)**; empty must
    mean "legal no-op", tested (§5.2 G6).
24. **Garnet's unlocked `DescribeUser`** — `User.cs:551` reads `_passwordHashes` without the lock
    every other accessor takes; `ACL LIST` concurrent with `>pass` can throw.
25. **Valkey's `ACL_DENIED_*` renumbering** — `valkey/src/server.h:3345-3349`. Internal constants,
    but a reminder that adding a scope dimension is not additive.

---

## 5. (e) VALIDATION + BENCH PLAN

Every arm follows the vacuous-validation rule: it asserts a **mechanism fired**, not that nothing
crashed. "0 permission bugs" proves nothing unless the gate **opened** — so every enforcement arm
must observe a denial counter increment, and the counter must be in the shipping binary.

New counters, in `LoopSignals` beside `accepts`/`rejected_conns` (`src/core/signal.h:76-80`), all
surfaced in `INFO` alongside the existing `# Stats` block (`t_server.cc:733-735`), mirroring redis's
`server.acl_info` fields, which in 7.4 are exactly four and live in the `# Stats` section
(`redis74/src/server.c:5445-5448`: `acl_access_denied_auth`, `_cmd`, `_key`, `_channel`):

```
acl_access_denied_cmd      acl_access_denied_key      acl_access_denied_channel
acl_access_denied_auth     acl_pubsub_clients_killed  acl_perm_retired
```

### 5.1 What can be differential, and what cannot

`tests/differ.py` boots a vanilla oracle and diffs every reply byte. Adding an `acl` suite is the
right move, but three classes of arm cannot go through it:

| Class | Differential? | Why / what to do instead |
|---|---|---|
| enforcement matrix (`NOPERM` vs `OK` per command class) | **YES** | identical command streams, identical `ACL SETUSER` prefix; every reply byte must match |
| error strings (`WRONGPASS` / `NOPERM` / `NOAUTH`) | **YES** | this is what the differ is *for* |
| `ACL GETUSER` reply | **YES** | field order is fixed by `acl.c:2909-2947`; the values are deterministic |
| grammar round-trip `SETUSER → GETUSER → SAVE → LOAD → GETUSER` | **YES** for GETUSER; **directed** for the file | file *paths* differ; compare the two servers' saved files byte-for-byte in the harness, not through the protocol |
| `ACL LIST` / `ACL USERS` **ordering** | **NO** — directed assert | vanilla iterates a **rax** in lexicographic key order (`raxSeek(&ri,"^",…)`, `acl.c:2955`). If our registry is not lexicographically ordered the arrays differ while both are correct. **Ruling: order lexicographically by username and make it differential after all** — it is free and it removes a whole class of "correct but different" noise. Assert the order in a directed arm too. |
| `ACL GENPASS` | **NO** — directed | random. Assert length == `(bits+3)/4`, charset `[0-9a-f]`, two calls differ, and the three error cases. |
| `ACL LOG` | **NO** — directed | `age-seconds`, `timestamp-*`, `client-info` all vary. Assert field **names** and count (10), `reason`/`context`/`object`/`username` values, and `count` incrementing on a repeat within 60 s. |
| `ACL WHOAMI` after AUTH switches | **YES** | |
| pub/sub revocation kill | **NO** — directed | the *timing* of the disconnect is not byte-comparable. Assert: socket closed, `acl_pubsub_clients_killed` incremented, and that a **widening** `SETUSER` kills nobody. |
| `LOAD` all-or-nothing | **partly** | the error *text* enumerates every bad line and includes the filename — differential only if the filenames match. Assert directed: pre-state preserved, error names every broken line, trailing `WARNING: ACL errors detected…`. |
| conf-file `user` directives | **NO** — directed boot arms | boot-time; assert via `ACL LIST` after boot. |

### 5.2 Directed arms — `tests/acl.py HOST PORT` plus purpose-booted cases

**Grammar round-trip (G)**

| Arm | Assertion |
|---|---|
| G1 | `SETUSER u on >p ~a:* &c.* +@read +get -del` → `GETUSER u` shows exactly those, in vanilla's field order, `commands` starting `-@all ` |
| G2 | `SETUSER u +@all -get` → `GETUSER u` `commands` starts `+@all ` (the reserved-bit trick, §2.1) |
| G3 | `SAVE` → read the file → `SETUSER u reset` → `LOAD` → `GETUSER u` byte-identical to G1's |
| G4 | `%RW~k` round-trips as `~k` (`sdsCatPatternString`, `acl.c:329-330`) — expected asymmetry, not a bug **(only when `%R~` ships)** |
| G5 | `>p` twice → one password. `#<hash>` of the same password as an existing `>p` → still one |
| G6 | `+@stream` and `+@geo` (**empty categories** in our registry) → `+OK`, no crash, `GETUSER` shows the rule. This is the Garnet bug (`User.cs:120-123`) |
| G7 | `+@nosuchcat` → `ERR Error in ACL SETUSER modifier '+@nosuchcat': Unknown command or category name in ACL` |
| G8 | `~p` after `allkeys` → the `EEXIST` message verbatim; `&p` after `allchannels` → the `EISDIR` message |
| G9 | `#<63 hex>` and `#<64 UPPERCASE hex>` → the `EBADMSG` message |
| G10 | `sanitize-payload` / `skip-sanitize-payload` accepted as **no-ops** (§1.2 NEVER row) and **not** emitted by `GETUSER`/`LIST` — document the divergence from vanilla, which does emit them |
| G11 | `reset` → `off`, `-@all`, no passwords, no keys, channels per `acl-pubsub-default` |
| G12 | `+select\|0` → the first-arg refusal (`ECHILD`-equivalent wording) |
| G13 | Rule ordering preserved: `+@read -get +get` → `GETUSER` `commands` ends `+get`, and the bitmap `memcmp` assertion (§2.2) does not fire |

**Enforcement matrix (E)** — for each, assert the reply **and** the matching counter increment

| Arm | Assertion |
|---|---|
| E1 | per command **class** (string/hash/list/set/zset/admin/pubsub/transaction/scripting), one representative each, denied and allowed |
| E2 | **denied key inside `MGET`** — `~a:*`, `MGET a:1 b:1` → `NOPERM No permissions to access a key`, `acl_access_denied_key`++, and **`a:1` is not returned** |
| E3 | **denied key inside `MSET`** (key_step 2) — `MSET a:1 v b:1 v` → denied, and **neither key is written** (check with an unrestricted connection) |
| E4 | **denied key inside `DEL`** (open-ended range) → denied, nothing deleted |
| E5 | **denied key inside `SINTERSTORE`** — destination denied, sources allowed, and the mirror case |
| E6 | **denied inside `EXEC`** — queue `GET a:1` + `GET b:1` under `~a:*`; `b:1` is refused **at queue time** with the ordinary `NOPERM` and `EXEC` returns `EXECABORT` |
| E7 | **`SETUSER` between `MULTI` and `EXEC`** — queue two allowed commands, revoke one, `EXEC`: assert the **EXEC-specific** `-NOPERM ACLs rules changed between…` string (§0.4) and that the *other* element still executed |
| E8 | **denied inside Lua `redis.call`** — `EVAL "return redis.call('GET', KEYS[1])" 1 b:1` under `~a:*` → denied at route time (key check hoisted, §2.7); and `EVAL "return redis.call('DEL', KEYS[1])" 1 a:1` under `-del` → denied on the **command bit**, from the EX thread |
| E9 | **AUTH parse fence** (§2.11) — send `AUTH u p\r\nGET restricted\r\n` in **one** TCP segment; assert the `GET` is evaluated under `u`, not under the pre-AUTH identity |
| E10 | **blocking re-entry** — `BLPOP k 0` under a user allowed `k`; revoke `k`; `LPUSH k v` from another connection; assert the blocked client gets `NOPERM`, not the value (§2.8 option b) |
| E11 | **subscriber-mode bypass** (matrix row 5) — subscribe, then send `PING`/`RESET`/a restricted command while subscribed; assert the ACL verdict is applied, i.e. the gate sits above `io_loop.h:413` |
| E12 | `SUBSCRIBE` denied channel → `NOPERM No permissions to access a channel`; `PSUBSCRIBE` matched **literally** (`&news.*` allows `PSUBSCRIBE news.*` but **not** `PSUBSCRIBE news.a*`); `PUBLISH` checks `argv[1]` only; **`UNSUBSCRIBE` always allowed** |
| E12s | **shard channels** (§0.6): `SSUBSCRIBE`/`SPUBLISH` gated by the **same** `&pattern` set as their global twins — `&news.*` must allow `SSUBSCRIBE news.a` and deny `SSUBSCRIBE other`; `SUNSUBSCRIBE` always allowed. Assert there is no separate shard namespace, and cover the third insertion point (`pubsub.inc:698`) that a pre-shard implementation would miss |
| E13 | `SORT k BY w_*` and `SORT k GET p_*` under a restricted user → the two `sort.c` refusal strings; under `~*` → allowed |
| E14 | `WATCH` on a denied key → denied |
| E15 | unix socket connection is gated identically to TCP |
| E16 | **whole-registry sweep**: for all 167 rows, a `-@all` user is denied everything except the `NoAuth` set; a `+@all ~* &*` user is allowed everything. Iterate `command_registry_at(0..command_registry_size()-1)` rather than spot-checking, and rather than hard-coding a count that this audit already watched change |

**Errors, identity, lifecycle (A)**

| Arm | Assertion |
|---|---|
| A1 | `AUTH nosuchuser x` and `AUTH realuser wrongpass` → the **same** `WRONGPASS` bytes |
| A2 | `AUTH` to an `off` user → `WRONGPASS` |
| A3 | `WHOAMI` tracks `AUTH` switches and returns `default` after `RESET` |
| A4 | `DELUSER default` → the refusal string; `DELUSER u` → count, **and u's connections are closed** |
| A5 | `SETUSER default off` + `requirepass` unset → new connections get `NOAUTH` (the `USER_FLAG_DISABLED` clause of `authRequired`) |
| A6 | `CONFIG SET requirepass x` then `ACL GETUSER default` shows one `#hash` and no `nopass` |
| A7 | `ACL LOAD` from a file setting `default`, then `CONFIG GET requirepass` returns the **stale** value (§0.11) |
| A8 | `SETUSER` is atomic: a rule list with a bad rule in the middle leaves the user **completely unchanged** |
| A9 | `acl_active_` is **false** with only `default` + `requirepass` set, and **true** after the first `SETUSER` of a second user. Expose it (INFO or `DEBUG`-equivalent) so the arm can assert it — otherwise the zero-cost claim is untestable, which is the vacuous-validation trap |

**Knobs / boot (K)**

| Arm | Assertion |
|---|---|
| K1 | `aclfile` + a `user` line → the verbatim mutual-exclusion message and `exit(1)` |
| K2 | `ACL LOAD`/`SAVE` with no `aclfile` → the long "not configured" error |
| K3 | **P0-a regression** (§3.2): a conf `user alice on #<64hex> ~* +@all` line survives; `ACL GETUSER alice` shows the hash. Then a leading-`#` line is skipped as a comment |
| K4 | P0-b: `user bob on >"pass phrase"` parses and `AUTH bob "pass phrase"` succeeds |
| K5 | `acl-pubsub-default` default is `resetchannels`; a new user has no channels; set `allchannels` and a new user has `&*` |
| K6 | `acllog-max-len 0` → `ACL LOG` returns an empty array and no allocation happens |
| K7 | **LOAD all-or-nothing**: a file with a good line 1 and a broken line 2 → the pre-existing users are **unchanged**, and the error text names line 2 and ends with `WARNING: ACL errors detected, no change to the previously active ACL rules was performed` |
| K8 | `ACL SAVE` writes via temp+rename: assert no `*.tmp-*` file survives, and that killing the server mid-`SAVE` never leaves a truncated file |

**Revocation (R)**

| Arm | Assertion |
|---|---|
| R1 | subscribe to `news.a`; `SETUSER u resetchannels &other` → connection **closed**, `acl_pubsub_clients_killed`++ |
| R2 | subscribe to `news.a`; `SETUSER u &news.* &more` (**widening**) → connection **stays open**, counter unchanged (the superset test, `acl.c:1930-1934`) |
| R3 | no subscribers at all → `SETUSER` narrowing kills nobody and does no sweep (`pubsubTotalSubscriptions()==0` short-circuit) |
| R4 | subscriptions spread across **several io threads** (channels with different `pubsub_home_for`) → all violating connections killed, none missed. This is the arm that catches a broadcast that only swept the issuing thread |
| R5 | `ACL LOAD` that removes a user → that user's connections closed |

### 5.3 ASAN + torture

Run the E and R arms under the ASAN build (`gate.sh:28-32`). The `.make-settings` cached-`SANITIZER`
trap applies — `ldd`-check the binary. The specific hazard ASAN is for: `AclPerm` reclamation
(§2.9). Add a torture arm that runs `ACL SETUSER` in a tight loop against a saturating p32 load and
asserts zero ASAN reports and `acl_perm_retired` > 0 — a retire count of zero would mean the arm
never exercised the reclaim path.

### 5.4 Differ suite

New `tests/differ.py` suite `acl`: a deterministic stream of `ACL SETUSER` variants followed by an
enforcement sweep, run against us and a **vanilla 7.4** oracle. Note the standing caveat recorded in
`AUDIT-SMALLS.md:2181-2188`: the 8.9 tree is not upstream 7.4, and matching *it* byte-for-byte is not
the same as matching redis. For ACL the two agree on everything we implement (§1.6), but the oracle
must be **`/tmp/claude-1000/redis74`**, built from that source, not `/home/user/Projects/redis`.

### 5.5 Bench cells — the pattern-tax curve

Instrument: loopback `instr/op` (memory: `tomokv-goodsize-nallocx-lesson` — it is the bisect
instrument for this class), plus throughput on the standard cells. Every cell is **p1 and p32**
(saturated-benching rule: single-conn never saturates). Boxguard: one server, one bench.

| Cell | Configuration | Bar |
|---|---|---|
| **a** | pre-ACL baseline (Wave-A binary, `requirepass` unset) | reference |
| **b** | ACL binary, no `aclfile`, no `user` lines, `requirepass` unset | **0 added instructions/op.** `acl_active_` false; the branch already exists from Wave A. Any delta means the multi2 out-of-lining failed |
| **c** | ACL binary, `requirepass` set only | **≤ +2/op** vs (a) with `requirepass` set. Must still be `acl_active_ == false` (§2.9) — if it is not, the design is wrong, not the number |
| **d** | one custom user, `+@all ~* &*`, connection authenticated as it | **≤ +4/op** — root-selector short-circuit only |
| **e** | custom user, `-@all +get +set ~*` | **≤ +8/op** — bitmap word load + AND, key loop still skipped by `ALLKEYS` |
| **f** | custom user, `-@all +get +set ~k:*` (1 pattern, matches) | measure. GET/SET are 1-key, so this is 1 glob call/op |
| **g** | as (f) with **8** patterns, the matching one **last** | measure — the pattern-tax slope. `8 × command_glob_match` per key |
| **h** | as (g) but on `MGET` with 8 keys | `64` glob calls/op — the worst realistic shape, and the one that decides whether §2.9's "no decision cache" ruling holds |

**Where the pattern cost lands:** in `command_glob_match` (`src/cmd/command.h:111`), the same
redis-compatible matcher `SCAN` and pub/sub already use — so the cost is a known quantity in this
tree, not a new dependency. `~*` never reaches it (the `ALLKEYS` flag short-circuits), which is why
cells (d)/(e) are flat and (f)-(h) are the only ones that can move.

**Decision rule:** if (h) exceeds the **3% always-on budget** relative to (a) *for a workload that
has ACL configured*, the fix is a first-match reorder inside `AclPerm::patterns` (promote the last
hit), **not** a decision cache (§2.9). If (b) or (c) show any delta at all, stop and fix the gate
placement before continuing — those two are the load-bearing claims.

### 5.6 Fork floor

The tomokv fork (`/home/user/Projects/wt-round-mainline`, redis 8.6.2) **kept redis's ACL intact**
(§0.2). Its ACL-on throughput is therefore a real floor:

| Cell | Arms |
|---|---|
| fork-floor GET/SET, p1 and p32 | fork with `-@all +get +set ~*` vs tomokv-cpp with the same user |
| fork-floor pattern | both with `~k:*` × 8 |

Plus the standing parity bar: every cell ≥ stable `730dc029f` (memory: `tomokv-parity-bar`), built
from a CLEAN worktree.

**Box note.** At audit time CPUs 64-127 and 224-255 were occupied by other lanes and all probes here
were pinned to 32-47. The bench cells above assume the standard geometry from `tests/gate_refs.txt`
(`p1_8c` = cores 0-7 / ratio 7:1 / 16 shards / loadgen 64-79; `p128_32c_*` = cores 0-31 /
ratio 18:14 / 64 shards / loadgen 64-127) and must be run when the box is exclusive.

---

## 6. (f) SIZE, BUILD ORDER, RISKS, SHELVE TRIGGER

### 6.1 Size estimate

| Component | LOC | Notes |
|---|---|---|
| `src/core/acl.h` — `AclPerm`/`AclUser`, registry, publication + retire, bit helpers | ~260 | the data model of §2.9 |
| `src/cmd/acl.h` + `src/cmd/acl.inc` — rule parser, `ACL` subcommand dispatch, describe/serialise, `acl_check_entry` | ~700 | vanilla's `acl.c` is 3241 lines but carries modules, selectors, first-args, cluster, the 1024-bit space and `ACLRecompute*`; our v1 scope is the root selector only |
| category table on `CommandSpec` | ~210 | 167 literals + the 21-name table. Mechanical, and the highest drift risk (R1) |
| channel-arg table (9 rows) + the check | ~70 | mirrors `db.c:2303-2313`, shard rows included |
| `src/core/io_loop.h` — one guarded call, the `acl_active_` latch, adopt-time index init | ~15 | additive; no restructuring |
| `src/net/conn.h` — `acl_user_idx_` + accessors + two `static_assert`s | ~12 | `sizeof(Client)` stays 1984 |
| `src/cmd/multi.inc` — EXEC-time re-check + per-element denial markers | ~90 | §2.8; the per-element half is most of it |
| `src/cmd/scripting.cc` — per-`redis.call` bitmap test (or the v1 `+@all`-only gate) | ~40 | §2.7 |
| `src/cmd/blocking.inc` — retire-time re-check | ~30 | §2.8 |
| `src/core/pubsub.inc` — channel check + the io→io revocation broadcast | ~150 | the riskiest 150 lines in the change |
| ACL LOG — per-thread deque, dedup, merge-on-read | ~140 | §2.12 |
| `src/core/config.h` — 3 knobs + the `user` directive + **P0-a/b/c conf-grammar fixes** | ~130 | §3.2 |
| `src/cmd/t_server.cc` — `init_config` rows, INFO counters, `CLIENT LIST` `user=` field | ~40 | `user=` is currently absent from our `CLIENT LIST` |
| `tests/acl.py`, differ `acl` suite, gate wiring | ~450 | ~50 arms |
| **Total** | **≈ 2300 LOC**, of which **≈ 1850 production** | |

### 6.2 Build order — four lanes, and what must not be merged together

| Lane | Contents | Can land alone? |
|---|---|---|
| **L0 — conf grammar** | P0-a/b/c (`config.h:309-339`), plus K3/K4 arms | **YES, and it must land first and separately.** It is a standalone correctness fix, it is testable without any ACL code, and shipping the `user` directive on today's loader would write silently-broken credentials |
| **L1 — category table** | `uint64_t acl_categories` on `CommandSpec`, the 21-name table, `ACL CAT` | **YES.** No behaviour change; `ACL CAT` is read-only. Landing it alone makes R1 auditable in isolation, which is exactly what a category table needs |
| **L2 — core ACL** | `AclPerm`/`AclUser`, publication, the gate, the parser, `SETUSER/GETUSER/DELUSER/USERS/LIST/WHOAMI/GENPASS/HELP`, key + command enforcement, `aclfile`/`user`/`LOAD`/`SAVE`, the `acl_user_idx_` field | **NO — this is one unit.** A half-landed check path is a privilege bug. But it explicitly **excludes** channels, revocation, EXEC/Lua/blocking re-entry, and ACL LOG |
| **L3 — re-entry closure** | channel check + pub/sub revocation broadcast, EXEC re-check + per-element denials, Lua `redis.call` bit test, blocking retire-time re-check | **NO — one unit with L2's ship gate.** L2 without L3 is *advertised* ACL with three bypasses (§2.6 rows 4, 6, 7, 9). It may be developed as a separate lane; it may not ship separately |
| **L4 — ACL LOG** | per-thread deque, dedup, merge-on-read, `acllog-max-len` | **YES, after L2+L3.** Observability, not enforcement |
| **LATER lanes** | keyspec table → `%R~`/`%W~` (§2.10); selectors + `clearselectors`; `+cmd\|subcmd` row splitting; `ACL DRYRUN` | each standalone |

**The ship gate is L0+L1+L2+L3 together**, with L4 immediately after. Landing L2 alone and calling
it "ACL" would be the mediocre ship the shelve-and-report rule exists to prevent.

### 6.3 Risk register

**R1 — Category-table drift vs vanilla. HIGHEST.** 167 rows × a 21-bit word, hand-assigned, on a table that grew by three rows during this audit alone. A wrong
bit is a silent privilege error: `+@read` granting a write, or `-@dangerous` failing to remove
`FLUSHALL`. Nothing in the build catches it, and a differ only catches it if the arm happens to name
that command. **Mitigation:** generate the table from `redis74/src/commands.def` (each `MAKE_CMD`
row's `acl_categories` argument) with a script, diff the generated file against the committed one in
`gate.sh`, and add arm E16's whole-registry sweep plus a per-category sweep asserting our
`ACL CAT <c>` membership equals vanilla's for every command we both have. **Verified: our 167
command names are a strict subset of redis 7.4's 251 top-level names** — `comm -23` of the registry
dump against `redis74/src/commands/*.json` returns empty. There is no command for which we would
have to *invent* a category assignment, which is exactly what makes the generation mechanical.

**R2 — The `%R~`/`%W~` trap (§0.9).** The temptation under schedule pressure is to ship `%R~`/`%W~`
against the flat key range "because it mostly works". It does not mostly work: it grants reads.
**Mitigation:** the grammar must **reject** `%` with an explicit error until the keyspec table lands.
A silent no-op (Garnet's choice for `~*`) is worse than a rejection here, because the operator
believes they configured something.

**R3 — Pub/sub revocation broadcast. Highest *implementation* risk.** Subscriptions are distributed
across io threads by `pubsub_home_for` (`pubsub.inc:78-86`), so the kill sweep is a fan-out that must
touch every thread and must not write another thread's `Client`. Getting it wrong is either a missed
revocation (security) or a cross-thread `Client` write (corruption). It also must respect the SPSC
invariant at `pubsub.inc:63-76`. **Mitigation:** arm R4 specifically spreads subscriptions across
threads; assert `acl_pubsub_clients_killed` equals the expected count exactly, not merely `> 0`.

**R4 — EXEC re-check semantics (§0.4).** Vanilla replies per element and continues. Our EXEC builds
a shard plan up front, so per-element denial needs plan-level markers. **Mitigation:** decide
explicitly — faithful (per-element) or documented divergence (whole-EXEC failure). Do not discover
this during implementation. Arm E7 is the test either way.

**R5 — The Lua identity carrier (§2.7).** No obvious place to hand an `AclPerm` to the EX thread
without growing `Op` (336, locked). **Mitigation:** the named fallback — deny `EVAL`/`EVALSHA` to any
user without `+@all` in v1 — is safe, free, and honest. Take it if the carrier costs a byte.

**R6 — Pattern-match cost (§5.5 cells f-h).** `nkeys × npatterns` glob calls on the hot path for
configured users. **Mitigation:** cells (f)-(h) are the gate; first-match reorder is the fix; a
decision cache is explicitly not.

**R7 — `Client` footprint knife-edge (§2.13).** The 8-byte form fits with **0 bytes** slack. Even
the 4-byte form has only 4. **Mitigation:** re-run `holes.cc` post-Wave-A-merge before writing the
field; ship the `offsetof` `static_assert`.

**R8 — `AclPerm` reclamation UAF.** A retired blob freed while an io thread holds the pointer.
**Mitigation:** v1 may never free (an `AclPerm` is a few hundred bytes and `SETUSER` is an admin op);
if reclamation ships, it rides the existing deferred-destroy idiom and the §5.3 ASAN torture arm must
show `acl_perm_retired > 0` — a zero would mean the arm proved nothing.

**R9 — Gate placement above the subscriber-mode branch (§2.6 row 5).** The early replies at
`io_loop.h:419-441` sit *above* the PubSub arm. A gate placed at the "obvious" spot next to the
PubSub branch is bypassable by any subscribed client. **Mitigation:** arm E11.

**R10 — `acl_active_` computed wrong.** If it turns true whenever `requirepass` is set, every
password-protected deployment pays the ACL path and cell (c) fails. **Mitigation:** compute it as
`(n_users > 1) || !default_is_unrestricted`; expose it; arm A9 asserts it.

**R11 — Conf P0-a shipping unfixed (§0.10).** Would write silently-broken credentials from a
correct-looking config file. **Mitigation:** L0 lands first, separately, with arm K3.

### 6.4 SHELVE recommendation trigger

Per the shelve-and-report rule, tell the owner to shelve ACL (branch + findings, reported in the
artifact, not shipped mediocre) if **any** of these is discovered:

1. **Cell (b) or (c) shows a non-zero instruction delta that cannot be removed.** The entire premise
   is that ACL is free when unconfigured. If the gate cannot be made free — e.g. because the
   `acl_active_` latch cannot be kept loop-local, or the multi2 out-of-lining still perturbs
   `parse_and_dispatch`'s layout — then always-on machinery is costing an unconfigured server, which
   violates the 3% budget on a feature most deployments will never enable. Shelve rather than ship a
   tax.
2. **The pub/sub revocation broadcast (R3) cannot be made exact.** If the io→io sweep cannot
   guarantee "every violating connection, no cross-thread `Client` write", then `&channel` is
   security theatre: the grammar accepts a restriction the server does not enforce for already-open
   subscriptions. Shelve **channels specifically** (ship L2 with `&`/`allchannels`/`resetchannels`
   *rejected* rather than silently unenforced) and report.
3. **The category table cannot be mechanically generated from `commands.def` (R1).** If it has to be
   hand-maintained, every future command row becomes a silent privilege-drift opportunity, and no
   test we can afford covers it. Shelve `+@category` — ship `+cmd`/`-cmd`/`allcommands`/`nocommands`
   only — and report.
4. **The `Client` field does not fit post-Wave-A (R7)** and no alternative representation is under
   4 bytes. Growing `Client` past 1984 is owner law, not a trade.
5. **Cell (h) shows the pattern tax exceeding 3%** *and* the first-match reorder does not recover it.
   Then key patterns are the wrong shape for this architecture and `~pattern` should be shelved in
   favour of command/category-only ACL — which is exactly what Garnet ships in production
   (§0.13), so it is a defensible product, not a failure.

Shelving item 2, 3 or 5 leaves a *smaller but honest* ACL. Shelving item 1 or 4 means the feature
does not land at all this wave. In every case: branch, write the findings into this file, and report
in the artifact.

---

## Appendix — where every change lands

| File | Change |
|---|---|
| `src/core/acl.h` | **new** — `AclPerm`, `AclUser`, the registry, publication + retire, bit helpers |
| `src/cmd/acl.h` | **new** — narrow entries: `acl_check_entry`, `acl_command_entry`, `acl_revoke_entry` (multi2 pattern, per `multi.h:41-48`) |
| `src/cmd/acl.inc` | **new** — bodies; textually `#include`d by `xshard.cc` beside `multi.inc` |
| `src/cmd/command.h` | `uint64_t acl_categories` on `CommandSpec` (`:52-70`); promote the key-range enumerator out of `multi.inc:1248-1257` |
| `src/cmd/commands.cc` | nothing (ids are already dense and boot-frozen, `:74-77`) |
| `src/cmd/t_string.cc`, `t_hash.cc`, `t_list.cc`, `t_set.cc`, `t_zset.cc`, `t_server.cc`, `scripting.cc` | one `acl_categories` literal per row (167 rows) |
| `src/cmd/t_server.cc` | `ACL` command row; `init_config` rows for `acl-pubsub-default`, `acllog-max-len` (`:211`); INFO counters beside `:733-735`; `user=` in `CLIENT LIST` |
| `src/core/io_loop.h` | the guarded gate above `:413`; `acl_active_` latch beside the other per-pass latches; `acl_user_idx_` init in `adopt_client` (`:312-328`) |
| `src/net/conn.h` | `uint32_t acl_user_idx_` in H2 (`:531` region); accessors; `static_assert(offsetof(...))`; `:538` stays 1984 |
| `src/core/pubsub.inc` | channel check before `:566` and `:618`; io→io revocation broadcast reusing `pubsub_post` (`:63-76`) |
| `src/cmd/multi.inc` | EXEC-time re-check in `multi_handle_io`'s EXEC arm using `command_key_args` (`:178-188`); per-element denial markers |
| `src/cmd/scripting.cc` | per-`redis.call` bit test in `redis_dispatch` (`:445-545`), or the v1 `+@all`-only gate |
| `src/cmd/blocking.inc` | retire-time re-check |
| `src/core/config.h` | **P0-a/b/c** conf-grammar fixes (`:309-339`); `aclfile`, `acl-pubsub-default`, `acllog-max-len` in `Config` (`:33-89`) and `parse_config_args` (`:130-289`); the `user` directive; mutual-exclusion check in `validate_config` (`:292-303`) |
| `src/core/signal.h` | six counters beside `:76-80` |
| `tomokv.conf` | the four knobs, annotated, including the per-thread `acllog-max-len` note and the `requirepass`-vs-`aclfile` override |
| `tests/acl.py` | **new** — the G/E/A/K/R arms of §5.2 |
| `tests/differ.py` | new `acl` suite (§5.4), oracle = `/tmp/claude-1000/redis74` build |
| `tests/gate.sh` | boot arms K1/K2, the ASAN torture arm (§5.3), the generated-category-table diff (R1) |
| `scratchpad/aclprobe/` | `probe.cc` (id-space width), `holes.cc` (Client holes) — re-run `holes.cc` post-Wave-A |
