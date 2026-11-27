# Shared settings for Summit's Android build scripts. Source it; every value
# can be overridden from the environment.
#
# SUMMIT_ANDROID_ROOT holds everything large: the cerbero checkout and its
# home (toolchain, dependency prefixes), engine build directories.
: "${SUMMIT_ANDROID_ROOT:=/data3/airos/summit-android}"
: "${SUMMIT_CERBERO_DIR:=$SUMMIT_ANDROID_ROOT/cerbero}"
: "${SUMMIT_CERBERO_CONFIG:=$SUMMIT_ANDROID_ROOT/summit.cbc}"
: "${SUMMIT_ANDROID_JOBS:=24}"
SUMMIT_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
: "${SUMMIT_ENGINE_SOURCE:=$SUMMIT_REPO/.cache/WebKit}"
: "${SUMMIT_CERBERO_HOME:=$SUMMIT_ANDROID_ROOT/cerbero-home}"

# summit_android_arch <arch>: sets CERBERO_ARCH (cerbero config suffix),
# ANDROID_ABI and SUMMIT_DIST (the dependency prefix cerbero installs into).
summit_android_arch() {
    case "$1" in
        arm64|aarch64|arm64-v8a) SUMMIT_ARCH=arm64; CERBERO_ARCH=arm64; ANDROID_ABI=arm64-v8a; CERBERO_DIST=android_arm64 ;;
        x86_64|x86-64) SUMMIT_ARCH=x86_64; CERBERO_ARCH=x86-64; ANDROID_ABI=x86_64; CERBERO_DIST=android_x86_64 ;;
        *) echo "unknown Android architecture: $1 (arm64 or x86_64)" >&2; return 1 ;;
    esac
    SUMMIT_DIST="$SUMMIT_CERBERO_HOME/dist/$CERBERO_DIST"
    SUMMIT_ENGINE_BUILD="$SUMMIT_ANDROID_ROOT/build/engine-$SUMMIT_ARCH"
}

summit_cerbero() {
    "$SUMMIT_CERBERO_DIR/cerbero-uninstalled" -c "$SUMMIT_CERBERO_DIR/config/cross-android-$CERBERO_ARCH.cbc" \
        -c "$SUMMIT_CERBERO_CONFIG" "$@"
}
