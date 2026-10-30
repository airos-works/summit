# GPU compositing for the modern Haiku port

Status: **built and running** in the `ModernGL` configuration (September 19,
2026). In the QEMU VM, with the private Mesa 25.3.6 (llvmpipe) and
`SUMMIT_FORCE_GL_COMPOSITING=1`, the WebProcess reports
`Summit GL compositing: enabled (forced) [llvmpipe (LLVM 21.1.8, 256 bits),
OpenGL ES 3.1 Mesa 25.3.6, BGRA readback]` and renders
`tests/fixtures/gpu/compositing.html` like the software path: correct
orientation and channel order, 50% green over white measured (127, 207, 127),
shadows, rotated 3D layers, a compositor-driven CSS animation and a fixed
element ([GL](screenshots/gl-compositing-llvmpipe.png),
[software](screenshots/gl-compositing-software.png)). The one visible
difference, too-heavy text in transparent composited tiles, came from glyph
drawing with `B_OP_OVER` and is fixed in the following build. Not yet verified:
a real GPU (Mali/Panfrost on KunanyiOS), resizing, multiple tabs, the runtime
fallback, and performance. See [gpu-validation.md](gpu-validation.md).
Software rendering remains the default, and the `Modern` configuration is built
without GL. Delta history: `.cache/gpu-compositing.patch`; the code is now part
of `engine/patches/0001-haiku-port.patch`.

## Architecture

The WebProcess composites with upstream TextureMapper (OpenGL ES 2 through EGL)
into an offscreen texture, reads the frame back with `glReadPixels()` into the
same `ShareableBitmap` the software path fills, and sends it with the same
`UpdateBitmapHaiku` message. KunanyiOS has no cross-process GPU buffers (Mesa
presents by CPU copy into a `BBitmap`), so readback is the honest cost of this
design; it is what WebKitGTK/WPE do without DMABuf.

    RenderLayerCompositor -> GraphicsLayerTextureMapper tree   (upstream)
      tiles painted by GraphicsContextHaiku via ImageBuffer     (existing port)
      -> BitmapTexture upload (new Haiku branch, premultiplies)
    LayerTreeHostHaiku::renderFrame()                           (new)
      TextureMapper paints into a BitmapTexture FBO -> glReadPixels -> bitmap
    DrawingAreaHaiku::display() -> UpdateBitmapHaiku            (unchanged IPC)

`DrawingAreaHaiku` keeps owning frame pacing (one frame in flight, acknowledged
by `DidPresentBitmapHaiku`) and *pulls* a frame from the host. The UI process is
unchanged and cannot tell the paths apart, so falling back needs no protocol.
Everything runs on the main thread with one process-wide, unshared GL context.

What it buys: transforms, opacity, filters and CSS animations of composited
layers no longer repaint content, and non-composited content is kept in tiles,
so only invalidated rects are repainted. The software path repaints the whole
page every frame. Compositing is on the main thread: animations do not run
while the main thread is busy.

## Why not Coordinated Graphics

Infeasible at 00991b6c for a port that has neither Skia nor Cairo nor GLib:

- `CoordinatedGraphics/LayerTreeHost.h` names `SkiaPaintingEngine`
  unconditionally; `ThreadedCompositor`/`AcceleratedSurface` are GTK/WPE + GLib.
- `CoordinatedPlatformLayer::Client::paintingEngine()` exists only under
  `USE(CAIRO)` / `USE(SKIA)`, and the non-Skia branch of
  `CoordinatedBackingStoreProxy.cpp` requires `Cairo::PaintingEngine`.
- PlayStation, the only other non-GLib user, carries ~2,300 lines of forked
  `*PlayStation` host/compositor/surface files.
- It needs `DrawingAreaCoordinatedGraphics` on both sides, replacing the working
  software drawing area, which must stay the fallback.

The `GraphicsLayerTextureMapper` path needed one WebCore hook: the `#else`
branch of `BitmapTexture::updateContents(NativeImage*)`, which was
`RELEASE_ASSERT_NOT_REACHED()`. `WCScene.cpp` (Windows) is the upstream
precedent for rendering into a `BitmapTexture` and reading it back.

## Build flags

`Source/cmake/OptionsHaiku.cmake`: new `option(USE_HAIKU_GL_COMPOSITING ON)`.
When on: `find_package(EGL REQUIRED)` and `USE_TEXTURE_MAPPER ON`. That is the
only cmakeconfig.h change. Deliberately unchanged:

- `USE_COORDINATED_GRAPHICS OFF`: `GraphicsLayerTextureMapper` is only built
  without it.
- `USE_GRAPHICS_LAYER_TEXTURE_MAPPER` off: no WebCore code tests it; in WebKit it
  only plumbs the native-window `LayerTreeHostTextureMapper` (window handle IPC).
