#!/usr/bin/env python3
"""On-demand, permanently untrusted qualification of an explicitly reviewed background.

First capture an inventory, review exact identities, and mark selected records reviewed=true
with classification desktop / interactive-frontend / waiting-supervisor. A system-service
whose exe link is permission-denied may instead be explicitly reviewed with
accept_incomplete_executable=true: only its observable PID/start/UID/comm/permission states
and argv digest are then bound. Executable path and bytes remain UNKNOWN. This weaker option
exists only for this permanently untrusted diagnostic; approval never includes descendants.
An explicitly reviewed idle-server also needs listener_ports. Its exact identity and every
observed TCP state on those ports are checked, and its CPU ticks retain the normal hard
rolling budget. Periodic TCP snapshots cannot exclude brief traffic between observations.
`run` captures 120 seconds before any boot, executes exactly one pinned ABBA block each for
h01/h09/h12 through abbagate.main(), then captures 60 seconds after the last measurement.
The normal generic CPU budget is only audited here; its crossings remain would-refuse events.
Known competing experiments, unreviewed server work, changed/unreviewed active identities and
observer errors still abort. No rate threshold, load pin or normal gate policy is relaxed.
Even twelve successful measurements cannot certify this operating environment: a separate
all-cell standing null is still required. These artifacts can never serve as that null.
"""
from __future__ import annotations

import argparse
from collections import deque
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import threading
import time

import abbagate as abba
import gate_quiet as quiet

CELLS = ("h01", "h09", "h12")
CLASSES = {"desktop", "interactive-frontend", "waiting-supervisor", "system-service", "idle-server"}
PREFLIGHT = 120
POSTFLIGHT = 60
_DIGESTS = {}


def file_identity(path):
    before = path.stat()
    fields = {"device": before.st_dev, "inode": before.st_ino, "size": before.st_size,
              "mtime_ns": before.st_mtime_ns, "ctime_ns": before.st_ctime_ns}
    key = tuple(fields.values())
    if key not in _DIGESTS:
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
            after = os.fstat(stream.fileno())
        if (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns) != key:
            raise quiet.QuietViolation(f"identity file changed while hashing: {path}")
        _DIGESTS[key] = digest.hexdigest()
    return {**fields, "sha256": _DIGESTS[key]}


def script_identity(entry, executable, argv):
    name = Path(executable).name
    python = re.fullmatch(r"python[0-9.]*", name)
    shell = name in ("bash", "sh", "dash", "zsh", "ksh")
    if not python and not shell:
        return None
    arguments = [os.fsdecode(value) for value in argv.split(b"\0") if value][1:]
    while arguments:
        argument = arguments.pop(0)
        if argument == "--":
            break
        if argument == "-c" or shell and re.fullmatch(r"-[a-zA-Z]*c[a-zA-Z]*", argument):
            return None  # The complete inline body is already bound by the argv digest.
        if python and argument in ("-m", "-"):
            raise quiet.QuietViolation("reviewed interpreter module/stdin source cannot be resolved exactly")
        if python and argument in ("-W", "-X"):
            if not arguments:
                raise quiet.QuietViolation("incomplete interpreter option")
            arguments.pop(0)
            continue
        if not argument.startswith("-"):
            arguments.insert(0, argument)
            break
        allowed = (re.fullmatch(r"-[uBEIOPsSvq]+|-W.+|-X.+", argument) if python else
                   re.fullmatch(r"-[a-zA-Z]+|--noprofile|--norc", argument))
        if not allowed:
            raise quiet.QuietViolation(f"cannot resolve reviewed interpreter option {argument}")
    if not arguments:
        return None  # Interactive/waiting interpreter without a script argument.
    script = Path(arguments[0])
    path = entry / "root" / str(script).lstrip("/") if script.is_absolute() else entry / "cwd" / script
    return {"path": str(path.resolve()), **file_identity(path)}


def identity(pid, expected_start=None, proc_root=Path("/proc")):
    entry = proc_root / str(pid)
    def start():
        return int((entry / "stat").read_text().rsplit(")", 1)[1].split()[19])
    before = start()
    target = os.readlink(entry / "exe")
    executable = file_identity(entry / "exe")
    argv = (entry / "cmdline").read_bytes()
    script = script_identity(entry, target, argv)
    if start() != before or (expected_start is not None and before != expected_start):
        raise quiet.QuietViolation(f"PID {pid} changed while reading its identity")
    return {"pid": pid, "start_ticks": before, "uid": entry.stat().st_uid,
            "exe": {"path": target, **executable}, "script": script,
            "argv_sha256": hashlib.sha256(argv).hexdigest()}


def denied(error):
    return {"status": "permission-denied", "error_type": type(error).__name__, "errno": error.errno}


def incomplete_identity(pid, expected_start=None, proc_root=Path("/proc")):
    entry = proc_root / str(pid)
    def state():
        raw = (entry / "stat").read_text()
        comm, fields = raw.split("(", 1)[1].rsplit(")", 1)
        return comm, int(fields.split()[19])
    comm, start = state()
    uids = [line.split()[1:] for line in (entry / "status").read_text().splitlines() if line.startswith("Uid:")]
    if len(uids) != 1 or len(uids[0]) != 4:
        raise quiet.QuietViolation(f"PID {pid} UID fields are not observable")
    try:
        os.readlink(entry / "exe")
    except PermissionError as error:
        observation = denied(error)
    else:
        raise quiet.QuietViolation(f"PID {pid} executable link is now readable; incomplete identity changed")
    try:
        argv = (entry / "cmdline").read_bytes()
        command = {"status": "readable", "bytes": len(argv), "sha256": hashlib.sha256(argv).hexdigest()}
    except PermissionError as error:
        command = denied(error)
    result = {"provenance": "incomplete-executable", "pid": pid, "start_ticks": start,
              "comm": comm, "proc_directory_uid": entry.stat().st_uid, "status_uids": list(map(int, uids[0])),
              "exe": None, "executable_observation": observation, "argv_observation": command,
              "limitation": "executable path, bytes and script provenance are unobserved"}
    if state() != (comm, start) or expected_start is not None and expected_start != start:
        raise quiet.QuietViolation(f"PID {pid} changed while reading its incomplete identity")
    return result


def inventory_cmdline(entry, start):
    try:
        return (entry / "cmdline").read_bytes()
    except PermissionError:
        # Capture only: no approval or activity exemption follows from missing argv.
        # The incomplete identity explicitly records this permission failure.
        return b""


