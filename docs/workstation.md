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
`AcceleratedSurface`; `LayerTreeHostHaiku` is a different path. Further work
must detect context loss, finish or cancel pending frame callbacks, retire old
GL resources without resolving functions against a lost context, clear shared
texture and Skia state, recreate the shared EGL display and compositor contexts,
and repaint the existing document. Multiple pages and worker contexts must be
quiesced before replacing shared state. A process restart or permanent software
fallback does not qualify this recovery path.

Evidence, exact source/binary hashes, commands, unsuccessful trials, PNGs and
terminal driver state are pinned in
`/mnt/HaikuWork/x399/evidence/wx5100/mesa-native/summit-recovery-manifest.json`.
No system Summit default was changed; the production media add-on is restored.

## WX5100 coordinated compositor recreation (2026-10-12)

The private candidate at
`/boot/home/wx5100/summit-compositor-recovery3/bundle` now recovers the visible
MSE page after an actual WX5100 timeout. Enable it with `SUMMIT_GPU_RECOVERY=1`.
The compiled source is `79ab203d765792d7bb20806d807178d3b0f615ba`, patch
`c9f9253e471bea8329f8653b38dfe0a1bded022641b152f9ab4a4637812f510c`, and relocated
engine `ec73bc11e0060fd615941b9a958dd5d5c48bd83ca55b8da8a7e0c67d736d1771`.
This remains a prototype on the `wx5100` branch, with the limits below.

The compositor checks reset status through a function resolved when its GL
context was created. A lost display stops composition and schedules recovery
on the main thread. Registered coordinated page hosts stop rendering, drain
their compositor queues, retire their GL resources and repaint their existing
layers after the shared EGL display and compositor contexts are recreated.
Resource owners retain the original GL display's loss state, so destroying an
old texture, buffer, shader or fence cannot delete a reused name in a new
namespace or invoke epoxy's lazy resolver against a lost context. Skia abandons
the lost main-thread context; CPU native images can be uploaded again.

QEMU caught a visible failure in the initial prototype: the JavaScript report
and decoder pixel captures passed, but the page was black after recreation.
The swap chain kept its old size, leaving the new context's viewport empty.
The final change forgets that size when retiring a context so the first new
frame reinstalls the viewport. The failed binaries, screenshots and logs are
preserved. Test-only `SUMMIT_TEST_COMPOSITOR_LOSS_AFTER_FRAMES=20` retires an
EGL display without resetting hardware; add
`SUMMIT_TEST_COMPOSITOR_LOSS_VIDEO_ONLY=1` to count video compositions and avoid
also testing the separate home-page process. These switches are absent from
the native GPU fault cases.

The corrected QEMU loss case records one retirement and one recreation. Both
decoder rounds reach EOS with 48 frames and zero drops, exact software pixel
references, a visible green completion banner and final video frame 47.
The visible video crop equals the no-loss reference byte for byte. One initial
no-loss run of this candidate logged a shutdown allocator error, `bogus pointer
(double free?)`, without a usable crash report. Two subsequent no-loss runs and
the loss case shut down cleanly. The error is retained as unresolved; passing
repeats do not establish its cause or eliminate it.

Native tests use driver `a4aebce1...`, Mesa `42352938...`, and test-only add-on
`eb1d8fdb...`. A timeout after 19 decoded Main10 pictures is exercised at both
readback and clear. Each case produces exactly one automatic kernel reset,
one shared-display/compositor recreation and two AMD decoder selections in
the existing document. Readback reports 48/7 frames/drops in the software
recovery round and 48/0 in the fresh hardware round; clear reports 48/19 and
48/0. Both rounds reach EOS, with exact independent software/hardware PNG
references. Both visible final video crops exactly equal a fresh no-reset
GPU-composited reference. That reference also completes both hardware rounds
at 48/0. The AMD Mesa backend reopens after loss; the WebProcess is not restarted.
All three native cases quit normally without crashes or owned leftovers.

Fresh EGL, GLES and window probes add exactly 116 completed GPU jobs after
the application cases. Root VRAM and ordinary-user RAM DMA pass too. Terminal
state is generation 27, automatic recovery enabled with 24 attempts and 24
successes, no pending recovery, no live or quarantined resources, and 628
completed/waited fences with 1,256 EOP events. The interrupt ring is empty and
all interrupt error counters are zero. Production media `3f28945c...` is restored.

