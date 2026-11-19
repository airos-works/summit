#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
# The build host is selected with SUMMIT_REMOTE_SHELL: tools/haiku.sh (the QEMU
# VM, the default) or tools/ws.sh (the workstation). SUMMIT_REMOTE_TAG names the
# host in the host-side lock files so both can build at the same time.
SUMMIT_REMOTE_SHELL=${SUMMIT_REMOTE_SHELL:-tools/haiku.sh}
SUMMIT_REMOTE_TAG=${SUMMIT_REMOTE_TAG:-vm}
if [[ ! $SUMMIT_REMOTE_SHELL =~ ^tools/(haiku|ws)\.sh$ || ! $SUMMIT_REMOTE_TAG =~ ^[a-z][a-z0-9-]{0,15}$ ]]; then
    echo 'SUMMIT_REMOTE_SHELL must be tools/haiku.sh or tools/ws.sh.' >&2
    exit 2
fi
SUMMIT_ENGINE_MODE=legacy
case "${1:-}" in
    --modern) SUMMIT_ENGINE_MODE=modern; shift ;;
    --modern-extensions) SUMMIT_ENGINE_MODE=extensions; shift ;;
esac
SUMMIT_ENGINE_SOURCE=/boot/home/summit-webkit
if [[ $SUMMIT_ENGINE_MODE == extensions ]]; then
    # Keep the feature-enabled dependency closure separate from the browser preview.
    SUMMIT_ENGINE_SOURCE=/boot/home/summit-webkit-extensions
fi
SUMMIT_ENGINE_TARGET=${1:-all}
# A second configuration (for example GL compositing with mimalloc) can be built beside the
# stable one: SUMMIT_ENGINE_BUILD_NAME=ModernGL SUMMIT_ENGINE_CMAKE_EXTRA="-DUSE_MIMALLOC=ON ...".
SUMMIT_ENGINE_BUILD_NAME=${SUMMIT_ENGINE_BUILD_NAME:-Modern}
SUMMIT_ENGINE_CMAKE_EXTRA=${SUMMIT_ENGINE_CMAKE_EXTRA:-}
# CMake preserves old OFF cache entries even when port defaults change. Migrate
# existing modern builds too; explicit feature overrides still take precedence.
if [[ $SUMMIT_ENGINE_MODE != legacy ]]; then
    for SUMMIT_FEATURE in ENABLE_FULLSCREEN_API ENABLE_OFFSCREEN_CANVAS ENABLE_OFFSCREEN_CANVAS_IN_WORKERS USE_JPEGXL USE_HEIF USE_JPEGXR; do
        if [[ " $SUMMIT_ENGINE_CMAKE_EXTRA " != *" -D$SUMMIT_FEATURE="* ]]; then
            SUMMIT_ENGINE_CMAKE_EXTRA="$SUMMIT_ENGINE_CMAKE_EXTRA -D$SUMMIT_FEATURE=ON"
        fi
    done
fi
# CMake preserves old cache entries across builds. The Haiku port defaults to
# mimalloc, but an older system-malloc build directory can silently retain the
# slower allocator unless the choice is explicit at configure time.
if [[ $SUMMIT_ENGINE_MODE == extensions && $SUMMIT_ENGINE_CMAKE_EXTRA != *-DUSE_MIMALLOC=* && $SUMMIT_ENGINE_CMAKE_EXTRA != *-DUSE_SYSTEM_MALLOC=* ]]; then
    SUMMIT_ENGINE_CMAKE_EXTRA="$SUMMIT_ENGINE_CMAKE_EXTRA -DUSE_MIMALLOC=ON -DUSE_SYSTEM_MALLOC=OFF"
fi
if [[ ! $SUMMIT_ENGINE_BUILD_NAME =~ ^[A-Za-z][A-Za-z0-9_-]{0,31}$ ]]; then
    echo 'SUMMIT_ENGINE_BUILD_NAME must be a short alphanumeric name.' >&2
    exit 2
fi
for SUMMIT_CMAKE_ARGUMENT in $SUMMIT_ENGINE_CMAKE_EXTRA; do
    if [[ ! $SUMMIT_CMAKE_ARGUMENT =~ ^-D[A-Za-z0-9_]+=[A-Za-z0-9_./+-]*$ ]]; then
        echo "Unsupported extra CMake argument: $SUMMIT_CMAKE_ARGUMENT" >&2
        exit 2
    fi
done
if [[ $SUMMIT_ENGINE_BUILD_NAME != Modern && ${SUMMIT_ENGINE_MODE} != extensions ]]; then
    echo 'Alternate build directories are only supported with --modern-extensions.' >&2
    exit 2
