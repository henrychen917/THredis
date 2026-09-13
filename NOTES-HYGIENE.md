# TomoKV-cpp hygiene audit

## Findings

| Finding | Category | Disposition | Evidence |
|---|---|---|---|
| “save” is mutable compatibility state, not a scheduler input. | Inert CONFIG knob | **Reported.** Recommend removing the mutable surface (or returning unsupported) until a periodic-save scheduler exists; silently implementing a scheduler is a separate behavioral feature. | t_server.cc:365 registers it; server_tail.cc:667-679 calls it compatibility-only/non-boot-parsed. Live Redis 7.4 “save 1 1” advanced LASTSAVE; TomoKV did not. |
| “databases” is mutable compatibility state although the server always has one keyspace. | Inert CONFIG knob | **Reported.** Recommend making it immutable at 1 or removing it from CONFIG SET. | t_server.cc:432 is the only value registration. Live probe: CONFIG SET read back 2, while SELECT 1 still returned the single-keyspace error. |
| “proto-max-bulk-len” is mutable compatibility state but does not control the parser's 512 MiB limit. | Inert CONFIG knob | **Reported.** Recommend making it immutable or wiring exact Redis limit semantics in a feature lane. | t_server.cc:433 registers the constant; server_tail.cc:670-675 explicitly classifies it as compatibility-only. No behavioral read exists. |
| Boot parsing and CONFIG SET implement different Redis memory suffix grammars. | Duplicated logic / behavior | **Reported.** Recommend one shared parser with the boot parser's decimal k/m/g and binary kb/mb/gb grammar. | config.h:63-91 versus t_server.cc:491-506. Redis 7.4: maxmemory 1k -> 1000; TomoKV -> 1024. |
| Five INFO values are literal placeholders: aof_delayed_fsync, instantaneous_ops_per_sec, total_net_input_bytes, total_net_output_bytes, and avg_ttl. | Inert INFO fields | **Reported.** Recommend omitting unsupported telemetry or implementing it; a literal zero advertises a measurement that did not occur. | Literals at t_server.cc:1564,1609,1612,1805. Traffic control: cmdstat_set calls=3 and db0 keys=2/expires=1, while the relevant fields remained zero. |
| Every commandstats row reports literal zero timing/rejection/failure members. | Inert INFO fields | **Reported.** | t_server.cc:1799; the same live control recorded three SET calls while usec, usec_per_call, rejected_calls, and failed_calls remained zero. |
| used_memory_peak mirrors current object bytes and can decrease. | False INFO semantics | **Reported.** Recommend a real monotonic peak or omit the field. | t_server.cc:1542-1548 passes obj_bytes to dataset and peak. Live probe: peak fell from 114752 to 32 after deleting the large value. |
| CONFIG RESETSTAT captures auth_failures but INFO does not subtract the baseline. | Unwired INFO baseline | **Reported.** Recommend applying minus_baseline as for adjacent counters. | StatBaseline field at t_server.cc:1378, capture at 1412, raw output at 1676. Invalid AUTH probe: before_reset=1 after_reset=1. |
| DUMP/RESTORE does not round-trip a hash carrying field TTL metadata. | Behavioral gap exposed by dead code | **Reported.** Implement the live codec in serialize.cc, then call note_loaded_object after RESTORE so background field expiry is armed. | TomoKV: 38-byte DUMP, RESTORE rejected its version/checksum; Redis 7.4: 51-byte DUMP, RESTORE OK, HGET value. The historical note named dead dumprestore.inc; live code is serialize.cc:846-939. |
| arity.py, blockmulti.py, cmdgap.py, multires.py, and xmove.py were shipped but absent from gate.sh. | Tests never run | **Fixed.** Added all five under both atomic settings; quick/full ledgers are now 204/214. | Parent gate had no invocation. Current rows are gate.sh:147,183. Each passed release and ASAN under atomic 0 and 1 with its mechanism counter/control firing. |
| The differential suite inventory listed wiredump and climon, then the wrapper appended both again. | Duplicated test logic | **Fixed.** differ.py list-generators is now the single suite inventory. | Parent differ_gate.sh appended both names. Current discovery returns 34 names and sort/uniq reports zero duplicates. |
| src/cmd/dumprestore.inc was a second 331-line DUMP/RESTORE implementation that nothing included. | Dead code / duplicate implementation | **Fixed.** Deleted. | At 741595447^, git grep for its include returned nothing; live handlers are in serialize.cc. |
| Signed config parsing negated the unsigned representation of INT64_MIN. | Undefined behavior | **Fixed.** Handle the minimum directly and test rejection. Observable knob grammar is unchanged. | config.h:326-334; optimized and UBSAN config-parser runs pass for -9223372036854775808. |
| AOF rewrite wrappers signalled PIDs captured from the background launch although the standing rule requires socket ownership. | Stale/unsafe test scaffolding | **Fixed.** Running servers adopt the unique PID from ss; stops poll listener release. Expected corrupt-file refusals run in the foreground. | Parent wrappers assigned ACTIVE_PID from the launch shell. Both matrices pass for persist-io normal and uring after the change. |
| codex.log, two finished lane plans, and a frozen cost scratchpad were committed residue. | Stale scaffolding | **Fixed.** Deleted codex.log, PERTHREAD_SPEC.md, NOTIFY_V2_ANALYSIS.md, and tomokv-costs.conf. | codex.log was 1,580,843 bytes / 29,629 lines. Excluding historical NOTES, the other filenames had zero references. |
| docs/DESIGN-ZC.md exactly duplicated the root note; the note also named old files and a false default. | Duplicate/stale documentation | **Fixed.** Removed the duplicate and corrected the canonical root copy (zc-min=16384 and current file ownership). | Pre-change cmp returned 0; config.h and tomokv.conf both default to 16384. |
| RESP3 documentation said CLIENT TRACKING was absent; persistence docs said dir/dbfilename were live-settable. | False comments | **Fixed.** Corrected comments/config documentation. | tracking.cc and tracking.py implement tracking. t_server.cc:631-633 rejects dir/dbfilename as immutable. |
| The historical HEXPIRE note named the dead RESTORE implementation and claimed rearming the live path does not provide. | False historical note | **Fixed/documented.** Updated its file table and linked this audit's report-only gap. | Old NOTES-HEXPIRE line named dumprestore.inc; note_loaded_object call sites are only scatter_engine.inc, snapshot.cc, and aof.cc. |
| Ten directed/manual assets still have no gate path. | Tests never run | **Reported.** Highest priority is restoring the eviction driver; recovery batteries should be purpose-wired separately. | See test census below. wiredump.py is standalone-only although full differ contains an internal wiredump suite. |
| evict_battery.py documents evict_drive.sh, but that driver does not exist. | Incomplete test scaffolding | **Reported.** Reconstruct the fresh-boot driver before gate wiring. | Its header names the driver; tests/evict_drive.sh is absent. |
| The root contains 57 lane NOTES and 213,904 bytes of scratchpad; 29 NOTES have no non-NOTES reference. | Stale scaffolding | **Reported.** Do not bulk-delete: many remain the only rationale or negative-control evidence. Owner should establish an archive policy. | Counts were taken before adding this note. |
| docs/specs/AUDIT-ACL.md cites missing scratchpad/aclprobe/probe.cc. | Stale documentation/scaffolding | **Reported.** Restore the exact probe or remove the non-reproducible provenance paragraph. | The directory contains only holes.cc, which is a different Client-layout probe. |
| Canonical decimal parsing is independently implemented across many command files; stream option parsing is duplicated verbatim. | Duplicated logic | **Reported.** Consolidate only with a dedicated compatibility battery because some callers intentionally differ. | Definitions occur in string/list/zset/hash/hash-TTL/server/server-tail/slowlog/serialize/stream/xshard files; parse_i64_option exists in both stream files. |
| Feature ownership has drifted. | Structure | **Reported.** Restructuring would create broad review churn, so leave it to owners. | t_server.cc is 2,257 lines; DUMP/RESTORE code and rows are split; HLL algorithms and PF handlers are split; cmdgap mixes cluster controls and RESTORE aliasing. |
| EX loops retain a WbEngine solely for uniform stats plumbing. | Stale architectural scaffold | **Reported.** Do not change in this lane; architecture/performance ownership is fixed. | ex_loop.h:880 says it never serves there. |
| hll.cc describes itself as a representation-faithful port of Redis hyperloglog.c. | Provenance/licence risk | **Reported.** Owner must establish provenance before editing or redistributing this code; this lane did not compare or copy source. | hll.cc:1-5 explicitly uses “port”, while the project rule forbids copying the Redis 7.4 RSALv2/SSPLv1 tree. |
| Build/source inventory is complete; the two TODOs are still actionable and scoped. | Negative audit result | **Retained.** | Make expands to 30 .cc files and find reports the same 30; clean build has no warnings. TODOs concern future role flip and density work. |

