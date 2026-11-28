#!/bin/bash
# Builds Summit's engine (the same WebKit tree air/OS builds: upstream pin plus
# engine/patches/0001-haiku-port.patch) as WPE WebKit for Android.
#
#   tools/android/build-engine.sh <arm64|x86_64> [--reconfigure] [ninja targets...]
#
# The source is SUMMIT_ENGINE_SOURCE (default .cache/WebKit, kept equal to the
# committed patch by tools/update-engine-patch.py). The build is out of tree in
# $SUMMIT_ANDROID_ROOT/build/engine-<arch> and incremental; "install" (the
# default target set ends with it) puts the libraries and processes into the
# cerbero dependency prefix, which tools/android/stage-engine.py copies into the
# app. Dependencies come from tools/android/build-deps.sh.
set -euo pipefail
. "$(dirname "$0")/env.sh"

[ $# -ge 1 ] || { sed -n 2,12p "$0"; exit 2; }
summit_android_arch "$1"; shift
RECONFIGURE=0
if [ "${1:-}" = "--reconfigure" ]; then RECONFIGURE=1; shift; fi
TARGETS=("$@")

NDK="$SUMMIT_CERBERO_HOME/android-ndk-29"
[ -d "$NDK" ] || { echo "no NDK at $NDK: run tools/android/build-deps.sh first" >&2; exit 1; }
[ -f "$SUMMIT_DIST/lib/libglib-2.0.so" ] || { echo "dependencies missing in $SUMMIT_DIST" >&2; exit 1; }

# The app's minSdk. Libraries built for it may use every API up to it.
API=33

case "$SUMMIT_ARCH" in
    arm64) PROC=aarch64 ;;
    x86_64) PROC=x86_64 ;;
esac

if [ $RECONFIGURE = 1 ] || [ ! -f "$SUMMIT_ENGINE_BUILD/build.ninja" ]; then
    mkdir -p "$SUMMIT_ENGINE_BUILD"
    OPTIONS=(
        -G Ninja
        -S "$SUMMIT_ENGINE_SOURCE" -B "$SUMMIT_ENGINE_BUILD"
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
        -DANDROID_ABI="$ANDROID_ABI"
        -DANDROID_PLATFORM="$API"
        -DANDROID_STL=c++_shared
        # Google Play requires 16 KB page support for apps targeting API 35+.
        -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON
        -DANDROID=1
        -DCMAKE_SYSTEM_PROCESSOR="$PROC"
        -DCMAKE_FIND_ROOT_PATH="$SUMMIT_DIST"
        -DCMAKE_INSTALL_PREFIX="$SUMMIT_DIST"
        -DCMAKE_INSTALL_LIBDIR=lib
        -DCMAKE_SHARED_LINKER_FLAGS="-L$SUMMIT_DIST/lib"
        -DCMAKE_C_COMPILER_LAUNCHER=ccache
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
        -DCMAKE_BUILD_TYPE=Release
        # Chained precompiled headers break the NDK cross build (cerbero's
        # wpewebkit recipe has the same workaround).
        -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON
        -DPORT=WPE
        -DUSE_GSTREAMER_GL=ON
        # air/OS uses OpenSSL as the crypto backend too.
        -DUSE_OPENSSL=ON
        -DUSE_WOFF2=OFF
        -DENABLE_DOCUMENTATION=OFF
        -DENABLE_INTROSPECTION=OFF
        -DENABLE_JOURNALD_LOG=OFF
        -DENABLE_BUBBLEWRAP_SANDBOX=OFF
        -DENABLE_SPEECH_SYNTHESIS=OFF
        -DENABLE_SPELLCHECK=OFF
        -DENABLE_WEBXR=OFF
        -DUSE_ATK=OFF
        -DUSE_LCMS=OFF
        -DUSE_AVIF=OFF
        -DUSE_GBM=OFF
        -DUSE_LIBHYPHEN=OFF
        -DUSE_LIBDRM=OFF
        -DUSE_JPEGXL=OFF
        -DUSE_LIBBACKTRACE=OFF
        -DUSE_SKIA_OPENTYPE_SVG=OFF
        -DUSE_SYSPROF_CAPTURE=OFF
        -DENABLE_WEBDRIVER=ON
        -DENABLE_WPE_LEGACY_API=OFF
        -DENABLE_WPE_PLATFORM_DRM=OFF
        -DENABLE_WPE_PLATFORM_WAYLAND=OFF
        -DENABLE_WPE_PLATFORM_HEADLESS=ON
        -DENABLE_BACK_FORWARD_LIST_SWIFT=OFF
        -DICU_ROOT="$SUMMIT_DIST"
        -DZLIB_ROOT="$SUMMIT_DIST"
        -DFREETYPE_LIBRARY="$SUMMIT_DIST/lib/libfreetype.so"
        -DFREETYPE_INCLUDE_DIR_ft2build="$SUMMIT_DIST/include/freetype2"
        -DFREETYPE_INCLUDE_DIR_freetype2="$SUMMIT_DIST/include/freetype2"
        -DGLIB_COMPILE_RESOURCES_EXECUTABLE="$SUMMIT_CERBERO_HOME/build-tools/bin/glib-compile-resources"
        -DJPEG_LIBRARY="$SUMMIT_DIST/lib/libjpeg.so"
        -DJPEG_INCLUDE_DIR="$SUMMIT_DIST/include"
        -DPNG_LIBRARY="$SUMMIT_DIST/lib/libpng.so"
        -DPNG_INCLUDE_DIR="$SUMMIT_DIST/include"
    )
    # Extra options for experiments, e.g. SUMMIT_ENGINE_CMAKE_EXTRA="-DENABLE_WK_WEB_EXTENSIONS=ON".
    # shellcheck disable=SC2206
    OPTIONS+=(${SUMMIT_ENGINE_CMAKE_EXTRA:-})
    summit_cerbero run cmake "${OPTIONS[@]}"
fi

if [ ${#TARGETS[@]} -eq 0 ]; then TARGETS=(install); fi
CCACHE_DIR="${CCACHE_DIR:-$SUMMIT_ANDROID_ROOT/ccache}" CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-60G}" \
    summit_cerbero run ninja -C "$SUMMIT_ENGINE_BUILD" -j "$SUMMIT_ANDROID_JOBS" "${TARGETS[@]}"
