#!/bin/bash
# Cross-builds the engine's dependencies for Android with cerbero (Igalia's
# wpe-android-cerbero fork, which knows how to build GLib, libsoup, GStreamer
# with the androidmedia hardware codecs, ICU, HarfBuzz and the rest for the NDK).
#
#   tools/android/build-deps.sh <arm64|x86_64>
#
# The first run clones cerbero into $SUMMIT_ANDROID_ROOT, bootstraps it (its
# own NDK and build tools) and takes a couple of hours; later runs only build
# what changed. The engine itself is built by tools/android/build-engine.sh.
set -euo pipefail
. "$(dirname "$0")/env.sh"

[ $# -eq 1 ] || { sed -n 2,10p "$0"; exit 2; }
summit_android_arch "$1"

# The cerbero revision Summit is tested with (wpe-android-cerbero main, 2026-11-27).
CERBERO_URL=https://github.com/Igalia/wpe-android-cerbero.git
CERBERO_REV=ded35792

mkdir -p "$SUMMIT_ANDROID_ROOT"
if [ ! -d "$SUMMIT_CERBERO_DIR/.git" ]; then
    git clone "$CERBERO_URL" "$SUMMIT_CERBERO_DIR"
    git -C "$SUMMIT_CERBERO_DIR" checkout -q "$CERBERO_REV"
fi
if [ ! -f "$SUMMIT_CERBERO_CONFIG" ]; then
    cat > "$SUMMIT_CERBERO_CONFIG" <<CBC
# Summit Android: keep every cerbero file under SUMMIT_ANDROID_ROOT.
home_dir = '$SUMMIT_CERBERO_HOME'
local_sources = '$SUMMIT_ANDROID_ROOT/cerbero-sources'
num_of_cpus = $SUMMIT_ANDROID_JOBS
CBC
fi

if [ ! -d "$SUMMIT_CERBERO_HOME/build-tools/bin" ] || [ ! -d "$SUMMIT_CERBERO_HOME/android-ndk-29" ]; then
    summit_cerbero bootstrap --system=no -j "$SUMMIT_ANDROID_JOBS"
fi

# What the engine links (cerbero's wpewebkit recipe deps, less OpenXR, which
# Summit does not enable) plus curl for the network process.
summit_cerbero build -j "$SUMMIT_ANDROID_JOBS" \
    icu freetype gettext gstreamer-1.0 gst-plugins-base-1.0 gst-plugins-good-1.0 gst-plugins-bad-1.0 \
    harfbuzz libepoxy libjpeg-turbo libpng libsoup libxslt libwebp openjpeg openssl xkbcommon \
    curl nghttp2 sqlite3