fi
# GCC's collector is held to a tenth of its usual growth because the VM has
# 20 GiB for up to six jobs. A machine with memory to spare should not pay for
# that: dropping the parameter roughly halves the time of WebCore's large
# unified sources. Changing it reconfigures CMake and rebuilds everything.
SUMMIT_ENGINE_CXX_FLAGS=${SUMMIT_ENGINE_CXX_FLAGS:--ftrack-macro-expansion=0 --param ggc-min-expand=10}
if [[ $SUMMIT_ENGINE_CXX_FLAGS == *[\"\$\`\\]* ]]; then
    echo 'SUMMIT_ENGINE_CXX_FLAGS must not contain quoting or substitution characters.' >&2
    exit 2
fi
SUMMIT_WEBKIT_JOBS=${SUMMIT_WEBKIT_JOBS:-3}
if [[ ! $SUMMIT_WEBKIT_JOBS =~ ^[1-9][0-9]?$ ]] || (( SUMMIT_WEBKIT_JOBS > 64 )); then
    echo 'SUMMIT_WEBKIT_JOBS must be an integer between 1 and 64.' >&2
    exit 2
fi
case "$SUMMIT_ENGINE_MODE:$SUMMIT_ENGINE_TARGET" in
    legacy:all|legacy:JavaScriptCore|legacy:jsc|legacy:WebCore|legacy:WebKitLegacy) ;;
    modern:all|modern:JavaScriptCore|modern:jsc|modern:WebCore|modern:WebKit|modern:WebProcess|modern:NetworkProcess) ;;
    extensions:all|extensions:JavaScriptCore|extensions:jsc|extensions:WebCore|extensions:WebKit|extensions:WebProcess|extensions:NetworkProcess|extensions:Skia) ;;
    *) echo 'Usage: build-webkit-in-vm.sh [--modern|--modern-extensions] [all|JavaScriptCore|jsc|WebCore|WebKitLegacy|WebKit|WebProcess|NetworkProcess|Skia]' >&2; exit 2 ;;
