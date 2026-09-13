# Cross-owner SORT BY/GET notes

## 2026-08-28: static trace and design checkpoint

No part of this lane is already implemented.  The current multi-executor path still refuses every
`*`-bearing BY/GET in `parse_sort_options`, and `sort_run` still calls `sort_lookup`, which reaches a
second `Shard` directly.  That is legal only because admission currently proves one executor owns
all shards.

The existing seams are sufficient without changing the MVCC resolver or atomic record engine:

1. `xshard_prepare` already forces an actually-dereferencing SORT off localfast.  Its phase-one
   group contains only the source key (the STORE destination is deliberately omitted until hop 2).
2. The source owner's task serializes one source `ObjectImage`.  This is the point at which the
   natural-order element list becomes knowable.
3. A SORT-only state object will be carved into the scatter arena.  Its vectors own the decoded
   elements, concrete derived key/field requests, owner-grouped request order, and resolved values.
   No state is added to `Op` or `Client`; the only new `ScatterState` member is a pointer at its cold
   tail.
4. Completion of the source wave expands every `*`-bearing BY and non-`#` GET pattern for every
   source element.  Requests are bucketed by `router().shard_of(hash)` and posted as a second READ
   wave.  Every task reads only the `FlatStore` of the `Shard&` passed to it.
5. `xshard_execute` already installs `PinnedNowMs(state.now_cut_ms)` and a `ReadEpochGuard` from
   `state.snapshot`/`origin_conn_id` for every owner task.  The derived wave will run under those
   same two cuts as the source wave.  Dereferencing SORT must therefore pin a snapshot even when
   the command has STORE and even at atomic mode 0.
6. Once the last derived owner finishes, one executor calls the existing `sort_run` ordering core
   with a resolved-value matrix.  Missing keys, wrong types, and absent hash fields leave the
   corresponding presence bit clear.  `GET #` and globless patterns never create requests.  The
   existing result/presence vectors then feed either the array reply or `sort_encode_store`.
7. STORE continues through the existing phase-two `hop2[0]` plan and `apply_image_selected`, so the
   destination is written only by its ordinary owner task.

`sort_run` will remain the single implementation of ordering, LIMIT, DESC, NULL handling, and GET
row construction.  The direct shard lookup will be removed; the core will consume optional
parallel resolved BY/GET arrays instead.  Non-dereferencing forms pass no resolved matrix.

### Wave/state details

The ordinary `phase` byte remains 1 for both read waves and 2 for STORE apply.  A SORT-only wave enum
in the arena sidecar distinguishes source/gather/resolved.  This avoids changing the generic
two-hop mutation tests.  Before calling `finish_phase1`, derived groups are replaced with the
original explicit-key groups so generic status/error scans never index the dynamic request order.

Bare commands need a derived-wave publisher shaped like `publish_phase2`.  EXEC children do not
post new tasks: all shards are already transaction participants, so the last source owner rebuilds
the child groups, resets `pending`, and returns `PhaseAdvanced`; participant loops discover their
new group on the next child stage.  The same happens once more for STORE apply.

Task dependency/hazard enumeration must recognize the derived-wave key order instead of indexing
the explicit argv-key array.  This is routing glue only.  The resolver, version selection, commit
publication, and atomic apply machinery are unchanged.

### Snapshot cut inside EXEC

The parent transaction already pins one `SnapshotCut` when it contains a multi-owner read and stamps
read-only xshard children from it before dispatch.  Dereferencing SORT must opt into that same path
even with STORE: its read participant set is dynamic and may span owners although its initial source
group has size one.  The child must borrow the parent's registered cut; it must not register its
private scatter pool independently.

### Required validation geometry (not run in this lane)

This lane must never be validated with one executor.  The gate geometry is exactly:

```
--shards 16 --ratio 6:2 --enable-debug-command yes
```

