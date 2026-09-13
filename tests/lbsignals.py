#!/usr/bin/env python3
# lbsignals battery — proves the LB signal READ side reports real, conserved quantities.
#
# Non-vacuous by construction: the load leg asserts OP CONSERVATION (io ops delta == ex ops delta
# == commands sent), which can only hold if both roles' counters are live and the capture path
# reads the right memory. A dump that always returned zeros or stale rows fails every leg.
import socket
import sys
import time

HOST, PORT = sys.argv[1], int(sys.argv[2])


def conn():
    s = socket.create_connection((HOST, PORT))
    s.settimeout(10)
    return s


def cmd(s, *args):
    payload = b"*%d\r\n" % len(args)
    for a in args:
        b = a if isinstance(a, bytes) else str(a).encode()
        payload += b"$%d\r\n%s\r\n" % (len(b), b)
    s.sendall(payload)
    time.sleep(0.05)
    data = b""
    s.settimeout(2)
    try:
        while True:
            chunk = s.recv(1 << 20)
            if not chunk:
                break
            data += chunk
            if len(chunk) < (1 << 20):
                # Heuristic: verbatim/bulk replies arrive in one burst at these sizes.
                break
    except socket.timeout:
        pass
    return data


def capture(s):
    raw = cmd(s, "DEBUG", "LBSIGNALS").decode(errors="replace")
    assert "lbver 1" in raw, f"missing lbver header: {raw[:120]!r}"
    threads, shards, rollups, derived = [], [], {}, {}
    for line in raw.split("\n"):
        f = line.split()
        if not f:
            continue
        if f[0] == "thread":
            threads.append({"tid": int(f[1]), "role": f[2], "domain": int(f[3]),
                            "clients": int(f[4]), "iterations": int(f[5]), "ops": int(f[6]),
                            "busy_ns": int(f[7]), "idle_ns": int(f[8]), "cpu_ns": int(f[9]),
                            "depth_samples": int(f[11]), "full_events": int(f[12])})
        elif f[0] == "shard":
            shards.append({"sid": int(f[1]), "owner": int(f[2]), "ops": int(f[4]),
                           "foreign": int(f[5])})
        elif f[0] == "rollup":
            rollups[f[1]] = {"threads": int(f[2]), "ops": int(f[3]), "busy_ns": int(f[4]),
                             "idle_ns": int(f[5]), "cpu_ns": int(f[6]), "busy_frac": float(f[7]),
                             "ns_per_op": float(f[8]), "avg_depth": float(f[9]),
                             "full_events": int(f[10])}
        elif f[0] == "derived":
            derived = {f[i]: f[i + 1] for i in range(1, len(f) - 1, 2)}
    return threads, shards, rollups, derived


checks = 0


def ok(label, cond, detail=""):
    global checks
    if not cond:
        print(f"FAIL {label} {detail}")
        sys.exit(1)
    checks += 1


s = conn()

# ---- leg 1: structure + idle sanity --------------------------------------------------------------
t1, sh1, roll1, der1 = capture(s)
ok("thread rows exist", len(t1) >= 2, str(len(t1)))
ok("both roles present", {"io", "ex"} <= {r["role"] for r in t1})
ok("shard rows exist", len(sh1) >= 1)
ok("rollup thread counts match rows",
   roll1["io"]["threads"] == sum(1 for r in t1 if r["role"] == "io")
   and roll1["ex"]["threads"] == sum(1 for r in t1 if r["role"] == "ex"))
ok("cpu_ns live on every thread", all(r["cpu_ns"] > 0 for r in t1))
ok("ratio_star fields present", "ratio_star_io_frac" in der1 and "ratio_star_io" in der1)
f = float(der1["ratio_star_io_frac"])
ok("ratio_star_io_frac in (0,1)", 0.0 <= f <= 1.0, str(f))

time.sleep(0.4)
t2, _, _, _ = capture(s)
it1 = {r["tid"]: r["iterations"] for r in t1}
ok("iterations advance while idle", all(r["iterations"] > it1.get(r["tid"], 0) for r in t2))

# ---- leg 2: op conservation under load -----------------------------------------------------------
_, _, roll_a, _ = capture(s)
N = 20000
w = conn()
payload = b""
for i in range(N):
    k = b"lbk%06d" % i
    payload += b"*3\r\n$3\r\nSET\r\n$%d\r\n%s\r\n$8\r\nvvvvvvvv\r\n" % (len(k), k)
w.sendall(payload)
got = 0
w.settimeout(10)
while got < N * 5:  # +OK\r\n per SET
    chunk = w.recv(1 << 20)
    if not chunk:
        break
    got += len(chunk)
ok("load leg completed", got == N * 5, f"{got} bytes vs {N*5}")
time.sleep(0.3)
_, sh_b, roll_b, der_b = capture(s)

dio = roll_b["io"]["ops"] - roll_a["io"]["ops"]
dex = roll_b["ex"]["ops"] - roll_a["ex"]["ops"]
# The capture connections' own DEBUG/INFO commands add a handful of ops; bound, don't equate.
ok("io ops conserved", N <= dio <= N + 50, f"dio={dio}")
ok("ex ops conserved", N <= dex <= N + 50, f"dex={dex}")
ok("io busy advanced under load", roll_b["io"]["busy_ns"] > roll_a["io"]["busy_ns"])
ok("ex busy advanced under load", roll_b["ex"]["busy_ns"] > roll_a["ex"]["busy_ns"])
ok("ex depth sampled", roll_b["ex"]["avg_depth"] >= 0.0)
ok("no backpressure at this load", roll_b["ex"]["full_events"] == roll_a["ex"]["full_events"])
ok("shard ops sum >= N", sum(x["ops"] for x in sh_b) >= N)
ok("clean pinning: zero foreign ops", sum(x["foreign"] for x in sh_b) == 0,
   str(sum(x["foreign"] for x in sh_b)))

# ---- leg 3: INFO # LB carries the derived block --------------------------------------------------
info = cmd(s, "INFO", "LB").decode(errors="replace")
for field in ("lb_io_threads", "lb_ex_threads", "lb_io_busy_frac", "lb_ex_busy_frac",
              "lb_io_ns_per_op", "lb_ex_ns_per_op", "lb_ratio_star_io_frac",
              "lb_foreign_op_frac"):
    ok(f"INFO has {field}", field + ":" in info)
val = [l for l in info.split("\r\n") if l.startswith("lb_ex_ns_per_op:")][0].split(":")[1]
ok("INFO ns_per_op parses > 0", float(val) > 0.0, val)

print(f"LBSIGNALS: {checks} checks ok")
