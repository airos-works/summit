#!/bin/sh
# Runs on Haiku: builds the summit_webkitlegacy package, the legacy BWebView
# API (libWebKitLegacy, WebPositive's) on Summit's engine, from legacy/
# (LEGACY_SOURCE, next to Summit's src/ and vendor/, whose developer tools
# are its web inspector).
#
#   make-legacy-package.sh LEGACY_SOURCE ENGINE_HEADERS ENGINE_LIB OUTPUT_DIR VERSION REVISION ENGINE_REQUIREMENT
#
# The package takes the place of haikuwebkit and haikuwebkit_devel: it
# provides them (WebPositive requires haikuwebkit), holds the same library
# and header paths, and replaces and conflicts with both. It needs the engine
# from summit_webkit (ENGINE_REQUIREMENT, e.g. "1.10.0-4").
set -eu
SRC=$1; HEADERS=$2; LIB=$3; OUT=$4; VERSION=$5; REVISION=$6; ENGINE=$7
case $OUT in /SummitExtensions/?*|/boot/home/?*) ;; *) echo "OUTPUT_DIR must be under /SummitExtensions or /boot/home" >&2; exit 2;; esac
BUILD=$OUT/legacy-build
rm -rf "$BUILD"
make -C "$SRC" OUT="$BUILD" ENGINE_HEADERS="$HEADERS" ENGINE_LIB="$LIB" VERSION="$VERSION"
S=$OUT/legacy-stage
rm -rf "$S"
mkdir -p "$S/lib" "$S/develop/lib" "$S/develop/headers" "$S/documentation/packages/summit_webkitlegacy"
cp "$BUILD/libWebKitLegacy.so.$VERSION" "$S/lib/"
ln -s "libWebKitLegacy.so.$VERSION" "$S/lib/libWebKitLegacy.so.1"
ln -s "../../lib/libWebKitLegacy.so.1" "$S/develop/lib/libWebKitLegacy.so"
cp "$SRC"/headers/*.h "$S/develop/headers/"
cp "$SRC/README.md" "$S/documentation/packages/summit_webkitlegacy/"
cat > "$S/.PackageInfo" <<EOF
name			summit_webkitlegacy
version			$VERSION-$REVISION
architecture	x86_64
summary			"The BWebView API of WebPositive on the Summit browser's engine"
description		"libWebKitLegacy with the classes of HaikuWebKit's API (BWebView, BWebPage, BWebWindow, BWebDownload, BWebSettings, BWebFrame) running on the Summit browser's WebKit engine (summit_webkit). Programs built for HaikuWebKit, such as WebPositive, run on it unchanged. Replaces haikuwebkit and haikuwebkit_devel."
packager		"Summit project"
vendor			"Summit project"
copyrights {
	"2026 Summit contributors"
	"2007-2010 Andrea Anzani, Ryan Leavengood, Maxime Simon, Stephan Aßmus (BWebWindow, headers)"
	"2012 Alexandre Deckner (WebKitInfo.h)"
}
licenses {
	"MIT"
	"BSD (2-clause)"
}
provides {
	summit_webkitlegacy = $VERSION
	haikuwebkit = $VERSION
	haikuwebkit_devel = $VERSION
	lib:libWebKitLegacy = $VERSION compat >= 1
	devel:libWebKitLegacy = $VERSION compat >= 1
}
requires {
	haiku
	summit_webkit >= $ENGINE
	lib:libscintilla
	lib:liblexilla
}
conflicts {
	haikuwebkit
	haikuwebkit_devel
}
replaces {
	haikuwebkit
	haikuwebkit_devel
}
EOF
mkdir -p "$OUT"
rm -f "$OUT/summit_webkitlegacy-$VERSION-$REVISION-x86_64.hpkg"
package create -C "$S" "$OUT/summit_webkitlegacy-$VERSION-$REVISION-x86_64.hpkg"
rm -rf "$S"
ls -la "$OUT/summit_webkitlegacy-$VERSION-$REVISION-x86_64.hpkg"
