# NOTES-SCRIPTSURF — scripting surface: SCRIPT family, RO variants, FUNCTION library

Lane D of wave 3. Branch `t-scriptsurf`. Server cores 40-43 / port 7240, oracle (vanilla redis
7.4.2) cores 44-47 / port 7245.

## What shipped

| command | form | notes |
| --- | --- | --- |
| `EVAL_RO` | `script numkeys key... arg...` | EVAL with the write gate armed; registry row is `Readonly` |
| `EVALSHA_RO` | `sha1 numkeys key... arg...` | same gate on the cached-source path |
| `SCRIPT KILL` | — | `NOTBUSY No scripts in execution right now.` — see "not supported" below |
| `SCRIPT DEBUG` | `YES\|SYNC\|NO` | `NO` → `+OK`; `YES`/`SYNC` refused; anything else → redis's `Use SCRIPT DEBUG YES/SYNC/NO` |
| `SCRIPT HELP` | — | our own text, not redis's (licence) |
| `FUNCTION LOAD` | `[REPLACE] <code>` | `#!lua name=<lib>` shebang; returns the library name |
| `FUNCTION DELETE` | `<lib>` | |
| `FUNCTION FLUSH` | `[ASYNC\|SYNC]` | both modes synchronous |
| `FUNCTION LIST` | `[LIBRARYNAME <lib>] [WITHCODE]` | RESP2 flat rows / RESP3 maps + flag sets |
| `FUNCTION STATS` | — | `running_script` nil + per-engine LUA counts |
| `FUNCTION DUMP` | — | our own frame (see below) |
| `FUNCTION RESTORE` | `<payload> [FLUSH\|APPEND\|REPLACE]` | all-or-nothing |
| `FUNCTION KILL` | — | `NOTBUSY` |
| `FUNCTION HELP` | — | our own text |
| `FCALL` | `fname numkeys key... arg...` | callback invoked as `fn(keys, args)` |
| `FCALL_RO` | same | refuses a function without `no-writes` |

Already present and re-verified, not re-implemented: `EVAL`, `EVALSHA`, `SCRIPT LOAD`,
`SCRIPT EXISTS`, `SCRIPT FLUSH` (all confirmed absent/present against the live binary before any
code was written, per the "verify before implementing" rule — only the five commands above were
actually missing from the registry).

New files: `src/cmd/functions.cc` (FUNCTION + FCALL, the library registry), `src/cmd/scripting.h`
(the seam), `tests/scriptsurf.py`. Extended: `src/cmd/scripting.cc`, `tests/differ.py` (suite
`script`), `src/cmd/command.h`, `src/cmd/commands.cc`, `src/core/io_loop.h`, `src/cmd/t_server.cc`
(INFO + CONFIG row), `src/core/config.h`, `tomokv.conf`, `Makefile`,
`tools/gen_acl_categories.py` + the regenerated `acl_categories_generated.h` (202 → 207 rows).

## Design

**One interpreter, two owners.** `scripting.cc` keeps the persistent per-thread `lua_State`, its
sandbox, the undo log, the eviction guard and the RESP converter. `functions.cc` owns the library
registry. They meet at `src/cmd/scripting.h`: `script_thread_state()`, `script_new_sandbox_state()`
and `script_execute(shard, op, ScriptInvocation)`. The Lua amalgamation is still compiled exactly
once (in `scripting.cc`); `functions.cc` includes only the vendored public headers and links
against it, so there is no second copy of Lua in the binary.

**Read-only gate.** `ScriptContext.readonly` is set from the entering handler variant and checked
at the `redis.call` dispatch site against `CommandSpec::flags & CmdFlags::Write`. That flag is
already exact in the registry (PERSIST/DEL/SETBIT are `Write`; TOUCH/EXISTS/TYPE are `Readonly`),
so no second whitelist exists to drift. `redis.pcall` surfaces the same refusal as a table error,
exactly as redis does. A function carrying `no-writes` runs read-only for **every** caller, not
only `FCALL_RO` — that is redis 7 semantics and is asserted in the battery.

`EVAL_RO`/`EVALSHA_RO`/`FCALL_RO` rows are `Readonly`, not `Write`: no AOF post-image pass and no
WATCH invalidation for an activation that cannot mutate. The atomic MVCC promotion still runs for
them, because it is keyed on the declared key range rather than on write-ness — which is what RYOW
inside a read-only script needs.

