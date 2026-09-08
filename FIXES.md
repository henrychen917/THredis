# Storage fixes

Scope: production edits are confined to `src/store/`. Input is `SURVIVING.md`, with
its repeated entries counted once. The worktree starts at `c8e61f646`.

**10 findings fixed, 2 partly fixed, 1 left for the command lane. No findings disputed.**
The incomplete portions are listed separately below; they remain defects.

## Changes and regression controls

All cases live in `tests/store_regression.cc`. Each is a separate gate row. Cases
with distinct ordinary/read-local implementations run both, and assert that the
read-local store is armed. These are owner-level tests: no server, scheduling
delays, skips, benchmarks, or assumptions about which live server owns a port.

| Finding | Change | Regression state and failure induction |
| --- | --- | --- |
| **C storage 1 / C atomics 1**: admission evicts an unlinked candidate | An owner-local flag covers the installed, unpublished group prefix. Admission refuses further eviction during that prefix; publication clears the flag. The flag consumes existing `AtomicPendingState` padding. | `unlinked` installs a fresh candidate, proves it has no linked record, proves the victim sampler can select it, and admits a second value under pressure. It checks that the first candidate survives and that abort cleanup releases it. Remove the `group_installing` admission guard: the candidate-survival assertion fails. |
| **C storage 2**: RANDOMKEY bypasses snapshot/atomic visibility | The walker resolves pending versions using the current owner read context, includes parked predecessors of physical deletes, and uses snapshot-aware logical expiry for ordinary slots. | `randomkey` holds a 128 KiB object's serializer cursor across its TTL, asserts that cursor is active, and resumes serialization after RANDOMKEY. It separately tests a fresh undecided candidate and a visible predecessor with physical store size zero. Restore the previous walker: the frozen-object retention assertion fails; the other arms reject visibility violations. |
| **C storage 3**: atomic rehash capacity | Preflight advances eight old slots per incoming key, matching ordinary insertion, and includes all unmoved old entries in its capacity check. Both rehash movers install the destination before withdrawing the source; a failed insertion preserves the old slot and accounting. | `rehash` constructs the audited 1,024-slot geometry: 600 live entries at slots 424–1,023 and 100 tombstones. It proves a same-size rehash starts, performs twelve 100-key passes, requires admission without spurious OOM, and checks every old/new key after draining. Restore the old `additional / 8 + 1` step count in either preflight: the admission assertion fails. |
| **S storage 4 / S atomics 16**: rollback capacity | Non-overlap rollback counts potential physical restorations and preflights capacity before changing any predecessor, chain link, or gauge. Allocation failure leaves the entire prefix retained for later cleanup. Both owner paths use the helper. | `rollback` parks 700 predecessors, inserts 1,400 unrelated keys into capacity 2,048, and asserts this exact state. It fails the rollback table allocation, requires unchanged ownership/accounting, then permits allocation and checks all 2,100 keys. Remove either rollback preflight call: restoration aborts at exhausted capacity. |
| **S storage 5**: overwrite re-enables snapshot eviction | The shared `make_room_for` helper suppresses eviction throughout snapshot capture, including its overwrite callers. | `snapshot-eviction` freezes a victim, inserts a post-cut raw string, forces the shard over budget, proves the frozen victim is sampleable, and executes a same-class overwrite. Remove the helper's snapshot guard: the overwrite fails and the victim is lost. |
| **S storage 6**: mixed atomic/plain flags | `KvObjFlagByte` makes layout and metadata reads/writes use atomic byte accesses. Raw copying of the byte wrapper is prohibited. Foreign touches retain their single CAS with no retry. | `flags` executes 100,000 foreign touches alongside owner TTL/key/layout/size reads and metadata writes, with a start barrier and completed-touch assertion. Its gate row runs under TSan. Replace the wrapper conversion's atomic load with `return value_;`: TSan reports a data race and exits nonzero. Restoring the old plain-byte header also fails the structural compile check. |
| **S storage 7**: atomic eviction absent from AOF | Atomic admission calls the same independent `record_delete` path as ordinary eviction before erasing its victim. | `aof-eviction` forces admission to evict exactly one unrelated key, checks that the eviction counter fired, and checks exactly one independent AOF delete for that key. Remove this call: the AOF-observation assertion fails. |
| **S storage 8**: ordinary eviction ignores script intents | Both victim choosers use the existing intent-aware `atomic_needs_version` exclusion. | `intents` proves a key is sampleable, pins it with no version record, then requires ordinary eviction to refuse it. After unpin, eviction must succeed. Restore either chooser's `atomic_has_record` check: the protected-victim assertion fails. |
| **S storage 9**: imported hash deadlines unregistered | `atomic_install_group` registers type-specific attention for an incoming hash carrying field TTLs. | `imported-hash` installs a TTL-bearing hash on a destination whose field-expiry gate is explicitly zero, commits the group, asserts registration, advances the owner clock, and requires actual field removal. Remove `note_loaded_object` from installation: the registration assertion fails. |
| **S storage 11**, registration portion: expiry allocation failure | A failed field-index insertion latches an incomplete-index bit in FlatStore's formerly reserved byte. The existing lazy gate remains positive after later stale-index cleanup. FLUSH resets both. INFO's attention count is no longer exact after this allocation failure; it remains at least one until FLUSH. | `field-index-failure` fails the first index allocation, asserts that the failure was consumed and that no index entry exists, then checks that the lazy gate stays armed through later index cleanup. It also calls the real hash reaper on the missed field. Remove the incomplete-index latch: the gate assertion fails. The temporary-vector failure is **not fixed** here. |
| **S storage 12 / S cmdsem F12**, active portion: phantom hash bytes | Active reaping reports the object's shrink before either retaining or erasing the now-smaller object. | `hash-bytes` repeatedly installs a real one-field hash with a field TTL, proves its bytes were charged and its field was actively reaped, and requires exactly zero object bytes after each deletion. Move the size update back inside the nonempty branch: the zero-byte assertion fails. The lazy branch is **not fixed** here. |
| **S storage 13**, MISPRICED: deadline sidecar allocation failure | If TTL registration fails, the prototype build erases any stale sidecar entry, forcing deadline lookup to use the inline value. The ordinary build retains its existing fallback behavior. | `deadline-sidecar` requires `TOMO_TTL_DEADLINE_SIDECAR=1`, fills the index to its growth threshold, fails that allocation during an extension, and checks survival past the old deadline plus expiry at the new one. Remove stale-entry erasure: the survival assertion fails. |

