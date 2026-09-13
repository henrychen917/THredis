# NOTES-ACLSEL — ACL selectors and enforcement

Branch `t-aclsel`, based on `perthread-locality`. Work used only cores 48–63 and ports
7550–7551. The behavioral oracle was the vanilla Redis 7.4.2 binary at
`/tmp/claude-1000/redis74/src/redis-server`, booted with `LC_ALL=C --save ''
--appendonly no --enable-debug-command yes`.

## What shipped

- `AclPerm` now owns an immutable ordered vector of `AclSelector` permission sets. The root set
  and each selector share the same command bits/rules, key patterns, channel patterns and flags.
- `ACL SETUSER` accepts packed selectors (`(~app:* +get)`), selectors fragmented across RESP
  arguments (`(`, `~app:*`, `+get`, `)`), empty selectors, multiple selectors, and
  `clearselectors`. `reset` clears selectors.
- `ACL GETUSER` reports the Redis RESP2 six-field selector rows; `ACL LIST`, `ACL SAVE`, ACL-file
  load, and `user ...` config lines serialize/parse the normalized parenthesized form.
- Admission tries the root set and then selectors. One whole permission set must authorize the
  command and every key/channel; command permission from one set cannot be combined with a key or
  channel pattern from another. The same evaluator is used by queue/EXEC and blocking rechecks.
- Pub/sub admission binds command and channel permission to one set. ACL-update revocation accepts
  a channel allowed by any root/selector channel set, so an unrelated root edit does not close a
  selector-backed subscriber; removing the selector does.
- New selectors inherit Redis's live `acl-pubsub-default` channel default. With `allchannels`, an
  empty selector reports `&*`; with `resetchannels`, it reports an empty channel string.
- Partial script selectors fail closed. This server does not yet enforce nested Lua/function
  commands for a restricted command set, so a selector that enables a `ScriptRoute` command is
  rejected unless the selector has `allcommands`. This avoids accepting/reporting a rule that the
  runtime cannot safely honor.

No command registry row, runtime knob, or `tomokv.conf` entry was added: selectors extend the
existing `ACL` command and use Redis's existing `acl-pubsub-default` knob unchanged.

## Design and architecture

The published `AclPerm*` remains immutable. `ACL SETUSER` builds a complete private image, validates
every selector, allocates/publishes one replacement permission blob, and retires the prior blob via
the existing ACL lifecycle. No shard/store path changed, and no MVCC/scatter machinery changed.

The selector vector is cold ACL state; neither `Op` nor `Client` changed (their footprint asserts
passed in every build). When ACL is inactive, IO dispatch still pays only the existing predicted
`security_check` branch and allocates no selector buffers. When ACL is active, permission checking
is out of line and may scan root plus selectors and their patterns. That is the feature-on cost; no
loopback performance number was requested or recorded.

## Compatibility audit

Every row below was checked on live target/oracle processes. `DIFFERS -> RESOLVED` includes the
pre-change target result and the final result. Exact raw replies are included for every remaining
or representative resolved difference.

