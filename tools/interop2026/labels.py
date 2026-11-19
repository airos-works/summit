#!/usr/bin/env python3
"""Build the labelled WPT lists for one or more Interop years."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import urllib.request

import yaml

INTEROP_DATA = "https://raw.githubusercontent.com/web-platform-tests/wpt.fyi/main/webapp/static/interop-data.json"

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("metadata", type=Path, help="wpt-metadata checkout")
parser.add_argument("out", type=Path, help="destination for labels.json and all.txt")
parser.add_argument("--year", action="append", help="Interop year, repeatable (default 2026)")
parser.add_argument("--interop-data", type=Path, help="use a saved wpt.fyi interop-data.json")
args = parser.parse_args()
if not args.metadata.is_dir():
    parser.error(f"metadata directory does not exist: {args.metadata}")
years = list(dict.fromkeys(args.year or ["2026"]))
with (args.interop_data.open("rb") if args.interop_data else urllib.request.urlopen(INTEROP_DATA)) as source:
    catalog = json.load(source)

tests = {}
areas_by_label = defaultdict(list)
for year in years:
    if year not in catalog:
        parser.error(f"unknown Interop year {year}")
    for area, info in catalog[year]["focus_areas"].items():
        if info.get("countsTowardScore"):
            key = f"{year}/{area}" if len(years) > 1 else area
            tests[key] = set()
            for label in info["labels"]:
                areas_by_label[label].append(key)

for path in args.metadata.rglob("META.yml"):
    with path.open() as source:
        meta = yaml.safe_load(source) or {}
    prefix = path.parent.relative_to(args.metadata).as_posix()
    prefix = "/" if prefix == "." else f"/{prefix}/"
    for link in meta.get("links") or []:
        for area in areas_by_label.get(link.get("label"), ()):
            for result in link.get("results", []):
                tests[area].add(prefix + result["test"])

args.out.mkdir(parents=True, exist_ok=True)
args.out.joinpath("labels.json").write_text(json.dumps({area: sorted(ts) for area, ts in tests.items()}, indent=1))
everything = sorted(set().union(*tests.values()))
args.out.joinpath("all.txt").write_text("".join(test + "\n" for test in everything))
for area in sorted(tests):
    print(f"{area:50} {len(tests[area]):5}")
print(f"{'all':50} {len(everything):5}")
