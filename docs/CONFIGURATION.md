# Configuration

The startup source of truth is [Config, parse_config_args, and validate_config](../src/core/config.h),
with file selection in [main.cc](../src/main.cc). The runtime table and setters
are in [t_server.cc](../src/cmd/t_server.cc). Defaults below describe the current
source, including options that are absent from `--help`.

## Syntax and runtime changes

Use separate CLI tokens, such as `--maxmemory 1gb`. `--name=value` is not
accepted. In a file, write `maxmemory 1gb`. Supply the file as the first bare
argument or with `--conf FILE`; the server does not automatically search for
`tomokv.conf`. File values are applied first, CLI values second.

File parsing supports single and double quotes. Double quotes recognize escaped
characters and `\xHH`; single quotes recognize `\'`. A `#` begins a comment only
as the first non-whitespace character of a line. Do not append inline comments
to settings. Empty values need `""`. There is no `include` directive. The loader
reads through a fixed 4096-byte line buffer, so keep configuration lines short.

For example, save this as `local.conf`:

```conf
bind 127.0.0.1
port 6379
thread-mode 2s
ratio 2:2
shards 16
atomic 1
save ""
```

```sh
./build/tomokv --conf local.conf
```

This example requires at least four allowed CPUs. It enables grouped multi-key
execution and disables periodic snapshots; those are explicit overrides.

In the tables:

- **Live** means `CONFIG GET` and `CONFIG SET` are implemented.
- **Boot/GET** means startup-only and reported by `CONFIG GET`; runtime setting
  is rejected.
- **Boot** means no entry in the runtime CONFIG table.
- **B** means behavior: command semantics, accepted inputs, authentication,
  persistence, or an execution/backend selection changes.
- **T** means tuning: placement policy, representation, sampling, or copying
  changes. Timing, resource use, and diagnostics can still differ.
- **R** means a Redis configuration name/grammar is supported, with differences
  stated explicitly. It does not imply identical internals or mutability.
  **Tomo** marks TomoKV-specific options.

`u32` is `0..4294967295`; `u64` is `0..18446744073709551615`;
`i64` is `-9223372036854775808..9223372036854775807`. Unless a row says otherwise,
unsigned numeric settings use decimal digits with no sign. Memory values accept
bare bytes or `b`, decimal `k/m/g`, or binary `kb/mb/gb`, case-insensitively.
`1m` is 1,000,000 bytes; `1mb` is 1,048,576 bytes. Memory parsing uses unsigned
64-bit arithmetic; overflowing inputs are not reliably rejected (see FINDINGS).
There is no universal meaning for `0` or `-1`: use the rule in the individual row.

## Threads, placement, and execution

| Name | Default | Valid values and meaning | Runtime | Origin / effect |
| --- | --- | --- | --- | --- |
| `thread-mode` | `2s` | `2s` / `split`: separate IO and executor threads; `1s` / `fused`: every selected thread does both. | Boot/GET | Tomo / B |
| `ratio` | derived | Positive `io:ex` logical-thread counts, spread over L3 domains. Split only; total must fit allowed CPUs and the 128-thread bound. | Boot | Tomo / T |
| `place` | derived | Comma-separated `ifid@CPU,ex@CPU,...`, with decimal allowed CPU IDs. In fused mode the role labels only select CPUs. | Boot | Tomo / T |
| `shards` | `-1` | `-1` derives `min(8 × initial executor count, 256)`; explicit `1..256` fixes the shard count. Resolved before loading persistence. | Boot | Tomo / B |
| `--no-pin`; file `pin no` | pinning on | Valueless CLI switch disables CPU pinning. File `pin yes` leaves the default on; it does not undo an earlier `pin no`. | Boot | Tomo / T |
| `lb` | `1` | `0` or `1`. Enables key and client balancing together. `0` allocates no LB policy, sampling/census sidecars, or observation windows. | Boot/GET | Tomo / T |
| `flip-auto` | `0` | `0` or `1`. Enables the automatic IO/executor role controller; `1` requires split mode. `0` leaves its dynamic state and fingerprint writer inactive. Manual `FLIP` is separate. | Boot/GET | Tomo / T |
| `flip-work-window` | `100` | `u32`. Mean parse passes per fingerprint sample. A selected pass contributes all its commands; `1` samples every pass, `0` disables fingerprinting. Only active with `flip-auto 1`. | Boot/GET | Tomo / T |
| `read-local` | `0` | `0` or `1`. Eligible GET/MGET execution on the connection thread; effective only with `thread-mode 1s` and `x-overlap 0`. Other modes accept `1`, log a notice, and use owner tasks. `0` allocates no read-local state. | Boot/GET | Tomo / B |
| `atomic` | `0` | `0` or `1`. Enables epoch-MVCC for ordinary grouped multi-shard mutations. `EXEC` and staged scripts may force groups even at `0`; disabling does not discard live group state. | Live | Tomo / B |
| `hash` | `mix64` | `mix64` or `siphash` (SipHash-1-2). Boot-randomized hash material controls routing and store lookup; persistence recovery restores saved hash material. | Boot | Tomo / T |
| `zc-min` | `16384` | `u32` bytes. `0` disables borrowed-value single-key GET replies; positive values set the minimum string length. MGET gather separately uses `min(zc-min, 1024)`, including when zero; see below. | Live | Tomo / T |
| `script-instruction-limit` | `100000` | `u64` Lua VM instructions, checked at 1000-instruction hook intervals. `0` is unlimited. Exceeding the budget aborts the activation; this is not Redis's `lua-time-limit` or `busy-reply-threshold`. | Boot/GET | Tomo / B |

