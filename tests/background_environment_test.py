#!/usr/bin/env python3
"""Serverless quiet-evidence controls retained in the ABBA gate battery.

Process authentication tests retired with the inventory mechanism. These controls
check the facts a receipt now needs: exactly the granted CPUs, intended ports,
a complete preflight within the fixed CPU budget, and samples spanning the run.
Live selected-CPU and port counterexamples belong to gate_quiet.py's self-test.
"""
import copy
import unittest

from abba_evidence import validate_quiet
from _abba_test_fixtures import quiet_record


class Controls(unittest.TestCase):
    def setUp(self):
        self.environment = dict(port=8700, server_cpus=[0, 1], load_cpus=[2, 3], server_physical=[0, 1])
        self.quiet = quiet_record(cpus=[0, 1, 2, 3], server_physical_cores=2, started_at=100, finished_at=200, samples=101)

    def validate(self):
        return validate_quiet(self.quiet, self.environment, now=201, started=100, elapsed=101)

    def test_selected_core_preflight_needs_no_process_inventory(self):
        self.assertEqual(self.validate(), (100, 200))
        self.assertNotIn("background_environment", self.quiet)
        self.assertNotIn("background_environment", self.environment)

    def test_cpu_budget_overrun_never_becomes_green(self):
        self.quiet["generic_cpu_screening"]["peak_rolling"]["cpu_seconds"] = .061
        with self.assertRaisesRegex(ValueError, "exceeded its CPU budget"):
            self.validate()

    def test_stored_budget_cannot_relax_the_fixed_screening_policy(self):
        for field, value in (("capacity_fraction", .01), ("cpu_budget_seconds", 1),
                             ("server_physical_cores", 128)):
            with self.subTest(field=field):
                baseline = copy.deepcopy(self.quiet)
                self.quiet["generic_cpu_screening"][field] = value
                with self.assertRaises(ValueError):
                    self.validate()
                self.quiet = baseline

    def test_short_preflight_is_not_complete(self):
        self.quiet["generic_cpu_screening"]["preflight_seconds"] = 19.9
        with self.assertRaisesRegex(ValueError, "did not cover"):
            self.validate()

    def test_unselected_cpu_rows_cannot_enter_the_accounting(self):
        self.quiet["cpus"].append(255)
        with self.assertRaisesRegex(ValueError, "exactly the measurement CPUs"):
            self.validate()

    def test_selected_cpu_rows_cannot_disappear(self):
        for field in ("cpus", "requested_cpus"):
            with self.subTest(field=field):
                baseline = copy.deepcopy(self.quiet)
                self.quiet[field].pop()
                with self.assertRaisesRegex(ValueError, "exactly the measurement CPUs"):
                    self.validate()
                self.quiet = baseline

    def test_missing_samples_or_artifact_fail(self):
        for field, value in (("samples", 1), ("cpu_samples", 1), ("sample_artifact", "")):
            with self.subTest(field=field):
                baseline = copy.deepcopy(self.quiet)
                self.quiet[field] = value
                with self.assertRaises(ValueError):
                    self.validate()
                self.quiet = baseline

    def test_port_checks_cannot_be_omitted(self):
        for field, value in (("ports", []), ("ports", [0]), ("listener_checks", 0)):
            with self.subTest(field=field, value=value):
                baseline = copy.deepcopy(self.quiet)
                self.quiet[field] = value
                with self.assertRaisesRegex(ValueError, "intended ports"):
                    self.validate()
                self.quiet = baseline

    def test_latched_failure_and_unfinished_observer_fail(self):
        for field, value in (("interference", {"error": "selected CPU busy"}), ("complete", False)):
            with self.subTest(field=field):
                baseline = copy.deepcopy(self.quiet)
                self.quiet[field] = value
                with self.assertRaisesRegex(ValueError, "incomplete, or contended"):
                    self.validate()
                self.quiet = baseline

    def test_runtime_cpu_observations_cannot_claim_foreign_attribution(self):
        self.quiet["generic_cpu_screening"]["scope"] = "continuous"
        with self.assertRaisesRegex(ValueError, "preflight scope"):
            self.validate()

    def test_old_inventory_policy_cannot_certify_current_observations(self):
        self.quiet["policy"] = "operational-environment-v1"
        with self.assertRaisesRegex(ValueError, "unsupported selected-core"):
            self.validate()


if __name__ == "__main__":
    unittest.main()
