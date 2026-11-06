# WebGPU on Dawn for Summit (summit#158)

Status: spike that works end to end on the X399 (GTX 1070, Mesa NVK). See
"Results" at the end.

## Architecture

```
navigator.gpu (WebCore/Modules/WebGPU/GPU*.cpp, the platform-neutral DOM layer)
   |  WebCore::WebGPU::GPU / Adapter / Device / ... (Modules/WebGPU/InternalAPI)
   v
Modules/WebGPU/Dawn/*  (new backend, USE(DAWN))           html/canvas/GPUCanvasContextDawn.cpp
   |  Dawn's C API (webgpu/webgpu.h)                          |  canvas frames as NativeImages
   v                                                          v
libwebgpu_dawn.so (Dawn 8674283f, Vulkan backend)      coordinated graphics layer contents
   |  dlopen("libvulkan.so.1")                            (CoordinatedPlatformLayerBufferNativeImage)
   v
Vulkan loader -> Mesa ICD (NVK on the X399; radv; lavapipe as fallback adapter)
```

### Backend (`Source/WebCore/Modules/WebGPU/Dawn/`)

A port of the Cocoa in-process backend (`Modules/WebGPU/Implementation/*Impl.cpp`)
to Dawn. Same class names (`GPUImpl`, `AdapterImpl`, `DeviceImpl`, ... and the
`DowncastConvertToBackingContext`), so the InternalAPI's `isXxxImpl()` type
traits and the structure of the Cocoa code carry over and the two can be
diffed. The differences:

- WebKit's `WebGPU.h` dialect is replaced by Dawn's `webgpu.h`: string views for
  entry points and constant keys, `BindingNotUsed` (not `Undefined`) for unused
  bind group layout members, vertex buffer holes as step mode `Undefined`,
  `WGPURenderPassMaxDrawCount` chained, per-stage storage limits through
  `WGPUCompatibilityModeLimits`, `wgpuAdapterGetFeatures`/`GetLimits`/`GetInfo`.
- Objective-C blocks are replaced by `makeDawnCallbackInfo<WGPUxxxCallbackInfo>(lambda)`
  (`WebGPUPtr.h`): a heap-allocated lambda behind a C trampoline. Dawn calls every
  callback exactly once (also when cancelled), so the lambda is always freed.
  (Dawn's own C++ wrapper only accepts `const` lambdas, and WebKit's completion
  handlers need mutable ones.)
- A WebGPU texture given where WebKit allows "texture shorthand" (attachments,
  bind group entries) becomes its default view, which Dawn requires.
- `invalidCommandEncoder()`, `invalidCommandBuffer()`, `invalidRenderPassEncoder()`,
  `invalidComputePassEncoder()` and `emptyBindGroupLayout()` were
  `RELEASE_ASSERT_NOT_REACHED()` in the Cocoa in-process backend (only the GPU
  process path implements them) but the DOM layer calls them after `end()`,
  `finish()` and `submit()`. On Dawn they are real used-up objects (a finished
  encoder, a submitted command buffer, an ended pass on a 1x1 attachment), cached
  per device, so using them is the validation error it should be.
- Uncaptured errors and device loss are callbacks given at device creation in
  Dawn; `DawnDeviceState` queues them until the DOM layer asks
  (`resolveUncapturedErrorEvent`, `resolveDeviceLostPromise`). The first 20
  uncaptured errors of a device are also written to the engine log.
  `GPUDevice::listenForUncapturedErrors` was Cocoa-only; it now also runs for Dawn.
- The "auto" pipeline layout: the DOM layer makes it as a pipeline layout with no
  bind group layout list (`createAutoPipelineLayout()`); on Dawn that is a
  `PipelineLayoutImpl` without a backing, so pipelines made with it pass a null
  layout and Dawn derives the layout from the shaders
  (`getBindGroupLayout()` then works as specified).
- `getCompilationInfo()` passes Dawn's 1-based line/column through (WebKit's own
  compiler counts from 0).

Stubbed / not supported yet (each fails cleanly):
- `importExternalTexture()` returns null, so `GPUDevice.importExternalTexture`
  throws `InvalidStateError`; external texture bind group layout entries are a
  validation error.
