# Native CSS cursors

The modern engine sends cursor keywords or bounded bitmap pixels from its
application thread to `BWebKitView`. Haiku's `BCursor::Archive(BMessage*)` and
`BCursor(BMessage*)` are stubs; using them loses image and keyword cursors.

Image cursors use a native `B_RGBA32` bitmap with premultiplied BGRA pixels and the
CSS hot spot. WebKit's image scale (including `image-set()` and SVG rasterization)
is removed before creating the bitmap. The resulting dimensions are logical
pixels. air/OS app_server scales the bitmap and hot spot for the display, so the
browser must not multiply them by the display scale again.

app_server's cursor blend adds the source channels to the remaining background
contribution. Sending straight channels causes translucent pixels to overflow
and change color. The transport therefore retains premultiplied channels.

Both ends bound the bitmap to 128 × 128 pixels. The receiving view checks the
dimensions, byte length and finite hot spot before allocating a `BBitmap`.
Malformed messages fall back to the default cursor. WebCore still selects the
CSS fallback keyword for missing or oversized images. The view owns its current
`BCursor` and releases it when the cursor changes or the view is destroyed.

This transport belongs to the modern engine. The legacy WebKit custom-cursor
conversion remains separate from this path.

## Native verification

`tests/fixtures/cursors.html` generates matching PNG, SVG and 2x PNG images with
transparent, opaque and translucent regions. `tests/ModernCursorTests.cpp` loads
the fixture through the real engine, moves the native pointer, checks the
transported bitmap and hot spot, and reads the cursor painted by app_server.
It also checks out-of-image hot spots, oversized and invalid image fallbacks, and
default, crosshair, hand and invisible keyword cursors. Captures are saved as
PPM files. Run this on a test desktop at both 100% and 200% display scaling.

Serve the fixture directory with an HTTP server, then compile on Haiku using
headers matching the engine bundle:

```sh
c++ -std=c++23 -O1 -Wall -Wextra -Wno-multichar -DBUILDING_HAIKU__=1 \
  -I"$WEBKIT_HEADERS" -I"$WEBKIT_HEADERS/WebKit" tests/ModernCursorTests.cpp \
  -L"$SUMMIT_BUNDLE/lib" -lWebKit -lbe -lgame -o ModernCursorTests
mkdir -p captures
```

Use the bundle's normal graphics environment, `WEBKIT_EXEC_PATH` and
`LIBRARY_PATH`, and an isolated profile:

```sh
./ModernCursorTests http://HOST:8766/cursors.html captures /tmp/summit-cursor-profile
```

The test waits for its helper processes to exit and follows Summit's application
exit path to avoid racing WebKit worker teardown against libbe global destruction.
Check the native syslog interval for fresh debugger events as well as the exit
status. Excalidraw's live tool cursors are an additional manual acceptance check.

On 2026-10-21, all ten native cases passed on air/OS at 200% display scaling
with the rebuilt engine. The test also reproduced the old engine's PNG transport
failure. WebCore resolves an out-of-image CSS hot spot to `(0, 0)` before
transport. Verification at 100% and with Excalidraw remains pending; issue #86
remains open.
