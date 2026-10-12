# WebGL

Added 30 September 2026 for [issue #3](https://github.com/jmgasper/summit/issues/3).
Summit had no WebGL: the Haiku port built WebKit with `ENABLE_WEBGL=OFF`, so
`canvas.getContext("webgl")` returned null and WebGL pages (the target was
[WebGL Aquarium](https://webglsamples.org/aquarium/aquarium.html)) showed
their "no WebGL" message.

## How it works

WebGL now runs as it does in the GTK and WPE ports, on the GPU stack Summit
already composites with:

```
page (WebGL 1/2)
  -> WebCore GraphicsContextGLANGLE (validation, the WebGL API)
  -> ANGLE, GL back-end on the system EGL  (Source/ThirdParty/ANGLE)
  -> libEGL.so.1 = the private Mesa 25.3 (glvnd, zink)
  -> Vulkan -> NVK -> the GeForce
```

- **Build.** `OptionsHaiku.cmake` enables WebGL by default whenever GL
  compositing is built (`USE_HAIKU_GL_COMPOSITING` with Skia, the
  coordinated layer tree) and sets `USE_ANGLE`. ANGLE's
  `PlatformHaiku.cmake` compiles its OpenGL back-end on EGL (CMake counts
  Haiku as UNIX, so ANGLE takes its Linux sources; `system_utils_linux.cpp`
  handles Haiku's missing `/proc` and `CLOCK_MONOTONIC_RAW`). No X11, Wayland
  or GBM.
- **Display.** `PlatformDisplayHaiku` gives ANGLE the native platform 0 and
  `EGL_DEFAULT_DISPLAY`, so ANGLE's EGL back-end opens the same display as the
  compositor with `eglGetDisplay`, not `eglGetPlatformDisplay` (which hangs in
  zink with a surfaceless platform, see [workstation.md](workstation.md)).
- **Frames.** Without GBM there is no buffer to share between processes, and
  none is needed: WebGL runs in the web process (Summit has no GPU process),
  and `GraphicsContextGLTextureMapperANGLE` renders into GL textures of a
  context shared with the compositor's. Each frame is handed to the
  coordinated compositor as a texture with a fence.
- **Headers.** Haiku's libepoxy declares the Khronos types itself, which
  collided with ANGLE's headers; `GLContext.h` now declares the native window
  type without EGL headers when ANGLE's came first (only WebGL's own
  translation units).
- **Hardware only.** WebGL exists where Summit composites with a GPU (the
  workstation's launcher sets the private Mesa up). Without an EGL display the
  web process makes no WebGL context and `getContext()` returns null, as the
  page expects of a browser without WebGL; nothing asserts.

## Checking it

`tools/bench/pages/webgl.html` makes WebGL 1 and 2 contexts, prints what
renders them, draws a triangle and reads its pixels back, and times an
animated scene of 400 lit cubes:

```sh
SUMMIT_BENCH_HOST=workstation python3 tools/bench/run-probe.py \
    --bundle /boot/home/summit/build-modern-browser/bundle-XXXX --page webgl.html \
    --env SUMMIT_LIBRARY_PATH_PREFIX=/boot/home/summit-mesa/prefix-20260930/lib \
    --env __EGL_VENDOR_LIBRARY_FILENAMES=/boot/home/summit-mesa/prefix-20260930/data/glvnd/egl_vendor.d/50_mesa.json
```

## Verified (30 September–1 October 2026, X399, GTX 1070)

`webgl.html` on the new engine: WebGL 1 ("WebGL 1.0", GLSL ES 1.0) and
WebGL 2 ("WebGL 2.0", GLSL ES 3.00) contexts, 33 and 30 extensions, maximum
texture size 16384; the triangle read back red inside and blue outside for
both; the scene of 400 lit cubes drawn one call each ran at 56 frames/s
(median frame 17 ms). The page showed all three canvases composited in the
window. The renderer strings are WebKit's privacy defaults ("WebKit WebGL",
unmasked "Apple GPU"), as in Safari. Before, `getContext("webgl")` returned
null.

Two things were found on the way. ANGLE's `Display.cpp` created an EGL
display for OpenGL ES only under `ANGLE_PLATFORM_LINUX` (the port had
patched the desktop-GL case alone) and did not advertise the EGL device type
WebKit's display attributes use, so every context failed with "Could not
create a WebGL context". Failures to make ANGLE's display or context are now
logged to standard error (`WebGL: …`), since release builds have no
WebGL log channel.

[WebGL Aquarium](https://webglsamples.org/aquarium/aquarium.html), the page
the issue names, in a 1916x1052 window at 200%: 52 frames/s with 500 fish
and 28 with 5,000 (its own counter, read without screenshots, which pause
drawing on this machine). Firefox 155 on the same machine has no WebGL:
`getContext("webgl")` returns null there and the page shows nothing.

## No context after 4f2a0a6, fixed in ce257e0 (13 October 2026, WX 5100)

4f2a0a6 makes WebKit's compositor contexts on Haiku lose their context on a
GPU reset wherever `EGL_EXT_create_context_robustness` is offered, and
ANGLE's native context shares with them. ANGLE asked for the same with EGL
1.5's `EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY`, which Mesa accepts
for an OpenGL ES context only from an EGL 1.5 display, and Haiku's EGL
driver is 1.4 (`EGL_LOG_LEVEL=debug`: "bad context attribute 0x31bd"); its
fallback without robustness cannot share with a robust context ("share list
notification strategy mismatch"), so every page got "WebGL: ANGLE made no
context (EGL error 0x3001)". `DisplayEGL::initializeContext` now uses the
extension's `EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT` below EGL
1.5.

On the X399's WX 5100 (radeonsi, the system Mesa) with `bundle-ci1631`:
`webgl.html` draws its triangle correctly with WebGL 1 and 2 and runs the
400 cubes at 57.6 frames/s (median frame 17 ms); WebGL Aquarium shows 60
frames/s in a 1024x1024 canvas; `platform.html` reports `webgl=Apple_GPU
webglDraw=ok`.

