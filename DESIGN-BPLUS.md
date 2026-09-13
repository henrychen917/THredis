# B+: atomic-safe IO-side reads

Status: design only; no engine changes in this lane.  Base: `d459626e9`.  Branch:
`t-bplus`.  The owner-approved direction is called **B+**.  The feature formerly called
“hot-forward” on `t-hotfwd` is called **read-forward** throughout this document.

B+ replaces a whole-mode or whole-shard refusal with a per-key fail-closed test.  It does **not**
teach an IO thread to resolve MVCC history.  A key whose raw representation may disagree with the
MVCC resolver goes to its owner; a multi-key local read is accepted only across a stable shard
publication generation.  These are acceleration gates, not new visibility rules.

## 1. Ground truth from code

### 1.1 Terms that must not be conflated

There are three different notions of “visible” in this code:

1. **Physically resident** means the FlatStore table slot points at an object (or tombstone).  An
   atomic candidate becomes physically resident before the group commits.
2. **Logically visible** means `FlatStore::find()` plus the atomic resolver selects the value for a
   particular read cut and originating connection.  Foreign readers do not see an epoch-zero
   candidate even when it is the physical pointer.
3. **Safe for a foreign direct read** means the physical pointer by itself is the same answer the
   resolver would return for “latest committed,” and its lifetime is protected by read-local QSBR
   or by a copied read-forward slot.  Commit alone does not always establish this condition.

Likewise, current “group open” indicators describe different intervals.  `ScatterState::epoch`
starts at zero, explicitly meaning its installs are private, while `record_refs` lets records retain
the state after the reply (`src/cmd/scatter_engine.inc:365-376`).  `apply_open` is intended to cover
admission through installation on all owners (`src/cmd/scatter_engine.inc:408-423`) and is opened
after state allocation (`src/cmd/scatter_engine.inc:1668-1689`).  On the ordinary grouped path the
last owner closes it before reply retirement (`src/cmd/scatter_engine.inc:3384-3409`).  The script
path returns through its own completion routine before that generic close, so its defensive close is
deferred to `xshard_destroy()`, normally at retirement
(`src/cmd/scatter_engine.inc:1848-1859`, `src/cmd/scatter_engine.inc:3668-3699`).  Connection
admission, `apply_open`, a prepared shard entry, a linked epoch-zero entry, and reply lifetime
therefore do not start or end together.

For B+, **open for key K on shard S** has one operational meaning: the shard owner has committed to
possibly changing K for a logical group, but the group decision is not yet safely consumable by a
foreign direct reader.  Its required interval begins before the first possible physical exchange.
The broader term **foreign-unsafe K** continues through abort restoration or committed collapse when
the raw table is not yet a latest-committed representation.  The filter in section 2 publishes the
broader interval.  Every open key is foreign-unsafe; a decided key may remain foreign-unsafe.

### 1.2 Exact grouped-entry writer lifecycle

The table describes the normal cross-owner grouped `AtomicEntry` path.  Variants that reserve or
publish tickets at different sites are called out below; the table is not a claim that every command
carrying `atomic_group` follows one function sequence.

| Stage | Raw FlatStore table | MVCC/logical state | Current read-local publication | Code |
|---|---|---|---|---|
| Admit and allocate | Unchanged | Shared group epoch is zero; `apply_open` becomes true | Unchanged | `src/cmd/scatter_engine.inc:1641-1689` |
| Prepare one shard | Unchanged | One `AtomicEntry` receives pointers to the group epoch/ref/abort words and the shard-local key span | With read-local armed, the conservative pending bit is release-published before the prepared handle can be returned | `src/store/flatstore_atomic.inc:268-291`, `src/store/flatstore_atomic.inc:633-653` |
| Install each key | Candidate pointer/tombstone replaces the old pointer immediately; the displaced pointer is saved in `parked[]` and `count` names the installed prefix | Shared epoch is still zero | The pending bit was already visible; each raw exchange is also inside the read-local mutation sequence | `src/store/flatstore_atomic.inc:306-315`, `src/store/flatstore_atomic.inc:1114-1153`, `src/cmd/scatter_engine.inc:2519-2577` |
| Publish shard entry | Raw candidate is already installed | The one shard entry is linked only after the complete local install pass | Pending remains set | `src/store/flatstore_atomic.inc:342-346`, `src/cmd/scatter_engine.inc:2578-2586` |
| Join owners | Every participating owner has finished its shard groups before the final owner proceeds | Still epoch zero until the decision | Pending remains set | `src/cmd/scatter_engine.inc:1003-1017`, `src/cmd/scatter_engine.inc:3384-3409` |
| Commit | No table-slot change is required | One ticket is release-stored to the shared epoch; the safe read watermark advances only after all reserved tickets have published | Pending remains set | `src/cmd/scatter_engine.inc:3464-3493`, `src/core/server.h:2400-2455` |
| Cleanup/collapse | Normal monotone history frees parked predecessors; abort or inverted ticket order may restore/rewrite the physical winner | Decided history older than the active floor/cutoff is removed | Pending clears only at final entry free, after detached objects have been submitted to QSBR retirement (not after their later reclamation) | `src/store/flatstore_atomic.inc:1219-1223`, `src/store/flatstore_atomic.inc:1240-1257`, `src/store/flatstore_atomic.inc:1307-1334`, `src/store/flatstore_atomic.inc:1375-1427`, `src/store/flatstore_atomic.inc:1622-1714`, `src/store/flatstore_atomic.inc:1938-2003` |

The write therefore reaches **live raw memory during install, while the group epoch is still zero**.
The executor preflights capacity, prepares the entry, then calls `atomic_install_group()` for each
key; only after those calls does it finish accounting and link the entry
(`src/cmd/scatter_engine.inc:2455-2586`).  `atomic_install_group()` performs the physical exchange
before recording the displaced object (`src/store/flatstore_atomic.inc:306-315`).  The prepare-before-
exchange ordering is already load-bearing for read-local: its comment says no caller can exchange a
slot until an acquiring reader can see the pending publication
(`src/store/flatstore_atomic.inc:633-653`).

Cross-owner commit is after install.  Each owner decrements the shared completion count only after
its work is complete; only the last owner makes the decision (`src/cmd/scatter_engine.inc:3384-3409`).
For the direct atomic-write path, that owner calls `atomic_commit_group(state.epoch)`
(`src/cmd/scatter_engine.inc:3464-3493`).  The server brackets ticket reservation and publication:
it reserves a ticket, optionally executes `DEBUG ATOMIC-COMMIT-DELAY`, release-stores the ticket into
the epoch, publishes composite member epochs, and then advances the safe watermark
(`src/core/server.h:2418-2455`).  `atomic_snapshot()` reads the safe watermark, not merely the drawn
ticket (`src/core/server.h:2466-2468`).

There are important qualifications to “ticket after install.”  A same-owner localfast plain version
draws and assigns a committed inline epoch before installing its per-key record; this is currently
safe because no other **owner task** can enter that shard until the handler completes
(`src/cmd/atomics_glue.inc:1003-1009`, `src/store/flatstore_atomic.inc:497-507`).  FLUSH likewise
prepares pseudo-entries, draws one ticket, and then installs all tombstones
(`src/store/flatstore_atomic.inc:575-622`).  Cross-owner scripts reserve a ticket before validation
and apply, then release-store it only after successful apply
(`src/cmd/scatter_engine.inc:3279-3285`, `src/cmd/scatter_engine.inc:3341-3365`).  The narrower
invariant above is: **publication of a shared cross-owner group epoch follows that group's successful
installs**.  There is no universal ticket/physical-install order on which B+ may rely.  Foreign local
reads are not protected by owner-task serialization, so every ticket-first and entryless mutation
path must publish the filter and store/slot sequence before its first physical change.

Other command families have distinct decision sites.  Phase-two atomic-apply commits in
`finish_phase2()` (`src/cmd/xshard_commands.inc:1479-1504`); atomic pop can commit in its phase-two
finalizer (`src/cmd/scatter_engine.inc:3488-3508`); LMPOP/ZMPOP mutate directly and create a plain
version only when the key already needs MVCC
(`src/cmd/scatter_engine.inc:2765-2799`); scripts have the pre-reserved path above; and EXEC publishes
its parent/child epochs together (`src/cmd/multi.inc:1850-1880`).  The implementation audit must
route all of these through the B+ publication hooks even when no grouped entry is allocated.

### 1.3 Single-key visibility and the meaning of a pending record

`atomic_epoch()` acquire-loads the shared group epoch, while plain pseudo-entries carry their epoch
inline (`src/store/atomic_mvcc.h:137-140`).  The existing same-connection conflict test calls an
entry undecided when that epoch is zero and the abort word is false
(`src/store/flatstore_atomic.inc:190-209`).  For one foreign-read key, the resolver:

- starts from the physical candidate and the chain of parked predecessors;
- excludes aborted candidates;
- excludes a foreign group candidate whose epoch is zero;
- excludes a committed candidate newer than the read's snapshot; and
- permits the originating connection's own committed/private overlay.

Those decisions and ranking are in `src/store/flatstore_atomic.inc:915-1018`.  Consequently, with one
record for K:

- before commit, a foreign logical read returns K's parked predecessor although the raw table
  already points at the new candidate;
- after epoch `T` is published, a latest read or cut `>= T` returns the candidate, while a cut `< T`
  still returns the predecessor; and
- the originating connection may see its own private candidate for read-your-writes.

All shard entries for a group point at the same atomic epoch word
(`src/store/atomic_mvcc.h:23-29`, `src/cmd/scatter_engine.inc:2465-2468`), making one release store
the cross-shard logical switch.  EXEC uses
the same rule: per-key transaction entries point at one transaction epoch
(`src/store/flatstore_atomic.inc:465-492`), and its finalizer publishes one ticket into the parent and
successful child epochs (`src/cmd/multi.inc:1850-1880`).

EXEC's physical sequence is prepare/install first, common decision second.  `prepare_write_key()`
prepares and immediately installs each copied-key transaction entry
(`src/cmd/multi.inc:768-827`); the store exchanges and links that entry at
`src/store/flatstore_atomic.inc:516-528`.  `lists_pending` then joins every participating shard before
the parent `atomic_commit_group()` publishes the same ticket into successful children
(`src/cmd/multi.inc:1850-1884`).

Abort is not commit.  A failed group sets `aborted` and returns without publishing an epoch; its
installed candidates may remain physically present and epoch-zero until cleanup restores or splices
the predecessors (`src/cmd/scatter_engine.inc:3464-3483`,
`src/store/flatstore_atomic.inc:1375-1427`).  Cleanup is owner-batched and asynchronous relative to
reply retirement (`src/core/ex_loop.h:909-939`).

