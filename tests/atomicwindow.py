"""Admission witnesses at the derived production window; no window configuration override.

Arm, release the DEBUG hold, THEN demand completion. Only a clean, fully drained witness miss
can be re-armed. Per-attempt telemetry measures both the window hit and reply rates; four attempts
are a bounded discovery budget, not a claim about an unmeasured false-failure probability.

Reconfiguration additionally pre-admits a cross-owner EXEC with the existing fan-out park. Its
copied ten-second deadline outlives the five-second arming budget, while the owner commit spin
is released BEFORE CONFIG. A faster MSET drain cannot remove that identified old-generation lease.
"""
import select
import socket
import threading
import time

import _lib


ARM_ATTEMPTS = 4
ARM_SECONDS = 5
RESUME_SECONDS = 30
DRAIN_SECONDS = 5


def held_burst(host, port, *, whole_window, reconfigure=False):
    """Witness overlap or global admission; optionally rebuild credits with groups still live."""
    if reconfigure and not whole_window:
        raise ValueError("credit reconfiguration needs the global-window arm")
    admin = _lib.Conn(host, port, timeout=30)
    try:
        server = _lib.info(admin, "server")
        limit = min(16 * int(server["shards"]), 1024)
        connections = 2 * ((limit + 63) // 64) + 2 if whole_window else 1
        depth = 64 if whole_window else 24
        label = "reconfigure" if reconfigure else "window" if whole_window else "overlap"
        history = []

        def sample():
            table = _lib.info(admin, "stats")
            return {key: int(table["atomic_" + key]) for key in
                    ("groups", "inflight", "window_stalls", "credit_debt", "credit_pool")}

        def idle(table):
            return (table["inflight"] == 0 and table["credit_debt"] == 0 and
                    table["credit_pool"] == limit)

        for attempt in range(1, ARM_ATTEMPTS + 1):
            if attempt > 1:
                # The control connection's IO placement also orders CONFIG relative to writers.
                admin.close()
                admin = _lib.Conn(host, port, timeout=30)
            deadline = time.monotonic() + DRAIN_SECONDS
            while not idle(sample()):
                if time.monotonic() >= deadline:
                    raise AssertionError("atomic groups/credits did not drain before arming")
                time.sleep(0.01)
            prefix = "derived-window:%d:%d" % (time.time_ns(), attempt)
            # Every group must span owners; a same-owner fast path is not an admission witness.
            burst_groups = connections * depth
            groups = burst_groups + int(reconfigure)
            buckets = _lib.owner_buckets(admin, prefix, per_owner=groups, limit=groups * 64)
            enough = [keys for keys in buckets.values() if len(keys) >= groups]
            # Build before the timed arm and release all senders together. Python encoding and
            # thread startup must not spend the publication window one connection at a time.
            payloads = [b"".join(
                _lib.encode("MSET", enough[0][i], "a", enough[1][i], "b")
                for i in range(index * depth, (index + 1) * depth))
                for index in range(connections)]
            clients = []
            blocker = None
            threads = []
            start = threading.Barrier(connections + 1, timeout=10)
            errors = []
            replies = [0] * connections
            peak = 0
            carried = 0
            stalls = 0
            held_stalls = 0
            armed = False
            released = False
            started = time.monotonic()
            arm_elapsed = 0.0
            resume_elapsed = 0.0
            resumed = None
            held_replies = 0
            release_replies = 0
            outcome = "failed"
            config_deadline = None
            blocker_done = not reconfigure
            lease_checked = not reconfigure
            lease_pool = None
            isolated_config = False

            def run(index, client):
                try:
                    start.wait()
                    client.raw(payloads[index])
                    for _ in range(depth):
                        reply = client.read()
                        if reply != b"OK":
                            raise AssertionError("unexpected atomic burst reply: %r" % (reply,))
                        replies[index] += 1
                except Exception as exc:
                    errors.append("client%d: %s" % (index, exc))

            def observe():
                nonlocal peak, stalls
                table = sample()
                peak = max(peak, table["inflight"])
                stalls = table["window_stalls"] - before["window_stalls"]
                if table["inflight"] > limit:
                    raise AssertionError("in-flight groups exceed derived limit: %d > %d" %
                                         (table["inflight"], limit))
                if table["credit_debt"] != 0:
                    raise AssertionError("fixed window acquired credit debt: %r" % table)
                if errors:
                    raise AssertionError("atomic burst failed: %r" % errors)
                return table

            try:
                for _ in range(connections):
                    clients.append(_lib.Conn(host, port, timeout=30))
                before = sample()
                try:
                    if reconfigure:
                        # Keep one identified old-generation lease alive independently of the
                        # MSET commit batches. CONFIG fans out to their owners, so a commit spin
                        # also holds CONFIG back and can let the entire burst retire first.
                        # This EXEC hook PARKS non-lead fragments. Disarm it once EXEC has
                        # copied its deadline, BEFORE CONFIG (which also takes the read fan-out
                        # hook). The parked transaction cannot finish within our arm budget.
                        blocker = _lib.Conn(host, port, timeout=30)
                        keys = (enough[0][burst_groups], enough[1][burst_groups])
                        for args, expected in ((("MULTI",), b"OK"),
                                               (("MSET", keys[0], "a", keys[1], "b"), b"QUEUED"),
                                               (("MGET", *keys), b"QUEUED")):
                            if blocker.must(*args) != expected:
                                raise AssertionError("lease holder did not queue its transaction")

                        def exec_calls():
                            # INFO omits commands with zero calls on a fresh boot.
                            row = _lib.info(admin, "commandstats").get("cmdstat_exec", "calls=0")
                            return int(row.split("calls=", 1)[1].split(",", 1)[0])

                        dispatched = exec_calls()
                        if admin.must("DEBUG", "ATOMIC-FANOUT-DEFER",
                                      str(2 * ARM_SECONDS * 1000000)) != b"OK":
                            raise AssertionError("lease hold did not arm")
                        config_deadline = time.monotonic() + ARM_SECONDS
                        blocker.send("EXEC")
                        # multi_dispatch_entry increments EXEC's command count AFTER copying
                        # the hook and posting its fragments. An admission count alone precedes
                        # that copy and would race disarm against dispatch.
                        while exec_calls() != dispatched + 1 or observe()["inflight"] != 1:
                            if time.monotonic() >= config_deadline:
                                raise AssertionError("lease holder was not admitted before burst")
                            time.sleep(0.005)
                        if admin.must("DEBUG", "ATOMIC-FANOUT-DEFER", "0") != b"OK":
                            raise AssertionError("lease hold did not disarm after dispatch")
                        if select.select([blocker.sock], [], [], 0)[0]:
                            raise AssertionError("lease holder completed before burst")
                    if admin.must("DEBUG", "ATOMIC-COMMIT-DELAY", "100000") != b"OK":
                        raise AssertionError("commit hold did not arm")
                    for index, client in enumerate(clients):
                        thread = threading.Thread(target=run, args=(index, client), daemon=True)
                        thread.start()
                        threads.append(thread)
                    started = time.monotonic()
                    start.wait()
                    deadline = config_deadline or started + ARM_SECONDS
                    while True:
                        table = observe()
                        armed = table["inflight"] >= 2 and (stalls > 0 or not whole_window)
                        if armed and reconfigure:
                            # The full-window witness is captured. Release the owner spin so
                            # CONFIG can run; the independently parked EXEC retains its lease.
                            if admin.must("DEBUG", "ATOMIC-COMMIT-DELAY", "0") != b"OK":
                                raise AssertionError("commit hold did not release before CONFIG")
                            # Same-value SET still rebuilds the credit generation (server.h).
                            # Keep atomic ON so all submitted groups remain subject to the bound.
                            if admin.must("CONFIG", "SET", "atomic", "1") != b"OK":
                                raise AssertionError("CONFIG SET atomic 1 failed")
                            after_config = observe()
                            if time.monotonic() >= config_deadline:
                                raise AssertionError("CONFIG exceeded the lease hold's arm budget")
                            if (after_config["inflight"] < 1 or
                                    select.select([blocker.sock], [], [], 0)[0]):
                                raise AssertionError("old-generation lease holder completed during CONFIG")
                            # The sole EXEC was admitted BEFORE the burst and its non-lead
                            # fragments are still parked. This names one surviving old lease;
                            # unrelated, newer MSET admissions cannot subtract it from the proof.
                            carried = 1
                            break
                        if armed and not reconfigure:
                            break
                        if not any(thread.is_alive() for thread in threads):
                            break
                        if time.monotonic() >= deadline:
                            break
                        time.sleep(0.005)
                finally:
                    arm_elapsed = time.monotonic() - started
                    held_replies = sum(replies)
                    held_stalls = stalls
                    # This MUST precede the completion deadline. Holding every commit until all
                    # 640 gate-geometry groups finish was a timing test, not a resume witness.
                    if admin.must("DEBUG", "ATOMIC-COMMIT-DELAY", "0") != b"OK":
                        raise AssertionError("commit hold did not disarm")
                    if reconfigure:
                        # EXEC copied its delay at dispatch; zero prevents NEW holds.
                        # Continue observing the bound while that one deadline expires below.
                        if admin.must("DEBUG", "ATOMIC-FANOUT-DEFER", "0") != b"OK":
                            raise AssertionError("lease hold did not disarm")
                    released = True

                resumed = time.monotonic()
                release_replies = sum(replies)
                deadline = resumed + RESUME_SECONDS
                while True:
                    table = observe()
                    burst_done = not any(thread.is_alive() for thread in threads)
                    if not lease_checked:
                        # MSETs admitted after CONFIG may return credits to the holder's IO
                        # cache, which remains active. Once they retire, rebuild once more:
                        # with no new admissions, the pool must exclude exactly the old EXEC.
                        if time.monotonic() >= config_deadline:
                            raise AssertionError("old lease accounting did not settle within "
                                                 "arm budget: %r" % table)
                        if burst_done and not isolated_config:
                            if admin.must("CONFIG", "SET", "atomic", "1") != b"OK":
                                raise AssertionError("isolated lease reconfiguration failed")
                            isolated_config = True
                            continue
                        if (burst_done and table["inflight"] == 1 and
                                table["credit_pool"] == limit - 1):
                            lease_checked = True
                            lease_pool = table["credit_pool"]
                    if not blocker_done and select.select([blocker.sock], [], [], 0)[0]:
                        if not lease_checked:
                            raise AssertionError("lease holder completed before its accounting was checked")
                        reply = blocker.read()
                        if reply != [b"OK", [b"a", b"b"]]:
                            raise AssertionError("unexpected lease holder reply: %r" % (reply,))
                        replies.append(1)
                        blocker_done = True
                    if blocker_done and burst_done:
                        break
                    if time.monotonic() >= deadline:
                        raise AssertionError("atomic burst did not resume AFTER hold release: "
                                             "replies=%d/%d" % (sum(replies), groups))
                    time.sleep(0.005)
                resume_elapsed = time.monotonic() - resumed
                if errors or sum(replies) != groups:
                    raise AssertionError("atomic burst failed: replies=%d/%d errors=%r" %
                                         (sum(replies), groups, errors))
                deadline = time.monotonic() + DRAIN_SECONDS
                while True:
                    after = observe()
                    if idle(after):
                        break
                    if time.monotonic() >= deadline:
                        raise AssertionError("derived-window credits did not return: %r" % after)
                    time.sleep(0.01)
                # Do not accept a transient sample gathered only during drain as the held witness.
                witnessed = (armed and held_replies < groups and
                             (carried > 0 or not reconfigure))
                outcome = "armed" if witnessed else "clean-miss"
            finally:
                if resumed is not None and not resume_elapsed:
                    resume_elapsed = time.monotonic() - resumed
                start.abort()
                # Wake blocked readers before touching their buffered file objects. close() alone
                # can wait for the reader's lock and hide a live helper behind a nominal join bound.
                for client in clients:
                    if client.sock is not None:
                        try:
                            client.sock.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass
                cleanup_deadline = time.monotonic() + 1
                for thread in threads:
                    thread.join(timeout=max(0, cleanup_deadline - time.monotonic()))
                stuck = any(thread.is_alive() for thread in threads)
                for client in clients:
                    if stuck:
                        client.sock.close()
                    else:
                        client.close()
                if blocker is not None:
                    blocker.close()
                held_rate = "%.1f" % (held_replies / arm_elapsed) if arm_elapsed else "n/a"
                resume_rate = ("%.1f" % ((sum(replies) - release_replies) / resume_elapsed)
                               if resumed is not None and resume_elapsed else "n/a")
                detail = ("%s attempt=%d/%d outcome=%s limit=%d groups=%d peak=%d "
                          "held_stalls=%d stalls=%d "
                          "carried=%d held_replies=%d arm_s=%.3f held_replies/s=%s "
                          "released=%s replies=%d/%d resume_s=%.3f resume_replies/s=%s" %
                          (label, attempt, ARM_ATTEMPTS, outcome, limit, groups, peak, held_stalls, stalls,
                           carried, held_replies, arm_elapsed, held_rate, released, sum(replies),
                           groups, resume_elapsed, resume_rate))
                if reconfigure:
                    detail += " lease_pool=%s" % lease_pool
                history.append(detail)
                print("  note atomic-window " + detail, flush=True)
                if stuck:
                    raise AssertionError("atomic burst helpers did not stop")
            if witnessed:
                return "%s credits=%d observed_attempts=1/%d" % (detail, limit, attempt)
            # Match multirace and OFF RENAME: retry ONLY a clean, joined, drained witness miss.
        raise AssertionError("%s window never observed: observed_attempts=0/%d; %s" %
                             (label, ARM_ATTEMPTS, " | ".join(history)))
    finally:
        admin.close()