## CONFIG census

CONFIG GET * emitted 62 names. Every name was traced to a boot consumer, live update application,
or the compatibility-only exceptions below.

~~~text
acl-pubsub-default
aclfile
acllog-max-len
aof-timestamp-enabled
aof-use-rdb-preamble
appenddirname
appendfilename
appendfsync
appendonly
atomic
atomic-window
auto-aof-rewrite-min-size
auto-aof-rewrite-percentage
client-output-buffer-limit
databases
dbfilename
dir
enable-debug-command
hash-max-compact-entries
hash-max-compact-value
latency-monitor-threshold
list-max-compact-entries
list-max-compact-value
maxclients
maxmemory
maxmemory-policy
maxmemory-samples
notify-keyspace-events
persist-io
protected-mode
proto-max-bulk-len
requirepass
save
script-crossshard-conflict-retries
script-crossshard-cut-slots
script-crossshard-max-bytes
script-crossshard-workbench-bytes
script-instruction-limit
set-max-compact-entries
set-max-compact-value
slowlog-log-slower-than
slowlog-max-len
stream-node-max-bytes
stream-node-max-entries
tcp-backlog
tcp-keepalive
timeout
tls-auth-clients
tls-ca-cert-dir
tls-ca-cert-file
tls-cert-file
tls-ciphers
tls-ciphersuites
tls-key-file
tls-ktls
tls-port
tls-prefer-server-ciphers
tls-protocols
tracking-table-max-keys
zc-min
zset-max-compact-entries
zset-max-compact-value
~~~

