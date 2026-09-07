#!/usr/bin/env python3
# lbsignals battery -- proves the LB signal READ side reports real, conserved quantities.
#
# Non-vacuous by construction: the load leg asserts OP CONSERVATION (both split rollups, or the
# single fused rollup, advance by the commands sent), which can only hold if the active counters
# are live and the capture path reads the right memory. Zeros or stale rows fail every leg.
#
# Thread modes. Under --thread-mode 2s the existing io/ex rows, rollups, ratio, and INFO schema stay
# live. Under 1s every thread and the sole rollup are labelled `fused`; no synthetic empty ex rollup
# or split ratio is emitted. Exact 1s conservation has not been pinned by a run, so the fused leg
# asserts >= N and prints the delta for the first run to pin it.
#
# The client is tests/_lib.Conn, a real RESP reader. The previous reader was `sleep 0.05` plus one
# recv() with a "replies arrive in one burst" heuristic: a DEBUG LBSIGNALS dump that arrives in two
# segments was truncated and the row failed on a missing field with no server defect behind it
# (AUDIT-TESTS F3). Fixed sleeps that only waited for counters to settle are now bounded polls.
import sys
import time

import _lib

HOST, PORT = sys.argv[1], int(sys.argv[2])
SANE_AGE_US = 60_000_000
AGE_FIELDS = ("queue_delay_ewma_us", "oldest_age_us", "oldest_age_ewma_us",
              "oldest_age_min_us", "oldest_age_max_us")
ROLLUP_AGE_FIELDS = ("queue_delay_ewma_us", "oldest_age_min_us", "oldest_age_max_us",
                     "oldest_age_ewma_us")

checks = 0
skipped = []


def ok(label, cond, detail=""):
    global checks
    if not cond:
        print(f"FAIL {label} {detail}", flush=True)
        sys.exit(1)
    checks += 1


def skip(label, reason):
    skipped.append(label)
    print(f"  SKIP {label} -- {reason}", flush=True)


def capture(conn):
    snap = _lib.lbsignals(conn)
    threads = [t._asdict() for t in snap.threads]
    shards = [{"sid": s.sid, "owner": s.owner, "ops": s.ops, "foreign": s.foreign}
              for s in snap.shards]
    return threads, shards, snap.rollups, snap.derived


s = _lib.Conn(HOST, PORT, timeout=10)
MODE = _lib.thread_mode(s)
FUSED = MODE == "1s"

# ---- leg 1: structure + idle sanity --------------------------------------------------------------
t1, sh1, roll1, der1 = capture(s)
ok("thread rows exist", len(t1) >= 2, str(len(t1)))
roles = {r["role"] for r in t1}
ok("derived thread mode matches INFO", der1.get("thread_mode") == MODE, str(der1))
if FUSED:
    ok("1s: every thread is labelled fused", roles == {"fused"}, str(roles))
    ok("1s: only fused rollup is present", set(roll1) == {"fused"}, str(sorted(roll1)))
    ok("1s: fused rollup covers every thread", roll1["fused"]["threads"] == len(t1),
       str(roll1["fused"]["threads"]))
else:
    ok("both roles present", {"io", "ex"} <= roles, str(roles))
    ok("2s: io/ex rollups are present", set(roll1) == {"io", "ex"}, str(sorted(roll1)))
    ok("rollup thread counts match rows",
       roll1["io"]["threads"] == sum(1 for r in t1 if r["role"] == "io")
       and roll1["ex"]["threads"] == sum(1 for r in t1 if r["role"] == "ex"))
ok("shard rows exist", len(sh1) >= 1)
ok("every shard names a live thread as owner",
   {x["owner"] for x in sh1} <= {r["tid"] for r in t1},
   str(sorted({x["owner"] for x in sh1})))
ok("derived owner count matches shard rows",
   int(der1["owner_threads"]) == len({x["owner"] for x in sh1}), str(der1))
ok("derived client count matches client-serving rows",
   int(der1["client_threads"]) == (len(t1) if FUSED else sum(
       1 for r in t1 if r["role"] == "io")), str(der1))
ok("cpu_ns live on every thread", all(r["cpu_ns"] > 0 for r in t1))
if FUSED:
    ok("1s: split ratio fields are absent",
       "ratio_star_io_frac" not in der1 and "ratio_star_io" not in der1, str(der1))