Without placement overrides, split mode divides the allowed CPUs evenly, with
IO taking the extra CPU. Under SMT it divides complete sibling pairs, with both
logical CPUs of a pair in the same role. Fused mode selects all allowed CPUs.
Split needs both roles; fused needs at least one thread. At most 128 threads may
be provisioned. If the allowed topology contains complete SMT pairs, selected
CPUs must have their reciprocal siblings selected, and split role counts must
be even. CPU topology and validation are in
[topology.h](../src/base/topology.h) and [placement.h](../src/core/placement.h).

`ratio` and `place` conflict within one input source. A CLI placement setting
replaces the other kind of placement supplied by the file. Automatic shard
resolution uses the initial executor count; later role changes do not resize
the shard set. Explicit shard counts are needed for recovery across different
initial thread geometries.

`zc-min 0` does not disable every internal borrow: MGET gather uses a zero copy
cutover in that case and borrows nonempty, non-integer string payloads. This is
distinct from disabling the single-key GET borrowed-reply path. The exact branch
is in [scatter_engine.inc](../src/cmd/scatter_engine.inc).

The only environment override read by server code is **`TOMOKV_L3_DOMAINS`**
(default unset). It supplies L3 groups when discovery is unsuitable:
comma-separated domains, with `-` for a range and `+` joining ranges inside a
domain, for example `0-3+8-11,4-7+12-15`. CPUs must be unique and in the allowed
affinity mask. This is a boot-only TomoKV placement override, not a CONFIG key.

### Study options

These are accepted options, despite being omitted from help and the reference
configuration. They change scheduling, not the number of architectural stages.

| Name | Default | Valid values and meaning | Runtime | Origin / effect |
| --- | --- | --- | --- | --- |
| `x-overlap` | `0` | `0`: ordinary rotation. `1`: interleaved IO/writeback schedule in split, interleaved IO/executor/writeback work in fused. `2`: gated unified three-way work schedule, fused only. | Boot/GET | Tomo / T |
| `x-ex-sched` | `0` | `0`: FIFO executor batch order. `1`: reorder eligible batch runs by connection-head rank and static command-length class, preserving dependency boundaries. | Boot/GET | Tomo / T |

Fused `x-overlap 1` and `2` require `net-io uring`; split rejects `2` and permits
its `1` schedule with either backend. `2` emits an experimental-schedule warning.
Read-local admission requires `x-overlap 0`. Implementations are in
[iopipe_pipeline.h](../src/core/iopipe_pipeline.h),
[genthread_pipeline.h](../src/core/genthread_pipeline.h),
[io_loop.h](../src/core/io_loop.h), and [ex_loop.h](../src/core/ex_loop.h).

## Networking and client limits

