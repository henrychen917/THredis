# AUDIT-TLS — TLS integration for TomoKV-cpp (perthread / pure 2s)

Governing audit. Read before writing any TLS code in `/home/user/Projects/tomokv-cpp-perthread`.
No repository was modified in producing this.

**Sources read (all first-hand):**

| Tree | Path | Verified |
|---|---|---|
| tomokv | `/home/user/Projects/tomokv-cpp-perthread/src/{net,core,cmd,exec}` | local, read |
| redis | `/home/user/Projects/redis/src/{tls.c,connection.c,connection.h,connhelpers.h,config.c,iothread.c}` (8.9.241) | local, read |
| valkey | `/home/user/Projects/valkey/src/{tls.c,config.c,io_threads.c,networking.c}` (unstable, `8cd535ddb`, tls.c = 2100 lines) | local, read + spot-verified |
| dragonfly | `/home/user/Projects/dragonfly/src/facade/{tls_helpers.cc,dragonfly_connection.cc,dragonfly_listener.cc}` | local, read + spot-verified |
| helio (dfly's io layer) | `util/tls/*`, `util/fibers/uring_socket.cc` | **NOT local** — `git submodule status` returns `-3d8b4ece14ed62cb516e36afbc8947c0cc392826` (leading `-` = never checked out). Citations are to upstream `romange/helio @ 3d8b4ec`. Run `git submodule update --init --recursive` before treating them as reproducible. |

Two design claims below were proved by compiling probes rather than asserted from memory
(§2.2, §2.3). Probe sources are in the scratchpad next to this file.

---

## 0. Corrections to the framing — read this first

The brief describes the tomokv io path as "io_uring multishot recv, credit-based sends". Both
descriptions are off, and **both corrections change the TLS verdict**, so they lead.

**0.1 — Recv is single-shot, one in flight per connection. Multishot is accept-only.**

`arm_recv` prepares a plain `io_uring_prep_recv` into a caller-chosen address
(`src/core/io_loop.h:225-238`, the prep at `:234`), guarded by `recv_armed_` so exactly one is
outstanding (`src/net/conn.h:275-276`). The only multishot op in the tree is accept
(`io_loop.h:217`, `io_uring_prep_multishot_accept`); `grep -rn multishot src/` returns four hits and
all four are accept. There are no provided buffers and no buffer rings.

This matters enormously. Dragonfly's TLS cannot use multishot recv, and helio says why in a comment
at `util/tls/tls_socket.cc:559-568` (upstream): the TLS engine's input buffer has fixed capacity, so
the reader must *control the read size* — a "pull" model — whereas multishot + provided buffers is a
"push" model where the kernel decides. Dragonfly enforces the exclusion at
`src/facade/dragonfly_connection.cc:3709-3711` (`if (up->BufRingEntrySize(kRecvSockGid) > 0 &&
!is_tls_)`), and the flag help at `:150`/`:153` marks IoLoopV2 as "for **non-TLS** ... connections".

**TomoKV is already in the pull model.** The single largest architectural concession that TLS
forces on Dragonfly costs tomokv nothing, because tomokv has not yet spent the multishot/provided-
buffer budget. TLS here does not remove a capability that exists; it constrains one that does not.

**0.2 — Sends are not credit-based; they are one-in-flight with a frontier cursor.**

The send-side flow control is four separate mechanisms, none of them a credit:

| Mechanism | Site | Value |
|---|---|---|
| one send outstanding per socket | `conn.h:375-376`, `wb.h:127` | boolean `send_inflight_` |
| iovec window per submission | `conn.h:518` | `kMaxSendIov = 16` |
| byte window per submission | `conn.h:519` | `kMaxSendBytes = 0x7ffff000` |
| serves per loop pass | `io_loop.h:1003` | `kServeBudget = 16` |

The `(head_, offset_)` pair in `SegmentQueue` (`conn.h:216-219`) is the resume frontier after a short
write. The only thing resembling a credit is the **parse-side** ROB window, `kRobWindow = 64`
(`conn.h:58`) — in-flight ops per connection, not bytes on a socket.

The distinction is load-bearing for TLS: §2.5 shows that OpenSSL's `WANT_WRITE` maps exactly onto
`send_inflight_`, and that the byte cursors (`wsent_`, `SegmentQueue::offset_`) are the site of the
single worst hazard in the whole integration.

**0.3 — The zero-copy send is a borrow, not `MSG_ZEROCOPY`.**

`DESIGN-ZC.md:16-18` states it: "no application-level copy of the value bytes... This does not use
kernel `MSG_ZEROCOPY`." The sendmsg iovec points into live FlatStore memory (`wb.h:145`,
`io_uring_prep_sendmsg`), and the borrow is released back to the owning executor on completion
(`wb.h:180-181`). Nothing about it survives contact with an encrypting transport (§2.8).

---

## 1. (a) Mechanisms per server

### 1.1 Redis — fd-BIO, readiness-driven, crossed-want state machine

**BIO.** Redis hands OpenSSL the socket directly: `SSL_set_fd(conn->ssl, conn->c.fd)` at
`src/tls.c:514` (accept path) and `:722` (connect path). OpenSSL issues its own `read(2)`/`write(2)`.
The only `BIO_s_mem` in the file is for PEM-encoding a peer cert (`:1297`), not for transport.

**Consequence — the crossed-want problem.** Because readiness and logical operation are decoupled,
redis must track "a read wants writability" and vice versa, as two flags
(`src/tls.c:443-444`):

```c
#define TLS_CONN_FLAG_READ_WANT_WRITE   (1<<0)
#define TLS_CONN_FLAG_WRITE_WANT_READ   (1<<1)
```

set in `updateStateAfterSSLIO` (`:576-577`), consumed by `updateSSLEvent` (`:616-631`) which
re-derives the epoll mask from `read_handler || WRITE_WANT_READ` / `write_handler ||
READ_WANT_WRITE`, and cleared ad hoc at three separate points inside the event handler
(`:792`, `:798`, `:805`). The header comment at `:423-436` is an explicit statement that this exists
solely because of the fd-BIO choice.

**Return-code funnel.** `handleSSLReturnCode` (`:528-553`) maps `SSL_ERROR_WANT_{READ,WRITE}` to a
`WantIOType` out-param and returns 0; everything else is an error. `updateStateAfterSSLIO`
(`:562-594`) turns want into `errno = EAGAIN; return -1` so callers see ordinary socket semantics.
`SSL_ERROR_ZERO_RETURN` and `SSL_ERROR_SYSCALL`-with-`errno==0` become `CONN_STATE_CLOSED` (`:582`);
everything else `CONN_STATE_ERROR`.

**The pending-data problem.** `SSL_read` can decrypt more than the caller asked for, leaving
plaintext inside OpenSSL that no socket event will ever announce. Redis keeps a per-event-loop list
of such connections (`tlsPendingAdd`, `:635-643`, list stored in `conn->c.el->privdata[1]`), checked
via the vtable hooks `has_pending_data` / `process_pending_data` (`:1356-1357`,
`connection.c:161-190`) and drained from `beforeSleep` (`server.c:1968`) and from
`IOThreadBeforeSleep` (`iothread.c:849-857`). `aeSetDontWait` is set when any remain
(`iothread.c:857`).

**IO-thread model — connection migration.** Redis 8 gives each io thread its own `aeEventLoop` and
moves a whole connection between them via two vtable methods (`:1338-1339`):
`connTLSUnbindEventLoop` (`:1051-1065`) tears down both events and removes the pending-list node;
`connTLSRebindEventLoop` (`:1067-1077`) asserts full quiescence
(`serverAssert(!conn->c.el && !conn->c.read_handler && !conn->c.write_handler &&
!conn->pending_list_node)`) and re-derives pending state with `if (el && SSL_pending(conn->ssl))
tlsPendingAdd(conn)`. Thread safety is by exclusive ownership.

**Write path.** `connTLSWrite` = `SSL_write` + funnel (`:1078-1086`). `connTLSWritev` (`:1088-1123`)
is the interesting one: if the total is under `NET_MAX_WRITES_PER_EVENT` it **memcpy-coalesces every
iovec into one VLA and encrypts once** (`:1116-1122`), on the stated reasoning that "it is worth
doing more memory copies in exchange for fewer system calls". Above that threshold it issues one
`SSL_write` per iovec. This is redis conceding, in code, that TLS already forces copies.

**Handshake cost, admitted in config.** `max-new-connections-per-cycle` defaults to **10**
(`config.c:3445`); `max-new-tls-connections-per-cycle` defaults to **1** (`config.c:3446`), consumed
at `tls.c:839`. A 10× throttle is redis's own operational estimate of a handshake's cost relative to
an `accept()`.

**Port semantics.** TLS is a separate listener on a separate port. `tls-port` defaults to 0
(`config.c:3509`); the listener is bound in `server.c:3286-3294` and switched live by `applyTLSPort`
(`config.c:2902-2907`). Plain and TLS coexist; running TLS on the canonical port is spelled
`port 0` + `tls-port 6379` (`redis.conf:193-196`). There is **no ClientHello sniffing and no
dual-stack port** anywhere in the tree.

### 1.2 Valkey — same lineage, three divergences worth stealing

Valkey's `tls.c` is 2100 lines against redis's 1414 and is still fd-BIO/readiness-driven. Three
divergences are real improvements.

**(i) Want-flags cleared at the point they are set.** Valkey deleted the `WantIOType` enum;
`handleSSLReturnCode(conn, ret)` sets the flags itself after an unconditional
`clearTLSWantFlags(conn)` (`valkey/src/tls.c:1130-1145`), and `registerSSLEvent(conn)` reads them
(`:1200-1212`). Redis's three scattered ad-hoc clears (`redis/src/tls.c:792,798,805`) are exactly
the shape that rots. Valkey also tightened the readiness predicate to require the co-handler
(verified locally, `valkey/src/tls.c:1251-1252`):

```c
int need_read  = conn->c.read_handler  || (conn->c.write_handler && (conn->flags & TLS_CONN_FLAG_WRITE_WANT_READ));
int need_write = conn->c.write_handler || (conn->c.read_handler  && (conn->flags & TLS_CONN_FLAG_READ_WANT_WRITE));
```

Redis (`:619-620`) omits the co-handler term and so registers interest for handlers that have since
been cleared — spurious wakeups.

**(ii) `SSL_pending` sampled at read time, list mutated separately.** `updateSSLPendingFlag`
(verified locally, `valkey/src/tls.c:1239-1245`) caches `SSL_pending(ssl) > 0` into
`TLS_CONN_FLAG_HAS_PENDING` immediately after every `SSL_read` (`:1757`, `:1850`, `:1875`);
`updatePendingData` (`:1223-1237`) only touches the list. This split is what makes threaded TLS
reads possible at all: the pending *observation* must happen on the thread that decrypted, the
pending *list* must be mutated by whoever owns the readiness state.