The unlinked-prefix guard can refuse a pressured atomic pass even when an unrelated
victim might otherwise have been available. This conservatively protects unpublished
ownership without changing the command lane's install order. Rehash work remains
bounded by eight slots per incoming key, plus one step, and the remaining old table.

## Remaining command-lane defects

- **S storage 10 / S cmdsem F04 — UNFIXED.** `hash_ttl_on_access` in
  `src/cmd/t_hash_ttl.cc` still returns immediately during capture. Ordinary HGET
  therefore retains the reported stale-field problem; HINCRBY/HSETNX are also
  affected. A correct fix needs command-side logical filtering or a pre-image-safe
  reap path, including write semantics. Simply removing the snapshot guard would
  violate snapshot lifetime. No green regression row is claimed for this finding.
  A directed test should hold a hash snapshot record across its field deadline,
  require that record to remain active, and assert HGET absence plus HINCRBY/HSETNX
  behavior before releasing capture. It would still fail on this tree.
- **S storage 11 — PARTIAL.** The `reap_due` temporary-vector allocation failure in
  `src/cmd/t_hash_ttl.cc` still returns zero reaped. Fixing the store's registration
  gate cannot distinguish that return from “nothing expired.” The command lane
  needs allocation-free reaping or explicit failure/logical-expiry handling. Its
  regression should fail precisely the due-field vector allocation after proving
  the deadline has elapsed; ordinary HGET must not return the expired value.
  Unindexed deadlines can also remain pending for active cleanup until accessed;
  the store fix guarantees the lazy gate, not successful attention allocation.
- **S storage 12 / S cmdsem F12 — PARTIAL.** The empty-hash branch of
  `hash_ttl_on_access` still omits its size delta before `store_erase`. It needs the
  same accounting correction applied here to active reaping. A lazy-access-only
  regression should pause active expiry, expire the last field through access, and
  require `obj_bytes_` to return exactly to its previous value. The active row does
  not stand in for this missing lazy fix.

These omissions follow the requested `src/store/*` source boundary; they are not
disputes or claims that the findings cannot be tested.

## Validation and limits

- Release server build passed with `make -j2`; no existing layout assertion was
  edited. `KvObj` remains 8 bytes, `AtomicEntry` 144, `AtomicPendingState` 1,352,
  and `FlatStore` 944. The existing reader/owner offset locks remain intact.
- All eleven default-build cases and the sidecar-only case passed. All twelve
  cases also passed with AddressSanitizer and UBSan in a sidecar-enabled test build.
- TSan's initial process failed before `main` with `unexpected memory mapping`.
  Running the test with `setarch x86_64 -R` resolved this without changing system
  settings. The flags check passed, and a throwaway plain-load mutation produced
  a TSan data-race report (exit 66). The gate uses this process-local invocation.
- All twelve directed negative controls rejected the broken mechanism. Eleven
  failed at runtime; restoring the original flags header failed the structural
  compile check. The additional plain-load flags mutation failed under TSan.
- `bash -n tests/gate.sh` and whitespace checks on this lane's changes passed.
  No server, full gate, benchmark, or pattern kill was run. Live fused/split boot
  and protocol integration checks remain for the maintainer's gate run.

The test binary links the real hash representation and reap implementation.
Its AOF spy verifies the store-to-producer deletion obligation, not disk replay;
its incremental snapshot hook verifies cursor/object lifetime, not the native
snapshot file format. Scatter identity and notification dependencies are isolated
at their existing seams. Unused command dependencies abort if accidentally called.
No performance result is claimed for these correctness changes.

To run the rows individually, build `build/store-regression`,
`build/store-regression-sidecar`, and `build/store-regression-tsan`. Invoke
`./build/store-regression CASE` for the ordinary cases,
`./build/store-regression-sidecar deadline-sidecar` for the prototype, and
`setarch x86_64 -R ./build/store-regression-tsan flags` for the race check.

## Gate count

**Set EXPECT_QUICK to 337 and EXPECT_FULL to 354** (355 with the optional NIC row).
Do not add only to the full tier: the eleven-case loop emits its row at
`tests/gate.sh:341`, and the sidecar row is at `tests/gate.sh:349`; both are before
the quick-tier exit at line 1275. Build failures make their dependent rows red;
the builds emit no additional ledger rows. The flags case remains one row when
run under TSan.

The actual worktree's previous counts are 325/342, not the earlier context's
327/344. Counting the current row lines with their enclosing loop multiplicities
gives 337/354: exactly **+12 quick and +12 full**. `EXPECT_QUICK` and `EXPECT_FULL`
are deliberately unchanged in the diff.
