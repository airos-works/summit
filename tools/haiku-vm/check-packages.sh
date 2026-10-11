#!/usr/bin/env bash
# tools/haiku-vm/check-packages.sh SUMMIT_WEBKIT.hpkg SUMMIT.hpkg
#
# Installs a Summit build on the stock Haiku machine of run-stock-vm.sh the way
# a user would (pkgman, with HaikuPorts for what the packages require) and runs
# the platform checks: pages/platform.html with H.264 and VP9 files (from
# SUMMIT_TEST_VIDEO_DIR, or already in /Data/pages), a page
# over HTTPS, the About box's build name, the runtime loader's complaints in the
# syslog, and that the default browser was left alone. The result lines are
# printed; screenshots go to the machine's directory.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
DIR=${SUMMIT_HAIKU_VM:-$ROOT/.vm/haiku-r1beta6}
ENGINE=${1:?usage: check-packages.sh SUMMIT_WEBKIT.hpkg SUMMIT.hpkg}
BROWSER=${2:?usage: check-packages.sh SUMMIT_WEBKIT.hpkg SUMMIT.hpkg}
vm() { "$ROOT/tools/haiku-vm/ssh.sh" "$@"; }
put() { scp -q -i "$DIR/id_ed25519" -P "${SUMMIT_HAIKU_VM_PORT:-2241}" -o IdentitiesOnly=yes \
	-o UserKnownHostsFile="$DIR/known_hosts" "$@"; }
shot() { SUMMIT_VM_QMP=$DIR/qmp.sock python3 "$ROOT/tools/vm.py" screenshot "$DIR/$1.png" > /dev/null; }

vm 'mkdir -p /Data/pkgs /Data/pages /Data/profiles'
put "$ENGINE" "$BROWSER" user@127.0.0.1:/Data/pkgs/
put "$ROOT"/tools/haiku-vm/pages/*.html user@127.0.0.1:/Data/pages/
# test-h264.mp4 and test-vp9.webm, for example from ffmpeg's testsrc
[[ -z ${SUMMIT_TEST_VIDEO_DIR:-} ]] || put "$SUMMIT_TEST_VIDEO_DIR"/test-* user@127.0.0.1:/Data/pages/

echo "== install"
default_before=$(vm 'setmime -get text/html 2>&1 | grep -i preferred || true')
vm "cd /Data/pkgs && pkgman install -y ./$(basename "$ENGINE") ./$(basename "$BROWSER") 2>&1" \
	| grep -E 'install package|update package|uninstall package|error|Error|Done' || true
default_after=$(vm 'setmime -get text/html 2>&1 | grep -i preferred || true')
[[ $default_before == "$default_after" ]] && echo "default browser: unchanged" \
	|| echo "default browser: CHANGED ($default_before -> $default_after)"

echo "== build"
vm 'grep -a -o "Built for [A-Za-z0-9/ ]*" /boot/system/apps/Summit | head -1; pkgman search -i -D summit 2>/dev/null | tail -n +3'

echo "== run"
vm 'syslog_mark=$(wc -l < /var/log/syslog); echo $syslog_mark > /Data/syslog.mark
	(/boot/system/apps/Summit --profile /Data/profiles/check https://example.com > /Data/summit-check.log 2>&1 &)
	sleep 15'
shot check-example
vm 'hey Summit get Title of Window 0 | grep result'
vm '/boot/system/apps/Summit --profile /Data/profiles/check "file:///Data/pages/platform.html?video=test-h264.mp4" > /dev/null 2>&1; sleep 12; hey Summit get Title of Window 0 | grep result'
shot check-h264
vm '/boot/system/apps/Summit --profile /Data/profiles/check "file:///Data/pages/platform.html?video=test-vp9.webm" > /dev/null 2>&1; sleep 12; hey Summit get Title of Window 0 | grep result'
shot check-vp9
vm '/boot/system/apps/Summit --profile /Data/profiles/check "https://en.wikipedia.org/wiki/Haiku_(operating_system)" > /dev/null 2>&1; sleep 15; hey Summit get Title of Window 0 | grep result'
shot check-wikipedia

echo "== runtime loader and stderr"
vm 'tail -n +$(( $(cat /Data/syslog.mark) + 1 )) /var/log/syslog | grep -E "runtime_loader|debug_server|Killing team" | sort | uniq -c | head -30; echo "--- stderr"; head -30 /Data/summit-check.log'