| Area checked | Status | Redis 7.4 / TomoKV observation |
|---|---|---|
| Selector presence, packed grammar | **DIFFERS -> RESOLVED** | `ACL SETUSER selprobe reset on nopass -@all resetkeys resetchannels "(~app:* +get)"`: Redis `+OK\r\n`; before `-ERR Error in ACL SETUSER modifier '(~app:* +get)': ACL selectors are not supported\r\n`; final `+OK\r\n`. |
| Fragmented and empty selectors | **CONSISTENT after implementation** | `(~a:*`, `+get)`; `(`, `~a:*`, `+get`, `)`; `()`; and `( )` all return `+OK\r\n` and identical `GETUSER` rows. |
| Invalid/nested/user-level selector tokens | **CONSISTENT after implementation** | `(on +get)`, `(>pw +get)`, `(clearselectors +get)`, `((~a:* +get))`, bare `)`, and a trailing `)` all byte-match Redis `Syntax error` replies. |
| Unmatched opening parenthesis | **DIFFERS -> RESOLVED** | Redis and final target: `-ERR Unmatched parenthesis in acl selector starting at '(~a:*'.\r\n`; before: selector-not-supported error. |
| `clearselectors`, root `resetkeys`, and `reset` | **CONSISTENT after implementation** | `resetkeys` changes only the root; `clearselectors` removes selectors in order; `reset` reports `selectors *0`. |
| RESP2 `ACL GETUSER` selector reporting | **DIFFERS -> RESOLVED** | Before, the selector field was always `*0\r\n`. Redis and final target report the exact subtree `*1\r\n*6\r\n$8\r\ncommands\r\n$10\r\n-@all +get\r\n$4\r\nkeys\r\n$4\r\n~x:*\r\n$8\r\nchannels\r\n$0\r\n\r\n`. |
| `ACL LIST` normalization | **DIFFERS -> RESOLVED** | Redis and final target: `user listprobe on nopass sanitize-payload resetchannels -@all (resetchannels &news:* -@all +publish) (~app:* resetchannels -@all +get)`. Before, SETUSER rejected the selectors. |
| `ACL SAVE` / `ACL LOAD` / config-line parsing | **DIFFERS -> RESOLVED** | Final target round-trips the normalized LIST bytes, including whitespace-split parenthesized selectors; the old parser rejected the first `(` token. |
| Key-pattern grant and negative control | **DIFFERS -> RESOLVED** | Selector `(~app:* +get)`: Redis/final `GET app:1 -> $1\r\nA\r\n`, `GET other:1 -> -NOPERM No permissions to access a key\r\n`; before, the user could not be created. |
| Command rule/category and exclusion | **DIFFERS -> RESOLVED** | `(~cat:* +@string -set)` grants GET/STRLEN and returns `-NOPERM User ... has no permissions to run the 'set' command\r\n` for SET on both. |
| Root-selector and selector-selector composition | **CONSISTENT after implementation** | Root `+get ~root:*` and selector `+set ~app:*` remain alternatives. Two `+mget` selectors for `a:*` and `b:*` each grant one-key MGET but jointly deny `MGET a:1 b:1`. |
| Channel patterns and command binding | **DIFFERS -> RESOLVED** | `(&news:* +publish)` gives `PUBLISH news:x -> :0\r\n`; `PUBLISH other -> -NOPERM No permissions to access a channel\r\n`. Root `allchannels` cannot be combined with selector `+publish`. |
| Selector-backed SUBSCRIBE and revocation | **CONSISTENT after implementation** | An unrelated root edit delivers the next message and increments the target kill counter by zero; `clearselectors` closes exactly one subscribed connection and leaves an ordinary authenticated connection open. |
| `acl-pubsub-default allchannels` | **DIFFERS -> RESOLVED** | During implementation the selector channel field was `$0\r\n\r\n`; Redis returns `$2\r\n&*\r\n`. Final target returns `$2\r\n&*\r\n`. |
| MULTI/EXEC permission-image change | **CONSISTENT after implementation** | A selector-granted GET queues; after `clearselectors`, EXEC contains the exact Redis changed-ACL command-permission error. |
| Blocking permission-image change | **CONSISTENT after implementation** | With a proven-empty blocking key, replacing `(~block:* +blpop)` with `(~other:* +blpop)` before wake returns `-NOPERM No permissions to access a key\r\n` on both. |
| RESP3 `ACL GETUSER` container types | **DIFFERS — handed on** | Exact replies below. This is the pre-existing S6 gap recorded in `NOTES-EDGEPROTO.md`; selector contents are present, but fixing all GETUSER RESP3 container types is outside this lane. |
| `%R~` read/write key patterns inside selectors | **DIFFERS — handed on** | Redis: `+OK\r\n`. Final target: `-ERR Error in ACL SETUSER modifier '(%R~x:* +get)': Read/write key patterns are not supported until command key specifications are available\r\n`. This is the existing root-rule/key-spec limitation, not weakened here. |
| Partial script selector `(~x:* +eval)` | **DIFFERS — fail-closed** | Redis: `+OK\r\n`. Final target: `-ERR Error in ACL SETUSER modifier '(~x:* +eval)': Script commands in ACL selectors require allcommands\r\n`. Nested script-command ACL enforcement is absent; accepting/reporting this rule would be unsafe. `allcommands` selectors remain accepted. |

Exact RESP3 residual (`ACL GETUSER resid-r3`, selector `(~x:* +get)`):

```text
Redis 7.4:
%6\r\n$5\r\nflags\r\n~3\r\n$2\r\non\r\n$6\r\nnopass\r\n$16\r\nsanitize-payload\r\n$9\r\npasswords\r\n*0\r\n$8\r\ncommands\r\n$5\r\n-@all\r\n$4\r\nkeys\r\n$0\r\n\r\n$8\r\nchannels\r\n$0\r\n\r\n$9\r\nselectors\r\n*1\r\n%3\r\n$8\r\ncommands\r\n$10\r\n-@all +get\r\n$4\r\nkeys\r\n$4\r\n~x:*\r\n$8\r\nchannels\r\n$0\r\n\r\n

TomoKV final:
*12\r\n$5\r\nflags\r\n*3\r\n$2\r\non\r\n$6\r\nnopass\r\n$16\r\nsanitize-payload\r\n$9\r\npasswords\r\n*0\r\n$8\r\ncommands\r\n$5\r\n-@all\r\n$4\r\nkeys\r\n$0\r\n\r\n$8\r\nchannels\r\n$0\r\n\r\n$9\r\nselectors\r\n*1\r\n*6\r\n$8\r\ncommands\r\n$10\r\n-@all +get\r\n$4\r\nkeys\r\n$4\r\n~x:*\r\n$8\r\nchannels\r\n$0\r\n\r\n
```