- Dead mutable compatibility values: save, databases, proto-max-bulk-len.
- Fixed compatibility constraint: aof-use-rdb-preamble is deliberately fixed to yes; boot rejects
  no and AOF uses the native snapshot preamble.
- Consumed values (58): acl-pubsub-default, aclfile, acllog-max-len, aof-timestamp-enabled,
  appenddirname, appendfilename, appendfsync, appendonly, atomic, atomic-window,
  auto-aof-rewrite-min-size, auto-aof-rewrite-percentage, client-output-buffer-limit, dbfilename,
  dir, enable-debug-command, all eight compact-encoding values, latency-monitor-threshold,
  maxclients, maxmemory, maxmemory-policy, maxmemory-samples, notify-keyspace-events, persist-io,
  protected-mode, requirepass, the four script-crossshard values, script-instruction-limit,
  slowlog-log-slower-than, slowlog-max-len, both stream-node values, tcp-backlog, tcp-keepalive,
  timeout, all eleven TLS values, tracking-table-max-keys, and zc-min.

Boot-only parser entries absent from the CONFIG table were checked as well: port, bind, unixsocket,
l3-domains, place, ratio, shards, shard-home, no-pin, hash, user, load, and lru-clock-shift all have
concrete startup/topology/store consumers. No dead boot-only Config field was found.

Live evidence:

~~~text
save oracle before=1787887833 after=1787887835 advanced=yes
save target before=1787887833 after=1787887833 advanced=no
memory oracle 1k=1000
memory target 1k=1024
databases target config=2 select1=ERR this server supports a single keyspace; only SELECT 0 is valid
~~~

## INFO census

INFO ALL emitted 178 names after PING and SET. Every emitted name was checked against its format
argument and the counter's increment/write site. The report-only exceptions are exactly:

- Literal placeholders: aof_delayed_fsync, instantaneous_ops_per_sec, total_net_input_bytes,
  total_net_output_bytes, and the avg_ttl component of db0.
- False accumulator: used_memory_peak.
- Incomplete RESETSTAT application: auth_failures.
- Dynamic cmdstat rows have a real calls count but literal timing/rejection/failure members.

The remaining census covers every server/client/memory field; all snapshot and AOF fields;
keyspace, ACL, tracking, blocking, pub/sub, TLS, slowlog, latency, script/function, and atomic
families; the LB section; dynamic commandstats rows; and db0. The exact inventory command was:

~~~sh
redis-cli -h 127.0.0.1 -p 7600 INFO ALL | tr -d '\r' |
  sed -n 's/^\([^#][^:]*\):.*/\1/p' | sort -u
~~~

The inventory contained 178 distinct names. No other literal or unconsumed formatter argument was
found. The falsifiable live tail was:

~~~text
acl_access_denied_auth
acl_access_denied_channel
acl_access_denied_cmd
acl_access_denied_key
acl_perm_retired
acl_pubsub_clients_killed
allocator_allocated
allocator_resident
aof_auto_rewrite_backoff_skips
aof_auto_rewrite_triggers
aof_base_size
aof_control_frames_deferred
aof_current_size
aof_delayed_fsync
aof_enabled
aof_fsyncs
aof_groups_committed
aof_groups_skipped_on_replay
aof_history_unlinks
aof_last_bgrewrite_status
aof_last_write_status
aof_pending_rewrite
aof_records_written
aof_replayed_records
aof_rewrite_base_size
aof_rewrite_completions
aof_rewrite_consecutive_failures
aof_rewrite_failures
aof_rewrite_in_progress
aof_rewrite_requests
aof_rewrite_scheduled
aof_send_gate_waits
arch_bits
atomic_chain_max
atomic_cleanup_fast
atomic_cleanup_slow
atomic_commit_holds
atomic_commit_windows
atomic_credit_debt
atomic_credit_pool
atomic_entries
atomic_exec_order_holds
atomic_exec_read_cuts
atomic_fanout_cuts
atomic_gauge_underflows
atomic_groups
atomic_inflight
atomic_localfast
atomic_pending_entries
atomic_predecessor_reads
atomic_promotions
atomic_read_cuts_held
atomic_records_freed
atomic_scan_order_holds
atomic_window_stalls
auth_failures
blocked_clients
blocking_waiters
client_no_touch_ops
client_output_buffer_limit_disconnections
client_pause_holds
client_scatter_io_responses
client_scatter_requests
cmdstat_info
cmdstat_ping
cmdstat_set
connected_clients
db0
evicted_keys
expired_hash_fields
expired_keys
function_calls
function_generation
function_readonly_rejections
function_thread_rebuilds
hash_field_expires
instantaneous_ops_per_sec
keyspace_hits
keyspace_misses
keyspace_rehashes
latency_events_recorded
lb_ex_avg_depth
lb_ex_busy_frac
lb_ex_full_events
lb_ex_ns_per_op
lb_ex_threads
lb_foreign_op_frac
lb_io_avg_depth
lb_io_busy_frac
lb_io_full_events
lb_io_ns_per_op
lb_io_threads
lb_ratio_star_io_frac
lb_total_threads
mem_allocator
monitor_clients
monitor_feed_lines
multiplexing_api
notify_events_dropped
notify_events_fired
number_of_cached_scripts
number_of_functions
number_of_libraries
plain_connections_received
pubsub_blobs
pubsub_channels
pubsub_deliveries
pubsub_delivery_batches
pubsub_home_entries
pubsub_inflight
pubsub_patterns
pubsub_pending_commands
pubsub_subscriptions
pubsubshard_channels
pubsubshard_subscriptions
rdb_bgsave_in_progress
rdb_last_save_time
redis_mode
redis_version
rejected_connections
script_apply_owner_tasks
script_chunk_cache_hits
script_chunk_cache_misses
script_crossshard_activations
script_crossshard_window_refusals
script_effect_writes
script_failed_after_effects
script_flush_generation
script_group_aborts_oom
script_group_commits
script_group_occ_giveups
script_group_occ_retries
script_intents_live
script_interpreter_builds
script_keys_armed
script_keys_released
script_readonly_rejections
script_run_attempts
script_stage_owner_tasks
script_staged_bytes_total
script_validate_owner_tasks
script_write_tickets_forced
slowlog_batches_timed
slowlog_entries_recorded
slowlog_escalations
snapshot_cuts_armed
snapshot_cuts_waited
snapshot_groups_drained
snapshot_preimages
tls_ciphertext_input_bytes
tls_ciphertext_output_bytes
tls_connections_received
tls_current_connections
tls_handshakes_completed
tls_handshakes_failed
tls_handshakes_started
tls_ktls_active
tls_ktls_fallback
tls_plaintext_input_bytes
tls_plaintext_output_bytes
tls_want_read
tls_want_write
tls_zc_suppressed
tomokv_version
total_commands_processed
total_connections_received
total_net_input_bytes
total_net_output_bytes
tracking_clients
tracking_invalidations
tracking_total_items
tracking_total_keys
tracking_total_prefixes
uptime_in_seconds
used_memory
used_memory_dataset
used_memory_peak
used_memory_rss
~~~

