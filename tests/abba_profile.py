#!/usr/bin/env python3
"""Opt-in CPU diagnostics for owned ABBA children; never gate or null evidence.

The grouped counters cover each server TID's user+kernel execution, excluding
hypervisor execution and other tasks such as ksoftirqd. Their windows encompass
the central command interval, but are NOT aligned with it. Ratios using central
commands or the wider schedstat endpoints are explicitly approximate. IPC and
cycles/reference-cycles use counters from the same group window.
"""
import array
import ctypes
import fcntl
import json
import os
from pathlib import Path
import platform
import struct
import threading
import time


class ProfileError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise ProfileError(message)


# Linux UAPI, /usr/include/linux/perf_event.h and x86_64 asm/unistd_64.h.
# PERF_ATTR_SIZE_VER0 is sufficient: counting only, without sample/mmap buffers.
class PerfAttr(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint32), ("size", ctypes.c_uint32),
                ("config", ctypes.c_uint64), ("sample_period", ctypes.c_uint64),
                ("sample_type", ctypes.c_uint64), ("read_format", ctypes.c_uint64),
                ("flags", ctypes.c_uint64), ("wakeup_events", ctypes.c_uint32),
                ("bp_type", ctypes.c_uint32), ("config1", ctypes.c_uint64)]


EVENTS = (("cycles", 0), ("instructions", 1), ("ref_cycles", 9))
READ_FORMAT = 1 | 2 | 4 | 8  # TOTAL_TIME_ENABLED, TOTAL_TIME_RUNNING, ID, GROUP
IOC_ENABLE, IOC_DISABLE, IOC_RESET, IOC_ID = 0x2400, 0x2401, 0x2403, 0x80082407
IOC_GROUP = 1


