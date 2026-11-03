#!/usr/bin/env python3
"""gaps.py [RUN ...]: best-estimate Summit score (each test counts its best
result over the named runs in results/, e.g. a WebDriver-screenshot run, an
on-screen reftest run and a rerun of the failures) and the overall points each
group of failures is worth if it reached Safari Technology Preview's result.
Writes results/issue-tests.json."""
import json, os, re, sys, collections
R = os.environ.get("INTEROP_ROOT", "/data1/airos/interop2026")
labels = json.load(open(f"{R}/labels.json"))
def score(r):
    subs = r.get("subtests", [])
    if subs: return sum(s["status"] == "PASS" for s in subs) / len(subs)
    return 1.0 if r["status"] in ("PASS", "OK") else 0.0
best = {}
for f in sys.argv[1:] or ["full1", "full1-reftest-screen", "rerun1"]:
    for r in json.load(open(f"{R}/results/{f}.json"))["results"]:
        best[r["test"]] = max(best.get(r["test"], 0), score(r))
stp = {t: (v["c"][0]/v["c"][1] if v["c"][1] else (1.0 if v["s"] in "PO" else 0.0)) for t, v in json.load(open(f"{R}/stp-summary.json")).items()}
groups = [  # (issue key, regex on test path)
 ("webtransport", r"^/webtransport/"),
 ("wd-pointer", r"dialog-closedby|dialog-popover-closedby|popover-implicit-anchor|user-select-001"),
 ("light-dismiss-hint", r"popover-light-dismiss-hint"),
 ("wd-wheel", r"scroll-snap/(input/mouse-wheel|overscroll-snap|unrelated-gesture)"),
 ("wd-sendkeys-body", r"navigation-api/focus-reset/"),
 ("wd-mock-capture", r"script-transform-generateKeyFrame"),
 ("webrtc-h264", r"^/webrtc/"),
 ("vt-crossdoc", r"css-view-transitions/navigation/|transition-in-hidden-page|start-skip-start|input-targets-root|window-resize-aborts"),
 ("vt-render", r"css-view-transitions/"),
 ("fetch-upload", r"request-upload\.h2"),
 ("fetch-range", r"^/fetch/range/"),
 ("nav-download", r"204-205-download"),
 ("nav-other", r"^/navigation-api/"),
 ("anchor-inline", r"css-anchor-position/"),
 ("snap-paged", r"scroll-snap/input/paged|scroll-target-align"),
 ("highlight", r"css-highlight-api/"),
 ("sda", r"scroll-animations/"),
 ("zoom", r"zoom/"),
]
area_of = {}
for a, ts in labels.items():
    for t in ts: area_of.setdefault(t, a)
gain = collections.defaultdict(float); tests = collections.defaultdict(list)
areas = {}
for a, ts in labels.items():
    areas[a] = sum(best.get(t, 0) for t in ts) / len(ts) * 1000
    for t in ts:
        d = stp.get(t, 0) - best.get(t, 0)
        if d <= 0.001: continue
        key = next((k for k, rx in groups if re.search(rx, t)), "other")
        gain[key] += d / len(ts) * 1000 / len(labels) / 10   # percentage points overall
        tests[key].append((t, round(best.get(t, 0), 2)))
total = sum(areas.values()) / len(areas) / 10
print(f"best-estimate overall {total:.1f}")
for a in sorted(areas): print(f"  {a[13:]:34} {areas[a]:7.1f}")
print()
for k, g in sorted(gain.items(), key=lambda x: -x[1]):
    print(f"{k:20} +{g:.2f} points  ({len(tests[k])} tests)")
json.dump({k: tests[k] for k in tests}, open(f"{R}/results/issue-tests.json", "w"), indent=1)