**(iii) IO-thread model — operation offload, not connection migration.** Valkey keeps exactly one
`aeEventLoop` (`server.el`) and one thread allowed to touch it. Worker threads run `SSL_read`,
`SSL_write`, `SSL_accept` and `SSL_pending` sampling; everything event-loop-shaped is deferred
behind a gate (verified locally, `valkey/src/tls.c:1214-1225`, `:1248`):

```c
static void postPoneUpdateSSLState(connection *conn_, int postpone_mask) { ... }
static void updatePendingData(tls_connection *conn) { if (conn->flags & TLS_CONN_FLAG_POSTPONE_UPDATE_STATE) return; ... }
static void updateSSLEvent   (tls_connection *conn) { if (conn->flags & TLS_CONN_FLAG_POSTPONE_UPDATE_STATE) return; ... }
```

The gate is raised before enqueueing (`io_threads.c:532/593/832`, each with a symmetric rollback if
the push fails) and lowered on completion (`networking.c:6571-6581`), after which `updateSSLState`
(`tls.c:1282-1291`) replays `updateSSLEvent` + `updatePendingData`. `SSL_accept` on a worker only
*records* its outcome into two new flags, `TLS_CONN_FLAG_ACCEPT_{ERROR,SUCCESS}` (`:1054-1055`);
`TLSHandleAcceptResult` (`:1263-1280`) does the event-loop mutation later on the main thread.
Publication is by an explicit `atomic_thread_fence(memory_order_release)` at `io_threads.c:796`.

**Config divergences.** Valkey adds `tls-auto-reload-interval` (int, default **0** = off, seconds —
verified locally, `valkey/src/config.c:3574`), driving a background `BIO_TLS_RELOAD` job that
SHA-256-fingerprints the cert files and swaps `SSL_CTX*` under a mutex; in-flight connections are
safe because `SSL_new` up-refs the context. It adds a `URI` variant to `tls-auth-clients-user` and
adopts `max-new-tls-connections-per-cycle` (default 1, `config.c:3516`). It **lacks**
`tls-expected-peer-name`, which redis has (`redis/src/config.c:3530`, implementation
`redis/src/tls.c:947-994` using `X509_VERIFY_PARAM_set1_host` + `X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS`).

**The `SSL_write` retry contract.** Verified locally at `valkey/src/tls.c:1674-1685`:

```c
/* In case when last write failed due to some internal reason, retry has to provide
 * at least the same amount of bytes (https://docs.openssl.org/master/man3/SSL_write).
 * If that condition is not met, OpenSSL will return "SSL routines::bad length". */
if (data_len < conn->last_failed_write_data_len) return -1;
ret = SSL_write(conn->ssl, data, data_len);
conn->last_failed_write_data_len = ret <= 0 ? data_len : 0;
```

and honoured again in `connTLSWritev` at `:1707` and `:1738-1745`. This is the single most
transferable correctness fact in either tree, and it collides head-on with tomokv's re-windowing
send loop — see §2.5 and §6.

### 1.3 Dragonfly / helio — memory-BIO pair, engine/socket split, pull model

*(helio citations are upstream `romange/helio @ 3d8b4ec`; the submodule is not checked out locally.
The `src/facade/*` citations were verified locally.)*

**BIO — a memory BIO pair, with zero-copy peek/commit.** `util/tls/tls_engine.cc:41-62`:

```cpp
Engine::Engine(SSL_CTX* context) : ssl_(::SSL_new(context)) {
  SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE);
  SSL_set_mode(ssl_, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  SSL_set_mode(ssl_, SSL_MODE_RELEASE_BUFFERS);
  ::BIO* int_bio = 0;
  BIO_new_bio_pair(&int_bio, 0, &external_bio_, 0);
  BIO_up_ref(int_bio);                 // rbio == wbio, refcount 2
  SSL_set0_rbio(ssl_, int_bio);
  SSL_set0_wbio(ssl_, int_bio);
}
```

Both size arguments are `0` = OpenSSL's bio-pair default (17 KB/direction); helio does not tune it.
The socket layer only ever touches `external_bio_`, through four primitives that hand out **interior
pointers** rather than copying (`tls_engine.cc:73-106`): `PeekOutputBuf` → `BIO_C_NREAD0`,
`ConsumeOutputBuf` → `BIO_nread`, `PeekInputBuf` → `BIO_nwrite0`, `CommitInput` → `BIO_nwrite`.
There is a dedicated regression test named `BIO_s_bio_ZeroCopy` (`tls_engine_test.cc:211`).

**Engine/socket split.** `Engine` (`tls_engine.h:15-141`) is a pure in-memory state machine — no fd,
no proactor, no syscalls. Its four op-codes (`tls_engine.h:19-39`) are the whole vocabulary:

| Code | Value | Meaning |
|---|---|---|
| `EOF_GRACEFUL` | −1 | peer sent `close_notify` (`SSL_ERROR_ZERO_RETURN`) |
| `EOF_ABRUPT` | −2 | FIN without `close_notify`, or protocol error |
| `NEED_READ_AND_MAYBE_WRITE` | −3 | `SSL_ERROR_WANT_READ` |
| `NEED_WRITE` | −4 | `SSL_ERROR_WANT_WRITE` |

Mapped in exactly one function, `Engine::ToOpResult` (`tls_engine.cc:228-278`). The awkward name of
−3 is deliberate, and the comment at `tls_engine.h:28-34` is the design rule worth memorising:

> We use BIO buffers, therefore any SSL operation can end up writing to the internal BIO and result
> in success, even though the data has not been flushed to the underlying socket. As a result, we
> must flush output buffer (if `OutputPending() > 0`) before we do any Socket reads.

**Want-* onto the proactor.** `TlsSocket::HandleEngineOp` (`tls_socket.cc:493-513`) is two lines:
`NEED_READ_AND_MAYBE_WRITE → HandleUpstreamRead()`, `NEED_WRITE → MaybeSendEngineOutput()`. The
flush-then-read dance is unconditional at the top of `HandleUpstreamRead` (`:408-453`). There are no
readiness masks to fix up, no crossed-want flags, and no `updateSSLEvent` analogue anywhere.

**Handshake.** Synchronous and fiber-blocking: `TlsSocket::Accept` (`:127-166`) loops
`Handshake() → MaybeSendEngineOutput() → HandleEngineOp()` until success. The flush happens *before*
returning on failure so the peer actually receives the alert. Cost is one fiber parked on the
connection's own proactor.

**Buffers.** BIO pair 17 KB/direction, fixed, no growth, no spill. Write-batch scratch is a
1392-byte stack buffer (`tls_socket.cc:336`), sized "sufficiently smaller than the usual MTU (1500)
and a multiple of 16" (`:334-336`). Practical consequence: **every upstream TLS read is capped at
~17 KB** regardless of caller buffer size, because `PeekInputBuf()` is what gets handed to
`next_sock_->Recv` (`:432-437`).

**No zero-copy sends, and TLS collapses scatter-gather.** No `IORING_OP_SEND_ZC` / `sendmsg_zc` /
`MSG_ZEROCOPY` anywhere in helio at this revision. `PushUserDataToEngine` (`tls_socket.cc:331-379`)
either passes one large iovec to `SSL_write` or **memcpy-coalesces small iovecs into the 1392-byte
stack buffer** (`:351-356`); the upstream write is then always a single contiguous region taken from
`PeekOutputBuf()` (`:457`, `:469`). Zero-copy is structurally impossible: the ciphertext must be
materialised somewhere.

**Port semantics — no `tls-port`.** Dragonfly has one global `--tls` switch
(`src/facade/dragonfly_listener.cc:29`, verified locally, and note it ships with *no help text*).
Every listener gets the same treatment via `ReconfigureTLS` (`:161-163`, `:250-258`). The only
per-listener exception is `--no_tls_on_admin_port` (`:30`), applied at `ApplyTlsCtx` (`:276-299`).
The `tls_auth_clients` config is **dead code wrapped in `#if 0`** (`:51-67`).

**ClientHello sniffing before OpenSSL.** Verified locally,
`src/facade/dragonfly_connection.cc:1055-1080`: dragonfly reads exactly 5 bytes on the *raw* socket,
validates the TLS record header (`buf[0] != 0x16 || buf[1] != 0x03 || buf[2] < 0x01 || buf[2] >
0x03`), replies `-ERR Bad TLS header, double check if you enabled TLS for your client.` on mismatch,
and only then constructs the `TlsSocket`, injecting the 5 sniffed bytes into the input BIO via
`InitSSL(ssl_ctx_, buf)`. The stated motivation (`:1044-1048`) is cost: reject zombie connections
"cheaply on the raw socket instead of tying up an OpenSSL context and handshake state machine that
will never complete." Same rationale drives `TCP_DEFER_ACCEPT` at `dragonfly_listener.cc:206-219`.

**kTLS.** Present only as a *feasibility probe in tests* — `tls_socket_test.cc:222-243` probes
`setsockopt(fd, IPPROTO_TCP, TCP_ULP, "tls", ...)`, and `:288-329` asserts
`BIO_get_ktls_send(SSL_get_wbio(ssl)) == 1` under `SSL_OP_ENABLE_KTLS`, skipping where unavailable.
Never used in production, and structurally incompatible with the bio-pair design, which is exactly
the trade this audit names in §2.9.

**Perf claims.** None. There are no TLS benchmarks and no quantified overhead numbers anywhere in
dragonfly's `docs/`, `tests/` or `tools/`. What exists is architectural and points one way: TLS opts
out of the fast path (`docs/pub-sub.md:287` lists IoLoop v1 as "Default for Redis, TLS"; multishot
excluded at `dragonfly_connection.cc:3710`). Observability instead of measurement:
`tls_handshakes_total{status=started|completed}` (`src/server/metrics.cc:137-140`), `tls_bytes`
(`:376`), `tls_accept_disconnects` → `listener_accept_error_total` (`:346`).

### 1.4 Convergence table

| Axis | redis | valkey | dragonfly/helio | tomokv should |
|---|---|---|---|---|
| BIO | fd (`SSL_set_fd`, tls.c:514) | fd | **memory pair** (`BIO_new_bio_pair`) | **memory pair** |
| Crossed want-flags needed | yes (2 flags, 3 clear sites) | yes (1 clear site) | **no** | **no** |
| Readiness bookkeeping | per-el epoll mask | per-el, gated | none | none |
| Pending-plaintext list | per-event-loop list | one global list | implicit (engine holds it) | implicit |
| Recv model | readiness/pull | readiness/pull | **pull, multishot excluded** | pull (already is) |
| Threading | conn migrates between els | ops offload, el pinned to main | fiber per conn on its proactor | **conn owned by one io thread for life** |
| Handshake | async via event handler | offloaded to io thread | sync, fiber-blocking | sync-ish on the owning io thread |
| Port | separate `tls-port` | separate `tls-port` | global `--tls`, no tls-port | **separate `tls-port`** |
| Pre-handshake sniff | no | no | **yes, 5 bytes** | later (§2.7) |
| Scatter-gather under TLS | coalesced by memcpy | coalesced, retry-aware | coalesced into 1392B | coalesced |
| kTLS | none | none | test probe only | **named later path** |