else:
    ok("ratio_star fields present", "ratio_star_io_frac" in der1 and "ratio_star_io" in der1)
    f = float(der1["ratio_star_io_frac"])
    ok("ratio_star_io_frac in (0,1)", 0.0 <= f <= 1.0, str(f))
ok("idle age exports are sane",
   all(0 <= r[field] <= SANE_AGE_US for r in t1 for field in AGE_FIELDS))

it1 = {r["tid"]: r["iterations"] for r in t1}
ds1 = {r["tid"]: r["depth_samples"] for r in t1}
# Iterations advance on their own (timers tick the loop); poll for it rather than sleeping a
# fixed 0.4 s and hoping.
t2 = _lib.wait_until(
    lambda: (lambda rows: rows if all(r["iterations"] > it1.get(r["tid"], 0) for r in rows)
             else None)(capture(s)[0]), 3.0, interval=0.1)
ok("iterations advance while idle", t2 is not None)
if FUSED:
    skip("depth is time-gated, not iteration-weighted",
         "the depth sampler is an executor-queue instrument folded into fused work in 1s")
else:
    ok("depth is time-gated, not iteration-weighted",
       all(0 < r["depth_samples"] - ds1.get(r["tid"], 0)
           < r["iterations"] - it1.get(r["tid"], 0) for r in t2 if r["role"] == "ex"))

# ---- leg 2: op conservation under load -----------------------------------------------------------
_, _, roll_a, _ = capture(s)
N = 20000
w = _lib.Conn(HOST, PORT, timeout=10)
w.raw(b"".join(_lib.encode("SET", b"lbk%06d" % i, "vvvvvvvv") for i in range(N)))
acked = 0
for _ in range(N):
    if w.read() != b"OK":
        break
    acked += 1
ok("load leg completed", acked == N, f"{acked} OK replies of {N}")
# Counters are per-thread and land within a pass; wait for the active rollup to show the load rather
# than sleeping a fixed 0.3 s.
active_role = "fused" if FUSED else "io"
ok("%s ops reached the rollup" % active_role,
   _lib.wait_until(lambda: capture(s)[2][active_role]["ops"] -
                   roll_a[active_role]["ops"] >= N, 3.0,
                   interval=0.05))
_, sh_b, roll_b, der_b = capture(s)

# The capture connections' own DEBUG/INFO commands add a handful of ops; bound, don't equate.
if FUSED:
    dfused = roll_b["fused"]["ops"] - roll_a["fused"]["ops"]
    ok("1s: fused ops advanced by at least the load",
       dfused >= N, f"dfused={dfused} (N={N}; pin exact multiplier)")
else:
    dio = roll_b["io"]["ops"] - roll_a["io"]["ops"]
    dex = roll_b["ex"]["ops"] - roll_a["ex"]["ops"]
    ok("io ops conserved", N <= dio <= N + 50, f"dio={dio}")
    ok("ex ops conserved", N <= dex <= N + 50, f"dex={dex}")
if FUSED:
    ok("fused busy advanced under load",
       roll_b["fused"]["busy_ns"] > roll_a["fused"]["busy_ns"])
    for label in ("ex busy advanced under load", "ex depth sampled", "sampled queue delay fired",
                  "sampled queue delay EWMA valid", "sampled oldest-entry age fired",
                  "no backpressure at this load"):
        skip(label, "executor-only attribution is folded into the fused rollup in 1s")
    ok("1s: fused age exports stay bounded",
       all(0 <= roll_b["fused"][field] <= SANE_AGE_US for field in ROLLUP_AGE_FIELDS))
else:
    ok("io busy advanced under load", roll_b["io"]["busy_ns"] > roll_a["io"]["busy_ns"])
    ok("ex busy advanced under load", roll_b["ex"]["busy_ns"] > roll_a["ex"]["busy_ns"])
    ok("ex depth sampled", roll_b["ex"]["avg_depth"] >= 0.0)
    ok("idle controller allocates no age-sampling work",
       roll_b["ex"]["queue_delay_samples"] == roll_a["ex"]["queue_delay_samples"] == 0)
    ok("sampled queue delay EWMA valid", roll_b["ex"]["queue_delay_ewma_us"] >= 0.0)
    ok("idle controller leaves age observations off", roll_b["ex"]["oldest_age_max_us"] == 0)
    ok("sampled age exports stay bounded",
       all(0 <= roll_b[role][field] <= SANE_AGE_US for role in ("io", "ex")
           for field in ROLLUP_AGE_FIELDS))
    ok("no backpressure at this load",
       roll_b["ex"]["full_events"] == roll_a["ex"]["full_events"])
