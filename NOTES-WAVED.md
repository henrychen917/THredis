# Wave D — cleanup pass

Base: `pre-cleanup-2026-08-29` / `bfeaae357`.

This pass is deletion-only apart from three constant-return contract simplifications and comments
protecting Redis RDB decode compatibility. No server behavior fix is included. Source delta before
this report: 34 insertions, 282 deletions. Every deletion category is independently committed, and
every commit message names its deleted symbols and its reachability check.

## Removed

### Hot/write-only state

- `Op::db`, its four assignments, and the now-dead `db` parameters in `make_child_op`,
  `normalize_multi_blocking_pop`, and `prepare_commands`. A whole-tree member-access search found
  only the IO prepare assignment, two assignments in `multi.inc`, and the scripting copy from one
  dead `Op::db` to another. `multi.inc` is textually included by `xshard.cc`; that inclusion path
  was checked. `Session` was not touched. The 336-byte `Op` footprint lock remains in the source;
  the removed byte is absorbed by the existing alignment gap.
- `StreamNode::live_entries` and its five XADD/XDEL/trim writes. Exact-name search over `src`,
  `tests`, and `tools` found no read. `StreamNode` has no size lock; the nearby 56-byte assertion is
  for the distinct `StreamHeader` type.
- Dead `WbEngine::Stats` fields `sqe_starved`, `handoffs`, `zc_suppressed_tls`,
  `tls_plaintext_bytes`, `tls_ciphertext_bytes`, `tls_want_read`, and `tls_want_write`, together
  with their writes and unprinted aggregation. Member-qualified searches distinguished them from
  the live, similarly named `LoopSignals` counters.
- `LoopSignals::accept_rejected` and its two writes. The live `Server` rejection counters used by
  INFO remain.

### Unused accessors and superseded wrappers

- `Client::segments_size`, `Client::watched_refs`. Exact call-shaped searches found no caller;
  `segments_size` duplicated live `output_list_length`, while `safe_to_release` reads
  `watched_refs_` directly. No `Client` data member or 1984-byte layout was changed.
- `Server::client_cron_armed_ptr`, `Server::debug_atomic_commit_delay`, `Server::set_maxmemory`,
  `Server::set_maxmemory_policy`, `Server::set_maxmemory_samples`. Exact-name searches found only
  the definitions. The debug field/setter/direct atomic read remain live, and CONFIG SET already
  calls `set_maxmemory_config`.
- `ThreadCtx::nchan`, `ThreadCtx::cpu`, `ThreadCtx::task_in`, `ThreadCtx::client_in`,
  `ThreadCtx::release_in`, and `Topology::domain_of_current_thread`. Exact call-shaped searches
  found no use; live code uses the channel APIs/internal arrays and `Topology::domain_of`.
- `FlatStore::obj_bytes`, `FlatStore::snapshot_finished`, and
  `FlatStore::atomic_set_read_epoch`. `obj_bytes` duplicated live `object_bytes`; the finished
  state machine field remains live; all atomic callers use `atomic_set_read_context`.
  `flatstore_atomic.inc` is included inside `FlatStore` by `flatstore.h`, so the search covered all
  translation units that receive the textual definition.