def inventory():
    rows = []
    for row in sorted(quiet.snapshot(cmdline_reader=inventory_cmdline).values(), key=lambda row: row.pid):
        if row.kernel_thread:
            continue  # PF_KTHREAD accounting remains separately visible in every run.
        record = {"pid": row.pid, "start_ticks": row.start, "parent_pid": row.parent,
                  "comm": row.name, "cpu_ticks": row.ticks, "affinity": sorted(row.affinity),
                  "reviewed": False, "classification": None, "identity": None}
        try:
            record["identity"] = identity(row.pid, row.start)
        except PermissionError as error:
            record["identity_error"] = str(error)
            try:
                record["identity"] = incomplete_identity(row.pid, row.start)
                record["accept_incomplete_executable"] = False
            except (OSError, quiet.QuietViolation) as observed_error:
                record["incomplete_identity_error"] = str(observed_error)
        except (OSError, quiet.QuietViolation) as error:
            record["identity_error"] = str(error)
        rows.append(record)
    return {"schema": 1, "captured_at": time.time(), "processes": rows}


def reviewed_inventory(document):
    if document.get("schema") != 1 or not isinstance(document.get("processes"), list):
        raise ValueError("invalid reviewed background inventory")
    reviewed, pids, ports = {}, set(), set()
    for row in document["processes"]:
        if row.get("reviewed") is not True:
            continue
        value = row.get("identity")
        idle_server = row.get("classification") == "idle-server"
        if idle_server:
            declared = row.get("listener_ports")
            if (row.get("comm") not in quiet.SERVERS or not isinstance(declared, list) or not declared or
                    any(type(port) is not int or not 0 < port < 65536 for port in declared) or
                    len(set(declared)) != len(declared) or ports.intersection(declared) or
                    type(row.get("parent_pid")) is not int or not isinstance(row.get("affinity"), list) or
                    not row["affinity"] or any(type(cpu) is not int or cpu < 0 for cpu in row["affinity"])):
                raise ValueError(f"PID {row.get('pid')} lacks distinct explicitly reviewed idle-server listener ports")
            ports.update(declared)
        elif "listener_ports" in row:
            raise ValueError("listener_ports requires explicit idle-server classification")
        if isinstance(value, dict) and value.get("provenance") == "incomplete-executable":
            command = value.get("argv_observation", {})
            if (row.get("classification") not in ("system-service", "idle-server") or row.get("accept_incomplete_executable") is not True or
                    value.get("pid") != row.get("pid") or value.get("start_ticks") != row.get("start_ticks") or
                    value.get("comm") != row.get("comm") or value.get("exe", "missing") is not None or
                    value.get("executable_observation", {}).get("status") != "permission-denied" or
                    len(value.get("status_uids", [])) != 4 or command.get("status") not in ("readable", "permission-denied") or
                    command["status"] == "readable" and not re.fullmatch(r"[0-9a-f]{64}", command.get("sha256", ""))):
                raise ValueError(f"PID {row.get('pid')} lacks explicit review of complete observable service fields")
            if row["pid"] in pids or row.get("comm") in quiet.COMPETING and not idle_server:
                raise ValueError(f"duplicate or competing reviewed PID {row['pid']}")
            pids.add(row["pid"])
            reviewed[row["pid"], row["start_ticks"]] = value
            continue
        if (row.get("classification") not in CLASSES or not isinstance(value, dict) or
                value.get("pid") != row.get("pid") or value.get("start_ticks") != row.get("start_ticks") or
                not isinstance(value.get("exe"), dict) or not value["exe"].get("path") or
                not isinstance(value["exe"].get("ctime_ns"), int) or
                not re.fullmatch(r"[0-9a-f]{64}", value["exe"].get("sha256", "")) or
                not re.fullmatch(r"[0-9a-f]{64}", value.get("argv_sha256", ""))):
            raise ValueError(f"reviewed PID {row.get('pid')} lacks an exact readable identity/classification")
        if row["pid"] in pids:
            raise ValueError(f"duplicate reviewed PID {row['pid']}")
        names = {row.get("comm"), Path(value["exe"]["path"]).name}
        if names & quiet.ACTIVE_EXPERIMENTS or names & quiet.SERVERS and not idle_server:
            raise ValueError(f"PID {row['pid']} is a server/compiler/generator, not idle background")
        pids.add(row["pid"])
        reviewed[row["pid"], row["start_ticks"]] = value
    if not reviewed:
        raise ValueError("no exact background identities have been explicitly reviewed")
    return reviewed


def tcp_snapshot(ports, net_root=Path("/proc/net")):
    """Read only: never connect to an unrelated listener to test its identity.

    Retain IPv4/IPv6 rows whose local OR remote port is reviewed. TIME_WAIT and
    CLOSED are recorded remnants, not live connections; every other non-LISTEN
    state (including handshakes and draining connections) invalidates the run.
    These namespace-wide records cannot bind an unreadable server fd to a port,
    and two snapshots cannot prove that no short connection occurred between them.
    """
    result = {"started_monotonic": time.monotonic(), "namespace": os.readlink("/proc/self/ns/net"),
              "ports": sorted(ports), "rows": []}
    for protocol in ("tcp", "tcp6"):
        lines = (net_root / protocol).read_text().splitlines()
        if not lines or "local_address" not in lines[0]:
            raise quiet.QuietViolation(f"unreadable TCP snapshot header: {protocol}")
        for raw in lines[1:]:
            fields = raw.split()
            try:
                local = int(fields[1].rsplit(":", 1)[1], 16)
                remote = int(fields[2].rsplit(":", 1)[1], 16)
                state = int(fields[3], 16)
                inode, uid = int(fields[9]), int(fields[7])
                if not 1 <= state <= 12:
                    raise ValueError("unknown TCP state")
            except (ValueError, IndexError) as error:
                raise quiet.QuietViolation(f"malformed {protocol} socket record: {raw}") from error
            if local in ports or remote in ports:
                result["rows"].append({"protocol": protocol, "local_port": local, "remote_port": remote,
                    "state": state, "uid": uid, "inode": inode, "raw": raw})
    result["ended_monotonic"] = time.monotonic()
    return result


def check_idle_connections(snapshot, ports):
    live = [row for row in snapshot["rows"] if row["state"] not in (10, 6, 7)]
    if live:
        raise quiet.QuietViolation(f"reviewed idle-server has observed non-listener live TCP connection: {live}")
    listening = {row["local_port"] for row in snapshot["rows"] if row["state"] == 10}
    if ports - listening:
        raise quiet.QuietViolation(f"reviewed idle-server listener disappeared: {sorted(ports - listening)}")


def summary(document):
    for row in document["processes"]:
        value = row.get("identity")
        description = ("INCOMPLETE executable provenance; " + str(value["executable_observation"])
                       if value and value.get("provenance") == "incomplete-executable" else
                       value["exe"]["path"] if value else "UNKNOWN: " + row.get("identity_error", "unreadable"))
        print(f"{'REVIEWED' if row.get('reviewed') else 'unreviewed':10} "
              f"{row['pid']}:{row['start_ticks']} {row['comm']} ticks={row['cpu_ticks']} "
              f"{description}")