## Tests and exact commands

Builds (lane brief's `-j8` limit):

```sh
make clean
make -j8
make asan
python3 -m py_compile tests/aclsel.py tests/differ.py
git diff --check
```

Release target and oracle boots used for the final matrix:

```sh
taskset -c 48-55 ./build/tomokv --port 7550 --bind 127.0.0.1 \
  --shards 8 --ratio 1:1 --atomic <0|1> --enable-debug-command yes \
  --dir /tmp/t-aclsel-target

LC_ALL=C taskset -c 56-63 /tmp/claude-1000/redis74/src/redis-server \
  --port 7551 --bind 127.0.0.1 --save '' --appendonly no \
  --enable-debug-command yes --dir /tmp/t-aclsel-oracle
```

Directed and differential commands, repeated under both `--atomic 0` and `--atomic 1`:

```sh
python3 tests/aclsel.py 127.0.0.1 7550
python3 tests/differ.py 127.0.0.1 7550 127.0.0.1 7551 aclsel 17
python3 tests/differ.py 127.0.0.1 7550 127.0.0.1 7551 aclsel 101
python3 tests/differ.py 127.0.0.1 7550 127.0.0.1 7551 aclsel 2027
```

Final release tails were identical in both atomic modes:

```text
aclsel: PASS (63 checks; parsing/reporting/root+selector/key/channel/command/EXEC/BLPOP/revocation fired)
DIFFER aclsel: 4235 checks, 0 diffs -> PASS (profiles=537, reporting=298, syntax=579, grants=1323, command_denials=577, key_denials=689, channel_denials=228)
DIFFER aclsel: 4235 checks, 0 diffs -> PASS (profiles=580, reporting=267, syntax=590, grants=1285, command_denials=543, key_denials=679, channel_denials=287)
DIFFER aclsel: 4235 checks, 0 diffs -> PASS (profiles=556, reporting=289, syntax=553, grants=1333, command_denials=617, key_denials=669, channel_denials=214)
```

The same directed battery (with an explicit branch for the documented fail-closed script rule)
also passes against the Redis oracle:

```text
aclsel: PASS (61 checks; parsing/reporting/root+selector/key/channel/command/EXEC/BLPOP/revocation fired)
```

ASAN/UBSAN boot and battery:

```sh
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
taskset -c 48-55 ./build/tomokv-asan --port 7550 --bind 127.0.0.1 \
  --shards 8 --ratio 1:1 --atomic 1 --enable-debug-command yes \
  --dir /tmp/t-aclsel-target
python3 tests/aclsel.py 127.0.0.1 7550
```

```text
aclsel: PASS (63 checks; parsing/reporting/root+selector/key/channel/command/EXEC/BLPOP/revocation fired)
stuck: live_conns=0 rob_not_quiesced=0 unsent_bytes_pending=0 | slots done=0 issued=0 free=0 flag_set=0
```

There were no ASAN/UBSAN diagnostics. The pre-adjustment sanitizer binary also ran the
4,235-check selector differ at seed 31337 with zero diffs; the adjustment affects only a
deliberately non-differable Redis divergence, and the final sanitizer-directed battery covers it.

Existing focused regressions:

```sh
python3 tests/acl.py 127.0.0.1 7550 /tmp/t-aclsel-users.acl
python3 tests/pubsub.py 127.0.0.1 7550
python3 tests/multi_exec.py 127.0.0.1 7550
python3 tests/blocking.py 127.0.0.1 7550
python3 tests/differ.py 127.0.0.1 7550 127.0.0.1 7551 compatintro 17
```

```text
acl: PASS (grammar, AUTH, enforcement/closure, revocation, ACL LOG, SAVE/LOAD)
pubsub: PASS (...)
MULTI/WATCH directed battery passed
BLOCKING PASS
DIFFER compatintro: 5588 checks, 0 diffs -> PASS (getkeys=1550, config_multi=635, client_rows=227, acl_mutations=398, notify_events=15, silence_controls=5)
```

`tests/gate.sh` was not run, per lane rules. Its ordinary both-atomic feature loop now includes
`aclsel`; `tests/differ.py --list-generators` also lists `aclsel`, so the differential gate will
discover it. No wire/NIC benchmark or loopback performance measurement was run.

## Scope handed on

1. RESP3 `ACL GETUSER` map/set container types remain the already-recorded S6 gap. Selector data is
   present, but the outer and nested containers use RESP2-style arrays.
2. `%R~`/`%W~` read/write key patterns still require command key-access specifications that this
   tree does not expose. Plain `~` key patterns are fully enforced in root and selectors.
3. Restricted script selectors are rejected until the script/function engine can enforce nested
   command ACLs against the same permission set. This is deliberately fail-closed; no selector is
   accepted and then silently ignored.