The SORT battery will fail unless INFO reports 6 I/O and 2 executor threads and a wide `DEBUG SHARD`
candidate walk observes exactly shards 0 through 15. Because the default placement assigns shard
`sid` to executor slot `sid % 2`, it walks candidate pattern prefixes, queries `DEBUG SHARD` for the
source and every concrete derived key, and requires at least one derived key whose shard maps to the
other executor slot. It prints the source/derived shard pair and fails loudly if none is found.
STORE destinations are selected by the same geometry search rather than assumed from a key spelling.

Per lane rules, I have not built anything, run any test/load script, or started a server.

## 2026-08-28: implementation checkpoint

Implemented the planned owner-only waves:

- `sort_run` no longer includes or reaches `Server`, `Shard`, `FlatStore`, or `KvObj`. It consumes
  optional parallel resolved BY/GET arrays, so it remains the only ordering/LIMIT/projection core.
- A `SortDerefState` object is carved after the ordinary scatter arrays. The only new
  `ScatterState` member is its pointer at the cold tail. Concrete keys, fields, values, presence
  bits, and dynamic key order live in that arena-owned object's vectors.
- Source completion decodes the natural elements, expands concrete requests, and rebuilds the dense
  shard groups. Each gather task promotes/resolves only keys in its own `Shard&`, under the existing
  `ReadEpochGuard(state.snapshot, origin_conn_id)` and `PinnedNowMs(state.now_cut_ms)`.
- The last gather task reduces into `ResultHeap`, restores the explicit source groups before generic
  error/status scanning, and either finishes the reply or enters the unchanged STORE hop.
- Bare SORT has a derived-wave publisher. EXEC uses the already-declared all-shard participant
  barrier and advances the child stage twice (source to gather, then gather to STORE when present).
- Dynamic request keys were added to task hazard/dependency enumeration; no generic explicit-key
  index is used during the gather wave.
- Dereferencing SORT now pins a snapshot even for STORE and atomic mode 0. In EXEC it causes the
  parent transaction to register a cut and borrows that exact cut, including for STORE children.
  No resolver/version/publication code was changed.
- The multi-executor option refusals, `sort_deref_local`, and both denial constants were removed.

## 2026-08-28: validation and cleanup checkpoint

- `tests/sort.py` now rejects any geometry other than 16 observed shards at a 6:2 I/O:executor
  ratio. It buckets candidate names with `DEBUG SHARD`, derives the default owner slot from
  `shard % 2`, and fails loudly unless the source has concrete BY and GET keys plus a STORE
  destination on the other executor. The proven command asserts both its exact result and exact
  derived-lookup/reduction counter deltas.
- `tests/gate.sh` gives SORT its own two-executor boot in both atomic modes. The old one-executor
  negative-control target and refusal assertions were removed; the semantic, RYOW, MULTI/EXEC,
  STORE, and randomized phases now all run under the cross-owner geometry.
- The differential SORT leg independently enforces the same 16-shard, 6:2 geometry. It reconstructs
  source membership while walking its generated stream, expands actual generated BY/GET patterns,
  and fails unless both classes have a concrete key on the executor opposite their source.
- Operator documentation no longer describes BY/GET as placement-dependent. The original
  `NOTES-SORT.md` is explicitly marked historical.

Per lane rules these are static test definitions only: no build, server, test, or load process was
started in this lane.

## 2026-08-28: terminal-path audit

The dynamic gather temporarily reuses `ShardGroup::begin/count`, but those ranges index the SORT
sidecar's dynamic `key_order`, not `ScatterState::key_order` (which is sized only for the explicit
source/destination argv keys). Successful reduction restores the source groups before generic
completion. Static review found that an OOM or queue-full exit could reach `first_error` through
notification finalization before that restore and index the wrong order array. `first_error` now
checks gather `group.error` values but deliberately skips the fixed per-key status scan during that
wave; gather requests have no per-key `status` entries. This covers both bare and EXEC terminal
paths without hiding any gather error.

