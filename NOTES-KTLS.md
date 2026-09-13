# kTLS lane notes

## What was built

TLS listeners now attempt a bidirectional Linux kTLS transport by default. The IO owner performs
the OpenSSL server handshake on a non-blocking socket BIO and retries `SSL_accept` through
io_uring readiness polls. Once transmit and receive offload are both active, the connection enters
the ordinary io_uring `recv`/`send` data path: the kernel returns plaintext on receive and builds
TLS records from plaintext sends.

The existing memory-BIO engine remains available per connection. `tls-ktls no` selects it
directly. An attempted offload that engages neither direction switches the completed session to
that engine. Since Linux cannot remove a direction after installing `TLS_TX` or `TLS_RX`, a rare
partial engagement retains OpenSSL's socket BIO rather than feeding ciphertext back through an
already-active kernel record layer.

OpenSSL 3.0.13 on this host engages both directions for TLS 1.2 AES-128-GCM. For TLS 1.3 it
engages `TLS_TX` but leaves `TLS_RX` disabled. The TLS keylog callback is therefore used only
in-process to capture `CLIENT_TRAFFIC_SECRET_0`; TomoKV derives the AES-128-GCM receive key and IV
with TLS 1.3 HKDF-Expand-Label and installs `TLS_RX` with the Linux kTLS ABI. Secrets and derived
material are cleansed after use. No key material is logged.

The final handshake flight and TLS 1.3 session-ticket work are completed and the socket BIO is
flushed before raw application IO starts. kTLS `recv` errors, including the `EIO` used for
non-application records such as peer `close_notify` and KeyUpdate, follow the normal clean
connection teardown. A server close sends `close_notify` through `TLS_TX` when no send is in
flight.

TLS responses keep the existing no-borrow contract. kTLS has a separate compile-time write-back
instantiation that copies and releases a potential borrowed value, so MSG_ZEROCOPY remains
disabled without adding a per-operation mode branch. No `Op`, `Client`, or `CommandSpec` field was
added; the existing TLS sidecar holds all new per-connection state and the footprint static
assertions still pass. With `tls-port 0`, the existing `run_loop<false>` path is retained.

## Command and configuration surface

No Redis command was added. The changed command surfaces are:

- `CONFIG GET tls-*` now reports `tls-ktls`.
- `CONFIG SET tls-ktls ...` rejects the knob as boot-only, like the other TLS listener knobs.
- `INFO STATS` now reports `tls_ktls_active` and `tls_ktls_fallback`.

Knob:

- `tls-ktls yes|no`, default `yes`. `yes` attempts bidirectional offload and silently falls back
  per connection; `no` forces the existing userspace TLS engine. Any other value is rejected.

Default cipher selection prefers and currently restricts the implicit lists to
`TLS_AES_128_GCM_SHA256` for TLS 1.3 and
`ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256` for TLS 1.2. An explicitly supplied
`tls-ciphers` or `tls-ciphersuites` value replaces the corresponding default.

Counter semantics:

- `tls_ktls_active` is a current gauge of successfully handshaken, bidirectionally offloaded
  connections. It decrements at the connection sidecar's deferred-free fence.
- `tls_ktls_fallback` is cumulative and increments for every successful handshake that remains
  in a userspace mode, including `tls-ktls no`.

## Directed and sanitizer verification

Certificates were generated once with:

```sh
python3 tests/tls.py --generate /tmp/tomokv-ktls-certs
```

Release build:

```sh
make -j12 clean && make -j12
```

The release matrix used the following server and battery commands for each
`atomic_mode in 0 1` and `ktls_mode in yes no` (the `extra` argument is empty for `yes` and
`--tls-ktls no` for `no`):

```sh
taskset -c 16-19 ./build/tomokv \
  --port 7210 --tls-port 7211 --bind 127.0.0.1 \
  --shards 16 --ratio 2:2 --protected-mode no --atomic "$atomic_mode" \
  --tls-cert-file /tmp/tomokv-ktls-certs/server.crt \
  --tls-key-file /tmp/tomokv-ktls-certs/server.key \
  --tls-auth-clients no "${extra[@]}"

python3 tests/tls.py 127.0.0.1 7211 /tmp/tomokv-ktls-certs no \
  --plain-port 7210 --full --expect-ktls "$ktls_mode"
```

All four cells passed. Each tail was:

```text
  ok   concurrent plaintext and TLS clients
  ok   wrong-transport connections fail cleanly
  ok   TLS counters prove handshake/transport/zc/plain arms fired
TLS PASS (no)
```

The battery explicitly exercises TLS 1.2 and TLS 1.3, verifies RESP echo traffic, 1 MiB and
8 MiB values in both directions, cross-shard MGET ordering, graceful shutdown, torn records,
inbound and outbound mid-stream disconnects, RESET, concurrent TLS/plaintext clients, and wrong
transport negative controls. Its mechanism assertions require `tls_ktls_active > 0` in attempt
cells, `tls_ktls_active == 0` plus a rising `tls_ktls_fallback` in forced-fallback cells, and
positive no-borrow/handshake/plain-listener counters.