**Library registry.** Process-wide control-plane state, like ACL users: `std::map` under a
cold-path mutex plus an atomic generation. Executor threads never read that map on the hot side of
a call. Each thread materializes the libraries into its own Lua state once per generation; the
generation stamp lives in that state's own Lua registry (`tomo_fn_generation`), so a state that
`SCRIPT FLUSH` rebuilt automatically loses its stamp and re-materializes — no thread_local to keep
in step with the interpreter's lifetime.

At library-load time `redis` is swapped for a registration-only table whose `__index` raises
redis's exact `Script attempted to access nonexistent global variable 'call'`; the callbacks
resolve `redis` from `_G` when they later run, so they see the real `call`/`pcall`/`setresp`. Lua
5.1 only skips a leading `#` line in `luaL_loadfile`, so the shebang is blanked IN PLACE (same byte
count) before `luaL_loadbuffer` — that is what makes our `user_function:2:` line numbers agree with
redis's.

**Error shapes.** Aligning the RO rejection meant aligning the whole activation-error format, so
that happened too:
* chunk names are now `@user_script` / `@user_function`, so Lua's position prefix reads
  `user_script:1:` instead of `[string "user_script"]:1:`;
* `redis.call` raises a TABLE (`{err=...}`), not a string, so Lua adds no position prefix — which
  is why redis's wire error for a failed `redis.call` has none either;
* a `lua_pcall` message handler records the first Lua frame's line, and the reply is assembled as
  `<message> script: <sha-or-function>, on @<chunk>:<line>.`;
* `Error compiling script (new function): ` is the compile prefix.

Sample (identical bytes on both servers):

```
EVAL "return redis.call('incr',KEYS[1])" 1 strk
  -ERR value is not an integer or out of range script: 6f5ade10…81a5, on @user_script:1.
EVAL_RO "return redis.call('set',KEYS[1],'v')" 1 strk
  -ERR Write commands are not allowed from read-only scripts. script: 7b614947…5f0c, on @user_script:1.
FCALL nw 1 kk
  -ERR Write commands are not allowed from read-only scripts. script: nw, on @user_function:2.
```

The pre-existing `BUSY script exceeded the N instruction limit` reply is unchanged (it is a
TomoKV mechanism redis has no counterpart for) and still short-circuits before the tail.

## The ordering bug the differ found (and the fix)

The script store is a **process-wide side effect one command produces and another consumes**. It
used to be written on the executor. Executors are per-shard and run concurrently, so two commands
of one pipelined batch whose declared keys land on different owners execute in an order the ROB
only restores for their *replies*. The `script` differ suite caught the consequence at op 4038 of
4236: a LATER `EVAL X` armed an EARLIER `EVALSHA sha(X)` that should have answered `NOSCRIPT`.
Sequentially the same 4236 ops had 0 diffs — pipelining is what exposed it.

Two changes close it, in both directions:

1. **The store is maintained on the IO thread, in parse order** (`script_route_store()` in
   `command_prepare_script_route`). EVAL validates + stores; EVALSHA resolves and answers
   `NOSCRIPT` there. Routing is the only stage that sees a connection's commands in order. Cost:
   one throwaway `lua_State` per DISTINCT script, process-wide — the second EVAL of the same source
   finds its sha stored and skips compilation, exactly as `SCRIPT LOAD` has always behaved. Error
   precedence is preserved (numkeys errors still beat compile errors, verified against the oracle).
2. **`CmdFlags::OrderedLocal`** (new bit 18), set on `SCRIPT` and `FUNCTION`. Connection-local
   commands normally answer at parse time, which let a pipelined `EVAL x` + `SCRIPT EXISTS sha(x)`
   ask the store before the EVAL that fills it had run. Commands with this bit wait for the ROB to
   drain first — the same `if (rob.in_flight() != 0) break;` barrier the blocking lowering already
   uses. It is one predicted-false test **inside** the ConnLocal branch, which the GET/SET dispatch
   path never enters, so the hot path is untouched.

`MULTI` inherits both: EVAL inside MULTI routes through the same `command_prepare_scan_route`, so
`MULTI; EVALSHA sha(X); EVAL X; EXEC` returns `NOSCRIPT` then `OK`, matching redis.

## Knob

`script-instruction-limit N` (boot-only, default 100000, `0` = unlimited), CLI
`--script-instruction-limit`, exposed immutable via `CONFIG GET`, documented in `tomokv.conf`.