Recovery remains opt-in. Worker Skia contexts currently defer display replacement
until they retire; explicit worker quiescing is unfinished. GPU canvas surfaces,
WebGL/ANGLE, noncomposited GPU rendering, paused YUV layer restoration, multiple
pages and late frame acknowledgements across recovery still need implementation
or qualification. The QEMU shutdown allocator error also needs diagnosis. This
checkpoint does not enable system defaults or complete the OS/application
release installation. Exact sources, binaries, failed and passing trials,
screenshots, source-manifest history and final diagnostics are pinned in
`/mnt/HaikuWork/x399/evidence/wx5100/mesa-native/summit-compositor-recovery-manifest.json`.

## WX5100 frame acknowledgements across recovery (2026-10-12)

The next private candidate is `/boot/home/wx5100/summit-frame-ack/bundle`,
compiled from `748f12a5ce0beb42140bed5d365f8512dc4e433a` with patch
`fd27619606f34f7d424808362c2a3e1a2be502abd5ae4aea0ae82f23bb48583c`.
Its relocated engine is
`fb4b02191a842ed2e27ba07ebe7eaf6c8ed1148535bdb9f43df01506b2111a9f`.
Recovery remains opt-in with `SUMMIT_GPU_RECOVERY=1`.

Haiku frame and completion IPC now carries a nonzero serial. Retiring the GL
context clears the pending serial but preserves the counter. A completion
only releases the compositor's wait when it matches the current pending
frame, so an acknowledgement from before recovery cannot complete a newer
frame. UI callbacks capture the serial of the frame they presented. Other
ports retain their existing IPC signatures. The first native build exposed
the IPC generator's lack of `#else` support; complementary `#if` conditions
fixed that build failure, whose inputs and logs are preserved.

Test-only `SUMMIT_TEST_REPLAY_FRAME_ACK=1` resends the previous acknowledgement
before completing a current frame. With `SUMMIT_SWAPCHAIN_TRACE=1`, QEMU
records 358 rejected stale acknowledgements in ordinary playback and 321 in
the display-loss case. The loss case recreates the display once and proves
the serial counter continues across recreation. Baseline, replay and loss
each complete both decoder rounds at 48 frames and zero drops, with exact
software pixel references and exact visible video/completion-banner crops.
All three quit cleanly without crash reports or owned leftovers.

The native Main10 readback test combines acknowledgement replay with one
actual test-only GPU timeout after 19 pictures. Automatic recovery advances
generation 27 to 28 and succeeds for the 25th time in 25 attempts. The same
WebProcess recreates its display once; its last observed pre-recovery pending
serial is 21 and its first post-recovery pending serial is 22. The log records
92 stale acknowledgements before recreation and 238 afterwards across the
browser's processes. The interrupted round completes 48 frames with 20 drops
and exact software pixels; the fresh hardware decoder completes 48 frames
with zero drops and exact hardware pixels. Both PNGs match the independently
qualified references, and visible final video frame 47 exactly matches the
prior no-reset GPU-composited reference. Shutdown is clean.

Fresh EGL, GLES and window checks add exactly 116 completed/waited GPU jobs,
514 to 630. Root VRAM and ordinary-user RAM DMA checks pass. Terminal state is
generation 28, automatic recovery enabled with 25 attempts and 25 successes,
zero live or quarantined resources, 630 completed/waited fences, 1,260 EOP
events and 795 interrupts. The ring is empty at `0x4ec0`; all interrupt error
counters are zero. Production media `3f28945c...` is restored and the desktop
remains intact. Driver, Mesa and original browser backups remain unchanged.

`GLContext::makeContextCurrent` also records `EGL_CONTEXT_LOST` when binding
a context fails, instead of relying on a reset-status query that cannot run
without a current context. This specific EGL error-return branch has not yet
been exercised; the recovery tests above do not establish its coverage.
Worker coordination, GPU canvas and WebGL/ANGLE restoration, noncomposited
GPU rendering, paused YUV contents, multiple pages and repeated losses still
need work. The prior intermittent QEMU shutdown allocator error remains open.
A private `debugger()` preload records stack addresses and loaded modules
before forwarding the original call. Its intentional QEMU double-free test
captures eight stack entries; both bare and preloaded tests exit 1. The first
self-test incorrectly expected exit 255; its failed expectation is preserved.
The helper is for named diagnostic runs only and changes no system defaults.
Three further QEMU playback/shutdown runs of the previous compositor candidate
with that recorder complete cleanly. A live module listing confirms it is
loaded in Summit, WebProcess and NetworkProcess. No allocator failure or stack
is captured in those browser runs; the original error is not considered fixed.

Exact sources, binaries, unsuccessful and successful trials, screenshots,
metadata history and terminal diagnostics are pinned in
`/mnt/HaikuWork/x399/evidence/wx5100/mesa-native/summit-frame-ack-manifest.json`.
This is a private recovery checkpoint, not the final OS/application release
or completion of full WX5100 acceleration.
