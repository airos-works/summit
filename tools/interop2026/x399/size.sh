#!/bin/sh
# size.sh: give the automated Summit window (test copy in /boot/home/interop/xh)
# an 800x600 CSS-pixel viewport (frame units are the same as CSS pixels here).
T=$(ps | grep "${SUMMIT_REMOTE_DIR:-/boot/home/interop}/xh/apps/Summit" | grep -v grep | awk '{print $(NF-3)}' | head -1)
[ -n "$T" ] || exit 1
for i in 0 1 2 3 4; do
	if hey $T get Title of Window $i 2>/dev/null | grep -q "(Automated)"; then
		hey $T set Frame of Window $i to "BRect(100,100,899,842)" >/dev/null
		echo "window $i"
		exit 0
	fi
done
exit 2
