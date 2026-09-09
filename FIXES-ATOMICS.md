# Atomic, transaction, and scripting fixes

This worktree is based on `c8e61f646`, not the `78c3e5391` baseline in the supplied
background. `SURVIVING.md` contains the same list twice; each finding below is counted
once. The pre-existing `CODEX-OUT.md` changes and `SURVIVING.md` were not edited.

Fourteen findings are repaired. **C atomics 6 is only partially repaired:** the
reported UNPIN-to-APPLY interleaving is rejected, but final review found a second
lost-update interval after an owner's APPLY. Its failing reproduction and the
remaining scope boundaries are recorded below; this diff does not establish full
script/ordinary-writer atomicity.

## Changes and regression rows

Every named row below runs `tests/atomic_survivors_unit.cc` through the
`build/atomic-survivors-unit` target. It includes the actual owner implementation,
initializes 16 shards with 6 IO and 2 executor roles, and drives the owner phases
without starting workers or opening a listener. The interleavings and allocation
failures are explicit, bounded, and cannot become skips. The gate gives every
finding its own named row and a 30-second process timeout.

1. **C atomics 1 / C storage 1 — admission evicts an unlinked candidate.**
   `scatter_engine.inc` admits the complete owner MSET/MSETNX span before installing
   any candidate. The existing key anchors own materialized values on failure, and
   earlier admissions are uncharged before discarding the empty entry. Admission can
   no longer evict an object installed by this still-unlinked group.
   Row: **`admission`**. Two fresh 64 KiB values share a shard, with a third key on
   another owner. An allkeys-random quota admits the first image and rejects the
   second. The test requires the maxmemory error, a materialized first anchor, zero
   installed keys, and zero published entries. **Induce failure:** restore the
   admit/install loop that publishes only after both keys; the first candidate is
   installed and becomes an eviction victim before publication.

2. **C atomics 2 — retained registration closure.**
   `functions.cc` gives the registration closure a Lua-owned userdata cell, roots it
   across library execution, and revokes its contents immediately after the protected
   call. A callback retaining the closure receives a registration-context error.
   Row: **`closure`**. Load a library that saves `redis.register_function`, then invoke
   its registered callback through FCALL and require the explicit revoked-context
   error. **Induce failure:** retain the stack-local sink pointer as the closure's
   upvalue, or omit revocation; the callback no longer reliably returns that error.

3. **C atomics 3 — impossible AOF participant.**
   `multi.inc` enumerates ScriptRoute keys using the declared `numkeys` range. ARGV
   cannot enter transaction `write_keys` or create an AOF dependency without an owner
   fragment. Invalid declarations leave validation to the existing command parser.
   Row: **`script_keys`**. EVAL, EVALSHA, and FCALL carry a key and an argument on
   different owners; the test requires exactly the declared key index. A zero-key
   script must enumerate no write participant. **Induce failure:** remove the
   ScriptRoute branch and use the registry's open-ended key range. This row checks
   the dependency producer directly; it is not an AOF crash/recovery test.

4. **C atomics 4 — EXEC RENAME misses its own SET.**
   `scatter_engine.inc` binds the connection identity for a lowered transaction
   child even when its read cut is `UINT64_MAX`. Ordinary standalone latest reads
   retain their previous guard behavior.
   Row: **`rename_overlay`**. On proven different owners, execute `SET a new; RENAME
   a b` after seeding `a=old`; require both successful replies, missing `a`, and
   `b=new`. **Induce failure:** make the read guard bind only finite cuts; `b=old`.

5. **C atomics 5 — sequential INCRs use the old safe cut.**
   `atomics_glue.inc` prepares ordinary scalar and local multi-key writers from the
   newest committed version. `begin_plain_version` no longer converts the latest
   sentinel back into the safe read watermark. Snapshot reads keep their old cuts.
   Row: **`write_latest`**. Pin a counter and reserve an unrelated ticket, require
   that the safe watermark stays held after the first connection's INCR, then
   require a second connection's INCR to return 2. Also create a fresh key above the
   held cut and require local MSETNX to see it and return 0. **Induce failure:**
   restore either writer's `server.atomic_snapshot()` clone cut; the second INCR
   returns 1 or MSETNX incorrectly installs its value.