| Name | Default | Valid values and meaning | Runtime | Origin / effect |
| --- | --- | --- | --- | --- |
| `bind` | `127.0.0.1` | One numeric IPv4 address; `0.0.0.0` binds all IPv4 interfaces. No hostname, address list, or IPv6 listener parsing. | Boot | R, narrower / B |
| `port` | `6379` | `0..65535`; `0` disables plaintext TCP. | Boot | R / B |
| `unixsocket` | unset | Filesystem socket path; unset/empty disables it. The listener owns path cleanup and rejects unsuitable existing paths. | Boot | R / B |
| `net-io` | `uring` | `uring` or `epoll`, case-insensitive. Epoll makes no io_uring syscalls and derives syscall persistence. Backend selection is explicit, not an automatic fallback. | Boot/GET | Tomo / B |
| `maxclients` | `10000` | `1..4294967295`. Acceptance limit; startup may reduce it to fit the open-file limit. Independent acceptors make this a pre-allocation safety check, not an exact reservation boundary. | Live | R / B |
| `timeout` | `0` | `0..2147483647` seconds. `0` disables idle expiry. Normal clients close after strictly more idle seconds; blocked and RESP2 subscriber clients are exempt. | Live | R / B |
| `tcp-keepalive` | `300` | `0..2147483647` seconds. Applied to new TCP clients; `0` skips keepalive setup. | Live, new clients | R / T |
| `tcp-backlog` | `511` | `0..2147483647`, passed to `listen`; the kernel may cap it. | Boot/GET | R / T |
| `client-output-buffer-limit` | see below | One or more `CLASS HARD SOFT SECONDS` clauses. Class is `normal`, `pubsub`, `replica` or `slave`. HARD/SOFT use memory syntax; SECONDS is `0..2147483647`. | Live | R, accounting differs / B |
| `proto-max-bulk-len` | `536870912` | Memory value from `1048576` through `4294901759` bytes (`UINT32_MAX - 65536`). Bounds request bulk lengths. | Live | R, bounded by 32-bit slices / B |
| `databases` | `1` | Only `1` accepted, including by CONFIG SET. The sole valid protocol database index is zero. | Live, fixed value | R, restricted / B |

Output buffer defaults are `normal 0 0 0`, `replica 268435456 67108864 60`, and
`pubsub 33554432 8388608 60`. Repeated clauses merge by class. A zero byte
threshold disables that check. The hard bound triggers at or above the limit;
the soft bound must remain exceeded for strictly more than the configured
seconds. Staged reply buffers and borrowed segments count toward usage.
`replica`/`slave` round-trips as `slave` but has no enforcement target because no
replication connection class exists. See [conn.h](../src/net/conn.h).

## Authentication and TLS

| Name | Default | Valid values and meaning | Runtime | Origin / effect |
| --- | --- | --- | --- | --- |
| `requirepass` | unset/empty | Password for the default user; empty removes that password requirement. Named users use ACL rules. | Live | R / B |
| `protected-mode` | `1` / `yes` | Startup: `0`, `1`, `no`, `yes` in lowercase. CONFIG SET also accepts case-insensitive words. Controls the unauthenticated remote-client protection check. | Live | R / B |
| `enable-debug-command` | `no` | `no`, `yes`, or `local` in lowercase. `local` permits loopback and Unix-socket peers. | Boot/GET | R / B |
| `aclfile` | unset/empty | ACL file path used at boot and by ACL LOAD/SAVE. Empty disables file-backed ACL loading/saving. | Boot/GET | R / B |
| `user` | no definitions | Repeatable `user NAME RULE...`; uses the ACL rule parser. CLI form is `--user NAME RULE...`, ending at the next `--` option. | Boot; runtime rules via ACL | R / B |
| `acl-pubsub-default` | `resetchannels` | `resetchannels` or `allchannels`, case-insensitive. Default channel permissions for ACL users. | Live | R / B |
| `acllog-max-len` | `128` | `u64` entries per IO owner's ACL log; `0` retains none. | Live | R, per-owner bound / B |
| `tls-port` | `0` | `0..65535`. `0` allocates no TLS context or connection state. Nonzero enables a separate TLS listener and must differ from nonzero `port`. | Boot/GET | R / B |
| `tls-cert-file` | unset/empty | PEM server certificate-chain path; required when TLS is enabled. | Boot/GET | R / B |
| `tls-key-file` | unset/empty | PEM private-key path; required when TLS is enabled. | Boot/GET | R / B |
| `tls-ca-cert-file` | unset/empty | Trusted CA file. | Boot/GET | R / B |
| `tls-ca-cert-dir` | unset/empty | OpenSSL CA directory. | Boot/GET | R / B |
| `tls-auth-clients` | `yes` | Case-insensitive `yes` (require certificate), `optional` (verify if supplied), `no` (do not require one). `yes`/`optional` require a CA file or directory. | Boot/GET | R / B |
| `tls-protocols` | empty: TLS 1.2 + 1.3 | Space-separated case-insensitive `TLSv1`, `TLSv1.1`, `TLSv1.2`, `TLSv1.3`; quote the list. Availability also depends on OpenSSL policy. | Boot/GET | R / B |
| `tls-ciphers` | empty: see below | OpenSSL cipher-list string for TLS through 1.2. Explicit nonempty input replaces the built-in list. | Boot/GET | R, Tomo default / B |
| `tls-ciphersuites` | empty: `TLS_AES_128_GCM_SHA256` | OpenSSL TLS 1.3 ciphersuite list. | Boot/GET | R, Tomo default / B |
| `tls-prefer-server-ciphers` | `no` | Case-insensitive `yes` or `no`. | Boot/GET | R / B |