The phrase **pending record** is also easy to misread:

- `AtomicPendingState::live` is an owner-local count of linked `AtomicEntry` nodes, not keys and not
  open groups (`src/store/atomic_mvcc.h:69-95`).
- Linking increments `live`; cleanup decrements it when an entry is removed
  (`src/store/flatstore_atomic.inc:878-897`, `src/store/flatstore_atomic.inc:1307-1334`).
- One grouped entry can cover a whole shard-local key span (`begin`, `count`, and `capacity` in
  `src/store/atomic_mvcc.h:29-40`); an EXEC copied-key entry has capacity one
  (`src/store/flatstore_atomic.inc:465-492`).
- `atomic_pending_entries()` returns exactly `live` (`src/store/flatstore_atomic.inc:36-42`), and INFO
  sums it across shards (`src/cmd/t_server.cc:1630-1652`).  It includes undecided, committed-but-not-
  cleaned, aborted-but-not-cleaned, and plain pseudo-entries.  It excludes a prepared-but-unlinked
  handle and script intents.  It is non-atomic and cannot be read as a foreign-thread gate.
- Exact owner-side membership, `atomic_key_pending()`, uses the 64-bit membership mask only as a
  prefilter and then compares every occurrence's full hash and key
  (`src/store/flatstore_atomic.inc:855-875`).

Current read-local deliberately publishes something stronger than `live`.  `ReadLocalStoreState`
has an atomic publication word and an owner-only `pending_count`
(`src/store/flatstore.h:488-500`).  Bit 1 means **any prepared atomic entry**, and bits 2..63 are the
mutation generation (`src/store/flatstore.h:544-550`).  Group, plain, and transaction prepare all set
the pending bit before returning (`src/store/flatstore_atomic.inc:633-653`,
`src/store/flatstore_atomic.inc:692-726`); the entry comment says it stays set through install,
linking, and collapse (`src/store/atomic_mvcc.h:37-40`).  Its sole clear is final read-local entry free,
after displaced objects have been submitted to deferred retirement
(`src/store/flatstore_atomic.inc:812-829`).  Thus there is a real interval with read-local pending = 1
and `atomic_pending_entries()` = 0.

Foreign probes acquire the publication word before loading topology/object memory and require the
same state afterward (`src/store/flatstore.h:632-679`).  Today both the parser and executor interpret
the one pending bit as a whole-shard refusal (`src/core/io_loop.h:4239-4271`,
`src/core/ex_loop.h:555-618`).  B+ refines that refusal; it does not weaken the mutation sequence,
QSBR, or resolver.

### 1.4 The constraint derived from cleanup

Removing K from a filter at the epoch store is **incorrect**.  Two groups can install overlapping
keys in order A then B but obtain commit tickets in order B then A.  In that case the later physical
pointer can be the older logical value.  The cleanup code explicitly detects ticket inversions and
exact-key overlap, then computes the committed argmax and may rewrite the physical slot
(`src/store/flatstore_atomic.inc:1259-1305`, `src/store/flatstore_atomic.inc:1830-1835`,
`src/store/flatstore_atomic.inc:1938-1960`).  Abort has the same raw/logical mismatch until restore.

Therefore B+'s non-negotiable invariant is:

> The shard owner publishes K as foreign-unsafe before any raw exchange, and K remains published
> until every open group touching it is decided **and** the representation a foreign reader would
> consume is a safe latest-committed representation.  Logical commit advances the multi-key
> publication history, but never by itself authorizes a raw-table read.

The conservative implementation is to retain one filter reference per prepared entry occurrence and
drop it at the same final-free point as today's whole-shard pending marker.  An individual key must
not close merely because its raw pointer has been canonicalized while retained history can still
resolve a cut below the copied/latest value.  Closing earlier would need a separate cut-coordination
or slot-epoch proof; it is outside this B+ baseline.

### 1.5 Ground truth of the `t-hotfwd` slot

The branch slot is a latest-live copied cache, not an MVCC version.  Its shared header contains only
sequence, hash, lengths, and expiry—no epoch or read cut
(`t-hotfwd:src/core/hot_forward.h:201-205`).  A reader makes one non-spinning equal-even sequence
attempt (`t-hotfwd:src/core/hot_forward.h:66-100`).

For an ordinary exact-key write, the branch intentionally leaves the old independent copy readable
while the owner handler runs, then attempts to publish the final resident object after the handler
(`t-hotfwd:src/core/ex_loop.h:1048-1058`, `t-hotfwd:src/core/ex_loop.h:1126-1129`,
`t-hotfwd:src/core/ex_loop.h:1201-1223`).  Under maxmemory, global atomic tracking, or a shard with
records the branch policy leaves it invalid instead (`t-hotfwd:src/core/ex_loop.h:1209-1215`).  When
that publish is allowed, a GET overlapping the write can linearize before or after it.  Broad,
transaction, scatter, and script paths invalidate rather than expose an intermediate copy
(`t-hotfwd:src/core/ex_loop.h:976-980`, `t-hotfwd:src/core/ex_loop.h:1067-1075`,
`t-hotfwd:src/cmd/scripting.cc:1061-1068`).  The branch design explicitly keeps atomic physical
changes invalid until a later sampled GET after pending state drains
(`t-hotfwd:DESIGN.md:215-222`).  B+ preserves this latest-committed-only invariant and replaces the
global atomic refusal with exact-key safety publication; it does not add a pre-image slot.

## 2. Per-key pending filter

### 2.1 Required contract

The filter is boot-allocated per physical shard and readable by IO/fused foreign threads.  It answers
only: “might this key currently be unsafe for a latest raw/slot read?”  A positive answer falls back;
a negative answer is permission to continue with the existing store/slot sequence protocol, not
permission to skip it.

Its contract is asymmetric:

- false positives are allowed and cost an owner fallback;
- false negatives are forbidden;
- the shard owner is the sole writer of filter cells, including across a FLIP ownership transfer;
- readers never follow pointers into owner state;
- add is release-published before slot invalidation or raw exchange;
- removal is release-published only after abort restoration or committed canonicalization and all
  relevant slot publication/invalidation; and
- overlap is reference-counted, so one group's close cannot expose a key still covered by another.

A script intent alone does not change the raw table or set today's read-local pending bit.  It makes
a later competing write require a version (`src/store/flatstore_atomic.inc:56-71`), while PIN and
UNPIN occur at `src/cmd/scatter_engine.inc:2211-2232` and
`src/cmd/scatter_engine.inc:2329-2347`.  B+ therefore does **not** filter the entire intent lifetime:
the competing writer publishes its own key before exchange, and the script publishes its declared
write keys when APPLY is about to mutate.  Filtering from PIN would be safe but would lengthen
fallback residence without protecting any physical change.  If an apply or broad path cannot
enumerate its exact write keys before mutation, it poisons the shard filter and falls back all keys
for that interval; it never silently omits them.

### 2.2 Representation options

Let `k` be key occurrences prepared by one group on this shard and `U` the currently published
distinct-key population.  The concrete sizes below make the trade-off comparable rather than
leaving “small” unspecified.

| Representation | Owner work per group lifecycle | Reader loads per key check | Memory per shard | Failure/false-positive shape |
|---|---:|---:|---:|---|
| Exact small set: two immutable 64-entry snapshots, `{hash,len,key[256]}` | Rebuild/copy `O(U+k)` on add and close; one release pointer/sequence publication each | Two sequence loads, `O(log U)` hash descriptors, then exact bytes | about 34 KiB plus control | Exact for <=64 keys of <=256 B; cap/long-key overflow must refuse the whole shard until drain |
| **Chosen: 4096 direct-mapped counting fingerprint cells** | `k` owner-local atomic load + release-store updates on prepare and `k` on final close, plus two owner-local span-total ops; first/last broad poison and rare saturated zero-drain rebuild are `O(4096)` | **One acquire cell load**; existing object/slot sequence loads are separate | **32 KiB + 24 B owner bookkeeping** | Exact unsafe keys always hit; unrelated keys can hit a wildcard bucket or 32-bit tag collision; poison makes the shard 100% positive; saturation stays fail-closed until all refs drain |
| Two-bank Bloom-with-generation, 8192 bits/bank, four hashes | Four bit publications per key plus bank/ref bookkeeping; close cannot delete bits | At least eight data-word loads plus generation control | about 2 KiB | False positives follow cumulative inserts, not current `U`; a long-lived group can block rotation and saturate both banks |

The exact set has attractive behavior at tiny `U`, but an exact variable-length key snapshot either
allocates/reclaims memory on the group path or imposes a hard key cap.  Its overflow mode is much
broader than a fingerprint bucket, and readers pay a population-dependent search.  The Bloom design
is compact, but safe deletion requires generations; continuously overlapping groups can prevent a
bank from becoming reusable, turning history rather than concurrency into the false-positive driver.

The counting fingerprint array is selected because it has fixed allocation, one bounded reader load,
owner-only constant-time updates, and deletion tied directly to the existing entry lifetime.  Its
32 KiB cost is 8 MiB for 256 shards and is allocated only when an IO-side local-read customer is
boot-armed.  Allocation failure disables/fails that requested acceleration at boot; writes never fail
later because optional filter metadata could not grow.

### 2.3 Cell layout and algorithms

Each cacheline-aligned array contains 4096 `std::atomic<uint64_t>` cells.  A cell packs a 31-bit
reference count, a 32-bit fingerprint derived from the already-computed 64-bit key hash, and one
`wildcard` bit.  Zero means empty.  Owner-only 64-bit `unsafe_total_refs`, `saturated_cells`, and
`shard_poison_refs` scalars provide exact drain/nesting witnesses even when a cell count can no
longer be decremented.  Readers still load only the selected cell.

The shard owner performs the following operations; no CAS is needed because ownership supplies the
single writer:

**AddSpan(keys), before the first possible exchange**

1. Check `unsafe_total_refs + key_count` for overflow and add the span count once.  No physical
   mutation may begin until every following cell publication is complete.
2. For each occurrence, index by independent mixed low bits and derive the nonzero 32-bit
   fingerprint from the remaining mixed bits.
3. Empty cell -> `{count=1, fingerprint, wildcard=0}`.
4. Same fingerprint -> increment `count`.
5. Different fingerprint or an existing wildcard -> increment `count` and retain/set `wildcard=1`.
6. Release-store each result.  If the count would first saturate, publish
   wildcard+maximum-count, increment `saturated_cells` once, and leave that bucket fail-closed.

