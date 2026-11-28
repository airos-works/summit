#!/usr/bin/env python3
"""Compares a wpt run on Android with one on air/OS, test by test.

  tools/android/compare-wpt.py ANDROID.json AIROS.json [more AIROS.json ...]

Both are wptreport files (wpt run --log-wptreport). Only the tests of the
Android run count. A test's score is the fraction of its subtests that pass
(or 1/0 by its status when it has none), as Interop scores tests; the air/OS
result for a test is taken from the last file that has it (reruns after the
main run). Prints the totals and the tests whose scores differ.
"""

import json
import sys

PASSING = {"PASS", "OK"}


def scores(paths):
    result = {}
    for path in paths:
        report = json.load(open(path))
        for test in report.get("results", []):
            subtests = test.get("subtests", [])
            if subtests:
                score = sum(s["status"] == "PASS" for s in subtests) / len(subtests)
                if test["status"] not in PASSING:
                    score = 0.0
            else:
                score = 1.0 if test["status"] == "PASS" else 0.0
            result[test["test"]] = (score, test["status"], len(subtests))
    return result


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    android = scores([sys.argv[1]])
    airos = scores(sys.argv[2:])
    common = [t for t in android if t in airos]
    missing = [t for t in android if t not in airos]
    total_android = sum(android[t][0] for t in common)
    total_airos = sum(airos[t][0] for t in common)
    print("tests: %d (%d not in the air/OS run)" % (len(android), len(missing)))
    if common:
        print("score: Android %.2f%%, air/OS %.2f%% over the %d common tests" %
              (100 * total_android / len(common), 100 * total_airos / len(common), len(common)))
    worse = sorted((t for t in common if android[t][0] < airos[t][0] - 1e-9), key=lambda t: android[t][0] - airos[t][0])
    better = [t for t in common if android[t][0] > airos[t][0] + 1e-9]
    print("\nworse on Android (%d):" % len(worse))
    for t in worse:
        print("  %-90s android %.2f %-7s  air/OS %.2f %s" % (t, android[t][0], android[t][1], airos[t][0], airos[t][1]))
    print("\nbetter on Android (%d):" % len(better))
    for t in better:
        print("  %-90s android %.2f %-7s  air/OS %.2f %s" % (t, android[t][0], android[t][1], airos[t][0], airos[t][1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