- `HashVal::field_count`, `SetVal::adopt_compact`, `Rob::pinned_rbuf_off`,
  `LoopSignals::utilisation`, and the unused AOF manager getters `file_path`,
  `directory_path`, and `last_error`. Each had no exact call site. Live siblings (`entries`,
  `replace_compact`, `avg_depth`, `free_sized`, and TLS's distinct `last_error`) remain.
- `command_client_no_evict` and `command_client_no_touch`. Whole-tree searches found only each
  declaration/definition. The setters, CLIENT INFO's direct `ClientMeta` reads, and the hot
  `Client::no_touch` consumer remain.
- `command_metadata_arity` and `server_tail_encoding_name`. Both were zero-caller wrappers; live
  code uses `command_metadata_arity_ok`/direct metadata and file-local `encoding_name`.
- Declared-only `server_tail_config_routes_all_shards` and
  `server_tail_config_resetstat_owner`, plus their now-unneeded `Shard` forward declaration.
- `notify_retire_entry`. Exact-name search plus manual inspection of `notify.inc` through its
  `xshard.cc` inclusion found only its declaration, definition, and `IoLoop` friend. The live
  `notify_take_batch` and `notify_retire_batch_entry` path remains. The Part 2
  `notify_abort_op` candidate was not touched.

### Dead chains and write-only cold state

- `lbsignals_diff` and its declaration, plus unused file-local `rd32`. Exact-name searches found
  no consumer; capture, format, and INFO entry points remain.
- The stranded requirepass mirror: `AuthConfigSnapshot`, `Server::auth_config_snapshot`,
  `Server::live_requirepass_hash_`, and `auth_password_matches`; the dead hash parameter/stores in
  `set_auth_config` were removed with the chain. `auth.inc` is included by `xshard.cc` and
  `acl.inc` by `acl.cc`; both paths were checked. `security_flags_`, SHA-256 digesting,
  `set_auth_config`, and live `acl_authenticate` remain.
- `Server::live_obuf_replica_hard_`, `live_obuf_replica_soft_`, and
  `live_obuf_replica_seconds_`, which were stored but never loaded. CONFIG parsing/storage/echo for
  the replica class remains intact.
- `ClientMeta::tracking_bcast` and the dead `bcast` plumbing into
  `command_client_set_tracking_view`. Exact-name search found assignments only. The real tracking
  broadcast state in `tracking.cc`/`climon.inc` remains.
- `Snapshot::writer_cursor_` and `FlatStore::snapshot_records_`. The first was only reset; the
  second was reset/incremented and never read. The similarly named live
  `AofManager::writer_cursor_` remains.
- `AofReplayPlan::file_sequence` and `AofReplayPlan::valid_file_bytes`. Each default-constructed
  field had one assignment and no read; the type has no positional aggregate initialization.

### Orphaned leaf symbols

- `ascii_equal` in `t_zset.cc`, `kUnlimited`, and `RangeSpec`: one exact occurrence apiece.
- `free_raw`: one exact occurrence; removing it also removes the allocator layer's only `dallocx`
  escape and leaves the documented sized-free discipline.

### Epoll hazard removal

- `EpollSet::inited`, `fd`, `mod`, `del`, and `nevents`. The sole `EpollSet` instance calls only
  `init`, `add`, `wait`, and `event`. `mod` and `del` are removed as hazards, not merely unused
  helpers: they invite re-arming/deleting registrations despite the header's armed-once contract
  and close-based teardown rule.

## Constant-return verdicts

- `note_send_stop`: **the constant `false` was correct, not a swallowed-error defect**. Its former
  result represented whether bytes progressed, and a stopped send progresses none. Fatality uses a
  separate `send_failed_` latch, consumed by `take_send_failure()` after every epoll send boundary.
  Returning true for fatal errors would corrupt progress accounting. The function now returns
  `void`; the inert `|` and `|=` consumers are gone, with the latch path unchanged.
- `append_rdb_length`: **constant `true` was correct but the boolean contract was residue**. The
  helper performs fixed-width `vector` appends; allocation failure throws and there is no
  recoverable false path. It now returns `void`, and callers retain the real boolean checks on
  `append_bytes`/`append_rdb_string`.
- `acl_add_unique`: **constant `true` was correct but the boolean contract was residue**. Duplicate
  and newly inserted values both mean the postcondition succeeded, callers discarded the result,
  and allocation failure is exception-based. It now returns `void`.

No defect was found in these three functions, so no defect fix was mixed into the cleanup commits.

## Kept / deferred

- Added permanent comments beside `kRdbSetIntset`, `kRdbHashListpack`, `kRdbZsetListpack`,
  `kRdbListQuicklist2`, `kRdbSetListpack`, `kRdbEncInt8`, `kRdbEncInt16`, `kRdbEncInt32`, and
  `kRdbEncLzf`: they are decode-only compatibility values consumed by RESTORE for real Redis DUMP
  payloads and must not be removed merely because the encoder does not produce them.
- The complete Part 2 needs-verification tier remains. In particular, no removal was made for
  `multi_queueing`, `notify_abort_op`, `climon_monitor_clock_ms_`,
  `BlockingRegistry::remove_front_from_shard`, `ScatterState::arena_bytes`,
  `ScatterState::geo_store_distance`, or `SortSpec::source_is_list`; V3/V7 reserve those textual
  `.inc` findings for the later decision.
- `TOMO_WEDGE_FORENSICS` was moved down from the HIGH list for this pass: static analysis proves it
  inert in normal builds, but the register itself says retaining the diagnostic hook is an owner
  decision. That is not sufficient authority for deletion.
- `MigrationPlan`, `Shard::foreign_ratio`, `Shard::migration_cost_bytes`, and
  `Shard::Stats::migrated_bytes` remain together as the explicit future-LB contract from V11.
- `Op::rbuf_off` remains even though removing the dead `Rob::pinned_rbuf_off` accessor exposes a
  write-only-looking chain. Removing or moving it would go beyond the register and touch the locked
  `Op` layout, so it is a later layout decision.
- `ClientMeta::no_evict`, its setter, and CLIENT INFO flag rendering remain. Only the zero-caller
  exported getter was removed; this pass does not change the known missing eviction-path behavior.

## Validation and measurement surface

Per `LANE_RULES.md`, nothing was built and no server, test, load generator, or test script was run.
Static validation consisted of whole-tree exact-name/member-call searches, manual `.inc` inclusion
checks, per-commit diff review, and `git diff --check` against the pre-cleanup tag.

There is no intended protocol behavior change. Code-generation/measurement surfaces are:

- all parsed commands (`Op::db` prepare store removed);
- all plaintext/TLS reply sends (dead Wb counter stores removed);
- XADD/XDEL/stream trim (dead `live_entries` stores removed);
- cold AUTH/ACL config publication, CLIENT TRACKING metadata, snapshots, and AOF recovery.

If the main session validates dynamically, use at least two executors and two shards for MULTI and
script paths, with both same-owner and cross-owner keys. Cross-owner keys must be found by walking
candidates and bucketing with `DEBUG SHARD`, failing loudly if the required geometry cannot be
found. Exercise both uring and epoll send engines and plaintext/TLS where available. The ordinary
GET, SET, MGET, and MSET cells remain mandatory because the first two removals touch common prepare
and reply code even though they do not change semantics.