**Might-contain(hash), on a foreign reader**

1. Acquire-load the one indexed cell.
2. Return false only for zero, or for a non-wildcard cell with a different fingerprint.
3. Return true for a matching fingerprint, wildcard, or saturated cell.

**CloseSpan(keys), only when every occurrence in this prepared span is safe**

1. For each occurrence, if the cell is saturated, leave its packed value unchanged because the lost
   high count cannot be reconstructed.
2. Otherwise decrement the packed count and store zero at count zero; retain its
   fingerprint/wildcard while nonzero.  A wildcard is intentionally sticky until the bucket drains
   because the cell does not remember which colliding tag remains.
3. Release-store after raw canonicalization/abort restore and read-forward invalidation/publication,
   then subtract this span's count once from `unsafe_total_refs`.
4. If `unsafe_total_refs` reaches zero, `shard_poison_refs` is zero, and `saturated_cells` is nonzero,
   release-clear all 4096 cells
   and reset the saturation count.  No unsafe key exists during this rare rebuild; new preparation
   cannot interleave because the owner is the sole writer.

Reference underflow is an invariant failure.  A theoretical 64-bit total overflow publishes a
permanent shard poison until restart rather than wrapping to a false empty state.

**Shard poison for an unenumerable write set.**  On `shard_poison_refs` 0->1, before mutation, walk
all 4096 cells and add one release-published wildcard contribution to each; nested opens only
increment the owner scalar.  On the final close, after every raw/slot representation is safe, walk
all nonsaturated cells and remove that contribution, clearing zeros and retaining wildcard on
nonempty cells; saturated cells stay positive until the exact zero-drain rebuild above.  This is the
explicit `O(4096)` owner cost and 100% fallback interval behind the word “poison.”  Sequential add
publication is safe because no broad physical mutation starts until the full walk completes;
sequential close is safe because raw state is already safe before the walk begins.  The final poison
close invokes the same full rebuild immediately if `unsafe_total_refs==0`, so a saturation cannot be
stranded merely because no later CloseSpan exists.

Different full keys with the same fingerprint merge harmlessly: both see a positive.  Different
fingerprints in one bucket make every key in the bucket positive until it drains.  Duplicate argv
occurrences may either be normalized once or added/closed symmetrically; they cannot be added once
and closed twice.

The filter check sits *inside* the existing sequence proof.  For FlatStore, a reader captures an even
read-local publication state, sees a negative filter cell, loads/copies the object, then validates the
same state.  For a read-forward slot, it captures an even slot sequence, verifies identity, sees a
negative filter cell, copies atomic words, then rechecks the same even sequence.  If add races a
reader that already saw empty, the following raw exchange or slot invalidation changes the enclosing
sequence; if no exchange has happened, the read can linearize before group open.  This is why one
filter load is sufficient without making the filter itself a second seqlock.

### 2.4 Expected fingerprint cost

For a freshly occupied direct-mapped table with uniformly distributed unsafe keys, an unrelated key
gets a wildcard bucket when at least two of the `U` keys occupy its bucket.  With
`lambda = U / 4096`, the instantaneous approximation is

```
p_fp ~= 1 - exp(-lambda) * (1 + lambda) + 2^-32
```

| Concurrent unsafe distinct keys `U` on a shard | Approximate unrelated-key false positive |
|---:|---:|
| 8 | 0.00019% |
| 64 | 0.0121% |
| 256 | 0.187% |

Sticky wildcards make these optimistic under adversarial long-running collision churn; INFO should
expose current wildcard and saturated bucket counts so this is measurable.  A stronger mixer makes
attacker-chosen bucket concentration no easier than attacking the server's keyed hash.  None of
these probabilities enter the correctness argument: an actual unsafe key is represented by a live
count or a fail-closed bucket.

## 3. Commit-generation double-check for multi-key local reads

> **Status (t-bplus-cellcheck):** the per-shard generation sweep described here was built, measured,
> and found too coarse; section 7 records the audit and the per-cell touch-epoch check that
> replaces it.  Section 3 remains as the design record of what was tried and why.

### 3.1 Counter semantics: all read-visible shard publications, not group commits only

The heading says “commit generation,” but the safe unit is broader: **a stable generation means no
read-visible mutation occurred on that shard**, not merely that no cross-owner group committed.
Group-only was considered and rejected.

Today a same-physical-shard MGET is one owner task and reads every argv entry without yielding
(`src/cmd/scatter_engine.inc:1547-1572`, `src/cmd/xshard_commands.inc:1778-1800`).  Once MGET reads
foreign to that owner, a plain `SET k new` can run between the two copies of `MGET k k`.  A counter
advanced only by atomic groups would accept `[old,new]`, changing an observable one-command result
even though no MVCC group exists.  Expiry, eviction, FLUSH, and same-owner multi-key writes create the
same class of problem.  B+ must detect them too.

The implementation should reuse the stable generation already embedded in the read-local
publication word rather than add a second writer RMW.  Bit 0 is the open mutation bit and bits 2..63
are advanced when the outer mutation closes (`src/store/flatstore.h:544-550`,
`src/store/flatstore.h:2780-2813`).  Ordinary insert/erase paths already execute inside that bracket
(`src/store/flatstore.h:2376-2420`), as do atomic physical exchanges
(`src/store/flatstore_atomic.inc:1114-1160`).  Nested store helpers share the outer bracket through
`mutation_depth`, so this is a publication sequence rather than an unbounded RMW per helper.

B+ names the stable high portion `local_publication_generation` and applies these semantics:

- every ordinary write, TTL rewrite/expiry, eviction, clear, and same-owner multi-key write that can
  change a local read answer advances it through the existing outer mutation bracket;
- every physical install for an atomic group advances it before commit, while the per-key filter
  prevents a reader from consuming the private candidate;
- abort restoration and any committed canonicalization advance it through their physical exchange;
- rehash/topology changes advance it conservatively for memory safety, even when the logical answer
  is unchanged.  Cleanup that only retires hidden parked objects need not advance it; cleanup that
  rewrites the raw winner does.  Conservative advances can retry but cannot change an answer; and
- a genuinely answer-changing path found by audit to bypass the bracket must add the bracket or an
  explicit owner publication.  It may not be exempted because it is “not an atomic group.”

No extra common-write atomic is required for the first implementation: B+ reads the sequence the
armed read-local store already maintains.  It is deliberately **not** the server-global MVCC ticket
and does not define a stronger database isolation level.  Its only job is to prove that a foreign
copy interval did not cross a mutation on a participating physical shard.

The existing 62-bit sequence is effectively non-wrapping, but the correctness rule should be formal:
before wrap, publish a permanent fail-closed generation state and send local multi-key reads to the
owner until restart.  Never wrap to an equal value and call it stable.

Why an exact commit-time bump is not needed in addition to the physical sequence: an affected key is
filtered from before its raw install until the raw representation is safe.  If an MGET read that key
before the group opened, the install changed its shard generation.  If it reaches the key while the
group/record remains unsafe, the filter forces fallback.  If it reaches the key after filter removal,
the representation is already safe.  An inverted or aborted history that rewrites the raw winner
also changes the sequence.  The filter bridges the logical-commit interval during which an epoch
store itself changes no table slot.

### 3.2 MGET local protocol

The complete reply remains private until validation.  The local path must not publish a partial RESP
array or transfer a zero-copy borrow before the final checks.

For one MGET attempt:

1. Route every key exactly as the scatter path does and build a sorted unique list of physical shard
   IDs.  Capture one command-wide realtime/TTL cut, matching the existing one-cut fan-out discipline
   (`src/cmd/scatter_engine.inc:1700-1701`, `src/cmd/scatter_engine.inc:2372-2382`).
2. Acquire-load an even read-local publication word from **every distinct shard before reading any
   value**.  Save its high generation.  An open mutation retries the whole attempt.
3. For each argv key in order, run the ordinary foreign probe: capture the shard's even state, check
   the per-key pending filter, find the exact object, copy a supported String value into MGET-owned
   storage, and revalidate the same shard state.  Never retain a FlatStore/KvObj pointer after that
   validation.  In the first B+ scope, missing, elapsed, unsupported/type, allocation, observer, or
   policy cases downgrade the whole command to preserve existing side effects and accounting.
4. Acquire-load the publication words from **every distinct shard again, after every value copy**.
   Accept only if every shard is non-mutating and every high generation equals step 2.  A filter hit
   or per-key validation failure is not patched element by element; discard the entire staged reply.
5. On a generation/sequence churn failure, retry the complete command once (two attempts total).  A
   second failure, or any non-transient filter/policy failure, falls back immediately to the existing
   owner/scatter MGET with its already pinned read cut.  The fallback is the semantic authority.

One full retry bounds foreign work to `2 * key_count` copies and avoids livelock under a hot writer.
The current single-key probe's three small retries (`src/core/ex_loop.h:555-618`) remain appropriate
for GET; copying an arbitrary MGET three times is not.

All initial counter loads precede all value loads, and all final counter loads follow them.  If an
attempt succeeds, the per-shard stable intervals have a common intersection: no participating store
mutation occurred between the end of the initial sweep and the start of the final sweep.  Every
copied value therefore belonged to one simultaneously valid raw world.  If a group begins but has
not installed anything before that interval ends, the command can linearize before it.  If it
installs, commits, abort-restores, expires, or is overlapped by an ordinary write inside the interval,
at least one generation changes or an affected-key filter check is positive.  That attempt cannot
publish.

This proof is intentionally stronger on the successful fast path than current cross-owner handling:
it detects any participant-shard mutation.  After the bounded fallback, semantics return to exactly
today's pinned-cut owner resolver; B+ makes no new promise about a sequence of independent plain
writes on different shards.

### 3.3 Why single-key GET and a latest-committed slot need no generation sweep

**Per-command atomicity.**  A one-key reply has no second element with which to tear.  For read-local,
the existing object/topology sequence rejects a physical mutation during the copy
(`src/store/flatstore.h:648-679`), and the per-key filter rejects an unsafe MVCC representation.  For
read-forward, the slot's two equal even sequence reads reject invalidation/publication churn
(`t-hotfwd:src/core/hot_forward.h:66-100`), and the filter rejects a touched open/unsafe key.  The
slot is a copied latest-committed value, never a historical pre-image.  A per-shard generation before
and after that same one key would duplicate the enclosing sequence proof.

