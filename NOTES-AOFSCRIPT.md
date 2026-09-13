# AOF script post-image recovery

## What changed

Writes made by `EVAL`, `EVALSHA`, and `FCALL` now enter the AOF as one post-image group per script
activation. `xshard_aof_emit_local()` checks `CmdFlags::ScriptRoute` before its ordinary
cross-shard-command classifier, obtains the declared `KEYS` range, stages every resulting
post-image with `record_group_post_image()`, seals the one-shard fragment, and publishes one GCMT
decision. A multi-key script therefore has one recovery decision rather than independent per-key
records.

`EVAL_RO`, `EVALSHA_RO`, and `FCALL_RO` remain excluded by their `Readonly` flags. A zero-key write
form emits no group because the scripting bridge already refuses access outside the declared range,
so such an activation has no possible store effect.

No runtime knob, config grammar, command row, struct field, or Makefile input changed.

## Root cause

Script commands always use the same-owner `local_xshard` path. The executor consequently called
`xshard_aof_emit_local()`, whose first action was `classify(op, kind)`. The classifier has no script
kind, so it returned before the `ScriptRoute` key-range logic in `aof_record_local_op()` could ever
run. Nested `redis.call()` writes do not invoke the op-level persistence hook themselves.

The snapshot and atomic touched-key walkers already handled `ScriptRoute` first, which made this an
AOF-only asymmetry. The fix mirrors that ordering and uses the established MULTI/EXEC group format
so an incomplete increment tail cannot recover only a subset of one multi-key script effect.

## Commands covered

- Write forms: `EVAL`, `EVALSHA`, `FCALL`
- No-record negative controls: `EVAL_RO`, `EVALSHA_RO`, `FCALL_RO`
- Same-owner multi-key `EVAL`, selected at runtime through `DEBUG SHARD`
- Script-only incremental growth for the size-based automatic rewrite trigger

Cross-owner scripts remain refused with `CROSSSLOT` by the existing routing contract. The test
therefore selects distinct keys owned by one shard instead of weakening that contract.

## Before/after evidence

Baseline reproduction on HEAD used port 7050, `--appendonly yes --appendfsync always`, and a fresh
directory. Before the unclean stop both values were live, but the increment scan reported:

```
plain=1
eval=0
```

After restart, the plain value recovered and the EVAL-written key was absent;
`aof_replayed_records:1` and `aof_groups_committed:0` confirmed that no script record had been
written.

The updated `tests/aof.py` raw-file audit verifies five `GroupPut` records, nonzero group tickets,
matching GCMT records, and a shared ticket for the two-key EVAL. It also searches the increment for
three unique read-only keys and requires zero occurrences:

```
AOF SCRIPT BYTES PASS: write_keys=5 groups=4 readonly_absent=3
AOF SCRIPT RECOVERY PASS: values=5 readonly_absent=3
AOF BYTE-EXACT PASS: 52 static replies + live monotonic PTTL
```

The full unclean-restart matrix passed 4/4 cells with `--appendfsync always`:

```
persist-io=normal atomic=0: committed=4 skipped=0 PASS
persist-io=normal atomic=1: committed=4 skipped=0 PASS
persist-io=uring  atomic=0: committed=4 skipped=0 PASS
persist-io=uring  atomic=1: committed=4 skipped=0 PASS
```

The directed incomplete-group test now stops the writer after the one-shard script fragment and
before GCMT. It passed under both persistence engines:

```
AOF DIRECTED SCRIPT GROUP STOP FIRED: committed=10
AOF INCOMPLETE GROUP RECOVERY PASS: present=0/16 committed=10 skipped=16
AOF GCMT ORDER PASS: 10 commits, every dependency at lower byte offset
```

The automatic rewrite matrix uses 384 EVAL writes for its growth phase. Both persistence engines
ran the built-in atomic 0/1 matrix, for 4/4 passing cells:

```
AOF TRIGGER PASS: atomic=0 script_growth=384 requests=6 completions=3 auto=1 failures=3 backoff=1
AOF TRIGGER RECOVERY PASS: atomic=0 keys=1248
AOF TRIGGER PASS: atomic=1 script_growth=384 requests=6 completions=3 auto=1 failures=3 backoff=1
AOF TRIGGER RECOVERY PASS: atomic=1 keys=1248
AOF REWRITE TRIGGER MATRIX PASS: atomic=0/1 live-config info auto backoff restart
```

This proves script post-images increase `aof_current_size` enough to fire the configured
growth-based rewrite, rather than merely proving that an unrelated SET workload can do so.

## Build and sanitizer evidence

Commands run from this worktree:

```
make -j12 clean
make -j12
make asan
```

The release and ASAN/UBSAN builds completed without diagnostics. The ASAN battery passed 2/2
purpose-selected cells (`normal/atomic=0`, `uring/atomic=1`), including byte inspection, DEBUG
LOADAOF, unclean-restart recovery, and shutdown invariants. No `AddressSanitizer` or UBSAN runtime
diagnostic appeared.

`tests/gate.sh` was not run because the lane instructions reserve its port and cores for the
mainline operator. Its AOF section is wired to run `tests/aof.py` for both atomic settings inside
both persistence-engine passes. Shell/Python syntax checks passed. No differ suite was added: the
change affects persistence bytes and recovery, not command reply semantics, so Redis byte-reply
differing cannot exercise the mechanism.

## Cost and remaining edge

The GET/SET dispatch path is unchanged. The new branch is reached only inside the existing AOF-on
`local_xshard` post-handler hook; `appendonly no` still avoids the AOF manager path entirely. A
direct negative control ran EVAL successfully with AOF off and confirmed that no `appendonlydir`
was allocated. No indicative performance run was warranted because no hot path was changed.

Function-library definitions themselves still do not survive restart, as already documented in
`NOTES-SCRIPTSURF.md`. This lane persists the store post-images produced by `FCALL`; adding library
objects to snapshot/AOF metadata is a separate existing persistence feature and is not required to
recover the written keys.