6. **C atomics 6 — UNPIN-to-APPLY lost update (PARTIAL).**
   `scatter_engine.inc` retains written-key intents through UNPIN and releases them
   in their owner's APPLY turn, including abort/error exits. Read-only intents still
   leave in UNPIN, before the group arrays are rebuilt. An AOF preparation failure
   still posts the aborting owners so these retained intents are released.
   `atomics_glue.inc` revalidates the script's read/write keys immediately before
   installation. A detected intervening write aborts the group with `TRYAGAIN`;
   ordinary writers continue to run, and a read never waits or retries.
   Row: **`script_apply`**. Drive PIN, READ, RUN, successful VALIDATE, and UNPIN;
   require the saved `BS` image, a retained write intent, and a released read-only
   intent. Complete foreign `APPEND x W` in that exact window, then require APPLY's
   conflict, released intent, and final `BW`. **Induce failure:** restore unconditional
   UNPIN, or remove the APPLY revalidation. The intent witness or the conflict/value
   checks fail. Group arrays are never rebuilt after installing records. This repairs
   the finding's exact pre-install interval; see the remaining post-install interval
   below before treating the finding as closed.

7. **S atomics 8 / C deadconf F01 — unprotected Lua conversion.**
   `scripting.cc` reads reply and error table fields with raw lookup, which cannot
   execute `__index`. The analogous library-error lookup is raw in `functions.cc`.
   Row: **`lua_conversion`**. Return and raise tables whose `__index` throws or loops
   forever. Require an empty array or ordinary error, followed by a successful
   activation. **Induce failure:** restore `lua_getfield` in reply or runtime-error
   conversion; the process panics or times out.

8. **S atomics 9 — child waits on parent WATCH reservation.**
   `scatter_engine.inc` retains the parent token on lowered children and uses it for
   WATCH readiness. `multi.inc` also uses the parent token when preparing a selected
   pop key. Child write reservations keep their own decision/lifetime bookkeeping.
   Row: **`watch_parent`**. Register WATCH, then run cross-owner RENAME inside EXEC;
   require completion, the source's removal, and the correct destination value.
   **Induce failure:** check readiness with the child's address again; the bounded
   owner-phase driver fails because the child waits for its own parent to commit.

9. **S atomics 10 — opposing WATCH reservations.**
   `multi.inc` allows read-only validation claims to coexist. A conflicting writer
   claim takes the existing conservative WATCH-abort path instead of waiting while
   retaining another owner's claim. Ordinary writers still respect live blocking
   reservations.
   Row: **`watch_cycle`**. Explicitly interleave A's first reservation on owner 0 with
   B's first reservation on owner 1. Require both reservations to block foreign
   writes, then complete both opposite read-only validations without dirtying either
   client. Publish both decisions and require all reservation refs to drain.
   **Induce failure:** return immediately when `watch_finalize_reservation` reports
   blocked; the second validation cannot complete.

10. **S atomics 11 — malformed transaction MSET.**
    `multi.inc` validates MSET/MSETNX pair parity before owner dispatch and produces
    the ordinary command-specific arity error as an EXEC array element. No key is
    prepared or installed for the invalid command.
    Row: **`mset_arity`**. Fresh clients queue `MSET a one b` and `MSETNX a one b`
    across owners. Require one error element and both keys absent. **Induce failure:**
    remove the pair check; the dedicated transaction writer consumes the trailing
    missing value and changes keys.

11. **S atomics 12 — successful retry watches nothing.**
    `multi.inc` rolls the session's watched-key vector back to its original size
    when WATCH preparation throws. Previously installed watches are preserved.
    Row: **`watch_oom`**. Sweep every throwing C++ allocation during preparation on
    fresh clients until success, requiring failures after session-vector allocation
    as well as earlier failures. Every failed attempt leaves no session key. Every
    retry must dispatch owner work, register the watcher, and become dirty on a
    foreign modification. **Induce failure:** remove the catch-side resize; the
    sweep finds the phantom key and fails before it can be deduplicated on retry.