Dragonfly is the only one of the three whose io model resembles tomokv's, and it is the only one
that chose a memory BIO. That is not a coincidence; it is the same forcing function (§2.2).

---

## 2. (b) The TomoKV design

### 2.1 Where the seam goes

**Above the engine, below the parser, inside the io thread.** Concretely: a TLS connection changes
what `arm_recv` writes into and what `WbEngine::pump` submits, and changes nothing else. The
parser, the ROB, the dispatch path, the executors, FlatStore and the retire path do not learn that
TLS exists.

That is achievable here in a way it is not in redis or valkey, because of one property tomokv
already has and they do not: **one io thread owns a connection's recv side, send side, parse state
and ROB drain for the connection's entire life** (`conn.h:1-14` header, `io_loop.h:318` "we are the
sender, for life"). Every mechanism redis and valkey built to make OpenSSL safe under threading —
event-loop rebinding (`redis/src/tls.c:1051-1077`), the postpone gate
(`valkey/src/tls.c:1214-1225`), the ACCEPT_SUCCESS/ACCEPT_ERROR deferral (`valkey tls.c:1054-1055`),
the release fence at `io_threads.c:796` — solves a problem tomokv does not have. An `SSL*` is not
thread-safe; a tomokv connection is single-threaded by construction. **Do not port any of that
machinery.** Porting it would be pure cost.

Proposed files:

```
src/net/tls_ctx.h    SSL_CTX construction, config validation, protocol/cipher application.  ~200 LOC
src/net/tls_conn.h   TlsConn: the per-connection engine. BIO pair, op-code funnel, handshake,
                     feed/drain, the plaintext/ciphertext cursor pair.                      ~320 LOC
```

`tls_conn.h` is deliberately shaped like helio's `Engine`: **no fd, no ring, no syscalls.** It takes
and returns buffers and op-codes. Every `io_uring` call stays in `io_loop.h`/`wb.h`. This is the
property that makes it unit-testable without a socket and is worth the discipline.

### 2.2 Memory BIO vs fd BIO — verdict, and the proof

**Verdict: memory BIO pair (`BIO_new_bio_pair`). An fd BIO is disqualified, not merely worse.**

An fd BIO means OpenSSL performs the syscalls. In a completion-based ring that forces one of two
bad shapes:

1. `IORING_OP_POLL_ADD` to learn readiness, then let OpenSSL do a blocking-mode-emulating
   `read(2)`/`write(2)`. That is io_uring used as an epoll replacement — two kernel entries where
   there was one, plus it re-imports redis's entire crossed-want state machine (`redis/src/tls.c:423-436`
   is the design comment; `:596-631` is the machinery). It also destroys the value of
   `IORING_SETUP_DEFER_TASKRUN` (`uring.h:57`), because the completions that matter no longer flow
   through the ring.
2. Run OpenSSL on a blocking fd on a separate thread. Contradicts the pure-2s ruling outright.

A memory BIO inverts the ownership: **we do the syscalls, OpenSSL does the crypto.** `arm_recv`
still submits exactly one recv; `pump` still submits exactly one send. The io_uring shape is
unchanged. That is why it fits.

Two further reasons specific to this tree:

- **The crossed-want problem disappears.** With a memory BIO there is no readiness registration to
  fix up, so there is nothing to cross. `WANT_READ` means "the input BIO is short of ciphertext" —
  and `flush_ready` already re-arms a recv on every active conn every pass (`io_loop.h:881`), so the
  condition is already continuously satisfied. `WANT_WRITE` means "the output BIO is full", which is
  precisely the existing `send_inflight_` condition (§2.5). Redis's two flags, three clear sites and
  `updateSSLEvent` (~60 lines) reduce to *zero* lines here.
- **The ciphertext ingress can be genuinely zero-copy**, via `BIO_nwrite0`/`BIO_nwrite`.

**Probe (run, not assumed).** `scratchpad/biopair_probe.c`, OpenSSL 3.0.13, confirms the exact
io_uring pattern is legal:

```
BIO_nwrite0 contiguous=32768 ptr=0x5649a209bf80
peer wrote 6, ext read=6 (SSLOUT)               <- intervening opposite-direction traffic
BIO_nwrite committed=10 ptr=0x5649a209bf80 same_as_p=1
internal read=10 payload='CIPHERTEXT'
```

That is: reserve with `BIO_nwrite0` → hand the pointer to `io_uring_prep_recv` → let unrelated
`SSL_write`/`BIO_read` traffic run on the *other* direction while the recv is in flight → commit the
CQE's byte count with `BIO_nwrite` → the pointer is unchanged and OpenSSL reads the bytes correctly.
The two directions of a bio pair are separate ring buffers, so a send in flight cannot invalidate a
recv reservation. This is the load-bearing safety property of the whole design and it now has a
receipt.

**Probe 2 — ring geometry and backpressure.** `scratchpad/biopair_wrap.c`, 4096-byte ring:

```
empty ring                   nwrite0=  4096
3000 buffered                nwrite0=  1096      <- contiguous run, not total free
after partial drain (wrap)   nwrite0=  1096      <- wrap: free space is split
after filling contiguous run nwrite0=  2500
RING FULL                    nwrite0=    -1      <- NOT 0; BIO_should_retry()==8
```

Three rules fall out, all directly analogous to existing tomokv mechanics:

| BIO fact | tomokv analogue |
|---|---|
| `BIO_nwrite0` returns the *contiguous* run, which can be short when the ring wraps | a short recv; loop around, same as a short `recv` today |
| a full ring returns **−1**, not 0 | `read_space()` returning `nullptr` (`conn.h:266`) — "do not read right now" |
| the ring never grows or moves | strictly safer than `rbuf_`, which can `realloc` (`conn.h:263`) |

**Sizing.** Do **not** take helio's default of 17 KB. `kRecvChunk` is 16 KB (`io_loop.h:45`) and
`zc_min` is 16384 (`config.h:61`), so a 17 KB ring would give a short contiguous run on almost every
wrap. Size both directions **64 KB** (`BIO_new_bio_pair(&internal, 65536, &external, 65536)`), which
keeps the common contiguous run ≥ `kRecvChunk` and holds four maximum TLS records. That is 128 KB of
fixed per-connection ciphertext staging — see §6 for the memory consequence, which is the largest
single cost of this design.

### 2.3 Handshake and connection state — where it lives, and the footprint lock

`Client` is footprint-locked at 1984 bytes by a `static_assert` (`conn.h:538`), and that assert is
signed against a 64-core A/B. **A naive `TlsConn* tls_` breaks it.**

Verified: compiling `conn.h` standalone today yields `sizeof(Client)=1984 alignof=64`
(`scratchpad/tls_layout_probe.cc`). Reading the declaration order at `conn.h:486-489`:

```c++
int       fd_   = -1;          // offset 0
uint32_t  rlen_ = 0;           // offset 4
uint32_t  rpos_ = 0;           // offset 8
                               // offset 12: 4 bytes of PADDING before the 8-aligned rbuf_
char*     rbuf_ = nullptr;     // offset 16
```

**Put a `uint32_t tls_slot_` in that hole.** `kNoTls = UINT32_MAX`; the heavyweight `TlsConn` lives
in a per-io-thread slab indexed by the slot. Three properties, all of which matter:

- `sizeof(Client)` is unchanged, so `conn.h:538` keeps passing and no re-A/B is owed.
- `is_tls()` is a compare against a word already resident on the hottest cache line — the same line
  as `fd_`, `rlen_`, `rpos_`, `rbuf_`, which every recv and send pass touches anyway.
- The TLS state does **not** land on the `alignas(64)` executor-facing tail (`conn.h:525-531`).
  Putting it there would pull that line to the io thread on every recv, which is exactly the cost
  the comment at `conn.h:417-419` records paying once already.

The implementer must add `static_assert(offsetof(Client, rbuf_) == 16)` (or equivalent) so a future
reorder cannot silently grow the struct.

**Handshake driving.** Do it helio's way (`tls_socket.cc:127-166`), adapted to completions rather
than fibers. The connection enters a `Handshaking` state at accept; `flush_ready` phase 1 already
visits every active conn every pass (`io_loop.h:851-900`), which is the natural driver:

```
on_accept -> adopt_client -> if tls: TlsConn::begin(); state = Handshaking; arm_recv(); mark_active()
each pass  -> if Handshaking:
                op = ssl->handshake()                 // SSL_accept
                flush output BIO to the socket ALWAYS, before anything else   <-- helio's rule
                op == 1              -> state = Connected; parse_and_dispatch may now run
                op == NEED_READ..    -> nothing; a recv is already armed
                op == NEED_WRITE     -> nothing; the flush above submitted it
                op < 0 (EOF/error)   -> close_client()
```

The unconditional flush-first is non-negotiable and is the one rule helio documents twice
(`tls_engine.h:28-34`, `tls_socket.cc:408-415`). Skipping it on the error path means the peer never
receives the TLS alert and sees a bare connection reset instead of a diagnosable failure.

`parse_and_dispatch` (`io_loop.h:357`) must not be reachable while `Handshaking`. The cleanest gate
is at the top of `on_recv` (`io_loop.h:342-354`), which is already the single place a recv
completion turns into parse work.

**Teardown.** `close_client` (`io_loop.h:928-971`) needs a `close_notify` attempt (`SSL_shutdown`)
before `::shutdown(fd, SHUT_RDWR)` at `:942`, and `TlsConn` must be released on the *deferred-free*
path in `reap_dead` (`io_loop.h:976-995`), not at close — a send CQE can still be outstanding, and
its iovecs point into the output BIO. This is exactly the invariant `reap_dead` already enforces for
borrows (`:985`, `c->send_inflight() || c->recv_armed()`); TLS adds a third thing pinned by the same
condition, and no new mechanism.

### 2.4 Recv path

Today (`io_loop.h:225-238`):

```c++
char* dst = c->read_space(kRecvChunk, avail, c->rob().quiesced());
if (!dst) return;
io_uring_prep_recv(s, c->fd(), dst, avail, 0);
```

Under TLS the destination becomes the input BIO reservation, and **the backpressure gate splits in
two**:

| Gate | Plain today | TLS |
|---|---|---|
| may I arm a recv? | is there `rbuf_` space (`read_space` ≠ null) | is there **BIO** space (`BIO_nwrite0` > 0) |
| may I produce plaintext? | n/a (kernel writes it) | is there `rbuf_` space — `SSL_read` into `read_space()` |

Both gates are needed. Arming recvs without the second gate buffers unbounded *ciphertext* while the
ROB refuses to drain; dropping the first gate corrupts the BIO.

```
arm_recv(TLS):     n = BIO_nwrite0(ext, &p);   if (n <= 0) return;   // full ring == read_space() null
                   io_uring_prep_recv(s, fd, p, n, 0);               // one in flight, unchanged

on_recv(TLS, res>0):
                   BIO_nwrite(ext, &q, res);                          // commit; q == p, proved §2.2
                   loop {
                     dst = c->read_space(kRecvChunk, avail, rob.quiesced());
                     if (!dst) break;                                 // plaintext backpressure
                     r = SSL_read(ssl, dst, avail);
                     if (r > 0) { c->commit_read(r); continue; }
                     op = funnel(r);                                  // helio's ToOpResult shape
                     if (op == NEED_READ)  break;                     // want more ciphertext
                     if (op == NEED_WRITE) { pump(c); break; }        // renegotiation / KeyUpdate
                     close_client(c); return;
                   }
                   parse_and_dispatch(c);
```

Three notes:

**(i) The loop is required, not an optimisation.** One `BIO_nwrite` of a 16 KB recv can hold several
TLS records and therefore several `SSL_read`-worth of plaintext. Reading once and returning is
exactly the bug redis papers over with its pending-data list (`redis/src/tls.c:635-643`, drained from
`beforeSleep`) and valkey with `TLS_CONN_FLAG_HAS_PENDING` (`valkey/src/tls.c:1239-1245`). **Draining
in a loop means tomokv needs neither.** If the loop must exit early because `read_space` refused,
the conn stays in `active_` (it is not `done` — `io_loop.h:890` requires `rob().quiesced()`), so
phase 1 revisits it next pass. That is the existing backpressure mechanism doing the job with no new
list.

**(ii) `NEED_WRITE` from a *read*.** This is the renegotiation / TLS 1.3 KeyUpdate case, and it is
where helio hit a real deadlock (`tls_socket.cc:385-406` documents the fix and names the
`Tls13KeyUpdateNeedWrite` test). Here it is trivial: call `pump(c)`, which is already idempotent and
already refuses when a send is in flight (`wb.h:127`). No lock, no condvar, no fiber. This is the
single clearest illustration of why one-thread-owns-the-conn is worth more than any TLS-specific
machinery.

**(iii) `recv_armed_` changes meaning, and that is safe.** Today it means "the kernel holds a raw
pointer into `rbuf_`" and is what makes `reset_rbuf_at_quiescence` safe (`conn.h:274-283`). Under
TLS the kernel holds a pointer into the *BIO*, and `rbuf_` is written only by `SSL_read`, on this
thread, synchronously. So the `!recv_armed_` term in `reset_rbuf_at_quiescence` becomes conservative
rather than necessary — harmless, and **do not weaken it**: the cost is one skipped reset, the risk
of "optimising" it is the heap corruption the comment at `conn.h:280-282` describes. The BIO
reservation replaces it as the thing being pinned, and BIO pair buffers never move or realloc, which
is strictly stronger than `rbuf_`'s guarantee.

### 2.5 Send path — where the whole design can go wrong

Today (`wb.h:126-165`): retire ops in ROB order → stage bytes into the fill buffer or segment queue
→ build up to 16 iovecs → one `sendmsg`. The TLS version inserts encryption between staging and
submission:

```
pump(TLS):
  if (send_inflight_) return false;                     // unchanged
  drain output BIO into the send request:
      n = BIO_nread0(ext, &cipher);  if (n > 0) -> prep_send(fd, cipher, n)   // flush FIRST
  else:
      take the next plaintext chunk from the staged fill-buffer / segment window
      r = SSL_write(ssl, chunk, chunk_len)
      r > 0        -> consume r PLAINTEXT bytes from the queue; go drain the output BIO
      NEED_WRITE   -> output BIO full: stop encrypting, leave the chunk staged, return
      NEED_READ    -> renegotiation: leave staged; the armed recv will drive it
```

**Hazard 1 — the two quantities.** This is the defect class named in
`thredis-wrong-two-quantities`: every flip defect compared mismatched kinds. Here the two quantities
are **plaintext accepted by `SSL_write`** and **ciphertext reported by the send CQE**, and today
both cursors are fed from the same number:

```c++
// wb.h:180-184, on_send_complete, res = CQE result
conn.consume_segments(static_cast<uint32_t>(res), ...);   // advances the PLAINTEXT frontier
conn.commit_write(static_cast<uint32_t>(res));            // advances wsent_, vs plaintext size()
```

Under TLS `res` is ciphertext and both of those are plaintext cursors. Wiring it up unchanged
silently corrupts the stream in proportion to TLS framing overhead, and only under partial writes —
the worst possible shape. **The rule, stated so it can be checked in review:**

| Cursor | Site | Driven by |
|---|---|---|
| `SegmentQueue` `(head_, offset_)` | `conn.h:216-219`, `consume_segments` | **`SSL_write`'s return value** (plaintext accepted) |
| `wsent_` | `conn.h:492`, `commit_write` | **`SSL_write`'s return value** |
| output-BIO read cursor | `BIO_nread` | **the send CQE `res`** (ciphertext) |
| `send_requested_` | `conn.h:496` | **ciphertext** submitted, so the short-write counter stays meaningful |

**Hazard 2 — the `SSL_write` retry contract vs. re-windowing.** OpenSSL requires a retried
`SSL_write` after `WANT_WRITE` to present at least the same length as the failed call; otherwise it
returns `SSL_routines::bad length`. Valkey tracks this explicitly in `last_failed_write_data_len`
(`valkey/src/tls.c:1674-1685`, and honoured again at `:1707` and `:1738-1745`).

tomokv's `build_segment_iov` (`conn.h:354-361`) rebuilds the window from the current frontier on
every `pump`, bounded by `kMaxSendIov`/`kMaxSendBytes` and by *whatever segments happen to be queued
at that moment*. Between a failed `SSL_write` and its retry, more replies may have retired and been
appended — so the recomputed chunk can differ. **Mitigation, and it must be in v1:** pin the offered
chunk on `TlsConn` (`pending_plain_ptr`, `pending_plain_len`) when `SSL_write` returns `WANT_WRITE`,
and re-offer exactly that chunk, byte-identical, until it is accepted. Do not re-window while it is
pinned. `SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER` permits the buffer to *move* but not the contents to
change — and since a `BUF` segment's payload is an independent malloc that retirement never moves
(`conn.h:92-94`), pinning a pointer is safe here.

**Hazard 3 — `SSL_MODE_ENABLE_PARTIAL_WRITE` is mandatory.** Without it `SSL_write` is all-or-
nothing against a fixed 64 KB output BIO, and any reply larger than the ring wedges the connection
permanently. Set the same three modes helio does (`tls_engine.cc:43-45`):
`SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_RELEASE_BUFFERS`.
Redis sets the first two at `redis/src/tls.c:235`; `RELEASE_BUFFERS` is helio's addition and is
worth having at high connection counts (it returns OpenSSL's own 16 KB+ per-`SSL` scratch buffers to
the allocator between records).

**What `WANT_WRITE` becomes.** Nothing new. It is "the output ring is full", which can only be true
while ciphertext is unsent, which is exactly `send_inflight_ == true` plus a non-empty BIO. The
existing completion path (`on_send_complete` → `pump`, `wb.h:202`) is already the re-drive. **No
new state, no new flag, no new queue.** Compare: redis needs `TLS_CONN_FLAG_READ_WANT_WRITE`,
`updateSSLEvent`, and epoll mask churn to express the same idea.

### 2.6 `tls-port` vs `port` semantics

**Adopt redis's model exactly. Reject dragonfly's.**

- `tls-port N`, default 0 = disabled (`redis/src/config.c:3509`).
- `port` and `tls-port` are independent listeners and may both be live. `port 0` disables plain.
- TLS on the canonical port is `port 0` + `tls-port 6379` (`redis.conf:191-196`).
- Refuse to boot if `tls-port != 0` and either `tls-cert-file` or `tls-key-file` is unset
  (mirrors `tlsConfigure`, `redis/src/tls.c:295-303`).

Dragonfly's single global `--tls` (`dragonfly_listener.cc:29`, shipped with an empty help string)
cannot express "plain on loopback, TLS on the wire", which is precisely the tomokv benchmark
posture, and it makes every A/B require a restart.

**Mechanically this is nearly free.** Each io thread already opens its own `SO_REUSEPORT` listener
(`io_loop.h:104-118`), armed with multishot accept (`:217`) tagged `UrKind::Accept`. TLS adds:

- a second listen fd per io thread, `tls_listen_fd_`;
- a new tag `UrKind::TlsAccept` — `UrKind` is a `uint8_t` in the top 16 bits of `user_data`
  (`uring.h:25-40`) with values 1..7 used, so this is free;
- one branch in `on_cqe` (`io_loop.h:242-259`) and one bool through `on_accept`/`adopt_client`
  (`:262`, `:312`), which already carry a `unix_socket` bool in exactly this shape.

The `SO_REUSEPORT` distribution property (`io_loop.h:59-70`) applies to the TLS listener unchanged —
each io thread accepts and then *owns* its TLS connections, which is what keeps the `SSL*` single-
threaded for free. The boot-time port probe in `main.cc:245-250` must be extended to the TLS port;
the `pgrep`-guard discipline from the server-leak incident applies to it identically.

**Not in v1: ClientHello sniffing.** Dragonfly's 5-byte pre-handshake check
(`dragonfly_connection.cc:1055-1080`) is genuinely good — it rejects a plaintext client on a TLS port
with a readable RESP error instead of a TLS alert, and it does so before allocating an `SSL`.
But it is an optimisation on top of a working handshake, it introduces a read that is not a normal
`arm_recv`, and with a separate `tls-port` the misconfiguration it catches is rarer. **Defer it, and
name it in the doc so it is not reinvented.** The cheap 80% today is dragonfly's other trick:
`TCP_DEFER_ACCEPT` on the TLS listener only (`dragonfly_listener.cc:206-219`), which costs one
`setsockopt` in `make_reuseport_listener` and keeps connections out of the accept queue until the
ClientHello has actually arrived.

### 2.7 Zero-copy borrow is DISABLED on TLS connections in v1 — where the check gates

