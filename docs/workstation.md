# The Haiku workstation: building, the GPU stack and benchmarking

Summit's second Haiku machine is a bare-metal workstation, and it is the only
one with a GPU. Everything in this document was set up and verified on
September 21, 2026; the QEMU VM ([VM.md](VM.md)) is unchanged and remains the
default target of every tool.

| | |
| --- | --- |
| Reached with | `bash tools/ws.sh '<command>'` (key in `.vm/ws_id_ed25519`) |
| CPU | AMD Ryzen Threadripper 1950X, 16 cores / 32 threads, 3.4 GHz |
| Memory | 64 GiB |
| GPU | NVIDIA GeForce GTX 1070 (GP104), 1920x1080 at 60 Hz |
| System | Haiku R1/beta6 hrev60097+49 x86_64 (the owner's own build) |
| Disk | 238 GiB NVMe, about 170 GiB free |
| Screen | VNC on port 5900 (`vncserver` starts at login), `screenshot -s -f png` over SSH |

## Building Summit natively

The workstation builds the engine about fifteen times faster than the VM (a
complete `ModernGL` configuration in roughly an hour and a half at `-j24`
instead of about ten hours), and it is where GPU compositing can actually be
tested. The build scripts take the machine as an environment variable:

```sh
export SUMMIT_REMOTE_SHELL=tools/ws.sh SUMMIT_REMOTE_TAG=ws
SUMMIT_ICU_JOBS=24 bash tools/build-icu-in-vm.sh            # once
SUMMIT_ENGINE_BUILD_NAME=ModernGL SUMMIT_WEBKIT_JOBS=24 \
  SUMMIT_ENGINE_CMAKE_EXTRA="-DUSE_HAIKU_GL_COMPOSITING=ON -DUSE_SYSTEM_MALLOC=OFF -DUSE_MIMALLOC=ON" \
  bash tools/build-webkit-in-vm.sh --modern-extensions all
bash tools/build-modern-browser-in-vm.sh --browser --bundle --modern-extensions
```

For interactive checks on September 24, 2026, the desktop launcher
`/boot/home/Desktop/Summit-current.sh` runs the tested
`bundle-s8a06mrs` with the private Mesa GPU libraries. The system-installed
`/boot/system/apps/Summit` is an older September 20 build. The desktop
launcher uses Summit's normal `SummitModern` profile; benchmark runs use
separate profiles. The launcher enables Skia GL Canvas by default after the
raster-scrollbar and Guardian playback checks. Set
`SUMMIT_SKIA_GL_CONTEXT=0` when launching it to compare CPU Canvas rendering.

`SUMMIT_REMOTE_TAG` only names the host-side lock and result files, so a VM
build and a workstation build can run at the same time.

One flag is worth changing here. The engine is compiled with
`--param ggc-min-expand=10`, which holds GCC's garbage collector to a tenth of
its usual growth so that six parallel jobs fit in the VM's 20 GiB. With 64 GiB
it only costs time: one WebCore unified source took **97 s with the parameter
and 57 s without**, and the build peaked at 31 GiB of 64 GiB. Set

```sh
SUMMIT_ENGINE_CXX_FLAGS=-ftrack-macro-expansion=0
```

for a full build here. It changes `CMAKE_CXX_FLAGS`, so it reconfigures CMake
and rebuilds everything: choose it when starting a build, not in the middle. Guest paths are the
ordinary ones (`/boot/home/summit-webkit-extensions`, `/boot/home/summit`);
the boot volume has room, so nothing needs a second volume as in the VM.

### Installing packages

**`pkgman` cannot download on this machine**: every repository access fails with
`Operation not supported`, although `curl` reaches the same URLs. Packages are
therefore fetched by hand and installed from the file:

```sh
curl -O https://haikuports-repository.cdn.haiku-os.org/master/x86_64/current/packages/<name>-<version>-x86_64.hpkg
pkgman install -y ./<name>-<version>-x86_64.hpkg
```

Two things to know. Dependencies still have to be fetched the same way, one
round at a time (`pkgman` prints them as "from repository"). And copying an
`.hpkg` straight into `/boot/system/packages` does *not* activate it; it wedges
`package_daemon` into "Another package operation is already in progress", which
`kill <package_daemon team>` clears (`launch_daemon` restarts it).

Added for the engine build: `gperf`, `ruby`, `nasm`, `patch`, `diffutils` and
the `_devel` packages for icu74, libxml2, libxslt, libjpeg_turbo, libpng16,
libwebp, lcms, libavif, woff2, libexecinfo, libiconv, libpsl, dav1d and rav1e.

## OpenGL on the GeForce: zink over NVK

Summit's compositor needs **EGL with OpenGL ES**
([gpu-compositing.md](gpu-compositing.md)). Haiku's own Mesa cannot provide it,
and neither can the Zink add-on that is already installed here:

- `mesa` 22.0.5 from HaikuPorts has EGL disabled on purpose ("Does not work"),
  so `eglInitialize` returns `EGL_NOT_INITIALIZED` — the same wall as in the VM
  ([vm-egl-stack.md](vm-egl-stack.md)).
- `~/config/non-packaged/add-ons/opengl/Zink` is a **`BGLView`** renderer. It
  serves desktop OpenGL through `libGL`, which no part of WebKit calls.

What does work is the private Mesa 25.3.6 from `tools/mesa-vm/`, built with the
`zink` Gallium driver instead of the software rasterizers, so GLES becomes
Vulkan and Vulkan is NVK:

```sh
SUMMIT_REMOTE_SHELL=tools/ws.sh SUMMIT_MESA_ROOT=/boot/home/summit-mesa \
  SUMMIT_MESA_JOBS=10 SUMMIT_MESA_DRIVERS=zink bash tools/mesa-vm/build.sh upload
SUMMIT_REMOTE_SHELL=tools/ws.sh SUMMIT_MESA_ROOT=/boot/home/summit-mesa \
  SUMMIT_MESA_JOBS=10 SUMMIT_MESA_DRIVERS=zink bash tools/mesa-vm/build.sh run \
  check clean tools glvnd mesa-prepare mesa-configure mesa-build mesa-install probe
```

Two upstream gaps had to be closed
(`tools/mesa-vm/mesa-25.3.6-summit-02-haiku-egl-zink.patch`):

- `src/egl/meson.build` gives the Haiku platform `driver_swrast` and
  `driver_panfrost` but not `driver_zink`, so `GALLIUM_ZINK` was never defined
  and every zink branch in the target helpers compiled out.
- `inline_sw_helper.h` calls `zink_create_screen()` without including
  `zink/zink_public.h` (`sw_helper.h`, its sibling, does include it).

`egl_haiku.cpp` then prefers a zink screen over the software one, unless the
display asks for software or `LIBGL_ALWAYS_SOFTWARE=1` is set.

Using it needs one variable, because Haiku's `runtime_loader` takes
`LIBRARY_PATH` as the complete search path:

```sh
LIBRARY_PATH=/boot/home/summit-mesa/prefix/lib:/boot/system/lib <program>
```

The Vulkan loader finds NVK by itself here; no `VK_ICD_FILENAMES` is needed.
`tools/bench/*` pass this through `SUMMIT_LIBRARY_PATH_PREFIX`.

**A changed Mesa goes into a directory of its own**, because a browser that
is running has the installed one mapped. After a patch in
`work/mesa-25.3.6`, `guest-build.sh mesa-build` relinks
`build/mesa-zink/src/egl/libEGL_mesa.so.0.0.0` in seconds. Copy `prefix` to
`prefix-<date>`, put the library in its `lib`, and change the path in its
`data/glvnd/egl_vendor.d/50_mesa.json`: libglvnd finds Mesa through that
file, which names the library by its full path, so `LIBRARY_PATH` alone
still loads the old one. A program then needs both:

```sh
M=/boot/home/summit-mesa/prefix-20260930
LIBRARY_PATH=$M/lib:/boot/system/lib \
__EGL_VENDOR_LIBRARY_FILENAMES=$M/data/glvnd/egl_vendor.d/50_mesa.json <program>
```

`SUMMIT_MESA_PREFIX=prefix-20260930 bash tools/install-on-workstation.sh
bundle-NAME` writes a launcher that does. `prefix-20260930` is Mesa with
`mesa-25.3.6-summit-05-frame-copies.patch` and its shader cache
(`-Dshader-cache=enabled`, 30 September 2026; the cache is
`/boot/home/.cache/mesa_shader_cache`); `prefix-20260929` is the same
without the cache; `prefix` is the build of 27 September.

**A VNC screenshot stops every window's drawing for about 4 s** on this
machine (docs/performance.md, 30 September 2026). Take none while the owner
is at it, and none during a timed run.

Verified by `tools/mesa-vm/`'s probes:

```
GL_RENDERER=zink Vulkan 1.3(NVIDIA GeForce GTX 1070 (NVK GP104-A) (MESA_NVK))
GL_VERSION=OpenGL ES 3.1 Mesa 25.3.6
clear+readback 1024x768: 1.45 ms/frame
surfaceless / pbuffer / second thread: 64x64 scene exact, 0 bad pixels
1024x768 blended textured quad: 0.55 ms/frame draw+finish, 1.83 ms draw+readback
```

Known limitation: the probe's last check, a display from
`eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA)`, **hangs** with zink. No
Summit code path uses that platform (`LayerTreeHostHaiku` uses
`eglGetDisplay(EGL_DEFAULT_DISPLAY)`), and every check before it passes.

## Hardware video decoding (NVDEC)

The owner's NVDEC decoder is installed as an ordinary Media Kit plugin in
`~/config/non-packaged/add-ons/media/plugins/nvdec`, so anything that decodes
through `BMediaFile`/`BMediaTrack` gets it without knowing. Summit's
`MediaPlayerPrivateHaiku` does exactly that. Confirmed with the owner's own
tool:

```
$ /boot/home/build/nvdec/mediadecode /boot/home/bbb12s.mp4 60
decoder: H.264 on the graphics card (NVDEC)
1920x1080, 7680 bytes a line, 30.00 pictures a second, 366 in all
60 pictures in 927.6 ms, 15.46 ms each (64.7 a second)
```

The plugin decodes **H.264 only** and hands back `B_RGB32`.

### Hardware video decoding in Summit

Summit reaches the plugin: a WebProcess playing a 1080p H.264 file has its
`video decoder` thread inside `nvdecH264Decode()` and uses 0.11–0.25 cores.
It then **crashes there**, and the crash is in the plugin, not in Summit:

```
thread 49419: video decoder   state: Exception (Segment violation)
  strlen + 0x32
  vfprintf / vsnprintf / __snprintf
  nvdecH264Decode(NvdecH264*, const uint8_t*, size_t, int64_t) + 0x6fb
      (/boot/home/build/nvdec/nvdec_h264.c:237)
  decoder: { reason: Bad address ... }
```

`NVDecPlugin.cpp` passed a stack-local `char reason[256]` to
`nvdecH264Create()`, which kept the pointer in `decoder->reason`. The fix in
[`tools/nvdec/reason-lifetime.patch`](../tools/nvdec/reason-lifetime.patch)
makes the buffer a decoder member. The patched add-on is installed on the
workstation, with the old binary saved as `nvdec.before-summit`. The owner's
`mediadecode` now reads 60 frames of `/boot/home/bbb12s.mp4` through NVDEC in
612 ms (98 frames/s), without a crash. This checks the plugin separately from
Summit. Earlier crash reports are in `/boot/home/summit/bench/reports/`.

Summit's `MediaPlayerPrivateHaiku` now opens local `file:` media through an
`entry_ref`. Haiku's `BUrl` reports an empty host as present for `file:///`, so
checking only `HasHost()` silently selected the URL reader and produced
`partial file` / `Invalid NAL unit size` errors. The native file reader removes
those errors. In the Skia coordinated build (`bundle-2apsm5d1`), a 12.24 s
H.264/AAC MP4 passed the former 0.6 s stall, and a video-only copy played to
its 12.24 s end. Reloading the video in the same tab also continued playback.
The patched plugin decoded all 366 source frames separately in 2.83 s.

The HTTP path now has a direct check. Media Kit's `BMediaFile(BUrl)` reader
reported corrupt packets and a partial MP4 near the file's end, and its audio
track clock advanced only about 0.05 seconds per real second. Summit now
downloads HTTP(S) media on the cancellable loader thread (up to 256 MiB),
opens the completed file as a `BFile`/`BDataIO` for Media Kit, and removes its
temporary pathname while the file handle stays open. Video uses the monotonic
clock; the backend keeps duration and signals end of stream after the tracks
finish. The HTTP probe is `tools/bench/pages/media.html`.

In `bundle-kqw134os`, the 12-second H.264/AAC file served over HTTP reached
`ended` at 12.45 seconds with `currentTime` advancing at normal speed
(`.vm/bench/probe-20260922-222126-summit-http-media-ended/`). A 720x1280
H.264 stream from a Reddit post played over HTTPS to `ended` at 12.7 seconds
(`.vm/bench/probe-20260922-222214-summit-reddit-cmaf/`). The Reddit post page
also loaded in Summit and showed its player at 0:12/0:12 with the completion
overlay (`.vm/bench/reddit-site-20260922/final.png`). The first full download
must finish before playback starts. `ENABLE_MEDIA_SOURCE` is still off, so
other sites or Reddit streams that require Media Source Extensions remain
unsupported.

## Firefox, for comparison

Firefox 155.0 is installed at `/boot/system/apps/Firefox/Firefox`.
`tools/bench/run-firefox-speedometer.py` runs the same local Speedometer 3.1
checkout in it; the page posts its result back to this host, so the score comes
from JSON rather than from a screenshot.

## Things that skew a measurement here

- **The screen blanker stops rendering.** After a few idle hours app_server
  sends no Draw messages, so the UI process never acknowledges a frame, the web
  process waits forever and the browser looks frozen (one run reported
  `present=602561 ms`). `guest.wake_display()` stops `screen_blanker` before
  and during a run.
- `vncserver` runs from the owner's boot launch folder and polls the frame
  buffer continuously, using about 3% of one core. Stop it for a quiet run.
- A parallel engine build saturates all 32 threads; `run-speedometer.py
  --wait-quiet` refuses to start until the machine is idle.
- `screenshot -s -f png <file>` exits non-zero even when it wrote the file.
- `~/config/settings/system/debug_server/settings` now asks for a report
  instead of a dialog when `WebProcess` or `NetworkProcess` crashes, so an
  unattended run is not stopped by an alert waiting to be answered.

## fontconfig has no conf.d

Haiku's `fontconfig` package installs `/boot/system/data/fontconfig/conf.avail`
but no `conf.d`, and `/boot/system/settings/fonts/fonts.conf` includes `conf.d`
with `ignore_missing="yes"`. None of the standard configuration is therefore
active -- including `45-generic.conf` and `60-generic.conf`, which define the
`serif`, `sans-serif` and `monospace` aliases.

`fc-match` conceals it, because it answers every query with a best effort, but a
program asking fontconfig to match a family gets nothing back. In Summit's Skia
build that crashed the web process on every Speedometer run: the last-resort
fallback fell through to `SkTypeface::MakeEmpty()`, and the empty font tripped a
hash table assertion (see [performance](performance.md)).

The fix is the ordinary fontconfig layout:

```
D=/boot/system/settings/fonts/conf.d
A=/boot/system/data/fontconfig/conf.avail
mkdir -p "$D"
for c in 10-hinting-slight 10-scale-bitmap-fonts 10-yes-antialias \
         11-lcdfilter-default 20-unhint-small-vera 25-unhint-nonlatin \
         30-metric-aliases 35-lang-normalize 40-nonlatin 45-generic 45-latin \
         48-guessfamily 48-spacing 49-sansserif 50-user 51-local 60-generic \
         60-latin 65-fonts-persian 65-khmer 65-nonlatin 69-unifont \
         80-delicious 90-synthetic; do
    ln -sf "$A/$c.conf" "$D/$c.conf"
done
```

Afterwards `fc-match serif`, `sans-serif` and `monospace` answer Noto Serif,
Noto Sans and Noto Sans Mono; before, all three answered Noto Sans.

## The applications menu (2026-09-29)

Deskbar's Applications > Summit is `/boot/system/apps/Summit` from the
`summit` package. Until today that was the 21 September preview package,
whose binary was the legacy-engine browser, so the menu launched an old
Summit while `Summit-current.sh` on the Desktop launched the current one.
`tools/package-launcher-on-workstation.sh` now builds and installs a
`summit` package (0.1.0~dev2) whose `apps/Summit` is a script with the
Summit icon that runs the Desktop launcher, so the menu follows whatever
`install-on-workstation.sh` installed last. Run it again only if the
package is removed; installing a new bundle needs only the install script.

The first version of that script `exec`ed the launcher. Launched from the
Deskbar (which hands the item to Tracker), the roster pre-registers the
script's team as the application; the browser then found "itself" already
running and its message loop spun at 100% CPU on a dead port (`strace`:
`port_buffer_size_etc` returning Bad port ID forever), a window never
appeared, and nothing was logged. Launched with `open` from a shell the
same script worked, which is why it was not caught. The script now starts
the launcher as its own team and exits (package 0.1.0~dev2-2), verified by
clicking through Deskbar > Applications > Summit over VNC.


## WX5100 decoder recreation and lost rendering contexts (2026-10-11)

The WX5100 recovery candidate is private at
`/boot/home/wx5100/summit-recovery/bundle`. It requests GLES reset notification
through `EGL_EXT_create_context_robustness` for the sharing, compositor and
Skia contexts. An AMD decoder failure retires that decoder without permanently
blacklisting the add-on: the asynchronous close gate still excludes a decoder
while it closes, and a fresh session must pass the recovered kernel's checks.
Other hardware backends keep their existing failure policy.

The candidate engine is `ce4032cd5ab522cc8eb9ddff4a28637818eb0073716fd746641386318ae75cad`,
compiled from the two engine changes in `4f2a0a6`, then relocated with the
existing bundler. Its private `libepoxy` dependency is included. Initial QEMU
staging failures (missing epoxy, then incorrect absolute RPATHs selecting a
system JavaScriptCore) are preserved alongside the corrected, passing runs.
The original bundle and its engine
`c07b7f15422e6ec13b28042f724c2218dab3aec02ed306d2043a6295fa127c7c`
remain untouched. The earlier app-recovery manifest recorded an unrelated
system engine hash; this is the actual original bundle's engine hash.

`tools/bench/pages/mse-recovery.html` opens two independent MSE decoders in one
document. A test-only add-on injects one real GPU readback timeout after 19
outputs. The host checks exactly one automatic reset and two AMD decoder
selections, proving the second decoder runs in the same process. Independent
no-reset references are necessary: native hardware and software color
conversion differ. At 320 by 240, the final Main10 software reference hash is
1164650555 and the hardware reference is 2776130761; recovered output matches
each reference byte for byte. `capturePixels=1` saves both PNGs in the probe
report, while `firstPixelHash` and `secondPixelHash` specify the references.
Without these parameters the two outputs must match each other exactly.

With CPU compositing, both decoders reach 48 frames and EOS; the first recovery
round reports 20 drops and the recreated hardware decoder reports zero. The
visible header updates and video reaches frame 47. No crash reporter or owned
browser helper remains. This qualifies decoder recreation, not uninterrupted
playback through a reset.

With GPU compositing, both decoder rounds also reach EOS with matching reference
pixels (19 drops in the first round, zero in the second), but the visible page
freezes at frame 16. Mesa now reports `GL_CONTEXT_LOST` during playback instead
of the earlier winsys abort. Shutdown then aborts in libepoxy's lazy
`glDeleteShader` resolver, called by `TextureMapperShaderProgram` destruction
inside `ThreadedCompositor::invalidate`. The new delayed-crash observation
catches both the syslog stack and the in-progress Debugger report. This is a
failed renderer recovery test even though its JavaScript report passes.

The active renderer is CoordinatedGraphics `ThreadedCompositor` with
`AcceleratedSurface`; `LayerTreeHostHaiku` is a different path. Reset detection
and resource cleanup guards are implemented in `1376553`, as described below.
Recovery still needs to finish or cancel pending frame callbacks, quiesce all
pages and worker contexts, clear shared texture and Skia state, recreate the
shared EGL display and compositor contexts, and repaint the existing document.
A process restart or permanent software fallback does not qualify this recovery
path.

The separate no-EGL startup failure is fixed by `f382c16`: an unavailable EGL
display now selects CPU rendering and sends a one-time notice to the browser.
Native validation with an invalid EGL vendor path passed DOM/canvas pixel
checks, animation, scrolling, navigation, multiple processes and clean shutdown;
web app notices are covered by `e7ee764`. See the 21 October startup-without-EGL
entry in `performance.md`. This does not repair the live GPU-reset failure
described here, so #100 remains open.

### Context lifecycle probe, 21 October 2026 (#100)

`tools/bench/egl-context-lifecycle.cpp` builds as a small native executable with
`g++ -std=c++17 egl-context-lifecycle.cpp -o egl-context-lifecycle -lepoxy -lbe`.
Run it with the same Mesa environment as Summit. It creates only offscreen
contexts owned by the probe and never resets the GPU or changes the display.

On the GTX 1070 workstation, Mesa 25.3.6/zink exposes EGL 1.4,
`EGL_EXT_create_context_robustness`, GLES 3.1 and
`glGetGraphicsResetStatusKHR`. All 15 checks pass: destruction of a current
context is deferred until it is unbound; binding its destroyed handle then
fails with `EGL_BAD_CONTEXT` (0x3006). A sibling context still sees their shared
texture, and the reset-status query remains `GL_NO_ERROR`. Both a replacement
in the existing share group and a fresh share group work; the fresh group
renders exact RGBA (204, 51, 102, 255), then tears down cleanly.

This distinction matters for the recovery test: destroying one compositor
context does not invalidate the shared textures or the Skia contexts as a
real graphics reset can. Recovery must also cover lost shared state, rather
than pass only a context-replacement test. The
[Khronos robustness specification](https://registry.khronos.org/OpenGL/extensions/KHR/KHR_robustness.txt)
requires a new context after a reset. Native probe output is retained at
`.vm/issue100/recovery/egl-lifecycle-native.log`. This is prerequisite evidence;
it does not qualify renderer recovery or close #100.

### Skia teardown after a failed bind

`tools/bench/skia-context-teardown.cpp` exercises the engine's actual Skia
context destructor. Build it from the engine's SDK environment (including its
private-header `CPATH`) with:

```sh
python3 tools/bench/build-skia-context-teardown.py /path/to/engine-build /path/to/deps /tmp/skia-context-teardown
```

Run the executable on Haiku with the candidate's `LIBRARY_PATH`, Mesa variables
and `SUMMIT_SKIA_GL_CONTEXT=1`, first with `healthy`, then with `failed-bind`.
The probe creates a real Skia GPU surface and checks exact RGBA
(204, 51, 102, 255). The failure case destroys only its own EGL context and
confirms that WebCore's next bind fails with `EGL_BAD_CONTEXT`. It then calls
`PlatformDisplay::clearGLContexts()`, retaining the surface until after that
call to cover delayed resource destruction.

The probe intercepts nine GL cleanup/flush entry points before Skia assembles
its GL interface. During teardown it counts and suppresses those calls to avoid
crashing in libepoxy's lazy resolver. Healthy teardown must request cleanup;
failed-bind teardown must abandon the context without those calls. This checks
Skia's response to a real bind failure, not a driver reset or compositor recovery.

On the pre-fix `5b804d7` native bundle, the healthy control passes with 14
observed cleanup calls. The failed-bind test deliberately fails with the same
14 calls, reproducing cleanup after the context can no longer bind. Logs are
retained at `.vm/issue100/recovery/skia-baseline-final-{healthy,failed-bind}.log`.

`ac13269` checks the result of `makeContextCurrent()` before releasing Skia's
GPU resources. A failed bind uses `abandonContext()` instead. On the native
candidate, the same probe passes: the failed-bind case makes **zero** observed
cleanup calls, while the healthy control still makes **14**. Both cases render
the expected pixels before teardown and abandon their retained Skia context.
The fixed logs and the audited bundle manifest are in
`.vm/issue100/recovery/`. An isolated browser using this candidate also passes
the uninstrumented GPU canvas pixel, WebGL 1/2, animation, scrolling, retained
document/text and clean-quit checks in `rendering-fallback.html`.
This fixes Skia's failed-bind cleanup; it does not
detect GPU resets or recover the compositor's shared resources, so #100 stays
open.

### Reset detection and retained GL resources, 22 October 2026 (#100)

`1376553` resolves the native reset-status function while a context is current
and checks it on subsequent binds, including the cached-current path. A reset
or failed bind retires the native share group permanently; clearing a later
reset-status response does not make that group's resources valid again.
Resources retain their context/share-group identity independently of EGL's
recyclable handles. Shader, texture, buffer and frame-target cleanup skips GL
calls after loss or when an unrelated context is current. Shared objects may
be deleted through a surviving sibling, while context-local framebuffer names
are not deleted there. Destroying a non-current context now preserves the
current sibling binding.

Compositor invalidation retires its resources on the compositor queue even if
binding fails. Its shader cache uses the retained context identity, and access
to the process-wide cache map is locked. Skia's memory-pressure cleanup also
abandons resources if binding fails.

`tools/bench/gl-resource-lifecycle.cpp` is built with the same SDK and helper as
the Skia probe, using `--source tools/bench/gl-resource-lifecycle.cpp`. Its modes
are `healthy`, `sibling`, `failed-bind`, `reset`, `foreign`, `destroyed` and
`terminated`. The reset mode substitutes the driver's reset-status entry point;
the other fault modes destroy/unbind only owned contexts or terminate only the
probe's EGL display. **No hardware reset is performed.**

Native comparison against the previous `ac13269` bundle uses its matching
headers (`--headers`), extracted from the committed engine patch. The healthy
control passes on both bundles. All six lifecycle cases fail on the previous
engine and pass on `1376553`, for **35 passing checks** on the candidate. Every
case first verifies exact framebuffer pixels and real texture, buffer and
shader objects. Healthy cleanup makes eight observed GL calls; sibling cleanup
makes seven, excluding the owner's framebuffer. The five invalid-context cases
make zero calls. The reset test checks detection, a cached function pointer,
sticky loss and propagation to siblings.

The previous Skia cleanup probes and 15-check native EGL probe still pass.
An uninstrumented browser passes **38 WebGL capture checks**, GPU canvas pixels,
animation, scrolling, two tabs, retained document/text after closing the capture
tab, and clean shutdown. A native page screenshot verifies all five fixture
colors after those context changes. The pre-existing Debugger alert for team
306157 overlaps its right side and was left untouched.

The fixture initially requested a stencil-only attachment which this driver
rejected with `vkCreateImage failed`; that unsuccessful setup is retained in
the evidence. The qualified fixture uses a color framebuffer. GL fences are
disabled in this engine configuration, so their guarded paths have no native
runtime acceptance here. These results do not establish real device-reset
recovery, worker quiescence, or a recovered browser document. **#100 remains
open for that coordinated recovery.** Logs, baseline header identities, source
audit, bundle manifest and screenshot are in `.vm/issue100/context-state/`.

Evidence, exact source/binary hashes, commands, unsuccessful trials, PNGs and
terminal driver state are pinned in
`/mnt/HaikuWork/x399/evidence/wx5100/mesa-native/summit-recovery-manifest.json`.
No system Summit default was changed; the production media add-on is restored.