12. **S atomics 13 / S deadconf F07 — Lua reply injection.**
    `scripting.cc` replaces CR and LF in `ok` and `err` payloads with spaces while
    preserving the rest of the payload and emitting one RESP line.
    Row: **`lua_lines`**. Require byte-exact single-frame status and error replies
    from values containing `\r\n:42`. **Induce failure:** append the original strings
    directly; the exact byte comparison detects the injected integer frame.

13. **S atomics 14 — unbounded FUNCTION LOAD body.**
    `functions.cc` runs validation and per-thread materialization through a bounded
    protected-call bridge in `scripting.cc`. The bridge installs the instruction
    hook, restores the previous context, and treats a caught timeout as failure.
    Row: **`library_limit`**. Load an infinite library body, then a library that
    catches that timeout and attempts registration. Both must return an instruction
    error and leave the global library count unchanged; a normal activation must
    still work. **Induce failure:** restore the unhooked library `lua_pcall`; the row
    times out. The configured unlimited setting remains unlimited.

14. **S atomics 15 (MISPRICED) — shared staging-failure flag.**
    `scatter_engine.inc` makes the shared flag `std::atomic<bool>`; this stays a small
    error-path fix and makes no claim about previously demonstrated bad commits.
    Row: **`stage_flag`**. A compile-time assertion locks the flag's synchronization
    contract, and a real owner READ above the derived 4 MiB cap must publish abort,
    the over-limit byte count, and the exact staging error. **Induce failure:** revert
    the shared flag to plain `bool`; the invariant fails compilation. Removing the
    flag publication or cap check instead fails the runtime assertions. This is not
    a claim that an unsanitized stress run can reliably detect a C++ data race.