`aclfile` and inline `user` definitions cannot be combined. ACL parsing,
selectors, command categories, key patterns, channel patterns, and password
rules are in [acl.cc](../src/cmd/acl.cc). Quote shell-sensitive CLI ACL tokens;
file quoting follows the configuration grammar above.

The default TLS <=1.2 cipher list is
`ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256`.
All TLS settings are immutable after boot. The implementation attempts kTLS
automatically and falls back to OpenSSL; there is no `tls-ktls` switch. See
[tls.cc](../src/net/tls.cc).

## Persistence

| Name | Default | Valid values and meaning | Runtime | Origin / effect |
| --- | --- | --- | --- | --- |
| `dir` | `.` | Nonempty directory for snapshots and AOF storage. | Boot/GET | R / B |
| `dbfilename` | `dump.tomo` | Nonempty filename without `/`; snapshot save target under `dir`. Does not select automatic boot loading. | Boot/GET | R, Tomo format/default / B |
| `load` | unset | Nonempty explicit TomoKV snapshot path. Read before listeners start, unless AOF recovery takes precedence. | Boot | Tomo / B |
| `save` | `3600 1 300 100 60 10000` | Seconds/changes pairs; seconds `1..u64 max`, changes `u64`. A satisfied clause schedules a snapshot. Empty string disables the schedule, not manual SAVE/BGSAVE. | Live | R / B |
| `appendonly` | `no` | Case-insensitive `yes` or `no`. Enables TomoKV AOF recording and recovery at boot. | Boot/GET | R name, boot-only / B |
| `appendfsync` | `everysec` | Case-insensitive `always`, `everysec`, `no`. Controls AOF fsync policy: durable reply gating, periodic syncing, or no periodic fsync. | Live | R / B |
| `appendfilename` | `appendonly.aof` | Nonempty name without `/`; prefix for AOF manifest/log files. | Boot/GET | R / B |
| `appenddirname` | `appendonlydir` | Nonempty name without `/`, below `dir`. | Boot/GET | R / B |
| `auto-aof-rewrite-percentage` | `100` | `u32` growth percentage; `0` disables automatic rewriting. | Live | R / T |
| `auto-aof-rewrite-min-size` | `67108864` | Memory value; minimum size for automatic rewrite consideration. | Live | R / T |
| `aof-use-rdb-preamble` | `yes` | Only case-insensitive `yes`. Fixed declaration: the base is a **TomoKV snapshot**, not Redis RDB. | Live, fixed value | R name, different format / B |
| `aof-timestamp-enabled` | `no` | Startup accepts case-insensitive `yes`/`no`; CONFIG SET additionally accepts `0`/`1`. Enables timestamp records. | Live | R / B |

Within a source, repeated nonempty `save` directives append clauses; an empty
directive clears them. The first `save` supplied by the CLI replaces the file's
schedule. Repeating `--save` on the CLI follows the same rule.

Snapshots need explicit `--load` on restart. AOF recovery reads the manifest
under `dir/appenddirname`, its snapshot base if present, and its incremental
files. Recovery requires the saved shard count and restores saved hash material.
Changing CPU geometry with `shards -1` can therefore make a saved file refuse
to load. See [snapshot.cc](../src/snapshot/snapshot.cc),
[aof.cc](../src/persist/aof.cc), and [main.cc](../src/main.cc).

## Memory and encodings