- `USE_EGL`, `USE_TEXTURE_MAPPER_GL`: referenced by no source file.
- `ENABLE_WEBGL OFF`, GPU process off. Do not define `ENABLE_3D_RENDERING`:
  `GraphicsContextHaiku.cpp` has a stale block under it that no longer compiles.

Links `libEGL` and `libGLESv2` (`OpenGL::GLES`) into WebCore and WebKit.
`-DUSE_HAIKU_GL_COMPOSITING=OFF` restores the previous build
(`GraphicsLayerHaiku.cpp` is compiled instead; both define `GraphicsLayer::create`)
and leaves cmakeconfig.h byte-identical, so the patch can land without the
~10 h rebuild and be switched on with the next planned one. Turning it ON
changes one cmakeconfig.h line, which rebuilds everything. The OFF
configuration follows from the `#if`s but was not compile-checked.

## Runtime selection

`LayerTreeHostHaiku::isAvailable()` probes once per WebProcess, on first use:

1. `SUMMIT_DISABLE_GL_COMPOSITING=1` -> software (wins over force).
2. `eglGetDisplay` + `eglInitialize` must succeed (`PlatformDisplayHaiku`, which
   returns null instead of crashing like the other ports' displays).
3. A GLES2 context must become current: pbuffer first (the path Mesa's Haiku
   platform is qualified with), surfaceless as fallback.
4. `GL_RENDERER` containing llvmpipe/softpipe/software/swrast/lavapipe is
   rejected unless `SUMMIT_FORCE_GL_COMPOSITING=1`.
5. A 4x4 FBO clear + `glReadPixels` self-test must return the exact pixel.

Every outcome logs one `Summit GL compositing: ...` line with renderer/version.
Observed in today's VM with a standalone program: `eglInitialize` returns
`EGL_FALSE` (0x3001) without crashing, i.e. step 2 selects software there.
"1" means set, non-empty and not "0". On success the display is installed with
`PlatformDisplay::setSharedDisplay()` and the context stays current for good
(`BitmapTexturePool`'s timer otherwise calls `sharedDisplay()`, which asserts).

The UI process always sends `acceleratedCompositingEnabled=false`
(`UIProcess/haiku/WebView.cpp`) and re-sends all preferences on any change.
`DrawingAreaHaiku::updatePreferences()` therefore re-applies the WebProcess's
decision after every push: GL available -> accelerated compositing **and**
`forceCompositingMode` on, so every page of the process takes one path.
If any of this is never reached, the result is today's software path.

Fallback at runtime: `renderFrame()` returning false (context lost, incomplete
FBO, GL error on readback, surface larger than the max texture size) disables GL
for the whole process, turns the settings off and drops that one frame; the next
rendering update tears the layers down and paints in software.

## Details that are easy to get wrong

- `B_RGBA32` is straight alpha; TextureMapper blends premultiplied
  (`GL_ONE, GL_ONE_MINUS_SRC_ALPHA`). Tiles are premultiplied on the CPU at
  upload. Opaque content looks right either way, so test translucency.
- With a `BitmapTexture` surface TextureMapper forces its Y flip, so
  `glReadPixels` rows arrive top-down like a `BBitmap`: no row reversal.
  Reading happens before `endPainting()`, which rebinds the 1x1 pbuffer.
- `GL_BGRA` readback needs `GL_EXT_read_format_bgra`; otherwise RGBA + swap.
- The real driver needs `HAIKU_CSF_DEVICE` in the **WebProcess** environment
  (see MESA.md); without it Mesa cannot open the GPU and the probe reports it.

## What a style or font change repaints (Haiku rules)

Tiles are repainted only where WebCore asks, so rules that ask for "all of
it" cost a full raster of every tile kept. On Haiku:

- A style change on `<html>` or `<body>` repaints the root contents only when
  the root background may have changed (`RenderBox::styleWillChange`), and
  does not repaint their whole visual overflow, after the change or after the
  layout it needs, when they paint nothing differently themselves
  (`RenderElement::setStyle`, `repaintAfterLayoutIfNeeded`); their moved
  children then repaint themselves. `SUMMIT_ROOT_STYLE_REPAINT=1` turns this
  off.
- A web font load renews only the fonts of descriptions whose families can
  reach the loaded family (`CSSFontSelector`, `FontCascadeCache`);
  `SUMMIT_FONT_LOAD_FAMILY_VERSIONS=0` renews all.
- Repeating-pattern tile pictures are shared across recordings so the
  picture shader's raster is reused (`GraphicsContextSkia::drawPattern`);
  `SUMMIT_PATTERN_PICTURE_CACHE=0` makes one per drawing.

See `docs/performance.md`, 30 October 2026, for measurements.

## What the compositor leaves out or keeps (Haiku)

The sections above describe the first, main-thread design. Today the web
process composites on its own thread with the coordinated-graphics
`TextureMapperLayer` tree. These Haiku additions decide what a composition
draws, all in `TextureMapperLayer.cpp` and `CoordinatedBackingStore.cpp`.

- **Occlusion.** Before a composition is painted,
  `collectOccludersHaiku()` walks the layers in paint order and notes what is
  drawn opaquely straight into the target:
  - opaque contents (a video frame, an opaque picture, an opaque solid color);
  - a layer's opaque background color, over all of the layer;
  - a layer's opaque tiles (tiles of a layer WebKit marked
    `contentsOpaque`).
  Only layers with axis-aligned transforms and clips occlude, and only where
  no surface, filter, mask, group opacity or 3D context stands between them
  and the target. While the composition is painted, the following are left out
  when what is drawn after them covers them entirely:
  - tiles and background colors;
  - contents;
  - whole layers drawn through an intermediate surface (a filtered, masked,
    group-opacity or backdrop-filtered layer, with everything in it).
  Each composition works this out anew, so layers that move, animate or scroll
  occlude where they are in that composition. Only drawing is left out: tiles
  are still painted and uploaded, and what a layer uncovers is drawn from the
  tiles it has. WebKit wraps every fixed and sticky layer in an empty
  structural layer that preserves 3D; with one flat child, that wrapper counts
  as a plain parent.
  - `SUMMIT_OCCLUSION=0` turns all of this off.
  - `SUMMIT_OPAQUE_OCCLUSION=0` keeps only the older contents-hide-tiles part.
- **Filter cache.** A filtered layer of its own (no children, mask or
  replica) keeps its filtered result while its contents, filters, size and
  scale stay the same. The result is drawn again moved by whole pixels:
  - in the target;
  - inside another layer's surface;
  - in a backdrop filter's copy of what lies behind it.
  Opacity and transform animations don't stop it; filter animations do.
  - `SUMMIT_FILTER_CACHE=0` turns the cache off.
  - `SUMMIT_FILTER_CACHE_IN_SURFACES=0` keeps it to the target.
  - `SUMMIT_FILTER_CACHE_ANIMATED=0` keeps it from layers with any running
    animation.
- **Backdrop cache.** A `backdrop-filter` re-paints what lies behind it into a
  surface and blurs it in every composition that draws it. Its result is kept
  while nothing under it changed. `collectDamage()` checks each kept result
  when it reaches the backdrop's layer. Damage is collected in paint order, so
  at that point the frame's damage is that of the layers painted before it.
  The frame-wide damage added at the end counts too. These drop the result:
  - a composition that does not check it;
  - a composition whose damage is not known (no damage collected, or a late
    tile upload that makes it the whole frame);
  - a change of the backdrop layer's place, size, filters or mask;
  - a clip larger than the one it was made under.
  `SUMMIT_BACKDROP_CACHE=0` turns it off.
- **Trace.** The compositor timing line (`SUMMIT_COMPOSITOR_TIMING_TRACE=1`)
  counts what was left out:
  - `culled` and `culledMpx` (an intermediate surface counts once, for the
    area it would have covered in the target);
  - `backdropHits` and `backdropMisses`.
  `SUMMIT_DRAW_TRACE=N` adds to each traced draw:
  - the layer and the surfaces drawn into and from;
  - each layer's place in the occlusion (`layer ... order= surfaceOrder=
    occluders=`);
  - what was culled;
  - why a filter or backdrop result was made again.

## Not implemented

- Threaded compositing, async scrolling, damage-limited readback (full frame
  is read each time), reuse of the frame texture across resizes.
- Video, WebGL and canvas as platform layers (they paint into tiles instead).
- UI-process preference to opt out; only the environment variables exist.
- Resize invalidates all root tiles (conservative; same cost as software).

## Validation

VM, once a Mesa whose EGL initializes is installed (software GL):
`SUMMIT_FORCE_GL_COMPOSITING=1`, check the log line, then compare screenshots
pixel-for-pixel against `SUMMIT_DISABLE_GL_COMPOSITING=1` on: a static page;
translucent layers and shadows over a known background (premultiplication);
an asymmetric page (orientation, red/blue order); CSS transform/opacity
animations and `position: fixed` while scrolling; HiDPI scale 2; window
resize; two tabs in one WebProcess. Without the variable on llvmpipe the log
must say "software renderer" and rendering must be unchanged. Force a failure
(e.g. a view larger than the max texture size) and confirm software takes over.

Real GPU: same suite without forcing, expecting `Mali-G610 (Panfrost)`; then
frame times for scrolling and animation against software, WebProcess memory,
and a soak test, since Mesa has not qualified sustained load or GPU reset.