Deliberately **not** named after redis's `lua-time-limit` / `busy-reply-threshold`: that is a
wall-clock threshold for starting to answer BUSY *while a script keeps running*, a different
mechanism, and the knob-compat rule says adopt the reference name only when the semantics are the
reference's. This is a hard instruction bound. Evidence it fires:

```
--script-instruction-limit 5000 : EVAL "while true do end" 0  -> -BUSY script exceeded the 5000 instruction limit
--script-instruction-limit 0    : EVAL "…for i=1,300000…" 0   -> :300000   (default 100000 would have aborted it)
CONFIG SET script-instruction-limit 9                          -> -ERR parameter is immutable at runtime
```

## INFO counters (added to `# Stats`)

`number_of_cached_scripts`, `number_of_libraries`, `number_of_functions` (redis's names),
`script_flush_generation`, `script_interpreter_builds`, `script_chunk_cache_hits`,
`script_chunk_cache_misses`, `script_readonly_rejections`, `function_generation`,
`function_calls`, `function_thread_rebuilds`, `function_readonly_rejections`.

They exist so the battery can prove mechanisms FIRED rather than that nothing broke, and each is
asserted with a delta and, where meaningful, a negative control (e.g. FCALL over 8 keys must NOT
move `function_thread_rebuilds` while the generation is unchanged).

## Deliberate divergences from redis 7.4 (all probed, none silent)

1. **`SCRIPT DEBUG YES|SYNC` is refused.** Redis answers `+OK` and then expects an ldb session.
   We have no interactive debugger, so answering `+OK` would strand the client. `NO` → `+OK`;
   the bad-argument message matches redis exactly.
2. **`SCRIPT KILL` / `FUNCTION KILL` always report `NOTBUSY`.** Scripts run inside one
   shard-owner task under a hard instruction limit, so an activation is never observable from
   another connection: `NOTBUSY` is the only reachable answer, not a placeholder. Long-script
   interruption is not supported.
3. **`FUNCTION DUMP`/`RESTORE` use our own frame** (`TOMOFUN1` magic, u32 count, length-prefixed
   library sources, FNV-1a-64 trailer) — not redis's RDB function payload. Cross-server interchange
   with redis is out of scope by brief; our own round trip is asserted, including corrupted and
   truncated payloads. Rebuilding from source means a restored library is validated by exactly the
   path a `LOAD` takes.
4. **`SCRIPT HELP` / `FUNCTION HELP` carry our own text.** Copying redis's help block would be
   copying RSALv2 source.
5. **`FUNCTION LIST` order is deterministic** (libraries and functions sorted by name); redis's is
   hash order. Both are unordered by contract; the differ normalizes.
6. **Undeclared keys stay refused** (`Script attempted to access an undeclared key`). Redis 7 only
   warns. Routing happens before execution here, so a key the script never declared has no owner.
   Pre-existing, unchanged.
7. **Declared keys must share one owner** (`CROSSSLOT`). Pre-existing, unchanged.
8. **`FCALL <missing-fn> <bad-numkeys>`** reports the numkeys error; redis reports
   `Function not found` first. Function existence is executor state, numkeys is validated during
   routing on IO. Everything else in the FCALL error matrix is byte-exact, including
   `Bad number of keys provided` (FCALL's own wording, distinct from EVAL's).
9. **`error` is reachable in a library body**; redis's load-time sandbox removes it. Only changes
   the message of a pathological library that calls `error()` at load time.

## Scope cut (documented, not silent)

**Functions do not survive a restart.** Neither AOF rewrite nor snapshot carries libraries, so a
`FUNCTION LOAD` is lost on an unclean stop or a clean restart. Redis persists functions inside the
RDB. Cut because the snapshot/AOF format work belongs to the persistence lane and adding a new
top-level record type from here would collide with it. **Item for the persistence lane:** add a
`FUNCTION` section to the snapshot format and an AOF-rewrite preamble emitting one
`FUNCTION LOAD REPLACE <code>` per library; `FUNCTION DUMP`'s frame in `functions.cc` is already
exactly that list of sources and can be reused verbatim.

**Also observed, NOT fixed (pre-existing, outside this lane):** `ex_loop` routes every
`local_xshard` op to `xshard_aof_emit_local`, whose `classify()` returns false for EVAL/EVALSHA
(and now FCALL), so it returns early — the `CmdFlags::ScriptRoute` branch inside
`aof_record_local_op` looks unreachable for script ops. If that reading is right, writes performed
by a script are not recorded in the AOF. FCALL inherits exactly EVAL's behaviour here, so this lane
adds no new gap, but the persistence lane should confirm it.

## Test evidence

Build: `make -j12` clean, `make asan` clean (both after `make clean`).

```
$ python3 tests/lua_scripting.py 127.0.0.1 7240          # pre-existing battery, unchanged
LUA SCRIPTING: 0 FAIL

$ python3 tests/scriptsurf.py 127.0.0.1 7240             # no oracle
SCRIPTSURF: 92 checks, 0 FAIL (no oracle: error shapes not byte-compared)

$ python3 tests/scriptsurf.py 127.0.0.1 7240 127.0.0.1 7245   # + 53 byte-exact oracle shapes
SCRIPTSURF: 145 checks, 0 FAIL

$ python3 tests/differ.py 127.0.0.1 7240 127.0.0.1 7245 script
DIFFER script: 4236 ops, 0 diffs -> PASS          # seeds 7, 11, 29, 101 and RESP3 (-3): all PASS
```

Both atomic modes (`--atomic 0` boot and `--atomic 1` boot) run the full set:

```
--atomic 1: LUA SCRIPTING 0 FAIL | SCRIPTSURF 145 checks 0 FAIL | DIFFER script 4236 ops 0 diffs
```

Regression check on the suites that share the touched dispatch path:

```
string 4033 ops 0 diffs | hash 3545 | list 3521 | set 3524 | zset 3531 | xshard 4276 | bitmap 4262
```

ASAN+UBSAN (`build/tomokv-asan`, `ldd`-verified to link `libasan.so.8` / `libubsan.so.1`), both
atomic modes:

```
--atomic 0: LUA SCRIPTING 0 FAIL | SCRIPTSURF 145 checks 0 FAIL | DIFFER script 4236 ops 0 diffs
--atomic 1: LUA SCRIPTING 0 FAIL | SCRIPTSURF 145 checks 0 FAIL | DIFFER script 4236 ops 0 diffs
sanitizer reports in the server log: none, except the pre-existing one below
```

The single sanitizer report is PRE-EXISTING and in vendored third-party code, not in this lane's:

```
third_party/lua/lstring.c:87: runtime error: load of misaligned address … for type 'const uint32_t'
```

That is `murmur32()`'s 4-byte block read over an unaligned string, introduced with the vendored
Lua in `0275f5659 lua: add single-owner eval scripting`. `git diff HEAD -- third_party/` is empty
for this branch; any EVAL triggers it. Worth a separate fix (read the block with `memcpy`), left
alone here because touching the vendored tree is out of lane.

**Cost when the feature is off.** The only edit on a dispatch path is the `OrderedLocal` test, and
it lives INSIDE the `if (ConnLocal || ConfigRoute)` arm that the GET/SET path never enters — one
predicted-false flag test guarding a `break`. A/B of `build/src/main.o` compiled with and without
that hunk: `.text` is byte-identical at 333212 bytes. Nothing else this lane added is reachable
without an EVAL/FCALL/SCRIPT/FUNCTION command. No throughput bench was run — the wire rig is
reserved and the brief asks for none.

`tests/gate.sh` was NOT run (it owns port 7899 and cores 0-7, reserved for the mainline operator).
No wire/NIC work was done. All work used cores 40-43 (server, port 7240) and 44-47 (oracle,
port 7245), `make -j12`.

## Where the differ suite is deliberately narrow

`gen_script` declares 0 or 1 keys per activation and always names `KEYS[i]` in `redis.call`. Both
are properties of TomoKV, not gaps papered over: a script declaring two keys routes to ONE owner
here, so a cross-shard pair is a `CROSSSLOT` vanilla redis never produces; and the declared range
is enforced because routing precedes execution. Everything else in the suite — reply conversion,
the full error text including the `script: <sha>, on @user_script:<line>.` tail, the read-only
gate, `SCRIPT LOAD`/`EXISTS`/`FLUSH`, `FCALL`/`FCALL_RO`, `FUNCTION LIST`/`STATS` — is byte-
compared. `FUNCTION` replies pass through a `sort_nested` normalizer for the library/function
ordering noted above.