esac
if (( $# > 1 )); then
    echo 'Only one engine target can be selected.' >&2
    exit 2
fi
mkdir -p .vm
exec 9>".vm/engine-build-$SUMMIT_REMOTE_TAG-$SUMMIT_ENGINE_BUILD_NAME.lock"
if ! flock -n 9; then
    echo 'Another Summit engine build script is active.' >&2
    exit 1
fi
# Also catch builds started directly over SSH before changing their sources.
# Separate build directories may run side by side; the same directory never twice.
bash "$SUMMIT_REMOTE_SHELL" "if ps | /bin/grep -Eq \"[n]inja -C WebKitBuild/$SUMMIT_ENGINE_BUILD_NAME \"; then echo \"An engine build of WebKitBuild/$SUMMIT_ENGINE_BUILD_NAME is active in the VM; inspect its existing log and process.\" >&2; exit 1; fi"
if [[ $SUMMIT_ENGINE_MODE != legacy ]]; then
    bash "$SUMMIT_REMOTE_SHELL" 'test -f /boot/home/summit-deps/icu78/lib/libicuuc.so.78.3 || { echo "Build private ICU first with tools/build-icu-in-vm.sh." >&2; exit 1; }'
fi
if [[ $SUMMIT_ENGINE_MODE == extensions ]]; then
    bash "$SUMMIT_REMOTE_SHELL" 'mkdir -p /boot/home/summit/tools /boot/home/summit/engine'
    tar -cf - tools/prepare-extension-deps.py engine/libzip.lock.json |
        bash "$SUMMIT_REMOTE_SHELL" 'tar -xf - -C /boot/home/summit && python3.10 /boot/home/summit/tools/prepare-extension-deps.py'
fi
if [[ $SUMMIT_ENGINE_MODE != legacy ]]; then
    # The image codecs live in a private prefix with verified source and file
    # manifests. Dependency work must finish before any engine sources change.
    bash "$SUMMIT_REMOTE_SHELL" 'mkdir -p /boot/home/summit/tools /boot/home/summit/engine'
    tar -cf - tools/prepare-image-codecs.py engine/image-codecs.lock.json engine/image-codec-patches |
        bash "$SUMMIT_REMOTE_SHELL" 'tar -xf - -C /boot/home/summit && python3.10 /boot/home/summit/tools/prepare-image-codecs.py'
    tar -cf - tools/prepare-ffmpeg-headers.py engine/ffmpeg-headers.lock.json |
        bash "$SUMMIT_REMOTE_SHELL" 'tar -xf - -C /boot/home/summit && python3.10 /boot/home/summit/tools/prepare-ffmpeg-headers.py'
    if [[ " $SUMMIT_ENGINE_CMAKE_EXTRA " != *" -DHAIKU_QUIC_ROOT="* ]]; then
        tar -cf - tools/prepare-webtransport-libs.py |
            bash "$SUMMIT_REMOTE_SHELL" 'tar -xf - -C /boot/home/summit'
        SUMMIT_QUIC_ROOT=$(bash "$SUMMIT_REMOTE_SHELL" '
            if ! pkg-config --exists libngtcp2 libngtcp2_crypto_ossl libnghttp3 && pkg-config --atleast-version=3.5 openssl; then
                python3.10 /boot/home/summit/tools/prepare-webtransport-libs.py --jobs '"$SUMMIT_WEBKIT_JOBS"'
            fi')
        if [[ -n $SUMMIT_QUIC_ROOT && ! $SUMMIT_QUIC_ROOT =~ ^/boot/home/summit-deps/[a-z0-9.-]+$ ]]; then
            echo "Unexpected private WebTransport prefix: $SUMMIT_QUIC_ROOT" >&2
            exit 1
        fi
        SUMMIT_ENGINE_CMAKE_EXTRA+=" -DHAIKU_QUIC_ROOT=$SUMMIT_QUIC_ROOT"
    fi
fi
python3 tools/prepare-webkit.py
SUMMIT_ENGINE_PATCH_SHA=$(python3 -c 'import json; print(json.load(open("engine/sources.lock.json"))["patch"]["sha256"])')
if [[ ! $SUMMIT_ENGINE_PATCH_SHA =~ ^[0-9a-f]{64}$ ]]; then
    echo 'Invalid engine patch digest.' >&2
    exit 1
fi
bash "$SUMMIT_REMOTE_SHELL" 'mkdir -p /boot/home/summit/tools && cat > /boot/home/summit/tools/sync-webkit-sources.py' < tools/sync-webkit-sources.py
# Source paths are fixed and passed as archive members, never as remote commands.
tar -C .cache/WebKit -czf - --exclude=__pycache__ CMakeLists.txt Configurations Source Tools |
    bash "$SUMMIT_REMOTE_SHELL" 'python3.10 /boot/home/summit/tools/sync-webkit-sources.py' "$SUMMIT_ENGINE_SOURCE" "$SUMMIT_ENGINE_PATCH_SHA"
# Keep large compiler temporaries on the engine volume, including when the
# extension source is a symlink to the separate native build disk.
if [[ $SUMMIT_ENGINE_MODE == extensions ]]; then
    bash "$SUMMIT_REMOTE_SHELL" 'export PKG_CONFIG_PATH=/boot/home/summit-deps/image-codecs-20261006/lib/pkgconfig:/boot/home/summit-deps/libzip-1.11.4/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}; cd /boot/home/summit-webkit-extensions && cmake -S . -B WebKitBuild/'"$SUMMIT_ENGINE_BUILD_NAME"' -G Ninja -DPORT=Haiku -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="'"$SUMMIT_ENGINE_CXX_FLAGS"'" -DENABLE_WEBKIT=ON -DENABLE_WEBKIT_LEGACY=OFF -DENABLE_WK_WEB_EXTENSIONS=ON -DENABLE_CONTENT_EXTENSIONS=ON -DENABLE_GPU_PROCESS=OFF -DENABLE_LAYOUT_TESTS=OFF -DICU_ROOT=/boot/home/summit-deps/icu78 -DCMAKE_INSTALL_PREFIX=/boot/home/summit-webkit-extensions-install '"$SUMMIT_ENGINE_CMAKE_EXTRA"' && mkdir -p WebKitBuild/'"$SUMMIT_ENGINE_BUILD_NAME"'/tmp && TMPDIR="$PWD/WebKitBuild/'"$SUMMIT_ENGINE_BUILD_NAME"'/tmp" DISABLE_ASLR=1 ninja -C WebKitBuild/'"$SUMMIT_ENGINE_BUILD_NAME"' -k 0 -j' "$SUMMIT_WEBKIT_JOBS" "$SUMMIT_ENGINE_TARGET"
elif [[ $SUMMIT_ENGINE_MODE == modern ]]; then
    bash "$SUMMIT_REMOTE_SHELL" 'export PKG_CONFIG_PATH=/boot/home/summit-deps/image-codecs-20261006/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}; cd /boot/home/summit-webkit && cmake -S . -B WebKitBuild/Modern -G Ninja -DPORT=Haiku -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="'"$SUMMIT_ENGINE_CXX_FLAGS"'" -DENABLE_WEBKIT=ON -DENABLE_WEBKIT_LEGACY=OFF -DENABLE_GPU_PROCESS=OFF -DENABLE_LAYOUT_TESTS=OFF -DICU_ROOT=/boot/home/summit-deps/icu78 -DCMAKE_INSTALL_PREFIX=/boot/home/summit-webkit-modern '"$SUMMIT_ENGINE_CMAKE_EXTRA"' && DISABLE_ASLR=1 ninja -C WebKitBuild/Modern -k 0 -j' "$SUMMIT_WEBKIT_JOBS" "$SUMMIT_ENGINE_TARGET"
else
    bash "$SUMMIT_REMOTE_SHELL" 'cd /boot/home/summit-webkit && cmake -S . -B WebKitBuild/Release -G Ninja -DPORT=Haiku -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="'"$SUMMIT_ENGINE_CXX_FLAGS"'" -DENABLE_WEBKIT=OFF -DENABLE_WEBKIT_LEGACY=ON -DENABLE_LAYOUT_TESTS=OFF -DCMAKE_INSTALL_PREFIX=/boot/home/summit-webkit-install && DISABLE_ASLR=1 ninja -C WebKitBuild/Release -j' "$SUMMIT_WEBKIT_JOBS" "$SUMMIT_ENGINE_TARGET"
fi