**Why it must be.** A `BORROW` segment is a raw pointer into live FlatStore memory handed to
`sendmsg` (`wb.h:97`, `wb.h:145`). Under TLS the bytes on the wire are ciphertext produced by
`SSL_write` into the output BIO. There is no arrangement in which the socket references plaintext
storage. The borrow's entire purpose — *the value bytes are never copied* — is unachievable; the
`SSL_write` that produces the ciphertext is itself the copy. Retaining the borrow protocol would buy
nothing and cost a registry entry plus a cross-thread release round trip per value. Helio reaches the
same conclusion structurally (`tls_socket.cc:331-379`: iovecs are memcpy-coalesced into a 1392-byte
buffer before encryption).

**There are exactly two producers of a `BORROW` segment.** Both were located by grepping the tree,
not from memory:

| # | Producer | Site | Thread |
|---|---|---|---|
| 1 | single-key `GET` | `src/cmd/t_string.cc:247-253` (registration), consumed at `src/net/wb.h:91-98` | executor registers, **io** appends |
| 2 | multi-key gather (`MGET`/scatter) | `src/cmd/scatter_engine.inc:1278-1288` (registration), consumed at `src/cmd/xshard_commands.inc:1130` | executor registers, **io** appends |

**The gate is two-layer, and the layering is the point.**

**Layer A — the mandatory correctness gate, io-side, at both append sites.** Both
`append_borrow_segment` calls run on the connection's owning io thread, which knows `c->is_tls()`.
This is where the invariant is enforced, because it is impossible to add a third borrow producer
without going through one of these two lines.

- `src/net/wb.h:91-101` — inside `WbEngine::serve`'s drain lambda. Under TLS, replace the
  `append_borrow_segment(op.zc_ptr, op.zc_len, op.zc_shard)` with
  `append_buf_segment(op.zc_ptr, op.zc_len)` (which mallocs and memcpys, `conn.h:104-113`) followed
  by an immediate `queue_borrow_release(op.zc_shard, op.zc_ptr)` (`io_loop.h:781-784`). The
  `[header BUF][value BUF][CRLF STATIC]` ordering is preserved, so nothing downstream changes.
- `src/cmd/xshard_commands.inc:1130` — same substitution; note the existing
  `slot.kind = ValueKind::Nil` at `:1134` already transfers ownership away from arena teardown, so
  the release must be issued explicitly on this path.

**Layer B — the optimisation gate, executor-side, GET only.** Layer A is correct but wasteful: it
pays a `FlatStore::borrow` + registry entry + cross-thread release for every large GET on a TLS
conn. Kill it at the source for the dominant path. There is an exact precedent for stamping
connection facts into an op at dispatch — `io_loop.h:478`:

```c++
if (c->has_atomic_group_io()) op->mark_atomic_hazard();
```

with the flag living in `route_flags_`, a `uint8_t` at `src/exec/op.h:123` of which **only bit 0 is
used** (`kAtomicHazard = 1u << 0`, `op.h:230`). So:

- add `kNoBorrow = 1u << 1` to `op.h:230` and `mark_no_borrow()` / `no_borrow()` beside
  `op.h:121-122`;
- stamp it next to the atomic-hazard stamp at `io_loop.h:478`, under `if (c->is_tls())`;
- change the gate at `src/cmd/t_string.cc:247-248` from
  `if (zc_min && value.n >= zc_min)` to `if (zc_min && value.n >= zc_min && !op.no_borrow())`.

Cost on plain connections: one test of a byte already loaded on the same line as `spec`/`shard`/
`hash`. `sizeof(Op)` is unchanged (`op.h` comment at `:118-120` notes the bit occupies existing
padding), so the 336-byte footprint lock holds.

**Do not** try to apply Layer B to the MGET gather. `ValueSlot::kInline` is 1024 bytes
(`config.h:15`), so a value above that physically cannot be inlined into the slot; suppressing the
borrow there needs a third `ValueKind` with arena-allocated storage. That is real work for a
non-dominant path. Layer A already makes it correct.

**Instrumentation is mandatory, per the vacuous-validation rule.** `WbEngine::Stats::zc_sends` and
`zc_bytes` already exist (`wb.h:241-243`) and `tests/gate.sh` section 4b already asserts
`zc_sends > 0` on the plain arm. Add a `zc_suppressed_tls` counter incremented at both Layer A
sites. The TLS validation arm must then assert **both** `zc_suppressed_tls > 0` (the gate was
reached) and `zc_sends` unchanged (no borrow escaped). Asserting only "no crash" would prove
nothing, and asserting only `zc_sends == 0` passes vacuously if the arm never sent a large value.

### 2.8 kTLS — the named later path that restores zero-copy

kTLS moves record framing and bulk encryption into the kernel: after
`setsockopt(fd, SOL_TCP, TCP_ULP, "tls", 4)` and `setsockopt(fd, SOL_TLS, TLS_TX, &crypto_info, ...)`
an ordinary `send`/`sendmsg` on that fd emits encrypted records. **That is what makes the borrow
protocol legal again**, because the kernel encrypts from the iovec the application supplies — the
plaintext is read once, in-kernel, and never materialised in userspace ciphertext form.

Availability was checked on this machine, not assumed:

```
kernel 7.0.0-28-generic
/usr/include/linux/tls.h   TLS_TX=1  TLS_RX=2  TLS_TX_ZEROCOPY_RO=3  TLS_RX_EXPECT_NO_PAD=4
OpenSSL 3.0.13             SSL_OP_ENABLE_KTLS present, SSL_sendfile present,
                           BIO_get_ktls_send / BIO_get_ktls_recv present
```

The catch, and it is why this is v2 and not v1: **kTLS requires OpenSSL to own a socket BIO.**
`BIO_get_ktls_send` interrogates the *wbio*, and OpenSSL only enables kTLS on a `BIO_s_socket`.
A bio-pair engine can never have it — which is precisely why helio's kTLS work is a
`GTEST_SKIP`-guarded feasibility probe (`tls_socket_test.cc:288-329`) and never reached production.

So the v2 shape is a **hybrid**, and it should be designed as such from the start:

1. Perform the handshake on the memory BIO exactly as in v1.
2. On success, extract the negotiated keys and install them with `TLS_TX` (and optionally `TLS_RX`)
   on the fd, then release the engine's send half.
3. From then on `pump` submits plaintext iovecs to an ordinary `io_uring_prep_sendmsg` — **the
   existing code path, unmodified** — and `BORROW` segments become legal again. Clear the `kNoBorrow`
   stamp and skip the Layer-A substitution once the socket reports kTLS TX active.
4. Keep RX on the memory BIO initially. `TLS_RX` is the fiddlier half (post-handshake messages,
   `TLS_RX_EXPECT_NO_PAD`), and TX alone is where the zero-copy value is.

The v1 design is deliberately compatible with this: the borrow gate is one flag plus two io-side
branches, and the recv path is untouched by TX-only kTLS. **Guard rails:** kTLS silently declines
for unsupported ciphersuites, so the code must read back `BIO_get_ktls_send()` and fall back rather
than assume; and a kTLS socket cannot renegotiate, so `SSL_OP_NO_RENEGOTIATION` becomes mandatory
rather than merely advisable.

### 2.9 Multishot recv + provided buffers under TLS — the honest position

Helio's comment (`tls_socket.cc:559-568`) says multishot is incompatible with TLS because a
kernel-pushed buffer may exceed the engine's available input space, requiring "a complex overflow
buffer implementation". That is accurate **for their design**, which recvs *directly into* the BIO.

It is not a universal law. With provided buffers you would `BIO_write(rbio, buf, len)` — one copy —
and the buffer ring itself absorbs overflow, because a buffer you have not returned to the ring is
still yours. Deferring the return *is* the backpressure. So the real trade is:

| | v1 pull + `BIO_nwrite0` | multishot + provided buffers |
|---|---|---|
| ciphertext ingress | **zero copy** | one memcpy per recv |
| kernel entries under load | one recv per arm | amortised |
| backpressure | `BIO_nwrite0 <= 0` (proved, §2.2) | deferred buffer return |
| new machinery | none | buf ring + deferred-return accounting |

Since tomokv has no buffer ring today (§0.1), v1 costs nothing to give up. **Revisit only after
measuring**, and only alongside plain-conn multishot — evaluating it TLS-first would attribute the
buffer-ring win to TLS.

---

## 3. (c) The exact knob subset to adopt first

Seven knobs, redis semantics, redis spellings. Everything else waits.

| Knob | Type | Default | Redis source | Semantics to reproduce exactly |
|---|---|---|---|---|
| `tls-port` | uint16 | `0` | `config.c:3509` | 0 = disabled. Independent of `port`; both may be live. `port 0` + `tls-port 6379` = TLS on the canonical port. Boot-only in v1 (redis makes it MODIFIABLE via `applyTLSPort`, `config.c:2902-2907`; defer that). |
| `tls-cert-file` | string | `null` | `config.c:3518` | PEM chain, loaded with `SSL_CTX_use_certificate_chain_file` (`tls.c:241`). **Required** when `tls-port != 0`; refuse to boot otherwise (`tls.c:295-298`). |
| `tls-key-file` | string | `null` | `config.c:3519` | PEM private key, `SSL_CTX_use_PrivateKey_file(..., SSL_FILETYPE_PEM)` (`tls.c:247`). Required with `tls-port` (`tls.c:300-303`). |
| `tls-ca-cert-file` | string | `null` | `config.c:3525` | CA bundle. Passed as the *file* arg of `SSL_CTX_load_verify_locations` (`tls.c:253-254`). |
| `tls-ca-cert-dir` | string | `null` | `config.c:3526` | CA directory, the *dir* arg of the same call. Requires `c_rehash`. **At least one of file/dir is required when `tls-auth-clients != no`** (`tls.c:305-309`). |
| `tls-auth-clients` | enum | `yes` | `config.c:3514`, enum at `config.c:116` | `yes` → `SSL_VERIFY_PEER \| SSL_VERIFY_FAIL_IF_NO_PEER_CERT`; `optional` → `SSL_VERIFY_PEER`; `no` → `SSL_VERIFY_NONE`. Applied **per accepted connection**, not on the CTX (`tls.c:502-512`) — keep that, it is what lets the mode change without rebuilding the context. Default `yes` is fail-secure; keep it. |
| `tls-protocols` | string | `null` → TLSv1.2+TLSv1.3 | `config.c:3527` | Space-separated, case-insensitive, from {`TLSv1`,`TLSv1.1`,`TLSv1.2`,`TLSv1.3`} (`tls.c:66-99`). Empty = the safe default (`tls.c:40-44`). |
| `tls-ciphers` | string | `null` | `config.c:3528` | `SSL_CTX_set_cipher_list` (`tls.c:260`). **TLS ≤ 1.2 only** — `redis.conf:326` says so explicitly. |

That is 8 rows because `tls-ca-cert-file` and `-dir` are one knob in two spellings; the brief lists
them as one. Two implementation notes:

