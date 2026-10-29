#!/usr/bin/env bash
# Build the summit_webkitlegacy package (legacy/: the BWebView API of
# WebPositive on Summit's engine) on the X399 workstation, against the
# summit_webkit package installed there, and copy it to artifacts/.
#
#   bash tools/package-summit-webkitlegacy.sh [ENGINE_VERSION]
#
# ENGINE_VERSION is the summit_webkit version the package requires (default:
# the one installed). docs/legacy-webview.md says how it is installed.
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
version=${SUMMIT_WEBKITLEGACY_VERSION:-1.10.0}
revision=${SUMMIT_WEBKITLEGACY_REVISION:-1}
shell="$SUMMIT_ROOT/tools/ws.sh"
out=/boot/home/summit/tmp/summit-webkitlegacy-package
engine=${1:-$(bash "$shell" "pkgman list-installed 2>/dev/null | sed -n 's/^summit_webkit[[:space:]]*\([^[:space:]]*\).*/\1/p' | head -1")}
if [[ ! $engine =~ ^[0-9][0-9A-Za-z._~-]*$ ]]; then
    echo "summit_webkit is not installed on the workstation (give its version)." >&2
    exit 2
fi
bash "$shell" "rm -rf $out/source && mkdir -p $out/source"
# The web inspector is Summit's developer tools, built from src/ (with
# nlohmann/json from vendor/); building needs scintilla_devel and
# lexilla_devel on the workstation.
tar -C "$SUMMIT_ROOT" -cf - legacy/Makefile legacy/README.md legacy/headers legacy/src src/core src/ui vendor/nlohmann \
    | bash "$shell" "tar -C $out/source -xf -"
bash "$shell" "cat > $out/make-legacy-package.sh" < "$SUMMIT_ROOT/tools/summit-webkit-package/make-legacy-package.sh"
bash "$shell" "sh $out/make-legacy-package.sh $out/source/legacy /boot/system/develop/headers/summit-webkit \
    /boot/system/lib/summit-webkit/lib $out $version $revision $engine"
mkdir -p "$SUMMIT_ROOT/artifacts"
name=summit_webkitlegacy-$version-$revision-x86_64.hpkg
bash "$shell" "cat $out/$name" > "$SUMMIT_ROOT/artifacts/$name"
ls -la "$SUMMIT_ROOT/artifacts/$name"
