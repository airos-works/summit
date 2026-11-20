#!/usr/bin/env python3
"""score.py REPORT.json [REPORT2.json ...]: annual Interop focus-area scores for Summit's
wptreport(s) next to Safari Technology Preview and Safari stable (wpt.fyi
summaries), per focus area, plus a per-test table (tests.json) for triage.

Per-test score = passing subtests / subtests (a test without subtests: 1 when
it passes); area score = mean over the area's tests, x1000, as wpt.fyi does.
Later reports override earlier ones test by test (reruns)."""
import json, sys, os
from statistics import mean

from focus_areas import areas_by_year, display_area

ROOT = os.environ.get("INTEROP_ROOT", "/data1/airos/interop2026")
labels = json.load(open(f"{ROOT}/labels.json"))

def summit_scores(paths):
    out = {}
    for path in paths:
        for r in json.load(open(path))["results"]:
            subs = r.get("subtests", [])
            if subs:
                p = sum(1 for s in subs if s["status"] == "PASS")
                score, detail = p / len(subs), f"{p}/{len(subs)}"
            else:
                score = 1.0 if r["status"] in ("PASS", "OK") else 0.0
                detail = r["status"]
            out[r["test"]] = {"score": score, "status": r["status"], "detail": detail,
                              "failing": [s["name"] for s in subs if s["status"] != "PASS"][:40],
                              "messages": {s["name"]: (s.get("message") or "")[:300] for s in subs if s["status"] != "PASS"},
                              "message": (r.get("message") or "")[:500]}
    return out

def safari_scores(path):
    out = {}
    for t, v in json.load(open(path)).items():
        passed, total = v["c"]
        out[t] = passed / total if total else (1.0 if v["s"] in ("P", "O") else 0.0)
    return out

summit = summit_scores(sys.argv[1:])
stp = safari_scores(f"{ROOT}/stp-summary.json")
stable = safari_scores(f"{ROOT}/safari-summary.json")

tests = {}
print(f"{'area':36} {'tests':>5} {'ran':>5} {'Summit':>7} {'STP':>7} {'Safari':>7}")
for year, areas in sorted(areas_by_year(labels).items()):
    scores = []
    for area, urls in sorted(areas.items()):
        ran = sum(url in summit for url in urls)
        summit_score = sum(summit[url]["score"] for url in urls if url in summit) / len(urls) * 1000
        stp_score = mean(stp.get(url, 0) for url in urls) * 1000
        stable_score = mean(stable.get(url, 0) for url in urls) * 1000
        scores.append((summit_score, stp_score, stable_score))
        name = display_area(area)
        print(f"{name:36} {len(urls):5} {ran:5} {summit_score:7.1f} {stp_score:7.1f} {stable_score:7.1f}")

        for url in urls:
            if url not in tests:
                entry = dict(summit.get(url, {"score": None, "status": "NOT RUN"}))
                entry.update(areas=[], stp=stp.get(url), safari=stable.get(url))
                tests[url] = entry
            tests[url]["areas"].append(area)
            tests[url]["area"] = area  # Keep the legacy last-area field for existing readers.

    title = f"{year} TOTAL (mean of areas)".lstrip()
    summit_mean, stp_mean, stable_mean = map(mean, zip(*scores))
    print(f"{title:36} {'':5} {'':5} {summit_mean:7.1f} {stp_mean:7.1f} {stable_mean:7.1f}")
json.dump(tests, open(f"{ROOT}/results/tests.json", "w"), indent=1)
