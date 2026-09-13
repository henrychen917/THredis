#!/usr/bin/env python3
"""Offline checks of scope injection and attribution; no server or load generator."""
import copy
from pathlib import Path
import tempfile
import unittest

import reorder_scope as scope


class ScopeTest(unittest.TestCase):
    def row(self, command="GET", path="eligible", ns_log2=7, count=10, **kw):
        result = dict(mode="1s", rl=1, atomic=1, key_lb=1, client_lb=1, reorder=1, overlap=1,
                      command=command, path=path, predicted=0, ns_log2=ns_log2, count=count,
                      ns_total=count * (1 << ns_log2), rank=0, later_short=0, local_fallback=-1)
        result.update(kw)
        return result

    def test_actual_source_hooks_and_missing_anchor_fail(self):
        original = (scope.ROOT / "src/core/ex_loop.h").read_text()
        patched = scope.instrument(original)
        self.assertEqual(patched.count("reorder_scope::Batch scope_batch"), 2)
        self.assertEqual(patched.count("reorder_scope::Execution scope_execute"), 1)
        self.assertEqual(patched.count("reorder_scope::Local scope_local"), 1)
        self.assertEqual(patched.count("reorder_scope::Outside scope_path"), 5)
        for damaged in (original.replace("    bool execute(const Task& t) {", ""),
                        original + "\n    bool execute(const Task& t) {"):
            with self.assertRaisesRegex(ValueError, "scope anchor changed"):
                scope.instrument(damaged)

    def test_scope_population_and_static_miss(self):
        rows = [self.row(), self.row("BITCOUNT", ns_log2=12, predicted=2, later_short=1),
                self.row("SET", ns_log2=12, predicted=1, count=2),
                self.row("MGET", "atomic_deferred", ns_log2=13, predicted=1, count=3),
                self.row("EVAL", "barrier", ns_log2=14, count=4)]
        original = copy.deepcopy(rows)
        result, = scope.summarize(rows)
        self.assertEqual(result["long_threshold_ns"], 2048)
        self.assertEqual(result["short_reference_attempts"], 10)
        by_command = {row["command"]: row for row in result["rows"]}
        self.assertEqual(by_command["SET"]["long_attempts"], 2)
        self.assertEqual(by_command["BITCOUNT"]["later_short"], 10)
        self.assertEqual(by_command["MGET"]["path"], "atomic_deferred")
        self.assertEqual(by_command["EVAL"]["path"], "barrier")
        self.assertEqual(original, rows)

    def test_local_does_not_supply_an_executor_short_reference(self):
        rows = [self.row(path="local-e0", local_fallback=0),
                self.row("BITCOUNT", ns_log2=12, predicted=2)]
        results = {row["venue"]: row for row in scope.summarize(rows)}
        self.assertIsNone(results["executor"]["long_threshold_ns"])
        self.assertEqual(results["local-e0"]["long_threshold_ns"], 2048)
        self.assertEqual(results["executor"]["short_reference_attempts"], 0)
        self.assertIsNone(results["executor"]["rows"][0]["long_attempts"])

    def test_modes_and_arms_do_not_mix(self):
        rows = [self.row(), self.row(mode="2s", ns_log2=8), self.row(reorder=0, ns_log2=9)]
        results = scope.summarize(rows)
        self.assertEqual(len(results), 3)
        self.assertEqual({r["long_threshold_ns"] for r in results}, {2048, 4096, 8192})

    def test_empty_capture_and_foreign_destination_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "no observations"):
                scope.read_rows(Path(directory), 1)
        for path in (scope.ROOT, scope.ROOT / "build", scope.ROOT.parent / "sibling"):
            with self.assertRaisesRegex(ValueError, "beneath this worktree"):
                scope.destination_path(path)

    def test_unclosed_thread_fails_even_without_timing_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "observations").mkdir()
            (path / "observations/1-2.opened").write_text("123\n")
            with self.assertRaisesRegex(ValueError, "did not close cleanly"):
                scope.read_rows(path, 123)


if __name__ == "__main__":
    unittest.main()