**Session monotonicity for current read-local GET.**  IO stamps one nondecreasing read cut per parse
pass (`src/core/io_loop.h:3514-3523`, `src/core/io_loop.h:3944-3959`).  A connection may have only one
outstanding local batch (`src/core/io_loop.h:3490-3500`).  The executor stages an entire contiguous
GET prefix: if any member needs the owner, it discards all local copies and dispatches the whole
prefix in order; otherwise it publishes all members together
(`src/core/ex_loop.h:620-699`).  Thus a newer local answer is never followed inside that prefix by an
owner answer at its older pass cut.  The next prefix is parsed only after the prior one retires and
therefore samples a cut no older than that completed latest read.

**Session monotonicity for local MGET.**  MGET must be a separate local-command admission class, not
part of the GET-prefix flag.  Read-only scatter is explicitly non-barriered today
(`src/core/io_loop.h:4075-4079`), so B+ creates a one-command local-read fence: after admitting a
local MGET, stop that connection's parse pass until the command has validated or been downgraded and
dispatched.  Reuse the existing unretired-read-local exclusion mechanism where possible
(`src/core/io_loop.h:3490-3500`).  No younger command is stamped while its latest local result is
unresolved.  On success, the next parse pass samples a fresh safe watermark; on fallback, the
original pinned cut governs the whole MGET.  This also avoids the current ordering trap: prefix
extension is decided before scatter classification and assumes a GET-only prefix
(`src/core/io_loop.h:3665-3677`), while MGET is presently a `MultiShard` xshard-only command
(`src/cmd/t_string.cc:1402-1403`).

**Session monotonicity for read-forward needs one cut-ordering rule, not a generation
double-check.**  The old branch samples one pass cut and then attempts the slot after routing
(`t-hotfwd:src/core/io_loop.h:2224-2234`, `t-hotfwd:src/core/io_loop.h:2864-2905`).  At atomic mode 1,
simply removing the global atomic gate would permit this bad sequence:

```
parse cut S -> slot publishes group T>S -> GET returns T -> younger owner fallback resolves at S
```

The key filter proves the slot itself is latest-committed; it does not rewrite the already sampled
cut.  B+ therefore makes the inclusive parse-pass `pass_read_cut` mutable and tracks
`forwarded_since_cut`.  Stable slot hits set that boolean but do not each load a global word.  Just
before the first younger **read** leaves the forwarding prefix, IO loads `atomic_snapshot()`, raises
`pass_read_cut` to the maximum, and clears the boolean.  Today the Op receives a provisional stamp
before routing (`src/core/io_loop.h:3944-3959`), so after an ineligible/failed slot attempt B+ must
overwrite that Op with `Op::set_read_cut(pass_read_cut)` before local/owner dispatch or
`xshard_prepare`; alternatively the implementation may defer the initial stamp until the outcome is
known.  Intervening writes close forwarding but intentionally remain unstamped and leave the refresh
obligation for the next read.  If the pass ends with only hits/writes, the next pass naturally
samples a fresh cut.  (The cleanup `atomic_read_floor` is the exclusive successor of such cuts;
these are not the same value.)  `Op::set_read_cut` supports overwrite, and scatter copies the
effective cut during preparation (`src/exec/op.h:163-166`,
`src/cmd/scatter_engine.inc:1730-1745`).  A
group-derived slot may be made even only after local final cleanup admitted its epoch under a safe
cleanup cutoff, so the boundary snapshot is at least that slot's logical publication
(`src/store/flatstore_atomic.inc:1622-1637`, `src/core/server.h:2466-2468`).  More generally, the
boundary snapshot covers every atomic group safely published by then; a still-unpublished group may
remain excluded and legally linearize after the hit.  A tracked/overlapping plain write remains
filtered and odd through its record cleanup.

Existing ROB order remains required: the first forwarded hit needs a quiescent ROB, and any
unretired write or non-forwarded predecessor closes the forwarding prefix
(`t-hotfwd:src/core/io_loop.h:2891-2902`).  Together, those rules prove that (a) forwarding cannot run
ahead of this connection's own write, (b) each returned slot is latest committed at its successful
validation, and (c) no younger owner-resolved command can use a cut older than an earlier slot hit.
Thus **per-command visibility** of a single-key/slot read needs only the key filter plus its existing
sequence; **session monotonicity** is supplied by established order plus the lazy boundary cut
refresh.  No slot epoch and no multi-shard generation sweep are needed.

## 4. Integration

### 4.1 Shared publication hooks

Factor a small per-shard `ForeignReadSafety` sidecar used by either armed customer.  It owns the
fingerprint cells and stable publication accessors; read-local additionally owns QSBR/topology state,
and read-forward owns its copied slot table.  Allocate the safety sidecar at boot when
`read-local || read-forward` is true.  It must never be allocated or reached from a disabled store
hot path.

For an ordinary grouped entry, reuse/rename the existing `read_local_pending_published` lifetime bit
instead of growing `AtomicEntry`; its current size and pool geometry are explicitly locked
(`src/store/atomic_mvcc.h:37-54`).  Preparation walks the original `begin..begin+capacity` key span
and adds one filter reference per occurrence before returning.  Close that same prepared capacity,
not installed `count`: a partial abort can install only a prefix while every key was published
before the first exchange (`src/store/flatstore_atomic.inc:309-314`,
`src/cmd/scatter_engine.inc:2578-2586`).

The renamed lifetime bit is governed by `ForeignReadSafety` being armed, independently of
`read_local_enabled_`.  The close hook must be reached by both the baseline cleanup flow that ends
in `atomic_free_entry()` (read-forward-only) and the flow that ends in
`atomic_free_entry_read_local()`; it cannot remain hidden in today's read-local-specific free path.
For grouped entries it still executes before the state-pin decrement described next, not from inside
the final pool/free call.

Grouped hashes remain in `ScatterState`; `atomic_entry_hash()` dereferences `entry.group`
(`src/store/flatstore_atomic.inc:855-863`).  Current cleanup can decrement the last owner/group state
pin before pooling the entry (`src/store/flatstore_atomic.inc:1699-1704`,
`src/store/flatstore_atomic.inc:1984-1993`).  B+ must instead run `CloseSpan` and clear the lifetime
bit **while that pin is still held**, then decrement owner/group refs, then free/pool the entry.
Walking hashes from final-free after the ref decrement would be a use-after-free.  A prepared entry
with no install can close immediately on discard because its executing state is still live; a
partially installed abort cannot close until restoration.

Plain preparation needs an explicit API correction.  `atomic_prepare_plain()` currently receives no
hash; `plain_hash` and membership are filled only by `atomic_install_plain()`, after preparation
(`src/store/flatstore_atomic.inc:450-470`, `src/store/flatstore_atomic.inc:497-507`).  Change prepare
to accept/store the hash (including every FLUSH pseudo-entry) so it can publish the correct cell
before returning and can close symmetrically on pre-install discard.  Do not reconstruct identity
from mutable request storage at final free.

`atomic_prepare_transaction(key, hash, ...)` already receives and copies both identity components;
map it to a one-key AddSpan/CloseSpan around EXEC's install and retain its state pin through close
(`src/store/flatstore_atomic.inc:470-492`, `src/cmd/multi.inc:768-827`).

Script intent PIN alone does not add a filter reference; script APPLY adds its known write keys
before mutation.  Same-shard/localfast atomic multi-key commands and entryless paths such as direct
atomic-pop mutations use a scoped owner-only key list: enumerate and add before the first handler
mutation, perform all changes, and close only after the raw/slot state is safe.  Every phase-two,
EXEC, FLUSH, script, pop, expiry, eviction, and broad-clear path must be audited; an unenumerable
write set uses the shard poison rather than bypassing publication.

The owner ordering invariant for a touched key is:

```
filter add (release)
    -> read-forward target invalid/odd (release, if it matches)
    -> [ticket allocation/publication is path-specific]
    -> FlatStore mutation-sequence open and physical install(s)
    -> [shared cross-owner group decision follows all installs]
    -> any physical cleanup/canonicalization or abort-restore rewrite, under mutation sequence
    -> publish latest read-forward copy or leave it unavailable
    -> filter close (release)
```

The brackets are intentional: cross-owner shared-epoch groups decide after install, while localfast
plain versions and FLUSH can publish a ticket first.  Correctness comes from the filter/slot and
mutation-sequence envelope, not from assuming one ticket order for every path.  Fast monotone
collapse that only retires parked predecessors need not open a mutation sequence; its raw winner was
already canonical before filter close (`src/store/flatstore_atomic.inc:1687-1714`).

On the conservative grouped read-forward path, the cleanup line chooses “leave it unavailable,”
then closes the filter; a later sampled ordinary GET sees the negative filter and republishes the
already-named slot.  Thus neither the even slot nor its active hint is exposed while the filter is
positive even though safe invalidation may precede filter close.

The final committer does not edit another shard's filter cells.  It publishes the shared decision;
each participating owner consumes that decision in its existing cleanup/owner work and closes only
its own cells.  Reply retirement need not wait for those closes—continued positives merely send
reads to the already-correct resolver.  This preserves the single-writer cell design and makes a
late owner notification a performance delay, never a visibility hole.

FLIP/load-balancing transfer treats the cells and read-forward owner metadata as physical-shard
state.  The existing quiescence boundary must transfer writer tenure before the new owner edits
either.  Foreign readers continue to use atomics and do not care which executor currently owns the
shard.

Expose shared sidecar gauges as `foreign_read_unsafe_refs`, `foreign_read_occupied_cells`,
`foreign_read_wildcard_cells`, `foreign_read_saturated_cells`, and
`foreign_read_poisoned_shards`.  The first is exact occurrences, not distinct keys; the cell gauges
support an occupancy estimate without mislabeling it as `atomic_pending_entries`.

### 4.2 Customer 1: read-forward slots

Retain the useful shape of `t-hotfwd`: one copied positive GET snapshot per physical shard, a sole
shard-owner writer, and no IO dereference into FlatStore/KvObj memory
(`t-hotfwd:src/core/hot_forward.h:1-6`).  The initial bounds remain a 256-byte key, a 4096-byte fully
formatted RESP reply, and promotion after 16 samples
(`t-hotfwd:src/core/hot_forward.h:23-28`).  Raw, integer, and external String encodings are
publishable; missing, wrong-type, or oversize states remain unavailable
(`t-hotfwd:src/core/hot_forward.h:289-347`).

