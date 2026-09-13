# Lane t-contarity: XGROUP/XINFO container arity and routing

## Problem and invariants

Redis 7.4.2 advertises both `XGROUP` and `XINFO` with arity `-2`: the bare container is invalid,
but every two-argument form must pass the outer arity gate. TomoKV advertises the same metadata
while enforcing minimums of four and three respectively, which prevents `HELP` and also makes the
unauthenticated minimum-arity probe disagree with the advertised row.

Changing only the minimum is invalid. Both rows truthfully declare their keyed arms at argv[2],
and the ordinary route hashes `first_key` before dispatch. A two-argument `HELP` has no argv[2].
The registry validator therefore correctly rejects a lowered row unless it also uses an existing
special-routing class.

The ordering at `src/core/io_loop.h` is intentionally unchanged: arity checks run before ACL
authentication. Consequently bare `XGROUP` remains an outer arity error. A known child with a bad
generated arity is also rejected there, before ACL/MULTI, but an unknown `XGROUP x` passes the
advertised `-2` parent bound and reaches the `NOAUTH` gate. Unknown-arm handling and shard
selection happen later on authenticated traffic.

## Construction

Use the existing `CursorShard | SubcmdRoute` route taken by `OBJECT` and `MEMORY`. Generalize it
from command-name branches to the generated subcommand metadata:

1. Resolve `argv[0]|argv[1]` through `cmdmeta_generated.inc`.
2. Validate the resolved child's positive/exact or negative/minimum arity from that row.
3. If its generated `first_key` is positive, hash that argument and route to its current shard
   owner. If it has no key (HELP and the other keyless OBJECT/MEMORY arms), pin the ordinary task
   to shard 0.

This makes the route data-driven for all four containers. It also removes the OBJECT/MEMORY
choice from the outer-arity error hook; the hook uses the same metadata resolver. A generated
variadic child that crosses a finite top-level maximum remains a handler-grammar syntax error,
which preserves `MEMORY USAGE`'s existing upper-bound behavior.

The arity gate asks the same resolver one narrower question before authentication: if argv[1]
names a generated child and that child's arity is wrong, it emits the pipe-qualified arity error.
An unknown child is not rejected by this check, preserving `XGROUP x` -> `NOAUTH` ordering. This
also prevents a malformed known child from entering a MULTI queue merely because the parent now
accepts two arguments.

`XGROUP` remains `Write`; only its HELP child is keyless. CREATE, SETID, DESTROY,
CREATECONSUMER, and DELCONSUMER all carry generated `first_key = 2`, so none can fall onto the
shard-0 pin. `XINFO` follows the same split: HELP is keyless and STREAM/GROUPS/CONSUMERS route by
argv[2]. The registry continues to publish `-2` through the existing generated command metadata.

The ordinary snapshot write gate historically captured `op.key()`, which is argv[1]. For the now
keyless XGROUP HELP arm that is not a key, and for every keyed XGROUP arm the actual key remains
argv[2]. Snapshot preparation therefore applies the container's declared key position when it is
present and skips capture for a keyless child. Transaction snapshot preparation already walks the
metadata-derived key argument list and needs no corresponding change.

The stream handlers keep their option-grammar checks, but their coarse private arity tables are
replaced by the shared generated-data validation. Exact Redis 7.4.2 HELP arrays are emitted before
any key access.

## Validation geometry

No build, server, or test is run in this lane. The main session must validate with at least two
executor threads; the standard gate boot `--shards 16 --ratio 6:2 --enable-debug-command yes`
satisfies that requirement.

The directed battery must not infer ownership from key spelling or shard number. It reads
`DEBUG LBSIGNALS` for the live shard-to-executor map, uses `DEBUG SHARD <candidate>` for every
candidate key, selects a key whose executor differs from shard 0's executor, and aborts if no such
key can be found. Every keyed XGROUP arm is then exercised on that key. This catches the dangerous
failure mode in which all SubcmdRoute arms are pinned to shard 0 along with HELP.

Required checks:

- exact `XGROUP HELP` and `XINFO HELP` arrays;
- bare `XGROUP`, `XGROUP BOGUS`, and representative known-arm arity errors;
- CREATE, SETID, CREATECONSUMER, DELCONSUMER, and DESTROY on the proven cross-owner key;
- `XINFO STREAM <missing-key>` returning `ERR no such key`;
- `COMMAND INFO xgroup` and `COMMAND INFO xinfo` retaining arity `-2`;
- the unauthenticated registry-minimum probe continuing to reach `NOAUTH` for `XGROUP x`.
