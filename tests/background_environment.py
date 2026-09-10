#!/usr/bin/env python3
"""Exact identity review shared by operational observation and untrusted diagnostics.

Review permits only the declared existing identity, never its descendants or a
replacement with the same name/UID. CPU activity is recorded, not translated to
performance error. Periodic observations cannot prove absence of brief work.
"""
from __future__ import annotations
import hashlib
import json
import os
from pathlib import Path
import re
import time
import gate_quiet as quiet

CLASSES = {"desktop", "interactive-frontend", "waiting-supervisor", "system-service", "idle-server"}
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


POLICY = "operational-environment-v1"
STRICT = "strict-foreign-activity-v1"
REVIEWED = "reviewed-idle-environment-v1"
FIELDS = ("pid", "start_ticks", "identity", "comm", "parent_pid", "affinity", "classification")
OPTIONAL_FIELDS = ("accept_incomplete_executable", "listener_ports")


def canonical_contract(document):
    rows = []
    if document is not None:
        reviewed_inventory(document)
        for row in document["processes"]:
            if row.get("reviewed") is not True:
                continue
            if (type(row.get("pid")) is not int or row["pid"] <= 0 or
                    type(row.get("start_ticks")) is not int or row["start_ticks"] <= 0 or
                    not isinstance(row.get("comm"), str) or not row["comm"] or
                    type(row.get("parent_pid")) is not int or row["parent_pid"] < 0 or
                    not isinstance(row.get("affinity"), list) or
                    not row["affinity"] or any(type(cpu) is not int or cpu < 0 for cpu in row["affinity"])):
                raise ValueError("reviewed environment requires captured parent and CPU affinity")
            saved = {key: row[key] for key in FIELDS}
            saved["affinity"] = sorted(set(saved["affinity"]))
            saved.update({key: row[key] for key in OPTIONAL_FIELDS if key in row})
            rows.append(saved)
        rows.sort(key=lambda row: (row["pid"], row["start_ticks"]))
    payload = {"policy": STRICT if document is None else REVIEWED, "reviewed_identities": rows}
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    return {**payload, "sha256": hashlib.sha256(encoded).hexdigest()}


def validate_contract(value):
    if not isinstance(value, dict) or set(value) != {"policy", "sha256", "reviewed_identities"}:
        raise ValueError("invalid background environment contract")
    rows = value["reviewed_identities"]
    if not isinstance(rows, list) or value["policy"] not in (STRICT, REVIEWED):
        raise ValueError("unknown background environment contract policy")
    for row in rows:
        if not isinstance(row, dict) or set(row) - set(FIELDS + OPTIONAL_FIELDS) or set(FIELDS) - set(row):
            raise ValueError("invalid reviewed identity contract fields")
    try:
        expected = canonical_contract(None) if value["policy"] == STRICT else canonical_contract(
            {"schema": 1, "processes": [{**row, "reviewed": True} for row in rows]})
    except (KeyError, TypeError) as error:
        raise ValueError("malformed reviewed identity contract") from error
    if value != expected:
        raise ValueError("background environment contract hash/order/fields differ")
    return value


class EnvironmentObserver:
    """An explicit observation contract, not a claim that background work is free.

    Same-process computation with unchanged provenance cannot be distinguished
    from ordinary frontend activity using CPU ticks alone. Review therefore also
    requires the operator to leave those frontends idle; samples retain activity
    for audit, and the complete identical-binary null independently tests the
    instrument. Neither permission nor a passing null proves absolute quiet.
    """
    def __init__(self, path=None):
        self.path = Path(path).resolve() if path else None
        self.raw = self.path.read_bytes() if self.path else None
        self.document = json.loads(self.raw) if self.raw is not None else None
        self.contract = canonical_contract(self.document)
        self.reviewed = reviewed_inventory(self.document) if self.document else {}
        self.rows = {(row["pid"], row["start_ticks"]): row for row in self.contract["reviewed_identities"]}
        self.ports = {port for row in self.rows.values() for port in row.get("listener_ports", [])}
        self.samples = 0
        self.listener_snapshots = 0
        self.last_inspection = None

    def cmdline(self, entry, start):
        try:
            return (entry / "cmdline").read_bytes()
        except PermissionError as error:
            expected = self.reviewed.get((int(entry.name), start), {})
            if (expected.get("provenance") != "incomplete-executable" or
                    expected.get("argv_observation") != denied(error)):
                raise
            return b""

    def inspect(self, before, current, owned):
        result = {"checked_at": time.time(), "identities_checked": 0, "tcp_snapshot": None,
                  "inspecting_identities": []}
        self.last_inspection = result
        # All declared identities stay bound even when their current CPU count is
        # zero. Missing/reused PIDs, exec, UID/permission changes and parent/worker
        # affinity changes cannot inherit the declaration silently.
        for key, expected in self.reviewed.items():
            result["inspecting_identities"] = [key]
            process = current.get(key[0])
            row = self.rows[key]
            if process is None or process.identity != key:
                raise quiet.QuietViolation(f"reviewed PID/start {key} exited or changed")
            if (process.name != row["comm"] or process.parent != row["parent_pid"] or
                    process.affinity != frozenset(row["affinity"])):
                raise quiet.QuietViolation(f"reviewed PID/start {key} comm/parent/affinity changed")
            reader = incomplete_identity if expected.get("provenance") == "incomplete-executable" else identity
            if reader(*key) != expected:
                raise quiet.QuietViolation(f"reviewed PID/start {key} executable/argv/observable identity changed")
            prior = before.get(process.pid)
            if prior and prior.identity == key and process.ticks < prior.ticks:
                raise quiet.QuietViolation(f"reviewed PID/start {key} CPU counter regressed")
            if row["classification"] == "idle-server":
                children = quiet.owned_processes(current, key) - {key}
                if children:
                    result["inspecting_identities"] = sorted(children)
                    raise quiet.QuietViolation(f"reviewed idle-server has unapproved descendants: {sorted(children)}")
            result["identities_checked"] += 1
        if self.ports:
            result["inspecting_identities"] = [key for key,row in self.rows.items()
                                                if row["classification"] == "idle-server"]
            result["tcp_snapshot"] = tcp_snapshot(self.ports)
            self.listener_snapshots += 1
            check_idle_connections(result["tcp_snapshot"], self.ports)
        result["inspecting_identities"] = []
        # QuietMonitor independently rejects known experiments even between CPU
        # bursts and any CPU-active unreviewed child. No descendant is inserted
        # into self.reviewed by this inspection.
        self.samples += 1
        return result

    def evidence(self, sample_artifact=None):
        return {"contract": self.contract,
                "source": {"path": str(self.path) if self.path else None,
                           "sha256": hashlib.sha256(self.raw).hexdigest() if self.raw is not None else None},
                "reviewed_inventory": self.document, "sample_artifact": str(sample_artifact) if sample_artifact else None,
                "sample_count": self.samples, "listener_snapshots": self.listener_snapshots,
                "limitation": "Exact review is not permission for foreground computation; unchanged-process work and brief between-snapshot processes/TCP traffic may be unobserved. CPU ticks are observations, never a throughput-error bound."}