def decode_group(raw, ids):
    require(len(raw) == 24 + 16 * len(ids), "short/unsupported perf group read")
    values = struct.unpack("=" + "Q" * (len(raw) // 8), raw)
    count, enabled, running = values[:3]
    require(count == len(ids), "perf group event count changed")
    found = {}
    for value, ident in zip(values[3::2], values[4::2]):
        require(ident in ids and ident not in found, "unknown/duplicate perf event identity")
        found[ident] = value
    return dict(raw_hex=raw.hex(), time_enabled_ns=enabled, time_running_ns=running,
                events=[dict(id=ident, name=ids[ident], value=value) for ident, value in found.items()])


def perf_delta(before, after):
    old = {row["id"]: row for row in before["events"]}
    new = {row["id"]: row for row in after["events"]}
    require(old.keys() == new.keys(), "perf event identity changed")
    enabled = after["time_enabled_ns"] - before["time_enabled_ns"]
    running = after["time_running_ns"] - before["time_running_ns"]
    require(enabled >= 0 and running >= 0, "perf timing counter reset")
    # Never scale multiplexed counts into supposedly exact observations. Zero/zero
    # is permitted for an idle TID, whose group never acquired on-CPU execution.
    require(enabled == running, f"perf group multiplexed/unavailable: enabled={enabled}, running={running}")
    result = {}
    for ident, row in new.items():
        require(row["name"] == old[ident]["name"], "perf event name changed")
        value = row["value"] - old[ident]["value"]
        require(value >= 0, "perf event counter reset")
        result[row["name"]] = value
    require(running > 0 or not any(result.values()), "perf values without running time")
    return dict(time_enabled_ns=enabled, time_running_ns=running, counts=result)


class PerfGroup:
    def __init__(self, tid):
        require(platform.machine() == "x86_64" and ctypes.sizeof(PerfAttr) == 64,
                "unsupported perf syscall/attribute ABI")
        self.fds, self.ids, self.enabled = [], {}, False
        self.record = dict(tid=tid, status="INCOMPLETE", events=[])
        libc = ctypes.CDLL(None, use_errno=True)
        libc.syscall.restype = ctypes.c_long
        try:
            for name, config in EVENTS:
                # Only the leader is pinned and disabled. No inheritance: attach
                # to these exact owned TIDs, not future descendants or whole CPUs.
                attr = PerfAttr(type=0, size=64, config=config, read_format=READ_FORMAT,
                                flags=(1 | 4 | 64) if not self.fds else 64)
                fd = libc.syscall(ctypes.c_long(298), ctypes.byref(attr), ctypes.c_int(tid),
                                  ctypes.c_int(-1), ctypes.c_int(self.fds[0] if self.fds else -1),
                                  ctypes.c_ulong(8))  # PERF_FLAG_FD_CLOEXEC
                if fd < 0:
                    code = ctypes.get_errno()
                    raise OSError(code, f"perf_event_open tid={tid} {name}: {os.strerror(code)}")
                self.fds.append(fd)
                ident = array.array("Q", [0])
                fcntl.ioctl(fd, IOC_ID, ident, True)
                require(ident[0] not in self.ids, "duplicate kernel perf event ID")
                self.ids[ident[0]] = name
                self.record["events"].append(dict(name=name, config=config, id=ident[0]))
        except BaseException as error:
            self.record.update(status="INVALID", error=str(error))
            self.close()
            error.profile_record = self.record
            raise

    def read(self):
        return decode_group(os.read(self.fds[0], 24 + 16 * len(self.ids)), self.ids)

    def begin(self):
        fcntl.ioctl(self.fds[0], IOC_RESET, IOC_GROUP)
        self.record["before"] = self.read()
        self.record["enable_before_monotonic"] = time.monotonic()
        fcntl.ioctl(self.fds[0], IOC_ENABLE, IOC_GROUP)
        self.enabled = True
        self.record["enable_after_monotonic"] = time.monotonic()

    def disable(self):
        if self.enabled:
            self.record["disable_before_monotonic"] = time.monotonic()
            fcntl.ioctl(self.fds[0], IOC_DISABLE, IOC_GROUP)
            self.record["disable_after_monotonic"] = time.monotonic()
            self.enabled = False

    def finish(self):
        self.record["after"] = self.read()
        self.record["delta"] = perf_delta(self.record["before"], self.record["after"])
        self.record["status"] = "COMPLETE"

    def close(self):
        errors = []
        try:
            self.disable()
        except OSError as error:
            errors.append(str(error))
        for fd in reversed(self.fds):
            try:
                os.close(fd)
            except OSError as error:
                errors.append(str(error))
        self.fds.clear()
        if errors:
            self.record.update(status="INVALID", cleanup_errors=errors)


def parse_stat(raw):
    end = raw.rfind(")")
    require(end > 0, "malformed proc stat")
    fields = raw[end + 1:].split()
    require(len(fields) >= 37, "short proc stat")
    return dict(pid=int(raw.split("(", 1)[0]), comm=raw[raw.index("(") + 1:end],
                state=fields[0], ppid=int(fields[1]), user_ticks=int(fields[11]),
                system_ticks=int(fields[12]), start_ticks=int(fields[19]), last_cpu=int(fields[36]))


def parse_cpus(value):
    result = []
    for part in value.split(","):
        pair = part.split("-")
        first, last = int(pair[0]), int(pair[-1])
        require(len(pair) <= 2 and 0 <= first <= last, "malformed task CPU affinity")
        result.extend(range(first, last + 1))
    require(result and len(result) == len(set(result)), "missing/duplicate task CPU affinity")
    return result


def exe_identity(path):
    target = os.readlink(path)
    st = path.stat()
    return dict(path=target, device=st.st_dev, inode=st.st_ino, size=st.st_size,
                mtime_ns=st.st_mtime_ns, ctime_ns=st.st_ctime_ns)


def process_snapshot(process, record, *, proc=Path("/proc")):
    """Update record incrementally so even a disappearing task leaves raw evidence."""
    require(process.poll() is None, f"owned process {process.pid} exited before CPU snapshot")
    path = proc / str(process.pid)
    record.update(pid=process.pid, before_monotonic=time.monotonic(), threads={})
    record["raw_stat"] = (path / "stat").read_text()
    record["stat"] = parse_stat(record["raw_stat"])
    require(record["stat"]["pid"] == process.pid and
            (process.pid == os.getpid() or record["stat"]["ppid"] == os.getpid()),
            "profile target is not this driver's own process or direct child")
    record["executable"] = exe_identity(path / "exe")
    tids = sorted(int(entry.name) for entry in (path / "task").iterdir() if entry.name.isdecimal())
    require(tids, "owned process has no observable TIDs")
    for tid in tids:
        task = path / "task" / str(tid)
        row = record["threads"][str(tid)] = dict(before_monotonic=time.monotonic())
        for name in ("stat", "status", "schedstat", "sched"):
            row["raw_" + name] = (task / name).read_text()
        row["stat"] = parse_stat(row["raw_stat"])
        status = dict(line.split(":", 1) for line in row["raw_status"].splitlines() if ":" in line)
        require(int(status["Pid"]) == tid and int(status["Tgid"]) == process.pid and row["stat"]["pid"] == tid,
                "task PID/TGID changed during CPU snapshot")
        row["uids"] = [int(value) for value in status["Uid"].split()]
        row["cpus_allowed"] = parse_cpus(status["Cpus_allowed_list"].strip())
        values = [int(value) for value in row["raw_schedstat"].split()]
        require(len(values) == 3 and min(values) >= 0, "invalid schedstat endpoint")
        row["schedstat"] = dict(zip(("runtime_ns", "runqueue_wait_ns", "timeslices"), values))
        sched = {key.strip(): value.strip() for key, value in
                 (line.split(":", 1) for line in row["raw_sched"].splitlines() if ":" in line)}
        row["scheduler"] = {key: int(sched[key].strip()) for key in
                            ("se.nr_migrations", "nr_switches", "nr_voluntary_switches", "nr_involuntary_switches")}
        require(parse_stat((task / "stat").read_text())["start_ticks"] == row["stat"]["start_ticks"],
                "task identity changed during CPU snapshot")
        row["after_monotonic"] = time.monotonic()
    require(tids == sorted(int(entry.name) for entry in (path / "task").iterdir() if entry.name.isdecimal()),
            "task set changed during CPU snapshot")
    require(parse_stat((path / "stat").read_text())["start_ticks"] == record["stat"]["start_ticks"] and
            exe_identity(path / "exe") == record["executable"] and process.poll() is None,
            "process identity changed during CPU snapshot")
    record["after_monotonic"] = time.monotonic()


def task_deltas(before, after, *, wait_available):
    require(before["pid"] == after["pid"] and before["stat"]["start_ticks"] == after["stat"]["start_ticks"] and
            before["executable"] == after["executable"], "profile process identity changed")
    require(before["threads"].keys() == after["threads"].keys(), "profile task set changed")
    result = {}
    for tid, first in before["threads"].items():
        last = after["threads"][tid]
        require(first["stat"]["start_ticks"] == last["stat"]["start_ticks"] and first["uids"] == last["uids"],
                "profile task identity changed")
        require(first["cpus_allowed"] == last["cpus_allowed"], "task affinity changed during profile")
        changes = {}
        for section, keys in (("stat", ("user_ticks", "system_ticks")),
                              ("schedstat", ("runtime_ns", "runqueue_wait_ns", "timeslices")),
                              ("scheduler", tuple(first["scheduler"]))):
            for key in keys:
                delta = last[section][key] - first[section][key]
                require(delta >= 0, f"task {tid} {key} counter reset")
                changes[key] = delta
        changes["runqueue_wait_ns"] = changes["runqueue_wait_ns"] if wait_available else None
        changes.update(cpus_allowed=first["cpus_allowed"], last_cpu_before=first["stat"]["last_cpu"],
                       last_cpu_after=last["stat"]["last_cpu"])
        result[tid] = changes
    return result


class WindowProfile:
    def __init__(self, folder, *, group_factory=PerfGroup):
        self.folder, self.group_factory, self.groups = Path(folder), group_factory, []
        self.targets = []
        self.record = dict(schema=1, kind="owned-cpu-profile", normal_gate_eligible=False,
            decision_input=False, status="INCOMPLETE", processes=[], server_groups=[],
            scope="per-server-TID user+kernel PMCs; other tasks/softirq workers excluded; offsets retained",
            alignment="encompassing intervals, not exact central-window alignment",
            clock_ticks_per_second=os.sysconf("SC_CLK_TCK"))

    def fail(self, error):
        self.record.update(status="INVALID", error=f"{type(error).__name__}: {error}")

    def begin(self, server, generators):
        try:
            setting = Path("/proc/sys/kernel/sched_schedstats").read_text().strip()
            require(setting in ("0", "1"), "unknown kernel schedstats setting")
            self.record["sched_schedstats"] = setting
            self.record["runqueue_wait_status"] = "AVAILABLE" if setting == "1" else "UNAVAILABLE: kernel schedstats disabled"
            self.targets = [server, *generators]
            for index, process in enumerate(self.targets):
                row = dict(kind="server" if index == 0 else "generator", index=index - 1, before={})
                self.record["processes"].append(row)
                process_snapshot(process, row["before"])
            for tid in self.record["processes"][0]["before"]["threads"]:
                try:
                    group = self.group_factory(int(tid))
                except BaseException as error:
                    self.record["server_groups"].append(getattr(error, "profile_record",
                        dict(tid=int(tid), status="INVALID", error=str(error))))
                    raise
                self.groups.append(group)
                self.record["server_groups"].append(group.record)
            for group in self.groups:
                group.begin()
        except BaseException as error:
            self.fail(error)
            self.close()
            raise

    def finish(self, central_start, central_end, commands):
        errors = []
        self.record.update(central_start_monotonic=central_start, central_end_monotonic=central_end,
                           central_commands=commands)
        try:
            # Disable EVERY group before reading counts or /proc, minimizing the
            # overhang while preserving each group's own enable/disable brackets.
            for group in self.groups:
                try:
                    group.disable()
                except BaseException as error:
                    group.record.update(status="INVALID", error=str(error))
                    errors.append(str(error))
            for group in self.groups:
                try:
                    failed = group.record.get("status") == "INVALID"
                    group.finish()
                    if failed:
                        group.record["status"] = "INVALID"
                    first, last = group.record["enable_after_monotonic"], group.record["disable_before_monotonic"]
                    group.record.update(start_offset_seconds=first - central_start, end_offset_seconds=last - central_end)
                    require(first <= central_start < central_end <= last, "PMC interval did not encompass central window")
                except BaseException as error:
                    group.record.update(status="INVALID", error=str(error))
                    errors.append(str(error))
            setting = Path("/proc/sys/kernel/sched_schedstats").read_text().strip()
            require(setting == self.record["sched_schedstats"], "kernel schedstats setting changed during profile")
            for row, process in zip(self.record["processes"], self.targets):
                row["after"] = {}
                process_snapshot(process, row["after"])
                row["deltas"] = task_deltas(row["before"], row["after"], wait_available=setting == "1")
                row.update(before_offset_seconds=row["before"]["after_monotonic"] - central_start,
                           after_offset_seconds=row["after"]["before_monotonic"] - central_end)
            require(not errors, "; ".join(errors))
            totals = {name: sum(group.record["delta"]["counts"][name] for group in self.groups) for name, _ in EVENTS}
            require(commands > 0 and all(value > 0 for value in totals.values()), "missing central work or aggregate PMC progress")
            runtime = sum(row["runtime_ns"] for row in self.record["processes"][0]["deltas"].values())
            require(runtime > 0, "server schedstat runtime did not advance")
            self.record["derived"] = dict(totals=totals, server_runtime_ns=runtime,
                ipc=totals["instructions"] / totals["cycles"],
                cycles_per_reference_cycle=totals["cycles"] / totals["ref_cycles"],
                approx_cycles_per_central_command=totals["cycles"] / commands,
                approx_instructions_per_central_command=totals["instructions"] / commands,
                approx_cycles_per_schedstat_ns_ghz=totals["cycles"] / runtime)
            self.record["status"] = "COMPLETE"
        except BaseException as error:
            self.fail(error)
            raise
        finally:
            self.close()
        require(self.record["status"] == "COMPLETE", "profile descriptor cleanup failed")

    def close(self):
        for group in self.groups:
            group.close()
            if group.record.get("cleanup_errors"):
                self.record.update(status="INVALID", error="profile descriptor cleanup failed")
        # A durable sibling artifact survives failures before Runner can publish its
        # normal measurement result. Raw /proc observations contain only owned tasks.
        self.folder.mkdir(parents=True, exist_ok=True)
        (self.folder / "cpu-profile.json").write_text(json.dumps(self.record, indent=2) + "\n")


def probe_own_pid():
    """A <1-second permission/ABI control, never a database measurement."""
    group = PerfGroup(threading.get_native_id())
    try:
        group.begin()
        end = time.monotonic() + .025
        while time.monotonic() < end:
            pass
        group.disable()
        group.finish()
        require(all(group.record["delta"]["counts"].values()), "own-PID PMC probe recorded an empty event")
        print(json.dumps(dict(kind="own-PID-perf-capability-control", normal_gate_eligible=False,
                             pid=os.getpid(), group=group.record), indent=2))
    finally:
        group.close()


def self_test():
    import copy
    import tempfile
    from types import SimpleNamespace
    import unittest
    from unittest import mock

    def snapshot(pid=101, advance=0):
        return dict(pid=pid, stat=dict(start_ticks=123), executable=dict(path="owned", inode=1),
            before_monotonic=9 if not advance else 31,
            after_monotonic=9.1 if not advance else 31.1,
            threads={str(pid): dict(stat=dict(start_ticks=123, user_ticks=advance, system_ticks=0, last_cpu=2),
                uids=[1000] * 4, cpus_allowed=[2, 130],
                schedstat=dict(runtime_ns=advance * 100, runqueue_wait_ns=advance, timeslices=advance),
                scheduler=dict(nr_switches=advance))})

    class Group:
        def __init__(self, tid):
            self.record = dict(tid=tid, status="INCOMPLETE")
            self.closed = False
        def begin(self):
            self.record.update(enable_before_monotonic=9.4, enable_after_monotonic=9.5)
        def disable(self):
            self.record.update(disable_before_monotonic=30.5, disable_after_monotonic=30.6)
        def finish(self):
            self.record.update(status="COMPLETE", before={"raw_hex": "before"}, after={"raw_hex": "after"},
                delta=dict(counts=dict(cycles=1000, instructions=2000, ref_cycles=500)))
        def close(self):
            self.closed = True

    class Controls(unittest.TestCase):
        def samples(self):
            ids = {10: "cycles", 11: "instructions", 12: "ref_cycles"}
            before = decode_group(struct.pack("=9Q", 3, 0, 0, 0, 10, 0, 11, 0, 12), ids)
            after = decode_group(struct.pack("=9Q", 3, 100, 100, 200, 10, 400, 11, 100, 12), ids)
            return ids, before, after

        def test_group_ids_raw_counts_and_exact_timing(self):
            _, before, after = self.samples()
            self.assertTrue(after["raw_hex"])
            self.assertEqual(perf_delta(before, after), dict(time_enabled_ns=100, time_running_ns=100,
                counts=dict(cycles=200, instructions=400, ref_cycles=100)))
            self.assertEqual(perf_delta(before, before)["time_running_ns"], 0)

        def test_group_rejects_multiplexing_resets_and_identity_change(self):
            _, before, after = self.samples()
            for field, value in (("time_running_ns", 99), ("time_running_ns", 101), ("time_enabled_ns", -1)):
                with self.subTest(field=field, value=value), self.assertRaises(ProfileError):
                    perf_delta(before, {**after, field: value})
            for field, value in (("id", 999), ("name", "other"), ("value", -1)):
                changed = copy.deepcopy(after)
                changed["events"][0][field] = value
                with self.subTest(field=field), self.assertRaises(ProfileError):
                    perf_delta(before, changed)

        def test_group_rejects_short_duplicate_unknown_and_zero_running_counts(self):
            ids, before, after = self.samples()
            for raw in (b"", struct.pack("=9Q", 2, 1, 1, 1, 10, 1, 11, 1, 12),
                        struct.pack("=9Q", 3, 1, 1, 1, 10, 1, 10, 1, 12),
                        struct.pack("=9Q", 3, 1, 1, 1, 10, 1, 11, 1, 99)):
                with self.assertRaises(ProfileError):
                    decode_group(raw, ids)
            with self.assertRaisesRegex(ProfileError, "without running time"):
                perf_delta(before, {**after, "time_enabled_ns": 0, "time_running_ns": 0})

        def test_task_deltas_keep_affinity_and_unavailable_wait_explicit(self):
            before, after = snapshot(), snapshot(advance=2)
            row = task_deltas(before, after, wait_available=False)["101"]
            self.assertEqual(row["runtime_ns"], 200)
            self.assertEqual(row["cpus_allowed"], [2, 130])
            self.assertIsNone(row["runqueue_wait_ns"])
            self.assertEqual(task_deltas(before, after, wait_available=True)["101"]["runqueue_wait_ns"], 2)
            self.assertEqual(after["threads"]["101"]["schedstat"]["runqueue_wait_ns"], 2)

        def test_task_rejects_exit_reuse_exe_affinity_and_counter_changes(self):
            before, after = snapshot(), snapshot(advance=2)
            mutations = [lambda row: row.update(pid=999), lambda row: row["stat"].update(start_ticks=124),
                         lambda row: row["executable"].update(inode=2), lambda row: row["threads"].clear(),
                         lambda row: row["threads"]["101"].update(cpus_allowed=[2]),
                         lambda row: row["threads"]["101"]["stat"].update(start_ticks=999),
                         lambda row: row["threads"]["101"]["schedstat"].update(runtime_ns=-1)]
            for mutate in mutations:
                changed = copy.deepcopy(after)
                mutate(changed)
                with self.assertRaises(ProfileError):
                    task_deltas(before, changed, wait_available=False)

        def test_proc_parser_retains_actual_cpu_and_masks(self):
            fields = ["R"] + ["0"] * 36
            for index, value in ((1, os.getpid()), (11, 12), (12, 3), (19, 456), (36, 130)):
                fields[index] = str(value)
            parsed = parse_stat("101 (name with ) space) " + " ".join(fields))
            self.assertEqual((parsed["pid"], parsed["comm"], parsed["last_cpu"]), (101, "name with ) space", 130))
            self.assertEqual(parse_cpus("2-3,130-131"), [2, 3, 130, 131])
            for value in ("", "2,2", "3-2", "2-3-4"):
                with self.assertRaises((ProfileError, ValueError)):
                    parse_cpus(value)

        def test_snapshot_rejects_foreign_and_exited_targets_before_opening_counters(self):
            process = SimpleNamespace(pid=101, poll=lambda: 0)
            with self.assertRaisesRegex(ProfileError, "exited"):
                process_snapshot(process, {})

            process.poll = lambda: None
            raw = "101 (foreign) " + " ".join(["R", "1"] + ["0"] * 35)
            with mock.patch.object(Path, "read_text", return_value=raw), \
                 self.assertRaisesRegex(ProfileError, "own process or direct child"):
                process_snapshot(process, {})

        def test_actual_own_pid_proc_snapshot_keeps_spaced_scheduler_keys(self):
            record = {}
            process_snapshot(SimpleNamespace(pid=os.getpid(), poll=lambda: None), record)
            self.assertEqual(record["stat"]["pid"], os.getpid())
            self.assertTrue(record["threads"])
            for row in record["threads"].values():
                self.assertIn("se.nr_migrations", row["scheduler"])
                self.assertIn("se.nr_migrations ", row["raw_sched"])
                self.assertEqual(row["cpus_allowed"], sorted(os.sched_getaffinity(int(row["stat"]["pid"]))))
                self.assertGreater(row["schedstat"]["runtime_ns"], 0)

        def drive(self, group_type=Group, mutation=None):
            directory = tempfile.TemporaryDirectory()
            self.addCleanup(directory.cleanup)
            profile = WindowProfile(directory.name, group_factory=group_type)
            calls = {}
            def observe(process, output):
                advance = calls.get(process.pid, 0)
                calls[process.pid] = advance + 1
                output.update(snapshot(process.pid, advance))
                if advance and mutation:
                    mutation(output)
            patch = mock.patch(__name__ + ".process_snapshot", side_effect=observe)
            setting = mock.patch.object(Path, "read_text", return_value="0")
            with patch, setting:
                profile.begin(SimpleNamespace(pid=101), [SimpleNamespace(pid=102)])
                profile.finish(10, 30, 100)
            return profile

        def test_complete_profile_keeps_every_owned_thread_and_offsets(self):
            profile = self.drive()
            self.assertEqual(profile.record["status"], "COMPLETE")
            self.assertFalse(profile.record["normal_gate_eligible"])
            self.assertFalse(profile.record["decision_input"])
            self.assertEqual([row["kind"] for row in profile.record["processes"]], ["server", "generator"])
            self.assertEqual(profile.record["derived"]["ipc"], 2)
            self.assertEqual(profile.record["derived"]["approx_cycles_per_central_command"], 10)
            self.assertEqual(profile.record["server_groups"][0]["start_offset_seconds"], -.5)
            self.assertEqual(profile.record["server_groups"][0]["end_offset_seconds"], .5)
            self.assertTrue(all(group.closed for group in profile.groups))
            self.assertEqual(json.loads((profile.folder / "cpu-profile.json").read_text()), profile.record)

        def test_profile_retains_partial_evidence_when_open_fails(self):
            with tempfile.TemporaryDirectory() as tmp:
                profile = WindowProfile(tmp, group_factory=mock.Mock(side_effect=PermissionError("perf denied")))
                with mock.patch(__name__ + ".process_snapshot", side_effect=lambda proc, row: row.update(snapshot(proc.pid))), \
                     mock.patch.object(Path, "read_text", return_value="0"), self.assertRaises(PermissionError):
                    profile.begin(SimpleNamespace(pid=101), [])
                record = json.loads((Path(tmp) / "cpu-profile.json").read_text())
                self.assertEqual(record["status"], "INVALID")
                self.assertEqual(record["server_groups"][0]["tid"], 101)
                self.assertIn("before", record["processes"][0])

        def test_profile_failures_never_become_complete_and_close_all_groups(self):
            class FailedRead(Group):
                def finish(self):
                    raise ProfileError("unavailable group")
            class WrongWindow(Group):
                def disable(self):
                    super().disable()
                    self.record["disable_before_monotonic"] = 29
            class FailedClose(Group):
                def close(self):
                    super().close()
                    self.record["cleanup_errors"] = ["injected close failure"]
            for kind in (FailedRead, WrongWindow, FailedClose):
                with self.subTest(kind=kind.__name__), self.assertRaises(ProfileError):
                    self.drive(kind)
            with self.assertRaisesRegex(ProfileError, "affinity changed"):
                self.drive(mutation=lambda row: row["threads"][str(row["pid"])].update(cpus_allowed=[3]))

        def test_constructor_has_no_perf_or_proc_work_until_begin(self):
            factory = mock.Mock(side_effect=AssertionError("unexpected counter open"))
            with tempfile.TemporaryDirectory() as tmp, mock.patch.object(Path, "read_text", side_effect=AssertionError("proc read")):
                profile = WindowProfile(tmp, group_factory=factory)
                self.assertFalse(factory.called)
                self.assertEqual(list(Path(tmp).iterdir()), [])
                self.assertEqual(profile.groups, [])

        def test_instrument_scope_includes_dormant_profile_module(self):
            from abba_instrument import instrument_fingerprint
            fingerprint = instrument_fingerprint(Path(__file__).resolve().parent.parent)
            self.assertIn("tests/abba_profile.py", [row["path"] for row in fingerprint["entries"]])

    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Controls))
    if not result.wasSuccessful():
        raise SystemExit(1)


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe-own-pid", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    elif args.probe_own_pid:
        probe_own_pid()
    else:
        parser.error("choose --probe-own-pid or --self-test")
