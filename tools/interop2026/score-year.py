#!/usr/bin/env python3
"""score-year.py OUT.json RUN.json[+PART.json ...] [RERUN.json ...]

One Interop year's score for Summit from wptreport files, next to the current
wpt.fyi runs of Safari TP, WebKitGTK, Chrome and Firefox on the same labels.
Per URL: passing subtests / subtests (no subtests: 1 on PASS/OK); area = mean
over its labelled URLs (unrun = 0) x1000 floored; total = mean of areas.
"as run" = first report (parts merged); "best" = later reports replace a URL
when they score higher."""
import json, math, os, sys
from statistics import mean

D = os.environ.get("INTEROP_YEAR_DIR", "/data1/airos/interop2026/n24")
labels = json.load(open(f"{D}/labels.json"))
out_path, runs = sys.argv[1], sys.argv[2:]


def load(path):
    res = {}
    for r in json.load(open(path))["results"]:
        subs = r.get("subtests", [])
        if subs:
            p = sum(s["status"] == "PASS" for s in subs)
            score, detail = p / len(subs), f"{p}/{len(subs)}"
        else:
            score, detail = (1.0 if r["status"] in ("PASS", "OK") else 0.0), r["status"]
        res[r["test"]] = dict(score=score, status=r["status"], detail=detail,
                              failing={s["name"]: (s.get("message") or "")[:300]
                                       for s in subs if s["status"] != "PASS"},
                              message=(r.get("message") or "")[:300], run=path.rsplit("/", 1)[-1])
    return res


def fyi(name):
    out = {}
    for t, v in json.load(open(f"{D}/{name}-summary.json")).items():
        p, tot = v["c"]
        out[t] = p / tot if tot else (1.0 if v["s"] in ("P", "O") else 0.0)
    return out


def area(results, urls):
    if not urls:
        return 1000
    return math.floor(sum(math.floor(results.get(u, 0) * 1000) / 1000 for u in urls) / len(urls) * 1000)


first = {}
for part in runs[0].split("+") if runs else []:
    first.update(load(part))
best = dict(first)
for path in runs[1:]:
    for url, r in load(path).items():
        if url not in best or r["score"] > best[url]["score"]:
            best[url] = r

others = {n: fyi(n) for n in ("stp", "gtk", "chrome", "firefox")}
cols = dict(as_run={u: r["score"] for u, r in first.items()}, best={u: r["score"] for u, r in best.items()}, **others)
table = {a: {c: area(res, urls) for c, res in cols.items()} for a, urls in sorted(labels.items())}
print(f"{'area':48} {'urls':>5} {'ran':>5} " + " ".join(f"{c:>7}" for c in cols))
for a, row in table.items():
    print(f"{a:48} {len(labels[a]):5} {sum(u in best for u in labels[a]):5} " + " ".join(f"{row[c]:7}" for c in cols))
totals = {c: round(mean(row[c] for row in table.values()) / 10, 2) for c in cols}
print(f"{'TOTAL':60} " + " ".join(f"{totals[c]:7}" for c in cols))

allurls = set().union(*labels.values())
failing = {u: dict(r, stp=others["stp"].get(u), gtk=others["gtk"].get(u),
                   areas=[a for a, us in labels.items() if u in us])
           for u, r in best.items() if r["score"] < 1.0 and u in allurls}
missing = sorted(allurls - set(best))
print(f"failing URLs (best): {len(failing)}, not run: {len(missing)}")
json.dump(dict(table=table, totals=totals, failing=failing, missing=missing), open(out_path, "w"), indent=1)
