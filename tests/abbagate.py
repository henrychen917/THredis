#!/usr/bin/env python3
"""Same-session headline ABBA gate. No historical rate is an input to any verdict.

WHY NOT STORED REFERENCE NUMBERS. A stored rate goes stale the moment the kernel, compiler,
microcode or machine changes; this tree's old tests/gate_refs.txt was pinned to a kernel that no
longer runs, and the gate's own comments then called every verdict provisional. Measuring both arms
in ONE session on ONE box removes drift, thermal state and machine configuration as variables. The
only difference left between the arms is the code.

THE REFERENCE is the last pushed build: origin/cpp resolved to a full commit id, then matched by
that exact commit against /home/user/Projects/bench-bins/MANIFEST.md. A filename containing
"headline" is NOT evidence of identity; two different digests for one commit are ambiguous and skip.
Missing or unbuildable reference => loud SKIP, never a pass: a correctness-only run must not be able
to call itself a clean gate.

ABBA, NOT A-THEN-B. Every cell runs reference, candidate, candidate, reference and takes the paired
difference. Non-interleaved A/B has produced wrong verdicts on this box before, and a 20-second
window exposes drift that a 90-second window hides. ABBA cancels a LINEAR trend when the run
midpoints are evenly spaced; it cannot promise cancellation of arbitrary interference, periodic
noise or nonlinear drift, so both arm spreads are always reported and a quiet box still matters.

THE THRESHOLD IS DERIVED, NOT CHOSEN. T for a block IS that block's observed reference spread
(100*|A2-A1|/Abar). There is no 3% default, no fixed noise floor, no multiplier, no historical
calibration, and no candidate-dependent widening. A quiet box therefore produces a STRICTER gate,
which is the correct incentive. Two samples measure an observed span, not a confidence interval:
the gate detects losses larger than that span, and a loss inside it is below the session's own
demonstrated resolution -- say that, do not pretend to more.

THE 2% INSTABILITY BOUNDARY IS A VALIDITY CHECK, NOT A REGRESSION ALLOWANCE. An arm whose spread
exceeds 2% fails the MEASUREMENT. Without this, a wildly noisy reference manufactures a permissive
threshold and waves a real regression through. A failed block is never rerun until it happens to
pass, and the best of several runs is never selected.

SATURATION IS A PRECONDITION, not a nice-to-have: an unsaturated cell has headroom that absorbs a
regression, so it cannot detect one at any repetition count. Load generator instances escalate until
the fastest arm stops gaining AND measured busy is at the required level. Depth 1 is exempt and
scored as latency -- it is round-trip bound by Little's law. Process CPU is NOT substituted for busy
percentage: doing so hides exactly the unsaturated case this check exists to catch.

THE VERDICT NAMES THE WORST CELL. It is the conjunction of cell verdicts; no average across GET,
SET, thread modes or cells can hide the one cell that fails. A failed precondition outranks passing
cells. --only is a diagnostic selection and yields PARTIAL with exit 3, never a complete-tier pass.

Exit 0: every cell passed; 1: failure; 3: loud skip or successful partial diagnostic.
--self-test is serverless. All other runs own and reap only their subprocess PIDs.
"""
import argparse
from dataclasses import asdict, dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys
import tarfile
import tempfile
import time

from _lib import Conn

ROOT = Path(__file__).resolve().parents[1]
WINDOW = 20
WARMUP = 3
TAIL = 5
KEYS = 2_000_000
MIN_BUSY = 98.0
# Project measurement-integrity boundary, NOT the regression tolerance.
MAX_SPREAD = 2.0
ORDER = ("A", "B", "B", "A")


class Skip(RuntimeError):
    pass


@dataclass(frozen=True)
class Cell:
    id: str
    mode: str
    read_local: int
    overlap: int
    reorder: int
    op: str
    depth: int
    conns: int


