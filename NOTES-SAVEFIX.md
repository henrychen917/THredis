# SAVE default gate fixes

## Constraints and status

This lane was inspected and changed without building, starting a server, or running any test or
benchmark. Only static source/diff inspection was performed. The reference-compatible default save
schedule remains `3600 1 300 100 60 10000` (`src/core/config.h:263-265`).

## 1. Notification class leak: actual mechanism

The `notify_armed_` latch is not itself the defect. Save must participate in that latch so the IO
dispatcher selects the registry's armed handler row (`src/core/io_loop.h:1238-1242`), and the latch
correctly includes `save_config_armed_` (`src/core/io_loop.h:2231-2237`). Without that selection,
the save scheduler would have no successful-mutation fire points to count.

The defect was the second half of the arming path. Before this fix, executor live-config refresh and
the synchronous `CONFIG SET notify-keyspace-events` barrier both represented save as
`NOTIFY_ALL | NOTIFY_SAVE` in the same shard mask that held the operator's notification classes.
For `notify-keyspace-events K$`, save therefore made the effective mask contain `K`, `$`, every
class in `A`, and `NOTIFY_SAVE`. An armed `LPUSH` called `notify_record(..., NOTIFY_LIST, ...)`:

1. The class gate accepted the list event because save had injected the list bit through
   `NOTIFY_ALL`.
2. Save's owner-local mutation counter incremented.
3. The slow recorder derived publication routes from the whole mixed mask. It found the operator's
   `K` bit and published `lpush`, even though the operator had not selected the list class.

Thus save's observer class bits and the operator's publication route were independently valid but
became invalid when combined in one undifferentiated mask. CLIENT TRACKING used the same synthetic
`NOTIFY_ALL` expansion and could create the same leak while a narrow K/E class was configured.

### Fix

The shard mask now stores only the configured notification flags plus the synthetic observer bits
(`src/core/ex_loop.h:135-143` and `src/cmd/t_server.cc:1231-1238`). It no longer materializes
`NOTIFY_ALL` on behalf of either observer.

`notify_routes_for_class` (`src/cmd/notify.h:57-63`) derives routes per event:

- K/E is retained only when `mask & cls` proves the operator selected that event class.
- `NOTIFY_TRACKING` is retained for the ordinary `NOTIFY_ALL` mutation surface, independently of
  K/E.

The inline recorders (`src/cmd/notify.h:174-197`) separately expand synthetic observers over
`NOTIFY_ALL`. A save-observed event still calls the owner-local `note_save_change()` counter, but a
save-only event with no real route returns before allocating a notification batch. The FlatStore
expiry/eviction path applies the same split (`src/cmd/notify.inc:408-424`), and the slow recorders
receive the already-filtered route word (`src/cmd/notify.inc:317-405`). `NOTIFY_NEW` and
`NOTIFY_KEY_MISS` remain outside `NOTIFY_ALL`, preserving the previous no-double-count/no-keymiss
observer surface.

No `Shard`, `Op`, or `Client` member was added or moved. Mutation counting remains owner-local in
`Shard::note_save_change` (`src/core/shard.h:139-146`), and cross-owner behavior is unchanged. The
clean handler specializations are unchanged; when notifications, tracking, and save are all off,
these new checks are unreachable behind the existing boot/live-latched armed-row selection.

### Validation geometry to run

`tests/notify.py:230-246` now makes the required A/B explicit on one 16-shard feature server (the
gate's normal 6 IO / 2 executor placement), using three connections: one admin, one mutator, and one
pattern subscriber. This is a single-key test, so same-owner versus cross-owner pairing is not
relevant.

For each of two live schedules, explicit empty/disabled save and an enabled clause whose enormous
seconds value prevents a snapshot during the check:

1. Configure `notify-keyspace-events` as `KE$`.
2. Mutate a new list with `LPUSH`; require no subscriber frame and no increase in
   `notify_events_fired`.
3. Configure the list class and mutate another list; require the exact `lpush` frame. This positive
   control prevents a server that emits nothing from passing.
4. Compare `rdb_changes_since_last_save`: the two mutations must add zero with save disabled and
   exactly two with save enabled. This proves class filtering did not disarm save's observer.
5. Restore the boot save value.

Run the complete notification battery as well, because the route derivation is shared by ordinary,
scatter, blocking, MULTI, script, expiry/eviction, and CLIENT TRACKING notification paths.

## 2. CONFIG REWRITE: product behavior was correct

No name was genuinely missing. The failing test had a stale classification: it used the presence of
any `save` line as proof that a non-boot name had leaked into the rewritten file. `save` is now a
boot-parsed, repeatable directive (`src/core/config.h:687-713`), and the rewrite emitter deliberately
writes one directive per clause (`src/cmd/server_tail.cc:670-680`). Three `save` lines are therefore
the correct file representation, while the runtime table and `CONFIG GET save` correctly flatten
the clauses to one space-separated value.

The product implementation was left unchanged. `tests/servertail.py:399-417` now:

- checks omission of the actual non-boot compatibility name, `aof-use-rdb-preamble`;
- requires the three ordered repeated directives;
- reboots the generated file and requires `CONFIG GET save` to return the flattened default.

Validation geometry: use scope B's throwaway server booted from a config file with two shards and
the battery's four-core default placement (2 IO / 2 executor), apply the existing live maxmemory and
slowlog mutations, rewrite, inspect the file as lines (not as a unique-name map), then reboot that
same file and check both the live mutations and flattened save value.

## 3. Differential harness configuration

The vanilla oracle was already booted with `--save ''` so a differential run could not create RDB
files (`tests/differ_gate.sh:161-164`). The target boot now carries the same explicit setting
(`tests/differ_gate.sh:183-190`). This is preferable to enabling save on the oracle: both sides stay
persistence-silent, while `CONFIG GET save` remains in the comparison surface. No `save` key or
result is hidden or special-cased in `differ.py`.

Validation geometry: run the full differ gate with its 16-shard, 6 IO / 2 executor target under
atomic 0 and 1, both seeds, and the pinned vanilla oracle. First confirm both processes report an
empty save value, then require the `compatintro` suite and the rest of the discovered suite matrix
to compare normally. The previous 119 construction-only save diffs should disappear without
reducing the compared key set.

## Measurement surface

The product-code change can touch successful mutation observation and notification publication for
all armed write families: strings/lists/sets/hashes/zsets/streams, generic key commands,
scatter/blocking/MULTI/script mutations, and keyless expiry/eviction. It also touches the route word
seen by CLIENT TRACKING when a narrow notification class is configured. CONFIG REWRITE and differ
changes are tests/harness only.

The main session should retain the mandatory GET, SET, MGET, and MSET throughput/latency/memory
cells. SET and MSET exercise the changed default-save observer path directly. GET and MGET are
headline zero-regression controls; they select armed rows under the default save schedule but have
no successful mutation to count. No performance measurement was made in this lane.

## Commits

- `771280cef` — separate save/tracking observation from operator notification classes.
- `bf32a011d` — correct CONFIG REWRITE's repeatable-save assertions.
- `aab1744ed` — align the differ target's save setting with the oracle.
- `b2429a19e` — prove save mutation counting remains armed through class filtering.
