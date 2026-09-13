# DESIGN-DEMOTEGATE — the demotion-plan write-accept gate (t-i1)

One change, `src/core/io_loop.h`, the ordinary point-write arm of the armed (read-local) parse
loop. Origin: t-cyclemap instruction lever I1 (`wt-cyclemap/CYCLEMAP.md` section 7.2).

## What

Every armed point write (SET, DEL, INCR, ... with a resolved single owner) called
`ReadLocalDemotionPlan::prepare(...)` to demote any still-pending local read that the write may
conflict with. For the common write -- no pending local read on the connection, or a pending read
whose key filter proves it disjoint -- `prepare` returns `true` at one of its first two early-outs
without building a plan: `loop_` stays null, so the later `active()` test is false and
`commit_reads()` is its `if (!loop_) return;` no-op. The call still cost a 12-argument out-of-line
frame plus four `std::abort` guards per op.

The gate hoists exactly those two early-out predicates to the call site -- both are single inline
loads `prepare` performs first (`read_local_pending_slots_ != 0`, and the pending-key bloom
`may_contain(hash)`) -- and calls `prepare` only when it would do work:

```
needs_demotion_plan = (reserve_owner_fenced_current && op->shard >= 0)
                   || (rob.has_pending_read_local()
                       && (!point_write_exact || rob.read_local_pending_may_touch(op->hash)));
```

## Why it is output-identical

For this call site `intersect_command` is null, `fallback_count` is 0 and `intersect_filter_miss`
is false, so `prepare`'s early-outs reduce to: return true iff `!reserve_current`
(`reserve_owner_fenced_current && shard >= 0`) and (`!has_pending_read_local()` or
(`require_hash_match` = `point_write_exact` and `!pending_may_touch(hash)`)). `needs_demotion_plan`
is the literal negation of that. When it is false the old code called `prepare`, got `true`, built
nothing, and set `read_local_commit_at_ordinary`; the new code sets the same flag without the call,
and every downstream consumer (`active()`, `commit_reads()`, `current_reserved()`) already treats
the never-touched plan as empty. When it is true the call is made with the identical arguments.
No store, no publication, no ordering changes; RYOW, in-order retire and single-owner writes are
untouched by construction.

## Measured (t-cyclemap, same source at e902c67d5; 1T = the instruction-slope instrument)

| cell | instr/op | cycles/op |
|---|---:|---:|
| armed SET p32, 1T | 3275 -> 3167 (-108, -3.3%) | 1076 -> 1053 (-23, -2.2%) |
| armed SET p32, 2T fused | user 2648 -> 2516 (-132) | 1252 -> 1212 (-40, -3.2%); store-queue-full -17% |
| delset, 1T | -62 | -17 |
| GET (control), 1T | -2 (null) | -- |

The saving exceeded the estimate because the removed code is argument marshalling; it retires at
~4.7 IPC, so cycles fall less than instructions. Battery on the prototype: RYOW, hazards, MULTI,
blocking, scripts, session_monotonic, bplus pass; the one failure (expwide S1) reproduced
identically on the unpatched base.

## Knobs, invariants, mode

No knob (hardcode-or-delete: hardcoded). 1s-only by construction (the armed parse arm); 2s is
unaffected. Read-local venue demotion is unchanged on every path where it does work.