| Name | Default | Valid values and meaning | Runtime | Origin / effect |
| --- | --- | --- | --- | --- |
| `maxmemory` | `0` | Memory value; `0` disables eviction-limit work. A positive budget is divided by shard count; it does not cover all process allocations or cap RSS. | Live | R, accounting differs / B |
| `maxmemory-policy` | `noeviction` | `noeviction`, `allkeys-lru`, `allkeys-lfu`, `allkeys-random`, `volatile-lru`, `volatile-lfu`, `volatile-random`, `volatile-ttl`; case-insensitive. | Live | R / B |
| `maxmemory-samples` | `5` | `1..64` candidates. | Live | R, bounded / T |
| `hash-max-compact-entries` | `512` | `u32` entries before expansion. | Live | Tomo / T |
| `hash-max-compact-value` | `64` | `u32` bytes for the field/value size threshold. | Live | Tomo / T |
| `list-max-compact-entries` | `4294967295` | `u32` entries; default imposes no smaller entry-count threshold. | Live | Tomo / T |
| `list-max-compact-value` | `8192` | `u32` bytes of **aggregate list payload**, not maximum element length. | Live | Tomo / T |
| `set-max-compact-entries` | `128` | `u32` members before expansion. | Live | Tomo / T |
| `set-max-compact-value` | `64` | `u32` member bytes. | Live | Tomo / T |
| `zset-max-compact-entries` | `128` | `u32` members before expansion. | Live | Tomo / T |
| `zset-max-compact-value` | `64` | `u32` member bytes. | Live | Tomo / T |
| `stream-node-max-bytes` | `4096` | `u32` macro-node rollover budget; `0` disables this axis. See runtime-range caveat below. | Live | R / T |
| `stream-node-max-entries` | `100` | `u32` macro-node entry budget; `0` disables this axis. See runtime-range caveat below. | Live | R / T |

Compact limits govern subsequent representation decisions, not an immediate
rewrite of the dataset. Exceeding a limit promotes a compact collection; there
is no automatic conversion back to compact form. Zero is a literal compact
limit, not "unlimited." These names are not aliases for Redis's listpack or
quicklist settings. Stream limits instead control when a new macro-node starts.
See [typeval.h](../src/store/typeval.h) and the corresponding `t_*.cc` handlers.

LRU uses a fixed five-bit clock with 256-second buckets; LFU also uses five-bit
metadata. The policy names match Redis, but the internal precision and memory
accounting do not. No `lru-clock-shift` runtime setting remains.

## Notifications, tracking, and observation

| Name | Default | Valid values and meaning | Runtime | Origin / effect |
| --- | --- | --- | --- | --- |
| `notify-keyspace-events` | empty | Any combination of `K E g $ l s h z x e t m d n A`, without spaces. Empty disables notifications. `K`/`E` select channel forms; the other letters select event classes. | Live | R / B |
| `tracking-table-max-keys` | `1000000` | `u64` keys per IO owner's tracking table; `0` means unlimited. Allocates tracking state only when clients enable tracking. | Boot/GET | R, per-owner and boot-only / B |
| `slowlog-log-slower-than` | `10000` | `-1..i64 max` microseconds. `-1` disables slow logging; `0` logs every eligible execution. | Live | R / B |
| `slowlog-max-len` | `128` | `u64` log entries; `0` retains none. | Live | R / B |
| `latency-monitor-threshold` | `0` | `u32` milliseconds; `0` disables monitoring. See runtime-range caveat below. | Live | R / B |

`A` includes ordinary mutation classes, expiration, eviction, and the accepted
module flag `d`, but excludes key misses `m` and new-key events `n`. Accepting
`d` does not enable modules. A delivery route still needs `K` or `E`. For
example, `Eg` selects generic key-event notifications. Tracking and scheduled
save observations use internal observer bits, separate from these configured
pub/sub flags. See [notify.h](../src/cmd/notify.h),
[tracking.cc](../src/cmd/tracking.cc), and [slowlog.cc](../src/cmd/slowlog.cc).

## Configuration administration and limits

`--conf FILE` and a bare first file argument are startup controls, default unset.
`--help` is a valueless startup control that prints usage and exits; it does not
start the server. Neither is a CONFIG key. They are TomoKV invocation controls,
with the bare-file convention also familiar from Redis.

Runtime examples, with `redis-cli` connected to the server:

```text
CONFIG GET thread-mode read-local atomic
CONFIG SET atomic 1
CONFIG SET save ""
CONFIG SET maxmemory 1gb maxmemory-policy allkeys-lru
```

