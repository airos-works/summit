#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
make -j6
SUMMIT_STAGE=$(mktemp -d /tmp/summit-package-XXXXXX)
trap 'rm -rf -- "$SUMMIT_STAGE"' EXIT
mkdir -p "$SUMMIT_STAGE/apps" "$SUMMIT_STAGE/data/Summit" \
    "$SUMMIT_STAGE/data/deskbar/menu/Applications" \
    "$SUMMIT_STAGE/documentation/packages/summit/vendor/nlohmann" "$SUMMIT_ROOT/artifacts"
cp build-haiku/Summit "$SUMMIT_STAGE/apps/Summit"
strip --strip-debug "$SUMMIT_STAGE/apps/Summit"
xres -o "$SUMMIT_STAGE/apps/Summit" build-haiku/Summit.rsrc
cp resources/start.html "$SUMMIT_STAGE/data/Summit/start.html"
cp resources/Summit.PackageInfo "$SUMMIT_STAGE/.PackageInfo"
cp README.md LICENSE "$SUMMIT_STAGE/documentation/packages/summit/"
cp -R docs "$SUMMIT_STAGE/documentation/packages/summit/"
mkdir -p "$SUMMIT_STAGE/documentation/packages/summit/vendor/fontawesome"
cp resources/artwork/fontawesome/LICENSE.txt resources/artwork/fontawesome/README.md "$SUMMIT_STAGE/documentation/packages/summit/vendor/fontawesome/"
cp vendor/nlohmann/LICENSE.MIT vendor/nlohmann/UPSTREAM.md "$SUMMIT_STAGE/documentation/packages/summit/vendor/nlohmann/"
mkdir -p "$SUMMIT_STAGE/documentation/packages/summit/vendor/cld2"
cp vendor/cld2/LICENSE vendor/cld2/README.md "$SUMMIT_STAGE/documentation/packages/summit/vendor/cld2/"
ln -s ../../../../apps/Summit "$SUMMIT_STAGE/data/deskbar/menu/Applications/Summit"
package create -C "$SUMMIT_STAGE" "$SUMMIT_ROOT/artifacts/summit-0.1.0~dev1-1-x86_64.hpkg"