## 2026-08-28: final static audit

- A tracked-source symbol sweep finds no `sort_deref_local`, `kSortByDenied`,
  `kSortGetDenied`, or `TOMO_SORT_NO_READCTX`. ACL's distinct permission-denial messages remain.
- `src/cmd/t_sort.cc` contains no `Server`, `Shard`, `FlatStore`, `KvObj`, or store access. The only
  keyspace reads in the new wave are through the `Shard&` passed to `xshard_execute`, after routing
  the concrete key with `router().shard_of` and posting its shard to `worker_of_shard`.
- Source and gather both pass through the same `PinnedNowMs(state.now_cut_ms)` and
  `ReadEpochGuard(state.snapshot, state.origin_conn_id)`. Bare dereferencing SORT registers that
  cut even at atomic mode 0 and with STORE; an EXEC child borrows its parent's registered cut and
  transaction-wide time cut.
- STORE reduction restores the fixed source groups and enters the pre-existing phase-two plan;
  `apply_image_selected` still runs only in the destination shard task.
- No file under `src/store/`, including `atomic_mvcc.h` and `flatstore_atomic.inc`, changed. The
  `atomics_glue.inc` edits only teach task hazard/key enumeration about the sidecar's dynamic key
  order; resolver, version selection, publication, and apply machinery are unchanged.
- Neither `Op` nor `Client` was edited. Their existing 336/1984-byte static assertions remain; the
  sole `ScatterState` addition is the `SortDerefState*` at the cold tail, and its object is carved
  and destroyed with the scatter arena.
- `git diff --check` is clean across the complete lane diff.

Per the lane prohibition, this audit is static only. No compilation, make target, server, load
generator, or test script was run.

## 2026-08-28: round-2 residual is the oracle's collation locale

The 93 residual rows are not bad derived values. They are the already-documented `ALPHA` locale
axis, exposed by a logging truncation and by the differential gate failing to enforce its own boot
requirement.

The seed-7 generator establishes the state without any post-run inference:

- Ops 137 through 236 issue exactly one `SET so:a_<m> <word><m>` for every `m` from 0 through 99.
  Nothing later writes or deletes an `so:a_*` key. `rebuild()` mutates only the selected source and
  its `so:w_*`, `so:h_*`, and `so:d_*` companions.
- Choosing `BY so:a_*` sets `alpha = True`, and the generator unconditionally appends `ALPHA`.
  These values are deliberately non-numeric and never enter numeric score conversion.
- The ordinary diff reporter printed only `o[:4]`. Thus evidence such as
  `['SORT', 'so:z1', 'BY', 'so:a_*']` hid `ALPHA`, LIMIT, GET, and STORE options.

Ops 268 and 279 use `so:l0 = [19, 50, 83, 6, 9, 68]`. Their weights are respectively
`delta019`, `char-lie050`, `delta083`, `golf006`, `Bravo009`, and `Echo!068`. C/byte collation is
therefore `[9, 68, 50, 19, 83, 6]`, byte-for-byte the target order at op 279. The oracle returned
`[9, 50, 19, 83, 68, 6]`: `Echo!068` moved from the uppercase byte-order block to its locale word
position. Op 268 is the same two element orders projected through `GET so:h_*->g`.

Op 280 is not a one-element source disagreement. Its full command is:

```
SORT so:z1 BY so:a_* ALPHA LIMIT 2 2
```

At that point `so:z1` has exactly `[13, 95, 43]` in zset score order, with weights
`fox_trot013`, `HOTEL095`, and `delta043`. C/byte collation orders the members `[95, 43, 13]`, so
offset 2 returns the target's sole `13`. The oracle locale puts `HOTEL095` last, so the same LIMIT
returns its sole `95`. No source divergence or reply/argv mismatch is needed; the abbreviated argv
was the entire mystery.

