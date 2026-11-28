#!/usr/bin/env python3
"""Rewrites PLATFORM(HAIKU) as USE(SUMMIT) on the preprocessor lines the engine
patch adds, in files under the given prefixes (minus Haiku-only files and
excluded files), unless the guarded block contains a marker of Haiku-only code.
USE(SUMMIT) is on for both builds (docs/android.md, "What is shared, and how").

  tools/summit-guards.py PREFIXES tools/summit-guards-exclude.txt \
      < engine/patches/0001-haiku-port.patch > rewritten.patch
  (apply rewritten.patch to the pinned upstream tree, build both ports,
  then tools/update-engine-patch.py --source TREE)

PREFIXES: comma-separated path prefixes, e.g. Source/WebCore/rendering/.
EXCLUDE_FILE lines: a path (that file stays Haiku), a path ending in "/" (that
directory does), "@MARKER" (a block containing MARKER stays Haiku), or
"path@MARKER". The compile of the Android build finds what the markers miss.

Phase 1 (November 2026) used these prefixes, under Source/WebCore/: rendering,
dom, layout, css, style, html, Modules, loader, bindings, svg, workers,
history, editing, animation, accessibility, xml, inspector, page,
platform/adwaita."""
import re, sys
prefixes = [p for p in sys.argv[1].split(",") if p]
exclude_files, markers, file_markers = set(), [], {}
for entry in open(sys.argv[2]).read().split("\n"):
    entry = entry.strip()
    if not entry or entry.startswith("#"): continue
    if entry.startswith("@"): markers.append(entry[1:])
    elif "@" in entry:
        path, marker = entry.split("@", 1); file_markers.setdefault(path, []).append(marker)
    else: exclude_files.add(entry)
lines = sys.stdin.read().split("\n")
IF = re.compile(r"[+ ]\s*#\s*(if|ifdef|ifndef)\b")
ELSE = re.compile(r"[+ ]\s*#\s*(else|elif)\b")
ENDIF = re.compile(r"[+ ]\s*#\s*endif\b")

def block(i):
    """Text of the block a guard at line i opens, up to its #else/#elif/#endif."""
    depth, text = 0, []
    for l in lines[i + 1:]:
        if l.startswith(("diff --git", "@@")): break
        if l.startswith("-"): continue
        if IF.match(l): depth += 1
        elif ENDIF.match(l):
            if depth == 0: break
            depth -= 1
        elif ELSE.match(l) and depth == 0: break
        text.append(l)
    return "\n".join(text)

current, eligible, stack, counts, kept = None, False, [], {}, 0
for i, line in enumerate(lines):
    m = re.match(r"diff --git a/(\S+) b/", line)
    if m:
        current = m.group(1); stack = []
        eligible = (any(current.startswith(p) for p in prefixes) and "haiku" not in current.lower()
                    and current not in exclude_files
                    and not any(e.endswith("/") and current.startswith(e) for e in exclude_files))
        continue
    if line.startswith("@@"): stack = []; continue
    if line.startswith("-") or not current: continue
    if IF.match(line) or ELSE.match(line):
        convert = None
        if "PLATFORM(HAIKU)" in line and line.startswith("+") and eligible:
            body = block(i)
            haiku_only = any(mk in body or mk in line for mk in markers + file_markers.get(current, []))
            convert = not haiku_only
            if haiku_only: kept += 1
        if IF.match(line): stack.append(convert)
        elif stack and convert is not None: stack[-1] = convert or stack[-1]
        if convert:
            lines[i] = line.replace("PLATFORM(HAIKU)", "USE(SUMMIT)")
            counts[current] = counts.get(current, 0) + 1
    elif ENDIF.match(line):
        convert = stack.pop() if stack else None
        if convert and line.startswith("+") and "PLATFORM(HAIKU)" in line:
            lines[i] = line.replace("PLATFORM(HAIKU)", "USE(SUMMIT)")
sys.stdout.write("\n".join(lines))
print("%d guards in %d files, %d kept Haiku-only" % (sum(counts.values()), len(counts), kept), file=sys.stderr)
