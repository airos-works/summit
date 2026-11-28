#!/usr/bin/env python3
"""Copies Summit's Android engine into the app's wpeview module.

    tools/android/stage-engine.py <arm64|x86_64> [--sysroot DIR]

The sysroot is the cerbero prefix that tools/android/build-deps.sh fills and
tools/android/build-engine.sh installs the engine into (default
$SUMMIT_ANDROID_ROOT/cerbero-home/dist/android_<arch>). It may also be an
unpacked WPE Android prebuilt package.

Unlike WPE Android's bootstrap, which ships every library cerbero built, this
copies only what the engine loads: the NEEDED closure of the engine, the
injected bundle, the GIO TLS module and the GStreamer plugins listed below. The
staged files (not in git) are:

  android/wpeview/src/main/cpp/imported/   headers and link-time libraries
  android/wpeview/src/main/jniLibs/<abi>/  runtime libraries
  android/wpeview/src/main/assets/{gstreamer-1.0,gio,injected-bundles}/<abi>/
  android/wpeview/src/main/java/org/freedesktop/gstreamer/  GStreamer's Java glue

Android only unpacks lib*.so files, so versioned sonames (libfoo.so.1) are
renamed to equal-length names (libfoo_1.so) inside every staged ELF file.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
WPEVIEW_MAIN = REPO / "android" / "wpeview" / "src" / "main"

ABIS = {"arm64": "arm64-v8a", "x86_64": "x86_64"}
CERBERO_DIST = {"arm64": "android_arm64", "x86_64": "android_x86_64"}

# Libraries the wpeview native code links against at build time; they stay in
# cpp/imported (the Gradle CMake build packages them from there).
BUILD_LIBS = ["libgio-2.0.so", "libglib-2.0.so", "libgmodule-2.0.so", "libgobject-2.0.so", "libWPEWebKit-2.0.so"]
BUILD_INCLUDES = [
    ("glib-2.0", "glib-2.0"),
    ("gstreamer-1.0", "gstreamer-1.0"),
    ("libsoup-3.0", "libsoup-3.0"),
    ("wpe-webkit-2.0", "wpe-webkit"),
    ("xkbcommon", "xkbcommon"),
    # Summit's core (libsummit) uses the engine's OpenSSL.
    ("openssl", "openssl"),
    ("curl", "curl"),
]

# GStreamer plugins Summit ships. Video and audio decoding goes to the
# device's hardware codecs through androidmedia (MediaCodec); dav1d, vpx,
# opus and vorbis are software fallbacks for streams the device cannot decode.
GST_PLUGINS = [
    # core and playback
    "coreelements", "app", "playback", "typefindfunctions", "autodetect",
    "audioconvert", "audioresample", "audiorate", "volume", "videoconvertscale", "videorate",
    "audiomixer", "interleave", "deinterlace", "rawparse", "subparse",
    # containers and parsers
    "isomp4", "matroska", "ogg", "audioparsers", "videoparsersbad", "id3demux", "wavparse",
    "mpegtsdemux", "adaptivedemux2",
    # hardware codecs, GL and audio output
    "androidmedia", "opengl", "opensles",
    # software fallbacks
    "dav1d", "vpx", "opus", "vorbis", "libav",
    # WebRTC and capture
    "webrtc", "nice", "dtls", "srtp", "rtp", "rtpmanager", "sctp", "openh264", "inter",
]

# Libraries that are dlopen'ed rather than NEEDED, and libcurl, which the app's
# libsummit links for the extension stores (core/ExtensionStoreDownload).
EXTRA_ROOTS = ["libWPEWebDriver.so", "libcurl.so"]


def read_elf(path):
    out = subprocess.run(["readelf", "-d", str(path)], capture_output=True, text=True,
                         env=dict(os.environ, LC_ALL="C")).stdout
    soname = None
    needed = []
    for line in out.splitlines():
        m = re.match(r"^ 0x[0-9a-f]+ \(NEEDED\)\s+Shared library: \[(.+)\]$", line)
        if m:
            needed.append(m.group(1))
        m = re.match(r"^ 0x[0-9a-f]+ \(SONAME\)\s+Library soname: \[(.+)\]$", line)
        if m:
            soname = m.group(1)
    return soname or path.name, needed


def adjusted_soname(soname):
    if soname.endswith(".so"):
        return soname
    parts = soname.split(".")
    if len(parts) > 2 and parts[-2] == "so":
        return ".".join(parts[:-2]) + "_" + parts[-1] + ".so"
    if len(parts) > 3 and parts[-3] == "so":
        return ".".join(parts[:-3]) + "_" + parts[-2] + "_" + parts[-1] + ".so"
    raise SystemExit(f"cannot adjust soname {soname}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("arch", choices=sorted(ABIS))
    parser.add_argument("--sysroot", type=Path)
    args = parser.parse_args()

    root = Path(os.environ.get("SUMMIT_ANDROID_ROOT", "/data3/airos/summit-android"))
    sysroot = args.sysroot or root / "cerbero-home" / "dist" / CERBERO_DIST[args.arch]
    lib = sysroot / "lib"
    abi = ABIS[args.arch]
    if not (lib / "libWPEWebKit-2.0.so").exists():
        sys.exit(f"no engine in {lib}: run tools/android/build-engine.sh {args.arch}")

    # Index every shared library in the sysroot by soname.
    by_soname = {}
    for path in lib.glob("*.so*"):
        if path.is_symlink() or not path.is_file() or path.suffix == ".a":
            continue
        try:
            soname, needed = read_elf(path)
        except Exception:
            continue
        by_soname.setdefault(soname, (path, needed))

    gst_dir = lib / "gstreamer-1.0"
    plugin_paths = []
    for name in GST_PLUGINS:
        p = gst_dir / f"libgst{name}.so"
        if p.exists():
            plugin_paths.append(p)
        else:
            print(f"warning: GStreamer plugin {name} not built", file=sys.stderr)
    gio_module = lib / "gio" / "modules" / "libgioopenssl.so"
    bundle = lib / "wpe-webkit-2.0" / "injected-bundle" / "libWPEInjectedBundle.so"

    # NEEDED closure from the roots.
    wanted = set()
    missing = set()
    queue = [read_elf(p)[1] for p in plugin_paths + [gio_module, bundle] if p.exists()]
    queue = [n for needed in queue for n in needed] + ["libWPEWebKit-2.0.so", "libc++_shared.so"] + \
        [r for r in EXTRA_ROOTS if r in by_soname]
    while queue:
        soname = queue.pop()
        if soname in wanted:
            continue
        if soname not in by_soname:
            missing.add(soname)
            continue
        wanted.add(soname)
        queue.extend(by_soname[soname][1])

    replacements = [("libnettle.so.8", "libnettle_8.so")]
    for soname in wanted:
        adj = adjusted_soname(soname)
        if adj != soname:
            replacements.append((soname, adj))
    for old, new in replacements:
        assert len(old) == len(new), (old, new)

    # Debug information stays in the sysroot (for symbolizing crash reports);
    # the staged copies are stripped, as the APK's would be anyway.
    ndk = Path(os.environ.get("ANDROID_NDK_HOME", root / "cerbero-home" / "android-ndk-29"))
    strip = next(iter(ndk.glob("toolchains/llvm/prebuilt/*/bin/llvm-strip")), None)

    def copy_patched(src, dst):
        dst.parent.mkdir(parents=True, exist_ok=True)
        data = src.read_bytes()
        for old, new in replacements:
            data = data.replace(old.encode(), new.encode())
        dst.write_bytes(data)
        if strip:
            subprocess.run([str(strip), "--strip-debug", str(dst)], check=True)

    imported = WPEVIEW_MAIN / "cpp" / "imported"
    imported_lib = imported / "lib" / abi
    jni = WPEVIEW_MAIN / "jniLibs" / abi
    for d in (imported_lib, jni):
        shutil.rmtree(d, ignore_errors=True)
        d.mkdir(parents=True)

    # Every library goes to imported/ (wpeview's CMake links some of them);
    # all but BUILD_LIBS also go to jniLibs. Gradle packages BUILD_LIBS from
    # the CMake IMPORTED targets, and a second copy would clash.
    total = 0
    for soname in sorted(wanted):
        src = by_soname[soname][0]
        name = adjusted_soname(soname)
        copy_patched(src, imported_lib / name)
        total += src.stat().st_size
        # Gradle packages the NDK's libc++_shared.so (the newer one, which
        # runs code built against the older one cerbero's NDK has).
        if name not in BUILD_LIBS and name != "libc++_shared.so":
            copy_patched(src, jni / name)
    shutil.rmtree(imported_lib / "glib-2.0", ignore_errors=True)
    shutil.copytree(lib / "glib-2.0", imported_lib / "glib-2.0")

    inc = imported / "include"
    shutil.rmtree(inc, ignore_errors=True)
    inc.mkdir(parents=True)
    for src_name, dst_name in BUILD_INCLUDES:
        shutil.copytree(sysroot / "include" / src_name, inc / dst_name)

    def stage_assets(kind, files):
        d = WPEVIEW_MAIN / "assets" / kind / abi
        shutil.rmtree(d, ignore_errors=True)
        d.mkdir(parents=True)
        for f in files:
            if f.suffix == ".so":
                copy_patched(f, d / f.name)
            else:
                shutil.copy(f, d / f.name)
        return d

    stage_assets("gstreamer-1.0", plugin_paths)
    stage_assets("gio", [gio_module])
    stage_assets("injected-bundles", [bundle, sysroot / "share" / "wpe-webkit-2.0" / "inspector.gresource"])

    gst_java = WPEVIEW_MAIN / "java" / "org" / "freedesktop" / "gstreamer"
    shutil.rmtree(gst_java, ignore_errors=True)
    gst_java.mkdir(parents=True)
    ndk_build = sysroot / "share" / "gst-android" / "ndk-build"
    shutil.copy(ndk_build / "GStreamer.java", gst_java)
    shutil.copytree(ndk_build / "androidmedia", gst_java / "androidmedia")

    # Android provides these.
    system = {"libandroid.so", "libc.so", "libdl.so", "libEGL.so", "libGLESv2.so", "libGLESv3.so", "liblog.so",
              "libm.so", "libmediandk.so", "libOpenSLES.so", "libvulkan.so", "libz.so", "libnativewindow.so",
              "libsync.so", "libaaudio.so", "libjnigraphics.so", "libcamera2ndk.so", "libbinder_ndk.so",
              "libc++_shared.so"}
    unresolved = sorted(missing - system)
    print(f"staged {len(wanted)} libraries ({total / 1e6:.1f} MB), {len(plugin_paths)} GStreamer plugins")
    if unresolved:
        print("NEEDED but not found:", " ".join(unresolved), file=sys.stderr)


if __name__ == "__main__":
    main()