The gather path also matches the former source-owner lookup mechanically: both expand the same
parsed prefix/element/suffix, hash with `FlatStore::hash_key`, route through `router().shard_of`,
accept only strings for the plain form, preserve integer text, and use the same read/time cut. Hash
field lookup likewise retains the old type, field-presence, and field-expiry checks. More directly,
the target's exact C ordering above could not be produced without gathering those exact distinct
`so:a_*` values.

The harness defect was in `tests/differ_gate.sh`: `gen_sort` and `NOTES-SORT.md` required
`LC_ALL=C`, but the shared pinned Redis boot inherited the caller's locale. The gate now starts the
oracle through `env LC_ALL=C`. The sort differ also runs a two-value mixed-case/punctuation
collation preflight on both sides and fails immediately with a precise boot diagnostic instead of
emitting a page of false gather diffs. Finally, SORT/SORT_RO diff diagnostics now print the full
argv rather than four entries.

No server code changed. Per the lane prohibition, this conclusion and the harness edits were
checked statically only: no build, server, test, or load script was run.

## 2026-08-28: round-3 MULTI RYOW phase-publication repair

The all-shard EXEC participation added for dereferencing SORT established the necessary local
program order, but the implementation did not safely publish the dynamic gather phase. A
barrier-only transaction fragment called `group_for()` against the live `ScatterState::groups`
table. The last source fragment concurrently reset that table, rebuilt it with the derived-key
groups, reset `pending`, and only then release-stored the new `MultiCommand::stage`.

That left no happens-before edge around the membership lookup. A fragment could see a new group
while it was only partly built and execute it as the old stage. Missing a derived request has exactly
the observed replies: a missing BY is a NULL/zero weight (one element moves), and a missing GET is a
null projection. Pre-existing values make the owner run promotion/resolution before the lookup,
widening the race; this accounts for the absent-key control being much harder to trigger without
changing the semantic diagnosis.

The repair gives each child stage an immutable 256-shard membership mask:

- Stage zero's mask is captured during EXEC preparation, before any owner task is posted.
- On `PhaseAdvanced`, the last old-stage participant first finishes rebuilding `groups`, captures
  the next mask, then release-stores the next stage number.
- A transaction fragment acquire-loads the stage and consults only that stage's mask. A nonmember
  waits without touching `groups`; a member may enter `xshard_execute`, where `group_for()` is now
  protected by the stage publication and the child `pending` join.

At the instant a gather fragment issues, its shard's transaction task has necessarily walked the
earlier commands in its per-shard list. Thus an earlier MSET candidate for the concrete derived key
is already installed, and `ReadEpochGuard(state.snapshot, state.origin_conn_id)` resolves the pinned
cut plus the transaction's own private overlay. The pinned cut is unchanged. The same mask rule also
closes the equivalent live-group race for generic two-hop children inside EXEC.

Per the lane prohibition this repair was reviewed statically only. It was not built, and no server,
test script, or load generator was run. Validation remains `tests/sort.py` at
`--shards 16 --ratio 6:2` in both atomic modes, with cross-owner geometry proven by `DEBUG SHARD` and
absence treated as a hard failure.

## 2026-08-28: integration onto t-merge14

Merged `t-merge14` at `953ca0774` into this lane. The integration commit is `dcb896835`; because it
has `t-merge14` as its second parent, `t-sortxshard` is now a descendant of mainline and can be
fast-forwarded into it.

### `multi_child` resolution

The integrated design keeps mainline's `ScatterState::multi_child` flag and child-local decision
words. `xshard_multi_child_bind` now takes one argument and sets `multi_child = true`; preparation
then captures the SORT lane's immutable stage-zero group mask. The flag remains load-bearing at all
four gates plus its declaration:

