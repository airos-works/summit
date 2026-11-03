#!/usr/bin/env python3
"""labels.py WPT_METADATA_DIR OUT_DIR: the Interop 2026 test lists.

Reads wpt.fyi's interop-data.json (the focus areas and the labels that count
for 2026) and the META.yml files of a wpt-metadata checkout, and writes
OUT_DIR/labels.json ({area: [test, ...]}) and OUT_DIR/all.txt (one test per
line, for `wpt run --include-file`)."""
import json, os, sys, urllib.request

import yaml

INTEROP_DATA = "https://raw.githubusercontent.com/web-platform-tests/wpt.fyi/main/webapp/static/interop-data.json"

metadata, out = sys.argv[1], sys.argv[2]
areas = json.load(urllib.request.urlopen(INTEROP_DATA))["2026"]["focus_areas"]
label_area = {label: area for area, info in areas.items() if info.get("countsTowardScore")
              for label in info["labels"]}

tests = {area: set() for area in set(label_area.values())}
for root, _, files in os.walk(metadata):
    if "META.yml" not in files:
        continue
    meta = yaml.safe_load(open(os.path.join(root, "META.yml"))) or {}
    base = "/" + os.path.relpath(root, metadata)
    for link in meta.get("links") or []:
        area = label_area.get(link.get("label"))
        if area:
            for result in link.get("results", []):
                tests[area].add(f"{base}/{result['test']}")

os.makedirs(out, exist_ok=True)
json.dump({area: sorted(ts) for area, ts in tests.items()}, open(f"{out}/labels.json", "w"), indent=1)
everything = sorted(set().union(*tests.values()))
open(f"{out}/all.txt", "w").write("\n".join(everything) + "\n")
for area in sorted(tests):
    print(f"{area:50} {len(tests[area]):5}")
print(f"{'all':50} {len(everything):5}")