Release shutdown evidence for the four cells had `live_conns=0`,
`rob_not_quiesced=0`, `unsent_bytes_pending=0`, and write-back `err=0`. Attempt-mode final logs
had `ktls_active=0 ktls_fallback=0` after all connections were freed; forced-mode logs had
`ktls_active=0 ktls_fallback=14`.

ASAN build and full batteries:

```sh
make asan

ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 \
taskset -c 16-19 ./build/tomokv-asan \
  --port 7210 --tls-port 7211 --bind 127.0.0.1 \
  --shards 16 --ratio 2:2 --protected-mode no --atomic 1 \
  --tls-cert-file /tmp/tomokv-ktls-certs/server.crt \
  --tls-key-file /tmp/tomokv-ktls-certs/server.key \
  --tls-auth-clients no "${extra[@]}"

python3 tests/tls.py 127.0.0.1 7211 /tmp/tomokv-ktls-certs no \
  --plain-port 7210 --full --expect-ktls "$ktls_mode"
```

Both the attempt and forced-fallback ASAN cells passed with the same battery tail. Scanning
`/tmp/ktls-asan-{yes,no}.log` found no AddressSanitizer, LeakSanitizer, or runtime-error report.

An additional TLS 1.3 boot explicitly configured
`TLS_AES_256_GCM_SHA384`, which produces partial OpenSSL offload on this host. A 1 MiB SET/GET
round trip passed and proved `tls_ktls_active=0` with `tls_ktls_fallback>0`:

```text
PARTIAL FALLBACK PASS ('TLS_AES_256_GCM_SHA384', 'TLSv1.3', 256) b'1'
```

`python3 -m py_compile tests/tls.py`, the standalone config parser test, invalid
`--tls-ktls maybe` rejection, and `git diff --check` also passed.

The command differ is not applicable to this lane: kTLS changes the transport below RESP and the
Redis 7.4 differ harness has no TLS transport mode. The directed battery byte-checks RESP values
through both transport implementations instead.

## INDICATIVE loopback performance

Environment: server cores 16-19, client cores 20-23, 2 IO + 2 EX threads, 16 shards,
`--atomic 0`, 100,000 preloaded 32-byte values, GET-only, pipeline 128, four memtier threads with
four connections each, 15 seconds. TLS cells used TLS 1.3. The representative commands were:

```sh
taskset -c 20-23 memtier_benchmark -s 127.0.0.1 -p 7210 \
  -t 1 -c 1 --pipeline=128 --ratio=1:0 --key-pattern=R:R \
  --key-minimum=1 --key-maximum=100000 --requests=100000 \
  --data-size=32 --hide-histogram

taskset -c 20-23 memtier_benchmark "${bench_args[@]}" \
  -t 4 -c 4 --pipeline=128 --ratio=0:1 --key-pattern=R:R \
  --key-minimum=1 --key-maximum=100000 --test-time=15 \
  --data-size=32 --hide-histogram
```

For TLS, `bench_args` was
`-s 127.0.0.1 -p 7211 --tls --tls-skip-verify --tls-protocols=TLSv1.3`.

| Mode | GET/s | average | p50 | p99 |
|---|---:|---:|---:|---:|
| Plaintext | 6,759,245.99 | 0.292 ms | 0.271 ms | 0.503 ms |
| Userspace TLS (`tls-ktls no`) | 4,409,526.15 | 0.457 ms | 0.439 ms | 0.807 ms |
| kTLS | 4,669,399.20 | 0.430 ms | 0.423 ms | 0.695 ms |

INDICATIVE result: kTLS was 5.9% faster than the userspace engine and recovered 11.1% of this
run's plaintext throughput gap. This does not meet the requested large-majority bar in this
loopback cell. All four client cores were saturated, and `/proc/net/tls_stat` reported software
offload (`TlsTxDevice=0`, `TlsRxDevice=0`), so this is not a NIC-offload result. The two IO
threads' summed busy time fell from 16,010.6 ms in userspace TLS to 9,303.6 ms with kTLS, while
client saturation limited the observed throughput gain. Final verdict cells remain with the
mainline operator as requested.

## Scope decisions

- MSG_ZEROCOPY was not enabled for TLS. This was explicitly out of scope unless verified, and the
  no-borrow path is preserved for both TLS transports.
- A TLS-aware random command differ was not added because transport selection is outside RESP
  command semantics; the expanded socket-level battery is the relevant byte-comparison test.
- No hard build or dependency blocker remains. The only unmet lane target is the indicative
  loopback performance bar recorded above; the implementation, fallback, and verification work
  is otherwise complete.