The slot protocol remains a non-spinning seqlock over atomic words: acquire an even sequence, check
hash/length/exact key, check the per-key filter, check ROB order, copy atomic reply words, and require
the same even sequence before the TTL check (`t-hotfwd:src/core/hot_forward.h:66-100`,
`t-hotfwd:src/core/hot_forward.h:296-361`).  Put the filter and order tests before the up-to-4096-byte
copy.  A touched target becomes odd after filter add and before physical install; it remains odd
until an owner can publish a proven latest committed object.  The conservative implementation leaves
it odd through record cleanup and lets a later sampled ordinary GET republish, exactly matching the
old branch's “no atomic intermediate” rule
(`t-hotfwd:DESIGN.md:215-228`).  Unrelated target shards/keys remain available.

After a successful copy, mark the forwarding prefix for the deferred pre-fallback cut refresh in
section 3.3.  Retain the exact two-argument GET (the branch flags GET alone), plaintext/borrowable
connection, positive-only String, MULTI exclusion, same-connection order, and log-every-command
fallback when `slowlog-log-slower-than` is set to 0
(`t-hotfwd:src/cmd/t_string.cc:1372-1376`).  A forwarded success must
complete the ordinary Op/ROB and increment command, operation, fingerprint, dedicated-forward, and
generic keyspace-hit accounting, as the branch does
(`t-hotfwd:src/core/io_loop.h:2864-2905`).  Any maxmemory-enabled configuration stays a fallback in
the B+ baseline, matching the branch's IO gate and all-slot invalidation
(`t-hotfwd:src/core/io_loop.h:2879-2884`, `t-hotfwd:src/core/ex_loop.h:424-435`).  Narrowing this to
selected policies would require a separate proof for touch/eviction semantics and live policy
transitions.

Preserve the owner-side expiry closure as well as the reader TTL check.  The owner tracks the minimum
absolute deadline and makes a due target odd before normal lazy/active expiry; FLIP and LB transfer
rebuild that owner metadata (`t-hotfwd:src/core/hot_forward.h:179-191`,
`t-hotfwd:src/core/ex_loop.h:358-368`, `t-hotfwd:src/core/ex_loop.h:408-420`,
`t-hotfwd:src/core/ex_loop.h:1240-1251`).  Clear the active hint with that invalidation.  Relying only
on IO's wall-clock rejection could resurrect a stale copy after a realtime clock rollback.

### 4.3 Customer 2: read-local GET while other atomic work is pending

Remove the parser's whole-shard `read_local_pending(state) != 0` rejection
(`src/core/io_loop.h:4260-4267`).  That branch is not the whole change: today
`read_local_state_eligible()` itself includes `PendingBit`, and the helper feeds validation,
prefetch, parser admission, and the executor's final pending fallback
(`src/store/flatstore.h:641-643`, `src/store/flatstore.h:676-690`,
`src/core/ex_loop.h:613-617`).  Split “stable/non-mutating” from “any atomic entry” at every one of
those sites.  The parser may use a positive per-key cell as an early fallback, but the executor-side
probe is authoritative because a group can open after parsing.

Change `read_local_probe(hash,key)` from “any pending entry” to the filter protocol in section 2:
capture an even mutation state, check exactly this key's cell, load/copy its immutable object, and
revalidate the mutation state.  Keep every current context, WATCH, unretired-write, lane-capacity,
type, expiry, and mutation-sequence rule (`src/core/io_loop.h:4239-4301`,
`src/core/ex_loop.h:555-618`).  Keep the all-local/all-owner prefix publication fence
(`src/core/ex_loop.h:620-699`).  Keep the existing public counter name
`read_local_fallback_atomic_pending` for observed filter positives, including permitted fingerprint
false positives, even though its scope becomes narrower
(`src/core/thread.h:114-130`, `src/cmd/t_server.cc:2172-2194`); add new names rather than
silently testing a nonexistent `read_local_fallback_pending`.

The result is the intended refinement: if group G has one pending key A on shard S, `GET A` falls
back to the owner resolver, but `GET B` on the same S can use read-local if B's fingerprint bucket is
negative and its store sequence is stable.  This is safe even when G is installed but epoch-zero,
committed but waiting for cleanup, or aborting, because A retains its filter reference in all three
states.

### 4.4 Customer 3: MGET through the local path

Do not implement this by merely adding `ReadLocalEligible` to MGET.  MGET is currently
`Readonly|MultiShard` with an xshard-only handler (`src/cmd/t_string.cc:1402-1403`), while the generic
read-local context gate rejects `MultiShard` (`src/core/io_loop.h:4250-4254`).  More importantly, the
GET-only prefix decision happens before scatter classification
(`src/core/io_loop.h:3665-3677`).  A flag-only patch could claim a cross-shard MGET as a GET prefix
and later send it through scatter, breaking the prefix's all-local/all-owner fence.

Add a distinct `ReadLocalMultiEligible`/MGET admission after key routing and scatter classification.
Reuse the route array the scatter preparation already builds
(`src/cmd/scatter_engine.inc:1530-1545`).  If all local-context gates pass, enqueue one local MGET
work item holding the key order, hashes, physical shards, and distinct-shard list; keep its connection
one-command fence until validation/fallback.  Execute the protocol in section 3.2 and fill only owned
`ValueSlot`/reply storage.  The existing cross-shard MGET uses one pinned read context and an owner-
specialized gather (`src/cmd/scatter_engine.inc:2433-2447`,
`src/cmd/scatter_engine.inc:2637-2676`); that exact path remains the fallback and final semantic
authority.  Its existing assembly code stays the one RESP-shape implementation
(`src/cmd/xshard_commands.inc:1531-1589`, `src/cmd/xshard_commands.inc:1674-1686`).

The downgrade cannot blindly reuse GET's local fallback, which posts only one `task_shard()` owner
task (`src/core/ex_loop.h:540-553`).  Same-owner MGET is the exception: `xshard_prepare` returns
`NotScatter`, marks `local_xshard`, and its fallback can post that one owner to the existing
non-yielding MGET handler (`src/cmd/scatter_engine.inc:1547-1572`,
`src/cmd/xshard_commands.inc:1778-1800`).

For true cross-shard MGET, preserve a prepared-but-unpublished fallback `ScatterState` at IO
admission.  A failed local attempt returns a downgrade token to its owning IO thread, which submits
that state through the normal scatter machinery with the original Op, read cut, ordering position,
and accounting.  Local success, cancellation, connection teardown, or failed admission must destroy
the unpublished state and unregister its pinned snapshot through normal `xshard_destroy()`
(`src/cmd/scatter_engine.inc:1848-1882`); otherwise it leaks both memory and the global cleanup floor.
If holding both forms proves too costly, return the token and rerun `xshard_prepare` in the owning
`ScatterArenaPool` instead.  In either design no staged bytes or borrowed pointer crosses the
transition, and only one path publishes a reply or counters.

Add local MGET counters for `hits`, `generation_retries`, `fallback_pending`,
`fallback_generation`, `fallback_policy`, and the existing type/missing/expiry/capacity reasons.
Count once per command, not once per key, except a separately named diagnostic key-probe counter.
That makes fallback-rate measurements in section 6 interpretable.

### 4.5 Complete read-forward rename and INFO surface

`t-hotfwd` has not landed on this base, so rename before integration and provide no compatibility
alias:

| Old branch name | B+ name |
|---|---|
| file `hot_forward.h` | `read_forward.h` |
| class/type/template/member/helper prefix `HotForward` / `hot_forward` | `ReadForward` / `read_forward` |
| command flag `HotForwardEligible` | `ReadForwardEligible` |
| CLI `--hot-forward 0|1` | `--read-forward 0|1` |
| CONFIG key `hot-forward` | `read-forward` |
| `tomokv.conf` prose | read-forward terminology only |

The old public surface is in `t-hotfwd:src/core/config.h:245-247`,
`t-hotfwd:src/core/config.h:717-724`, `t-hotfwd:src/core/config.h:1067-1068`, and
`t-hotfwd:src/cmd/t_server.cc:331-332`.  Rename source identifiers, help, validation errors, comments,
tests, documentation, and stats together; do not leave “hot forward” as a user-visible synonym.

Expose dedicated INFO fields; `read_forward_active_slots` is a current gauge and the others are
cumulative counters:

- `read_forward_hits`
- `read_forward_promotions`
- `read_forward_active_slots`
- `read_forward_fallback_pending`
- `read_forward_fallback_order`
- `read_forward_fallback_churn`
- `read_forward_fallback_expired`
- `read_forward_invalidations`
- `read_forward_cut_lifts`

`read_forward_fallback_pending` counts only an actual active exact-slot attempt that races and
observes a positive filter.  The normal owner path makes the slot odd and clears its hint first, so
that transition is evidenced by `read_forward_invalidations`, not by manufacturing an IO fallback
counter write.

An optional `read_forward_fallback_policy` is useful for maxmemory/slowlog exclusions.  Continue to
include successes in ordinary `keyspace_hits`, but do not make that aggregate the only evidence that
forwarding engaged.  The old branch folded hits into the generic count and emitted no dedicated
read-forward fields (`t-hotfwd:src/cmd/t_server.cc:1476-1480`,
`t-hotfwd:src/cmd/t_server.cc:1617-1620`, `t-hotfwd:src/cmd/t_server.cc:1841-1843`).  Do **not** count
every ineligible/no-slot GET; writing a counter on the inert path would recreate the cost being
removed.

### 4.6 Armed-but-inert read-forward must be approximately free

The owner-provided `t-hotfwd` acceptance result (2026-09-02) found the decisive regression at atomic
mode 1: hot-key, 32 cores, p32 was 4.76 Mops/s with read-forward off and 4.03 Mops/s with it armed,
**-15.3%**, while forwarded hits were approximately zero.  This was not a stale SET record.  Atomic
mode 1 permanently sets the enabled bit, and
`atomic_tracking_active()` tests the entire nonzero word
(`t-hotfwd:src/core/server.h:170-171`, `t-hotfwd:src/core/server.h:2248-2252`,
`t-hotfwd:src/core/server.h:2268-2270`).  The branch consequently rejects both promotion and IO hits
forever (`t-hotfwd:src/core/ex_loop.h:1191-1192`,
`t-hotfwd:src/core/io_loop.h:2879-2884`).

B+ removes that global atomic gate.  Static inspection also found avoidable machinery in the failed
build; these are plausible contributors, not separately benchmark-attributed causes, and the
implementation acceptance must verify the result:

1. **No broad template dimension.**  Do not thread `ReadForward` through the entire IO event/execute
   graph or select a larger executor body for every batch.  Keep narrow cold helpers at a sampling
   tick, a matching mutation boundary, and the exact routed GET attempt.  The old branch selected
   true/false executor bodies per batch (`t-hotfwd:src/core/ex_loop.h:921-930`).
