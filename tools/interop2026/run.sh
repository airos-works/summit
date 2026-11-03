#!/bin/bash
# run.sh LIST OUTNAME [wpt run arguments]: run the tests in LIST (one URL
# prefix per line) against Summit on the test machine; writes
# $INTEROP_ROOT/results/OUTNAME.json (wptreport) and OUTNAME.log.
#
# INTEROP_ROOT      holds wpt/ (with wpt-summit.patch applied) and results/
#                   (default /data1/airos/interop2026)
# WPT_SERVER_HOST   this host's address on the test machine's network
#                   (default 192.168.1.200); wpt's host names resolve to it
#                   there (see the README)
# SUMMIT_REMOTE_BUILD  folder of the Summit to test on the test machine (default xh)
# SUMMIT_SCREEN_SHOTS=1  take reftest screenshots from the screen through
#                   Summit's MCP server instead of WebDriver (see the README)
KIT=$(cd "$(dirname "$0")" && pwd)
ROOT=${INTEROP_ROOT:-/data1/airos/interop2026}
DIR=${SUMMIT_REMOTE_DIR:-/boot/home/interop}
LIST=$(realpath "$1"); OUT=$2; shift 2
mkdir -p "$ROOT/results"
cd "$ROOT/wpt" || exit 1
export WPT_SERVER_HOST=${WPT_SERVER_HOST:-192.168.1.200}
export SUMMIT_REMOTE_BINARY=$DIR/${SUMMIT_REMOTE_BUILD:-xh}/apps/Summit SUMMIT_SHIM=$KIT/summitdriver-shim INTEROP_KIT=$KIT
# wptrunner wants a local binary path; the real one is SUMMIT_REMOTE_BINARY.
exec ./wpt run webkit --webkit-port=summit \
  --binary="$KIT/summitdriver-shim" \
  --binary-arg=--profile --binary-arg="$DIR/profile" \
  --webdriver-binary="$KIT/summitdriver-shim" \
  --include-file="$LIST" --processes=1 --no-fail-on-unexpected \
  --test-types testharness reftest print-reftest crashtest --enable-webtransport-h3 \
  --log-wptreport="$ROOT/results/$OUT.json" \
  --log-mach="$ROOT/results/$OUT.log" --log-mach-level=info \
  "$@"
