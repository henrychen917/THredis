#!/usr/bin/env python3
"""Plan the default iteration gate, full push/release gate, or correctness/perf diagnostics."""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import sys

from gate_measurements import ratio as measured_ratio


SERVER_PER_SLOT = 8
LOAD_PER_SLOT = 2
PORTS_PER_SLOT = 3
MIN_PHYSICAL = 16
MAX_PHYSICAL = 128
ROOT = Path(__file__).resolve().parent.parent


def parse_cpu_range(spec, label="CPU range", allow_empty=False):
    if isinstance(spec, (list, tuple, set, frozenset)):
        values = sorted(spec)
        if any(not isinstance(cpu, int) or cpu < 0 for cpu in values):
            raise ValueError(f"{label}: CPU IDs must be nonnegative integers")
        if len(values) != len(set(values)):
            raise ValueError(f"{label}: duplicate CPU IDs")
        if not values and not allow_empty:
            raise ValueError(f"{label} must not be empty")
        return values
    if not spec and allow_empty:
        return []
    if not isinstance(spec, str) or not spec:
        raise ValueError(f"{label} must not be empty")
    result = set()
    for part in spec.split(","):
        if not re.fullmatch(r"[0-9]+(?:-[0-9]+)?", part):
            raise ValueError(f"{label}: invalid CPU list {spec!r}; use 0-7,64-71")
        bounds = [int(value) for value in part.split("-")]
        first, last = bounds[0], bounds[-1]
        if last < first or last > 1048575:
            raise ValueError(f"{label}: invalid CPU range {part!r}")
        chunk = set(range(first, last + 1))
        if result & chunk:
            raise ValueError(f"{label}: CPU {min(result & chunk)} appears more than once")
        result.update(chunk)
    return sorted(result)


def cpu_string(values):
    values = sorted(set(values))
    ranges = []
    index = 0
    while index < len(values):
        first = last = values[index]
        index += 1
        while index < len(values) and values[index] == last + 1:
            last = values[index]
            index += 1
        ranges.append(str(first) if first == last else f"{first}-{last}")
    return ",".join(ranges)


def read_topology(cpus=None):
    root = Path("/sys/devices/system/cpu")
    if cpus is None:
        cpus = parse_cpu_range((root / "online").read_text().strip(), "online CPUs")
    topology = {}
    for cpu in cpus:
        path = root / f"cpu{cpu}/topology/thread_siblings_list"
        try:
            siblings = frozenset(parse_cpu_range(path.read_text().strip(), str(path)))
        except OSError as exc:
            raise ValueError(f"CPU {cpu}: cannot read sibling topology at {path}: {exc}") from exc
        if cpu not in siblings:
            raise ValueError(f"CPU {cpu}: malformed sibling topology {sorted(siblings)}")
        topology[cpu] = siblings
    for cpu, siblings in topology.items():
        for other in siblings.intersection(topology):
            if topology[other] != siblings:
                raise ValueError(f"CPUs {cpu} and {other}: inconsistent sibling topology")
    return topology


def permitted_cpus(cpus):
    # sched_getaffinity alone sees an invoking shell's taskset restriction, not the cgroup's
    # available CPUs. Try the requested mask in this short-lived planner and restore it before
    # returning; the kernel intersects it with the actual online/cgroup permission mask.
    previous = os.sched_getaffinity(0)
    try:
        os.sched_setaffinity(0, set(cpus))
        return set(os.sched_getaffinity(0))
    except OSError as exc:
        raise ValueError(f"requested CPUs are unavailable: {exc}") from exc
    finally:
        os.sched_setaffinity(0, previous)