2. **Define both discovery modes without a per-GET observer call.**  With key LB enabled, preserve
   the compact `note_lb_hash` path and invoke a cold observer only when its existing all-op 1/N tick
   fires.  With key LB off, mainline does not arm that sampler
   (`src/core/ex_loop.h:95-100`), so compare the existing owner-local GET command count, already
   maintained for INFO (`src/core/thread.h:262-269`), with a `next_read_forward_sample` threshold.
   The predicted-false comparison is the only per-eligible-GET addition; the cold sample validates
   success/representation and writes the next threshold.  Do not replace the normal GET body with
   the branch's larger observer call on every operation.  The two old sampling shapes
   are visible at `t-hotfwd:src/core/ex_loop.h:58-61` and
   `t-hotfwd:src/core/ex_loop.h:1166-1198`.  Stop/relax discovery while the shard already has an
   active target, and require promotion/engagement tests with key LB both on and off.
3. **Keep the active hint IO-local on the parse path.**  Rare owner transitions maintain a global
   atomic shard bitmap with `fetch_or`/`fetch_and` plus an active-slot gauge; a plain shared bitmap
   store could lose another owner's bit.  An IO thread refreshes its local cached bitmap only on an
   existing low-frequency maintenance/timer tick, never once per parse pass.  Common inert parsing
   is therefore one predicted branch on local state, with no shared-line load; when locally nonzero,
   ordinary routing tests a local shard bit before touching scratch/filter/slot state.  Set the
   global bit only after the filter is negative and an even slot is published; make the slot odd
   before clearing it.  A stale false negative merely delays acceleration until refresh, and a stale
   positive makes an authoritative slot/filter attempt fail safely.
4. **Order cheap rejection before copying.**  Once a shard bit and exact slot identity match, test
   ROB order and the pending filter before copying reply words.  Sample `CLOCK_REALTIME` only for a
   stable matching slot with TTL, as the branch already does
   (`t-hotfwd:src/core/hot_forward.h:66-100`).
5. **Counters start at a real attempt.**  Per-IO single-writer relaxed counters are aggregated by
   INFO.  No-slot, wrong-command, inactive-shard, and disabled policy checks on the common path do not
   increment reason counters.  `read_forward_cut_lifts` increments only when the cold
   post-hit/pre-fallback refresh actually raises a cut, not once per hit.

Acceptance for the implementation lane separates engagement from inertia.  The exact atomic-1
64-byte hot String, 32-core p32 cell from the failed run is now a **positive** control: warm until
promotion and active-slot counters prove engagement, then require nonzero dedicated hits and the
intended speedup.
Separate sustained inert controls use missing/wrong-type/over-4096-byte targets, any maxmemory
policy, and a deliberately unsafe-held target; each must prove zero active hits and keep armed
off/on throughput within 1% or benchmark noise with no material p99 regression.  A brief pre-
promotion warmup is not an inert benchmark.  “The feature was unable to engage” is not an excuse for
an armed cost.

## 5. Test plan

This lane does not run tests.  The implementation lane must add a directed `tests/bplus.py` (or
equivalent focused cases) and then include it in the normal debug gate.  Every race arm below has an
explicit scheduler checkpoint, a geometry assertion, and a mechanism counter; repeated sleep-and-
hope loops are not acceptance evidence.

### 5.1 Deterministic infrastructure

Use the existing debug surface:

- `DEBUG SHARD key` is the boot-specific routing oracle (`src/cmd/t_server.cc:931-938`).
- `DEBUG LBSIGNALS` supplies shard-to-executor mapping (`src/cmd/t_server.cc:814-820`); the robust
  selection pattern already exists in `tests/atomic_torn.py:125-193`.
- `DEBUG ATOMIC-COMMIT-DELAY us` holds a group after ticket reserve and before the epoch store, for at
  most one second (`src/cmd/t_server.cc:940-952`, `src/core/server.h:2438-2452`).
- `DEBUG ATOMIC-FANOUT-DEFER us` parks all but the lead owner fragment of a cross-shard read
  (`src/cmd/t_server.cc:968-982`, `src/cmd/scatter_engine.inc:1810-1820`,
  `src/cmd/atomics_glue.inc:656-680`).
- `DEBUG ATOMIC-READ-DELAY us` currently stalls only an owner-side plain read with pending records
  (`src/cmd/t_server.cc:954-966`, `src/cmd/atomics_glue.inc:896-903`).

The last point matters: none of today's hook consumption sites can pause a **successful IO-local**
copy between its checks.  The B+ implementation must reuse the existing `ATOMIC-READ-DELAY` knob at
three debug-only checkpoints: after an initial local filter miss, after local MGET's first element,
and after read-forward has its parse cut but before it loads/copies the slot.  The local path must
snapshot the configured duration into that Op, consume the local arm once, increment a checkpoint-
specific counter, and park/requeue until the stored deadline.  Setting the knob back to zero after
the counter moves prevents an unrelated control read from arming but does not shorten the captured
hold.  Do not spin or block the owner needed by the test.  This extends the current bounded-delay
hook; it does not pretend the existing load-and-sleep site is a releasable latch.  Disabled
production cost is one predicted-false check in an already armed-only helper.

The existing fan-out hook likewise has only a deadline/park return, not a counter
(`src/cmd/atomics_glue.inc:673-680`).  Add debug-only `atomic_fanout_defer_parked`, incremented on the
first actually deferred fragment **after snapshot registration**.  `atomic_fanout_cuts` is not a
substitute: under ordinary atomic tracking it need not increment
(`src/cmd/scatter_engine.inc:1734-1745`).  For the read-forward checkpoint, the continuation must
also preserve the original `pass_read_cut`, forwarding-tail, and `forwarded_since_cut` state across
requeue; resuming as a fresh parse pass would sample away the very stale-cut hazard being tested.

`atomic_commit_holds` is not a direct “delay entered” counter either.  It advances when a separate
needs-snapshot read is prepared while the drawn sequence is ahead of the safe watermark
(`src/cmd/scatter_engine.inc:1734-1745`).  Tests below deliberately issue that probe and require its
delta; an implementation may additionally add a direct commit-delay checkpoint counter, but must
not assert the existing field without the probe.

The deterministic pattern is: arm; wait for the new checkpoint counter; disarm globally; run and
complete the writer strictly before the held Op's deadline; let the deadline resume the read; assert
reply ordering, elapsed window, and mechanism-counter deltas.  Failure to complete inside the
window is a failed/unsupported run, not a semantic pass.  `tests/execatomic.py:200-271` is the
existing timed-park/writer-inside-window precedent; its 50 ms scheduling lead does **not** provide
the new checkpoint counter.  The existing session oracle is
`GET a` followed by `MGET a,...`: a later smaller generation is a session reversal and unequal MGET
elements are a torn command (`tests/session_monotonic.py:8-40`).

Every case must fail or skip loudly if it cannot prove its geometry/window.  The kernel-7.0.0-30
lesson is encoded in `tests/atomic_torn.py:657-667`: a probabilistic control that happened not to tear
does not become a clean pass.  Required non-vacuity assertions include distinct physical shards and
executors, debug checkpoint consumed, writer completed inside the hold, and the expected B+ hit,
retry, fallback, promotion, or cut-lift counter advanced.

### 5.2 Directed hazard A: group opens during a local read

1. With `--atomic 1 --read-local 1`, use SHARD/LBSIGNALS to select target A and partner P on distinct
   executors, plus unrelated B on A's physical shard.  Use the controllable filter-index seam to
   choose B in a known-negative cell; an allowed fingerprint false positive cannot be treated as a
   failed refinement.  Initialize A/B/P to `old` and establish local-hit controls.
2. Arm `ATOMIC-FANOUT-DEFER 10000000` and start a holder MGET over A/P.  Wait for
   `atomic_fanout_defer_parked`; its
   registered old cut will retain the writer's later predecessors.  Disarm the global fan-out knob
   without changing the holder's captured deadline.  Then arm one-shot local
   `ATOMIC-READ-DELAY 1000000`, send `GET A`, and wait until it has captured an even state and negative A cell
   but is parked before copying.  Disarm the global hook without changing that Op's stored deadline.
3. Arm `ATOMIC-COMMIT-DELAY 200000` and asynchronously send `MSET A new P new`.  The shorter commit
   hold must fit strictly inside the already-captured one-second local-read hold.  Issue nondeferred
   cross-shard probe reads until `atomic_commit_holds` advances or the bounded window expires.  The
   counter proves a read cut observed the drawn-but-unpublished window; raw candidates are installed
   and A's filter must be positive.  Missing the counter is a failed arm, not a pass.
4. When the local deadline resumes GET, its enclosing state/filter validation must fail and discard
   the staged copy.  After commit, its owner fallback at the retained old cut must return `old`.
   Assert the existing `read_local_fallback_atomic_pending` advanced and the local-hit counter did
   not.
5. While the commit/read window is held, issue `GET B` with the local delay disarmed.  It must hit
   read-local and return `old`.  This proves B+ refined the gate per key rather than retaining the
   old whole-shard refusal.
6. Let the holder's captured 10-second deadline expire, let cleanup finish, require A eventually
   reads `new`, and require filter references and pending gauges to return to baseline without
   underflow.

Repeat the same schedule with a promoted read-forward slot for A.  Group preparation must make the
slot odd and remove the active hint, so assert `read_forward_invalidations`, active-slot loss, owner
fallback, and no forwarded hit rather than requiring `read_forward_fallback_pending`.  After the
holder releases and cleanup is safe, an ordinary sampled GET of the already-named target can
republish it without another 16-sample promotion
(`t-hotfwd:src/core/hot_forward.h:121-125`).  Also run MSETNX's partial-install abort shape; no
local/forward read may expose its rejected candidate before restore.

### 5.3 Directed hazard B: commit/write between MGET keys

**Generation-directed atomic case.**  Select MGET keys A/B/... and an outside partner P such that A/P
span executors.  Park after copying first key A, then commit `MSET A new P new` and require its reply
before the held MGET resumes.  Use the controllable cell seam to ensure A's live reference does not
false-positive any remaining argv key.  The final shard sweep must therefore detect A's changed
publication generation.  Require `local_mget_generation_retries` or
`local_mget_fallback_generation`; accept only the complete old vector or the vector with A=`new`,
never a reply assembled across the invalid first attempt.

