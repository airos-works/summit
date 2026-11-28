#!/bin/bash
# WPT on Summit for Android, as tools/interop2026/run.sh runs it on air/OS.
#
#   tools/android/run-wpt.sh WPT_DIR OUT [wpt run arguments: tests, --include-file=LIST ...]
#
# WPT_DIR is a wpt checkout with tools/interop2026/wpt-summit.patch (tools/*)
# and tools/android/wpt-summit-android.patch applied. Needs a debug build of
# the app on the device or emulator (WebDriver: tools/android/summit-webdriver).
# The display is set to 800x600 at 1x for the run (tests expect that viewport),
# and put back after. Results: OUT.json (wptreport) and OUT.log.
set -uo pipefail
KIT=$(cd "$(dirname "$0")" && pwd)
WPT=$(realpath "${1:?wpt checkout}")
OUT=$(realpath -m "${2:?output name}")
shift 2
. "$KIT/env.sh" 2>/dev/null || true
[ -f ~/android/env.sh ] && . ~/android/env.sh
ADB=${ADB:-adb}

"$ADB" shell wm size 800x600
"$ADB" shell wm density 160
python3 "$KIT/wpt-proxy.py" --port 8899 > "$OUT.proxy.log" 2>&1 &
proxy=$!
cleanup() {
    kill "$proxy" 2>/dev/null
    "$ADB" shell am force-stop works.airos.summit
    "$ADB" shell wm size reset
    "$ADB" shell wm density reset
}
trap cleanup EXIT

cd "$WPT" || exit 1
# wptserve binds to an address: this host has no hosts entries for
# web-platform.test, and the device reaches it through the proxy anyway.
WPT_SERVER_HOST=127.0.0.1 ./wpt run webkit --webkit-port=summit-android \
    --binary="$KIT/summit-webdriver" --webdriver-binary="$KIT/summit-webdriver" \
    --processes=1 --no-fail-on-unexpected --no-pause-after-test \
    --test-types testharness reftest print-reftest crashtest \
    --log-wptreport="$OUT.json" --log-mach="$OUT.log" --log-mach-level=info \
    "$@"