- WebXR bindings (`createXRBinding()` returns null).
- `copyExternalImageToTexture()` with GPU-resident sources: never reached
  (`HAVE(IOSURFACE)` is off), every source takes GPUQueue's CPU read-back path,
  which ends in `writeTexture()`. That path's format conversion
  (`copyToDestinationFormat`) is still `PLATFORM(COCOA)` only, so the RGBA8 bytes
  are written unconverted: right for `rgba8unorm` destinations, wrong for others.
- `pauseAllErrorReporting()` (Web Inspector shader editing) only silences
  uncaptured errors; `create*PipelineWithPipelineLayoutFromPipelineAsync`
  (inspector) creates the pipeline with its own layout.
- `TextureImpl::undestroy()` is a no-op (Cocoa canvas only).
- Workers: `WorkerNavigator.gpu` stays null (its guard is still
  `HAVE(WEBGPU_IMPLEMENTATION)`), so no WebGPU in workers or worker
  OffscreenCanvas yet.
- HDR canvases (`toneMapping: extended`) are configured as standard.

### Threading and event pumping

Everything runs on the web process's main thread, like WebGL on this port (no
GPU process). One `WGPUInstance` per web process, created lazily by
`createDawnGPU()` (`WebGPUCreateDawn.cpp`) the first time a page touches
`navigator.gpu`, with the `TimedWaitAny` instance feature.

All asynchronous calls (requestAdapter/Device, mapAsync, onSubmittedWorkDone,
create*PipelineAsync, popErrorScope, getCompilationInfo, device lost) use
`WGPUCallbackMode_AllowProcessEvents`, so callbacks only run inside
`wgpuInstanceProcessEvents()`. `DawnEventPump` keeps a main `RunLoop::Timer`
that calls it every 1 ms while any callback is outstanding (counted by a
`DawnPendingCallback` captured in each lambda) and every 100 ms while devices
are alive (Dawn frees finished resources and notices device loss there); with
neither it stops. Uncaptured errors arrive synchronously from Dawn calls and are
forwarded with `callOnMainRunLoop`.

### Presentation (`GPUCanvasContextDawn`, `PresentationContextImpl`, `CompositorIntegrationImpl`)

There is no WSI surface. `configure()` remembers the device, format, usage
(plus `CopySrc`) and view formats; `getCurrentTexture()` creates an ordinary
Dawn texture of the canvas size and hands out the same one until the canvas is
presented (the spec's "expire the current texture" happens at presentation, not
at the end of the task as on Cocoa).