**All-key atomic case.**  Repeat with an atomic MSET changing all eight MGET keys.  This may encounter
a later key's positive filter before the final sweep, so require a pending **or** generation
retry/fallback counter and an all-old or all-new reply, never a mix.

**Plain-write duplicate case.**  Initialize K=`old`, send `MGET K K`, and park after the first K
copy.  Complete ordinary `SET K new`, then resume.  Require a generation retry/fallback and two equal
elements.  This is the test that rejects a group-only generation.

Add writer-before, writer-after, and unarmed duration controls; a missing/type/expired element must
downgrade the whole command without leaking staged bytes or borrows.  Under a continuous writer,
assert at most two local attempts and eventual owner fallback—no spin, starvation, or duplicate
accounting.  Exercise same-shard MGET and cross-shard MGET separately because the former currently
gets its atomicity from one non-yielding owner loop (`src/cmd/xshard_commands.inc:1778-1800`).

### 5.4 Directed hazard C: latest slot versus an older cut

This negative-control proves why slot safety needs only the key filter/sequence while session order
still needs the lazy cut refresh.

1. Start with `--atomic 1 --read-forward 1 --read-local 1`.  Choose target K on a physical shard
   outside Q/P's group, and choose Q/P on distinct executors;
   ensure Q has no usable slot.  Initialize all to `old`; promote only K and assert
   `read_forward_promotions`/`read_forward_active_slots`.
2. Arm `ATOMIC-FANOUT-DEFER 10000000` and start a holder MGET over Q/P.  Wait until it registers and
   holds the old cut S (`src/cmd/atomics_glue.inc:400-449`), then require
   `atomic_fanout_defer_parked`.  A later
   committed epoch at or above the published exclusive floor cannot clean while this holder remains
   (`src/store/flatstore_atomic.inc:1622-1637`).  Disarm the global knob without shortening the
   holder's captured deadline.
3. On the tested connection send one payload, `GET K; GET Q`.  At one-shot
   `ATOMIC-READ-DELAY 1000000`, park the first GET after its pass cut S' is chosen but before slot
   load; wait for the read-forward checkpoint counter, consume/disarm the hook, and assert the
   continuation retained S' rather than starting a new parse pass.
4. Commit atomic `MSET Q new P new` at T and require its reply.  Q/P's old history remains retained by
   the holder and Q remains filter-positive.  **After** T's reply, complete exact `SET K new`; the
   unrelated K filter is negative, so B+ can reuse the post-handler exact-write publication shape
   (`t-hotfwd:src/core/ex_loop.h:1126-1129`, `t-hotfwd:src/core/ex_loop.h:1201-1223`) after replacing
   the branch's global-atomic/shard-record rejection (`t-hotfwd:src/core/ex_loop.h:1209-1215`) with
   K's exact negative filter.  Assert K is active/even and Q still has a filter reference.
5. Let the tested Op's deadline resume.  `GET K` must hit and return `new`.  Before preparing the
   younger non-forwarded `GET Q`, the lazy boundary refresh must raise `pass_read_cut` to at least T;
   Q's positive filter forces its owner resolver, which must return `new`, never retained `old`.
   Assert `read_forward_hits`, `read_forward_cut_lifts`, Q's
   `read_local_fallback_atomic_pending`, and owner resolution with the lifted cut
   (`src/cmd/atomics_glue.inc:1016-1022`).
6. Let the holder's captured 10-second deadline expire, verify Q/P records and filter references
   drain, and run an unarmed duration control.  A run without the holder, the K hit, an actual cut
   lift, or Q's retained record/fallback is vacuous.

Without the refresh, the real-time chain `commit T reply -> SET K reply -> forwarded K=new` is
followed by Q resolving at stale S'<T and returning `old`: time goes backward on one connection.
Cross-shard MGET is not used for the second read because snapshot registration would itself confirm
and widen the stale cut (`src/cmd/atomics_glue.inc:424-448`).

### 5.5 Representation, lifecycle, and acceptance matrix

Add focused representation tests with a controllable hash/index seam: same fingerprint references,
different fingerprints sharing one bucket, sticky wildcard until drain, overlapping add/close,
saturation poison plus the exact-total zero-drain rebuild, and no premature empty cell.  Add
lifecycle cases for prepare-with-no-install,
partial abort, ordinary monotone cleanup, overlapping inverted-ticket cleanup, duplicate argv keys,
script intent PIN/APPLY/UNPIN (PIN alone stays unfiltered; APPLY publishes before mutation),
competing writes during an intent, and FLIP ownership transfer.  In each, a false positive is
acceptable; a negative for a still-unsafe key is fatal.

Run semantic matrices at atomic 0/1 and read-local/read-forward off/on for GET, same-shard MGET, and
cross-shard MGET; RESP2/RESP3, pipeline depth 1/32, missing, type, integer/raw/external String, TTL,
ACL/WATCH/MULTI, notification/tracking/MONITOR, TLS, maxmemory, slowlog-all, FLUSH, and shutdown.  The
existing `tests/session_monotonic.py` and atomic torn/EXEC batteries remain mandatory.

Performance acceptance includes:

- disabled off-vs-base code/layout checks;
- sustained armed-but-inert atomic-1 cases using wrong-type/oversize/maxmemory/unsafe targets, with
  zero active hits and off/on parity;
- the exact hot String 32-core p32 cell that regressed 15%, warmed into an active read-forward
  positive control with nonzero promotion/hit evidence;
- uniform GET/read-local with unrelated pending groups on the same shards;
- populated MGET-8, 9:1 MGET-8:MSET-8, 1:1 mm-mix, and equal GET/SET/MGET-8/MSET-8; and
- filter wildcard population, MGET generation retry/fallback rate, throughput, p50/p99, executor
  ops, and memory.

No performance result counts unless the corresponding engagement/fallback counters prove which path
ran.

## 6. Risks, non-goals, and expected fallback rates

### 6.1 Risks and containment

| Risk | Consequence | Containment |
|---|---|---|
| Missed key or wrong add-before-install ordering | A foreign reader can expose a private, aborted, or stale physical value | One publication helper used by group/plain/transaction/apply preparation; pass hash into plain prepare; release-before-exchange assertions; fail-closed poison for unenumerable/overflow cases; directed checkpoint tests |
| Clearing at logical commit | Ticket inversion or abort leaves the wrong raw winner readable | Retain per-entry references to final safe cleanup/canonicalization; slot stays odd; section 1.4 invariant |
| Fingerprint collision/sticky wildcard | Extra owner fallbacks, especially after churn | 4096 cells, strong keyed mixing, wildcard/saturation INFO, clear only on verified drain; correctness unaffected |
| Long-held snapshot floor or slow cleanup | Committed keys remain filtered longer than their logical open interval | Measure filter residence and refs; preserve bounded owner cleanup; never trade a false negative for hit rate |
| Write-heavy shards churn MGET generation | Repeated copies and fallback pressure | One full retry only, then existing owner path; per-command retry/fallback counters |
| MGET reply allocation/copy amplification | Large values double work before a failed final check | No borrow before validation; bounded retry; initial positive-string scope; allocation/cap failure falls back |
| Latest slot above an older parse cut | Later same-session owner read can go backward | Lazy post-hit/pre-fallback safe-watermark cut refresh plus existing ROB order; deterministic slot/cut test |
| Dynamic script/broad mutation misses exact keys | Filter false negative | Publish declared APPLY keys before mutation; if the exact write set is unavailable, poison affected shard(s) and invalidate slots |
| Expiry or maxmemory side effects differ off-owner | Incorrect nil/touch/notification behavior | One MGET realtime cut; fallback elapsed/unsupported cases; keep read-forward disabled for every maxmemory-enabled configuration in the B+ baseline |
| Active-slot hint races | Lost acceleration or extra probe | Hint is never authority; multiwriter global bitmap uses RMWs and IO reads a local cached copy; set only after even publish plus filter clear, clear after odd invalidation; slot/filter validation decides |

### 6.2 What B+ deliberately does not do

- **No pre-image service.**  B+ never copies or returns a parked predecessor on an IO/fused foreign
  thread.  A filtered key simply falls back to the owner and existing MVCC resolver.
- **No historical read-forward slots.**  A slot has no epoch/cut variants.  It is unavailable or one
  latest-committed positive String reply.
- **No MVCC semantic change.**  Ticket reservation, epoch publication, safe watermark, read cuts,
  own-write overlay, abort, and collapse winner selection remain authoritative.
- **No claim that `atomic_pending_entries` is a key gauge.**  New filter reference/distinct/wildcard
  telemetry gets new names; the existing entry count keeps its existing unit.
- **No unbounded retry.**  Churn degrades to owner execution.
- **No automatic expansion to EXISTS/TOUCH/KEYS, arbitrary types, negative slots, or TLS forwarding.**
  Those need separate side-effect and reply-lifetime designs.
- **No promise of acceleration for a hot key while it is unsafe.**  That fallback is the cost of
  choosing no pre-image cache.

### 6.3 Quantified fallback model

The relevant population is not merely epoch-zero groups.  Let, for shard `s`:

- `K_s` = the read key-selection/address universe on that shard, including queried missing keys;
- `A_s` = the number of all distinct keys occupying the shard's unsafe filter, including decided
  records waiting for safe cleanup;
- `U_s` = the number of those unsafe keys whose identities are in the read-selection universe;
- `m_s` = keys from one read on that shard; and
- `f_s` = unrelated-key fingerprint false-positive probability from section 2.4.

For uniform independent key selection and cell occupancy, with `f_s` computed from total occupancy
`A_s`, one key's approximate filter-positive
probability and an MGET union are

```
r_s = U_s / K_s + (1 - U_s / K_s) * f_s
P_filter = 1 - product_s (1 - r_s) ^ m_s .
```

These are **filter fallback conditional on an otherwise eligible local read**, not total
read-forward fallback.  Generation churn and policy/type/missing/order fallbacks are separate.  With
`K=100,000` per shard and a shared uniform read/write universe (`A=U`):

| Unsafe keys `U` | Read-local GET filter fallback | Local MGET-8: at least one filter fallback |
|---:|---:|---:|
| 8 | about 0.00819% | about 0.0655% |
| 64 | about 0.0761% | about 0.607% |

An unenumerable broad-write poison is the explicit worst case: every otherwise eligible local read
of that shard filter-falls back for the poison interval, and its read-forward target is invalidated.

The one-slot-per-shard read-forward customer has a different coverage limit.  With a usable target
whose read share is `h`, target unsafe duty `d`, and target-bucket false-positive probability
`f_target`, its total fallback before policy/order/churn terms is approximately