**Use `TLS_method()` + `SSL_CTX_set_min_proto_version`/`_max_`, not redis's subtractive form.**
Redis builds with `SSLv23_method()` and then subtracts with `SSL_OP_NO_TLSv1` etc.
(`tls.c:209-229`), which needs one `#ifdef` per protocol version forever. Dragonfly already does it
the modern way (`src/facade/tls_helpers.cc:92`, `:134`). Keep redis's *config string grammar* — that
is the compatibility surface operators see — but implement it as min/max.

**`tls-ciphersuites` (TLS 1.3) is a deliberate omission and must be documented as such.** With
`tls-protocols` defaulting to TLSv1.2+1.3 and `tls-ciphers` applying only to ≤1.2, an operator who
sets `tls-ciphers` expecting to constrain TLS 1.3 gets a silent no-op. Either add
`tls-ciphersuites` in v1 (redis `config.c:3529`, one `SSL_CTX_set_ciphersuites` call — genuinely
~5 lines) or reject `tls-ciphers` with an explicit error naming the limitation. **Silent no-op is
the one outcome to avoid.** Recommendation: add it; it is cheaper than the support burden.

Not in v1, and why: `tls-key-file-pass` / `tls-client-key-file-pass` (needs a password callback and
`SENSITIVE_CONFIG` handling), `tls-dh-params-file` (`SSL_CTX_set_dh_auto(ctx, 1)` on OpenSSL 3 is
right for everyone — `redis/src/tls.c:390-392`), `tls-session-caching` / `-cache-size` / `-timeout`
(resumption is a handshake-churn optimisation; measure the churn arm first — §5), `tls-client-*`
(no outbound TLS: no replication, no cluster bus), `tls-replication` / `tls-cluster` (same),
`tls-prefer-server-ciphers`, `tls-auth-clients-user`, `tls-expected-peer-name`,
`tls-auto-reload-interval`.

**Knob-philosophy compliance** (`tomokv.conf` house rules, `config.h:7-8`: numeric where possible,
0 = off and off allocates nothing, −1 = auto). `tls-port 0` satisfies it exactly: zero means no TLS
listener, no `SSL_CTX`, no per-conn slab, and the `is_tls()` test is a compare against a word already
in cache. The string knobs are unavoidable — a cert path has no numeric encoding — but they are
boot-only and cost nothing at runtime.

**Wiring.** All eight land in `Config` (`src/core/config.h:33-89`), get a `--flag` arm in
`parse_config_args` (`config.h:130-289`), and become visible through `init_config`
(`src/cmd/t_server.cc:211`). The conf-file loader (`config.h:309-339`) translates `tls-port 6380` to
`--tls-port 6380` mechanically, so no separate work. Validation belongs in `validate_config`
(`config.h:292-303`) so a bad combination fails before any thread starts — the same discipline as
the existing shard/dbfilename checks.

---

## 4. (d) Steal / Avoid

### Steal

1. **The engine/socket split with explicit op-codes** — helio `tls_engine.h:15-141`. A pure
   in-memory `Engine` with no fd and no syscalls, and a socket layer that owns all I/O. Four op-codes
   (`EOF_GRACEFUL`, `EOF_ABRUPT`, `NEED_READ_AND_MAYBE_WRITE`, `NEED_WRITE`) mapped in exactly one
   funnel (`tls_engine.cc:228-278`). Unit-testable without a socket; the single highest-value
   structural idea in any of the three trees.
2. **"Flush output BIO before any socket read", unconditionally** — helio `tls_engine.h:28-34`,
   `tls_socket.cc:408-415`. And flush before returning from a failed handshake, so the peer receives
   the alert (`tls_socket.cc:127-166`).
3. **Zero-copy peek/commit BIO access** — helio `tls_engine.cc:73-106` (`BIO_nread0`/`BIO_nread`,
   `BIO_nwrite0`/`BIO_nwrite`). Verified here directly against the io_uring reserve-then-commit
   pattern (§2.2).
4. **The `SSL_write` retry length contract** — valkey `tls.c:1674-1685`, `:1707`, `:1738-1745`. Take
   the *mechanism* (pin the failed chunk, re-offer ≥ the same length), not valkey's silent
   `return -1` escape hatch, which leaves `errno` stale and is itself annotated as papering over an
   unresolved bug. This is the top P0 for tomokv's re-windowing send loop (§2.5).
5. **`SSL_MODE_RELEASE_BUFFERS`** — helio `tls_engine.cc:45`. Neither redis nor valkey sets it.
   Returns OpenSSL's per-`SSL` 16 KB+ scratch between records; at thousands of connections this is
   the difference between a TLS build being deployable and not.
6. **Per-connection client-auth via `SSL_set_verify`, not the CTX** — redis `tls.c:502-512`. Keeps
   `tls-auth-clients` changeable without rebuilding the context, and keeps the fail-secure `default:`
   branch (`:509`) when the enum is somehow out of range.
7. **A separate accept budget for TLS** — redis `config.c:3446` (default 1 vs 10). tomokv's
   multishot accept has no budget at all today; a handshake storm on the TLS listener would occupy
   an io thread that also owns live connections. Bound TLS accepts per pass.
8. **`TCP_DEFER_ACCEPT` on the TLS listener** — dragonfly `dragonfly_listener.cc:206-219`. One
   `setsockopt` in `make_reuseport_listener` (`io_loop.h:104-118`); keeps half-open connections out
   of the accept queue until a ClientHello arrives.
9. **Handshake counters and cert-expiry observability** — dragonfly's `SSL_CTX_set_info_callback`
   counting `SSL_CB_HANDSHAKE_START`/`_DONE` (`tls_helpers.cc:153-162`), and valkey's
   `tls_*_cert_expires_in_seconds` INFO fields (`valkey/src/server.c:6282-6287`). Expiry-in-seconds
   is computed once at load and is exactly the metric to alert on.
10. **Pre-load certificate validity checking** — valkey `isCertValid` (`tls.c:467-489`), applied to
    the server cert and every CA in the store. Failing configuration beats failing every handshake.
11. **The 5-byte ClientHello sniff** — dragonfly `dragonfly_connection.cc:1055-1080`. Named for v2,
    with the injection trick (`InitSSL(ctx, buf)` feeding the sniffed bytes into the input BIO) so
    the handshake is not disturbed.
12. **`tls-port` semantics** — redis `config.c:3509`, `redis.conf:191-196`. Separate listener,
    coexisting with plain, `port 0` to go TLS-only.

### Avoid

1. **fd BIO / `SSL_set_fd`** — redis `tls.c:514`, `:722`. Disqualified under io_uring (§2.2).
2. **The crossed-want state machine** — redis `tls.c:443-444`, `:576-577`, `:596-631`, and the three
   ad-hoc clear sites at `:792`, `:798`, `:805`. It is a *consequence* of the fd BIO. Copying it into
   a memory-BIO design would add ~60 lines of state that can only ever be dead.