def validate_axes(server_cores, server_smt, load_cores, load_smt, *, topology=None,
                  check_available=True):
    axes = {name: parse_cpu_range(value, "--" + name.replace("_", "-"), "smt" in name)
            for name, value in (("server_cores", server_cores), ("server_smt", server_smt),
                                ("load_cores", load_cores), ("load_smt", load_smt))}
    requested = set(cpu for values in axes.values() for cpu in values)
    if topology is None:
        topology = read_topology(sorted(requested))
    missing = requested - topology.keys()
    if missing:
        raise ValueError(f"requested CPUs have no topology: {cpu_string(missing)}")
    seen = {}
    for name, values in axes.items():
        for cpu in values:
            if cpu in seen:
                raise ValueError(f"CPU {cpu} overlaps --{seen[cpu].replace('_', '-')} and "
                                 f"--{name.replace('_', '-')}")
            seen[cpu] = name
    for role in ("server", "load"):
        owners = {}
        for cpu in axes[role + "_cores"]:
            group = topology[cpu]
            if group in owners:
                raise ValueError(f"--{role}-cores contains SMT siblings {owners[group]} and {cpu}; "
                                 f"put the sibling in --{role}-smt")
            owners[group] = cpu
        for cpu in axes[role + "_smt"]:
            if topology[cpu] not in owners:
                raise ValueError(f"--{role}-smt CPU {cpu} has no physical core in --{role}-cores")
    server_groups = {topology[cpu] for cpu in axes["server_cores"]}
    for cpu in axes["load_cores"] + axes["load_smt"]:
        if topology[cpu] in server_groups:
            raise ValueError(f"load CPU {cpu} shares a physical core with server CPUs "
                             f"{cpu_string(topology[cpu])}; server SMT siblings must stay reserved")
    if check_available:
        available = permitted_cpus(requested)
        if available != requested:
            raise ValueError(f"requested CPUs are unavailable: {cpu_string(requested - available)}")
    return axes


def parse_ports(spec):
    if not isinstance(spec, str) or not re.fullmatch(r"[0-9]+-[0-9]+", spec):
        raise ValueError("--ports must be a first-last range, e.g. 7899-7998")
    first, last = map(int, spec.split("-"))
    if not 1 <= first <= last <= 65535:
        raise ValueError("--ports must satisfy 1 <= first <= last <= 65535")
    return first, last


def executable(value, label):
    if not value:
        return ""
    path = Path(value).expanduser().resolve()
    if not path.is_file() or not os.access(path, os.X_OK):
        raise ValueError(f"{label} is not an executable file: {path}")
    return str(path)


def default_physical(available, topology):
    groups = {}
    for cpu in sorted(available):
        groups.setdefault(topology[cpu], cpu)
    return sorted(groups.values())[:MAX_PHYSICAL]