Multi-pair CONFIG SET validates values before dispatching owner updates. Boot-only
keys in the runtime table return an immutable-parameter error. Keys such as
`port`, `bind`, `ratio`, `place`, `shards`, `hash`, `load`, and `user` are not in
that table; use startup configuration or the relevant ACL command instead.

**CONFIG REWRITE does not preserve the complete startup configuration.** It
replaces the loaded file with the runtime CONFIG table, omitting boot-only keys
that are absent from that table, inline ACL users, and comments. It also writes
nonempty string values without escaping/quoting, so values with spaces or quote
characters may not reload. It errors if no configuration file was loaded.
Maintain a complete configuration file directly when preserving startup settings
matters. These implementation defects are recorded in
[FINDINGS](FINDINGS.md#configuration-round-tripping).

**Runtime-range caveat:** CONFIG SET currently accepts `u64` values for the two
`stream-node-max-*` options and `latency-monitor-threshold`, then narrows them
to 32 bits. Startup rejects values beyond `u32`. Keep them within `u32`; the
larger values can make CONFIG GET disagree with effective behavior. See
[FINDINGS](FINDINGS.md#configuration-validation).

## Build controls and derived limits

The [Makefile](../Makefile) exposes these build controls; none is a runtime CONFIG
key:

| Variable | Default | Meaning / values |
| --- | --- | --- |
| `CXX` | `g++` | C++ compiler command; the normal build contains a GCC-specific `--param`. |
| `CXXFLAGS` | `-std=c++20 -O2 -g -Wall -Wextra -march=native -pthread` | Compiler flags. Replacing them must retain the required language and threading support. |
| `LDLIBS` | `-luring -pthread`, with Makefile additions `-lssl -lcrypto` | Link libraries; the link recipe additionally supplies allocator libraries and `-lm`. A command-line override needs to include the complete required list. |
| `JE` | `1` | Exact `1` selects jemalloc; use `0` for the libc allocator path. Other values also take the non-jemalloc Makefile branch. |
| `JEDIR` | `/home/user/Projects/refs/jemalloc/_install` | jemalloc prefix when the system header is absent; expects `include/` and `lib/libjemalloc.a`. |

These change build/allocator behavior; compiler flags also control optimization
and instrumentation. Make does not record variable changes in dependency files;
clean before changing them. The sanitizer and negative-control targets are
separate research/test builds, not runtime settings.

The following formerly configurable choices are now fixed or derived in code:

| Choice | Current rule | Source |
| --- | --- | --- |
| Atomic admission window | `min(16 × resolved shards, 1024)` | [server.h](../src/core/server.h) |
| Script staging bytes | 4 MiB if boot `maxmemory` is zero; otherwise `max(4 MiB, min(maxmemory / shards / 16, 64 MiB))` | [server.h](../src/core/server.h) |
| Script workbench / conflict retries / cut slots | Twice staging bytes / 8 / 4 per IO | [server.h](../src/core/server.h) |
| Persistence IO engine | uring with `net-io uring`; syscalls with epoll | [config.h](../src/core/config.h) |
| Read-local capture, filter, interleave, recycling | Enabled as part of the armed local-read implementation | [ex_loop.h](../src/core/ex_loop.h), [flatstore.h](../src/store/flatstore.h) |
| LB sampling, imbalance band, move cap, cooldown | Derived from traffic, quiet jitter, and transfer duration; observation tick 1000 ms, decision spans three ticks | [weighted_lb.h](../src/core/weighted_lb.h) |
| ROB / embedded string / scatter inline / common arena | 64 ops / 192 bytes / 1024 bytes / 16384 bytes | [rob.h](../src/net/rob.h), [kvobj.h](../src/store/kvobj.h), [scatter_engine.inc](../src/cmd/scatter_engine.inc), [xshard.h](../src/cmd/xshard.h) |

The headers also retain compile-time research selectors
`TOMO_TTL_DEADLINE_SIDECAR` (`0` default, `1` prototype) and
`TOMO_READ_LOCAL_SET_TAX_VARIANT` (`0` default, `1`/`3` experimental in-place
overwrite; `2` is rejected). They are not runtime choices. The nondefault SET
variants conflict with the stated immutable-reader contract and are identified
in [FINDINGS](FINDINGS.md#reader-contract); the normal Makefile build leaves
them off.