`getCurrentTexture()` marks the canvas dirty (`willUpdateCanvasContents`, which
schedules `prepareForDisplay` in the next rendering update and marks the
compositing layer's contents for display). `prepareForDisplay()`:

1. `CompositorIntegrationImpl::prepareForDisplay` -> `PresentationContextImpl::readCurrentFrame`:
   `copyTextureToBuffer` into a persistent `MapRead` buffer (rows padded to 256
   bytes), submit, `wgpuBufferMapAsync` with `WaitAnyOnly` and
   `wgpuInstanceWaitAny` (1 s timeout) - i.e. a synchronous read-back -
   then rows are copied into a Skia raster image (BGRA, premultiplied; `rgba8unorm`
   is swizzled, `rgba16float` converted; alpha forced to 255 for `opaque`).
2. The picture becomes the layer contents through
   `GraphicsLayerContentsDisplayDelegateCoordinated` +
   `CoordinatedPlatformLayerBufferNativeImage` (the same buffer type the
   accelerated 2D canvas uses), which the compositor uploads to a GL texture.
3. The texture is presented and destroyed; the next `getCurrentTexture()` makes
   a new one.

`toDataURL()`, `drawImage(webgpuCanvas)`, `createImageBitmap(canvas)` and the
non-composited paint path read the same picture (`surfaceBufferToImageBuffer` /
`surfaceBufferToNativeImage`, which first present a pending frame for the
drawing buffer). `transferToImageBitmap()` of an OffscreenCanvas ends the frame.
`getPreferredCanvasFormat()` stays `bgra8unorm`, which is Skia's N32 order on
x86_64 and arm64, so the common case is a plain row copy.

## Build: finding Dawn

- `OptionsHaiku.cmake`: `ENABLE_WEBGPU` is a PUBLIC option, default OFF until
  airos-ci ships Dawn. With it on, `DAWN_INSTALL_DIR` (cache path) must point at
  a Dawn install (`include/webgpu/webgpu.h`, `lib/libwebgpu_dawn.so`); it makes
  the imported target `Dawn::webgpu_dawn` and sets `USE(DAWN)`.
- `Source/CMakeLists.txt`: Haiku skips `Source/WebGPU` (WebGPU.framework and the
  WGSL compiler are Metal-only and unused).
- `WebCore/PlatformHaiku.cmake`: the backend sources are listed there (not in
  the unified `Sources.txt`) and only they get Dawn's include directory
  (`set_source_files_properties(... INCLUDE_DIRECTORIES)`), so Dawn's
  `webgpu/webgpu.h` never meets WebKit's `<WebGPU/WebGPU.h>` or the InternalAPI's
  `"WebGPU.h"`. `WebGPUCreateDawn.h` (no Dawn headers) is the entry point:
  `Navigator::gpu()` calls `WebGPU::createDawnGPU()` directly under `USE(DAWN)`.
  (The Cocoa route is `ChromeClient`/`GraphicsClient::createGPUForWebGPU()`,
  which is `HAVE(WEBGPU_IMPLEMENTATION)`-only and declared in headers most of
  WebCore includes; widening those guards is the cleaner long-term hook, at the
  cost of rebuilding most of WebCore and WebKit. `OffscreenCanvas` now also
  creates WebGPU contexts on the main thread under `USE(DAWN)`.)
- The Cocoa `Modules/WebGPU/Implementation` files stay in `Sources.txt` and
  compile to nothing (`HAVE(WEBGPU_IMPLEMENTATION)`).
- libWebKit gets `NEEDED libwebgpu_dawn.so` (Dawn's SONAME); the engine's RPATH
  `$ORIGIN` finds it next to libWebKit.

CI passes `-DDAWN_INSTALL_DIR=$P/dawn` (airos-ci `summit/build-engine.sh`)
whenever Dawn is in the deps prefix; the init cache turns `ENABLE_WEBGPU` on.

## Run time

- `libwebgpu_dawn.so` needs only libroot, libstdc++ and libgcc_s, so linking it
  is safe on machines without Vulkan: Dawn `dlopen`s `libvulkan.so.1` when the
  first adapter is requested. Without a loader or ICD, `requestAdapter()`
  resolves to `null` (and Dawn logs nothing; a page sees "no adapter").
- Symbol clashes: libWebKit's own abseil (libwebrtc) is not exported (checked:
  the only dynamic symbols both export are libstdc++ template instantiations),
  and libWebKit is linked `-Bsymbolic`, so Dawn's exported abseil and its weak
  `AbslInternal*` hooks do not interpose on WebKit's copy or vice versa.
- Runtime preference: `WebView.cpp` sets `WebGPUEnabled` on when the engine is
  built with WebGPU; `SUMMIT_PREF_OFF=webgpu` turns it off (#147 convention).

## Packaging for airos-ci

Done (6 Nov 2026):
- Fork `airos-works/dawn` branch `airos-8674283f` (Dawn 8674283f with the Haiku
  port: POSIX platform rather than Linux, `libvulkan.so.1`, executable and
  module paths, worker thread names; no DRM, dma-buf or RenderDoc), made by
  `forks/make-forks.py` from `forks/patches/dawn-haiku.patch`.
- `summit/build-deps.sh r_dawn`: Dawn's dependencies at the commits its `DEPS`
  pins (`tools/fetch_dawn_dependencies.py --shallow`), abseil with
  `forks/patches/abseil-cpp-dawn-haiku.patch` (`GetTID`), Vulkan and Null
  backends, Tint with the WGSL reader and SPIR-V writer, the monolithic
  `libwebgpu_dawn.so` installed in `$P/dawn` (its `webgpu/webgpu.h` stays out
  of the engine's include path) and copied to `$P/lib`, where summit_webkit's
  packaging finds it through libWebKit's `NEEDED`. x86_64 13 MB, arm64 11.5 MB.
- Still to do: Dawn's, Tint's and abseil's licence notices in the package.

## Risks

- Synchronous read-back per frame: the main thread waits for the GPU each frame
  a canvas is drawn (a 1200x1200 frame is ~5.5 MB copied twice). Fine for
  samples, a bottleneck for heavy content and large canvases at 200% scale.
- Dawn on Haiku is new: NVK only so far; radv/lavapipe/panvk untested. Device
  loss and driver hangs have not been exercised.
- Everything on the main thread: shader compilation (Tint + driver) blocks it for
  synchronous pipeline creation, as it does in WebKit's Cocoa in-process mode.
- The DOM layer was only ever compiled for Cocoa (and the GPU-process proxies);
  GCC builds it now, but untested paths may still assume Cocoa behaviour.
- Security: WebGPU runs in the (sandbox-less) web process with direct Vulkan
  access, as WebGL does on this port; Dawn's validation is the only barrier
  between page and driver (Chromium has a GPU process for this).
- Memory: one texture allocation per canvas frame (no recycling yet).

## Next steps

1. Zero-copy presentation: export the frame texture from Vulkan
   (`VK_KHR_external_memory_fd`/dma-buf is compiled out on Haiku today) and
   import it into the compositor's EGL/GL (`GL_EXT_memory_object_fd`), or have
   Dawn render into a buffer the compositor can sample; until then, at least an
   asynchronous read-back (map in the background, show the frame one update
   later) and a small texture pool.
2. External textures from video: wrap decoded frames (CPU planes, or VA-API/NVDEC
   surfaces) as Dawn textures for `importExternalTexture()`; enable GPUQueue's
   CPU format conversion (`copyToDestinationFormat`) outside Cocoa.
3. arm64: build Dawn for arm64 (configure.sh arm64) and try panvk on the Rock 5 /
   Cubie (Mali), V3DV on the Pi; lavapipe as the CPU fallback adapter.
4. Workers (`navigator.gpu` in dedicated workers) and OffscreenCanvas in workers.
5. Run the WebGPU CTS (Dawn passes most of it upstream; the WebKit DOM layer is
   the unknown).

## Results (X399, GTX 1070, Mesa NVK, 3840x2160 at scale 2, 6 Nov 2026)

A private engine build of this code with the Dawn build above, in a test copy
of Summit.

- `navigator.gpu` present on secure pages; `getPreferredCanvasFormat()` = `bgra8unorm`.
- `requestAdapter()`: Vulkan adapter on NVK, not fallback. `adapter.info` vendor/
  architecture/device/description = "nvidia" (WebKit's DOM layer reduces the name to
  its first word). Features: bgra8unorm-storage, clip-distances,
  core-features-and-limits, depth-clip-control, depth32float-stencil8,
  dual-source-blending, float32-blendable, float32-filterable,
  indirect-first-instance, primitive-index, rg11b10ufloat-renderable, subgroups,
  texture-compression-bc, texture-compression-bc-sliced-3d, texture-formats-tier1,
  texture-formats-tier2, timestamp-query (no shader-f16 on this driver).
  maxBufferSize 2 GiB, maxTextureDimension2D 32768, maxComputeInvocationsPerWorkgroup 1024.
- Compute (`compute.js`): 1,048,576 u32 through a storage buffer with
  createComputePipelineAsync + 'auto' layout, writeBuffer, dispatch, copy,
  onSubmittedWorkDone, mapAsync: PASS, 27 ms round trip. Error scope catches a
  GPUValidationError; the `uncapturederror` event fires; `device.destroy()`
  resolves `device.lost` with reason "destroyed".
- Without a Vulkan driver (`VK_ICD_FILENAMES=/nonexistent.json`):
  `requestAdapter()` resolves to null, the web process keeps running.
- WebGPU samples (webgpu.github.io/webgpu-samples, standalone pages): helloTriangle
  (`shots/helloTriangle.png`), helloTriangleMSAA, rotatingCube, twoCubes,
  texturedCube (image through copyExternalImageToTexture), instancedCube,
  fractalCube (canvas as texture source), cubemap, computeBoids, gameOfLife,
  particles, renderBundles, shadowMapping, deferredRendering, transparentCanvas
  (premultiplied alpha over page text), resizeCanvas, multipleCanvases all render
  correctly (`shots/batch1.png`, `shots/batch2.png`). Most run at 58-60 fps;
  particles and gameOfLife draw ~19 frames/s (their own frame loop: one submit per
  rAF, ~50 ms each, GPU-bound - Pascal under nouveau/NVK runs at boot clocks).
- `toDataURL()` and `drawImage(webgpuCanvas)` read the right pixels (red centre,
  black corner of the triangle canvas).
- Not broken: Wikipedia renders normally; three.js WebGL (Littlest Tokyo) runs at
  59 fps; a WebGL2 clear/readPixels returns the right colour.