class QualificationMonitor(quiet.QuietMonitor):
    def _snapshot(self):
        def read_cmdline(entry, start):
            try:
                return (entry / "cmdline").read_bytes()
            except PermissionError as error:
                expected = self.reviewed.get((int(entry.name), start), {})
                if (expected.get("provenance") != "incomplete-executable" or
                        expected.get("argv_observation") != denied(error)):
                    raise
                # Only an explicitly reviewed, already-unreadable argv may stay unreadable.
                # Known comms still trigger the observer; new children/identities remain foreign.
                return b""
        return quiet.snapshot(cmdline_reader=read_cmdline)

    def __init__(self, *args, reviewed, document, output, **kwargs):
        self.reviewed = reviewed
        self.document = document
        self.phase = "preflight"
        self.lock = threading.RLock()
        self.would_refuse = []
        self.idle_servers = {(row["pid"], row["start_ticks"]): row["listener_ports"]
            for row in document["processes"] if row.get("reviewed") is True and row.get("classification") == "idle-server"}
        self.idle_windows = deque()
        self.idle_peak_seconds = 0.0
        self.preflight_complete = False
        self.postflight_seconds = None
        self.sample_path = output / "background-samples.jsonl"
        self.sample_path.touch(exist_ok=False)
        super().__init__(*args, **kwargs)

    def set_phase(self, phase):
        with self.lock:
            self.observe()  # Charge the complete interval before changing its label.
            self.check()
            self.phase = phase

    def sample(self):
        with self.lock:
            before, began = self.previous, self.previous_at
            super().sample()
            after = self.previous
            activity = quiet.interference(before, after, self.root, self.cpus, self.ancestors, self.helpers)
            kernel = quiet.interference(before, after, self.root, self.cpus,
                                        self.ancestors, self.helpers, kernel_only=True)
            # Only this diagnostic subclass may audit the generic numeric heuristic. The
            # normal observer's active-experiment and unknown-state failures stay hard.
            failure = self.failure
            generic = (failure and "error" not in failure and failure.get("processes") and
                       all(row["reason"] == "foreign CPU activity" for row in failure["processes"]))
            if generic:
                self.would_refuse.append({"phase": self.phase, **failure})
                self.failure = None
            owned = quiet.owned_processes(after, self.root) | self.ancestors | self.helpers.keys()
            active = {(row["pid"], row["start_ticks"]) for row in activity if row["cpu_ticks"]}
            sockets, server_activity = None, []
            try:
                for key, expected in self.reviewed.items():
                    current = after.get(key[0])
                    if current is None or current.identity != key:
                        raise quiet.QuietViolation(f"reviewed PID/start {key} exited or changed")
                    reader = incomplete_identity if expected.get("provenance") == "incomplete-executable" else identity
                    if reader(current.pid, current.start) != expected:
                        raise quiet.QuietViolation(f"reviewed PID {current.pid} observed identity changed")
                if self.idle_servers:
                    # Qualification02 refused one 10ms Garnet housekeeping tick
                    # after four idle days. Only explicitly reviewed PID/start +
                    # executable/argv (or declared incomplete provenance) + ports
                    # get this diagnostic-only treatment. No name/UID exemption,
                    # no descendants, and no server CPU budget relaxation follows.
                    ports = {port for declared in self.idle_servers.values() for port in declared}
                    sockets = tcp_snapshot(ports)
                    for key in self.idle_servers:
                        current, prior = after[key[0]], before.get(key[0])
                        declaration = next(row for row in self.document["processes"]
                                           if (row["pid"], row["start_ticks"]) == key)
                        if (current.name != declaration["comm"] or current.parent != declaration["parent_pid"] or
                                current.affinity != frozenset(declaration["affinity"])):
                            raise quiet.QuietViolation(f"reviewed idle-server comm/parent/affinity changed: {key}")
                        ticks = current.ticks - prior.ticks if prior and prior.identity == key else current.ticks
                        if ticks < 0:
                            raise quiet.QuietViolation(f"reviewed idle-server CPU counter regressed: {key}")
                        server_activity.append({"pid": key[0], "start_ticks": key[1], "cpu_ticks": ticks})
                        descendants = quiet.owned_processes(after, key) - {key}
                        if descendants:
                            raise quiet.QuietViolation(f"reviewed idle-server has unapproved descendants: {sorted(descendants)}")
                    self.idle_windows.append((began, self.previous_at, server_activity))
                    while self.idle_windows and self.idle_windows[0][1] <= self.previous_at - self.window_seconds:
                        self.idle_windows.popleft()
                    server_seconds = sum(row["cpu_ticks"] for _, _, rows in self.idle_windows
                                         for row in rows) * self.tick_seconds
                    sample_seconds = sum(row["cpu_ticks"] for row in server_activity) * self.tick_seconds
                    self.idle_peak_seconds = max(self.idle_peak_seconds, server_seconds, sample_seconds)
                    if max(server_seconds, sample_seconds) > self.cpu_budget_seconds:
                        raise quiet.QuietViolation(f"reviewed idle-server CPU budget exceeded: "
                            f"{max(server_seconds, sample_seconds):.6f}s > {self.cpu_budget_seconds:.6f}s per {self.window_seconds:g}s")
                    check_idle_connections(sockets, ports)
                for row in after.values():
                    if row.kernel_thread or row.identity in owned:
                        continue
                    if row.name in quiet.ACTIVE_EXPERIMENTS or row.experiment_driver:
                        raise quiet.QuietViolation(f"active foreign experiment PID {row.pid} ({row.name})")
                    if row.identity not in active:
                        continue
                    if row.name in quiet.SERVERS and row.identity not in self.idle_servers:
                        raise quiet.QuietViolation(f"foreign server/experiment PID {row.pid} is CPU-active")
                    if row.identity not in self.reviewed:
                        known = next((item for item in self.document["processes"]
                                      if (item["pid"], item["start_ticks"]) == row.identity), {})
                        detail = known.get("identity_error", "identity not explicitly reviewed")
                        raise quiet.QuietViolation(f"unreviewed CPU-active PID/start {row.identity} ({row.name}): "
                                                   f"{detail}; background cannot be qualified")
            except (OSError, quiet.QuietViolation) as error:
                self.failure = self.failure or {"observed_at": time.time(), "error": str(error)}
            event = {"phase": self.phase, "started_monotonic": began, "ended_monotonic": self.previous_at,
                     "user_cpu_activity": activity, "kernel_cpu_activity": kernel,
                     "rolling_cpu_seconds": sum(row["cpu_ticks"] for _, _, rows in self.activity_windows
                                                for row in rows) * self.tick_seconds,
                     "cpu_budget_seconds": self.cpu_budget_seconds,
                     "idle_server_cpu_activity": server_activity, "idle_server_tcp_snapshot": sockets,
                     "would_refuse": failure if generic else None, "hard_failure": self.failure}
            with self.sample_path.open("a") as stream:
                stream.write(json.dumps(event, sort_keys=True) + "\n")
                stream.flush()
                os.fsync(stream.fileno())

    def capture(self, seconds):
        started = time.monotonic()
        self.observe()
        self.check()
        while time.monotonic() < started + seconds:
            time.sleep(max(0, min(self.interval, started + seconds - time.monotonic())))
            self.observe()
            self.check()
        return time.monotonic() - started

    def preflight(self):
        self.preflight_seconds = self.capture(PREFLIGHT)
        self.preflight_complete = True
        self.phase = "setup"
        return self.evidence()

    def close(self):
        if not self.closed:
            self.stop_event.set()
            if self.thread is not None:
                self.thread.join()
            if self.preflight_complete and self.failure is None:
                self.phase = "postflight"
                try:
                    self.postflight_seconds = self.capture(POSTFLIGHT)
                except quiet.QuietViolation:
                    pass  # Main's final check retains the hard failure and exits red.
            super().close()
        return self.evidence()

    def evidence(self):
        result = super().evidence()
        result.update(complete=False, diagnostic_complete=self.closed and self.failure is None,
                      scope="background-qualification; never eligible for a standing null or gate receipt",
                      postflight_seconds=self.postflight_seconds, would_refuse=self.would_refuse,
                      reviewed_inventory=self.document, sample_artifact=str(self.sample_path))
        result["idle_server_screening"] = {"identities_and_ports": [
            {"pid": key[0], "start_ticks": key[1], "listener_ports": ports} for key, ports in self.idle_servers.items()],
            "peak_rolling_cpu_seconds": self.idle_peak_seconds, "cpu_budget_seconds": self.cpu_budget_seconds,
            "window_seconds": self.window_seconds,
            "limitation": "periodic namespace TCP snapshots cannot exclude brief traffic between samples or prove socket-to-PID attribution; incomplete executable identity remains unknown"}
        return result


