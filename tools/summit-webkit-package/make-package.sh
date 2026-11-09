#!/bin/sh
# Runs on Haiku: builds the summit_webkit package from a Summit bundle.
#
#   make-package.sh BUNDLE MESA_PREFIX WEBKIT_SOURCE OUTPUT_DIR VERSION
#
# The package installs the engine for programs other than Summit (Aurora and
# the Electron apps built on it) under /boot/system/lib/summit-webkit:
#   WebProcess, NetworkProcess     the engine's processes
#   lib/                           libWebKit, libJavaScriptCore, ICU, libzip
#   mesa/lib, mesa/egl_vendor.d    a Mesa with EGL (stock Haiku's has none);
#                                  the process launcher points the engine's
#                                  processes at it unless the program chose
#                                  a glvnd vendor itself
# and the public headers under develop/headers/summit-webkit/WebKit.
set -eu
B=$1; M=$2; SRC=$3; OUT=$4; VERSION=$5; REVISION=${6:-1}
case $OUT in /SummitExtensions/?*|/boot/home/?*) ;; *) echo "OUTPUT_DIR must be under /SummitExtensions or /boot/home" >&2; exit 2;; esac
test -x "$B/WebProcess" && test -x "$B/NetworkProcess" && test -f "$B/lib/libWebKit.so.1"
test -f "$M/lib/libEGL_mesa.so.0"
S=$OUT/stage
rm -rf "$S"
E=$S/lib/summit-webkit
mkdir -p "$E/lib" "$E/mesa/lib" "$E/mesa/egl_vendor.d" "$S/develop/headers/summit-webkit/WebKit" \
    "$S/documentation/packages/summit_webkit"
cp -a "$B/lib/." "$E/lib/"
cp -a "$B/WebProcess" "$B/NetworkProcess" "$E/"
for l in libEGL libEGL_mesa libGLESv2 libGLdispatch libGL libOpenGL; do
    cp -a "$M/lib/$l".so* "$E/mesa/lib/"
done
cat > "$E/mesa/egl_vendor.d/50_mesa.json" <<EOF
{
    "file_format_version" : "1.0.0",
    "ICD" : {
        "library_path" : "/boot/system/lib/summit-webkit/mesa/lib/libEGL_mesa.so.0"
    }
}
EOF
for h in UIProcess/API/haiku/WebKitView.h UIProcess/API/haiku/WebKitContext.h UIProcess/API/haiku/WebKitEmbedding.h \
    UIProcess/API/haiku/WebKitExtensionPermission.h UIProcess/API/haiku/WebKitInfo.h Shared/API/c/WKBase.h \
    Shared/API/c/WKDeclarationSpecifiers.h Shared/API/c/haiku/WKBaseHaiku.h; do
    cp "$SRC/Source/WebKit/$h" "$S/develop/headers/summit-webkit/WebKit/"
done
if test -d "$B/licenses"; then cp -a "$B/licenses" "$S/documentation/packages/summit_webkit/"; fi
cp "$B/build-manifest.json" "$S/documentation/packages/summit_webkit/"

# Requirements: every library the package's programs and libraries need that
# the package does not carry itself. Haiku's own libraries come with haiku.
own=$(cd "$E" && find . -name '*.so*' | sed 's|.*/||')
needed=$(find "$E" -type f \( -name '*.so*' -o -name WebProcess -o -name NetworkProcess \) -exec readelf -d {} \; 2>/dev/null \
    | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | sort -u)
requires=""
for n in $needed; do
    if echo "$own" | grep -qx "$n"; then continue; fi
    case $n in
    libroot.so|libbe.so|libnetwork.so|libbnetapi.so|libtranslation.so|libmedia.so|libgame.so|libdevice.so|libtracker.so|libtextencoding.so|libpackage.so|libbsd.so|libgnu.so|libshared.so)
        continue;;
    # air/OS's Neural Kit, which only lib/libsummitspeech.so links (speech
    # recognition, docs/speech-recognition.md); the engine opens that library
    # when a page asks for it and does without it where the kit is missing.
    libneural.so)
        continue;;
    esac
    # HaikuPorts names libraries in lower case with underscores (lib:libllvm,
    # lib:libharfbuzz_icu). LLVM's soname carries its major version.
    name=$(echo "$n" | sed 's/\.so.*//; s/-/_/g' | tr 'A-Z' 'a-z')
    case $n in libLLVM.so.*) name="$name >= $(echo "$n" | sed 's/^libLLVM\.so\.\([0-9]*\).*/\1/')";; esac
    requires="$requires	lib:$name
"
done
cat > "$S/.PackageInfo" <<EOF
name			summit_webkit
version			$VERSION-$REVISION
architecture	x86_64
summary			"The WebKit engine of the Summit browser, for other programs"
description		"The Summit browser's WebKit build (libWebKit with its web and network processes, JavaScriptCore and ICU) and a Mesa with EGL, installed in /boot/system/lib/summit-webkit. Programs link lib/libWebKit.so.1 from there and include develop/headers/summit-webkit."
packager		"Summit project"
vendor			"Summit project"
copyrights {
	"Apple Inc. and the WebKit contributors"
	"The Mesa and libglvnd contributors"
	"Summit contributors"
}
licenses {
	"GNU LGPL v2"
	"GNU LGPL v2.1"
	"BSD (2-clause)"
	"BSD (3-clause)"
	"MIT"
}
provides {
	summit_webkit = $VERSION
}
requires {
	haiku
$requires}
EOF
cat "$S/.PackageInfo"
# Say which requirements nothing installed here provides (they may still be
# in the repositories).
provided=$(for p in /boot/system/packages/*.hpkg; do package list -i "$p" 2>/dev/null; done | sed -n 's/^\s*provides:\s*\([^ ]*\).*/\1/p' | sort -u)
echo "$requires" | sed 's/^\t//; s/ .*//' | while read -r r; do
    test -z "$r" || echo "$provided" | grep -qx "$r" || echo "not installed here: $r"
done
mkdir -p "$OUT"
rm -f "$OUT/summit_webkit-$VERSION-$REVISION-x86_64.hpkg"
package create -C "$S" "$OUT/summit_webkit-$VERSION-$REVISION-x86_64.hpkg"
rm -rf "$S"
ls -la "$OUT/summit_webkit-$VERSION-$REVISION-x86_64.hpkg"
