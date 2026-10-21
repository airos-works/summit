#!/usr/bin/env bash
# Run the profile-training workloads on the X399 against an instrumented
# bundle (built with -fprofile-generate -fprofile-update=atomic and linked with
# -Wl,-u,__gcov_dump_int so every module owns its dumper). Every process
# writes its counters on exit because
# SUMMIT_GCOV_DUMP=1. Haiku's libgcov does not lock the .gcda files, so the
# engine takes turns on a lock file (WTFProcess.cpp, SUMMIT_GCOV_LOCK) and
# helpers get three minutes before anyone is killed. The last step prints
# each sample file's run count: DOM, style and bindings code runs in every
# web process, so a count far below the others means merges were lost.
#
#   bash tools/bench/train-pgo.sh /boot/home/summit/build-modern-browser/BUNDLE [BUILD_NAME]
#
# Workloads: Speedometer 3.1, the 400-card scroll fixture, a Wikipedia wheel
# burst, two windows of real sites with a scroll while the other reloads, and
# video through NVDEC, through libavcodec (16 reference frames) and through
# Media Source Extensions. Needs tools/bench/make-refs-fixtures.sh and
# make-mse-fixtures.sh to have been run.
set -euo pipefail
bundle=${1:?usage: train-pgo.sh BUNDLE_PATH [BUILD_NAME]}
build=${2:-SkiaCGMiPGO2}
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
export SUMMIT_BENCH_HOST=workstation SUMMIT_BENCH_LEFTOVER_GRACE=180
env=(--env SUMMIT_LIBRARY_PATH_PREFIX=/boot/home/summit-mesa/prefix/lib --env SUMMIT_GCOV_DUMP=1
     --env SUMMIT_SKIA_GL_CONTEXT=1 --env SUMMIT_SCROLL_REFRESH_TIMER=16)
step() { echo "== $(date +%H:%M:%S) $*"; }

step speedometer
python3 tools/bench/run-speedometer.py --bundle "$bundle" --iterations 10 --label pgo2-train "${env[@]}" | grep -E '^score|contention' || true
step scroll fixture
python3 tools/bench/run-probe.py --bundle "$bundle" --page scroll.html --label pgo2-train-scroll "${env[@]}" | tail -1 || true
step wikipedia scroll
python3 tools/bench/run-scroll.py "https://en.wikipedia.org/wiki/Haiku_(operating_system)" --bundle "$bundle" --settle 10 \
    --notches 200 --window 0,24,1919,1079 --label pgo2-train-wiki "${env[@]}" | tail -1 || true
step real sites
python3 tools/bench/run-multitab.py --bundle "$bundle" --windows 2 --tabs 6 --idle 10 --load-timeout 120 --label pgo2-train "${env[@]}" \
    | grep -E 'load:|scroll under' | cut -c1-160 || true
for page in "media.html?src=refs-media/refs4.mp4&seek=6.5" "media.html?src=refs-media/refs16.mp4&seek=6.5" "mse.html?muted=1&seek=6.5"; do
    step "$page"
    python3 tools/bench/run-probe.py --bundle "$bundle" --page "$page" --label pgo2-train-media "${env[@]}" | tail -1 || true
done
step done
bash tools/ws.sh "find /boot/home/summit-webkit-extensions/WebKitBuild/$build -newer $bundle/Summit -name '*.gcda' | wc -l
cd /boot/home/summit-webkit-extensions/WebKitBuild/$build/Source/WebCore/CMakeFiles/WebCore.dir/__/__/WebCore/DerivedSources/unified-sources
for u in dom-1 dom-5 style-1 html-1 JSBindings-1 rendering-1 page-1; do
    echo \"\$u \$(gcov-dump -l UnifiedSource-\$u.cpp.gcda 2>/dev/null | grep -m1 -o 'runs=[0-9]*, sum_max=[0-9]*')\"
done"
