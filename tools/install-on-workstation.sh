#!/usr/bin/env bash
# Install a built Summit bundle on the X399 workstation: the desktop launcher
# /boot/home/Desktop/Summit-current.sh points at it (the previous launcher is
# kept in /boot/home/summit/launcher-backups/, not on the Desktop). Programs that embed the engine
# (Aurora and its apps) use the summit_webkit package instead
# (tools/package-summit-webkit.sh).
#
#   bash tools/install-on-workstation.sh bundle-XXXXXXXX
#
# SUMMIT_MESA_PREFIX names the private Mesa the launcher runs with, a
# directory of /boot/home/summit-mesa. A new Mesa build is installed beside the
# old one, so that a browser that is running keeps the libraries it started
# with. "system" is the system's own Mesa in /boot/system/non-packaged, which
# the workstation's AMD WX 5100 needs (radeonsi on amdgpu_drm; the private zink
# Mesa needs a Vulkan driver, which only the NVIDIA card had). The default is
# "prefix-20261002" on NVIDIA where that tested build is installed, otherwise
# "system" where available, or "prefix". The AMD-only system Mesa silently
# falls back to llvmpipe on NVIDIA. SUMMIT_VULKAN_ICD can select a private ICD.
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
bundle=${1:?usage: install-on-workstation.sh bundle-NAME}
if [[ ! $bundle =~ ^bundle-[A-Za-z0-9_-]+$ ]]; then
    echo 'The bundle must be named bundle-XXXXXXXX.' >&2
    exit 2
fi
mesa=${SUMMIT_MESA_PREFIX:-}
if [[ -n $mesa && $mesa != system && ! $mesa =~ ^prefix[A-Za-z0-9_.-]*$ ]]; then
    echo 'SUMMIT_MESA_PREFIX must be "system" or name a prefix* directory of /boot/home/summit-mesa.' >&2
    exit 2
fi
vulkan_icd=${SUMMIT_VULKAN_ICD:-}
if [[ -n $vulkan_icd && ! $vulkan_icd =~ ^/[A-Za-z0-9_./-]+$ ]]; then
    echo 'SUMMIT_VULKAN_ICD must be an absolute manifest path.' >&2
    exit 2
fi
stamp=$(date +%Y%m%d-%H%M)
bash "$SUMMIT_ROOT/tools/ws.sh" "set -e
B=/boot/home/summit/build-modern-browser/$bundle
test -x \$B/Summit && test -x \$B/WebProcess && test -f \$B/lib/libWebKit.so.1
SYSTEM_EGL=/boot/system/non-packaged/add-ons/opengl/egl_vendor.d/50_mesa.json
mesa=$mesa
if test -z \"\$mesa\"; then
    if test -e /dev/graphics/nvidia0 && test -f /boot/home/summit-mesa/prefix-20261002/lib/libEGL_mesa.so.0; then
        mesa=prefix-20261002
    elif test -f \$SYSTEM_EGL; then mesa=system; else mesa=prefix; fi
fi
VULKAN_ICD=$vulkan_icd
if test -z \"\$VULKAN_ICD\" && test \$mesa != system && test -e /dev/graphics/nvidia0; then
    VULKAN_ICD=/boot/home/build/install/data/vulkan/icd.d/nouveau_icd.x86_64.json
fi
if test -n \"\$VULKAN_ICD\"; then test -f \"\$VULKAN_ICD\"; fi
if test \$mesa = system; then
    MESA_LIB=/boot/system/non-packaged/lib
    MESA_EGL=\$SYSTEM_EGL
else
    M=/boot/home/summit-mesa/\$mesa
    MESA_LIB=\$M/lib
    MESA_EGL=\$M/data/glvnd/egl_vendor.d/50_mesa.json
    grep -q \$M/lib/ \$MESA_EGL
fi
test -f \$MESA_LIB/libEGL_mesa.so.0 && test -f \$MESA_EGL
L=/boot/home/Desktop/Summit-current.sh
mkdir -p /boot/home/summit/launcher-backups
if test -f \$L; then cp \$L /boot/home/summit/launcher-backups/Summit-current.pre-$stamp.sh; fi
cat > \$L <<EOF
#!/bin/sh
set -eu
SUMMIT_BUNDLE=\$B
export WEBKIT_EXEC_PATH=\"\\\$SUMMIT_BUNDLE\"
export SUMMIT_SKIA_GL_CONTEXT=\\\${SUMMIT_SKIA_GL_CONTEXT:-1}
export SUMMIT_SCROLL_REFRESH_TIMER=\\\${SUMMIT_SCROLL_REFRESH_TIMER:-16}
export LIBRARY_PATH=\"\$MESA_LIB:\\\$SUMMIT_BUNDLE/lib:/boot/home/config/non-packaged/lib:/boot/home/config/lib:/boot/system/non-packaged/lib:/boot/system/lib\"
export __EGL_VENDOR_LIBRARY_FILENAMES=\$MESA_EGL
# One line a second while the pointer moves: how late pointer events arrive
# and gaps in the mouse's own reports (docs/performance.md, 29 September).
export SUMMIT_INPUT_LAG_TRACE=\\\${SUMMIT_INPUT_LAG_TRACE:-1}
# Message dispatches of the window and application threads over 50 ms.
export SUMMIT_UI_STALL_TRACE=\\\${SUMMIT_UI_STALL_TRACE:-1}
# The executable the system starts for web links when Summit is the default
# browser (Preferences): a link that follows each install.
export SUMMIT_SYSTEM_LAUNCHER=/boot/home/summit/Summit-installed
exec \"\\\$SUMMIT_BUNDLE/Summit\" \"\\\$@\" 2>>/boot/home/summit/summit-stderr.log
EOF
# Both launch routes need the private NVIDIA manifest. It can remain retired
# from the system-wide loader while Summit uses it explicitly.
if test -n \"\$VULKAN_ICD\"; then
    sed -i \"/exec .*SUMMIT_BUNDLE/i export VK_ICD_FILENAMES=\$VULKAN_ICD\" \$L
fi
chmod +x \$L
# Started by the system rather than the launcher (a web link while Summit is
# the default browser), Summit reads the launcher's environment from here.
cat > \$B/launch.env <<ENV
# Written by tools/install-on-workstation.sh; see ApplyLaunchEnvironment().
WEBKIT_EXEC_PATH=\$B
LIBRARY_PATH^=\$MESA_LIB
__EGL_VENDOR_LIBRARY_FILENAMES=\$MESA_EGL
SUMMIT_SKIA_GL_CONTEXT=1
SUMMIT_SCROLL_REFRESH_TIMER=16
ENV
if test -n \"\$VULKAN_ICD\"; then
    printf 'VK_ICD_FILENAMES=%s\\n' \"\$VULKAN_ICD\" >> \$B/launch.env
fi
ln -sfn \$B/Summit /boot/home/summit/Summit-installed
# libnetwork's nsswitch_conf_file_path() is not thread-safe; a real config file
# newer than the settings directory keeps a racy re-parse from opening the
# directory and exiting the network process (docs/kunanyios-platform-issues.md).
N=/boot/system/settings/network/nsswitch.conf
if ! test -f \$N; then
    printf '# Created by Summit (tools/install-on-workstation.sh): the same sources libnetwork\n# uses without a file. A file newer than /boot/system/settings stops a racy\n# re-parse in nsswitch_conf_file_path() from opening the directory instead;\n# see Summit docs/kunanyios-platform-issues.md.\nhosts: files dns\n' > \$N
    echo \"created \$N\"
fi
echo \"launcher -> \$B (Mesa: \$mesa)\"
cat \$L"