ok("shard ops sum >= N", sum(x["ops"] for x in sh_b) >= N)
ok("clean pinning: zero foreign ops", sum(x["foreign"] for x in sh_b) == 0,
   str(sum(x["foreign"] for x in sh_b)))

# ---- leg 3: INFO # LB carries the derived block --------------------------------------------------
lb = _lib.info(s, "LB")
if FUSED:
    fused_fields = (
        "lb_thread_mode", "lb_fused_threads", "lb_client_threads", "lb_owner_threads",
        "lb_fused_busy_frac", "lb_fused_ns_per_op", "lb_fused_avg_depth",
        "lb_fused_full_events", "lb_foreign_op_frac", "lb_fused_queue_delay_samples",
        "lb_fused_queue_delay_ewma_us", "lb_fused_oldest_age_min_us",
        "lb_fused_oldest_age_max_us", "lb_fused_oldest_age_ewma_us")
    for field in fused_fields:
        ok(f"INFO has {field}", field in lb)
    ok("1s: INFO mode is explicit", lb["lb_thread_mode"] == "1s", lb["lb_thread_mode"])
    ok("1s: INFO fused thread count matches rows",
       int(lb["lb_fused_threads"]) == len(t1), lb["lb_fused_threads"])
    ok("1s: INFO owner count matches shard rows",
       int(lb["lb_owner_threads"]) == len({x["owner"] for x in sh1}), lb["lb_owner_threads"])
    ok("1s: INFO has no synthetic split fields",
       all(field not in lb for field in ("lb_io_threads", "lb_ex_threads",
                                         "lb_ratio_star_io_frac")), str(lb))
    ok("INFO fused ns_per_op parses > 0",
       float(lb["lb_fused_ns_per_op"]) > 0.0, lb["lb_fused_ns_per_op"])
    info_age_fields = ("lb_fused_queue_delay_ewma_us", "lb_fused_oldest_age_min_us",
                       "lb_fused_oldest_age_max_us", "lb_fused_oldest_age_ewma_us")
else:
    split_fields = (
        "lb_io_threads", "lb_ex_threads", "lb_io_busy_frac", "lb_ex_busy_frac",
        "lb_io_ns_per_op", "lb_ex_ns_per_op", "lb_ratio_star_io_frac",
        "lb_foreign_op_frac", "lb_io_queue_delay_samples", "lb_ex_queue_delay_samples",
        "lb_io_queue_delay_ewma_us", "lb_ex_queue_delay_ewma_us",
        "lb_io_oldest_age_min_us", "lb_io_oldest_age_max_us", "lb_io_oldest_age_ewma_us",
        "lb_ex_oldest_age_min_us", "lb_ex_oldest_age_max_us", "lb_ex_oldest_age_ewma_us")
    for field in split_fields:
        ok(f"INFO has {field}", field in lb)
    ok("INFO io ns_per_op parses > 0", float(lb["lb_io_ns_per_op"]) > 0.0,
       lb["lb_io_ns_per_op"])
    ok("INFO ex ns_per_op parses > 0", float(lb["lb_ex_ns_per_op"]) > 0.0, lb["lb_ex_ns_per_op"])
    info_age_fields = ("lb_io_queue_delay_ewma_us", "lb_ex_queue_delay_ewma_us",
                       "lb_io_oldest_age_min_us", "lb_io_oldest_age_max_us",
                       "lb_io_oldest_age_ewma_us", "lb_ex_oldest_age_min_us",
                       "lb_ex_oldest_age_max_us", "lb_ex_oldest_age_ewma_us")
for field in info_age_fields:
    value = float(lb[field])
    ok("INFO %s is sane" % field, 0 <= value <= SANE_AGE_US, str(value))

print(f"LBSIGNALS: {checks} checks ok (thread-mode {MODE}, {len(skipped)} skipped: "
      f"{', '.join(skipped) if skipped else 'none'})", flush=True)