15. **S deadconf F09 — catchable instruction-limit error.**
    `scripting.cc` checks the latched timeout independently of `lua_pcall`'s return
    status. An activation that catches the hook error cannot return success.
    Row: **`instruction_limit`**. Run the finding's `pcall` loop followed by `return
    42`, require BUSY, and require a subsequent normal activation to succeed.
    **Induce failure:** check only nonzero pcall status again; the activation returns 42.

## Remaining findings and scope boundaries

No finding is marked DISPUTED.

**C atomics 6 remains open for a post-APPLY lost update.** The additional directed
probe `post_apply_probe` exits 1 on this diff. It is deliberately excluded from the
fifteen green regression rows, rather than converted into an expected failure or a
skip. Run it with:

```sh
taskset -c 32-39 timeout --foreground 30 ./build/atomic-survivors-unit post_apply_probe
```

The probe requires two owners, successful validation, and an installed epoch-zero
script image on owner 0 while owner 1 still has not applied. Starting from `x=B`,
the script appends `S` to `x` and also writes a key on owner 1. Complete owner 0's
APPLY, then a foreign `APPEND x W`, then owner 1's APPLY and the normal successful
decision. Both APPEND replies are **2**, and the final value is **BW**. Legal serial
success would require **BSW** with replies 2/3, or **BWS** with replies 3/2. The
ordinary writer skips the undecided script version and installs a newer ticket;
the remaining owner's validation never revisits `x`.

Another validation wave only moves this interval. A complete repair needs one
commit-or-abort decision shared by the script and conflicting ordinary writers,
including the storage handoff after one owner's private installation. The current
epoch and abort words do not jointly arbitrate that decision. Adding a late abort
store can race successful publication, and making ordinary foreign writers wait
would violate the supplied ordering law. I did not introduce either workaround or
change the storage/executor contracts owned by other lanes. The narrower repair is
retained and tested, but **full repair is not claimed**.

The following shared findings are real and remain unfixed here because their
necessary changes belong to the other file lanes:

- **S atomics 7 / C concurrency 1, WATCH-disconnect null client:** the dereference
  is in `src/core/ex_loop.h` before the null-aware transaction handler. Changing the
  legal null cleanup producer would not repair the executor's client-lifetime
  contract. The executor lane must guard that maxmemory/no-touch access.
- **S atomics 16 / S storage 4, rollback capacity:** restoration capacity belongs
  in `src/store/flatstore_atomic.inc` / `flatstore.h`, including ordinary inserts
  while predecessors are parked. Reserving a larger initial scatter table cannot
  bound subsequent foreign inserts and would not make the defect impossible.
- **S cmdsem F08, no-op writes dirty WATCH:** the unconditional completion hook
  requires trustworthy mutation information from command handlers and executor
  completion. Inferring mutation from success/error replies in `multi.inc` would
  incorrectly suppress WATCH changes from commands with partial effects.

Other storage findings (atomic rehash capacity, eviction/AOF and eviction/intents,
and imported hash-field deadlines) were left to the storage lane. No tests or fixes
for those findings are claimed by this diff.

## Layouts, gate count, and validation

No existing layout assertion changed. The extra parent token is in the unlocked
scatter sidecar. Op, Client, ThreadCtx, Shard, FlatStore, Rob, AtomicEntry and Config
remain governed by their existing build assertions. This checkout's Config assertion
is **528**, not the background's 624; this diff does not change it.

The expected counts for **this checkout** should become **341 quick / 358 full**
(full without the optional NIC row), from **325 / 342**. The added build row is at
`tests/gate.sh:326`; the fifteen-case loop emits its row at line 334. Both are above
the quick-tier exit at line 1261, so the delta is **+16 in each tier**, with no
full-only additions. A static count of success-row sites, multiplied by their
enclosing loops, independently gives 341 / 358. The EXPECT constants were not
edited. When applying only this lane's delta to the background's later 327 / 344
baseline, the corresponding totals would instead be 343 / 360.

Validation completed:

- Release build and the unit target succeeded after the final code edit, with no
  compiler warnings. Existing layout assertions passed.
- All **15/15** named regressions passed again after that build on CPUs 32–39,
  eight CPUs selected from the allowed 32–43 set, at 16-shard / 6:2 geometry.
- **14 behavioral negative controls rejected broken code.** Nine owner/transaction
  rows failed against this checkout's original `multi.inc`, `scatter_engine.inc`,
  and `atomics_glue.inc`. Five Lua rows failed against the original scripting and
  function objects too: four returned failure, and the unbounded library body hit
  its 30-second process timeout. Only the staging flag was kept atomic in the old
  scatter copy so its independent type assertion would allow those other tests to
  compile. These throwaway sources and binaries are under `build/atomic-pre` and
  are absent from the patch. Reverting the staging flag itself is rejected by the
  compile-time assertion; no TSan result is claimed.
- Existing individual batteries passed in six fresh, isolated server boots:
  `multi_exec.py`, `scriptsurf.py` (92 checks), and `xscript.py` in split mode at
  atomic 0 and 1; `multi_exec.py` and `scriptsurf.py` in fused mode at atomic 0 and
  1, and again with fused read-local armed at atomic 0 and 1. This is **14 battery
  invocations**, at 16 shards and eight CPUs (split ratio 6:2). All six servers
  exited cleanly, and all six `shutdown_report.py ... clean` checks passed. Only
  the exact PIDs started for these batteries were stopped.
- After the final script cleanup-scope adjustment, `xscript.py` passed again in
  split mode with atomic 0 and 1, and both shutdown-report checks passed. Including
  these two reruns, **16 individual battery invocations and eight clean shutdowns**
  passed.
- The additional `post_apply_probe` failed as documented above; it is an unresolved
  correctness result, not part of the passing total.
- `bash -n tests/gate.sh` and whitespace checks over the edited code passed.
- Neither the full gate nor the quick gate was run. No benchmarks were run.

Build, unit, live-battery, shutdown, and negative-control logs are retained under
`build/`: `final-rebuild.log`, `atomic-unit-final-results.log`,
`atomic-live-results.log`, `atomic-xscript-final-results.log`, `atomics-live-*`,
`atomics-final-*`, `atomic-post-apply-unfixed.log`, and `atomic-pre`.