def read_cells(path):
    cells = []
    for lineno, line in enumerate(path.read_text().splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        fields = [x.strip() for x in line.split("|")]
        if len(fields) != 11:
            raise ValueError(f"{path}:{lineno}: expected 11 pipe-separated fields")
        ident, mode, rl, ov, ro, op, depth, conns, *_historical = fields
        if (not re.fullmatch(r"[A-Za-z0-9_-]+", ident) or mode not in ("1s", "2s")
                or op not in ("GET", "SET") or not re.fullmatch(r"p[1-9][0-9]*", depth)
                or not re.fullmatch(r"[1-9][0-9]*", conns)
                or any(not re.fullmatch(prefix + "=[01]", value)
                       for prefix, value in (("rl", rl), ("ov", ov), ("ro", ro)))):
            raise ValueError(f"{path}:{lineno}: unsupported/malformed cell: {line}")
        cells.append(Cell(ident, mode, int(rl[-1]), int(ov[-1]), int(ro[-1]),
                          op, int(depth[1:]), int(conns)))
    if not cells or len({c.id for c in cells}) != len(cells):
        raise ValueError("headline cells must be nonempty with unique IDs")
    return cells


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def capture(argv, cwd=ROOT, timeout=30):
    return subprocess.run([str(a) for a in argv], cwd=cwd, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)


def git(*args):
    p = capture(["git", *args])
    if p.returncode:
        raise RuntimeError(p.stdout.strip())
    return p.stdout.strip()


def cpus(spec):
    result = set()
    for part in spec.split(","):
        if not re.fullmatch(r"[0-9]+(-[0-9]+)?", part):
            raise ValueError(f"invalid CPU list: {spec}")
        bounds = [int(x) for x in part.split("-")]
        lo, hi = bounds[0], bounds[-1]
        if hi < lo:
            raise ValueError(f"invalid CPU range: {part}")
        result.update(range(lo, hi + 1))
    return sorted(result)


def cpu_string(values):
    return ",".join(str(c) for c in values)


def check_placement(server_cpus, load_cpus):
    if set(server_cpus) & set(load_cpus) or len(server_cpus) < 2:
        raise RuntimeError("CPU sets must be disjoint, with >=2 server cores")
    # The invoking shell can itself be taskset-pinned. Its current affinity is NOT the
    # machine/cgroup limit: validate the exact affinity the children will receive instead.
    requested = sorted(server_cpus + load_cpus)
    p = capture(["taskset", "-c", cpu_string(requested), sys.executable, "-c",
                 "import os; print(','.join(map(str, sorted(os.sched_getaffinity(0)))))"])
    if p.returncode or p.stdout.strip() != cpu_string(requested):
        raise RuntimeError(f"requested CPU sets are unavailable: {p.stdout.strip()}")


def load_layout(load_cpus, n, conns):
    """Keep the cell's TOTAL connections fixed; partition physical/SMT pairs together."""
    if conns % n:
        raise ValueError(f"{conns} connections cannot be divided equally over {n} instances")
    groups, seen = [], set()
    for cpu in load_cpus:
        if cpu in seen:
            continue
        topology = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list")
        siblings = set(cpus(topology.read_text().strip())) if topology.exists() else {cpu}
        group = sorted(siblings.intersection(load_cpus))
        groups.append(group)
        seen.update(group)
    if n > len(groups):
        raise ValueError("more load instances than physical CPU groups")
    result = []
    for i in range(n):
        assigned = sorted(c for g in groups[i * len(groups) // n:(i + 1) * len(groups) // n]
                          for c in g)
        per_instance = conns // n
        threads = max(t for t in range(1, min(16, len(assigned), per_instance) + 1)
                      if per_instance % t == 0)
        result.append({"cpus": assigned, "threads": threads, "clients": per_instance // threads})
    return result


def spread(a, b):
    return 200.0 * abs(a - b) / (a + b)


def paired(runs, metric="rate"):
    if len(runs) != 4 or tuple(r["arm"] for r in runs) != ORDER:
        raise ValueError("measurements must be A1, B1, B2, A2 (ABBA)")
    values = [r[metric] for r in runs]
    if any(not math.isfinite(v) or v <= 0 for v in values):
        raise ValueError(f"invalid {metric}: {values}")
    a1, b1, b2, a2 = values
    a, b = (a1 + a2) / 2, (b1 + b2) / 2
    # Adjacent differences, with both pairs oriented candidate minus reference.
    delta = 100 * ((b1 - a1) + (b2 - a2)) / (a1 + a2)
    return {"metric": metric, "reference": [a1, a2], "candidate": [b1, b2],
            "reference_mean": a, "candidate_mean": b, "delta_pct": delta,
            "pair_deltas_pct": [100 * (b1 - a1) / a1, 100 * (b2 - a2) / a2],
            "reference_spread_pct": spread(a1, a2), "candidate_spread_pct": spread(b1, b2),
            "threshold_pct": spread(a1, a2)}


def assess(cell, rounds):
    current = rounds[-1]
    rate = paired(current["runs"])
    p = paired(current["runs"], "latency_ms") if cell.depth == 1 else rate
    reasons = []
    for name in ("reference", "candidate"):
        if p[f"{name}_spread_pct"] > MAX_SPREAD:
            reasons.append(f"{name} spread exceeds the project's {MAX_SPREAD:g}% stability boundary")
    # Positive loss always means regression, for both throughput and latency.
    loss = p["delta_pct"] if cell.depth == 1 else -p["delta_pct"]
    if loss > p["threshold_pct"]:
        reasons.append("paired regression exceeds measured reference spread")
    gain, plateau_noise = None, None
    if cell.depth > 1:
        if len(rounds) < 2:
            reasons.append("no higher-instance saturation probe")
        else:
            previous = paired(rounds[-2]["runs"])
            fast = max(rate["reference_mean"], rate["candidate_mean"])
            before = max(previous["reference_mean"], previous["candidate_mean"])
            gain = 100 * (fast / before - 1)
            plateau_noise = max(previous["reference_spread_pct"], rate["reference_spread_pct"])
            if previous["reference_spread_pct"] > MAX_SPREAD:
                reasons.append("previous saturation probe was unstable")
            if previous["candidate_spread_pct"] > MAX_SPREAD:
                reasons.append("previous candidate saturation probe was unstable")
            if gain > plateau_noise:
                reasons.append("fastest arm is still gaining with more load instances")
        if any(r["busy_pct"] < MIN_BUSY for r in current["runs"]):
            reasons.append(f"server not at least {MIN_BUSY:g}% busy in every ABBA run")
    return {**p, "throughput": rate, "instances": current["instances"],
            "busy_pct_abba": [r["busy_pct"] for r in current["runs"]],
            "loss_pct": loss, "margin_pct": loss - p["threshold_pct"],
            "fastest_gain_pct": gain, "plateau_noise_pct": plateau_noise,
            "saturation_exempt": cell.depth == 1,
            "verdict": "FAIL" if reasons else "PASS", "reasons": reasons}


def saturation_done(cell, rounds):
    if cell.depth == 1:
        return True
    a = assess(cell, rounds)
    return (a["fastest_gain_pct"] is not None
            and a["fastest_gain_pct"] <= a["plateau_noise_pct"]
            and min(a["busy_pct_abba"]) >= MIN_BUSY
            and a["throughput"]["reference_spread_pct"] <= MAX_SPREAD
            and a["throughput"]["candidate_spread_pct"] <= MAX_SPREAD
            and paired(rounds[-2]["runs"])["reference_spread_pct"] <= MAX_SPREAD
            and paired(rounds[-2]["runs"])["candidate_spread_pct"] <= MAX_SPREAD)


def overall(rows):
    failed = [r for r in rows if r["verdict"] != "PASS"]
    pool = failed or rows
    # A precondition/error failure outranks a throughput win elsewhere.
    worst = max(pool, key=lambda r: (not bool(r.get("assessment")),
                                    r.get("assessment", {}).get("margin_pct", 0)))
    return ("FAIL" if failed else "PASS"), worst["cell"]["id"]


def manifest_reference(directory, commit):
    manifest = directory / "MANIFEST.md"
    if not manifest.is_file():
        return None
    matches = []
    for line in manifest.read_text().splitlines():
        if not line.startswith("|"):
            continue
        hashes = re.findall(r"(?<![0-9a-f])[0-9a-f]{7,40}(?![0-9a-f])", line)
        if not any(commit.startswith(h) for h in hashes):
            continue
        names = re.findall(r"tomokv-[A-Za-z0-9_.-]+", line)
        for name in names:
            binary = directory / name
            if binary.is_file() and os.access(binary, os.X_OK):
                matches.append(binary.resolve())
    matches = sorted(set(matches))
    if len(matches) > 1 and len({sha256(p) for p in matches}) != 1:
        raise Skip(f"ambiguous manifest: multiple different binaries for origin/cpp {commit}")
    return matches[0] if matches else None


class Children:
    def __init__(self):
        self.active = []

    def start(self, argv, log, cwd):
        with log.open("w") as stream:
            p = subprocess.Popen([str(a) for a in argv], cwd=cwd, stdout=stream,
                                 stderr=subprocess.STDOUT, start_new_session=True)
        self.active.append(p)
        return p

    def stop(self, p):
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait(timeout=10)
        if p in self.active:
            self.active.remove(p)

    def close(self):
        for p in list(reversed(self.active)):
            self.stop(p)


def resolve_reference(args, out):
    try:
        commit = git("rev-parse", "--verify", "origin/cpp^{commit}")
    except RuntimeError as e:
        raise Skip(f"no origin/cpp reference: {e}") from e
    binary = manifest_reference(args.bench_bins, commit)
    provenance = {"commit": commit, "ref": "origin/cpp", "manifest": str(args.bench_bins / "MANIFEST.md")}
    if binary:
        provenance.update(source="pinned MANIFEST.md binary", path=str(binary))
    elif args.build_reference:
        src = out / "reference-source"
        src.mkdir()
        archive = out / "reference.tar"
        p = capture(["git", "archive", "--format=tar", "-o", archive, commit])
        if p.returncode:
            raise Skip(f"reference archive unavailable: {p.stdout}")
        with tarfile.open(archive) as tar:
            tar.extractall(src, filter="data")
        argv = ["taskset", "-c", args.server_cores, "make", "-j8"]
        print(f"REFERENCE: no matching pin; building {commit}; log {out / 'reference-build.log'}", flush=True)
        # make owns compiler descendants: its private process group is killed only on abort.
        with (out / "reference-build.log").open("w") as log:
            p = subprocess.Popen(argv, cwd=src, stdout=log, stderr=subprocess.STDOUT,
                                 start_new_session=True)
            try:
                rc = p.wait(timeout=1800)
            except subprocess.TimeoutExpired as e:
                raise Skip("reference build timed out; see reference-build.log") from e
            finally:
                if p.poll() is None:
                    os.killpg(p.pid, signal.SIGKILL)
                    p.wait()
        if rc:
            raise Skip(f"could not build reference {commit}; see reference-build.log")
        binary = src / "build/tomokv"
        provenance.update(source="built origin/cpp", path=str(binary), build_argv=argv,
                          compiler=capture(["g++", "--version"]).stdout.splitlines()[0])
    else:
        raise Skip(f"NO REFERENCE BINARY for origin/cpp {commit}; no matching MANIFEST.md pin; "
                   "enable --build-reference 1 or refresh the pin. This is NOT a pass.")
    provenance["sha256"] = sha256(binary)
    return binary, provenance


def accepted(binary, name, value):
    p = capture([binary, f"--{name}", str(value), "--help"], timeout=10)
    if p.returncode == 0 and "usage:" in p.stdout:
        return True
    if "unknown argument" in p.stdout and f"--{name}" in p.stdout:
        return False
    raise RuntimeError(f"cannot probe {binary.name} --{name}: {p.stdout[:500]}")


def knob_plan(cell, support):
    wanted = {"thread-mode": cell.mode, "read-local": cell.read_local,
              "overlap": cell.overlap, "reorder": cell.reorder}
    plans, notes = {}, []
    for arm in ("A", "B"):
        plans[arm] = {}
        for name, value in wanted.items():
            if support[arm][name]:
                plans[arm][name] = value
            elif arm == "A" and name != "thread-mode":
                notes.append(f"reference predates --{name}; omitted requested {value}; legacy behavior")
            else:
                raise RuntimeError(f"{arm} does not accept required --{name}")
    return plans, notes


def lb_snapshot(conn, path):
    raw = conn.must("DEBUG", "LBSIGNALS")
    if not isinstance(raw, bytes):
        raise RuntimeError("DEBUG LBSIGNALS returned no telemetry")
    path.write_bytes(raw)
    rows = {}
    for line in raw.decode().splitlines():
        f = line.split()
        if f and f[0] == "thread":
            # Schema 1 stable prefix: tid role domain clients iters ops busy_ns idle_ns cpu_ns.
            rows[int(f[1])] = {"role": f[2], "busy": int(f[7]), "idle": int(f[8])}
    if not rows:
        raise RuntimeError("LBSIGNALS has no thread counters; cannot prove saturation")
    return rows


def busy_between(start, end):
    if start.keys() != end.keys():
        raise RuntimeError("thread topology changed during measurement")
    busy, idle, per_thread = 0, 0, {}
    for tid in start:
        b, i = end[tid]["busy"] - start[tid]["busy"], end[tid]["idle"] - start[tid]["idle"]
        if b < 0 or i < 0 or b + i <= 0 or start[tid]["role"] != end[tid]["role"]:
            raise RuntimeError("missing, reset, or changed-role busy counters")
        busy += b
        idle += i
        per_thread[tid] = 100 * b / (b + i)
    return 100 * busy / (busy + idle), per_thread


def info(conn, section):
    raw = conn.must("INFO", section)
    return dict(line.split(":", 1) for line in raw.decode().splitlines() if ":" in line)


def cpu_seconds(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def memtier_totals(path):
    data = json.loads(path.read_text())
    totals = data["ALL STATS"]["Totals"]
    rate, latency = float(totals["Ops/sec"]), float(totals["Latency"])
    if not all(math.isfinite(x) and x > 0 for x in (rate, latency)):
        raise RuntimeError(f"invalid memtier totals in {path}")
    if float(totals.get("Errors", 0)) or float(totals.get("Errors/sec", 0)):
        raise RuntimeError(f"memtier reported errors: {path}")
    return {"rate": rate, "latency_ms": latency}


class Runner:
    def __init__(self, args, out, binaries, children):
        self.args, self.out, self.binaries, self.children = args, out, binaries, children
        self.server_cpus, self.load_cpus = cpus(args.server_cores), cpus(args.load_cores)

    def memtier(self, layout):
        return ["taskset", "-c", cpu_string(layout["cpus"]), self.args.memtier,
                "-s", "127.0.0.1", "-p", str(self.args.port), "--protocol=redis",
                "-t", str(layout["threads"]), "-c", str(layout["clients"]),
                "--key-minimum=1", f"--key-maximum={KEYS}", "--key-pattern=P:P",
                "-d", "64", "--distinct-client-seed", "--hide-histogram"]

    def measure(self, cell, arm, sequence, instances, knobs):
        folder = self.out / cell.id / f"n{instances}-{sequence}-{arm}"
        folder.mkdir(parents=True)
        layout = load_layout(self.load_cpus, instances, cell.conns)
        # Never connect to or terminate an existing listener, even if it speaks TomoKV.
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", self.args.port))
        command = ["taskset", "-c", self.args.server_cores, self.binaries[arm],
                   "--port", str(self.args.port), "--bind", "127.0.0.1", "--atomic", "1",
                   "--enable-debug-command", "yes", "--save", "", "--appendonly", "no",
                   "--dir", str(folder)]
        # Defaults are made explicit so changes to placement cannot masquerade as code gains.
        ex = len(self.server_cpus) // 2
        if cell.mode == "2s":
            command += ["--ratio", f"{len(self.server_cpus) - ex}:{ex}", "--flip-auto", "0"]
        command += ["--shards", str(min(8 * (ex if cell.mode == "2s" else len(self.server_cpus)), 256))]
        for name, value in knobs.items():
            command += [f"--{name}", str(value)]
        started = time.monotonic()
        srv, conn, generators = None, None, []
        log = folder / "server.log"
        result = {"arm": arm, "instances": instances, "complete": False,
                  "server_argv": [str(x) for x in command],
                  "load_layout": layout, "artifacts": str(folder.relative_to(self.out))}
        print(f"  {cell.id} n={instances} {sequence}:{arm} boot/populate/20s", flush=True)
        try:
            srv = self.children.start(command, log, folder)
            deadline = time.monotonic() + 30
            while True:
                if srv.poll() is not None:
                    raise RuntimeError(f"server exited {srv.returncode}: {log.read_text()[-1000:]}")
                try:
                    conn = Conn("127.0.0.1", self.args.port, timeout=10)
                    identity = info(conn, "server")
                    if int(identity["process_id"]) != srv.pid:
                        raise RuntimeError("listener PID is not our child")
                    break
                except (OSError, EOFError):
                    if conn:
                        conn.close()
                        conn = None
                    if time.monotonic() >= deadline:
                        raise RuntimeError("server boot timed out")
                    time.sleep(0.1)
            result["pid"] = srv.pid
            for name, value in {"atomic": 1, **knobs}.items():
                actual = conn.must("CONFIG", "GET", name)
                if actual != [name.encode(), str(value).encode()]:
                    raise RuntimeError(f"boot did not apply {name}={value}: {actual!r}")
            population = self.memtier({"cpus": self.load_cpus, "threads": 8, "clients": 8})
            population += ["--pipeline=32", "--ratio=1:0", "-n", "allkeys"]
            pop = self.children.start(population, folder / "populate.log", folder)
            if pop.wait(timeout=180):
                raise RuntimeError("key population failed")
            self.children.stop(pop)
            if conn.must("DBSIZE") != KEYS:
                raise RuntimeError(f"population did not create exactly {KEYS} keys")
            result["populate_seconds"] = time.monotonic() - started
            for i, placement in enumerate(layout):
                argv = self.memtier(placement) + [f"--pipeline={cell.depth}",
                        "--ratio=" + ("0:1" if cell.op == "GET" else "1:0"),
                        f"--test-time={WARMUP + WINDOW + TAIL}",
                        f"--json-out-file={folder / f'load-{i}.json'}"]
                generators.append(self.children.start(argv, folder / f"load-{i}.log", folder))
                result.setdefault("load_argv", []).append(argv)
            # The counter window excludes setup/teardown and observes the SAME 20 s for all LGs.
            time.sleep(WARMUP)
            if any(p.poll() is not None for p in generators):
                raise RuntimeError("load generator exited before the measurement window")
            if int(info(conn, "clients")["connected_clients"]) != cell.conns + 1:
                raise RuntimeError("not all requested load connections are active")
            before_lb = lb_snapshot(conn, folder / "lb-before.txt")
            if len(before_lb) != len(self.server_cpus):
                raise RuntimeError("server thread count differs from requested CPU geometry")
            roles = {role: sum(row["role"] == role for row in before_lb.values())
                     for role in {row["role"] for row in before_lb.values()}}
            expected_roles = ({"fused": len(self.server_cpus)} if cell.mode == "1s"
                              else {"io": len(self.server_cpus) - ex, "ex": ex})
            if roles != expected_roles:
                raise RuntimeError(f"actual thread roles {roles} differ from {expected_roles}")
            result["thread_roles"] = roles
            before = info(conn, "stats")
            before_cpu, t0 = cpu_seconds(srv.pid), time.monotonic()
            time.sleep(WINDOW)
            after = info(conn, "stats")
            t1, after_cpu = time.monotonic(), cpu_seconds(srv.pid)
            after_lb = lb_snapshot(conn, folder / "lb-after.txt")
            if any(p.poll() is not None for p in generators):
                raise RuntimeError("load generator ended inside the 20-second window")
            if int(info(conn, "clients")["connected_clients"]) != cell.conns + 1:
                raise RuntimeError("load connections disappeared during measurement")
            commands = int(after["total_commands_processed"]) - int(before["total_commands_processed"]) - 1
            if commands <= 0:
                raise RuntimeError("no commands completed")
            misses = int(after["keyspace_misses"]) - int(before["keyspace_misses"])
            if misses != 0:
                raise RuntimeError(f"GETs missed prepopulated keys: {misses}")
            busy, per_thread = busy_between(before_lb, after_lb)
            result.update(rate=commands / (t1 - t0), commands=commands, window_seconds=t1 - t0,
                          midpoint_monotonic=(t0 + t1) / 2, busy_pct=busy, thread_busy_pct=per_thread,
                          cpu_pct=100 * (after_cpu - before_cpu) / ((t1 - t0) * len(self.server_cpus)),
                          info_before=before, info_after=after)
            totals = []
            for i, p in enumerate(generators):
                if p.wait(timeout=30):
                    raise RuntimeError(f"load generator {i} failed; see {folder}")
                totals.append(memtier_totals(folder / f"load-{i}.json"))
            total_rate = sum(t["rate"] for t in totals)
            result.update(complete=True, memtier=totals, memtier_rate=total_rate,
                          latency_ms=sum(t["latency_ms"] * t["rate"] for t in totals) / total_rate)
        except BaseException as e:
            result["error"] = f"{type(e).__name__}: {e}"
            raise
        finally:
            if conn:
                conn.close()
            for p in generators:
                self.children.stop(p)
            if srv:
                self.children.stop(srv)
            result["wall_seconds"] = time.monotonic() - started
            (folder / "measurement.json").write_text(json.dumps(result, indent=2) + "\n")
        print(f"    {result['rate']/1e6:.5f}M/s busy={result['busy_pct']:.3f}% "
              f"CPU={result['cpu_pct']:.3f}% latency={result['latency_ms']:.5f}ms", flush=True)
        return result


def print_cell(row):
    c = row["cell"]
    for note in row.get("notes", []):
        print(f"  {c['id']} COMPATIBILITY: {note}", flush=True)
    if "assessment" not in row:
        print(f"{c['id']} {row['verdict']}: {row['reason']}", flush=True)
        return
    a = row["assessment"]
    units, scale = ("ms (depth-1 latency; saturation exempt)", 1) if c["depth"] == 1 else ("Mops/s", 1e6)
    av, bv = [v / scale for v in a["reference"]], [v / scale for v in a["candidate"]]
    print(f"{c['id']} A={av[0]:.6f},{av[1]:.6f} B={bv[0]:.6f},{bv[1]:.6f} {units} "
          f"paired={a['delta_pct']:+.4f}% spread A/B={a['reference_spread_pct']:.4f}/"
          f"{a['candidate_spread_pct']:.4f}% threshold={a['threshold_pct']:.4f}% "
          f"busy(ABBA)={','.join(f'{x:.3f}' for x in a['busy_pct_abba'])}% "
          f"instances={a['instances']} {a['verdict']}", flush=True)
    if a["fastest_gain_pct"] is not None:
        print(f"  fastest-arm gain at higher instance count={a['fastest_gain_pct']:+.4f}% "
              f"vs measured plateau noise={a['plateau_noise_pct']:.4f}%", flush=True)
    for reason in a["reasons"]:
        print(f"  FAIL: {reason}", flush=True)


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--self-test", action="store_true")
    p.add_argument("--candidate", type=Path, default=Path(os.getenv("GATE_ABBA_CANDIDATE", ROOT / "build/tomokv")))
    p.add_argument("--cells", type=Path, default=Path(os.getenv("GATE_ABBA_CELLS", "/home/user/Projects/headline-cells.txt")))
    p.add_argument("--bench-bins", type=Path, default=Path(os.getenv("GATE_ABBA_BINS", "/home/user/Projects/bench-bins")))
    p.add_argument("--build-reference", type=int, choices=(0, 1), default=int(os.getenv("GATE_ABBA_BUILD_REFERENCE", "1")))
    p.add_argument("--server-cores", default=os.getenv("GATE_ABBA_CORES", "8-31"))
    p.add_argument("--load-cores", default=os.getenv("GATE_ABBA_LOAD_CORES", "64-127,192-255"))
    p.add_argument("--port", type=int, default=int(os.getenv("GATE_ABBA_PORT", "8700")))
    p.add_argument("--memtier", default=os.getenv("GATE_ABBA_MEMTIER", "memtier_benchmark"))
    p.add_argument("--output", type=Path, default=None)
    p.add_argument("--only", default="", help="comma-separated IDs; partial diagnostic, never a full-tier PASS")
    p.add_argument("--max-instances", type=int, choices=(1, 2, 4, 8), default=8,
                   help="bounded doubling search (default 8); 1 cannot prove deep-pipeline saturation")
    return p.parse_args()


def main(args):
    start = time.monotonic()
    out = (args.output or ROOT / "build" / f"abbagate-{time.strftime('%Y%m%d-%H%M%S')}-{os.getpid()}").resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = {"schema": 1, "verdict": "FAIL", "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "window_seconds": WINDOW, "order": ORDER, "cells": [], "output": str(out)}
    children = Children()

    def interrupted(signum, _frame):
        raise InterruptedError(f"interrupted by signal {signum}")

    old_handlers = {sig: signal.getsignal(sig) for sig in (signal.SIGINT, signal.SIGTERM)}
    for sig in old_handlers:
        signal.signal(sig, interrupted)
    try:
        quiet_file = os.getenv("GATE_QUIET_FILE")
        if quiet_file:
            quiet = Path(quiet_file)
            age = time.time() - quiet.stat().st_mtime if quiet.exists() else -1
            if age < 60 * float(os.getenv("GATE_QUIET_MINUTES", "3")):
                raise Skip(f"quiet file {quiet} is absent or too recent; no CPU work started")
        server_cpus, load_cpus = cpus(args.server_cores), cpus(args.load_cores)
        check_placement(server_cpus, load_cpus)
        if not 8700 <= args.port <= 8739:
            raise ValueError("ABBA port must be within 8700-8739")
        cells = read_cells(args.cells)
        report["cell_source"] = {"path": str(args.cells.resolve()), "sha256": sha256(args.cells),
                                 "text": args.cells.read_text(), "total_cells": len(cells)}
        if args.only:
            selected = set(args.only.split(","))
            if selected - {c.id for c in cells}:
                raise ValueError("--only names a cell absent from the supplied headline file")
            cells = [c for c in cells if c.id in selected]
        reference, provenance = resolve_reference(args, out)
        report["reference"] = provenance
        if not args.candidate.is_file() or not os.access(args.candidate, os.X_OK):
            raise RuntimeError(f"candidate executable unavailable: {args.candidate}")
        binaries = {}
        for arm, source in (("A", reference), ("B", args.candidate.resolve())):
            dest = out / f"binary-{arm}"
            shutil.copy2(source, dest)
            binaries[arm] = dest
        report["candidate"] = {"path": str(args.candidate.resolve()), "sha256": sha256(binaries["B"]),
                               "workspace_commit": git("rev-parse", "HEAD"),
                               "workspace_status": git("status", "--short")}
        if sha256(binaries["A"]) != provenance["sha256"]:
            raise RuntimeError("reference changed while copying")
        print(f"REFERENCE {provenance['source']} {provenance['commit']} sha256={provenance['sha256']}", flush=True)
        print(f"CANDIDATE {args.candidate} sha256={report['candidate']['sha256']}", flush=True)
        args.memtier = shutil.which(args.memtier)
        if not args.memtier:
            raise RuntimeError("memtier_benchmark not available")
        args.memtier = str(Path(args.memtier).resolve())
        report["environment"] = {"uname": list(os.uname()), "server_cpus": server_cpus,
                                 "load_cpus": load_cpus, "port": args.port, "keys": KEYS,
                                 "data_bytes": 64, "key_pattern": "P:P", "atomic": 1,
                                 "split_ratio": f"{len(server_cpus)-len(server_cpus)//2}:{len(server_cpus)//2}",
                                 "split_flip_auto": 0, "memtier_path": args.memtier,
                                 "memtier_sha256": sha256(Path(args.memtier)),
                                 "memtier_version": capture([args.memtier, "--version"]).stdout.strip()}
        print(f"GEOMETRY server={args.server_cores} ({len(server_cpus)} cores) load={args.load_cores}; "
              "source headline file records 32 server cores; actual geometry recorded above. "
              "Cell connections are TOTAL, shared across load instances. Split uses fixed even ratio, flip=0.", flush=True)
        support = {arm: {name: accepted(binary, name, value) for name, value in
                        (("thread-mode", "1s"), ("read-local", 0), ("overlap", 0), ("reorder", 0))}
                   for arm, binary in binaries.items()}
        report["accepted_knobs"] = support
        runner = Runner(args, out, binaries, children)
        for cell in cells:
            row = {"cell": asdict(cell), "verdict": "FAIL", "rounds": []}
            report["cells"].append(row)
            try:
                plans, row["notes"] = knob_plan(cell, support)
                row["knobs"] = plans
                for note in row["notes"]:
                    print(f"  {cell.id} COMPATIBILITY: {note}", flush=True)
                for n in (1, 2, 4, 8):
                    if n > args.max_instances or n > cell.conns:
                        break
                    if cell.conns % n:
                        continue
                    round_ = {"instances": n, "runs": []}
                    row["rounds"].append(round_)
                    for sequence, arm in enumerate(ORDER, 1):
                        round_["runs"].append(runner.measure(cell, arm, sequence, n, plans[arm]))
                    row["assessment"] = assess(cell, row["rounds"])
                    row["verdict"] = row["assessment"]["verdict"]
                    print_cell(row)
                    if saturation_done(cell, row["rounds"]):
                        break
            except InterruptedError:
                raise
            except Exception as e:
                row.pop("assessment", None)
                row.update(verdict="FAIL", reason=f"measurement error: {e}")
                print_cell(row)
            finally:
                children.close()
                report["elapsed_seconds"] = time.monotonic() - start
                (out / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        report["verdict"], report["worst_cell"] = overall(report["cells"])
        if args.only and report["verdict"] == "PASS":
            report["verdict"] = "PARTIAL"
        print(f"ABBA {report['verdict']} worst={report['worst_cell']} "
              f"({len(cells)}/{report['cell_source']['total_cells']} cells); results={out / 'results.json'}", flush=True)
        return 1 if report["verdict"] == "FAIL" else 3 if report["verdict"] == "PARTIAL" else 0
    except Skip as e:
        report.update(verdict="SKIP", reason=str(e))
        print(f"ABBA SKIP — NOT A PASS: {e}", file=sys.stderr, flush=True)
        return 3
    except (Exception, KeyboardInterrupt) as e:
        report.update(verdict="FAIL", reason=f"{type(e).__name__}: {e}")
        print(f"ABBA FAIL: {report['reason']}", file=sys.stderr, flush=True)
        return 1
    finally:
        # Complete reaping even if the user presses Ctrl-C again during teardown.
        for sig in (signal.SIGINT, signal.SIGTERM):
            signal.signal(sig, signal.SIG_IGN)
        children.close()
        report["elapsed_seconds"] = time.monotonic() - start
        (out / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        print(f"ABBA elapsed={report['elapsed_seconds']:.1f}s; {out / 'results.json'}", flush=True)
        for sig, handler in old_handlers.items():
            signal.signal(sig, handler)


def self_test():
    import contextlib
    import io
    import unittest
    from unittest import mock
    (ROOT / "build").mkdir(exist_ok=True)

    class ABBA(unittest.TestCase):
        def setUp(self):
            self.cell = Cell("h01", "1s", 1, 0, 0, "GET", 32, 512)

        def round(self, rates, n=1, busy=99.5, latency=None):
            return {"instances": n, "runs": [dict(arm=arm, rate=rate, busy_pct=busy,
                    latency_ms=(latency or [1, 1, 1, 1])[i])
                    for i, (arm, rate) in enumerate(zip(ORDER, rates))]}

        def test_abba_cancels_linear_drift(self):
            p = paired(self.round([100, 100.1, 100.2, 100.3])["runs"])
            self.assertAlmostEqual(p["delta_pct"], 0)
            self.assertAlmostEqual(p["threshold_pct"], 100 * .3 / 100.15)
            self.assertGreater(p["pair_deltas_pct"][0], 0)
            self.assertLess(p["pair_deltas_pct"][1], 0)

        def test_aabb_is_rejected(self):
            runs = self.round([100] * 4)["runs"]
            runs[1]["arm"], runs[3]["arm"] = "A", "B"
            with self.assertRaises(ValueError):
                paired(runs)

        def test_regression_cannot_hide_in_reference_noise(self):
            rounds = [self.round([100, 98, 98, 100.1], n) for n in (1, 2)]
            a = assess(self.cell, rounds)
            self.assertEqual(a["verdict"], "FAIL")
            self.assertGreater(a["loss_pct"], a["threshold_pct"])
            self.assertTrue(saturation_done(self.cell, rounds))

        def test_unsaturated_equal_arms_fail(self):
            rounds = [self.round([100] * 4, n, busy=97.99) for n in (1, 2, 4, 8)]
            self.assertEqual(assess(self.cell, rounds)["verdict"], "FAIL")
            self.assertFalse(saturation_done(self.cell, rounds))

        def test_fastest_arm_still_gaining_fails(self):
            rounds = [self.round([100, 110, 110, 100], 1), self.round([100, 120, 120, 100], 2)]
            self.assertFalse(saturation_done(self.cell, rounds))
            self.assertEqual(assess(self.cell, rounds)["verdict"], "FAIL")

        def test_one_probe_cannot_prove_plateau(self):
            self.assertEqual(assess(self.cell, [self.round([100] * 4)])["verdict"], "FAIL")

        def test_busy_is_required_in_every_run(self):
            rounds = [self.round([100] * 4, n) for n in (1, 2)]
            rounds[1]["runs"][2]["busy_pct"] = 50
            self.assertFalse(saturation_done(self.cell, rounds))

        def test_reference_noise_cannot_turn_a_bad_session_green(self):
            rounds = [self.round([100, 99, 99, 103], n) for n in (1, 2)]
            self.assertEqual(assess(self.cell, rounds)["verdict"], "FAIL")

        def test_stable_equal_arms_pass(self):
            rounds = [self.round([100, 100, 100, 100], n) for n in (1, 2)]
            self.assertEqual(assess(self.cell, rounds)["verdict"], "PASS")
            self.assertEqual(assess(self.cell, rounds)["threshold_pct"], 0)

        def test_depth_one_uses_latency_and_is_exempt(self):
            c = Cell("p1", "2s", 0, 0, 0, "GET", 1, 512)
            a = assess(c, [self.round([100] * 4, busy=10, latency=[1, 1.1, 1.1, 1.001])])
            self.assertEqual(a["verdict"], "FAIL")
            self.assertTrue(a["saturation_exempt"])
            self.assertGreater(a["loss_pct"], 9)
            self.assertEqual(assess(c, [self.round([100] * 4, busy=10)])["verdict"], "PASS")

        def test_worst_failure_not_an_average(self):
            rows = [{"cell": {"id": "slow"}, "verdict": "FAIL", "assessment": {"margin_pct": 3}}]
            rows += [{"cell": {"id": str(i)}, "verdict": "PASS", "assessment": {"margin_pct": -90}}
                     for i in range(31)]
            self.assertEqual(overall(rows), ("FAIL", "slow"))

        def test_historical_numbers_are_not_inputs(self):
            with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
                f = Path(tmp) / "cells"
                f.write_text("h01 | 1s | rl=1 | ov=0 | ro=0 | GET | p32 | 512 | garbage | stale | ignored\n")
                self.assertEqual(read_cells(f), [self.cell])
                f.write_text(f.read_text() * 2)
                with self.assertRaises(ValueError):
                    read_cells(f)

        def test_reference_must_match_last_push(self):
            with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
                directory = Path(tmp)
                binary = directory / "tomokv-old-1234567"
                binary.write_text("old")
                binary.chmod(0o700)
                (directory / "MANIFEST.md").write_text("| `tomokv-old-1234567` | 1234567 | old |\n")
                self.assertIsNone(manifest_reference(directory, "abcdef0" + "0" * 33))
                self.assertEqual(manifest_reference(directory, "1234567" + "0" * 33), binary)

        def test_missing_knobs_are_reported_and_candidate_keeps_them(self):
            support = {arm: {k: True for k in ("thread-mode", "read-local", "overlap", "reorder")}
                       for arm in ("A", "B")}
            support["A"].update(overlap=False, reorder=False)
            plans, notes = knob_plan(self.cell, support)
            self.assertEqual(len(notes), 2)
            self.assertNotIn("overlap", plans["A"])
            self.assertIn("overlap", plans["B"])

        def test_idle_spinning_is_not_busy(self):
            start = {0: {"role": "fused", "busy": 0, "idle": 0}}
            end = {0: {"role": "fused", "busy": 1, "idle": 99}}
            self.assertEqual(busy_between(start, end)[0], 1)
            with self.assertRaises(RuntimeError):
                busy_between(start, start)

        def test_load_escalation_preserves_total_connections(self):
            load = list(range(64, 128)) + list(range(192, 256))
            for n in (1, 2, 4, 8):
                layout = load_layout(load, n, 512)
                self.assertEqual(sum(x["threads"] * x["clients"] for x in layout), 512)
                assigned = [c for x in layout for c in x["cpus"]]
                self.assertEqual(sorted(assigned), load)
                self.assertEqual(len(assigned), len(set(assigned)))

        def test_real_orchestration_order_and_negative_control(self):
            # Exercise the actual driver loop and JSON/exit verdict, replacing only the
            # expensive measurement boundary. Removing comparison or reordering AABB fails.
            with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
                directory = Path(tmp)
                binary = directory / "candidate"
                binary.write_bytes(b"test executable identity; never executed")
                binary.chmod(0o700)
                source = directory / "cells"
                source.write_text("h01 | 1s | rl=1 | ov=0 | ro=0 | GET | p32 | 512 | stale | stale | stale\n")
                for candidate_rate, expected in ((100, 0), (98, 1)):
                    output = directory / str(candidate_rate)
                    argv = ["abbagate.py", "--candidate", str(binary), "--cells", str(source),
                            "--output", str(output), "--memtier", sys.executable, "--max-instances", "2"]
                    with mock.patch.object(sys, "argv", argv):
                        args = parse_args()
                    order = []

                    def measure(_self, cell, arm, sequence, instances, knobs):
                        order.append((instances, arm))
                        return dict(arm=arm, rate=100 if arm == "A" else candidate_rate,
                                    busy_pct=99.9, latency_ms=1)

                    provenance = dict(source="test", commit="0" * 40, sha256=sha256(binary))
                    with mock.patch.object(Runner, "measure", measure), \
                         mock.patch(__name__ + ".resolve_reference", return_value=(binary, provenance)), \
                         mock.patch(__name__ + ".accepted", return_value=True), \
                         mock.patch(__name__ + ".check_placement"), \
                         mock.patch.dict(os.environ, {"GATE_QUIET_FILE": ""}), \
                         contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(main(args), expected)
                    self.assertEqual(order, [(n, a) for n in (1, 2) for a in ORDER])
                    result = json.loads((output / "results.json").read_text())
                    self.assertEqual(result["worst_cell"], "h01")
                    self.assertEqual(result["verdict"], "PASS" if expected == 0 else "FAIL")

        def test_only_owned_children_are_stopped(self):
            with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
                directory = Path(tmp)
                children = Children()
                outsider = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
                try:
                    child = children.start([sys.executable, "-c", "import time; time.sleep(60)"],
                                           directory / "child.log", directory)
                    children.close()
                    self.assertIsNotNone(child.poll())
                    self.assertIsNone(outsider.poll())
                finally:
                    children.close()
                    outsider.terminate()
                    outsider.wait(timeout=10)

    return 0 if unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ABBA)).wasSuccessful() else 1


if __name__ == "__main__":
    args = parse_args()
    sys.exit(self_test() if args.self_test else main(args))
