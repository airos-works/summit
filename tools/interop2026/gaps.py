#!/usr/bin/env python3
"""gaps.py [RUN ...]: best-estimate Summit score (each test counts its best
result over the named runs in results/, e.g. a WebDriver-screenshot run, an
on-screen reftest run and a rerun of the failures) and the annual focus-area points each
group of failures is worth if it reached Safari Technology Preview's result.
Writes results/issue-tests.json."""
import json, os, re, sys, collections

from focus_areas import areas_by_year, display_area

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
tests = collections.defaultdict(dict)
for year, areas in sorted(areas_by_year(labels).items()):
    area_scores = {}
    gain = collections.defaultdict(float)
    annual_tests = collections.defaultdict(set)
    for area, urls in areas.items():
        area_scores[area] = sum(best.get(url, 0) for url in urls) / len(urls) * 1000
        for url in urls:
            score_difference = stp.get(url, 0) - best.get(url, 0)
            if score_difference <= 0.001:
                continue
            key = next((key for key, regex in groups if re.search(regex, url)), "other")
            gain[key] += score_difference * 100 / (len(urls) * len(areas))
            annual_tests[key].add(url)
            tests[key][url] = round(best.get(url, 0), 2)

    total = sum(area_scores.values()) / len(areas) / 10
    print(f"{year} best-estimate overall {total:.1f}".lstrip())
    for area in sorted(area_scores):
        name = display_area(area)
        print(f"  {name:34} {area_scores[area]:7.1f}")
    print()
    for key, points in sorted(gain.items(), key=lambda pair: -pair[1]):
        print(f"{key:20} +{points:.2f} points  ({len(annual_tests[key])} tests)")
json.dump({key: list(urls.items()) for key, urls in tests.items()}, open(f"{R}/results/issue-tests.json", "w"), indent=1)