~~~text
INFO before DEL:
used_memory_dataset:114752
used_memory_peak:114752
instantaneous_ops_per_sec:0
total_net_input_bytes:0
total_net_output_bytes:0
cmdstat_set:calls=3,usec=0,usec_per_call=0.00,rejected_calls=0,failed_calls=0
db0:keys=2,expires=1,avg_ttl=0
INFO after DEL:
used_memory_dataset:32
used_memory_peak:32
cmdstat_set:calls=3,usec=0,usec_per_call=0.00,rejected_calls=0,failed_calls=0
db0:keys=1,expires=1,avg_ttl=0
auth_failures before_reset=1 after_reset=1
~~~

## Test-file census

The calibration examples expireindex.py, borrow_registry.py, and xshard_dispatch_scale.sh were
already wired in the current parent. aof_frames.py is indirectly executed by aof_frame_order.py;
aof_rewrite.py and aof_rewrite_triggers.py are driven by matrix wrappers; differ.py is driven by
differ_gate.sh; niclib.sh and gate_refs.txt support optional full-tier NIC rows.

The five accidentally omitted batteries are now wired. These remain outside the gate:

- Recovery/manual: edgetime_persist.sh, hexpire_persist.sh, flush_capture.py.
- Incomplete: evict_battery.py (missing its documented driver).
- Slow/campaign/performance: lru_slow.sh, writer_atomic.py, writer_atomic_campaign.py,
  benchfeat.py, broaden_bench.cc.
- Standalone overlap: wiredump.py. Full differ has an internal wiredump suite, but the standalone
  boot/CLI path is not invoked.

## Dead-code, structure, and comment sweep

- Every remaining .inc is textually included; dumprestore.inc alone had no include.
- All 30 production .cc files appear in Make's SRC, and Make names no missing file.
- The clean warning-enabled build emitted no warning, so there is no compiler-reported internal
  unused function. Registry/table-only reachability was preserved.
- Debug hooks were mapped to atomfix, atomic/script/EXEC, snapshot/AOF, and edgetime tests; none was
  deleted merely because it is table- or test-reached.
- The only word-boundary TODOs are TODO(flip) in thread.h and TODO(density) in kvobj.h. Both still
  describe real future work and were retained.
- Redis-factual comments were searched with rg. The proven false RESP3 claim was corrected. The
  HLL provenance claim and RESTORE/HFIELDTTL behavior are reported rather than silently rewritten.

## Verification

The reserved tests/gate.sh wrapper was not executed. Its quick-tier batteries were invoked manually
on cores 128-143 and ports 7600-7601. Every server stop used the unique PID resolved from its
listening socket, and every port was checked released before reuse.

Build/static commands:

