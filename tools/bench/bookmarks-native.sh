#!/usr/bin/env bash
# Build the core and native bookmark tests with a Haiku compiler. No engine is
# linked or launched: unused modern toolbar code is removed from this probe.
set -euo pipefail
if [[ $# != 1 ]]; then
    echo 'Usage: bookmarks-native.sh OUTPUT_DIRECTORY' >&2
    exit 64
fi
SUMMIT_BOOKMARK_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
mkdir -p -- "$1"
SUMMIT_BOOKMARK_OUTPUT=$(cd -- "$1" && pwd)
cd "$SUMMIT_BOOKMARK_ROOT"
read -r -a SUMMIT_BOOKMARK_COMPILER <<< "${CXX:-g++}"
read -r -a SUMMIT_BOOKMARK_CPPFLAGS <<< "${CPPFLAGS:-}"
read -r -a SUMMIT_BOOKMARK_LDFLAGS <<< "${LDFLAGS:-}"
SUMMIT_BOOKMARK_FLAGS=(-std=c++23 -O1 -g -ffunction-sections -fdata-sections -fvisibility=hidden
    -Wno-multichar -DSUMMIT_MODERN_WEBKIT=1 -DBUILDING_HAIKU__=1 -Isrc -Ivendor
    "-I${WEBKIT_INCLUDE_DIR:-/boot/system/develop/headers}")
SUMMIT_BOOKMARK_SOURCES=()
for SUMMIT_BOOKMARK_UNIT in Bookmarks BookmarkIO BookmarkKeywords BookmarkQueries HistoryVisits Profile Address Favicon Zoom Protocol UserAgent Spelling TranslationModels PageLanguage InternalPages DownloadHistory DownloadText; do
    SUMMIT_BOOKMARK_SOURCES+=("src/core/$SUMMIT_BOOKMARK_UNIT.cpp")
done
for SUMMIT_BOOKMARK_UNIT in Chrome BookmarkImport BookmarkEditor SharedProfile DownloadList; do
    SUMMIT_BOOKMARK_SOURCES+=("src/ui/$SUMMIT_BOOKMARK_UNIT.cpp")
done
SUMMIT_BOOKMARK_OBJECTS=()
for SUMMIT_BOOKMARK_SOURCE in "${SUMMIT_BOOKMARK_SOURCES[@]}"; do
    SUMMIT_BOOKMARK_OBJECT="$SUMMIT_BOOKMARK_OUTPUT/$(basename "${SUMMIT_BOOKMARK_SOURCE%.cpp}").o"
    "${SUMMIT_BOOKMARK_COMPILER[@]}" "${SUMMIT_BOOKMARK_CPPFLAGS[@]}" "${SUMMIT_BOOKMARK_FLAGS[@]}" -c "$SUMMIT_BOOKMARK_SOURCE" -o "$SUMMIT_BOOKMARK_OBJECT"
    SUMMIT_BOOKMARK_OBJECTS+=("$SUMMIT_BOOKMARK_OBJECT")
done
for SUMMIT_BOOKMARK_TEST in BookmarkTests BookmarkQueryTests BookmarkFolderQueryTests BookmarkTimeQueryTests BookmarkModifiedQueryTests BookmarkCombinedQueryTests BookmarkSiteQueryTests BookmarkDateQueryTests BookmarkVisitQueryTests BookmarkRootQueryTests BookmarkTransitionQueryTests BookmarkSidebarQueryTests DownloadVisitTests NativeDownloadHistoryTests NativeBookmarkTests; do
    "${SUMMIT_BOOKMARK_COMPILER[@]}" "${SUMMIT_BOOKMARK_CPPFLAGS[@]}" "${SUMMIT_BOOKMARK_FLAGS[@]}" \
        "tests/$SUMMIT_BOOKMARK_TEST.cpp" "${SUMMIT_BOOKMARK_OBJECTS[@]}" "${SUMMIT_BOOKMARK_LDFLAGS[@]}" \
        -Wl,--gc-sections -lbe -ltracker -ltranslation -lcrypto -lnetwork -o "$SUMMIT_BOOKMARK_OUTPUT/$SUMMIT_BOOKMARK_TEST"
done
echo "Built core bookmark/query tests, DownloadVisitTests, NativeDownloadHistoryTests and NativeBookmarkTests in $SUMMIT_BOOKMARK_OUTPUT"
echo "Run BookmarkSiteQueryTests with tests/fixtures/bookmarks/site-query-firefox158.json as its argument."
echo "Run BookmarkDateQueryTests with tests/fixtures/bookmarks/date-query-firefox158.json as its argument."
echo "Run BookmarkVisitQueryTests with tests/fixtures/bookmarks/visit-query-firefox158.json as its argument."
echo "Run BookmarkRootQueryTests with tests/fixtures/bookmarks/root-query-firefox158.json as its argument."
echo "Run BookmarkTransitionQueryTests with tests/fixtures/bookmarks/transition-query-firefox158.json as its argument."
echo "Run BookmarkSidebarQueryTests with tests/fixtures/bookmarks/sidebar-query-firefox158.json as its argument."
