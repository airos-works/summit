#!/usr/bin/env bash
# Read-only hardware enumeration and policy checks. Does not open, configure,
# reset or transfer to an eligible device. Run on Haiku after source staging.
set -euo pipefail
SUMMIT_USB_ROOT=$(cd "$(dirname "$0")/.." && pwd)
SUMMIT_USB_SOURCE=${SUMMIT_USB_SOURCE:-$SUMMIT_USB_ROOT/.cache/WebKit/Source/WebKit/UIProcess/haiku/usb}
SUMMIT_USB_OUTPUT=${SUMMIT_USB_OUTPUT:-$SUMMIT_USB_ROOT/.vm/usb-tests}
mkdir -p "$SUMMIT_USB_OUTPUT"
c++ -std=c++20 -O2 -Wall -Wextra -I"$SUMMIT_USB_SOURCE" \
    "$SUMMIT_USB_ROOT/tests/USBPolicyTests.cpp" -o "$SUMMIT_USB_OUTPUT/USBPolicyTests"
"$SUMMIT_USB_OUTPUT/USBPolicyTests"
# The WebHID report descriptor parser is portable: this also runs on the host.
c++ -std=c++20 -O2 -Wall -Wextra -I"$SUMMIT_USB_SOURCE" \
    "$SUMMIT_USB_ROOT/tests/HIDDescriptorTests.cpp" -o "$SUMMIT_USB_OUTPUT/HIDDescriptorTests"
"$SUMMIT_USB_OUTPUT/HIDDescriptorTests"
c++ -std=c++20 -O2 -Wall -Wextra -fPIC -DSUMMIT_USB_STANDALONE -I"$SUMMIT_USB_SOURCE" \
    "$SUMMIT_USB_SOURCE/USBDeviceSessionHaiku.cpp" "$SUMMIT_USB_ROOT/tests/USBNativeTests.cpp" \
    -ldevice -lbe -o "$SUMMIT_USB_OUTPUT/USBNativeTests"
"$SUMMIT_USB_OUTPUT/USBNativeTests"