~~~sh
make clean && make -j8
g++ -std=c++20 -O1 -g -fsanitize=address -march=native -pthread -I. \
  src/main.cc src/net/tls.cc src/cmd/*.cc src/snapshot/*.cc src/persist/*.cc \
  -o /tmp/tomokv-hygiene-asan -luring -pthread -lssl -lcrypto
g++ -std=c++20 -O2 -I. tests/config_parser_test.cc \
  -o /tmp/tomokv-hygiene-config-parser-test
/tmp/tomokv-hygiene-config-parser-test
python3 tools/gen_acl_categories.py --redis-root /tmp/claude-1000/redis74 \
  --check src/cmd/acl_categories_generated.h
bash -n tests/gate.sh tests/differ_gate.sh \
  tests/aof_rewrite_matrix.sh tests/aof_rewrite_trigger_matrix.sh
python3 tests/differ.py --list-generators
~~~

Common release feature/debug boot:

~~~sh
taskset -c 128-135 ./build/tomokv --port 7600 --bind 127.0.0.1 \
  --shards 16 --ratio 4:4 --protected-mode no --atomic "$AT" \
  --enable-debug-command yes
~~~

Directed loops, each for AT=0 and AT=1:

~~~sh
for t in s6 multi_exec blocking blockmulti stream streamgroups pubsub lua_scripting \
  scriptsurf limits resp3 bitfield dumprestore zsetops geo climon climon2 tracking \
  hexpire servertail lcs concur edgeproto edgeenc edgetime arity cmdgap; do
  python3 tests/$t.py 127.0.0.1 7600
done

for t in lbsignals slowlog atomfix scriptatomic execatomic execiso execfix multires \
  session_monotonic xacct xmove xscript; do
  python3 tests/$t.py 127.0.0.1 7600
done
~~~

The five newly wired batteries also ran against /tmp/tomokv-hygiene-asan under both atomic
settings. Selected tails:

~~~text
ARITY: 63 checks, 0 failures -> PASS
BLOCKMULTI PASS: 141 checks; collection_fired=12 wait_deadlines_fired=2 wait_disconnect_fired=1
CMDGAP PASS: 22 checks; 4 inventory rows, 3 cluster-disabled replies, 1 restore alias fired
multires atomic=0: window stays shut -> PASS
multires atomic=1 release: hazard window opened 116x -> PASS
multires atomic=1 ASAN: hazard window opened 127x -> PASS
PASS xmove atomic=0
PASS xmove atomic=1
~~~

Broader quick-tier evidence:

~~~text
TORTURE PASS
RYOW PASS
idle_cpu_jiffies_5s=83
ATOMIC_TORN PASS (OFF controls exposed anomalies; ON controls reported zero)
ATOMIC_RYOW PASS (stalls=21, torn=0)
feature atomic=0 passed=27
feature atomic=1 passed=27
debug atomic=0 passed=12
debug atomic=1 passed=12
EXPIREINDEX PASS (sidecar population=1048576)
BORROW-REGISTRY PASS (~1993 live borrows, growth ratio=1.010)
XSHARD-DISPATCH-SCALE PASS (best 128t/4t excess ratio=0.960, limit=1.20)
notify: ok (notify_events_fired=1608)
~~~

Purpose-boot matrix commands:

~~~sh
PERSIST_IO=normal GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_matrix.sh
PERSIST_IO=normal GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_trigger_matrix.sh
PERSIST_IO=uring  GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_matrix.sh
PERSIST_IO=uring  GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_trigger_matrix.sh
~~~

Other purpose-booted battery invocations (each boot used the knobs documented by gate.sh):

~~~sh
python3 tests/xscript.py 127.0.0.1 7600 off|limit|window
python3 tests/expireindex.py 127.0.0.1 7600
python3 tests/borrow_registry.py 127.0.0.1 7600
XDS_PORT=7600 XDS_CPUS=128-143 XDS_BIN=./build/tomokv \
  bash tests/xshard_dispatch_scale.sh
python3 tests/dumprestore.py 127.0.0.1 7600 prepare_restart|verify_restart
python3 tests/auth.py 127.0.0.1 7600 gatepass
python3 tests/acl.py 127.0.0.1 7600 ACL_FILE
python3 tests/debug.py 127.0.0.1 7600
python3 tests/snap_cut_battery.py 7600 save|verify_cut
python3 tests/snap_cut_battery.py 7600 atomic_groups FILE mset 5
python3 tests/snap_cut_battery.py 7600 atomic_groups FILE exec 3
python3 tests/snap_typed_roundtrip.py 7600 build_save|verify
python3 tests/snap_typed_race.py 7600 race|verify
python3 tests/aof.py 127.0.0.1 7600 populate|loadaof|verify|snapshot STATE
python3 tests/aof_torn_group.py 127.0.0.1 7600 prepare|verify|scan PATH
python3 tests/aof_fsync.py 127.0.0.1 7600 populate|verify STATE POLICY COUNT
python3 tests/aof_frame_order.py 127.0.0.1 7600 APPENDONLY_DIR
python3 tests/tls.py 127.0.0.1 7601 TLS_DIR yes|optional|no --plain-port 7600
~~~

~~~text
snapshot matrix normal: PASS (cut/reload, MSET+EXEC groups, typed 40/40, preimage race)
snapshot matrix uring: PASS (same matrix)
AOF normal/uring x atomic 0/1: 369 written, 369 replayed, byte-exact PASS
AOF group normal/uring: 10 ordered GCMT commits, recovery PASS
AOF always/everysec normal+uring: 512 keys, 528 records recovered
AOF no-sync normal+uring: syncs=0 waits=0
AOF-off controls: data absent after unclean stop/recovery, no appendonlydir
AOF REWRITE MATRIX PASS: atomic=0/1 stages=3 corruptions=5 (normal and uring)
AOF REWRITE TRIGGER MATRIX PASS: atomic=0/1 live-config info auto backoff recovery (both)
AOF FRAME ORDER PASS: large_records=128, control frames present, deferrals>0, interleaves=0
TLS PASS (yes)
TLS PASS (optional); KTLS_LIVE_OK tls_ktls_active:2
TLS PASS (no); accepts=32 freed=32 zc_suppressed=21
~~~

No byte-differ suite was added or run: this lane intentionally changed no command semantics, and
the INT64_MIN fix preserves the same rejected knob result while removing undefined behavior. The
differential-runner change was inventory-only and was checked to return 34 unique suites. No
NIC/wire benchmark was run.