def make_plan(args, *, topology=None, available=None, check_available=True):
    # The iteration budget changes only the measured cells. Normalize every complete gate to
    # the existing full correctness path, so a new purpose cannot bypass full-only batteries.
    purpose = args.tier
    correctness_ratio = measured_ratio("correctness", SERVER_PER_SLOT)
    if os.getenv("GATE_RATIO", correctness_ratio) != correctness_ratio:
        raise ValueError("GATE_RATIO differs from the reviewed correctness geometry; update gate_measurements.json")
    tier = purpose if purpose in ("quick", "perf") else "full"
    subset = args.subset or ("smoke" if purpose == "iteration" else "full")
    if purpose == "quick" and args.subset is not None:
        raise ValueError("quick is correctness-only and does not accept --subset")
    if purpose in ("push", "release", "full") and subset != "full":
        raise ValueError(f"{purpose} requires --subset full; smoke is an iteration/perf diagnostic")
    if topology is None:
        topology = read_topology()
    if available is None:
        available = permitted_cpus(topology)
    defaults = default_physical(available, topology)
    server = None if args.server_cores is None else parse_cpu_range(args.server_cores, "--server-cores")
    load = None if args.load_cores is None else parse_cpu_range(args.load_cores, "--load-cores")
    if server is None and load is None:
        if len(defaults) < MIN_PHYSICAL:
            raise ValueError(f"physical core budget is {len(defaults)}; the gate requires "
                             f"{MIN_PHYSICAL}-{MAX_PHYSICAL} distinct physical cores")
        count = len(defaults) // (SERVER_PER_SLOT + LOAD_PER_SLOT) * SERVER_PER_SLOT
        server, load = defaults[:count], defaults[count:]
    elif server is None or load is None:
        supplied = load if server is None else server
        unknown = set(supplied) - topology.keys()
        if unknown:
            raise ValueError(f"requested CPUs have no topology: {cpu_string(unknown)}")
        occupied = {topology[cpu] for cpu in supplied}
        remaining = [cpu for cpu in defaults if topology[cpu] not in occupied]
        remaining = remaining[:max(0, MAX_PHYSICAL - len(supplied))]
        if server is None:
            server = remaining
        else:
            load = remaining
    axes = validate_axes(server, args.server_smt, load, args.load_smt or "",
                         topology=topology, check_available=check_available)
    total = len(server) + len(load)
    if not MIN_PHYSICAL <= total <= MAX_PHYSICAL:
        raise ValueError(f"physical core budget is {total}; the gate requires {MIN_PHYSICAL}-"
                         f"{MAX_PHYSICAL} distinct physical cores")
    count = min(len(server) // SERVER_PER_SLOT, len(load) // LOAD_PER_SLOT)
    if count < 1:
        raise ValueError(f"budget cannot fit one correctness slot: need {SERVER_PER_SLOT} server "
                         f"physical cores and {LOAD_PER_SLOT} load physical cores; got "
                         f"{len(server)} server and {len(load)} load")
    first, last = parse_ports(args.ports)
    required = 1 if args.tier == "perf" else PORTS_PER_SLOT * count
    if last - first + 1 < required:
        raise ValueError(f"--ports {args.ports} has {last-first+1} ports; {count} correctness slots "
                         f"need {required} ({PORTS_PER_SLOT} per slot); widen the port range")
    slots = []
    for index in range(count):
        servers = server[index * SERVER_PER_SLOT:(index + 1) * SERVER_PER_SLOT]
        loads = load[index * len(load) // count:(index + 1) * len(load) // count]
        groups = {topology[cpu] for cpu in loads}
        smt = [cpu for cpu in axes["load_smt"] if topology[cpu] in groups]
        slots.append({"server_cores": cpu_string(servers), "load_cores": cpu_string(loads),
                      "load_smt": cpu_string(smt), "load_cpus": cpu_string(loads + smt),
                      "port": first + PORTS_PER_SLOT * index})
    # Correctness partitions and the headline measurement have different geometry. Once every
    # correctness child has exited, the measurement keeps at most 32 physical server cores and
    # gives every other selected physical core to its load generators. Correctness needs protocol
    # traffic, not saturation: omitted load SMT stays unused there. Regression needs headroom:
    # omission enables available load siblings, while an explicitly empty --load-smt reserves
    # them. Server SMT is never automatic, and no server physical sibling can enter the load set.
    perf_server = server[:32]
    perf_load = sorted(load + server[32:])
    server_groups = {topology[cpu] for cpu in perf_server}
    perf_server_smt = [cpu for cpu in axes["server_smt"] if topology[cpu] in server_groups]
    perf_load_smt = sorted(axes["load_smt"] + [cpu for cpu in axes["server_smt"]
                                               if topology[cpu] not in server_groups])
    if args.load_smt is None:
        load_groups = {topology[cpu] for cpu in perf_load}
        perf_load_smt = sorted({cpu for group in load_groups for cpu in group
                                if cpu in available and cpu in topology} - set(perf_load))
    # Validate the derived phase as well as the user axes: topology/availability filtering must
    # never turn the headroom default into physical-core contention with a server thread.
    validate_axes(perf_server, perf_server_smt, perf_load, perf_load_smt,
                  topology=topology, check_available=check_available)
    perf = {"server_cores": cpu_string(perf_server), "server_smt": cpu_string(perf_server_smt),
            "load_cores": cpu_string(perf_load), "load_smt": cpu_string(perf_load_smt),
            "server_cpus": cpu_string(perf_server + perf_server_smt),
            "load_cpus": cpu_string(perf_load + perf_load_smt),
            "threads": len(perf_server) + len(perf_server_smt), "port": first}
    candidate = executable(args.candidate_binary, "--candidate-binary")
    reference = executable(args.reference_binary, "--reference-binary")
    build_cpus = sorted(cpu for values in axes.values() for cpu in values)
    header = (f"{total} physical cores; {count} correctness slots = min({len(server)}/8 server, "
              f"{len(load)}/2 load), 8 server threads at ratio {correctness_ratio} and 3 ports per slot; "
              f"correctness load is modest protocol traffic (at least 2 physical cores per slot), "
              f"server SMT reserved; isolated ABBA {subset}: {len(perf_server)} server + "
              f"{len(perf_load)} load physical cores, {len(perf_load_smt)} load SMT threads "
              f"({'automatic available siblings' if args.load_smt is None else 'explicit selection'}), "
              f"{len(perf_server_smt)} explicit server SMT threads")
    if len(server) > len(perf_server):
        header += f" ({len(server)-len(perf_server)} surplus server cores move to ABBA load)"
    return {"tier": tier, "purpose": purpose, "subset": subset,
            "physical_cores": total, "slot_count": count, "correctness_ratio": correctness_ratio,
            "server_per_slot": SERVER_PER_SLOT, "load_min_per_slot": LOAD_PER_SLOT,
            "ports_per_slot": PORTS_PER_SLOT, "ports_first": first, "ports_last": last,
            "axes": {key: cpu_string(value) for key, value in axes.items()}, "slots": slots,
            "perf": perf, "candidate_binary": candidate or str(ROOT / "build/tomokv"),
            "reference_binary": reference, "build_candidate": int(not candidate),
            "build_cores": cpu_string(build_cpus), "build_jobs": len(build_cpus),
            "header": header, "abba_extra": getattr(args, "abba_extra", [])}


def shell_plan(plan):
    scalars = {"TIER": plan["tier"], "GATE_PURPOSE": plan["purpose"],
               "GATE_PHYSICAL_CORES": plan["physical_cores"], "GATE_RATIO": plan["correctness_ratio"],
               "GATE_SLOTS": plan["slot_count"], "GATE_PORT_FIRST": plan["ports_first"],
               "GATE_PORT_LAST": plan["ports_last"], "CANDIDATE_BINARY": plan["candidate_binary"],
               "REFERENCE_BINARY": plan["reference_binary"], "PERF_THREADS": plan["perf"]["threads"],
               "BUILD_CANDIDATE": plan["build_candidate"], "BUILD_CORES": plan["build_cores"],
               "BUILD_JOBS": plan["build_jobs"], "PLAN_HEADER": plan["header"]}
    scalars.update({"GATE_" + key.upper(): value for key, value in plan["axes"].items()})
    scalars.update({"PERF_" + key.upper(): value for key, value in plan["perf"].items()})
    lines = [f"{key}={shlex.quote(str(value))}" for key, value in scalars.items()]
    for name, field in (("SLOT_CORES", "server_cores"), ("SLOT_LOAD_CORES", "load_cpus"),
                        ("SLOT_LOAD_PHYSICAL", "load_cores"), ("SLOT_LOAD_SMT", "load_smt"),
                        ("SLOT_PORTS", "port")):
        lines.append(f"{name}=(" + " ".join(shlex.quote(str(slot[field])) for slot in plan["slots"]) + ")")
    abba = ["--subset", plan["subset"], "--candidate-binary", plan["candidate_binary"], "--ports",
            f"{plan['ports_first']}-{plan['ports_last']}", "--port", str(plan["perf"]["port"])]
    for key in ("server_cores", "server_smt", "load_cores", "load_smt"):
        abba += ["--" + key.replace("_", "-"), plan["perf"][key]]
    if plan["reference_binary"]:
        abba += ["--reference-binary", plan["reference_binary"]]
    abba += plan["abba_extra"]
    lines.append("ABBA_ARGS=(" + " ".join(shlex.quote(value) for value in abba) + ")")
    return "\n".join(lines) + "\n"


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("tier", nargs="?", choices=("iteration", "push", "release", "full", "quick", "perf"),
                        default="iteration", help="iteration (default): full correctness + smoke; "
                        "push/release/full: full correctness + all cells; quick: correctness only; "
                        "perf: measurement diagnostic")
    result.add_argument("--subset", choices=("smoke", "full"), help="iteration/perf override; "
                        "push/release/full require full; quick has no measurements")
    result.add_argument("--server-cores", default=os.getenv("GATE_SERVER_CORES", os.getenv("GATE_CORES")))
    result.add_argument("--server-smt", default=os.getenv("GATE_SERVER_SMT", ""))
    result.add_argument("--load-cores", default=os.getenv("GATE_LOAD_CORES"))
    result.add_argument("--load-smt", default=os.getenv("GATE_LOAD_SMT"),
                        help="explicit load sibling CPUs; omitted: automatic for ABBA only; "
                        "empty string: reserve omitted siblings")
    result.add_argument("--ports", default=os.getenv("GATE_PORTS", "7899-7998"))
    result.add_argument("--reference-binary", default="")
    result.add_argument("--candidate-binary", "--candidate", default="")
    result.add_argument("--json", action="store_true")
    result.add_argument("--self-test", action="store_true")
    return result


def self_test():
    import subprocess
    import unittest
    from unittest import mock

    # Deliberately noncontiguous, non-offset sibling IDs: assuming this box's +128 relationship
    # would let several rejection tests pass vacuously. No servers, binds, or child processes.
    topology = {cpu: frozenset((cpu, 1000 + 3 * cpu)) for cpu in range(128)}
    topology.update({1000 + 3 * cpu: group for cpu, group in list(topology.items())})

    class PlanningTests(unittest.TestCase):
        def plan(self, *flags):
            # Gate workers export their own slice of GATE_*; validation of the planner must use
            # its synthetic topology even when called from one of those workers.
            with mock.patch.dict(os.environ, {}, clear=True):
                args = parser().parse_args(list(flags))
            return make_plan(args, topology=topology, available=set(topology), check_available=False)

        def test_default_full_box(self):
            plan = self.plan()
            self.assertEqual((plan["purpose"], plan["tier"], plan["subset"]),
                             ("iteration", "full", "smoke"))
            self.assertEqual(plan["physical_cores"], 128)
            self.assertEqual(plan["slot_count"], 12)
            self.assertEqual(plan["perf"]["server_cores"], "0-31")
            self.assertEqual(plan["perf"]["load_cores"], "32-127")
            self.assertEqual(plan["perf"]["server_smt"], "")
            self.assertEqual(parse_cpu_range(plan["perf"]["load_smt"]),
                             [1000 + 3 * cpu for cpu in range(32, 128)])
            self.assertEqual(len(parse_cpu_range(plan["perf"]["load_cpus"])), 192)
            self.assertTrue(all(slot["load_smt"] == "" for slot in plan["slots"]))
            self.assertEqual(plan["axes"]["load_smt"], "")

        def argv(self, plan):
            # Execute the actual shell assignment/expansion used by gate.sh. These strings
            # include empty explicit axes and must survive quoting exactly, not just look right.
            script = shell_plan(plan) + 'printf "%s\\0" "$TIER" "$GATE_PURPOSE" "${ABBA_ARGS[@]}"'
            result = subprocess.run(["bash", "-uc", script], check=True, capture_output=True)
            return result.stdout.decode().rstrip("\0").split("\0")

        def test_complete_tier_aliases_emit_full_correctness_and_actual_subset_argv(self):
            import abbagate
            for purpose, subset in (("iteration", "smoke"), ("push", "full"),
                                    ("release", "full"), ("full", "full")):
                with self.subTest(purpose=purpose):
                    tier, label, *argv = self.argv(self.plan(purpose))
                    self.assertEqual((tier, label), ("full", purpose))
                    with mock.patch.object(sys, "argv", ["abbagate.py", *argv]):
                        parsed = abbagate.parse_args()
                    self.assertEqual(parsed.subset, subset)
                    self.assertEqual(parsed.server_cores, "0-31")
                    self.assertEqual(len(parse_cpu_range(parsed.load_smt)), 96)

        def test_diagnostic_tiers_and_strengthening_iteration(self):
            for purpose in ("quick", "perf"):
                plan = self.plan(purpose)
                self.assertEqual((plan["tier"], plan["purpose"]), (purpose, purpose))
            for purpose in ("iteration", "perf"):
                for subset in ("smoke", "full"):
                    self.assertEqual(self.plan(purpose, "--subset", subset)["subset"], subset)
            for purpose in ("push", "release", "full"):
                with self.subTest(purpose=purpose), self.assertRaisesRegex(ValueError, "requires --subset full"):
                    self.plan(purpose, "--subset", "smoke")
            with self.assertRaisesRegex(ValueError, "correctness-only"):
                self.plan("quick", "--subset", "full")

        def test_explicit_empty_load_smt_reserves_siblings(self):
            plan = self.plan("--load-smt", "")
            self.assertEqual(plan["perf"]["load_smt"], "")
            self.assertEqual(len(parse_cpu_range(plan["perf"]["load_cpus"])), 96)
            with mock.patch.dict(os.environ, {"GATE_LOAD_SMT": ""}, clear=True):
                args = parser().parse_args([])
            plan = make_plan(args, topology=topology, available=set(topology), check_available=False)
            self.assertEqual(plan["perf"]["load_smt"], "")

        def test_auto_smt_obeys_selected_budget_and_available_topology(self):
            with mock.patch.dict(os.environ, {}, clear=True):
                args = parser().parse_args(["--server-cores", "0-15", "--load-cores", "32-47"])
            available = set(range(128)) | {1000 + 3 * cpu for cpu in range(40, 64)}
            plan = make_plan(args, topology=topology, available=available, check_available=False)
            self.assertEqual(plan["perf"]["server_cores"], "0-15")
            self.assertEqual(plan["perf"]["load_cores"], "32-47")
            self.assertEqual(parse_cpu_range(plan["perf"]["load_smt"]),
                             [1000 + 3 * cpu for cpu in range(40, 48)])
            for cpu in parse_cpu_range(plan["perf"]["load_cpus"]):
                self.assertFalse(topology[cpu] & set(range(16)))

        def test_minimum_budget(self):
            plan = self.plan("--server-cores", "0-7", "--load-cores", "8-15", "--ports", "9000-9002")
            self.assertEqual(plan["slot_count"], 1)
            self.assertEqual(plan["perf"]["threads"], 8)
            self.assertEqual(plan["slots"][0]["load_cpus"], "8-15")

        def test_planned_port_overrides_stale_abba_environment(self):
            import abbagate
            plan = self.plan("--server-cores", "0-7", "--load-cores", "8-15", "--ports", "9000-9002")
            _, _, *argv = self.argv(plan)
            with mock.patch.dict(os.environ, {"GATE_ABBA_PORT": "65000"}, clear=True), \
                 mock.patch.object(sys, "argv", ["abbagate.py", *argv]):
                parsed = abbagate.parse_args()
            self.assertEqual(abbagate.select_port(parsed.ports, parsed.port), (9000, (9000, 9002)))

        def test_slots_own_disjoint_resources(self):
            plan = self.plan("--server-cores", "0-31,64-95", "--load-cores", "32-47,96-111")
            cpus, ports = set(), set()
            for slot in plan["slots"]:
                current = set(parse_cpu_range(slot["server_cores"]) + parse_cpu_range(slot["load_cpus"]))
                self.assertEqual(len(parse_cpu_range(slot["server_cores"])), 8)
                self.assertFalse(current & cpus)
                cpus |= current
                current_ports = set(range(slot["port"], slot["port"] + PORTS_PER_SLOT))
                self.assertFalse(current_ports & ports)
                ports |= current_ports

        def test_server_load_overlap(self):
            with self.assertRaisesRegex(ValueError, "overlaps"):
                self.plan("--server-cores", "0-7", "--load-cores", "7-15")

        def test_load_on_server_sibling(self):
            with self.assertRaisesRegex(ValueError, "shares a physical core"):
                self.plan("--server-cores", "0-7", "--load-cores", "8-15,1000")

        def test_sibling_must_name_its_physical_axis(self):
            with self.assertRaisesRegex(ValueError, "no physical core"):
                self.plan("--server-cores", "0-7", "--server-smt", "1024", "--load-cores", "8-15")

        def test_physical_axis_cannot_hide_smt(self):
            with self.assertRaisesRegex(ValueError, "contains SMT siblings"):
                self.plan("--server-cores", "0-7,1000", "--load-cores", "8-15")

        def test_explicit_smt_follows_selected_physical_owner(self):
            plan = self.plan("--server-cores", "0-39", "--server-smt", "1000,1096",
                             "--load-cores", "40-63", "--load-smt", "1120")
            self.assertEqual(plan["perf"]["server_smt"], "1000")
            self.assertEqual(plan["perf"]["load_smt"], "1096,1120")
            self.assertEqual(plan["perf"]["threads"], 33)
            self.assertTrue(all("1000" not in slot["server_cores"] for slot in plan["slots"]))

        def test_insufficient_ports_does_not_reduce_concurrency(self):
            with self.assertRaisesRegex(ValueError, "12 correctness slots need 36"):
                self.plan("--ports", "9000-9034")

        def test_no_correctness_slot(self):
            with self.assertRaisesRegex(ValueError, "cannot fit one correctness slot"):
                self.plan("--server-cores", "0-6", "--load-cores", "7-15")

        def test_invalid_budget(self):
            with self.assertRaisesRegex(ValueError, "physical core budget is 15"):
                self.plan("--server-cores", "0-7", "--load-cores", "8-14")

        def test_default_small_machine_fails_with_budget(self):
            with mock.patch.dict(os.environ, {}, clear=True):
                args = parser().parse_args([])
            with self.assertRaisesRegex(ValueError, "physical core budget is 8"):
                make_plan(args, topology=topology, available=set(range(8)), check_available=False)

        def test_unavailable_requested_cpu_fails(self):
            with mock.patch(__name__ + ".permitted_cpus", return_value=set(range(15))):
                with self.assertRaisesRegex(ValueError, "CPUs are unavailable: 15"):
                    validate_axes("0-7", "", "8-15", "", topology=topology)

        def test_unknown_cpu_fails_without_reading_unrelated_state(self):
            with self.assertRaisesRegex(ValueError, "no topology: 999999"):
                self.plan("--server-cores", "0-7", "--load-cores", "8-15,999999")

        def test_invalid_ranges(self):
            for spec in ("", "0-", "3-1", "0,,2", "1,1", "0-7,7-9", "-1", "1;true"):
                with self.subTest(spec=spec), self.assertRaises(ValueError):
                    parse_cpu_range(spec)
            for spec in ("9000", "0-10", "9999-9998", "1-65536"):
                with self.subTest(ports=spec), self.assertRaises(ValueError):
                    parse_ports(spec)

        def test_nonexistent_binary_fails_before_run(self):
            with self.assertRaisesRegex(ValueError, "not an executable file"):
                self.plan("--candidate-binary", "/nonexistent/gate-candidate")

        def test_plan_is_deterministic(self):
            self.assertEqual(shell_plan(self.plan()), shell_plan(self.plan()))

    suite = unittest.defaultTestLoader.loadTestsFromTestCase(PlanningTests)
    from gate_measurements import self_test as measurements_self_test
    return max(0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1,
               measurements_self_test())


def main():
    argument_parser = parser()
    arguments, extra = argument_parser.parse_known_args()
    if extra and arguments.tier != "perf":
        argument_parser.error("unrecognized arguments: " + " ".join(extra))
    arguments.abba_extra = extra
    if arguments.self_test:
        return self_test()
    try:
        plan = make_plan(arguments)
    except (ValueError, OSError) as exc:
        print(f"gate resources: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(plan, sort_keys=True, indent=2) if arguments.json else shell_plan(plan), end="\n" if arguments.json else "")
    return 0


if __name__ == "__main__":
    sys.exit(main())