```
P_read_forward ~= 1 - h * (1 - d) * (1 - f_target).
```

For uniform `K=100,000`, `h=1/K`, so even after forcing a target active this is about **99.999%**;
organic promotion is effectively absent because the detector requires 16 consecutive sampled
occurrences of one key (`t-hotfwd:src/core/hot_forward.h:102-145`).  Read-forward is a hot-key
accelerator, not a uniform-cache design.

For a hot-key shape, let `h` be the hot target's read share and `d` its foreign-unsafe wall-time
duty.  Read-local's true-overlap term is approximately
`h*d + (1-h)*U_cold/K_cold` before fingerprint collisions.  Read-forward uses the coverage formula
above.  At `h=90%`, `d=5%`, and negligible `f_target`, read-local pays about **4.5%** true hot overlap
while read-forward falls back about **14.5%** total (the 10% non-target reads plus 4.5% overlap).  At
`h=100%`, both reduce to `d` when collision, policy, order, and churn terms are negligible.  With
independent group arrivals at rate `R` and mean unsafe residence
`L`, `d ~= 1-exp(-R*L)`: `R*L=0.05` gives 4.88%, while `R*L=0.5` gives 39.3%.  During every unsafe
interval the target falls back 100%; B+ deliberately has no pre-image to serve.

For this memo, **mm-mix** means the repository's 1:1 MSET/MGET command mix, not MULTI/EXEC.  The
native tool's `mcmd ... mix` alternates commands over independently generated random key sets
(`tools/benchtxn.cc:17-21`, `tools/benchtxn.cc:375-404`, `tools/benchtxn.cc:475-502`).  Neither command
is a read-forward GET, so read-forward eligibility is 0% in this shape; the useful B+ customer is
local MGET.  If MSET-8 arrives at rate `R_w`, keys are uniform, there are `S` shards,
and mean filter residence is `L`, low-occupancy Little's-law gives

```
A_s ~= 8 * R_w * L / S
A_s,occupancy ~= K_s * (1 - exp(-8 * R_w * L / (S * K_s)))
P_mm_filter ~= 1 - (1 - r_s)^8 .
```

When reads and writes share that universe, `U_s=A_s`.  Thus at `K_s=100,000`, a measured `U_s=8`
gives about **0.0655%** filter fallback per MGET and `U_s=64` gives about **0.607%**, subject to the
independent-key/cell approximation.  The 50% write
share also raises generation churn.  Let `G_s` be the measured rate of **all** generation-advancing
outer brackets on shard `s` (writes, expiry/eviction/clear, cleanup/canonicalization, and conservative
topology work), and `T_s` the MGET interval exposed to that shard.  An independence approximation is
`1-exp(-sum_s G_s*T_s)`.  Atomic groups correlate shard events, so the sum may double-count a group
and is an estimate/upper model, not a prediction.  One retry then bounds the cost and owner fallback
terminates it.

These numbers are examples, not forecasts.  INFO must expose unsafe refs/total-occupancy estimates,
wildcards, actual filter fallbacks, and generation retry/fallback counts.  Uniform local traffic
tracks unsafe-key density, hot traffic pays update duty plus read-forward coverage, and mm-mix adds
the eight-key union plus bounded all-publication churn; none is hidden by weakening MVCC or serving a
pre-image off-owner.

## 7. Cell-epoch audit and replacement (lane `t-bplus-cellcheck`)

Line numbers in this section are at base commit `4b3a79753` (`t-bplus-impl`).

### 7.1 Measured defect

`--thread-mode 1s --read-local 1 --atomic 1`, 32 cores, 1:1 MGET8/MSET8 at p32, 512 conns, 1M keys,
64 shards, 14 s: `mget_local_hits 4.79M` (about 50%), `mget_fallbacks 4.87M`, of which
`fallback_generation 3.76M` (77%), `generation_retries 5.95M`, `fallback_seq_churn 1.06M` (22%),
`fallback_atomic_pending 57k` (1%).  Net throughput versus `--read-local 0`: mm11 +2.1%, mm91 -1.2%.
Separately, mix19 (plain GET/SET 1:9, no groups, atomic 1) measured -4.3% versus mainline while pure
GET was neutral, which points at the SET publish path.

### 7.2 What the MGET double-check read and what advanced it

- Both local MGET variants (`src/core/ex_loop.h:893-1030` plain, `:1032-1176` capture-prefetch)
  acquire-load `local_publication_generation_acquire()` for every touched shard before any value
  load (`:919-934`, `:1051-1065`) and again after every copy (`:1000-1015`, `:1147-1162`).
- With the filter armed that accessor returns `ReadLocalStoreState::publication`
  (`src/store/flatstore.h:755-761`, word at `:494`); with the filter OFF it returns the table word
  `probe_sequence` (`:495`).
- `publication` is advanced by `read_local_publication_mutation_begin/end`
  (`src/store/flatstore.h:3396-3409`, odd/even via `read_local_advance_generation` `:3413-3431`).
  Callers: `ReadLocalPublicationGuard answer_change` in `insert_into_read_local` (`:3009` new slot,
  `:3037` same-key replace), `erase_in_read_local` (`:3079`), the compiled-out selector-3 overwrite
  (`:2652`), and every `ReadLocalTableGuard`, because `read_local_table_mutation_begin/end` nests the
  publication bracket (`:3379-3393`).  TableGuard sites: atomic physical exchange
  (`src/store/flatstore_atomic.inc:1125`, one bracket per installed key), capacity grow (`:693`),
  ownership rebind (`flatstore.h:746`), scoped filter publication for local-xshard/script writes
  (`:842`), snapshot mark (`:2670`), rehash table move on insert (`:2837`), FLUSH clears
  (`:2865`, `:2895`), empty-table install (`:2993`), grow (`:3139`), rehash step (`:3151`), shard
  poison open (`:3482`).
- Therefore **every** plain SET/DEL/expiry/eviction on a touched shard and every per-key group
  install advanced the checked word.  Group commit (the epoch release-store) advances nothing;
  cleanup advances it only through a physical rewrite (`atomic_exchange_physical_read_local`).  At
  the measured rate (about 690k MSET8/s over 64 shards = 86k installs/s/shard, plus the same order
  of plain publications) a touched shard advanced during nearly every 8-key MGET window, so the
  one retry fired 5.95M times and 3.76M MGETs still fell back.  The check was correct and useless.

### 7.3 What `seq_churn` validated on the MGET path

The executor never returns `SeqChurn` for MGET: after two attempts it classifies the fallback as
`AtomicPending` (a queried key is now filter-positive) or `Generation`
(`src/core/ex_loop.h:1021-1030`, `:1167-1176`).  Every one of the 1.06M
`read_local_mget_fallback_seq_churn` therefore came from the **parser** admission sample
(`src/core/io_loop.h:4241-4257`): for each key, IO acquire-loads the shard's table word and rejects
the whole MGET if `!read_local_state_eligible(state)`, i.e. the table-mutation bit is odd at that
instant (`:4305-4327`).  The odd interval is one atomic exchange bracket (~200 ns) at 86k/s/shard,
about 1.4% duty per shard; sampled on 8 shards it rejects about 11% of MGETs.  That sample is
table-generation, not a per-op counter, but it is a snapshot of a bracket that closes long before
the executor reads anything; it validates nothing the executor does not validate itself.

### 7.4 Unconditional costs found on plain paths

- Plain GET: `foreign_read_key_unsafe` tests the pending bit of the already-loaded table word first
  (`src/store/flatstore.h:788-796`); no filter cell is touched when the shard has no open group.
  Confirmed zero-work; matches the neutral pure-GET measurement.
- Plain SET/DEL: the `publication` bracket above runs on every insert/replace/erase regardless of
  pending state (two loads, two release stores, an acq_rel fence, a depth counter, and a
  `__builtin_expect(...,true)` branch that is always mispredicted while armed).  Its only consumer
  was the MGET sweep.  This is the mix19 suspect and is removed with the sweep.

### 7.5 Replacement: per-cell touch epoch

`ForeignReadSafety` gains `epochs_[4096]` of `std::atomic<uint32_t>` beside `cells_`.  Layout:
cells 32768 B at offset 0, epochs 16384 B at 32768, three owner witnesses at 49152, `alignas(64)`
rounds `sizeof` to 49216 B (was 32832); 256 shards = 12.0 MiB, heap-allocated inside
`ReadLocalStoreState`, no locked struct grows (FlatStore 944, Shard 1440, ThreadCtx 1408, ExLoop
5848 unchanged).  The owner bumps a cell's epoch after every cell transition it publishes: add
(group/plain/transaction prepare, scoped writes, poison open), close (final entry free, scope
close, poison close), saturated-drain rebuild, and permanent poison.  Order is **cell store, then
epoch store**, both release.  Readers acquire-load an epoch only for keys whose shard word carries
the pending bit and only while the filter is armed; with the filter OFF (A/B control) every shard
keeps the generation rule below.

MGET protocol: (1) acquire-load the table word of every touched shard; poisoned generation
falls back; a shard without the pending bit must be even and records its generation; a shard with
the pending bit records nothing but marks its keys.  (2) For each marked key, acquire-load its
cell epoch.  (3) Copy every value with the unchanged per-key probe/validate.  (4) Acquire fence,
then for each non-pending shard require an even, equal generation; for each marked key require an
equal epoch.  Mismatch retries the whole command once, then falls back (`Generation`); a per-key
probe churn that survives the retry falls back as `SeqChurn`.  MGETs with more than 128 keys use
the generation rule for every touched shard (coarse, still correct).

Why there is no false negative.  A group publishes its cells before its first exchange, so a
reader that copied a pre-group value of `a` and later synchronized with any exchange of that group
(by reading a post-group `b`) also sees the epoch bump sequenced before that exchange.  A reader
whose pre-snapshot already contains the bump synchronizes with the epoch store, which is
sequenced after the cell store, so its cell probe must see the positive cell and falls back.
ABA: with counting alone a group that opens, installs, commits, and drains inside one MGET window
leaves the cell at 0 both times; the epoch is monotonic and reads +2 (add and close), so the
window is rejected.  Non-pending shards keep the table-generation rule, which every group install
advances; a group that opened but installed nothing changed no answer and the MGET linearizes
before it.  FLUSH clears now poison the filter for their duration so every epoch advances.

Kept as before: fail-closed saturated/wildcard/poisoned cells, the filter check inside the per-key
sequence proof, single-owner cell writes, immutable replacement, QSBR, and the one MGET recheck.
