#!/usr/bin/env python3
"""score.py REPORT.json [REPORT2.json ...]: Interop 2026 scores for Summit's
wptreport(s) next to Safari Technology Preview and Safari stable (wpt.fyi
summaries), per focus area, plus a per-test table (tests.json) for triage.

Per-test score = passing subtests / subtests (a test without subtests: 1 when
it passes); area score = mean over the area's tests, x1000, as wpt.fyi does.
Later reports override earlier ones test by test (reruns)."""
import json, sys, os

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

rows, tests = [], {}
for area in sorted(labels):
    ts = labels[area]
    ran = [t for t in ts if t in summit]
    s = sum(summit[t]["score"] for t in ts if t in summit) / len(ts) * 1000
    p = sum(stp.get(t, 0) for t in ts) / len(ts) * 1000
    q = sum(stable.get(t, 0) for t in ts) / len(ts) * 1000
    rows.append((area.replace("interop-2025-", "").replace("interop-2026-", ""), len(ts), len(ran), s, p, q))
    for t in ts:
        e = dict(summit.get(t, {"score": None, "status": "NOT RUN"}))
        e.update(area=area, stp=stp.get(t), safari=stable.get(t))
        tests[t] = e

print(f"{'area':36} {'tests':>5} {'ran':>5} {'Summit':>7} {'STP':>7} {'Safari':>7}")
for name, n, ran, s, p, q in rows:
    print(f"{name:36} {n:5} {ran:5} {s:7.1f} {p:7.1f} {q:7.1f}")
k = len(rows)
print(f"{'TOTAL (mean of areas)':36} {'':5} {'':5} {sum(r[3] for r in rows)/k:7.1f} {sum(r[4] for r in rows)/k:7.1f} {sum(r[5] for r in rows)/k:7.1f}")
json.dump(tests, open(f"{ROOT}/results/tests.json", "w"), indent=1)