def measurement_windows(report, events):
    windows = []
    for row in report.get("cells", []):
        for block in row["rounds"]:
            for run in block["runs"]:
                if not run.get("complete") or "midpoint_monotonic" not in run:
                    continue
                begin = run["midpoint_monotonic"] - run["window_seconds"] / 2
                end = run["midpoint_monotonic"] + run["window_seconds"] / 2
                selected = [(index, event) for index, event in enumerate(events, 1)
                            if event["started_monotonic"] < end and event["ended_monotonic"] > begin]
                users = {}
                for _, event in selected:
                    for process in event["user_cpu_activity"]:
                        key = process["pid"], process["start_ticks"]
                        saved = users.setdefault(key, {**process, "cpu_ticks": 0})
                        saved["cpu_ticks"] += process["cpu_ticks"]
                windows.append({"cell": row["cell"]["id"], "artifacts": run["artifacts"],
                    "started_monotonic": begin, "ended_monotonic": end,
                    "sample_lines": [index for index, _ in selected],
                    "foreign_user_activity": list(users.values()),
                    "accounting": "full CPU delta for every partially overlapping sample; conservative edge bound"})
    return windows


def run(args):
    inventory_bytes = args.inventory.read_bytes()
    document = json.loads(inventory_bytes)
    reviewed = reviewed_inventory(document)
    # Use the real parser and real loop, but offer no arbitrary passthrough that could change
    # the experiment's cells, pins, window, arm identity, population, escalation or sequence.
    argv = ["abbagate.py", "--collect-null", "1", "--build-reference", "0", "--subset", "full",
            "--only", ",".join(CELLS), "--candidate", str(args.candidate), "--output", str(args.output),
            "--cells", str(args.cells), "--memtier", args.memtier]
    for key in ("server_cores", "load_cores", "server_smt", "load_smt", "ports", "port"):
        value = getattr(args, key)
        if value is not None:
            argv += ["--" + key.replace("_", "-"), str(value)]
    original = sys.argv
    try:
        sys.argv = argv
        options = abba.parse_args()
    finally:
        sys.argv = original
    cells = abba.selected_cells(abba.read_cells(options.cells), "full", options.only)
    if (tuple(cell.id for cell in cells) != CELLS or
            any(cell.depth <= 1 or not cell.instances for cell in cells)):
        raise ValueError("qualification requires h01/h09/h12 in source order, each with a pinned deep-pipeline floor")
    if abba.WINDOW != 20 or abba.ORDER != ("A", "B", "B", "A"):
        raise ValueError("qualification design requires the unchanged 20s ABBA measurement")
    def monitor(*positional, **keywords):
        output = options.output.resolve()
        (output / "reviewed-background.json").write_bytes(inventory_bytes)
        return QualificationMonitor(*positional, reviewed=reviewed, document=document, output=output, **keywords)
    rc = abba.main(options, diagnostic_monitor=monitor, diagnostic_profile=getattr(args, "profile", 0))
    report = json.loads((options.output / "results.json").read_text())
    sample_file = options.output / "background-samples.jsonl"
    events = [json.loads(line) for line in sample_file.read_text().splitlines()] if sample_file.exists() else []
    complete = (len(report.get("cells", [])) == 3 and
        [row["cell"]["id"] for row in report["cells"]] == list(CELLS) and
        all(len(row["rounds"]) == 1 and row["rounds"][0]["instances"] == row["cell"]["instances"] and
            [run["arm"] for run in row["rounds"][0]["runs"]] == list(abba.ORDER) and
            all(run.get("complete") is True for run in row["rounds"][0]["runs"]) for row in report["cells"]))
    evidence = {"run_kind": "background-qualification", "normal_gate_eligible": False,
        "driver_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "reviewed_inventory_sha256": hashlib.sha256(inventory_bytes).hexdigest(),
        "expected_complete_measurements": 12, "exact_pinned_measurements_complete": complete,
        "observed_complete_measurements": sum(run.get("complete") is True for row in report.get("cells", [])
                                               for block in row["rounds"] for run in block["runs"]),
        "windows": measurement_windows(report, events)}
    (options.output / "qualification-windows.json").write_text(json.dumps(evidence, indent=2) + "\n")
    if not complete:
        print("BACKGROUND QUALIFICATION INCOMPLETE: expected exactly 3 pinned ABBA blocks / 12 complete measurements", file=sys.stderr)
        return 1
    return rc