3. **A pending-plaintext list** — redis `tls.c:635-653` + the `has_pending_data`/
   `process_pending_data` vtable pair (`connection.h:90-91`); valkey's global list
   (`valkey/src/tls.c:116-118`). Draining `SSL_read` in a loop until `WANT_READ` (§2.4) makes the
   entire mechanism unnecessary. Note redis's version also has a live use-after-free: it walks with a
   cached `listIter` while handlers can free the *next* connection (`redis/src/tls.c:1279-1284` vs
   `connTLSClose`'s `listDelNode` at `:899-903`); valkey fixed it by popping before handling
   (`valkey/src/tls.c:1915-1923`). Do not inherit the bug *or* its fix — inherit neither by not
   having the list.
4. **Event-loop rebinding** — redis `tls.c:1051-1077`, with its full-quiescence assert. Solves
   connection migration between threads. tomokv connections never migrate.
5. **The postpone/update-state gate** — valkey `tls.c:1214-1225`, `:1248`, plus the submit/rollback
   triples at `io_threads.c:532/593/832` and the completion replay at `networking.c:6571-6581`.
   Genuinely elegant, and genuinely solving "workers do crypto, one thread owns readiness". tomokv
   has neither half of that problem. Also note valkey's own version discards the mask
   (`tls.c:1216-1220`), making the `CONN_POSTPONE_READ`/`WRITE` bits dead weight — even the good
   version is not fully realised.
6. **`SSLv23_method()` + `SSL_OP_NO_*` subtraction** — redis `tls.c:209-229`. Use `TLS_method()` +
   min/max proto version (dragonfly `tls_helpers.cc:92`, `:134`).
7. **`CRYPTO_set_locking_callback` / `USE_CRYPTO_LOCKS`** — redis `tls.c:106-140`, valkey
   `tls.c:125-159`. Dead since OpenSSL 1.1.0 (2016). The build requires ≥ 1.1.1 anyway.
8. **`SSL_CTX_set_session_id_context(ctx, "redis", 5)`** — redis `tls.c:322` (dragonfly does the
   same with `"dragonfly"`). Only meaningful with session caching, which v1 does not ship; copying
   the constant blindly is an interop landmine.
9. **`exit()` from a library helper** — dragonfly's `DFLY_SSL_CHECK` calls `exit(17)`
   (`tls_helpers.h:34-39`) while adjacent failures return `nullptr`. Pick one error style; return a
   status and let `main.cc` decide.
10. **A debug hook branching in the hot write path** — valkey's `debug_force_tls_write_error`
    (`tls.c:1665-1672`) tests a global on every `connTLSWrite`. If a fault-injection hook is needed,
    put it behind a compile-time flag, the way `TOMO_WEDGE_FORENSICS` already is
    (`conn.h:46-50`).
11. **Config that looks live but is not** — dragonfly's `tls_auth_clients` inside `#if 0`
    (`dragonfly_listener.cc:51-67`), and its documented "to update certs, set tls_cert_file then set
    `tls true`" dance (`server_family.cc:1236-1237`). `config.h:8` already states the rule: *a field
    in Config that nothing reads is a lie — delete it.*
12. **Byte-at-a-time `sync_readline`** — valkey `tls.c:1862-1900`, one `SSL_read` plus one
    `updateSSLPendingFlag` per byte. Only exists for blocking replication paths tomokv does not have.
13. **A global `--tls` switch with no per-listener control** — dragonfly
    `dragonfly_listener.cc:29`. Forces a restart for every plain-vs-TLS A/B.

---

## 5. (e) Validation plan

Four arms plus a perf cell set. Every arm follows the vacuous-validation rule: it asserts a
**mechanism fired**, not merely that nothing crashed. All arms extend `tests/gate.sh`; the existing
zero-copy section 4b (`gate.sh:101-117`, which greps `zc_sends=` out of the server log) is the
template.

**Prerequisite — certificates.** Reuse redis's generator: `/home/user/Projects/redis/utils/gen-test-certs.sh`
produces `ca.{crt,key}`, `redis.{crt,key}`, `client.{crt,key}`, `server.{crt,key}` and a SAN cert.
Copy it to `tests/gen-certs.sh` with paths retargeted; do not write a new one.

**Load generator.** `memtier_benchmark` is installed and supports `--tls --cert --key --cacert
--tls-skip-verify --tls-protocols --sni` (verified via `--help`). Both p1 and p32 arms are therefore
available with the existing harness, and the saturated-benching rule applies unchanged: single-conn
never saturates, so every cell is p1 **and** p32.

### Arm 1 — handshake churn

**What it catches:** handshake state machine bugs, `SSL` leaks, deferred-free ordering under
`close_notify`, accept-budget starvation, and the "one io thread's handshakes starve its live
connections" failure.

- N cycles of connect → handshake → `PING` → close, at ≥ 4 concurrent connectors, ≥ 60 s.
- Repeat with RST-mid-handshake (close the socket after the ClientHello) — this is the TLS analogue
  of the torture battery's RST churn (`tests/torture.py`), and it is the path that found the
  idempotent-`close_client` double-delete (`io_loop.h:929-934`).
- Run under ASAN. The `.make-settings` cached-`SANITIZER` trap applies: `ldd`-check the binary.
- **Assert:** `tls_handshakes_completed > 0` **and** `== tls_handshakes_started − expected_failures`;
  `accepts` on the TLS listener > 0 on **more than one** io thread (proves `SO_REUSEPORT` distributes
  TLS accepts — the 6-io-threads-577-connections distribution bug at `io_loop.h:59-70` would
  otherwise reappear silently); RSS flat across cycles; zero ASAN reports.

### Arm 2 — renegotiation and large values

**What it catches:** the three §2.5 hazards — the two-quantities cursor mismatch, the `SSL_write`
retry-length violation, and a `WANT_WRITE` that never re-drives.

- Value sizes spanning the interesting boundaries: 1 KB (below everything), 16 KB (`kRecvChunk`,
  `io_loop.h:45`), 16384 (`zc_min` exactly, `config.h:61`), 64 KB (the proposed BIO ring), 1 MB, 8 MB
  (multi-`SSL_write`, guaranteed partial writes against a 64 KB ring).
- `GET`/`SET` round-trip byte-compared, not just length-compared. `tests/differ.py` already does
  vanilla-oracle comparison; point it at a TLS endpoint.
- `MGET` with mixed sizes straddling `ValueSlot::kInline` (1024) and `zc_min`, to exercise the
  second borrow producer (`scatter_engine.inc:1278-1288` → `xshard_commands.inc:1130`).
- **Renegotiation proper:** TLS 1.3 KeyUpdate is the live case (classical renegotiation is off —
  `SSL_OP_NO_RENEGOTIATION`). Drive it with `openssl s_client` issuing `K` mid-stream, or by setting a
  low `SSL_CTX` key-update limit. This is exactly what deadlocked helio
  (`tls_socket.cc:385-406`, `Tls13KeyUpdateNeedWrite`), so it must be a real arm and not a TODO.
- **Assert:** every value byte-identical; `short_writes > 0` (`wb.h:186` — proves the partial-write
  path was actually taken, otherwise the retry contract was never tested); `send_errors == 0`;
  a new `tls_want_write` counter > 0; `zc_suppressed_tls > 0` and `zc_sends == 0` (§2.7).

### Arm 3 — mixed TLS + plain connections

**What it catches:** cross-contamination — a plain conn taking a TLS branch or vice versa — and the
`is_tls()` check being read from the wrong place. Redis has exactly this test:
`tests/unit/tls.tcl:118` "TLS: switch between tcp and tls ports".

- Boot with `port P` and `tls-port Q` both live. Run plain and TLS memtier instances simultaneously
  against the same server, same keyspace.
- One io thread must own both a plain and a TLS connection at once — with `SO_REUSEPORT` on both
  listeners and enough connections this happens naturally, but **assert it** rather than assume it,
  by dumping per-thread plain/TLS accept counts.
- Negative cases, both directions: plain client → TLS port (must fail cleanly, not hang, not crash);
  TLS client → plain port (must fail cleanly). Redis tests the first at `tls.tcl:5`.
- `tls-auth-clients yes` with no client cert must be rejected; with a valid cert accepted; `optional`
  and `no` behave per §3 (redis `tls.tcl:11-37`).
- **Assert:** both connection kinds served concurrently with correct data; at least one io thread
  shows both kinds; `zc_sends > 0` (the *plain* conns still borrow — this is the check that proves
  the TLS gate did not disable zero-copy globally, which is the most likely way to get a "clean" but
  worthless result); zero cross-talk.

### Arm 4 — parity bar

Per the standing rule, every cell must be on par with or beat stable `730dc029f` **on plain
connections**. TLS must cost nothing when `tls-port 0`.

- `stablecmp32` from a clean worktree; the 8-cell quick-check (p32/p1 × GET/SET, 2 statics) with
  `tls-port 0`.
- **Assert:** within box noise (±2% on the 7700X when exclusive; ±0.15% on the EPYC). Anything
  outside that means the `is_tls()` branch or the `Client` layout change cost something, and the
  hardcode-or-delete rule applies to the branch itself.

### Perf expectation — the honest range

**Nobody publishes a number.** This is the finding, not a gap in the search: dragonfly has no TLS
benchmark anywhere in `docs/`, `tests/` or `tools/`; redis and valkey ship no TLS perf claim in-tree.
What all three publish is *architectural evidence* of cost, and it is consistent:

| Evidence | Source | What it implies |
|---|---|---|
| TLS accept budget 1 vs plain 10 | redis `config.c:3445-3446`; valkey `config.c:3516` | a handshake is ≈10× an `accept()` in event-loop occupancy |
| TLS excluded from multishot + buffer rings | dfly `dragonfly_connection.cc:3710`; helio `tls_socket.cc:559-568` | TLS conns forfeit the recv fast path entirely |
| TLS pinned to the older IoLoop v1 | dfly `docs/pub-sub.md:287`, flag help `:150`/`:153` | TLS is not on the optimised path |
| iovecs memcpy-coalesced before encryption | helio `tls_socket.cc:351-356` (1392 B); redis `tls.c:1116-1122` | scatter-gather is given up under TLS |
| upstream reads capped at the BIO size (~17 KB) | helio `tls_socket.cc:432-437` | large reads fragment into more syscalls |
| `NET_MAX_WRITES_PER_EVENT` copy-for-syscalls tradeoff | redis `tls.c:1091-1101` | redis accepts a full reply memcpy to cut syscalls — TLS already forces copies |

**Therefore, the expectation to write down before measuring** — as a hypothesis the arms will
falsify or confirm, not as a claim:

| Cell | Expected TLS tax vs plain | Dominant term |
|---|---|---|
| small-value GET/SET, p32 | **−25 % to −45 %** | per-record framing + per-`SSL_read`/`SSL_write` call overhead; the memory says small-value is dispatch-bound, and TLS adds a fixed per-op cost to exactly that bound |
| small-value, p1 | **−10 % to −25 %** | latency-bound, so the added work partly hides in RTT |
| 16 KB+ values | **−15 % to −30 %** | AES-GCM bulk throughput with AES-NI (GB/s class) plus the **loss of the borrow** — this cell pays the zero-copy regression twice and is where kTLS eventually pays back |
| handshake churn | **order of magnitude** on connect rate | asymmetric crypto; the whole reason for a session cache and for redis's budget-of-1 |
| `tls-port 0` (plain) | **0 %, within noise** | this is the parity bar, not an expectation |

**Do not report any of these as results.** The sanity-gate rule applies with full force: if a
measured TLS tax comes in better than −10 % on the small-value p32 cell, the most likely explanation
is that TLS was not actually on the path (client fell back to plain, or the arm connected to `port`
rather than `tls-port`). Verify the negotiated cipher per connection before believing any number.

---

## 6. (f) Build size estimate and riskiest parts

### Size

| Component | LOC | Notes |
|---|---|---|
| `src/net/tls_ctx.h` — SSL_CTX build, config validation, protocol/cipher application | ~200 | mirrors redis `createSSLContext` (`tls.c:202-277`) + the required half of `tlsConfigure` (`:285-412`), minus DH/session/client-cert |
| `src/net/tls_conn.h` — `TlsConn`: BIO pair, op-code funnel, handshake, feed/drain, cursor pair | ~320 | helio's `Engine` (`tls_engine.h/.cc`, ~280 LOC upstream) plus the pinned-chunk retry state |
| `src/core/io_loop.h` — TLS listener, `UrKind::TlsAccept`, handshake state, TLS `arm_recv`/`on_recv`, close | ~130 | additive branches; no restructuring |
| `src/net/wb.h` — encrypt-in-`pump`, cursor split in `on_send_complete`, Layer-A borrow gate | ~90 | the highest-risk 90 lines in the change |
| `src/net/conn.h` — `tls_slot_` in the offset-12 pad, accessors, `static_assert(offsetof(...))` | ~20 | `sizeof(Client)` must stay 1984 |
| `src/exec/op.h` + `src/cmd/t_string.cc` — `kNoBorrow` flag, Layer-B gate | ~12 | `sizeof(Op)` must stay 336 |
| `src/cmd/xshard_commands.inc` — Layer-A gate on the gather path | ~15 | |
| `src/core/config.h` + `src/cmd/t_server.cc` — 8 knobs, parse, validate, CONFIG GET | ~110 | |
| `src/main.cc` — TLS port probe, CTX init before listeners | ~35 | |
| `Makefile` — `-lssl -lcrypto`, optional `TLS=0` like the existing `JE=0` | ~10 | |
| `tests/` — cert generation, 4 arms, gate wiring | ~250 | |
| **Total** | **≈ 1200 LOC**, of which **≈ 600 is production code** | |

For calibration: redis's `tls.c` is 1414 lines and valkey's 2100, but both carry outbound TLS,
replication/cluster TLS, blocking sync paths, session caching, cert reload, peer-name verification,
and a decade of OpenSSL version compatibility. The v1 scope here is server-side inbound TLS only.

**Non-code cost — memory.** A 64 KB × 2 BIO pair is **128 KB of fixed ciphertext staging per TLS
connection**, plus the `SSL` object (~2–4 KB, less with `SSL_MODE_RELEASE_BUFFERS`). Against an idle
`Client` of ~8.5 KB RSS today, that is more than an order of magnitude. At 10 000 TLS connections it
is ~1.3 GB. **This is the largest single cost of the design and it must be a knob, not a constant.**
Recommended: `tls-bio-size`, default 65536, honouring the house rule that thresholds self-derive —
default it to `max(4 * kRecvChunk, 65536)` so it tracks `kRecvChunk` if that ever changes. Operators
with high connection counts and small values should be able to drop it to 16 KB and pay in short
contiguous runs instead of RSS. Add per-thread accounting the way dragonfly does
(`CRYPTO_set_mem_functions`, `dragonfly_listener.cc:111-151`, surfaced as `tls_bytes` in
`metrics.cc:376`) so the cost is visible rather than inferred.

### Riskiest parts, ranked

**1. The plaintext/ciphertext cursor split (§2.5, hazard 1).** `on_send_complete` currently feeds the
CQE byte count into two plaintext cursors (`wb.h:180`, `wb.h:183`). Under TLS these are different
quantities. Getting it wrong corrupts the reply stream in proportion to TLS framing overhead, only
under partial writes, and therefore only at large values or under load. This is textbook
`thredis-wrong-two-quantities`. **Mitigation:** name both operands in the code — call the variables
`plain_accepted` and `cipher_sent`, never `n` or `res`; add a debug assertion that
`cipher_sent >= plain_accepted` on any completed record; make arm 2 assert `short_writes > 0` so the
path is provably exercised rather than provably absent.

**2. The `SSL_write` retry-length contract vs. `build_segment_iov` re-windowing (§2.5, hazard 2).**
`build_segment_iov` (`conn.h:354-361`) recomputes the window from the live queue on every `pump`, and
retirement can append between a failed `SSL_write` and its retry. OpenSSL answers a shrunk retry with
`SSL_routines::bad length`. Valkey hit this in production and carries a `TODO` about an unresolved
case (`valkey/src/tls.c:1680-1683`). **Mitigation:** the pinned-chunk rule (§2.5), plus an assertion
that a pinned chunk's `(ptr, len)` is bit-identical on re-offer.

**3. Deferred-free ordering with an `SSL` object in the picture (§2.3).** `reap_dead`
(`io_loop.h:976-995`) holds a corpse while `serve_pending || send_inflight || recv_armed`. Under TLS
a send in flight references the **output BIO**, and a recv in flight references the **input BIO** —
both inside `TlsConn`. Freeing `TlsConn` at `close_client` instead of at reap is a use-after-free that
the RST-churn arm will find under ASAN and that loopback testing alone will not. **Mitigation:**
free `TlsConn` only at the same point `delete c` happens (`io_loop.h:990`), never at close; add a
`TOMO_WEDGE_FORENSICS` counter for TLS frees and assert it equals TLS accepts at shutdown.

**4. Handshake occupancy starving live connections on the same io thread.** A handshake is
asymmetric crypto on the io thread that also owns N established connections. Redis's budget-of-1
(`config.c:3446`) exists for exactly this. tomokv's multishot accept has no budget at all
(`io_loop.h:217`), and `flush_ready` phase 1 walks every active conn each pass (`io_loop.h:851`) —
so a handshake burst inflates every pass. **Mitigation:** bound TLS handshake steps per pass the way
`kServeBudget` bounds serves (`io_loop.h:1003`), and make arm 1 measure established-connection p99
*during* a handshake storm, not only after.

**5. `sizeof(Client)` / `sizeof(Op)` footprint locks (§2.3, §2.7).** Both are `static_assert`-ed
(`conn.h:538`, and the `Op` lock noted at `config.h:17`) and both are signed against 64-core A/Bs.
The proposed placements — `uint32_t` in the offset-12 pad, `kNoBorrow` in `route_flags_` bit 1 —
preserve both, but they depend on declaration order that a future edit could disturb silently.
**Mitigation:** add `static_assert(offsetof(Client, rbuf_) == 16)` so the pad is load-bearing by
contract rather than by luck.

**6. The BIO ring size as a hidden wedge.** A ring smaller than the largest single reply is fine
*only if* `SSL_MODE_ENABLE_PARTIAL_WRITE` is set. Without it, one oversized reply wedges the
connection forever with no error — the identical shape to the `kRbufSoftCap` wedge the comment at
`conn.h:61-67` documents ("a 2 MB SET stalls its connection forever"). **Mitigation:** set the mode
(§2.5, hazard 3) and put an 8 MB value in arm 2 specifically to prove the partial path works.

**7. Validation that passes without testing anything.** A TLS arm that connects, does a `PING`, and
reports success proves nothing — not that records were encrypted, not that the borrow gate fired, not
that any partial write occurred. **Mitigation:** every assert listed in §5 is a counter comparison or
a byte comparison, never a liveness check. `PING` is not liveness here any more than it was in the
soak harness.

---

## Appendix — where every change lands

| File | Change |
|---|---|
| `src/net/tls_ctx.h` | **new** — `SSL_CTX` construction and validation |
| `src/net/tls_conn.h` | **new** — per-conn engine; no fd, no ring, no syscalls |
| `src/net/conn.h` | `tls_slot_` at offset 12 (`:486-489`); `offsetof` assert; keep `:538` at 1984 |
| `src/net/wb.h` | `pump` `:126-165` encrypts; `on_send_complete` `:169-204` cursor split; Layer-A gate at `:91-101` |
| `src/net/uring.h` | one enum value `UrKind::TlsAccept` at `:25-33` |
| `src/core/io_loop.h` | TLS listener at `:104-118`; accept branch `:241-298`; handshake in `flush_ready` `:851-900`; TLS `arm_recv` `:225-238`; TLS drain in `on_recv` `:342-354`; `close_notify` in `close_client` `:942`; `TlsConn` free in `reap_dead` `:976-995`; `kNoBorrow` stamp beside `:478` |
| `src/core/config.h` | 8 knobs in `Config` `:33-89`, `parse_config_args` `:130-289`, `validate_config` `:292-303` |
| `src/exec/op.h` | `kNoBorrow = 1u << 1` at `:230`; accessors at `:121-122`; `sizeof(Op)` stays 336 |
| `src/cmd/t_string.cc` | Layer-B gate at `:247-248` |
| `src/cmd/xshard_commands.inc` | Layer-A gate at `:1130` |
| `src/cmd/t_server.cc` | `init_config` `:211` exposes the 8 knobs; TLS counters in INFO |
| `src/core/signal.h` | TLS counters beside `:76-81` (`tls_handshakes_*`, `tls_want_write`, `zc_suppressed_tls`) |
| `src/main.cc` | TLS port probe beside `:245-250`; `SSL_CTX` built before any listener |
| `Makefile` | `-lssl -lcrypto`; `TLS=0` escape mirroring `JE=0` at `:5-18` |
| `tests/gen-certs.sh` | **new** — retargeted from `redis/utils/gen-test-certs.sh` |
| `tests/gate.sh` | arms 1–4 as new sections, modelled on 4b (`:101-117`) |

---

## Implementation receipt — 2026-08-27, base `420b4d492`

This implementation uses an OpenSSL memory-BIO pair. `TlsConn` contains no fd BIO, socket, ring or
syscall; io_uring remains the sole socket owner. kTLS was not implemented and remains explicitly
deferred to v2.

The audit's offset-12 placement was stale on this base: `last_interaction_s_` now occupies bytes
12..16. The mirror-struct probe in `scratchpad/tls_layout_probe.cc` re-derived the remaining
four-byte tail hole and places `tls_slot_` at offset **1980**. The compiled locks are
`sizeof(Client) == 1984` and `sizeof(Op) == 336`.

The receive-side quantity split is structural: socket CQEs commit ciphertext only to the external
BIO, while only positive `SSL_read` results advance the Client RESP buffer. The directed TLS 1.2
torn-record arm sends all but the last byte of a record, verifies that no reply or parser progress
escapes, supplies the final byte, and then verifies the torn command plus a following command remain
synchronized. The send side likewise names and advances `plain_accepted` separately from
`cipher_sent`.

GET's FlatStore borrow is disabled with a copy-only handler variant selected only by
`parse_and_dispatch<true>`; the plaintext handler has no transport load or branch. The cross-shard
MGET producer is gated independently at assembly. Gate counters prove both TLS suppression sites
fired. Plaintext GET and SET handler bodies match the base instruction stream after normalizing
link-relative target displacements (1724 and 2786 bytes respectively).

### Plaintext instruction parity (`tls-port 0`)

Measurement used the exact base worktree, 2 IO + 2 executor threads on CPUs 248-251, load on
252-255, memtier 8x8 clients, pipeline 32, 64-byte values, 64,000,000 operations per repeat, and
three repeats. Values are retired server instructions per operation; the delta is post minus base.
The hardware-counter spread is recorded because it is wider than one instruction, while the
normalized hot handler bodies provide the byte-level off-state check.

| Cell | base median | TLS code, `tls-port 0` median | delta instr/op | regression bar |
|---|---:|---:|---:|---:|
| GET hit, p32 | 2937.199 | 2926.599 | **-10.600** | pass (`<= +1`) |
| SET, p32 | 3242.915 | 3241.681 | **-1.234** | pass (`<= +1`) |

Repeat ranges were GET base 2906.637-2980.990, post 2884.831-2930.328; SET base
3239.648-3247.504, post 3239.342-3243.908.

### Wire throughput result

Measurement used 4 IO + 2 executor threads on CPUs 248-253, load on 254-255, simultaneous plain
and TLS listeners in one server, memtier 2x16 clients, pipeline 32, 64-byte values, 5 seconds, and
three repeats. TLS used memtier's OpenSSL transport against the dedicated TLS port; the server
reported non-zero plaintext and ciphertext counters and freed all 192 measured TLS connections.

| Cell | plain median ops/s | TLS median ops/s | measured TLS tax |
|---|---:|---:|---:|
| GET hit, p32 | 2,617,532.78 | 427,121.05 | **-83.68%** |
| SET, p32 | 2,288,108.78 | 327,715.25 | **-85.68%** |

This is substantially worse than the audit's -25% to -45% hypothesis. It is an informational v1
result, not rewritten to fit the hypothesis; correctness and plaintext parity remain separate
gates.

### Validation receipt

- Required quick gate on `GATE_PORT=7953 GATE_CORES=248-255`: **75 ok, 0 FAIL**; TLS slots
  **28/28 freed**, TLS application send errors zero, borrow suppressions 20 (both producers).
- Full TLS battery passed under ASAN+UBSAN, including client-auth `yes`, `optional`, and `no`, bad
  certificates/transports, 8 MiB pipelining, RESET/RST cleanup, mixed plain+TLS clients, the torn
  record arm, and both borrow producers.
- Plaintext oracle differ, both atomic settings: string 4033 ops / 0 differences, xshard 4276 / 0,
  cgaps 3310 / 0.
- The available Redis 7.4 oracle was built without TLS (`Missing implement of connection type tls`),
  so a cross-server TLS differ was unavailable; the tomokv-only byte-comparison battery was used.
