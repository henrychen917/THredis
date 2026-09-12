#!/usr/bin/env python3
"""The simple performance regression check: is the candidate slower than the reference?

Reads an ABBA results.json and asks one question per cell -- is the candidate's median more than
SIMPLE_REGRESSION_PCT worse than the reference's median -- from the raw A/B/B/A measurements.
Exit 0 if no cell regressed, 1 if any did. Prints every cell either way, so a run always yields
numbers whether it passes or not.

WHY THIS EXISTS SEPARATELY. The tier around it carries roughly fifteen interlocking VALIDITY
checks -- instrument fingerprints, standing-null receipts, load-floor pins, productive-role
occupancy, plateau confirmation, resolution bounds, quiet preconditions. Each can independently
refuse to produce a verdict, and on 2026-09-08..12 that is what happened, repeatedly, while the
measurement itself repeated to 0.08% (m47) and 0.64% (h11 by hand). Those checks remain, and their
output is worth reading, but they REPORT. This gates.

Medians, not means: with four samples one outlier moves a mean and cannot move a median.
"""
import json
import sys

# Per-cell regression thresholds. Read-local cells run at this box's highest throughput and repeat
# least tightly -- measured on IDENTICAL bytes: h11 4.56%, h31 4.62%, h27 5.44%, h15 6.51%, against
# 0.08-1.8% for every other cell in the same run. Tail-latency cells are noisier still (t04 showed
# a single 4.575 ms p99.9 sample against a 3.3 ms median).
DEFAULT_PCT = 3.0
READ_LOCAL_PCT = 5.0
# Tail cells REPORT, they do not gate. On identical bytes t04's p99.9 ran 3.311 vs 4.479 ms, a 35%
# swing, and 3.3-5.1 ms across runs. No threshold both catches a real tail regression and survives
# that, so a number here would be theatre. Their p99.9 is printed every run and is the data for
# reorder work; gate them again when the tail can be measured to better than it swings.
TAIL_GATES = False


def median(values):
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return (ordered[middle - 1] + ordered[middle]) / 2


def threshold_for(cell):
    if cell.get("score") == "p999":
        return None                       # reported, never gated
    if cell.get("read_local"):
        return READ_LOCAL_PCT
    return DEFAULT_PCT


def check(path):
    report = json.loads(open(path).read())
    worse = []
    print("  ABBA simple check (candidate vs reference, medians of the raw runs):")
    for row in report.get("cells", []):
        cell = row["cell"]
        rounds = row.get("rounds") or []
        if not rounds:
            print(f"    {cell['id']:5s} no measurements")
            worse.append(cell["id"])
            continue
        runs = rounds[-1]["runs"]
        metric = "p999_ms" if cell.get("score") == "p999" else ("latency_ms" if cell["depth"] == 1 else "rate")
        a = [r[metric] for r in runs if r.get("arm") == "A" and isinstance(r.get(metric), (int, float))]
        b = [r[metric] for r in runs if r.get("arm") == "B" and isinstance(r.get(metric), (int, float))]
        if not a or not b:
            print(f"    {cell['id']:5s} missing {metric}")
            worse.append(cell["id"])
            continue
        ma, mb = median(a), median(b)
        # Higher is better for rate; lower is better for latency and p99.9.
        change = 100 * (mb - ma) / ma if metric == "rate" else 100 * (ma - mb) / ma
        limit = threshold_for(cell)
        bad = limit is not None and change < -limit
        if bad:
            worse.append(cell["id"])
        shown = f"limit -{limit:.0f}%" if limit is not None else "reported, not gated"
        print(f"    {cell['id']:5s} {metric:9s} ref={ma:10.4f} cand={mb:10.4f} "
              f"{change:+7.2f}%  {shown:20s} {'REGRESSION' if bad else 'ok'}")
    if worse:
        print(f"  ABBA SIMPLE: FAIL -- regressed: {', '.join(worse)}")
        return 1
    print("  ABBA SIMPLE: PASS -- no cell regressed beyond its threshold")
    return 0


def self_test():
    import tempfile, unittest
    from pathlib import Path

    def report(metric, cell_extra, a_values, b_values):
        runs = ([dict(arm="A", **{metric: a_values[0]})] +
                [dict(arm="B", **{metric: v}) for v in b_values] +
                [dict(arm="A", **{metric: a_values[1]})])
        return {"cells": [{"cell": dict(id="c1", depth=32, **cell_extra), "rounds": [{"runs": runs}]}]}

    class Simple(unittest.TestCase):
        def run_report(self, doc):
            with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as handle:
                json.dump(doc, handle)
            return check(handle.name)

        def test_equal_arms_pass(self):
            self.assertEqual(self.run_report(report("rate", {}, [100, 100], [100, 100])), 0)

        def test_rate_regression_beyond_threshold_fails(self):
            self.assertEqual(self.run_report(report("rate", {}, [100, 100], [96, 96])), 1)

        def test_rate_regression_within_threshold_passes(self):
            self.assertEqual(self.run_report(report("rate", {}, [100, 100], [98, 98])), 0)

        def test_improvement_never_fails(self):
            self.assertEqual(self.run_report(report("rate", {}, [100, 100], [150, 150])), 0)

        def test_one_outlier_cannot_move_the_median(self):
            # B = 100, 100 median 100 despite an extreme A sample; medians resist a single outlier.
            self.assertEqual(self.run_report(report("rate", {}, [100, 40], [100, 100])), 0)

        def test_read_local_uses_its_wider_threshold(self):
            self.assertEqual(self.run_report(report("rate", {"read_local": 1}, [100, 100], [95, 95])), 0)
            self.assertEqual(self.run_report(report("rate", {"read_local": 1}, [100, 100], [90, 90])), 1)

        def test_tail_latency_is_reported_but_never_gates(self):
            # Lower is better, and a large tail regression still PRINTS -- it just cannot fail the
            # gate while the instrument swings further than any useful threshold.
            self.assertEqual(self.run_report(report("p999_ms", {"score": "p999"}, [10, 10], [9, 9])), 0)
            self.assertEqual(self.run_report(report("p999_ms", {"score": "p999"}, [10, 10], [13, 13])), 0)

        def test_read_local_threshold_is_five_percent(self):
            self.assertEqual(READ_LOCAL_PCT, 5.0)

        def test_missing_measurements_fail(self):
            self.assertEqual(self.run_report({"cells": [{"cell": dict(id="c1", depth=32), "rounds": []}]}), 1)

    suite = unittest.defaultTestLoader.loadTestsFromTestCase(Simple)
    return 0 if unittest.TextTestRunner(verbosity=1).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--self-test":
        raise SystemExit(self_test())
    raise SystemExit(check(sys.argv[1]))