- `src/cmd/xshard_commands.inc:1535` prevents a child phase-two apply from drawing its own ticket.
- `src/cmd/scatter_engine.inc:1062` prevents a child from creating a second AOF scatter group.
- `src/cmd/scatter_engine.inc:2639` keeps a MULTI pop inside the parent's prepared-write path.
- `src/cmd/scatter_engine.inc:3336` prevents an atomic-write child from committing independently.
- `src/cmd/scatter_engine.inc:399` is the flag itself, set by the bind at line 3382.

The brief's assertion that `external_epoch` and `external_aborted` must be restored is superseded by
mainline commit `d1a4d3650` (`multilower: isolate xshard child runtime failures`). Restoring those
pointers with their old parent targets would silently revert that shipped fix: an ordinary runtime
error in one queued xshard command would set the parent's abort word and cancel otherwise successful
EXEC elements. They are therefore deliberately not restored.

Instead, records created by each xshard child point at that child's existing `epoch` and `aborted`
words. A normal child error aborts only that child. At the parent finalizer,
`publish_multi_child_epochs` release-stores the one shared ticket into every child epoch inside the
`Server::atomic_commit_group` callback, before the safe watermark publication. A genuine WATCH,
connection, preparation, or transaction-wide abort calls `abort_multi_children` before reservation
finalization. This preserves both requirements: successful children become visible at one EXEC
ticket, while per-command runtime errors retain Redis EXEC isolation.

The SORT lane's MULTI publication repair remains orthogonal to those decision words. Each child
stage has an immutable 256-shard mask. The last participant rebuilds `ScatterState::groups`, captures
the next mask, and only then release-stores `command.stage`; retrying fragments acquire-load the
stage and never consult a group table while it is being rewritten. This keeps the validated dynamic
SORT gather ordering without weakening mainline's commit, abort, or AOF ownership.

### Mainline preservation audit

The pre-merge feature side remained the evolved side of `scatter_engine.inc`; the merge applied
mainline's 52-line delta onto it. A static comparison from the old feature tip to the integrated tree
shows the same 37-path mainline surface as `c0d7f27fd..t-merge14`. The overlapping paths were read
individually:

- `scatter_engine.inc` retains the complete dynamic SORT gather and stage capture, plus mainline's
  `DebugBorrowCount` route and all child-local `multi_child` gates.
- `multi.inc` retains the stage masks and all-shard SORT participation, plus mainline's logical AOF
  post-image resolution, per-child runtime-error isolation, common-ticket epoch publication, and
  transaction-wide child-abort propagation.
- `xshard_commands.inc` retains SORT's gather/reduction changes plus mainline's `multi_child` commit
  gate and `DebugBorrowCount` reply.
- The P0 push fix is intact: `Client::append_oob` seals older fill bytes and uses owned segments for
  a non-quiescent ROB; `WbEngine::draining`, `defer_oob`, and each drain arm flush only after a reply
  boundary; pub/sub and climon use that path; `assemble_mget` still stages its header/borrow/tail
  segments inside the protected retire drain. The segmented/deferred mechanism counters and their
  measured INFO reporting remain present.
- Mainline's container routing, MULTI lowering tests/notes, borrow-registry instrumentation,
  info-reporting changes, P0 repro/battery, and `INFO everything` fix are unchanged on their
  non-overlapping paths. The gate conflict retains both `contarity`/`infofix` and cross-owner SORT;
  its ledger is 213 quick / 223 full, accounting for all six added both-mode rows relative to the
  207/217 common-base ledger.

Measurement surface for the integrated lane is SORT/SORT_RO (including STORE and derived BY/GET),
EXEC containing lowered xshard children, atomic/AOF commit of those children, and mainline's
out-of-band pub/sub/tracking/MONITOR interaction with borrowed replies including MGET. The owner
should retain the lane's requested SORT differential and 16-shard 6:2 MULTI race geometries, then
run the full gate so GET, SET, MGET and MSET remain covered.

Per the binding lane rule, this integration was verified by source/history comparison only. No
build, `make`, server, test script, load generator, or benchmark was run.