def self_test():
    import contextlib
    from dataclasses import replace
    import io
    import tempfile
    from types import SimpleNamespace
    import unittest
    from unittest import mock
    from abba_evidence import validate_null

    def metadata(pid=20, start=3):
        return {"pid": pid, "start_ticks": start, "uid": 1000,
                "exe": {"path": "/test/frontend", "device": 1, "inode": pid, "size": 10,
                        "mtime_ns": 1, "ctime_ns": 1, "sha256": "a" * 64}, "script": None,
                "argv_sha256": "a" * 64}

    def document():
        return {"schema": 1, "captured_at": 1, "processes": [
            {"pid": 20, "start_ticks": 3, "comm": "frontend", "cpu_ticks": 0,
             "reviewed": True, "classification": "interactive-frontend", "identity": metadata()}]}

    def weak_metadata():
        return {"provenance": "incomplete-executable", "pid": 20, "start_ticks": 3, "comm": "frontend",
                "proc_directory_uid": 0, "status_uids": [0, 0, 0, 0], "exe": None,
                "executable_observation": denied(PermissionError(13, "permission denied")),
                "argv_observation": {"status": "readable", "bytes": 32, "sha256": "a" * 64},
                "limitation": "executable path, bytes and script provenance are unobserved"}

    def weak_document():
        value = document()
        value["processes"][0].update(classification="system-service", accept_incomplete_executable=True,
                                      identity=weak_metadata())
        return value

    def idle_document(weak=False):
        value = weak_document() if weak else document()
        row = value["processes"][0]
        row.update(classification="idle-server", comm="memcached" if weak else "GarnetServer",
                   listener_ports=[11211] if weak else [8590], parent_pid=1, affinity=list(range(64)))
        if weak:
            row["identity"]["comm"] = row["comm"]
        else:
            row["identity"]["exe"]["path"] = "/test/GarnetServer"
        return value

    def idle_sockets(ports=(8590,), state=10):
        return {"started_monotonic": 0, "ended_monotonic": 0, "namespace": "net:[123]",
                "ports": list(ports), "rows": [{"local_port": port, "remote_port": 0,
                                                "state": state} for port in ports]}

    class Controls(unittest.TestCase):
        def test_incomplete_reader_records_denial_without_inventing_an_executable(self):
            with tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                entry = root / "20"
                entry.mkdir()
                fields = ["0"] * 20
                fields[19] = "3"
                (entry / "stat").write_text("20 (systemd-logind) " + " ".join(fields))
                (entry / "status").write_text("Uid:\t0\t0\t0\t0\n")
                (entry / "cmdline").write_bytes(b"/usr/lib/systemd/systemd-logind\0")
                with mock.patch.object(os, "readlink", side_effect=PermissionError(13, "permission denied")):
                    observed = incomplete_identity(20, 3, root)
                    self.assertIsNone(observed["exe"])
                    self.assertEqual(observed["executable_observation"]["errno"], 13)
                    self.assertEqual(observed["status_uids"], [0] * 4)
                    self.assertEqual(observed["comm"], "systemd-logind")
                    self.assertEqual(observed["argv_observation"]["sha256"],
                                     hashlib.sha256((entry / "cmdline").read_bytes()).hexdigest())
                with mock.patch.object(os, "readlink", return_value="/now/readable"):
                    with self.assertRaisesRegex(quiet.QuietViolation, "now readable"):
                        incomplete_identity(20, 3, root)

        def test_incomplete_service_requires_explicit_acceptance_and_never_accepts_servers(self):
            value = weak_document()
            self.assertEqual(reviewed_inventory(value)[20, 3]["provenance"], "incomplete-executable")
            for field, replacement in (("reviewed", False), ("accept_incomplete_executable", False),
                                       ("classification", "desktop")):
                mutated = weak_document()
                mutated["processes"][0][field] = replacement
                with self.subTest(field=field), self.assertRaises(ValueError):
                    reviewed_inventory(mutated)
            for name in ("GarnetServer", "memcached", "cc1plus"):
                mutated = weak_document()
                mutated["processes"][0]["comm"] = name
                mutated["processes"][0]["identity"]["comm"] = name
                with self.subTest(name=name), self.assertRaises(ValueError):
                    reviewed_inventory(mutated)

        def test_exact_incomplete_service_can_be_observed_but_remains_untrusted(self):
            with self.fixture() as (monitor, rows, clock), \
                 mock.patch(__name__ + ".incomplete_identity", return_value=weak_metadata()) as read:
                monitor.document = weak_document()
                monitor.reviewed = reviewed_inventory(monitor.document)
                clock[0] = 20
                rows[20] = replace(rows[20], ticks=201)
                monitor.sample()
                monitor.check()
                self.assertEqual(read.call_count, 1)
                self.assertFalse(monitor.evidence()["complete"])
                self.assertEqual(len(monitor.would_refuse), 1)

        def test_any_incomplete_observation_change_is_hard(self):
            for field, replacement in (("status_uids", [1] * 4), ("comm", "changed"),
                    ("executable_observation", denied(PermissionError(1, "different denial"))),
                    ("argv_observation", denied(PermissionError(13, "now unreadable"))),
                    ("argv_observation", {"status": "readable", "bytes": 32, "sha256": "b" * 64})):
                with self.subTest(field=field), self.fixture() as (monitor, rows, clock), \
                     mock.patch(__name__ + ".incomplete_identity", return_value={**weak_metadata(), field: replacement}):
                    monitor.document = weak_document()
                    monitor.reviewed = reviewed_inventory(monitor.document)
                    monitor.sample()
                    with self.assertRaisesRegex(quiet.QuietViolation, "observed identity changed"):
                        monitor.check()

        def test_unreadable_argv_is_allowed_only_when_that_exact_state_was_reviewed(self):
            with self.fixture() as (monitor, rows, clock), \
                 mock.patch.object(Path, "read_bytes", side_effect=PermissionError(13, "denied")), \
                 mock.patch.object(quiet, "snapshot", side_effect=lambda **kw: kw["cmdline_reader"](Path("/proc/20"), 3)):
                value = weak_document()
                value["processes"][0]["identity"]["argv_observation"] = denied(PermissionError(13, "denied"))
                monitor.reviewed = reviewed_inventory(value)
                self.assertEqual(monitor._snapshot(), b"")
                monitor.reviewed = reviewed_inventory(weak_document())  # Previously readable.
                with self.assertRaises(PermissionError):
                    monitor._snapshot()
                monitor.reviewed = reviewed_inventory(document())  # Full identity cannot downgrade.
                with self.assertRaises(PermissionError):
                    monitor._snapshot()

        def test_script_content_change_is_detected_at_same_path_and_same_argv(self):
            with tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                entry = root / "99"
                entry.mkdir()
                (entry / "exe").symlink_to(Path(sys.executable).resolve())
                (entry / "root").symlink_to("/")
                (entry / "cwd").symlink_to(root)
                fields = ["0"] * 20
                fields[19] = "123"
                (entry / "stat").write_text("99 (python3) " + " ".join(fields))
                script = root / "supervisor.py"
                script.write_text("print(1)\n")
                (entry / "cmdline").write_bytes(os.fsencode(sys.executable) + b"\0" + os.fsencode(script) + b"\0")
                before = identity(99, 123, root)
                original = script.stat()
                script.write_text("print(2)\n")
                os.utime(script, ns=(original.st_atime_ns, original.st_mtime_ns))
                after = identity(99, 123, root)
                self.assertEqual(before["argv_sha256"], after["argv_sha256"])
                self.assertEqual(before["script"]["mtime_ns"], after["script"]["mtime_ns"])
                self.assertNotEqual(before["script"]["sha256"], after["script"]["sha256"])

        @contextlib.contextmanager
        def fixture(self, declaration=None):
            declaration = declaration or document()
            clock = [0.0]
            rows = {10: quiet.Process(10, 1, 1, "python3", 10, frozenset(range(64))),
                    20: quiet.Process(20, 3, 1, declaration["processes"][0]["comm"], 100, frozenset(range(64)))}
            with tempfile.TemporaryDirectory() as tmp, \
                 mock.patch.object(quiet, "snapshot", side_effect=lambda **kwargs: dict(rows)), \
                 mock.patch.object(quiet, "read_topology", side_effect=lambda cpus: {c: frozenset([c]) for c in cpus}), \
                 mock.patch.object(time, "monotonic", side_effect=lambda: clock[0]), \
                 mock.patch(__name__ + ".identity", side_effect=lambda pid, start:
                            declaration["processes"][0]["identity"] if pid == 20 else metadata(pid, start)):
                monitor = QualificationMonitor(list(range(32)), list(range(32, 64)), own_root_pid=10,
                    reviewed=reviewed_inventory(declaration), document=declaration, output=Path(tmp))
                yield monitor, rows, clock

        def test_idle_server_requires_explicit_ports_and_identity_review(self):
            for weak in (False, True):
                self.assertIn((20, 3), reviewed_inventory(idle_document(weak)))
                for field, replacement in (("reviewed", False), ("listener_ports", []),
                        ("listener_ports", [0]), ("listener_ports", [True]), ("listener_ports", [8590, 8590]),
                        ("classification", "desktop"), ("comm", "cc1plus")):
                    value = idle_document(weak)
                    value["processes"][0][field] = replacement
                    with self.subTest(weak=weak, field=field, replacement=replacement), self.assertRaises(ValueError):
                        reviewed_inventory(value)
            value = idle_document(True)
            value["processes"][0]["accept_incomplete_executable"] = False
            with self.assertRaises(ValueError):
                reviewed_inventory(value)

        def test_single_reviewed_server_tick_retained_full_and_incomplete(self):
            for weak in (False, True):
                value = idle_document(weak)
                with self.subTest(weak=weak), self.fixture(value) as (monitor, rows, clock), \
                     mock.patch(__name__ + ".incomplete_identity", return_value=value["processes"][0]["identity"]), \
                     mock.patch(__name__ + ".tcp_snapshot", return_value=idle_sockets(value["processes"][0]["listener_ports"])):
                    rows[20] = replace(rows[20], ticks=101)
                    clock[0] = 1
                    monitor.sample()
                    monitor.check()
                    event = json.loads(monitor.sample_path.read_text())
                    self.assertEqual(event["user_cpu_activity"][0]["cpu_ticks"], 1)
                    self.assertEqual(event["idle_server_cpu_activity"][0]["cpu_ticks"], 1)
                    self.assertEqual(event["idle_server_tcp_snapshot"]["rows"][0]["state"], 10)
                    self.assertAlmostEqual(monitor.evidence()["idle_server_screening"]["peak_rolling_cpu_seconds"], .01)
                    self.assertFalse(monitor.evidence()["complete"])

        def test_idle_server_budget_is_hard_and_aggregates_partial_windows(self):
            with self.fixture(idle_document()) as (monitor, rows, clock), \
                 mock.patch(__name__ + ".tcp_snapshot", return_value=idle_sockets()):
                clock[0], rows[20] = 19, replace(rows[20], ticks=190)
                monitor.sample()
                monitor.check()
                clock[0], rows[20] = 21, replace(rows[20], ticks=200)
                monitor.sample()
                with self.assertRaisesRegex(quiet.QuietViolation, "idle-server CPU budget exceeded"):
                    monitor.check()
                self.assertAlmostEqual(monitor.idle_peak_seconds, 1.0)
                # Hard failure remains latched even after the offending window expires.
                clock[0] = 100
                monitor.sample()
                with self.assertRaises(quiet.QuietViolation):
                    monitor.check()

        def test_two_idle_servers_share_one_budget(self):
            value = idle_document()
            second = {**value["processes"][0], "pid": 21, "start_ticks": 4,
                      "listener_ports": [11211], "identity": metadata(21, 4)}
            value["processes"].append(second)
            with self.fixture(value) as (monitor, rows, clock), \
                 mock.patch(__name__ + ".tcp_snapshot", return_value=idle_sockets((8590, 11211))):
                # Capture both existing servers before charging their CPU deltas.
                rows[21] = quiet.Process(21, 4, 1, "GarnetServer", 0, frozenset(range(64)))
                monitor.sample()
                clock[0] = 1
                rows[20], rows[21] = replace(rows[20], ticks=160), replace(rows[21], ticks=60)
                monitor.sample()
                with self.assertRaisesRegex(quiet.QuietViolation, "1.200000s > 0.960000s"):
                    monitor.check()
                events = [json.loads(line) for line in monitor.sample_path.read_text().splitlines()]
                self.assertEqual([row["cpu_ticks"] for row in events[-1]["idle_server_cpu_activity"]], [60, 60])

        def test_idle_server_port_states_descendants_and_identity_are_hard(self):
            for state in (1, 2, 3, 4, 5, 8, 9, 11, 12):
                with self.subTest(state=state), self.fixture(idle_document()) as (monitor, rows, clock), \
                     mock.patch(__name__ + ".tcp_snapshot", return_value=idle_sockets(state=state)):
                    monitor.sample()
                    with self.assertRaisesRegex(quiet.QuietViolation, "non-listener live TCP"):
                        monitor.check()
                    self.assertEqual(json.loads(monitor.sample_path.read_text())["idle_server_tcp_snapshot"]["rows"][0]["state"], state)
            for change in ("child", "identity", "parent", "affinity", "comm", "compiler", "driver"):
                with self.subTest(change=change), self.fixture(idle_document()) as (monitor, rows, clock), \
                     mock.patch(__name__ + ".tcp_snapshot", return_value=idle_sockets()):
                    if change == "child":
                        rows[30] = quiet.Process(30, 4, 20, "sleep", 0, frozenset([0]))
                    elif change == "compiler":
                        rows[30] = quiet.Process(30, 4, 1, "cc1plus", 0, frozenset([0]))
                    elif change == "driver":
                        rows[30] = quiet.Process(30, 4, 1, "python3", 0, frozenset([0]), experiment_driver=True)
                    else:
                        fields = {"identity": {"start": 99}, "parent": {"parent": 99},
                                  "affinity": {"affinity": frozenset([1])}, "comm": {"name": "changed"}}
                        rows[20] = replace(rows[20], **fields[change])
                    monitor.sample()
                    with self.assertRaises(quiet.QuietViolation):
                        monitor.check()

        def test_idle_listener_disappearance_snapshot_error_and_exec_change_are_hard(self):
            for error in (False, True):
                with self.subTest(error=error), self.fixture(idle_document()) as (monitor, rows, clock), \
                     mock.patch(__name__ + ".tcp_snapshot", side_effect=PermissionError("TCP unobserved") if error else None,
                                return_value={**idle_sockets(), "rows": []}):
                    monitor.sample()
                    with self.assertRaises(quiet.QuietViolation):
                        monitor.check()
            with self.fixture(idle_document()) as (monitor, rows, clock), \
                 mock.patch(__name__ + ".identity", return_value=metadata()), \
                 mock.patch(__name__ + ".tcp_snapshot", return_value=idle_sockets()):
                monitor.sample()
                with self.assertRaisesRegex(quiet.QuietViolation, "observed identity changed"):
                    monitor.check()

        def test_tcp_parser_reads_both_families_and_remote_ports_without_connecting(self):
            with tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                header = "  sl  local_address rem_address st tx_queue tr tm->when retrnsmt uid timeout inode\n"
                def record(local, remote, state, inode=123):
                    return f"0: 00000000:{local:04X} 00000000:{remote:04X} {state:02X} 00000000:00000000 00:0 0 1000 0 {inode}\n"
                (root / "tcp").write_text(header + record(8590, 0, 10) + record(8590, 42, 6))
                (root / "tcp6").write_text(header + record(1234, 8590, 1) + record(4321, 999, 1))
                observed = tcp_snapshot({8590}, root)
                self.assertEqual(len(observed["rows"]), 3)
                self.assertEqual({row["protocol"] for row in observed["rows"]}, {"tcp", "tcp6"})
                with self.assertRaisesRegex(quiet.QuietViolation, "non-listener live TCP"):
                    check_idle_connections(observed, {8590})
                observed["rows"] = [row for row in observed["rows"] if row["state"] != 1]
                check_idle_connections(observed, {8590})  # TIME_WAIT retained, not a live peer.
                (root / "tcp6").write_text(header + "malformed\n")
                with self.assertRaisesRegex(quiet.QuietViolation, "malformed tcp6"):
                    tcp_snapshot({8590}, root)

        def test_reviewed_ticks_cross_budget_but_never_certify_quiet(self):
            with self.fixture() as (monitor, rows, clock):
                clock[0] = 20
                rows[20] = replace(rows[20], ticks=201)
                monitor.sample()
                monitor.check()
                self.assertEqual(len(monitor.would_refuse), 1)
                self.assertAlmostEqual(monitor.would_refuse[0]["rolling_cpu_seconds"], 1.01)
                self.assertAlmostEqual(monitor.cpu_budget_seconds, .96)
                self.assertFalse(monitor.evidence()["complete"])
                event = json.loads(monitor.sample_path.read_text())
                self.assertEqual(event["user_cpu_activity"][0]["cpu_ticks"], 101)
                self.assertIsNotNone(event["would_refuse"])

        def test_unreviewed_child_does_not_inherit_approval(self):
            with self.fixture() as (monitor, rows, clock):
                rows[30] = quiet.Process(30, 4, 20, "worker", 1, frozenset([0]))
                clock[0] = 1
                monitor.sample()
                with self.assertRaisesRegex(quiet.QuietViolation, "unreviewed CPU-active"):
                    monitor.check()

        def test_sleeping_compiler_and_active_servers_are_always_hard(self):
            for name, ticks in (("cc1plus", 0), ("GarnetServer", 1), ("memcached", 1)):
                with self.subTest(name=name), self.fixture() as (monitor, rows, clock):
                    rows[30] = quiet.Process(30, 4, 1, name, ticks, frozenset([0]))
                    clock[0] = 1
                    monitor.sample()
                    with self.assertRaises(quiet.QuietViolation):
                        monitor.check()

        def test_reused_pid_and_changed_executable_argv_are_hard(self):
            with self.fixture() as (monitor, rows, clock):
                rows[20] = replace(rows[20], start=99)
                monitor.sample()
                with self.assertRaisesRegex(quiet.QuietViolation, "exited or changed"):
                    monitor.check()
            with self.fixture() as (monitor, rows, clock), \
                 mock.patch(__name__ + ".identity", return_value={**metadata(), "argv_sha256": "b" * 64}):
                monitor.sample()
                with self.assertRaisesRegex(quiet.QuietViolation, "observed identity changed"):
                    monitor.check()

        def test_unreadable_reviewed_identity_and_observer_error_stay_hard(self):
            with self.fixture() as (monitor, rows, clock), \
                 mock.patch(__name__ + ".identity", side_effect=PermissionError("unreadable exe")):
                monitor.sample()
                with self.assertRaisesRegex(quiet.QuietViolation, "unreadable exe"):
                    monitor.check()
            with self.fixture() as (monitor, rows, clock), \
                 mock.patch.object(quiet, "snapshot", side_effect=PermissionError("unknown observer")):
                monitor.observe()
                with self.assertRaisesRegex(quiet.QuietViolation, "unknown observer"):
                    monitor.check()

        def test_inventory_retains_unknown_system_identity_without_approving_it(self):
            process = quiet.Process(33, 4, 1, "system-service", 1, frozenset([0]))
            with mock.patch.object(quiet, "snapshot", return_value={33: process}), \
                 mock.patch(__name__ + ".identity", side_effect=PermissionError("exe unreadable")):
                capture = inventory()
            self.assertEqual(len(capture["processes"]), 1)
            self.assertFalse(capture["processes"][0]["reviewed"])
            self.assertIsNone(capture["processes"][0]["identity"])
            capture["processes"][0].update(reviewed=True, classification="desktop")
            with self.assertRaises(ValueError):
                reviewed_inventory(capture)

        def test_server_cannot_be_reviewed_as_desktop(self):
            for name in ("GarnetServer", "memcached", "cc1plus"):
                value = document()
                value["processes"][0]["comm"] = name
                with self.subTest(name=name), self.assertRaises(ValueError):
                    reviewed_inventory(value)

        def test_preflight_and_postflight_capture_actual_fixed_lengths(self):
            with self.fixture() as (monitor, rows, clock), \
                 mock.patch.object(time, "sleep", side_effect=lambda seconds: clock.__setitem__(0, clock[0] + seconds)):
                monitor.preflight()
                self.assertEqual(monitor.preflight_seconds, 120)
                monitor.close()
                self.assertEqual(monitor.postflight_seconds, 60)
                self.assertTrue(monitor.evidence()["diagnostic_complete"])
                self.assertFalse(monitor.evidence()["complete"])
                events = [json.loads(line) for line in monitor.sample_path.read_text().splitlines()]
                self.assertEqual({e["phase"] for e in events}, {"preflight", "postflight"})

        def test_window_summary_charges_both_partial_edges_in_full(self):
            report = {"cells": [{"cell": {"id": "h01"}, "rounds": [{"runs": [{
                "complete": True, "midpoint_monotonic": 20, "window_seconds": 20, "artifacts": "h01/n4-1-A"}]}]}]}
            def event(a, b, ticks):
                return {"started_monotonic": a, "ended_monotonic": b, "user_cpu_activity": [
                    {"pid": 20, "start_ticks": 3, "cpu_ticks": ticks}]}
            result = measurement_windows(report, [event(8, 11, 2), event(11, 29, 3), event(29, 31, 4), event(31, 32, 99)])
            self.assertEqual(result[0]["sample_lines"], [1, 2, 3])
            self.assertEqual(result[0]["foreign_user_activity"][0]["cpu_ticks"], 9)

        def test_profile_cli_is_numeric_and_default_off(self):
            argv = ["background_qualification.py", "run", "--inventory", "x", "--candidate", "y", "--output", "z"]
            for flags, expected in (([], 0), (["--profile", "1"], 1)):
                with mock.patch.object(sys, "argv", argv + flags):
                    self.assertEqual(parse_args().profile, expected)
            with mock.patch.object(sys, "argv", argv + ["--profile", "2"]), \
                 contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                parse_args()

        def test_profiled_real_loop_stays_permanently_untrusted(self):
            self.test_real_main_takes_exactly_twelve_measurements_and_never_writes_trusted_evidence(profile=1)

        def test_real_main_takes_exactly_twelve_measurements_and_never_writes_trusted_evidence(self, profile=0):
            with tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                candidate = root / "candidate"
                candidate.write_bytes(b"never executed")
                candidate.chmod(0o700)
                manifest = root / "inventory.json"
                manifest.write_text(json.dumps(document()))
                cells = root / "cells"
                cells.write_text("".join(f"{name} | 1s | rl=1 | ov=0 | ro=0 | GET | p32 | 512 | - | - | 4\n" for name in CELLS))
                args = SimpleNamespace(inventory=manifest, candidate=candidate, output=root / "output", cells=cells,
                    profile=profile, memtier=sys.executable, server_cores="0-31", load_cores="32-63", server_smt="", load_smt="", ports=None, port="9079")
                phases, calls, writes = [], [], []
                fake = SimpleNamespace(start=lambda: None, check=lambda: None, set_phase=phases.append,
                    evidence=lambda: {"complete": False, "scope": "background-qualification"},
                    close=lambda: {"complete": False, "scope": "background-qualification"})
                def measure(runner, cell, arm, sequence, instances, knobs):
                    if profile:
                        from abba_profile import WindowProfile
                        self.assertIs(runner.profile_factory, WindowProfile)
                    else:
                        self.assertIsNone(runner.profile_factory)
                    calls.append((cell.id, instances, sequence, arm))
                    return {"arm": arm, "rate": 100, "busy_pct": 99.9, "latency_ms": 1,
                            "instances": instances, "load_layout": abba.load_layout(runner.load_cpus, instances, cell.conns),
                            "complete": True, "midpoint_monotonic": 20 + len(calls) * 30,
                            "window_seconds": 20, "artifacts": f"{cell.id}/n{instances}-{sequence}-{arm}"}
                real_write = Path.write_text
                def write(path, value, *args, **kwargs):
                    if path.name == "results.json":
                        record = json.loads(value)
                        self.assertEqual(record["run_kind"], "background-qualification")
                        self.assertFalse(record["normal_gate_eligible"])
                        self.assertFalse(record["measurement_valid"])
                        self.assertFalse(record["comparison_trusted"])
                        self.assertNotEqual(record.get("null_control", {}).get("verdict"), "PASS")
                        writes.append(record)
                    return real_write(path, value, *args, **kwargs)
                with mock.patch(__name__ + ".QualificationMonitor", return_value=fake), \
                     mock.patch.object(abba.Runner, "measure", measure), \
                     mock.patch.object(abba, "accepted", return_value=True), \
                     mock.patch.object(abba, "check_placement"), \
                     mock.patch.object(os, "sched_setaffinity"), \
                     mock.patch.object(abba, "null_result", side_effect=AssertionError("diagnostic called null_result")), \
                     mock.patch.object(Path, "write_text", write), \
                     mock.patch.dict(os.environ, {"GATE_QUIET_FILE": ""}), contextlib.redirect_stdout(io.StringIO()):
                    rc = run(args)
                self.assertEqual(rc, 3)
                self.assertEqual(calls, [(cell, 4, sequence, arm) for cell in CELLS for sequence, arm in enumerate(abba.ORDER, 1)])
                self.assertEqual(len([phase for phase in phases if phase.startswith("measurement:")]), 12)
                self.assertGreaterEqual(len(writes), 4)
                result = writes[-1]
                self.assertEqual(result.get("cpu_profile_requested", False), bool(profile))
                self.assertEqual(result["statistical_verdict"], "PASS")
                self.assertEqual(result["null_control"]["verdict"], "UNTRUSTED")
                counts = json.loads((args.output / "qualification-windows.json").read_text())
                self.assertTrue(counts["exact_pinned_measurements_complete"])
                self.assertEqual(counts["observed_complete_measurements"], 12)
                with self.assertRaises(ValueError):
                    validate_null(result, now=time.time())

    suite = unittest.defaultTestLoader.loadTestsFromTestCase(Controls)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        raise SystemExit(1)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    capture = commands.add_parser("inventory", help="read-only identity capture; approves no processes")
    capture.add_argument("--output", required=True, type=Path)
    show = commands.add_parser("summary", help="print reviewed identities without exposing argv bodies")
    show.add_argument("inventory", type=Path)
    launch = commands.add_parser("run", help="diagnostic only: 120s + three ABBA blocks + 60s")
    launch.add_argument("--inventory", required=True, type=Path)
    launch.add_argument("--candidate", required=True, type=Path)
    launch.add_argument("--output", required=True, type=Path)
    launch.add_argument("--cells", type=Path, default=abba.ROOT / "tests/headline_cells.txt")
    launch.add_argument("--memtier", default="memtier_benchmark")
    launch.add_argument("--profile", type=int, choices=(0, 1), default=0,
                        help="1 records owned task/PMC diagnostics; remains ineligible as gate/null evidence")
    for name in ("server-cores", "load-cores", "server-smt", "load-smt", "ports", "port"):
        launch.add_argument("--" + name)
    commands.add_parser("self-test", help="serverless identity and real-loop controls")
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()
    if args.command == "inventory":
        document = inventory()
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x") as stream:
            json.dump(document, stream, indent=2)
            stream.write("\n")
        summary(document)
    elif args.command == "summary":
        summary(json.loads(args.inventory.read_text()))
    elif args.command == "self-test":
        self_test()
    else:
        sys.exit(run(args))
