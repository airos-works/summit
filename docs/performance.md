# Summit performance: Speedometer 3.1 baseline, where the time goes, stress test

## 13 October 2026: the Radxa Cubie A7S

The Cubie A7S (Allwinner A733: two Cortex-A76 at 2.0 GHz, six Cortex-A55 at
1.8 GHz, an Imagination BXM-4-64 GPU through zink, 1920x1080, no fan, an SD
card) runs air/OS with Summit from the image. Measured on the board through
its MCP service, with engines built in the CI server's own arm64 tree (about
a minute for a few files) and run from `/boot/home/sw` beside the installed
packages. Baseline: the CI build of `ce257e0`.

| | before | after |
|---|---|---|
| Speedometer 3.1 (3 iterations) | 1.65 | 2.60 |
| short JS + DOM benchmark (ms) | 97-244 / 145-354 | 85-109 / 145-164 |
| Wikipedia, 1920x1056, wheel at 100 Hz | 26-31 fps | 37-41 fps |
| frames slower than 50 ms per scroll burst | 6 | 0-2 |
| 1080p H.264 video, maximized window | 22 fps | 30 fps (all) |
| YouTube watch page (480p VP9), compositor frame | 628 ms | 17-18 ms (30 fps) |
| scrolling Reddit (r/pics), wheel at 100 Hz | 3-4 fps | 8-16 fps |
| scrolling BBC News, wheel at 100 Hz | 15-18 fps | 25-35 fps |
| a caret blinking in a focused text field, per blink | 42-48 ms, 1.8 Mpx | 1-2 ms, nothing drawn |
| YouTube (Big Buck Bunny), what it plays | 480p30 VP9, 1.1 cores | 720p60 H.264, 0.9-1.0 cores |
| YouTube watch page maximized, drawn per video frame | 2.3-2.45 Mpx | 1.5-1.6 Mpx |

What changed:

- **Fast cores** (`045ec98`). Haiku's scheduler treats all cores alike, so a
  page's main thread ran on an A55 most of the time, and the core a launch
  happened to get decided the speed (2.2 times; 2.6 without the JIT).
  `PerformanceCoresHaiku.h` gives the web process's main thread and the
  compositing thread an affinity for the cores whose MIDR is not one of ARM's
  little designs, where a SoC has both kinds (also the RK3588 of the ROCK 5).
  `SUMMIT_PERFORMANCE_CORES=0` turns it off. During an 8 s load of the
  Guardian the main thread used 6.5 s of CPU, the network process 2.4 s over
  several threads and the browser 0.25 s, so only the web process is pinned.
- **No periodic read-back while app_server's copy is written** (`7ba7204`,
  issue #50). With `SUMMIT_DIRECT_PRESENT_BACK=1` (the Cubie image sets it)
  each direct present also writes app_server's copy, and the frames that went
  through app_server every 250 ms only refreshed the view's bitmap, each with
  a 42-100 ms blocking read. One frame still goes through app_server once the
  page has been quiet for 250 ms.
- **No page background under the tiles that cover it** (`da79bad`). Haiku's
  `paintSelf` draws the root layer's background color under its tiles (issue
  #13); a scroll frame drew 3.8 Mpx for a 1.75 Mpx view. Now 1.71.
- **Tiles without the per-pixel color matrix** (`dafa2ba`). The TextureRGB
  shader multiplied every pixel by a 4x4 uniform matrix that is the identity,
  or swaps red and blue for Skia's BGRA tiles. On this GPU that about doubled
  a pixel's cost (the GPU team's `pvr_glcomposite`: 27 ns against 12-14 for a
  plain texture). Two shader options sample plainly or with `.bgra`.
- **Occlusion** (`cab3d62`). Before a composition is painted, opaque contents
  that go straight into the target (a video frame, an opaque solid color) are
  noted with their place in the paint order, and tiles painted before them
  that they cover are left out. A maximized 1080p video drew 3.26 Mpx in 34
  draws per frame, now 2.10 in 14. `SUMMIT_OCCLUSION=0` turns it off.
- **Filter layers drawn again only when something reaches them** (`07520b0`).
  Every layer with a filter or mask was damaged in every composition; the
  "changed" mode measured as harmless on the X399 is now the default
  (`SUMMIT_FILTER_LAYER_DAMAGE=always` restores the old one). The same commit
  stops a page whose frames take over 250 ms from having every other frame
  read back as "quiet" (only the quiet-page timer's frame goes through
  app_server now).
- **Empty filtered layers skipped** (`03b5665`). YouTube's player has an empty
  blurred layer (its ambient light in the light theme) that went through
  eight surfaces and two blur passes with each video frame.
- **The rounded-clip shader only in the corners** (`71610b4`). Under a
  rounded clip every pixel ran the RoundedRectClip shader (about 100 ns a
  pixel here); YouTube's player has rounded corners, so its video took
  ~100 ms of GPU per frame. With one axis-aligned rounded clip a draw is made
  in parts that tile the clip without overlapping: the plus-shaped middle with
  the scissor alone, the four corner boxes with the shader. Removing the
  page's border-radius by script gave the same 116 -> 17 ms.
- **Layer contents uploaded before drawing starts** (`f754dbd`). A contents
  picture (scrollbar, video planes) was uploaded mid-composition, and zink
  then waited for the GPU to finish everything queued: Reddit's 21x913
  scrollbar held each scrolled frame 100-230 ms in a 0.1 ms upload (a
  `glFinish()` placed before it took the time instead).
- **Filter results reused while a layer only moves** (`9df3984`). Reddit
  shows a dimmed, blurred copy of each picture behind it; every scrolled
  frame blurred them all again (nine surfaces). The filtered result of a
  layer of its own is kept and drawn moved by whole pixels while its scale,
  filters, colors, contents and backing store (now versioned) are the same.
  `SUMMIT_FILTER_CACHE=0` turns it off.
- **Network cache** (`ac00e3a`). A record cut short by a power loss (the
  meta data checksum covers only the meta data) aborted the network process
  in `readRecordFromData`; it is now discarded.
- **A page's last frame through app_server draws and reads only what
  changed** (`eb296c6`). That frame (sent a quarter of a second after a page
  stops changing) redrew and read back the whole page even when only a caret
  had blinked: an empty damage record took `TextureMapper::beginClip` to the
  stencil, which drew every layer against it, and the shared bitmap, having
  missed the frames that went straight to the screen, was read in full. A
  target that is already current is no longer painted; each shared-memory
  target keeps what changed since its bitmap was last read and reads only
  that; app_server is told only what the direct frames changed. A build that
  read the whole frame after each partial read found the bitmap identical in
  643 reads (direct present on and off).
- **ARM boards decline VP9 and AV1 to Media Source Extensions** (`29428cd`).
  The boards decode H.264 with their own decoder (rpi_mmal, sunxi_cedar) or,
  without one, in software for a fraction of what VP9 and AV1 cost. Offered
  all three, YouTube took VP9 and stayed at 480p30; offered H.264 it plays
  720p60 (0.6-8 % of frames dropped). x86_64 is unchanged;
  `SUMMIT_MEDIA_PREFER_H264=0` offers VP9 and AV1 again, `=1` declines them
  on x86_64 too.
- **Rounded clips occlude** (`8b67d98`). Opaque contents under a rounded clip
  (YouTube's player rounds its corners) hid nothing, so every video frame
  drew the page under the video too. The cross of a rounded rectangle's
  middle bands is now an occluder.

Tried and dropped: setting libgcc's `__aarch64_have_lse_atomics` so the
out-of-line atomic helpers use the LSE instructions (libgcc never sets it on
Haiku, which has no `getauxval`); Speedometer went 2.63 -> 2.39 over two
alternated 10-iteration runs. Pinning every web-process thread to the two A76
cores (not measured: the board hung at that launch). Waiting a second instead
of 250 ms before the settle read-back (no measurable difference). A quarantine
for textures just returned to `BitmapTexturePool` and re-specifying a texture
on whole updates (neither removed the upload stall; uploading earlier did).
Four Skia painting threads instead of three (no gain). Drawing only the
uncovered parts of tiles that opaque contents cover in part (on YouTube 5 %
fewer pixels without the rounded-clip occluders, 40 % with them, but the
GPU took longer per frame than with whole tiles). Reporting 60 frames a second
above 640x360 as not smooth in media capabilities: YouTube honours it, but a
video offered at 60 above 480p (Big Buck Bunny) then plays at 480p30 (1.0
cores against 1.8); left as a choice for later.

Ways of measuring that held up: `SUMMIT_COMPOSITOR_TIMING_TRACE=1` (per frame:
draws, drawn and blended Mpx, surfaces, blur passes, paint and GPU-wait time),
`SUMMIT_LAYER_PAINT_TRACE=1` (layer branches over 10 ms), hiding parts of a
page through `summitctl devtools-do evaluate` in one session, and local
fixtures that copy a page's structure (video behind a blurred layer, VP9 MSE
with `object-fit: cover`). `profile -a` hung the board twice while video
played, even with direct present off.

Where the time still goes: the GPU. A probe drawing into a 1920x1080
target through the same EGL/zink stack fills about 5 ns a pixel with a solid
color and 9-10 ns with a texture (12 ns blended), whatever the filtering,
precision or target, plus 0.03-0.1 ms a draw; the GPU runs at a fixed
600 MHz. That is about 0.35 pixels a clock where a BXM-4-64 should do 4, so
one full-window layer costs about 20 ms and 60 frames a second at 1080p is
out of reach whatever Summit does; Summit can only draw fewer pixels. Reddit
draws 4-6 Mpx a scrolled frame (the page, a blurred 30 % copy of each
picture, and three transparent layers stacked over each picture), BBC
2.1-3.1. Page loads are main-thread work (JS parse and run, style, layout,
malloc); about 5 % of the main thread's samples are libgcc's out-of-line
atomics. Idle pages cost nothing (Wikipedia: no frames, 0.002 cores).

YouTube's `getVideoPlaybackQuality()` counts frames decoded, not composited:
720p60 reported 60 frames a second while the maximized watch page composited
18-25 (now about 25 at 20 ms of GPU each). The board heat-soaks at 76-78 C
during tests and its throttling levels change the GPU and CPU clocks, so
frame times varied two- to threefold between runs of one build; drawn
megapixels and alternated runs are the comparisons that held.

Intermediate surfaces cost far more than their pixels here: each one seems
to make the GPU finish the work queued before it. In some YouTube sessions
(6-25 of the frames in a 30 s run, none in others) five 41-pixel-high
buttons under the player that carry a CSS filter were filtered again with
each rendering update for the first 10-20 s: 40 surfaces and 10 blur passes
for 3.5 Mpx, and 300-900 ms a frame, while the video stood still. The filter
cache keeps such a layer only while its backing store is complete and
unchanged; why those buttons kept changing could not be traced before the
state stopped reproducing.

The CI build of `8b67d98` is installed on the board
(`summit-0.1.0~git20261013.1516`, `summit_webkit-1.10.1~git20261013.1516`;
earlier packages are in `/boot/home/summit-package-backup`) and in
`/mnt/HaikuWork/cubie/image-packages` for the next image.

Found on the OS side (not changed here):

- The board hung hard (no ping) four times in about 105 Summit launches,
  each within about a second of a launch. Not CPU load or heat (an 8-core
  spin stays at 79 C under throttling), not frequency changes
  (`dvfs_stress`), not direct present (30 launches without it and 30 with
  it, no hang), not the test driver's polling (20 launches). The serial
  console has carried garbage since 12 October and the NanoKVM shows no
  picture, so there is no KDL output yet.
- The SD card runs at SDR25 (25 MB/s): the first launch after a boot took
  8.4 s against 1.2 s warm.
- The little cluster stays at 1794 MHz while idle (the thermal driver reads
  its load as 45-95 %), and the board idles at 60-77 C with throttling from
  80 C; during these tests it reached level 2 (big cores 1612 MHz, GPU
  400 MHz).
- libgcc's LSE detection is compiled out for Haiku, so every C/C++ atomic on
  arm64 takes the exclusive-loop path through a call.
- `pvr_drm.c`: "Core count fetching is unimplemented".
- With zink on this PowerVR, a texture upload after a composition's first
  draws waits for the GPU to finish all of them, and each intermediate
  surface is a render pass of its own (a blur costs several).
- `profile -a` hangs the board while video plays.
- The GPU fill rate above (pvr through zink, 600 MHz, the top of
  `sunxi_thermal`'s table): the largest single cost on this board.
- The packages are zlib-compressed (`summit_webkit`: 236 MB in 85); zstd
  decompresses several times faster and the Cubie's packagefs has it, but
  every image's packagefs would need it before the CI (airos-ci) switches.
- OpenSSL uses the ARMv8 crypto instructions (AES-GCM 700 MB/s on an A55,
  1.5 GB/s on an A76), so TLS is not a cost.

## 9 October 2026: latest ARM64 build installed on the Pi

At the owner's request, the tested performance candidate from Summit
`918dcbd` was installed as `summit-0.1.0~git20261009.0135-1` and
`summit_webkit-1.10.1~git20261009.0135-1`. It uses WebKit `fb054d09146b`
with Haiku patch `013447c0317e`. WebExtensions and AVIF remain disabled.
The engine's cached runtime revision string still reports `00991b6c`;
the package provenance and binary hashes identify the actual source/build.

The packages preserve the native-tested binaries, with library paths adjusted
for the installed layout and application resources retained. Both extracted
packages were verified before upload; all **36 installed ELF hashes** match
the package manifest. `pkgman` upgraded only Summit and its engine. Copies
of the previous packages are retained in
`/boot/home/summit-deploy-20261009/rollback/`.

Launching `/boot/system/apps/Summit` with a fresh test profile and no special
environment passes the 300-frame scroll fixture at **50.27 fps**, and the
720p MSE fixture presents **360 frames with zero drops**. Both runs quit
normally, leave no helper processes and pass all **18 health checks**.
Evidence: `.vm/performance-12h-20261008/deploy-pi-20261009/`,
`pi-probe-20261009-123648-installed-default-scroll/` and
`pi-probe-20261009-123720-installed-default-video/` under the same session
directory. The normal Deskbar entry resolves to the installed application.

## 8 October 2026: twelve-hour performance and hardening session

This session ran from **09:35:55 to 21:36 UTC**, after the separate six-hour
pass below. Its baseline is installed `bundle-tmeeg8cb`, patch `b178f34d…`,
with Mesa `prefix-20261002`. Evidence is under
`.vm/performance-12h-20261008/`.

The measured Pi default-scroll fixture improves from **39.16 to 50.785 fps**
(29.7%); X399 scrolling remains near 60 fps, and neither platform's short
Speedometer comparison establishes a gain. The committed fixes repair missing
readback pixels, video timestamps and final-frame draining, ARM64 build
compatibility, and a separately reproduced Haiku loader startup race. The final
1,528-action stress run and native media, graphics and UI checks pass. Scope,
failed diagnostic attempts and measurement limits are recorded below.

The initial 400-card scroll probe ran 900 frames at **59.59 fps**, with an
18 ms p99 and 22 ms maximum interval, in a 1661 × 798 viewport at DPR 2.
It was uncontended and quit normally without leftover helpers or crash events.
This measures script-driven scroll callbacks, not native wheel presentation.
Evidence: `.vm/bench/probe-20261008-203810-summit-perf12h-scroll-baseline/`.
All five host CTest suites and ten Python unit tests also passed.

The first renderer defect is in partial-frame readback. When a wide damaged
rectangle becomes a full-width read band, the helper also appends uncovered
narrow rectangles to its output. It subsequently treats those narrow rectangles
as full-width bands: another rectangle at the same vertical position can be
discarded despite lying on the opposite side of the frame. The fix limits that
vertical-only coverage decision to actual full-width bands.

`tools/test-engine-readback-rects.py` compiles the two production geometry
helpers verbatim with native WebCore rectangle types and a frozen engine.
The original fails the specific separated-rectangle case and 13,093 checks
overall; the candidate passes all **996,205** coverage, bounds and read-count
checks across 12,000 seeded cases. This is geometry verification, not a GPU test.
Evidence: `.vm/readback-rects-20261008-{204223-before,204328-after}/`.

The browser reproduction changes three composited rectangles through unique
colors, ending in green. With shared-bitmap presentation, the original leaves
the right rectangle red while the other two turn green. The reusable
`tools/bench/run-readback-damage.py` checks actual screenshot pixels, since
correct DOM styles cannot establish that a frame reached the screen.
Baseline evidence:
`.vm/bench/probe-20261008-204702-summit-perf12h-damage-baseline/`.
An earlier two-color fixture masked the defect because stale swap-chain buffers
could already contain the final color; that fixture was corrected.

Candidate **`bundle-ktyzor6m`**, patch `af560d77…`, passes the screenshot test
with ordinary shared-bitmap reads, 384 KiB banded reads, and direct presentation.
All three quit normally without leftover helpers or crash events. Its retained
graphics regression also passes all **68 checks**, including WebGL 1/2 state,
canvas clipping, updates while hidden, resizing and exact restored pixels.
The repeated 900-frame scroll probe measures **59.63 fps**, 19 ms p99 and a
20 ms maximum, uncontended and clean; this is consistent with the baseline,
not an established speed improvement. This change repairs omitted pixels
without changing readback thresholds or adding framebuffer allocations.
Evidence: `.vm/performance-12h-20261008/readback-validation.json`.

The 384 KiB run exercises the banding strategy used by V3D on the Pi but is
still an X399 test. A read-only check also confirmed that the actual Pi was
reachable at its current DHCP address, running arm64 hrev60206+750. Subsequent
private-bundle validation on that device is recorded below.

The candidate then passed all **56 native streaming cases**, **50 PCM seek
checks**, **five video-timing checks** and exact Opus trimming, with normal
browser quit, no audio-output errors, no crash/debugger events and no leftover
helpers. Evidence: `.vm/performance-12h-20261008/streaming-readback/`.
This validates the candidate with the current media services; it does not
resolve the intermittent system audio stall reported in the earlier session.
The normal installed launcher still points to the baseline while subsequent
performance work continues with isolated bundles.

Native wheel checks now verify root scroll positions and optionally capture
each burst after its timing interval. An initial real-site sweep included a
consent overlay, an empty GitHub issues page and YouTube's empty home page;
those results do not establish scrolling performance. With loaded, scrollable
pages, four 60-notch bursts at 25 ms intervals give these ranges on X399:

| Page | Native view FPS | Worst interframe gap across bursts |
| --- | ---: | ---: |
| Wikipedia, Haiku article | 58.21–59.55 | 35.3 ms |
| Reddit, popular feed | 57.35–58.44 | 45.1 ms |
| GitHub, WebKit repository | 58.98–60.03 | 25.5 ms |
| YouTube, watch page | 55.61–58.30 | 43.0 ms |

Every burst moved the root scroll position; intermediate screenshots confirm
changed content. Native frame counts can include video or other animation,
and are not physical display-refresh measurements. All four runs quit cleanly
without crash events or leftover helpers. Evidence:
`.vm/performance-12h-20261008/real-wheel-verified.json`.
The negative control on `about:blank` correctly reports `no-scroll-movement`
and exits with status 1 after normal teardown:
`.vm/bench/scroll-cycles-20261008-211823-perf12h-wheel-negative/`.

The physical Pi's installed engine uses upstream `00991b6c`, an older revision
than X399. Its 400-card probe at 1661×798, DPR 1 measures **37.60 fps** over
600 scroll frames, with 35 ms p99, normal teardown and no crashes. Readback
costs roughly 19 ms of a 26 ms frame. This is a separate device baseline;
its resolution, software and CPU differ from the X399 measurements.

The image's newer Mesa supports GPU-assisted readback, but the installed
Summit still selects the earlier CPU-oriented 384 KiB bands. Alternating
explicit 384 KiB and whole-rectangle runs gives **36.81–37.13 fps** versus
**47.56–48.14 fps**, respectively. Whole reads therefore improve this fixture
by about 29%; 128 KiB bands are slower at 34.89 fps. A 1 MiB-band run suffered
a collection timeout after the browser quit; recovery found its completed
36.55 fps result, no leftover processes and no crash. It remains separately
marked as a collection failure. Evidence:
`.vm/performance-12h-20261008/pi-band-sweep.json` and its per-run artifacts.

The main engine now consumes Mesa's optional `haiku_mesa_readback_band_bytes`
hint, already present in the separate arm64 checkout. Explicit overrides
retain priority; an absent or negative hint retains the old renderer-specific
fallback. The extracted production helper chooses whole reads on the actual
Pi, 59-row bands with GPU readback disabled, and respects both explicit
overrides. All four native EGL cases verify **21,207,648 pixels each** with
no mismatch. The reusable probe is `tools/pi/probes/readback-policy.cpp`;
evidence: `.vm/performance-12h-20261008/pi-readback-policy-results.json`.
The X399 candidate, `bundle-no8kj8a0`, passes separated-damage screenshots
with shared and direct presentation. Its 900-frame scroll repeat measures
59.65 fps, 19 ms p99 and 20 ms maximum, with no interval over 33 ms. These
checks quit normally without crashes or leftover helpers. Evidence:
`.vm/performance-12h-20261008/readback-hint-validation.json`.
The whole-read speedup above was measured using the installed Pi engine's
explicit override. Results from the subsequently completed ARM64 build are
recorded below.

The current upstream engine needed three arm64 adaptations that previously
lived only in the separate cross-build checkout: Haiku signal-context register
access, ELF GOT references in the ARM64 interpreter, and instruction-cache
maintenance through `clear_caches`. Those changes are now in the main port
patch. The newly built JavaScriptCore runs on the actual Pi and passes all
16 smoke checks with its default JIT settings, 15 with JIT disabled, and 16
with eager synchronous optimization. These include ICU, async functions, hot
JavaScript and, in both JIT runs, WebAssembly. This is a port smoke check,
not JSC conformance or testing on heterogeneous ARM cores. Evidence:
`.vm/performance-12h-20261008/pi-current-jsc-results.json`.

The full arm64 browser build is isolated from the existing cross-build
checkout and its uncommitted changes. Its first attempt exceeded the build
scope's memory limit; it resumed incrementally with six compiler jobs.

The installed Pi engine scores **1.593** on a three-iteration Speedometer 3.1
diagnostic with whole reads forced (individual scores 1.440, 1.653, 1.686).
This short run has wide variation and is only a baseline for the forthcoming
current-engine comparison. Its original harness marked a network helper
present immediately after browser exit; a subsequent audit found it gone.
The harness now observes up to five seconds of asynchronous helper teardown.
Evidence: `.vm/performance-12h-20261008/pi-probe-20261008-213334-installed-speedometer-whole/`.

Media checks also expose the age of the installed Pi engine. A 360-frame
720p H.264 fixture finishes but reports 370–376 total frames; a freshly encoded
360-frame 1080p clip reports 392 total frames, including 42 dropped frames.
The newer decoder code already prevents replaying samples during initial
decoder setup. The current X399 engine reports exactly 360 frames and zero
drops on the 720p control. These are decoder counters, not proof that every
frame reached the physical display. Native Pi validation of the newer browser
and further decoder fixes are recorded below.

`mse.html` now records display dimensions, supports an explicit display width,
and optionally checks a caller-supplied expected frame count at normal end.
`expectedFrames=360` passes on X399 and rejects the installed Pi engine's
370-frame run. Evidence:
`.vm/bench/probe-20261008-214734-summit-perf12h-mse-expected-current/` and
`.vm/performance-12h-20261008/pi-probe-20261008-214811-installed-mse-expected/`.
An earlier remuxed 1080p input also stalled near its end; its edit-list and
duration differences make it a separate case from the fresh 12-second encode.
Fixture hashes and metadata are in
`.vm/performance-12h-20261008/media-fixtures.json`.

The current ARM64 JavaScriptCore also passes **39 upstream stress cases** on
the physical Pi: 13 selected tests under default, interpreter-only and eager
JIT modes, with each test's required options preserved. These cover atomic
operations, BigInt, ARM64 condition chains, overflow recovery, OSR materialization,
regular-expression bounds and GC. The run has no crash events and the device
health check passes. This selected set is not the complete upstream suite.
Evidence: `.vm/performance-12h-20261008/pi-jsc-stress-results.json`.

The full isolated ARM64 engine and CMake browser now link. The build exposed
an extension-disabled configuration error: package snapshot code referenced
archive helpers that are compiled only with WebExtensions enabled. Its source
and header now use the same feature gate. The browser's CMake target also
omitted five existing UI sources; it now matches the native build script's
source list. All five host CTest suites pass after the changes. The old ARM64
SDK lacks the current Bluetooth ATT constant, so this isolated build supplies
its unchanged native value (`0x0004`) only to `BluetoothSessionHaiku.cpp`.
No shared SDK or installed OS files were changed by this build. Runtime
validation of the private browser bundle is recorded below.

A three-round X399 page-load comparison collected Wikipedia, GitHub, BBC and
Reddit timings for Summit and Firefox. It is diagnostic, not a matched speed
claim: Summit uses the display's DPR 2 while Firefox reports DPR 1, viewport
heights differ, and donation banners/feed content vary. Separate DOM and
screenshot checks confirm loaded visible images in both browsers. The audit
also caught one Summit WebProcess startup crash inside Haiku's library loader
while loading Mesa, concurrent with media-type preparation and an IPC thread's
first TLS allocation. The subsequent loader investigation is recorded below;
these page-load runs are not a clean stability pass. Evidence:
`.vm/bench/pageload-20261008-215158/`,
`.vm/performance-12h-20261008/{content-verify2,content-verify3,pageload-monitor.json,crashes}/`.

The current ARM64 engine and application now run from private Pi bundles;
the installed Summit application is unchanged. This build has WebExtensions
disabled, so it does not establish feature parity with the extension-enabled
X399 build. All five native ARM64 application suites pass **610 checks**.
The first upload attempt stopped on a connection timeout after three suites;
the two remaining suites passed on a verified retry. Evidence:
`.vm/performance-12h-20261008/pi-arm64-core-tests.json`.

The new Pi browser removed the installed engine's duplicated video frames,
but exposed two additional end-of-stream faults. MMAL's last two pictures
arrived after the playback clock stopped, leaving their presentation wait
blocked even though they were already due. Media Kit's H.264 fallback returned
the last input packet's timestamp, including invalid values during draining.
The candidate allows already-due pictures through a stopped clock and reuses
the existing input presentation-time queue for H.264 through Media Kit.

Private `pi-bundle-timeline`, engine patch `013447c0…`, passes the following
on the physical Pi. Both traced 720p paths produce **360 distinct, sorted
presentation timestamps**, from 66,666 through 12,033,333 microseconds.

| Playback case | Hardware total / dropped | Software total / dropped |
| --- | ---: | ---: |
| 720p, video only | 360 / 0 | 360 / 0 |
| 1080p, video only | 360 / 0 | 360 / 1 |
| 720p, 100 ms page-thread work each second | 360 / 0 | 360 / 0 |
| 720p, AAC audio | 360 / 0 | 360 / 0 |
| 720p, seek to 8 seconds after 3 seconds | 211 / 0 | 215 / 0 |

All runs end without decode errors, quit normally, leave no helpers, and pass
the device health check. Seek counts differ because the seek is triggered by
a periodic page callback; they are not full-clip frame-count comparisons.
Audio runs are muted and establish decoder/clock behavior, not audible output.
Counters do not establish physical display delivery. Evidence:
`.vm/performance-12h-20261008/{pi-timeline-validation.json,pi-timeline-pts.json}`.
The same patch is now frozen in X399 `bundle-hbgpfqma`. Its full native
streaming regression passes **56 cases**, **21 video-timestamp checks**,
**50 PCM seek checks** and exact Opus trimming. The additional H.264 timing
checks cover rollover, discontinuities, abort/reuse and stream reopening.
All **68 graphics lifetime checks** and both shared/direct separated-damage
screenshot checks also pass. Each run quits normally without crash events or
remaining helpers; streaming reports no audio-output errors. Separately, the
native codec driver passes 54 decode runs across 30 valid fixtures; malformed
inputs are covered by their expected-error cases in the browser suite.
Evidence: `.vm/performance-12h-20261008/{streaming-media-timeline,codecs-media-timeline-valid}/`
and `x399-media-qualification.json` in the same directory.

X399 NVDEC version 2 also completes the 720p and 1080p 360-frame probes with
zero drops, plus stable pause/resume and paused-seek pictures. Its name
deliberately bypasses the old NVDEC reference-limit override, so attempted
software-override browser runs are annotated as additional hardware coverage.
The native codec driver now has an explicit `--software-h264` option; it
passes the same 30 files / 54 decode checks, with all nine H.264 cases selecting
Media Kit's `h264` software decoder and preserving exact presentation order.
This verifies native software decoding, while the Pi checks above also cover
software playback through the browser's clock. Evidence:
`.vm/performance-12h-20261008/{x399-playback-validation.json,codecs-forced-software/result.json}`.

The final X399 ABBA comparison uses fresh profiles, 1661×798 at DPR 2 and no
detected contention. Three-iteration Speedometer scores are **10.360 / 10.136**
for baseline and **10.510 / 10.314** for current; their wide confidence
intervals overlap. The 900-frame scrolling runs are **59.61 / 59.61 fps** and
**59.64 / 59.65 fps**, respectively, with no interval over 33 ms and at most
23 ms in any run. Neither establishes an X399 speed gain. All runs quit
normally without crash events or leftover helpers. The final bundle also
passes the **145-check native close suite**. Evidence:
`.vm/performance-12h-20261008/x399-matched-performance.json` and
`.vm/modern-close-889f3ab7df05ea57c29a1466/`.

The final **44.9-minute stress run** completes **1,528 actions** with seed
100809 and up to eight tabs: 400 opens, 395 closes, 348 navigations, 148 tab
selections, 155 history operations and 82 reloads. It reports no command/load
failures, timeouts, unexpected renderer exits, crash reports or debugger
events. Returning to the starting page and settling for 30 seconds leaves
473.0 MiB of aggregate resident areas, versus 300.4 MiB at cold start. These
totals include shared mappings; this result does not establish memory
neutrality. Normal browser quit leaves no helper processes. Evidence:
`.vm/bench/stress-20261009-074822-perf12h-final-long/`.

The tested Summit bundles remain private: the X399 desktop launcher still
selects baseline `bundle-tmeeg8cb`, and the installed Pi application is
unchanged. The final X399 candidate is `bundle-hbgpfqma`; the Pi media candidate
is `pi-bundle-timeline` and has WebExtensions disabled. The separately tested
X399 loader package 1.1 remains active. The final environment audit confirms
no remaining candidate processes and that the restored test VM is off.
Evidence: `.vm/performance-12h-20261008/final-environment-audit.json`.

The reusable MSE fixture now also checks pause/resume and seeking while paused.
On both Pi decoder paths, a normal pause holds the clock, frame count and
sampled pixels fixed, then resumes through all 360 frames. A paused seek to
eight seconds produces its new picture after 447–464 ms and then holds a
nonblank picture exactly until resume. The seek promise currently resolves
when samples are queued, before this picture arrives; the fixture records
arrival separately. Its first 250 ms snapshot therefore caught the single
target-picture update and failed, rather than establishing unwanted playback
while paused. This existing seek-notification behavior remains unchanged.
Evidence: `.vm/performance-12h-20261008/pi-probe-20261009-070009-pause-seek-picture-hardware/`
and `pi-probe-20261009-070338-pause-seek-picture-software/` in the same directory.

A matched Pi Speedometer ABBA comparison, three iterations per fresh profile,
scores **1.617 and 1.676** for installed Summit and **1.661 and 1.729** for the
private current build. Both use whole reads, 1661×798 at DPR 1, the same OS
boot and the same driver/library versions. The wide confidence intervals
overlap; this is a successful responsiveness regression check, not evidence
of a significant speed improvement. The shared Pi performance controller
updated OS components during this session, so earlier measurements are kept
separate. These runs use its accepted bitmap-pool app_server `59055941…`;
the roster path and unchanged boot time were verified after the four runs.
The harness now captures the actual app_server path before future tests.
Evidence: `.vm/performance-12h-20261008/pi-speedometer-comparison.json`.

A separate default-settings ABBA scroll comparison completes 900 frames per
run: installed **39.52 / 38.80 fps**, current **50.71 / 50.86 fps**. The means
are 39.16 and 50.785 fps, a **29.7%** improvement in this fixture. All four
runs share the 19:23:15 UTC boot, app_server `59055941…`, and identical
library/driver hashes. That boot was the other controller's kernel trial;
its packaged version still reported hrev750, so this comparison is kept
separate from earlier measurements. On the earlier original-kernel boot,
forcing whole reads in installed Summit gives 52.16 / 52.06 fps, matching
current Summit's default 52.09 / 52.10 fps. These controls attribute the gain
to the readback policy. Evidence:
`.vm/performance-12h-20261008/pi-scroll-default-comparison.json` and
`pi-matched-scroll-results.json` in the same directory.

Pi native-wheel checks confirm loaded content and actual root movement on
Wikipedia and GitHub. Four 60-notch bursts at 25 ms intervals range from
36.46–39.27 fps on Wikipedia and 38.76–41.93 fps on GitHub with the current
engine; installed Summit ranges from 25.23–37.91 and 39.91–44.65 fps.
These real-page checks do not show a consistent gain across sites. Screenshots
were taken after timing. One initial Wikipedia run failed collection on a
connection timeout and is excluded; its complete repeat passes. Evidence:
`.vm/performance-12h-20261008/pi-probe-20261009-061743-wheel-wiki-current-repeat/`
and the adjacent `061304`, `061503` and `061539` wheel runs.

Native close/cancel/navigation tests pass **145 checks** twice on the Pi and
once on X399. The driver now waits for the existing 250 ms batched session
save; its first Pi failure read the profile too early, while the final saved
session was correct. The X399 runner now honors the same optional graphics
library prefix as the benchmark launcher; its initial omitted-prefix run
failed EGL startup and is excluded. Successful runs quit without crash events
or remaining helpers. Evidence:
`.vm/performance-12h-20261008/pi-close-20261009-{063010,063412}/` and
`.vm/modern-close-1976c25e0b76494aa99cdf86/`.

Eight mixed Pi tabs preserve their DOM/canvas state and restore **1,325,478
pixels per tab exactly**, with no differing pixels. Captures wait six seconds
for scrollbar fading to finish; the earlier comparison caught 140–920
changing scrollbar pixels rather than lost page content. Aggregate resident
areas fall from 760.2 MiB with the tabs loaded to 738.7 MiB after hiding and
172.6 MiB after closing them. These totals include shared mappings and are
not unique process memory. The system-used reading falls from 1351.4 to
616.5 MiB after closing; one blank tab remains. Final quit leaves no helpers
and device health passes. This run used a later OS boot and is a lifetime
correctness check, not a comparison with the older platform. Evidence:
`.vm/performance-12h-20261008/pi-probe-20261009-064007-lifetime-current-eight-settled/`.

The loader crash investigation found an unsynchronized TLS-template vector:
loading another library could reallocate it while a new thread copied a TLS
template. A focused native test retained all libraries until its workers
finished, yet observed two initialization mismatches after 26 passing rounds.
The separate experimental Haiku fork now locks template accesses through the
copy and atomically reads the global generation. Established thread-local
blocks retain their lock-free access path; the new lock is reset after fork.

The loader candidate passes 57,600 TLS checks before and after a QEMU reboot,
all 22 standard loader tests, the 2,000-check generation test, and C++ TLS
construction/destruction. Its unattended rollback was tested from the prior
fix to the candidate and back, then to the VM's original loader. An initial
upgrade attempt encountered a package-manager confirmation dialog; the tested
procedure removes each version and verifies activation before installing the
next. X399 now uses removable package
`summit_runtime_loader_tls_fix-1.1-1-x86_64.hpkg`, loader SHA-256
`c9c57eae848f9b860ffffdd0b600631da181825c87a24d5fc8b40a6eaffd9d4c`.
All 57,600 native TLS checks and the other loader regressions pass; the guard
verified installation. X399 was not rebooted. All **100 subsequent browser
startup/local-page/normal-quit cycles** pass without crash events or leftover
helpers. These correctness checks ran during the engine rebuild and do not
establish startup performance. Evidence:
`.vm/performance-12h-20261008/tls-concurrency/`.
The test VM's original loader was restored and verified before orderly
shutdown, returning that VM to its original off state.

## 8 October 2026: six-hour optimization session

The retained build is **`bundle-tmeeg8cb`**, installed on X399. Deferred canvas
hit-region clipping reduced runtime for the six measured HTML canvas clipping
workloads by **22–54%**. Paired Speedometer runs were essentially unchanged (+0.18%).
The texture pool also avoids entering GL twice a second when no idle texture
can be reclaimed. Memory-policy experiments did not establish a useful saving,
so GC, allocator and hidden-buffer settings retain their original defaults.
A separately reproduced Haiku loader TLS bug was fixed and deployed through
a removable package; the earlier condition-variable exception remains
unexplained. Extended media verification also reproduced an intermittent Haiku
HD Audio service stall, detailed below. Measurements, rejected trials and
verification follow.

All issue fixes were installed and verified on X399 before this session began.
The session started at **02:14:10 UTC** (13:14 Hobart). Excluding 1,275 seconds
of workstation outage and user-requested pause, the six-hour minimum was reached
at **08:35:25 UTC** (19:35 Hobart). Final media validation finished at **08:39 UTC**.
Earlier issue implementation, builds and verification do not count toward it.
The baseline is `bundle-ralgs2gr`, engine patch
`303c94b24cd5f1cf92493e00a6c30f0985a7e52d3630c24b14f538719850605f`,
with Mesa `prefix-20261002` and the October 7 WebKit pin. The normal installed
launcher and its loaded engine/helper paths were verified.

Measurements use owned browser groups and isolated profiles while preserving
the user's original browser and other applications. CPU contention, actual
viewport dimensions, memory usage, teardown and crash evidence accompany the
runs. The session record is `.vm/optimization-2026-10-08/session.json`.
Initial baseline (no optimization gains claimed yet):

- Three uncontended ten-iteration Speedometer 3.1 runs scored **10.882,
  10.787 and 10.771**. Each used a 1280 × 854 viewport at DPR 2, with a fresh
  profile. The first iteration was slower than the following nine in each run.
  Evidence: `.vm/bench/speedometer-20261008-{131644-opt1008-baseline-a2,131924-opt1008-baseline-b,132050-opt1008-baseline-c}/`.
- Twelve real sites, at the larger 1920-wide window used by `run-multitab.py`,
  used **4093 MiB** after loading and idling, then **6142 MiB** after visiting
  every tab and letting hidden-tab buffers settle. Median first frame after
  switching was **25.8 ms**, maximum **133.7 ms**. Closing eleven tabs and
  navigating the remaining tab to blank settled at **598 MiB** after 45 seconds.
  Evidence: `.vm/optimization-2026-10-08/real-tabs-baseline/` and
  `.vm/bench/multitab-20261008-132621-opt1008-baseline-real/`.
- The new local `tools/bench/run-tab-memory.py` separates blank, DOM, canvas
  and composited-layer pages. Twelve mixed tabs settled at **1996 MiB**;
  the initial blank browser used **234 MiB**, and closing back to blank settled
  at **282 MiB**. All **783** page-state and canvas-pixel checks passed, including
  reactivation of every tab. The screenshot was inspected. Evidence:
  `.vm/bench/tab-memory-20261008-133025-opt1008-baseline-mixed/`.

Memory figures above sum resident process areas and can count shared mappings
more than once; they are not proportional-set-size measurements. The local
harness also records system used memory, CPU samples and each process's
`listarea` output. Both tab runs quit normally with no leftover helpers or new
crash/debugger events. Local fixtures provide repeatability; real-site loads
also reflect changing remote content and network conditions.

The first experiments use separate `bundle-vhe8tqtg`, patch
`6439bb9995f1d7e1a1d5b9db48215bdc425024849671cd6e08e96dc3af8b52a6`.
Its texture pool checks for releasable entries before making a GL context
current; idle live tiles no longer enter the driver on every 500 ms cleanup
tick. Timers still run so a texture released by another thread is reclaimed.

An opt-in hidden-page cleanup experiment follows actual visibility, since
Haiku's hidden card views remain attached to their window and do not trigger
WebKit's existing `pageWillLeaveWindow` cleanup. After 30 seconds it clears
noncritical caches and unused allocator/Skia resources, preserving live state
and back/forward entries. Visible, audible, capturing and extension pages are
excluded. `SUMMIT_HIDDEN_MEMORY_TRIM=1` enables this; `=2` adds a full JavaScript
garbage collection without explicitly deleting compiled code. The default is
**off** in that trial bundle, pending useful gains on representative pages. Trace timings are
available with `SUMMIT_HIDDEN_MEMORY_TRACE=1`.

On the twelve local mixed pages, modes 1 and 2 passed all 783 checks each and
quit without helpers or crash/debugger events. Their hidden memory totals were
2049 and 2010 MiB; the disabled control measured 2037 MiB. Mode 1 callbacks
took at most 1.129 ms, mode 2 at most 6.612 ms. These small memory differences
do not establish a practical improvement. Evidence:
`.vm/optimization-2026-10-08/hidden-memory-trials.json`.

The real-site follow-up also found no gain: after visiting every tab and
settling, mode 0 used **6089 MiB**, versus **6101 MiB** for mode 2. Both
returned to about **618 MiB** after closing back to blank and quit normally
without leftover helpers or crash/debugger events. The opt-in cleanup code
was therefore removed from the next source revision; it is not a production
memory policy. The idle texture-pool fix remains. Evidence:
`.vm/optimization-2026-10-08/real-tabs-trials.json`.

Separately, setting `MIMALLOC_PURGE_DELAY=0` on the original baseline reduced
the local hidden-tab total from 1996 to 1893 MiB, with all 783 checks passing.
Its first uncontended Speedometer run scored 10.470 (versus 10.771–10.882
above). It remains an experiment rather than a default change. Evidence:
`.vm/bench/tab-memory-20261008-133341-opt1008-purge0-mixed/` and
`.vm/bench/speedometer-20261008-133651-opt1008-purge0-a/`.

`canvas-clip-cost.html` compares HTML and Offscreen 2D canvases with
`willReadFrequently: true`, alternating their order for five rounds of 10,000
save/clip/single-pixel-fill/restore operations. Median milliseconds on the
original installed engine:

| Clip | HTML canvas | OffscreenCanvas |
| --- | ---: | ---: |
| Rectangle | 26 | 14 |
| Rectangle, then translated nested rectangle | 41 | 17 |
| Rounded rectangle | 261 | 135 |
| Rounded rectangle, then nested rectangle | 558 | 236 |
| Twenty-point polygon | 756 | 561 |
| Polygon, then nested rectangle | 1423 | 989 |

All resulting pixels passed in the corrected fixture, the run was uncontended
and the browser quit without helpers or crash/debugger events. Evidence:
`.vm/bench/probe-20261008-140039-summit-opt1008-clip-baseline-b/`.
The first attempt had a fixture error: its nested case read a pixel before
accounting for the translation. Its validation failed and it is excluded.
`run-probe.py` now reports an explicit `passed: false` payload as a failed
probe, and records teardown, helper and crash evidence. These timings motivate
an investigation of eager hit-region clip bookkeeping; they do not by
themselves attribute every HTML/Offscreen difference to that bookkeeping.

Before changing the clip representation, the native hit-region regression
fixture was extended to cover late Path2D mutation, clipping-time transforms,
40 nested clips with a saved ancestor, even-odd holes, empty chains, pending
clip reset and transformed clipped clearing. The installed baseline passed
**66 native clicks and 681 checks**, with clean teardown and no new
crash/debugger events:
`.vm/optimization-2026-10-08/hit-regions-expanded-baseline/`.

Deferred clip snapshots in `bundle-om1ysaa5` avoid those intersections until
a hit region is added or cleared. They retain clipping-time paths, transforms
and fill rules, share snapshots across saved states, and flatten chains at
16 entries. Drawing still applies its clip immediately. The new engine passes
the expanded 66-click / 681-check fixture and all 20 lifetime/recording checks,
with no helpers or crash/debugger events left behind.

Two candidate runs, with an original-bundle repeat between them, measured:

| Clip | Original HTML ms | Deferred HTML ms |
| --- | ---: | ---: |
| Rectangle | 26, 26 | 19, 19 |
| Rectangle, then translated nested rectangle | 41, 41 | 26, 27 |
| Rounded rectangle | 261, 260 | 146, 147 |
| Rounded rectangle, then nested rectangle | 558, 555 | 256, 258 |
| Twenty-point polygon | 756, 750 | 582, 562 |
| Polygon, then nested rectangle | 1423, 1399 | 1038, 994 |

These are medians of five rounds of 10,000 operations, with the same fixture,
window, environment and isolated profiles. Every pixel check passed; all runs
were uncontended and quit normally without helpers or new crash/debugger
events. The reduction is about **22–54% for these clipping workloads**; it is
not a claim about whole-browser speed. OffscreenCanvas does not maintain this
hit-region metadata and remains a comparison in each run. Evidence:
`.vm/optimization-2026-10-08/deferred-validation.json`, plus the original
baseline cited above. Engine patch:
`b178f34dcbc14aa0cf15e144e5ef4607a77b6ae90c4df3ffd078300721cdb9e2`.

The stress harness now uses workstation fixture addresses/binding, accepts
the same explicit browser environment as other benchmarks, and cleans up its
owned browser on harness failure. A two-minute local smoke run completed
55 navigation/tab/history actions with no findings, crashes, debugger events
or leftover helpers. Separately, an owned orphan-process test verified that
`guest.terminate()` removes helpers even when their UI parent has already
exited, while preserving an unrelated guard process and the user's original
browser. Evidence: `.vm/bench/stress-20261008-143841-opt1008-stress-smoke/`
and `.vm/optimization-2026-10-08/orphan-cleanup.json`.

The deferred-clipping bundle also passes all 120 platform-page checks,
unchanged grid/reflow geometry, WebGL 1/2 pixels and animation, canvas paths
and damage, and the H.264/HEVC transport-stream playback/seek checks. Evidence:
`.vm/optimization-2026-10-08/deferred-regressions/summary.json`.

Two alternating full Speedometer pairs, now using 15 iterations per fresh
profile, scored **10.811, 10.911** for the candidate and **10.870, 10.813**
for the original installed bundle. Their means differ by only 0.18%; this
does not establish an overall browser-speed change. All four runs were
uncontended, completed without benchmark events, and quit normally with no
helpers or new crash/debugger events. The harness now checks teardown before
declaring success, including helpers whose parent exited during launch.
Evidence: `.vm/optimization-2026-10-08/speedometer-regression-matrix.json`.

Collector-thread experiments on that same bundle used two alternating
15-iteration runs per setting. The default eight markers scored **10.846,
10.941**; four scored **10.844, 11.031**; two scored **11.071, 10.895**.
All were uncontended and clean. Mean differences of +0.4% and +0.8% do not
establish a reliable improvement, so no collector default has changed.
Evidence: `.vm/optimization-2026-10-08/speedometer-gc-matrix.json`.

`gc-churn.html` provides a separate allocation diagnostic: 1,200 batches of
24,000 records with strings, typed-array payloads and shared references,
keeping only the latest eight batches. It checks retained data throughout,
records batch time and frame intervals, waits 20 seconds with the working
set retained, then releases it and waits another 20 seconds. It never forces
collection. The default and four-marker runs each pass all **38,320** checks;
the small native smoke case passes 688 checks.

`run-probe.py --sample-interval 2` records owned-process memory, threads and
CPU alongside contention. `guest.load_report()` now preserves this memory
data from its existing native sample, also making it available to Speedometer.
Resident area sums can double-count shared mappings; sampled maxima are
observed peaks, not exact high-water marks. Page phase timestamps use the
guest clock, so the experiment runner measures its offset from the host clock
before each run. Evidence:
`.vm/optimization-2026-10-08/gc-churn-matrix-a.json` and
`.vm/bench/probe-20261008-145131-summit-opt1008-gc-smoke/`.

Allocation-policy results (all 38,320 checks pass in every run, uncontended,
with normal quit and no helper/crash/debugger events):

| Policy | Work ms | Retained-idle MiB | Released-idle MiB | Work frame p99 ms |
| --- | ---: | ---: | ---: | ---: |
| Default, first / repeat | 12266 / 12388 | 1154 / 1199 | 1142 / 1185 | 31 / 32 |
| Default, four GC markers | 11927 | 1155 | 1138 | 30 |
| Default, two GC markers | 12011 | 1259 | 1245 | 33 |
| Normal heap policy | 12880 | 696 | 639 | 29 |
| Normal policy, four markers | 11786 | 740 | 726 | 22 |
| Normal policy, two markers | 11960 | 768 | 732 | 22 |

On this 64 GiB host, `JSC_aggressiveHeapThresholdInMB=131072` selects the
normal policy. It changes both proportional heap growth and the aggressive
policy's allocation allowance used to avoid frequent collections. Setting
only `JSC_heapGrowthMaxIncrease=1` retained about 1141 MiB; reducing
`JSC_maxEdenSizeForRateLimitingMultiplier` to 4 or 0 retained about 975 or
1073 MiB. Neither isolated setting matched the normal-policy combinations.
Frame intervals are not direct GC-pause measurements. Evidence:
`.vm/optimization-2026-10-08/gc-churn-matrix-{a,b}.json`.

Full-browser validation rejected the normal-policy combinations as defaults:

| Heap policy / markers | Two 15-iteration Speedometer scores | Mean versus default |
| --- | --- | ---: |
| Default / 8 | 10.895, 10.784 | — |
| Normal / 8 | 10.165, 10.175 | −6.2% |
| Normal / 4 | 10.486, 10.511 | −3.1% |
| Normal / 2 | 10.574, 10.517 | −2.7% |

All eight runs were clean and uncontended. The allocation diagnostic's
throughput result did not predict the broader browser workload; the memory
savings are a tradeoff, not a free improvement. The installed settings remain
unchanged. Evidence:
`.vm/optimization-2026-10-08/speedometer-heap-matrix.json`.

Smaller changes to the aggressive policy's allocation allowance avoided that
Speedometer cost in two alternating runs each:

| Rate-limiting multiplier / GC markers | Scores | Mean |
| --- | --- | ---: |
| Default 8 / 8 | 10.778, 10.935 | 10.8566 |
| 4 / 8 | 10.900, 10.785 | 10.8425 |
| 4 / 4 | 10.857, 10.918 | 10.8877 |
| 6 / 8 | 10.905, 10.905 | 10.9046 |

All eight runs were clean and uncontended, with no option-parsing warnings.
These small differences do not establish a speed improvement or regression.
The multiplier limits extra allocation permitted to delay frequent GC; it
does not cap live heap size. With the usual 32 MiB large-heap minimum on this
host, multiplier 4 lowers that allowance from 256 MiB to 128 MiB. Its allocation
diagnostic retained-idle total was 975 MiB versus 1154–1199 MiB for default,
so it proceeds to real-tab and longer workload validation with eight markers.
No installed setting has changed yet. Evidence:
`.vm/optimization-2026-10-08/speedometer-cap-matrix.json`.

The twelve-site trials did **not** reproduce a general memory reduction:
valid default runs settled at **6061 and 6031 MiB** after visiting all tabs;
multiplier 4 settled at **6022 and 6129 MiB**. All four valid runs closed back
to blank, quit normally and left no helpers or crash/debugger events. The
allocation-heavy diagnostic remains the only demonstrated memory saving.
Evidence: `.vm/optimization-2026-10-08/real-cap-trials.json` and
`.vm/optimization-2026-10-08/opt1008-default-crash-repeat-a/run.json`.

One additional default-control run failed: GitHub's WebProcess exited during
tab switching, with an invalid-opcode exception in `pthread_cond_wait` on a
collector helper thread. Its lower memory total is excluded. The report's
instruction pointer falls inside the call instruction immediately before the
normal return address; this observation does not establish the cause.
The installed baseline and candidate JavaScriptCore libraries have the same
SHA-256, `348d27b11dfe0c55d4267f174193d3ec1d3005bfb9d024aa6a856069ca02ecaa`.
The immediate full-workload repeat above passed. Further native condition
variable and sustained-browser checks are in progress; no fix is claimed.
Evidence: `.vm/optimization-2026-10-08/opt1008-default-b-real/run.json` and
`WebProcess-346376-debug-08-10-2026-04-57-23.report` in the same session directory.

The separate native condition-variable probe completed **3,602,343 wakeups**
in 60 seconds without signals or crash events. Its next mode repeatedly sent
`SIGUSR1` to eight waiting threads; during that mode X399 stopped answering
SSH and ping. The owned test was team 352204. An attempt to kill that team
could not connect. NanoKVM remained reachable and reported power on and HDMI
enabled, but supplied no video. No restart was performed; owner approval was
requested because open applications were being preserved. This probe does
not establish the cause of the earlier WebProcess exception, and the signal
flood must not be repeated. Native long-run verification and final optimized
installation are pending recovery. Evidence:
`.vm/optimization-2026-10-08/{cond-signal.log,cond-signal-results.json,native-stall-status.json}`.

After the owner rebooted the workstation, it was reachable and idle again at
05:27:40 UTC. The excluded outage/pause shifts the session's minimum finish
to 08:35:25 UTC. The earlier unthrottled signal probe will not be repeated.

A new 25-action browser smoke exposed a separate Mesa startup crash: a once
trampoline called a null callback stored in dynamic thread-local storage.
A two-library native reproducer showed that Haiku's loader discarded a new
thread's first TLS write after another TLS image had unloaded. Its new vector
started at generation zero, so the second access invalidated the newly
created block. Initializing the vector at the current generation fixes that
reset. The platform fix and regression fixture are committed in the
[Haiku fork](https://github.com/jmgasper/haiku/commit/d9e624084c).

The local loader build passed. An isolated QEMU overlay failed before the
change, passed all 2,000 TLS checks after it, failed again after rollback,
and passed after reinstall and normal VM reboot. The existing 22 loader
checks and the C++ TLS lifetime fixture also passed. Native X399 failed
1,224 of 2,000 checks before the fix and passed all 2,000 afterward. Only the
loader was installed, through a removable package, without rebooting X399.
The planned automatic rollback guard failed to start because of a script
quoting error; activation was verified directly and package removal had
already been tested in QEMU. This does not establish the cause of the earlier
condition-variable invalid-opcode crash. Evidence:
`.vm/optimization-2026-10-08/tls-generation/`.

The repeated browser smoke completed all 25 seeded actions in 53.4 seconds,
with normal quit, no remaining helpers and no crash/debugger findings. Its
14 native load samples had a 169 ms median. `stress-browser.py --actions`
now supports matched action counts, records native navigation timing, detects
renderer exits in every tab, and returns failure for recorded findings or an
unmet action target. The earlier crash run correctly failed. Evidence:
`.vm/bench/stress-20261008-164722-opt1008-stress-actions-smoke/`.

The smaller GC allowance also passed the twelve-tab local test after loader
repair: all 783 checks, 2009 MiB hidden and 281 MiB after closing to blank,
normal quit and no helpers or crash events. This is comparable to the earlier
default local measurements and does not support enabling the setting.
Evidence: `.vm/bench/tab-memory-20261008-164856-opt1008-cap4-controlled-tabs/`.

A separate disposable QEMU overlay ran a bounded condition-variable/signal
diagnostic with eight waiters. Each case ran for ten seconds; the sender
slept 10 ms between signal operations and waits for acknowledgements had a
two-second timeout. Plain waits completed 37,952 wakeups, the no-op handler
case sent 990 signals, and nested suspend/resume completed 970 handshakes.
All exited normally and the VM shut down normally. This did not reproduce
the earlier native invalid-opcode crash and does not establish its cause.
No signal diagnostic was repeated on X399. Evidence:
`.vm/optimization-2026-10-08/cond-signal-vm-bounded.{cpp,log}`.

After the loader repair, the default candidate completed a matched
**1,200-action** local stress run in **2,552 seconds**: 324 new tabs,
319 closes, 232 navigations, 136 selections, 126 history actions and
63 reloads. All actions completed without stalled loads, renderer exits,
crash/debugger findings or leftover helpers; the browser quit normally.
The 648 native load samples had a **112 ms median** and **376 ms p95**.
After closing the workload and settling, resident-area totals were
60 MiB for the browser, 39 MiB for networking and 242 MiB for renderers.
This is a stability measurement, not a claim of zero memory growth.
Evidence: `.vm/bench/stress-20261008-165154-opt1008-stress1200-default/`.

The next trial, committed as `ab0fc08`, reclaims presentation buffers after
30 seconds hidden. It retains the UI's current immutable frame, DOM/JS state,
canvas pixels and layer tiles, while dropping spare UI bitmaps, the renderer
bitmap/context and the compositor's full-frame output texture. Reactivation
forces a complete repaint. Pending presentations and forced repaint callbacks
delay reclamation. `SUMMIT_RELEASE_HIDDEN_FRAME_BUFFERS=1` opts in; it remains
off by default while memory, restored pixels and switching costs are measured.

That first presentation-buffer trial was rejected. Its renderer changes
targeted `DrawingAreaHaiku`, but the installed `SkiaCGMiPGO` configuration
uses coordinated graphics and `AcceleratedSurface`; only the UI-side cleanup
ran. Hidden resident-area totals were 2014/2013 MiB for the controls and
2009 MiB with the trial, without a useful reduction. All three runs passed
783 page checks and quit cleanly. The controls' 24 restored screenshots
matched exactly; the enabled run's restoration screenshots were entirely
black because the desktop screen saver activated, so they provide no visual
validation. The harness now wakes the display through settling and captures.
The ineffective engine changes have been removed from the next candidate.
Evidence: `.vm/optimization-2026-10-08/presentation-buffers/`.

The replacement experiment targets the active TextureMapper compositor.
Its existing hidden-buffer cleanup keeps the last shared bitmap for the UI,
but the trial also releases that target's GPU color/depth attachments after
outstanding presentations finish. It recreates them on the next render and
invalidates their damage history to require a complete repaint. Live page,
canvas and layer resources remain. The experiment is initially off;
`SUMMIT_RELEASE_HIDDEN_GPU_BUFFERS=1` enables it.

In `bundle-vctsados`, the coordinated trial actually released the GPU
attachments for all eleven hidden tabs. Both controls and the enabled run
passed **783 checks** each and quit normally without helpers or crash events.
All **36** initial/restored content screenshot pairs matched exactly.
Hidden resident-area totals were **2001 and 1993 MiB** in the controls versus
**2016 MiB** enabled; switching medians were **11.4 / 9.6 ms** versus
**10.5 ms**. These results do not demonstrate a resident-memory reduction.
Evidence: `.vm/optimization-2026-10-08/gpu-buffers/local-results*.json`.

The dedicated graphics-lifetime fixture passed **68 checks** with the trial
enabled. Both WebGL versions retained their buffers, scissor state and pixels;
the 2D canvas retained its clipping state and pixels. Updates made while hidden
survived another restoration and window resize. The unchanged restoration
matched the original screenshot exactly. It quit normally without helpers
or crash events. Evidence:
`.vm/optimization-2026-10-08/opt1008-gpu-buffer-graphics/`.

The real-site pair likewise did not justify the GPU-buffer policy: hidden
resident-area totals were **6359 MiB** disabled and **6830 MiB** enabled.
Remote content varies, so this pair does not establish a causal memory
regression, but it supplies no evidence of the intended saving. The experiment
was removed, including its context callback and framebuffer refactor; the
engine patch returned exactly to `b178f34d…`, retaining the measured canvas
clip optimization and the idle texture-pool improvement. No GC allowance,
heap-growth, allocator-purge or hidden-buffer policy change is being shipped.
Evidence: `.vm/optimization-2026-10-08/gpu-buffers/real-results.json`.

The retained engine was rebuilt and installed as **`bundle-tmeeg8cb`** from
source **`b4006ff`**, with patch `b178f34d…`. JavaScriptCore remains byte-identical
to the initial baseline (`348d27b1…`), and rebuilt WebKit exactly matches the
validated retained candidate (`8cea0f61…`), confirming the experimental code
was fully removed. The normal desktop launcher passed all
48 OffscreenCanvas/image-codec checks, loaded its executable, helpers and
engine libraries from the new bundle, and quit without helpers or crash
events. Its screenshot was inspected. The first installed check had a fixture
server error (missing image-file routes); its failed evidence is preserved.
After fixing the server mapping, a fresh run passed:
`.vm/optimization-2026-10-08/final-installed-1791444384/`.

The reusable `tools/bench/run-presentation-buffer-lifetime.py` also passed all
68 checks on the installed retained engine, including exact restored pixels,
background updates and resize, with normal quit and no helpers or crash events.
Evidence: `.vm/bench/presentation-lifetime-20261008-182633-opt1008-final-retained/`.

The installed retained bundle completed a second **1,200-action** stress run
with the same seed, local fixtures and six-tab limit as the earlier control:
324 new tabs, 319 closes, 232 navigations, 136 selections, 126 history actions
and 63 reloads over **2,570.8 seconds**. There were no findings, crash reports,
syslog debugger events or leftover helpers, and the browser quit normally.
After settling, resident-area totals were **59.7 MiB** for Summit, **34.8 MiB**
for the network process and **245.5 MiB** across two renderers. The earlier
retained-engine run settled at 59.7 / 38.6 / 241.8 MiB. These totals support
stable teardown in this workload, not a demonstrated memory saving or proof
that no leak is possible. The 648 native loads had a **118.5 ms median** and
**389.2 ms p95**; variation against the earlier run is not treated as a speed
change because both use identical WebKit and JavaScriptCore binaries.
Evidence: `.vm/bench/stress-20261008-182757-opt1008-stress1200-final-retained/`.

The first final streaming run encountered a system audio failure starting at
case 49: six of 56 cases failed clock checks, and the browser recorded 36
audio-output connection errors. No browser crash or helper leak occurred.
After Summit exited, an independent silent `BSoundPlayer` also blocked while
starting; Haiku's HD Audio message port had all 64 queue slots occupied.
This establishes that the failure persisted outside Summit, but does not
identify its cause. The existing September 25 audio-service incident below
is relevant history, not proof that these incidents have the same cause.

With no browser or media player running, the public media shutdown API returned
success and the services automatically relaunched. The immediate launch call
returned `B_ALREADY_RUNNING`; an immediate sound check preceded audio-node
readiness. A fresh check after initialization passed with **100 silent audio
callbacks** and an empty HD Audio message queue. No workstation reboot was
performed. The failed run and each recovery step remain in
`.vm/optimization-2026-10-08/final-retained/validation-retry/streaming/` and
`.vm/optimization-2026-10-08/media-recovery/`.
The streaming harness now keeps the display awake and rejects audio-output
errors and crash-related syslog events as well as crash reports.

The first post-recovery repeat had no audio-output errors, crashes or helpers
left behind. Cases 1–55 passed; case 0 exposed a fixture timing race. Cold
audio setup took **711 ms**, so the playback clock had reached its check point
before the first 40 ms canvas-sampler callback ran. The existing clock and
dimension checks passed, but there were no sampled frames yet. The fixture
now waits at most **one additional second** for three distinct decoded frames;
its playback-clock limits remain unchanged. That failed repeat is preserved in
`.vm/optimization-2026-10-08/final-retained/streaming-after-media-recovery/`.

The next run passed its first 17 cases, then the HD Audio service stalled
again. It was cancelled, and its browser quit normally with no helpers or
crash events. Read-only thread/semaphore queries showed **HD Audio control**
waiting for the **multi_audio audio output** thread to exit; that worker was
waiting on the driver's **hda_buffer_sem**. The checked-out Haiku source has
an unbounded `wait_for_thread()` in `MultiAudioNode::_StopOutputThread()` and
an interruptible, unbounded buffer wait in the HDA driver. These observations
locate the blocked threads; they do not establish why buffer interrupts ceased.
No audio driver change was made. A service restart is a recovery measure,
not a fix for the recurrence. Evidence:
`.vm/optimization-2026-10-08/media-recovery/thread-state-stalled.log` and
`.vm/optimization-2026-10-08/final-retained/streaming-final/`.

After the second audio recovery, four cases passed with no audio errors, but
the native driver then failed its focus check. It requested activation only
once while the display-wake helper could still be stopping the screen blanker.
The driver now retries activation of its owned window within the existing
20-second wait. This failed attempt also quit without helpers or crash events;
its evidence is in
`.vm/optimization-2026-10-08/final-retained/streaming-fresh-services/`.

The final complete run passed **all 56 cases and 1,692 DOM checks**, plus
**50 PCM seek checks, five video-timing checks and exact Opus trimming**.
It quit normally with no audio-output errors, crash reports, crash-related
syslog events or leftover helpers. The final source changes after the installed
`b4006ff` build affect test harnesses and documentation only. This successful
run verifies the installed browser against functioning media services; it
does **not** resolve the recurring system audio stall described above.
Evidence: `.vm/optimization-2026-10-08/final-retained/streaming-focus-retry/`;
cross-check of installation, graphics, stress, media and patch identity:
`.vm/optimization-2026-10-08/final-audit.json`.

## 5 October 2026: quitting with a busy page, helpers left behind, the Pi's video decoder

Three items left open on the Raspberry Pi 4 the night before, checked on the
X399 for regressions. In short:

| | Before | After |
|---|---|---|
| Pi: quit with YouTube loading, until the window is gone | 0.9-18.8 s (14-21 s typical) | 1.2-1.4 s |
| Pi: a start right after quitting | lost while the old instance exited (nothing opened) | opens (waits for, or is passed on by, the old one) |
| Helper killed with its browser while starting | suspended for good, ~10 MiB each | removed by the next browser's first launch |
| Pi: MSE H.264 720p30 | libavcodec, 515-540 of 898 pictures, 1.37-1.47 cores | VideoCore, all pictures, 0.65-0.95 cores |
| Pi: MSE H.264 1080p30 | libavcodec, 652 of 898, 2.52-2.67 cores | VideoCore, 886-929 shown, 1.33-1.35 cores |
| Pi: H.264 1080p30 file | libavcodec, 27 fps, 2.9 cores | VideoCore, 25 fps, 2.1 cores |
| X399: Speedometer 3.1, 3 runs each, alternating | 11.38, 10.64, 11.39 | 11.32, 11.16, 11.45 |

Installed: Pi `summit_webkit` 1.10.0-9 and `summit` 0.1.0~git20261005-1;
X399 `bundle-r6slr78u` (Mesa prefix-20261002).

### Quitting while a page's main thread is busy

`quit-trace.sh`/`quit-strace.sh` (tools/pi) with `SUMMIT_QUIT_TRACE=1`, which
now traces every step in the browser and the engine on one clock. Quitting
asks each tab's page (TryClose: its before-unload handlers) before anything
closes. The answer comes when the page's main thread gets to it; a YouTube
page still loading on the Pi runs one script for 4-5 s at a time. The Haiku
port waited 10 s for it, then pinged the process (3+ s) and, if the process
answered, cancelled the close; the window stayed up all that time.

Upstream WebKit closes after 50 ms without an answer, Firefox after
`dom.beforeunload_timeout_ms` (1 s). Summit now gives the page 1 s and then
closes it; a before-unload prompt the page shows stops the clock, as before
(`runBeforeUnloadConfirmPanel()`). Pi, quit 7-13 s into YouTube's load,
alternating: new 1.94, 6.29, 2.08, 1.89 s; installed 2.30, 0.91, 18.80,
13.66 s. The window goes 1.1-1.4 s after the request.

What is left after the window has gone is the SD card (1.4 MB/s, 25 MHz):
while the network and web processes write YouTube's HTTP cache and its 44 MB
bytecode cache, the browser's last profile write waited 4-5 s and its
team's teardown after `exit()` 12-14 s (`threadstate` shows no thread busy;
the team can no longer be inspected once it dies). On a quiet card the
whole quit takes 1.5-2 s.

That tail cost something visible: Summit is single-launch, and a start in
those seconds was handed to the exiting instance and lost. Now the last
profile write happens after the application has left the registrar; a start
that reached the quitting instance is started again once it has gone
(`started again with N arguments` in the trace); a start that finds an
instance still writing its profile (a port named `Summit exits <hash of the
executable>`) waits for it, up to 20 s. `relaunch-test.sh` (tools/pi): a
start 0.3 s and 2 s after the quit both open their page.

The X399 quits in 0.14-0.77 s with either build.

The first 26 s, 44 s and 66 s quits measured in the morning were the Pi
running out of memory (below), not the browser.

### Helpers left suspended

A browser killed while `load_image()` loads a helper (0.3-0.6 s on the Pi)
leaves that team with one suspended thread for good. The engine's process
launcher now removes, on a thread of its own at its first launch, every team
running its own WebProcess or NetworkProcess executable (same file) whose
parent is gone (the kernel team adopts orphans) and whose threads are all
suspended. Reproduced with `killonspawn` (tools/pi/probes; kills a browser
the moment it has started a helper): both helpers are left suspended, and
the next start logs `Summit: removed ... left suspended by a browser that
died while starting it` for each, on the Pi and on the X399.

### The Raspberry Pi's video decoder

The Pi image has `rpi_mmal` (H.264 on the VideoCore firmware) and `rpi_hevc`
(the SoC's HEVC block). Neither registers a format with the media kit, on
purpose (a refusal would leave a stream without a decoder), so Summit could
not reach them: the media kit picked the ROCK 5's `00_rockchip_mpp`, which
fails on the Pi, and Summit fell back to libavcodec.

- `AddOnVideoDecoderHaiku` (MediaDecoderSelectionHaiku.cpp) loads `rpi_mmal`
  by name, as airTime does, for eight-bit 4:2:0 progressive H.264 up to
  1920x1088 (from the avcC's sequence parameter set), before the media kit's
  choice, in the MSE player and the file player. Anything it refuses goes
  on as before; `SUMMIT_HARDWARE_VIDEO=0` turns it off. The engine has no
  RTTI: the add-on's `MediaPlugin` is turned into its `DecoderPlugin` with
  the offset-to-top in its vtable, checked against the virtual base offset.
- The samples the add-on read while it was set up are given again to the
  next decoder if it refuses the stream.
- Its pictures are I420. MSE (YouTube) sends them to the compositor as
  planes: `CoordinatedPlatformLayerBufferPlanesHaiku` uploads them into
  three single-channel textures (`BitmapTexture::Flags::SingleChannel`, Haiku
  only) and draws them with TextureMapper's planar YUV shader (BT.709 at 720
  lines and more, BT.601 below). An image is made on the CPU only when
  something paints the video itself (`SUMMIT_VIDEO_PLANES=0` does it for
  every picture). The file player converts on the CPU (NEON); that
  conversion took 10.2 ms a 1080p picture because GCC 13 sends a
  `uint8x16x4_t` for `st4` through the stack, and takes 4.3 ms (an 8 MB
  `memcpy`'s time) with zips and plain stores.
- While the firmware decodes, the ARM's memory copies run about three times
  slower (rpi4 `docs/rpi4/MEDIA.md`): the file player, which copies every
  picture twice more (into its BBitmap, then for the compositor), gains CPU
  but not pictures; the MSE path, which copies nothing, gains both.
- The firmware's decoder service can stop answering until the next boot
  (below). A decoder that failed is closed on a thread of its own; when
  that takes seconds or a request times out, the add-on is not used again
  until the system restarts (`/tmp/Summit hardware video failed` names the
  boot) and videos go to libavcodec.
- After a flush (a seek) the MSE player's sample source told the decoder the
  stream had ended; the Pi's decoder then waits for the firmware to confirm
  that before returning. The source now cancels (`B_CANCELED`) after a flush
  or when closing and ends the stream only at its end; a flush during set-up
  no longer drops the next generation's samples. X399, NVDEC, 1080p seek:
  target reached 0.24 s after the seek (0.28 before).

Measured on the Pi with `tools/pi/pages/mse.html` (a fragmented MP4 through
MSE, 2 MB appends, title = presented/dropped) and `vplay.html`, served from
the host, display awake (the scripts now stop the screen blanker: a blanked
screen stops app_server drawing and makes the compositor look cheap):

| Clip | Decoder | Pictures shown / dropped | Web process |
|---|---|---|---|
| MSE 720p30 | VideoCore, planes | 849-915 / 0-49 | 0.65-1.47 cores |
| | VideoCore, CPU conversion | 593 / 305 | 1.59 |
| | libavcodec | 423-540 / 358-475 | 1.15-1.47 |
| MSE 1080p30 | VideoCore, planes | 886-929 / 26-92 | 1.33-1.35 |
| | libavcodec | 652 / 246 | 2.59-2.67 |
| File 1080p30 | VideoCore | 25 fps | 2.11-2.14 |
| | libavcodec | 27 fps | 2.88-2.91 |

YouTube gives the Pi 480p H.264 (854x480): 1.17 cores against 1.71.
Not done: the file player with planes (it would need its frame hand-off
reworked); `rpi_hevc` (Summit offers no HEVC through MSE).

### The Pi out of memory, and a kernel panic

The RAM disks that hung on 5 October at 00:25 held 2.3 GB of bundles that
could not be freed: 1.1 GB free with nothing running, "Summit memory
pressure: 95%, critical" in every test, stalls of tens of seconds. In that
state a web process's mimalloc returned memory with `MADV_FREE` at a thread's
exit and the kernel panicked (`VMAnonymousCache::Commit()` assertion from
`Discard()`; docs/kunanyios-platform-issues.md). The board was restarted from
its kernel debugger over the serial console (the NanoKVM's keyboard does not
reach it) and later once more with `shutdown -r` to clear the firmware's
decoder service. After a restart: 3.7 GB free.

### Start-up

Installed 1.10.0-9 on the restarted Pi, warm: the browser's code at
0.30-0.31 s, the first frame with the page 1.20-1.39 s.

## 4 October 2026 (evening): the Raspberry Pi 4 - start-up, scrolling, page loads

The Pi 4 (4 GB, four Cortex-A72 at 1.5 GHz, air/OS on an SD card, Mesa's
v3d) ran the engine of 2 October, built for it with Skia, GL compositing
and WebGL (`summit_webkit` 1.10.0-2, `/mnt/HaikuWork/rpi4/summit-gl`). This
session rebuilt that engine from the current tree, measured where its time
goes, and fixed what was Summit's. Tools and the loop: `tools/pi/README.md`.

In short, warm launches of a small page, seconds after exec:

| | browser code runs | window | first frame with the page |
| --- | --- | --- | --- |
| installed build (2 October) | 1.68 | 1.74 | 5.85 |
| current engine (link flags of 3 October) | 0.66 | 0.70 | 3.01 |
| ... shader warm-up after the first frame | 0.66 | 0.70 | 2.0 |
| ... bundled libraries with GNU hash tables | 0.43 | 0.47 | 1.52 |
| ... libraries most symbols are found in listed first | 0.39 | 0.43 | 1.46 |
| installed packages (summit_webkit 1.10.0-3) | 0.38 | 0.43 | 1.37-1.40 |

The first launch after a boot also reads the engine from the SD card
(25 MHz default speed, about 28 MB/s through packagefs): 11.2 s to the
browser's code with the old build, 8.7 s with the new packages right after
installing them. A faster SD mode in the OS's driver (high speed or UHS), or
reading the engine's files in the background after login, would help there.

Speedometer 3.1 on the Pi (three iterations): 1.02 +- 0.21 -> 1.36 +- 0.22;
the final packages (1.10.0-7) 1.34 +- 0.32.

### Libraries: the runtime loader binds everything at load

The arm64 engine predated `-Bsymbolic-functions` and `--hash-style=both`
(3 October). Haiku's `profile -a` works on the Pi: 93% of a bare
`WebProcess` start (634 ms) was the runtime loader (`find_symbol`,
`match_symbol`, `strcmp`). (The arm64 kernel books all user time as kernel
time, so `time` showed it as `sys`; a bash loop shows the same.)

- The current engine's link flags: libWebKit's symbol relocations 356k ->
  112k, a bare web process 1.48 -> 0.62 s.
- The 25 third-party libraries bundled with the engine (ICU, OpenSSL,
  libxml2, curl, FreeType, Fontconfig, HarfBuzz, libepoxy...) were linked
  with SysV hash tables only, and most lookups pass several of them before
  reaching their definition (a library looks up even its own symbols). A
  model of the lookups (`tools/pi/lookup-scenarios.py`) put 869,000 SysV
  chain walks into one web process start. `tools/pi/build-deps-gnu-hash.sh`
  rebuilds them from the same patched sources with `--hash-style=both` and
  `-Bsymbolic-functions` (libepoxy `-Bsymbolic`: its exports are function
  pointer variables; OpenSSL keeps its own `-Bsymbolic`): a bare web
  process 0.66 -> 0.42 s, the browser's code runs at 0.43 s instead of
  0.66, the first frame comes 0.5 s sooner.
- Left for the OS: Haiku's own libraries (libroot, libbe, libstdc++,
  libmedia...) are SysV-only too; GNU hash tables there would speed up every
  program's start, on the X399 as well.

### Shader warm-up before the first frame

The compositor compiled 18 shader programs (the ones twenty sites used by the
end of a scroll) as soon as it was created. With Mesa's shader cache that is
a few milliseconds; the Pi's Mesa is built without one
(`-Dshader-cache=disabled`) and v3d takes 40-150 ms a program: 2.2 s before
the first frame. Now the two programs every first frame draws with are
compiled while the page is fetched, the rest after the first frame that
shows the page's tiles, one per compositor task, only when no composition is
waiting and none was sent for 100 ms, and only until they have taken 150 ms
(`SUMMIT_WARM_SHADERS=<ms>|all|0`). The X399, with its cache, still warms
all of them.

The spare web process for the next tab now starts a second after a page has
loaded rather than at once.

`SUMMIT_STARTUP_TRACE=1` prints the milestones of all processes on
`system_time()`; on the Pi, warm: browser code 0.43 s, web and network
processes launched 0.47, their libraries loaded 0.87, web process main 0.90,
EGL 1.11, compositor context 1.20, page 1.25, first frame 1.43, first frame
with the page's tiles 1.69.

### Scrolling: frames held for a screen that was never used

The current engine scrolled Wikipedia on the Pi at 7 frames a second (the
build of 2 October: 30-35). Frames went out exactly 128 ms apart while the
compositor sat idle: the browser held each frame's FrameDone until app_server
had drawn it, which it does only for a window whose page also presents
straight into the screen (#16). The Pi's libEGL_mesa has no Summit present
entry points, so the web process reads every frame back, but the browser did
not know and offered the target again every second. The web process now
declines the target (a DirectFrame for target 0) and the browser stops
holding: 24-32 frames a second.

### Read-back on V3D

The GPU's buffers are mapped write-combining (`v3d` driver: uncached for the
CPU), and Mesa de-tiles every `glReadPixels` on the CPU into a staging buffer
the size of the read. A whole 1920x1000 frame read at 139 MB/s (55 ms);
bands of 16-64 rows at 260 MB/s (29 ms), as the staging buffer then stays in
the A72's L2. Pixel buffer objects are no faster (the copy out of the mapped
buffer runs at 420 MB/s), nor is a pbuffer, nor any combination of RGBA and
BGRA. On V3D Summit now reads in 384 KB bands (`SUMMIT_READBACK_BAND_KB`);
in a scroll the read went from 18.4 to 15.3 ms for 0.6 Mpx.

What is left per frame of a scroll, 0.6 Mpx: read 15 ms, GPU 4, tile
uploads 4, drawing 1.5. A maximised window (2 Mpx) cannot pass ~20 frames a
second this way. The fix belongs to the OS: frames the display scans out
from GPU memory (the HVS), or cached mappings with cache maintenance for
reads.

### Page loads: style sheets added while a page runs

Seconds until the tab stops loading, three visits each (some of the
current build's rounds overlapped a profile run):

| | installed | current |
| --- | --- | --- |
| Wikipedia article | 10.7, 10.7, 9.6 | 8.5, 9.1, 3.8 |
| BBC News | 29.7, 22.8, 23.1 | 14.8, 17.2, 5.2 |
| GitHub WebKit | 2.8, 3.1, 2.8 | 2.2, 1.9, 1.6 |

A Wikipedia article kept the web process's main thread busy for 6.6 s of
its 8.7 s load: style 27%, layout 21%, JavaScript 10.5% (parsing and
bytecode; the bytecode cache helps on later visits). With
`SUMMIT_PAGE_UPDATE_TRACE=3` the article was restyled from scratch three
more times after its first style pass, 300-400 ms each, each followed by a
relayout. A temporary trace of style invalidations found why: Wikipedia's
ResourceLoader adds module styles as the page runs, and WebKit restyles every
element of a document when an added sheet holds anything but style rules and
`@media` (here `@supports` and `@keyframes`), and also at every later sheet
change once a style pass has skipped elements because a sheet was still
loading (upstream's "FIXME: This should just invalidate elements with
missing styles").

- Rules inside `@supports` now go into the invalidation rule set as at the
  top level (RuleSetBuilder evaluates the condition); a new `@keyframes`
  reaches the animations that name it through `Resolver::addKeyframeStyle()`
  -> `Document::keyframesRuleDidChange()`. `SUMMIT_STYLE_SHEET_INVALIDATION=all|verify`.
- Elements left without style are remembered (up to 4096) and only their
  subtrees are invalidated. `SUMMIT_MISSING_STYLE_INVALIDATION=all|verify`.
- `verify` restyles everything after the targeted invalidation has been
  resolved and compares 21 computed properties of every element: Wikipedia
  (three checks, 7,423-9,728 elements) and GitHub (1,490): no differences.
  On the X399 also The Guardian, Reddit and YouTube (with uBlock Origin and
  1Password), BBC News, CNN (35 checks, 72,518 elements), Stack Overflow,
  Hacker News, Amazon, MDN, The New York Times and GitHub trending: the only
  differences were the `transform` of spinners and a pulsing icon, animations
  sampled at two moments.

| Wikipedia on the Pi, per load | style | layout |
| --- | --- | --- |
| before (three loads) | 2.14-2.18 s | 1.84-1.85 s |
| after | 1.07-1.10 s | 1.10-1.11 s |

The time to the load event hardly moves (the extra restyles came around and
after it); the main thread is free 1.8 s sooner and the page uses that much
less CPU.

### Video: the decoder the media kit picked failed

`<video>` with H.264 played nothing on the Pi: the image carries the ROCK 5's
`00_rockchip_mpp` decoder add-on, which sorts first, takes H.264 and fails to
set up there, and the media kit does not try the next add-on. A track
without a decoder now goes to libavcodec (`SoftwareVideoDecoderHaiku`, the
NVDEC fallback), and an MSE decoder that fails is set to libavcodec. A 1080p
clip plays, in software: about 2.6 cores, and ~11 compositions a second (each
frame is uploaded into the GPU's tiled memory and read back). The image
blocks the add-on on the Pi from its next build (rpi4 fda5bdb813); hardware
decoding (`rpi_mmal`) is not reachable through the public BMediaDecoder API.

### Idle and memory

A loaded Wikipedia page with nothing moving: under 0.5% of a core for all of
Summit's processes. Allocated memory: browser 45 MiB, network process 40,
the page's process 199, the spare process 28.

### Library order

Haiku's runtime loader searches images breadth-first in load order, the
executable's list first. WebProcess, NetworkProcess and Summit now list
libbe, libstdc++ and libroot (Summit also libJavaScriptCore) right after
libWebKit (no two of them define a common symbol). The model promised half
the lookup work; measured, the browser's code runs 30 ms sooner and a web
process loads 1-2% faster. Reordering `DT_NEEDED` with patchelf afterwards
does not work: it adds load segments the runtime loader refuses ("Could not
map image: Bad data").

### Installed

- Raspberry Pi (5 October, 02:20): `summit_webkit-1.10.0-7` (engine from
  Summit d2f5170: the mimalloc slots, the library order and `-Bsymbolic`
  below) with `summit-0.1.0~git20261004-1` (unchanged browser), `pkgman
  install` of the local file. Staged in `/mnt/HaikuWork/rpi4/packages-arm64`
  for the next image (1.10.0-5 and -6 moved to `packages-arm64-replaced`).
  Installed, warm: the browser's code at 0.32 s, first frame with the page
  1.58-1.59 s (1.69 s for 1.10.0-6 in the same state of the Pi; both 0.3 s
  above what a restarted Pi gives, see the RAM disk hang below); the first
  launch after installing, from the SD card: 8.1 s to the browser's code.
  Launch, quit and force-kill cycles of the installed build left nothing
  behind. 1.10.0-6 (01:10) had the mimalloc slots and library order only.
- X399: `bundle-3_40rbw1` (Mesa prefix-20261002), same sources: the
  browser's code at 0.11 s, first frame 0.45-0.48 s. Before it tonight
  `bundle-vg3_w8ii` (01:35, without `-Bsymbolic`): against the previous
  `bundle-p82o30tm`, alternating, Speedometer 3.1 11.26, 11.39, 11.26,
  11.05, 11.38, 10.87 (mean 11.20) against 11.06, 11.12, 11.06, 11.14
  (11.09); the DOM page 235 ms against 241; a bare web process 131-133 ms
  against 134-137.
- Before that, on the evening of 4 October: Pi `summit_webkit-1.10.0-5`
  (Summit 3f427fc), start page warm at 1.39 s; X399 `bundle-p82o30tm`,
  checked against `bundle-r0_tdpf3` (start-up 0.13 s to the browser's code,
  first frame 0.53-0.56 s, Speedometer 11.17 / 11.11 against 11.09 / 11.26,
  Wikipedia scroll bursts 53-59 fps, direct present still used, style verify
  modes clean on Wikipedia, GitHub, The Guardian, and with uBlock Origin and
  1Password on Reddit and YouTube).

### No shader compile inside the first frame with the page

The compositor compiled the rounded solid colour program on its second
paint; with the warm-up now after the first frame with tiles, that paint was
this frame (start page on the Pi: paint 89 ms, composition 128 ms). The
warm-up, which starts with that program, compiles it a moment later:
composition 54 ms.

### A cold start is the SD card's

The first launch after a new package is installed (or after a boot) reads
the engine from the SD card: 8.5 s to the browser's code. Reading the
engine's libraries ahead of time does not help: all of them, read cold in
parallel, took 20 s (the card seeks between them), where the loader reads
only the pages it touches. The SD driver runs the card at 25 MHz default
speed; a faster mode is the way to a faster cold start.

### Allocations: mimalloc's thread heap through the runtime loader

A profile of a Wikipedia load on the Pi had 1.5% of the web process's main
thread in `__tls_get_addr`, `get_tls_address` and `TLSBlockTemplates::Get`
(the runtime loader). On Haiku every access to a thread local of a shared
library is a call into the runtime loader with generation checks, and
mimalloc read its default heap from one (`__mi_theap_default`) on every
allocation. mimalloc on Haiku now keeps the default and the cached heap in
two slots from `tls_allocate()`, read off the thread pointer (`tpidr_el0`,
`%fs`), as it does with TlsAlloc slots on Windows; until the slots exist
(they are allocated by the first thread init, in the process constructor)
the thread locals are used. Haiku zeroes a new thread's slots except on
stacks the caller supplies, which nothing in Summit's processes does.

A page of allocation-heavy work (`tools/pi/pages/dombench.html`: HTML
parsing, DOM building, style and layout, JSON; median of five rounds, three
launches each, alternating):

| | before | after |
| --- | --- | --- |
| Pi | 1597, 1582, 1589 ms | 1531, 1477, 1529 ms |
| X399 | 241, 245, 240 ms | 236, 233, 235 ms |

### libJavaScriptCore loaded second, at last

The library order above did not take for libJavaScriptCore: CMake dropped
the copy inserted after libWebKit for the one libWebKit itself brings in, at
the end of the line, so WebProcess and NetworkProcess loaded it after a
dozen libraries. libWebKit looks up 49,000 symbols in it (the method tables
of its 2,200 JS wrapper classes point at `JSC::JSObject` functions), each
one through the SysV tables of libbe, libstdc++ and libroot first. Both
executables now get libWebKit and libJavaScriptCore as link options, which
come before everything else. The lookup model: 914k -> 426k units per
process. Measured on the Pi, warm, five launches each: a web process has its
libraries 15-20 ms sooner (0.355 -> 0.34 s after its launch), the first
frame with the page 10-15 ms sooner. Bare processes on the X399: 2-4 ms
faster.

### The loader's own work: repeated lookups

What is left of a web process's 0.34 s in the runtime loader is mostly
lookups repeated for the same symbol: libWebKit has 112,000 symbol
relocations but only 33,800 distinct symbols, and the runtime loader does
the whole breadth-first search for each (`vtable for
__cxxabiv1::__si_class_type_info` 11,980 times, `__cxa_pure_virtual` 4,103
times, each of 30 `JSC::JSObject` methods about 2,200 times). Profile of a
bare web process on the Pi: `find_symbol` 20%, `elf_gnuhash` 13%,
`match_symbol` 12.5%, relocation 12%, `strcmp` 11%. A per-image cache of
resolved symbols while an image is relocated (FreeBSD's rtld keeps one)
would cut the model's work 3.6 times; it belongs to the OS's runtime loader.

### libWebKit binds its own data symbols when it is linked

43,000 of libWebKit's lookups were of its own symbols: the type_info
objects every vtable points at, vtables and statics reached through the
GOT. `-Bsymbolic-functions` leaves data symbols to the runtime loader;
libWebKit is now linked with `-Bsymbolic` (after the other flag, so it
wins): 111k symbol relocations -> 69k. The binding only changes where an
image searched before libWebKit defines the same symbol, and libWebKit comes
right after the executable in the browser and both helper processes; the
executables define only Haiku's ABI markers, the two `WebCore::Headroom`
constants (compared by value) and libstdc++'s shared_ptr type_info objects
(compared by name), and have no copy relocations.

| | before | after |
| --- | --- | --- |
| Pi: browser's code runs | 0.38 s | 0.32-0.33 s |
| Pi: web process libraries | 0.34 s | 0.275 s |
| Pi: first frame with the page | 1.71 s | 1.58-1.60 s |
| X399: browser's code runs | 0.131 s | 0.110 s |
| X399: first frame | 0.515 s | 0.43-0.48 s |
| X399: bare web process | 136 ms | 111 ms |
| X399: Speedometer 3.1, five runs | 11.16 | 11.27 |

(Pi: copies on the SD card, alternating, after the RAM disk hang, which adds
the same 0.3 s to both.) The DOM page, Wikipedia scrolling (53 and 59 fps,
direct present) and a profile with uBlock Origin and 1Password on YouTube
and Reddit are unchanged on the X399.

### Measuring start-up on the X399: copy bundles early

A bundle copied a minute before is slower to start: a fresh copy of the
installed bundle loaded the web process's libraries in 161-195 ms where the
copy made two hours earlier took 102-115 ms (same files, alternating
launches). Freshly written files are slower to map for a while; let a new
copy sit (or compare two fresh copies) before timing start-up. And a
Speedometer run there posts its result 25 s after the page loads; the
240-300 s `x-speedo.sh` keeps the browser open is only a ceiling, so match
results to runs by the server's request log, not by when a run ended.

### Reading the start-up trace on the X399

`first frame received by the browser` counts the first frame that comes as
a bitmap (`Frame`). On the X399 the first frame goes straight into the
screen (`DirectFrame`, `SUMMIT_SWAPCHAIN_TRACE=1` shows `send=2 direct`), at
0.49 s; the 0.83 s of that trace line is a later read-back frame. The page
is on the screen at the web process's `first frame sent`.

### Open

- Quitting with YouTube open takes 14-17 s on the Pi (old and new builds):
  the page's busy main thread does not answer TryClose within the port's
  10 s, then the close is retried. (Fixed 5 October, see that section.)
- A maximised window scrolls at ~20 fps (read-back), video at ~11 fps
  (software decoding, upload and read-back of each frame).
- A browser killed while it launches a helper (load_image() takes 0.3-0.6 s
  on the Pi) leaves that helper behind for good: Haiku starts the new team's
  main thread suspended until the parent resumes it, and nothing does once
  the parent is gone. Each holds ~10 MiB (one suspended thread, its libraries
  loaded). Seen after the smoke test's quit-then-kill; a browser killed at
  any other moment takes all its helpers with it within 2-10 s
  (`tools/pi/board/orphan-test.sh`). The browser could kill such suspended
  helpers of its own executable path, whose parent is gone, when it starts.
  (Done 5 October.)
- The Pi's RAM disk (ramfs) hung around 00:25 on 5 October: a network process
  left behind by a force-quit browser and an `rm -rf` in that RAM disk sit
  in kernel waits and ignore `kill -9`; `ls`, `df` and `profile -a` hang with
  them (the profiler then holds the next process it attached to). The rest
  of the system works; a reboot clears it. Benchmarks moved to a second RAM
  disk (`RD=/boot/home/summit-ec/rd2`).

### Left for the OS image

- Mesa with its shader cache (`-Dshader-cache=enabled`; it also brings
  program binaries): the two programs of every first frame cost 200 ms per
  web process, each new program 40-150 ms.
- Presentation without the read-back (frames scanned out from GPU memory),
  or cached buffer mappings for reads: 15-29 ms per megapixel now.
- GNU hash tables for Haiku's own libraries.
- A cache of resolved symbols in the runtime loader while it relocates an
  image (above): the largest part of every process start that is left.
- arm64 string routines: `memcpy` is a C loop of 8-byte words with a byte
  loop for the tail and `memset` the generic C one (1.9% and 0.5% of the web
  process's main thread in a page load); the runtime loader's `strcmp` is 11%
  of loading a web process. Arm's optimized-routines versions would help
  every program.
- `__tls_get_addr` goes through the runtime loader on every call; a
  per-thread cache of the module's block, or TLS descriptors, would make
  thread locals of shared libraries cheap again.

## 4 October 2026 (morning): pictures that flicker while Reddit is scrolled

Reported on the workstation at 175%: scrolling a page full of pictures
(Reddit), the pictures disappear and come back. Three causes, all in the
engine, none of them particular to 175%. The build with the fixes is
**bundle-r0_tdpf3** (Mesa prefix-20261002).

### Seeing it: frames read from the screen

A screenshot shows app_server's copy of the screen, and a dump of the frame
buffer takes 4 s. `tools/bench/fbcap` (new) reads a part of the frame buffer
the card scans out, every eighth pixel of the column of posts in 20 ms, 480
or 700 times in a row; `frames.py` makes contact sheets of consecutive frames
and counts the frames with a black box where a picture belongs and the frames
that are one flat colour. `scroll-capture.sh` loads a page in a private copy
of the browser, scrolls it steadily down and back up and captures meanwhile.
(`nvscanout --band` is not a crop of the dump: it paints a test band into the
screen.)

### 1. A scroll released the layers a rendering update had replaced

With the layer set committed in every composition, as upstream does: a
picture visible in one frame, a black box in the next three to six, the
picture again. The compositor takes the page's new set of layers at the
start of every composition. A composition for a scroll alone runs while the
rendering update that brought the set still waits for its tiles (the update
is held back, the scroll is not), took the set, and released the layers the
page had replaced; their replacements are attached by the update itself
(children, tiles). Until it came, the frames showed neither: where Reddit
had made a post's picture layers anew, the post's black background.

The set is now committed with the rendering update it belongs to
(`CoordinatedSceneState::flushCompositingState`).
`SUMMIT_COMMIT_LAYERS_WITH_UPDATE=0` is the old way. In 30 s of scrolling
r/pics the trace had 2 and 3 frames without the pictures' layers before, and
the captures show the black boxes at those moments; none after.

### 2. Pictures' percentage heights laid the whole feed out again

The larger part of what the eye sees as pictures missing. Scrolled at an
ordinary speed (2.5 notches a second and more), r/pics had no rendering
update for 5 to 16 s at a time: after each batch of posts the page forces
about forty layouts in a row, 130 to 230 ms each. The compositor scrolls on
into what was never painted; when the page catches up the posts appear with
black boxes first and the pictures a moment later.

The layout kept on 3 October (an item's layout is kept when its container
asks again with nothing changed) did not apply: 12,900 block layouts for 550
changed renderers, 13 kept. A picture with a percentage height is noted with
*every* block above it whose height is automatic or a percentage
(`RenderReplaced`, for the height it resolves against is found by going up
through such blocks), and each of those blocks marks the whole chain down to
the picture for layout whenever it is laid out itself, changed or not. Under
`main` there were 85 such pictures; Reddit's grid lays `main` out five times
in one layout (to measure it, with the measured height, and again for each
repeat of the boxes above), so the whole feed was laid out five times, 35 ms
a time.

A chain is now left alone when a box between the block and the picture
settles the height the picture resolves against
(`RenderBlockFlow::dirtyForLayoutFromPercentageHeightDescendant`): a flex
item that its container gave a height for percentages, a box with an aspect
ratio, a box with a fixed height, the order
`RenderBox::availableLogicalHeightUsing()` looks in. That box is laid out by
its own container when its height changes and passes it on. Standards mode
only: in quirks mode a percentage skips blocks with automatic heights, boxes
with an aspect ratio among them, and the marks made for the picture are what
gets the boxes in between laid out (the test page without its doctype gave
different sizes until the rule was limited).

| r/pics, 120 notches at 60 ms, two runs each | before | after |
| --- | --- | --- |
| layout during the scroll | 4.3 s, 5.8 s | 0.3 s, 0.5 s |
| layouts of 20 ms and more | 37, 41 | 4, 5 |
| longest time without a rendering update | 4.7 s, 5.1 s | 1.5 s, 1.7 s |

`SUMMIT_PERCENT_HEIGHT_CHAINS=all` marks every chain as before; `=verify`
marks them too and reports, after each layout, a picture whose size came out
different although the box that settles it kept its height: none on 25 sites
(Reddit, Airbnb, YouTube, CNN and Twitch have such chains).
`SUMMIT_PERCENT_HEIGHT_TRACE=1` lists the chains marked and left;
the layout why-trace counts them (`percentChainsMarked`, `percentChainsLeft`)
and, with `SUMMIT_LAYOUT_WHY_ITEMS`, lists an item's pictures.
`tools/bench/pages/percent-height-chains.html` changes the heights around
18 such pictures in 26 steps: the same 52 readings with the rule and
without, in standards mode (711 chains left alone) and in quirks mode.

### 3. A layer of a part of the page had tiles only close to the viewport

The page's own layer is as wide as the viewport, so the area it may cover
goes into its height: tiles one and a half viewports above and below what
shows. A layer of a part of the page (Reddit composites each picture, since
a blurred copy lies behind it) covered half a viewport each way, 400 pixels
of the page, and was in view before it was painted when the page was a
little late. Such a layer now reaches as far as the page around it
(`CoordinatedBackingStoreProxy::computeCoverAndKeepRect`,
`SUMMIT_LAYER_TILE_REACH=<viewports>`, 0 for the old distance; not under
memory pressure).

### Result

Frames read from the screen while r/pics is scrolled down for 10 s (150
notches at 60 ms) and back up, 480 frames each way, two runs:

| | installed (bundle-orh0apka) | bundle-r0_tdpf3 |
| --- | --- | --- |
| down: frames with the page not painted | 267, 251 | 22, 47 |
| down: longest time without a rendering update | 6.6 s, 15.9 s | 1.4 s, 1.6 s |
| back up: frames with the page not painted | 0, 63 | 16, 25 |

What is left in the new build is the moment the next batch of posts arrives:
Reddit shows its grey placeholders for about half a second, and the last
post before them has its black box until its picture has loaded.

Not the cause, though suspected first: tiles whose upload a composition
leaves for the next (no tile waited in view in 1,600 frames; the compositor
timing trace now counts them, `waitingInView` and `texturelessInView`, and
`SUMMIT_TILE_HOLE_TRACE=1` names the layer), and decoded pictures being
discarded.

## 3-4 October 2026 (night): Reddit's layouts, style sheets from extensions

Two items left open by the sessions above: the Reddit freeze after a load
(issue #20, "engine layout performance, not fixed") and the style work that
extensions add to every page load. Both are engine changes; the build
installed with them is **bundle-orh0apka** (Mesa prefix-20261002).

### Reddit: an item's layout is kept when nothing it depends on changed

`SUMMIT_LAYOUT_WHY_TRACE=<ms>` (new) reports, for each layout of the render
tree that takes at least that long, the blocks that were laid out although
nothing in them needed it when the layout began, or more than once, by
element and container. On reddit.com/r/sydney after the next batch of posts
arrives (26 posts, 9,400 renderers), one forced layout with 126 renderers
changed laid blocks out **10,737 times**: 170 ms, and the page's components
force about thirty in a row (4.0-4.7 s in three runs, the "no rendering
update for 4.5 s").

Flex and grid containers lay their items out several times in one layout (to
measure them, again with the final size, again to stretch them) and mark the
item as needing layout each time, whether or not anything changed. Nested,
the repeats multiply. Reddit's feed is a grid in a flex box in a grid, with
posts that hold galleries (a flex box in a grid in a flex box); in that one
layout:

| box | in | layouts |
| --- | --- | --- |
| `div.subgrid-container` (flex) | `div.grid-container` (grid) | 3 |
| `main` | `div.main-container` (grid) | 15 |
| `faceplate-batch` (the posts) | `shreddit-feed` (flex) | 31 |
| `div.carousel-container` (flex) | `faceplate-carousel` (flex) | 252 |
| `li` (a gallery picture) | `ul` (grid) | 3,629 |

The engine now keeps an item's layout when its container asks for it again
with nothing changed (`RenderBlock::layout()`, Haiku only): the sizes the
container imposes are those of the item's last layout (overriding width and
height, grid area, containing block width, and the container's phase where
the item can tell), the item still has the size that layout left, every mark
on it was made by the container (the containers mark through
`summitSetNeedsLayoutByContainer()` / `summitSetChildNeedsLayoutByContainer()`),
and none of its children needs layout. Two sets of sizes count as the same
where they must give the same layout: which phase a flex container is in for
an item without percentage heights inside, and, for a plain block without
percentage heights inside, "no size imposed" against "the size it took when
none was imposed" (containers measure first and then impose what they
measured). Never kept: items with a percentage height of their own,
anonymous items (the block inside a button takes its height from the button
without an imposed size), subgrids, anything in quirks mode, paginated or
under a line clamp. `SUMMIT_LAYOUT_REUSE=0` turns it off.

`SUMMIT_LAYOUT_REUSE=verify` lays the item out anyway and reports every box
in it that the layout moved or resized. That found three ways the first
version was wrong before any page showed it (a layout made while the flex
basis stood in for the item's size; marks on an item's children that did not
come from the container, as for a percentage height whose base changed; the
button case above). With those fixed: no box different in 20 sites loaded,
scrolled and resized (YouTube home, watch and search pages, Reddit, GitHub,
The Guardian, BBC, Amazon, eBay, Google search, Gmail, Twitch and others;
12,000 kept layouts checked on YouTube's home page alone), and in
`tools/bench/pages/layout-reuse.html` (1,500 small changes to nested grids
and flex boxes, a forced layout after each; its checksum of positions is the
same with reuse on and off).

The same build with reuse on and off, four alternated runs each
(`SUMMIT_LAYOUT_WHY_TRACE=3`; "incremental" is a layout of the full feed with
under 300 renderers changed):

| | reuse on | reuse off |
| --- | --- | --- |
| incremental layout, median | 24-45 ms | 66-105 ms |
| blocks laid out in it, median | 1,476-2,874 | 3,880-6,752 |
| all layouts in the run | 0.76-1.81 s | 2.14-3.37 s |

On the page state with more galleries (the first runs of the night) a
forced layout went from 170 ms to 42 ms. None of the later runs with reuse
on stalled after the load. (A "page stall" reported right at the start of a
navigation is the wait for the first bytes, not this.) Speedometer 3.1 is
unchanged (9.52 and 9.25 against 9.45 and 9.19 for the installed build, 3
iterations, alternated).

What is left in a Reddit layout: `main` is still laid out up to three times
when a post inside it changed (measured without a height, then with the
height it measured, and it has percentage heights inside, so the two are
not the same to the engine), and items with percentage heights (each gallery
picture's `figure`) are laid out every time. Keeping measurements apart from
layouts would remove the first.

Tools on the X399 (`/boot/home/summit`): `claude-rd-run.py BUNDLE LABEL
ROUNDS [K=V]` (r/sydney without extensions, a wheel burst to bring the next
posts, counts of posts, stalls and layouts), `claude-rd-why.py` (per-layout
numbers from the trace), `claude-verify.py BUNDLE-DIR LABEL URL...` (verify
mode over a list of sites). `SUMMIT_LAYOUT_WHY_ITEMS=<text>` logs every
layout request for items whose description contains the text, with the
inputs of the last layout and the present ones.

### Style sheets from extensions are added in place

With the owner's extensions (uBlock Origin, 1Password), style resolution in
the first 12 s of a load took 1,271-1,480 ms on Reddit against 778-825 ms
without them, and 749-1,139 ms against 342-433 ms on YouTube
(`SUMMIT_PAGE_UPDATE_TRACE=3`, "Summit style phases"). uBlock adds its
cosmetic filters with `tabs.insertCSS()` four to seven times while a page
loads (`SUMMIT_EXTENSION_CSS_TRACE=1`), the last three once the page is
built, and each of those was followed by a style update of 80-116 ms. Two
causes:

- A sheet added to a page made the document throw away every style
  resolver, those of its shadow trees too, and resolve the style of every
  element again (`DocumentScope::didChangeExtensionStyleSheets()`).
- After giving the sheet to its page, the web process told every document of
  every page it holds to build its injected sheets again and resolve all
  styles again (`WebUserContentController::addUserStyleSheets()`), although
  a sheet for one page is in no other document's sheets. That also undid
  any cheaper update of the page itself.

Now a sheet for one page reaches only that page, and when it holds only
style rules (also inside `@media` and `@supports`) and keyframes and comes
last among the page's injected sheets, it is added to the existing resolver:
the user rules are built again from the injected sheets, the resolvers'
features collected again, and only elements the new sheet's selectors can
match are restyled, in the document and in each shadow tree, as for a style
sheet the page itself adds. A sheet whose selectors can match anything (one
of uBlock's has such a rule) still restyles everything, but no resolver is
rebuilt (Reddit: 379 shadow trees sharing 67 resolvers). Removing a sheet,
and sheets with other rules (font faces, layers, imports), go the old way.
`SUMMIT_INCREMENTAL_EXTENSION_CSS=0` goes the old way always; `=verify` then
resolves everything the old way as well and reports each element for which
21 properties (display, visibility, opacity, position, sizes, colours,
transform, filter and others) came out different. On YouTube, Reddit, The
Guardian, BBC News and GitHub that reported only a text field's placeholder
(its width is set by layout) and a spinner in mid-rotation.

Three alternated loads each with the owner's extensions, style resolution in
the first 12 s:

| | in place | old way |
| --- | --- | --- |
| YouTube | 510-524 ms | 671-716 ms |
| Reddit | 1,501-1,628 ms | 1,194-1,675 ms |

YouTube saves about 170 ms of main thread per load. On Reddit the
difference is inside the spread of its loads (the page keeps building
itself for seconds, and the sheet that matters there is the one that can
match anything). The trace now says which way each sheet went and why.

### Measured and not kept

- **A wide blur made at a reduced size** (scale down by 2-8, blur, scale
  back up) for the view-transition snapshots that YouTube's watch page paints
  on the CPU: it looked the same and was slower, 165 ms against 136 ms a
  frame for six blurred boxes at 200%. Skia's CPU blur already costs about
  what the scaling does; the cost of those snapshots is the number of pixels
  times the number of frames, not the blur's radius.
- **Showing a page once it has drawn a skeleton** (six boxes with a
  background of their own counting as "visually non-empty"): YouTube's home
  page today has no such boxes before its first image, which is what shows
  it; nothing changed and the rule was taken out again.
  `SUMMIT_FIRST_SHOWN_TRACE=1` (kept) reports when a page had enough to be
  shown in place of the one before it and what counted.

The X399's boot volume could not take another bundle ("File too large" with
10 GB free: too fragmented at 95% full). 435 bundles from earlier sessions
were removed from `/boot/home/summit/build-modern-browser` (about 100 GB);
the installed one, those the launcher backups name and the last few were
kept.

## 3 October 2026 (evening): issue #22, YouTube with uBlock Origin

The owner's report: YouTube's home page loads choppily, scrolls badly, and a
video clicked on it starts after a long pause. Measured on the X399 with
copies of the owner's profile (uBlock Origin 1.75.0 and 1Password, Firefox
builds), a fresh browser per run, a scripted click on the first thumbnail.

**Not a Summit regression.** The build of 2 October evening
(bundle-ywvpzj7_) did no better than the installed one: three alternated
rounds each, click to first video frame 2.65-3.47 s against 2.35-2.82 s.

**The extensions.** Click to first video frame, by profile:

| extensions | click to first frame |
| --- | --- |
| none | 0.66-0.79 s |
| 1Password only | 1.0-1.8 s |
| uBlock Origin only | 3.2-10.7 s |
| both | 2.5-2.8 s |

uBlock's blocking webRequest decisions were not it (1-3 ms each,
`SUMMIT_WEBREQUEST_TRACE=1`). The page's main thread was: in the 5 s after a
click, 1.4 s in calls through Proxies, 1.0 s in `JSON.parse`, 0.7 s
recording stacks for thrown errors. `SUMMIT_JS_THROW_TRACE=N` (every Nth
exception with its stack) found 1.6 million SyntaxErrors in an 80 s run,
126,000 a second after the click and 54,000 a second while the video
played: `JSON.parse(undefined)` inside uBlock's `editObj`.

**Cause.** One of uBlock's YouTube filters,
`trusted-edit-inbound-object, JSON.stringify, 0, ...`, traps
`JSON.stringify` and clones each argument with `JSON.parse(JSON.stringify(x))`,
catching the error when the value does not serialize. That costs one error
per call, as designed. But uBlock's Firefox build checks that its scriptlets
ran in the page through `self.wrappedJSObject[sentinel]` before it records
them as injected (`uBO_scriptletsInjected`), and Summit's content scripts had
no `wrappedJSObject`. The check threw, nothing was recorded, and each later
trigger (response headers, navigation committed, the content script
starting) injected the scriptlets again. Every injection wrapped
`JSON.stringify` once more, and each layer's "safe" `JSON.stringify` was the
layer below: 2^N errors per call. That this showed now is likely a filter
list update, not a browser change.

**Fixes** (commit d633372 and the bundle installed with it):

- Content scripts get a `wrappedJSObject` (WebExtensionControllerProxy):
  a Proxy whose reads, writes, `in` and `delete` reach the page's global
  object, with plain values only (strings, numbers, booleans, null,
  undefined); the page's objects never cross into the extension's world.
  uBlock now injects once: 35,000 errors in a run instead of 320,000 to
  1.6 million. (1Password only tests for `cloneInto`, which is still absent.)
- Errors are cheaper: an exception reuses the frames its Error recorded
  instead of walking the stack again; the frame vector doubles instead of
  growing by a quarter (six copies of every frame for a stack 60 deep);
  Summit gives pages 10 frames in `error.stack`, as Chrome does
  (`JSC_defaultErrorStackTraceLimit=10`, set in `main()` unless already set).
  A throw-and-catch loop 60 calls deep: 1.25 million in 12 s before, 6.7
  million after.
- A call through a Proxy no longer makes "apply" an atom string each time
  (a quarter of the call; 259 -> 236 ns, `tools/bench/pages/proxy-calls.html`).
- The bytecode cache (3 October morning) encoded its functions in 4 ms slices
  back to back, 570-700 ms of YouTube's main thread while the home page was
  still filling in. Slices now wait at least 12 ms after each other, twice as
  long each time one ran late (up to half a second).

With both extensions, three alternated rounds:

| | click to player | click to playback start |
| --- | --- | --- |
| installed build (bundle-6hq57edu) | 1.88-2.20 s | 2.77-3.30 s |
| this build | 1.49-1.59 s | 2.01-2.77 s (2.77: a seek to 75 s first) |

What is left: uBlock's four `JSON.stringify` traps (one per filter) still
throw once each per call, and its JSON-path filters walk YouTube's request
objects (as fast as V8 on this machine: 180 us vs 107 us per small object).
1Password still adds 0.3-1 s to a click. YouTube's watch page also starts a
view transition, whose snapshots WebKit paints on the CPU at 200% with the
ambient-mode blur, 0.6-0.8 s of main thread per click
(`ViewTransition::updatePseudoElementRenderers` snapshots the new state every
frame when the capture clips its overflow; `ImageBufferSkiaAcceleratedBackend`
gives GPU buffers to canvases only). Turning view transitions off did not
bring the first frame sooner (1.39 vs 1.43 s), so it is a smoothness item for
later, not the start delay.

Issues #23 (Netflix) and #24 (Disney+) need Encrypted Media Extensions with a
licensed DRM module; Summit has neither, and Widevine's license does not allow
using it without an agreement with Google. Nothing was built for them.

## 3 October 2026 (afternoon): issues #19, #20 and #21

All three were filed against bundle-7dlqk_lh, the first build that presented
pages straight into the screen (#16). Installed at the end:
**bundle-6hq57edu** (Mesa prefix-20261002), and in the Haiku fork an
app_server and `nvidia_rm.accelerant` with a hardware cursor.

**#19, the pointer over a video.** The pointer was a software cursor drawn
into the frame buffer, which the direct frames painted over, and moving it
put back old pixels. The X399 now has a hardware cursor (details in
[hidpi.md](hidpi.md#the-pointer-and-old-pixels-issues-19-and-20-3-october-2026)).

**#20, pages that did not load fully.** Two causes.

- *Old pixels on the screen.* Whatever app_server redrew in a window (a
  window moved off it, a menu closing, the screen blanker going away) it
  copied from its own copy of the screen, which direct frames never wrote,
  so parts of a page showed another window, black, or an earlier frame,
  around whatever kept changing; a navigation could leave most of the
  previous page standing around the new one's video in screenshots. Fixed
  in the engine (same section of hidpi.md).
- *Reddit stops drawing for 3-4 s after it loads.* reddit.com/r/sydney
  (the version with 27 posts at load, 5 of them ads) draws nothing for 3.4
  to 5.6 s at a time ("Summit page stall: ... no rendering update for 3.6
  s"). The page's own components (Lit `firstUpdated`, `measureViewChange`,
  `insetWidth` in the redditstatic.com chunks) read element geometry about
  400 times in 10 s from a chain of microtasks that never yields to
  rendering, each read just after another component changed the page, so
  each forces a layout: 60 ms at first, growing to 160-360 ms as posts are
  added (`SUMMIT_PAGE_UPDATE_TRACE=3` logs every forced layout over 5 ms),
  about 35 in a row. The time is WebKit's flex and grid layout (intrinsic
  track sizing lays out every grid item again; blocks inside inline content)
  - engine layout performance, not fixed. An earlier conclusion that uBlock
  Origin caused it was wrong: runs without uBlock happened to get the page
  version with 3 posts; with 27 posts it stalls with no extensions at all.
  uBlock adds its cosmetic filters in six `tabs.insertCSS()` calls within
  0.3 s (`SUMMIT_EXTENSION_CSS_TRACE=1`); each makes WebKit rebuild every
  style resolver of the page, shadow trees' too
  (`DocumentScope::didChangeExtensionStyleSheets()`), a candidate for
  incremental insertion on pages made of web components.
- Found on the way: when JavaScriptCore dropped a script from its code cache
  it asked the bytecode cache to write the script's file, and every function
  compiled since was encoded right there (about 70 us each, so seconds for a
  script with thousands). They are now encoded 4 ms at a time like the
  scheduled write (`CachedScriptSourceProviderHaiku.cpp`). In these runs the
  biggest such write was 936 functions, 41-65 ms, so this was not the Reddit
  stall.
- The "in view but rendering frozen for 10.0 s" reports seen after most
  navigations to another site were the previous page, frozen in the
  back/forward cache; they are no longer reported, and the report now names
  the freeze reasons.

**#21, ABC News video.** The article's video is JW Player 8.38 with plain MP4
files. JW Player first checks whether it may play with a tiny clip in a
`blob:` URL, and Summit's media player handed `blob:` and `data:` URLs to
Media Kit, which cannot read them: `play()` was refused with
`NotSupportedError`, JW Player reported `autostartNotAllowed`
(`autoplayDisabled`) and the page took the player away again. Such URLs are
now fetched whole through WebCore's media resource loader into a file first;
the check resolves in about 65 ms and the article's video plays (25 frames/s,
31.6 s clip). `SUMMIT_MEDIA_PLAYBACK_TRACE=1` reports the fetch. JW Player
also waits until half its player is in view before starting.

How it was measured: tools on the X399 under `/boot/home/summit`
(`claude-d2-*`): private bundle copies, copies of the owner's profile
(private, never left the machine), `tools/bench/fbgrab.py` for what is
really on the screen (a VNC screenshot is app_server's copy) and
`profile -a -f` with `SUMMIT_DIRECT_PRESENT=0`.

## 3 October 2026: start-up, process launches and first bytes

In short (details below):

- Start-up: first frame 1.06-1.15 s (installed build of 2 October) ->
  0.65-0.69 s (launcher thread, GNU hash tables, functions bound inside
  their libraries).
- Speedometer 3.1: 7.9-8.2 -> 11.2-11.3, from binding the libraries' own
  function calls at link time (`-Bsymbolic-functions`).
- Page loads: connections opened ahead of need (preconnect, DNS prefetch,
  the address bar), brotli and zstd, no AVIF decode on the main thread to
  learn a size, first images decoded off the main thread, the Media Kit's
  formats read on a thread. Summit paints first sooner than Firefox on
  five of six test pages and reaches DOMContentLoaded sooner on all six.
- Responsiveness: the profile is saved off the window threads; the
  largest-contentful-paint entry is no longer lost; the first frame after
  a pause goes straight to the screen.
- Scripts: compiled bytecode kept on disk between visits (JavaScriptCore's
  `CachedBytecode`, which WebCore never used): YouTube's warm loads reach
  DOMContentLoaded about 270 ms sooner and their largest paint 360 ms
  sooner.
- Benchmarks: run-pageload.py alternates which browser loads first (some
  servers answer the second request from a cache the first one warmed).

Measured on the X399 with the browser started on about:blank, from the
launch to the first composited frame (three runs each):

| | browser up | web and network processes up | first frame |
| --- | --- | --- | --- |
| installed build (fork + exec, SysV hash) | 0.07 s | 0.51 s | 1.06-1.15 s |
| load_image() on the main thread, one launch at a time | 0.07 s | network 0.51, web 0.84 s | 1.34-1.44 s |
| load_image() on a launcher thread, launches at once | 0.07 s | 0.50 s | 1.17-1.21 s |
| ... and GNU hash tables | 0.07 s | 0.37 s | 0.84-0.88 s |

- **Launching with load_image().** WebKit's launcher forked the browser and
  exec'd the child. A fork copies the parent's address space, and with the
  window a direct window (issue #16) that included the frame buffer mapping,
  which hung the machine. `load_image()` starts a new team instead, but it
  returns only when the child has loaded its libraries (a third of a second
  for a web process): it now runs on a launcher thread, so the UI is not
  blocked, and launches run at once. A child may then inherit another
  launch's IPC endpoint, so each one closes every inherited descriptor but
  its own first (`AuxiliaryProcessMainCommon::parseCommandLine`).
- **GNU hash tables** (`-Wl,--hash-style=both`, engine `OptionsHaiku.cmake`
  and `build-modern-browser.py`). Haiku's runtime_loader binds every symbol
  when it loads an image (no lazy binding), and libWebKit has 257,328
  symbol relocations. It already supports DT_GNU_HASH, whose bloom filter
  settles most lookups that miss an image; the toolchain's default was SysV
  only. 100 ms off the browser's own start and 230 ms off the first frame,
  and every web process for a new tab starts that much sooner. Only a
  relink: no object changes.
- **Functions bound inside their library** (`-Wl,-Bsymbolic-functions`
  for the engine's shared libraries, `OptionsHaiku.cmake`). A library's
  calls to its own exported functions went through the global symbol
  lookup at every load; bound at link time, libWebKit's symbol
  relocations fall from 355,762 to 111,373 and libJavaScriptCore's from
  52,362 to 8,541. A web process loads its libraries in 162 ms instead of
  263 (`WebProcess` run without arguments), and the browser, its network
  process and its web process all load libWebKit: the first frame after
  launch came at 0.65-0.69 s instead of 0.88-0.92. Again only a relink
  (12 s for libWebKit). Data symbols are still looked up, and nothing
  outside the libraries replaces their functions; a browsing session,
  uBlock Origin and the page-load run behaved as before.

| | first frame after launch |
| --- | --- |
| GNU hash tables (3 October, morning) | 0.84-0.92 s |
| ... and functions bound inside their library | 0.65-0.69 s |

It also made the engine itself faster. A call from one function of
libWebKit to another exported one went through the PLT and a GOT entry,
since another image could have replaced it; bound at link time, the linker
turns most of them into direct calls. Speedometer 3.1 (local copy, 5
iterations, `run-speedometer.py --port 8961` with the Mesa environment),
alternated:

| build | Speedometer 3.1 |
| --- | --- |
| just before the relink (`bundle-4nbt7v3e`) | 7.66 ± 0.68, 8.13 ± 0.79 |
| just after it (`bundle-hx2ws_mn`) | 10.9 ± 1.5, 11.3 ± 1.4 |
| installed on the X399 before today (`bundle-ywvpzj7_`) | 8.21 ± 0.90, 7.86 ± 0.78 |
| this morning's last build (`bundle-scnjbd30`) | 11.2 ± 1.4, 11.3 ± 1.4 |

About 40 % more: Firefox 155 scored 8.34 ± 0.37 here in September.

The compiler side of the same idea did not build. The Haiku build exports
nearly every function (no `-fvisibility=hidden`: 173,570 in libWebKit), and
with `-fPIC` GCC assumes each could be replaced, so it neither inlines nor
binds them; `-fno-semantic-interposition` lifts that. With the profile
(`-fprofile-use`) GCC 13 crashed on it, deterministically, in Skia and a
dozen WebCore unified sources (internal compiler error, segmentation
violation at the end of a function); `-fno-ipa-pure-const` avoided the
crash in Skia but not in WebCore. Left out; a newer GCC, or hidden
visibility with explicit exports, would be the way back to it.

**First bytes are the network's.** Time to first byte was 400-700 ms in
Summit where Firefox had 160-200 ms on 1 October:

- *Wikipedia* is 191 ms away from the X399 (ping): connect, TLS 1.3 and
  the request are a round trip each, and curl on the machine takes 640 ms,
  as Summit does (654). Firefox would save a round trip with HTTP/3, which
  Summit's libcurl does not have.
- *GitHub* answers the first request after a pause in 700 ms whoever asks
  (curl: 745 ms, then 105 ms for the next ones, 25 s apart): its edge
  caches the page. The comparison loaded Firefox right after Summit, so
  Firefox met the cache Summit had warmed. With Summit's exact request
  headers curl gets the page in 110 ms.
- The page-load script now records the navigation's phases (DNS, connect,
  TLS, request, response) and takes `SUMMIT_BENCH_EXTRA_ENV` for traces.
- *The Guardian* varies its pages on the User-Agent (`Vary:
  Accept-Encoding,User-Agent`, `max-age=60`) and renders each one for
  0.7-1.3 s (`x-timer`): a request with a User-Agent nobody sent in the
  last minute waits for that, the next one gets the page in 70 ms. A
  mainstream Safari or Firefox User-Agent was no faster from here. Since
  the same holds for GitHub, the comparison now alternates which browser
  loads a page first, round by round, and records the four resources that
  finished last and the element of the largest paint.

**Saving the profile on its own thread.** `SharedProfile::Save` wrote the
whole profile (up to 2,000 history entries, bookmarks, every window's
tabs) as JSON and waited for the disk (`fsync`) on the thread that asked:
a window thread every five seconds after any visit, on closing or moving
a tab, and once in every window's constructor, before the first window
was shown; the application thread (which runs WebKit's main loop) when a
window closed. One such save in the owner's log took 97 ms. Saves now go
to a thread of the profile's own, which waits 250 ms to take whatever
else changes with them; quitting writes what is left before the process
exits. Clearing history still writes at once.

### Against Firefox at the end of the morning

`run-pageload.py --rounds 3 --browser both` on the final build
(`bundle-scnjbd30`; browsers alternating first, medians, ms; cold / warm):

| page | browser | first paint | largest paint | DOMContentLoaded |
| --- | --- | --- | --- | --- |
| Reddit | Summit | 870 / 922 | 1860 / 1752 | 1041 / 1066 |
| Reddit | Firefox | 1059 / 1266 | 1415 / 1352 | 1078 / 1318 |
| The Guardian | Summit | 293 / 902 | 1088 / 1880 | 707 / 1204 |
| The Guardian | Firefox | 1179 / 1213 | 1413 / 1385 | 1427 / 1491 |
| YouTube | Summit | 1376 / 1771 | 2991 / 1771 | 2290 / 1275 |
| YouTube | Firefox | 977 / 1106 | 2752 / 2845 | 2320 / 2437 |
| Wikipedia | Summit | 1286 / 958 | 1380 / 1103 | 1146 / 1069 |
| Wikipedia | Firefox | 1387 / 1146 | 1645 / 1228 | 1411 / 1150 |
| BBC News | Summit | 171 / 304 | 1028 / 972 | 443 / 612 |
| BBC News | Firefox | 402 / 430 | 919 / 762 | 1324 / 762 |
| GitHub | Summit | 1074 / 835 | 1074 / 835 | 1429 / 1168 |
| GitHub | Firefox | 1292 / 1246 | 2044 / 1832 | 1679 / 1556 |

Summit paints first sooner than Firefox on five of the six pages and is
ready (DOMContentLoaded) sooner on all six. Still behind: YouTube's first
paint, Reddit's largest paint, and The Guardian's largest paint when warm.
(The Guardian's cold first byte, 107 against 759 ms, is its server's
per-User-Agent cache, not Summit.)

### Against Firefox at the end of the session

The same run on the last build (`bundle-7dlqk_lh`, with the bytecode
cache below; 10:00, medians, ms, cold / warm):

| page | browser | first paint | largest paint | DOMContentLoaded |
| --- | --- | --- | --- | --- |
| Reddit | Summit | 877 / 982 | 2048 / 1968 | 984 / 1216 |
| Reddit | Firefox | 832 / 1085 | 1178 / 1538 | 992 / 1464 |
| The Guardian | Summit | 962 / 639 | 1864 / 1344 | 1136 / 609 |
| The Guardian | Firefox | 409 / 816 | 645 / 998 | 717 / 1059 |
| YouTube | Summit | 1242 / 1420 | 2948 / 1420 | 2273 / 1018 |
| YouTube | Firefox | 952 / 1072 | 2616 / 2794 | 2304 / 2404 |
| Wikipedia | Summit | 1308 / 1054 | 1396 / 1211 | 1144 / 1171 |
| Wikipedia | Firefox | 1382 / 1159 | 1649 / 1306 | 1410 / 1164 |
| BBC News | Summit | 307 / 369 | 1288 / 944 | 702 / 591 |
| BBC News | Firefox | 295 / 481 | 872 / 838 | 1192 / 846 |
| GitHub | Summit | 1132 / 1190 | 1132 / 1190 | 1487 / 1496 |
| GitHub | Firefox | – / 3160 | – / 4312 | 1317 / 3284 |

Warm YouTube loads now finish (DOMContentLoaded, largest paint) in less
than half Firefox's time. The Guardian's cold row is its server's
per-User-Agent cache again (first byte 660 ms for Summit, 127 for
Firefox), and GitHub's edge was slow for Firefox's warm rounds (first
byte 2.1 s). Still behind: Reddit's largest paint, BBC News's cold one,
and YouTube's first paint (see the end of the bytecode section).

### Connections opened before they are needed

Summit never connected ahead of a request. WebKit's preconnects (`<link
rel=preconnect>`, `Link: <...>; rel=preconnect` headers, and the connection
the network process opens to a page being navigated to while the web
process gets ready) are compiled only where `ENABLE(SERVER_PRECONNECT)` is
on, the GLib and Cocoa ports; `<link rel=dns-prefetch>` reached an empty
`DNSResolveQueueCurl::platformResolve`. Every page paid DNS, TCP and TLS
(two or three round trips) for each server at the moment it first needed
it.

libcurl has no preconnect, and a `CURLOPT_CONNECT_ONLY` connection is never
reused for another transfer (`url_match_connect_config`). A preconnect is
now an ordinary transfer that stops in `CURLOPT_PREREQFUNCTION`, which
libcurl calls once the connection is up (DNS, TCP, TLS, ALPN) and before
anything is sent. A transfer that ends early leaves libcurl unsure of the
connection's state, so it keeps it only if it is multiplexed: HTTP/2
connections stay in the pool; HTTP/1.1 ones are closed, and their servers
are remembered (from preconnects and from every response) and not
preconnected again. The `NetworkProcess` code is enabled with
`ENABLE(SERVER_PRECONNECT) || PLATFORM(HAIKU)` in the five places that
need it, rather than the feature flag that would rebuild all of WebKit,
and the two link preferences are set in `WebView.cpp`. DNS prefetch
resolves into libcurl's shared DNS cache with a connect-only transfer
whose socket callback refuses the socket. The address bar has the
network connect to where Enter would go (a page completed from history,
the search engine, a suggestion chosen with the arrow keys; not in
private windows), through a new `BWebKitView::Preconnect()`.
`SUMMIT_PRECONNECT=0` turns all of it off.

On The Guardian (`SUMMIT_NET_TRACE=1`) the page's own connection was up
80 ms after the navigation began; its ten `Link:` and `<link>` preconnects
were all HTTP/2 and up within 40-180 ms of the response headers; every
later request to those servers went out on them (`dns=0 connect=0`).
Typing a search into the address bar connected to DuckDuckGo, and a
completion from history to its site, before Enter.

Three alternating pairs of `run-pageload.py --browser summit --rounds 2`,
with and without `SUMMIT_PRECONNECT=0` (medians of three, ms; cold = empty
cache):

| page | | first byte | FCP | LCP | DOMContentLoaded | load |
| --- | --- | --- | --- | --- | --- | --- |
| Wikipedia, cold | on / off | 650 / 842 | 1320 / 1509 | 1439 / 1605 | 1147 / 1339 | 2766 / 3239 |
| The Guardian, warm | on / off | 464 / 720 | 905 / 907 | 1864 / 2128 | 1259 / 1505 | 6866 / 5918 |
| BBC News, cold | on / off | 96 / 112 | 323 / 335 | 1236 / 1204 | 746 / 600 | 1588 / 14048 |
| BBC News, warm | on / off | 234 / 287 | 458 / 776 | 1272 / 1292 | 687 / 706 | 1487 / 5787 |

The other pages (Reddit, YouTube, GitHub, and the rounds not shown)
moved by less than their own spread. The page's own connection gains
little in a running browser: the network process starts the
main-resource preconnect only 2-43 ms before the web process asks for
the page (`SUMMIT_NET_TRACE`); Wikipedia's cold gain is mostly a slow
first DNS answer (213 ms against 25) that the preconnect took off the
navigation's timeline. The gains come from the hints: the connections a
page names are open by the time its parser reaches the resources.
YouTube and Google pages would gain more from brotli (YouTube's front
page is 98 KB as brotli against 255 KB as gzip, 25-60 ms to download
here) and HTTP/3; the system libcurl has neither.

### Brotli and zstd

The system libcurl (8.22) is built with zlib only, and Summit let it
decode responses, so it asked for `gzip, deflate` where every other
browser asks for brotli too (Chrome and Firefox zstd as well). Sites
answer what is asked: YouTube's front page is 98 KB as brotli and 255 KB
as gzip; Google, MDN and Cloudflare send brotli, Facebook and Instagram
zstd. And a CDN that keeps a pre-compressed copy of a large script for
the common encodings may compress the gzip one on the fly: YouTube's
2.5 MB main script came at 3.5 MB/s in Summit's cold loads, Firefox's
brotli copy of it at 24 MB/s.

`CurlRequest` now switches libcurl's content decoding off
(`CURLOPT_HTTP_CONTENT_DECODING`), asks for `gzip, deflate, br, zstd`
itself, and undoes the codings named by `Content-Encoding` as the body
arrives: zlib for gzip (several members too) and deflate (with or without
the zlib header), libbrotlidec and libzstd loaded from the system on first
use (when they are missing those two are not asked for). A request for a
range asks for `identity`, since a coding applies to the whole resource.
`SUMMIT_CONTENT_ENCODINGS=curl` goes back to libcurl's decoding. A test
server sending one 3 MB text in every coding (gzip, two gzip members,
zlib, raw deflate, br, zstd, `gzip, br`) got it back intact in each, and
YouTube, Facebook, Google, MDN, the Cloudflare blog and Instagram load.

Three alternating pairs against `SUMMIT_CONTENT_ENCODINGS=curl` (medians,
ms): YouTube's cold load transfers 3.3 MB instead of 3.9 and reaches its
largest paint at 2852 instead of 3101 (DOMContentLoaded 2168 / 2383, first
paint 1321 / 1392); Google's cold DOMContentLoaded came at 695 instead of
1366 and its largest paint at 800 instead of 918. Facebook, MDN, the
Cloudflare blog and Reddit moved within their spread; warm loads come from
the cache, which keeps bodies decoded.

### Pictures decoded on the main thread

A system profile of the web process while The Guardian's front page loads
(`profile -a`, 4 s, browser started first so its images are known) had the
main thread busy for 2.5 s, 92-95 ms of it in dav1d, the AV1 decoder
behind AVIF. Two causes:

- **AVIF decoded to learn its size.** `AVIFImageReader::parseHeader`, which
  WebKit calls on the main thread as an image's data arrives to find its
  size, ran `avifDecoderNextImage()`: a full decode of the first image.
  `decodeFrame()` then decoded it again for drawing. `avifDecoderParse()`
  already gives width and height; the header no longer decodes. dav1d no
  longer appears on the main thread at all (the same code is in the GTK
  and WPE ports).
- **Small pictures decoded in the paint.** WebKit decodes an image on its
  decoder thread only if it is large (500 KB decoded) and only when the
  layer is painted for the first time (`RenderLayerBacking`); Cocoa's tiles
  ask for it on each tile's first paint. Tiles are now painted with
  `DefaultAsynchronousImageDecode` and anything over 32 KB decoded counts:
  an element's first image is decoded off the main thread and painted when
  ready. Elements already showing an image still decode in place, so
  nothing flickers.

Against the build before both changes (two alternating pairs, medians,
ms): The Guardian's DOMContentLoaded came at 1302 instead of 1502 cold and
316 instead of 394 warm, its largest paint a little sooner (1842 / 1890,
978 / 1056); BBC News and YouTube moved within their spread (their load
events follow ad chains).

**The Media Kit's formats, read on a thread.** The first time a page asks
what it can play, the Haiku media engine lists the Media Kit's file
formats, which loads its add-ons into the web process: 27 ms, on the main
thread, symbol lookups and relocations in the profile. Each web process
now reads them on a thread of its own as it starts (a prewarmed one well
before its page); `canPlayType()` answers as before.

Still open: on Reddit the main thread spends 100-115 ms in libwebp, under
`RenderLayerBacking::updateImageContents`: an image the compositor shows
directly (a layer of its own) is handed to it decoded, and
`GraphicsLayerCoordinated::setContentsToImage` decodes it there and then.
Asking the decoder's thread first and setting the layer's contents when it
was done did not take the time off the main thread (probably animated
images, which it left alone), and was taken out again.

The same profile shows two smaller costs that were left alone: every
`thread_local` in a library goes through `__tls_get_addr` into
runtime_loader's `get_tls_address` (Haiku has no static TLS), about 2% of
the main thread, mostly mimalloc's default heap; and libWebKit and
libJavaScriptCore each link their own copy of mimalloc (WTF and bmalloc
are static in both), which rules out the easy fix of giving mimalloc fixed
native TLS slots (`%fs` on Haiku).

### Largest contentful paint was reported late, not painted late

With the order alternated, Summit's first byte, first paint and
DOMContentLoaded were level with Firefox's or ahead on all six pages;
the largest contentful paint (LCP) was not: 2036 ms against 1299 on The
Guardian. The page's resource timings said its lead image (an 88 KB AVIF)
had arrived after 243 ms, and a test page showed the decoders are quick
(1240x992: AVIF 48 ms, JPEG 25, WebP 22).

WebKit makes an LCP entry in the rendering update after the one that
painted the element: painting (`LargestContentfulPaintData::didPaintImage`,
`didPaintText`) asks for another update. Painting runs inside the update,
and `FrameRenderer::scheduleRenderingUpdateRunLoopObserver()` dropped any
request made while an update ran, so the entry waited for whatever updated
the page next, or for ever on a page that went still. The request is now
remembered and made again when the update ends (`LayerTreeHost`). A test
page's image is reported 112-116 ms after the navigation began instead of
232-236. The other ports built on `FrameRenderer` (GTK, WPE) drop it the
same way.

On The Guardian itself the fix moved little (LCP 2056 / 1468 ms cold /
warm afterwards): there the candidates (headlines, then the image) are
reported between 0.7 and 2.0 s for a reason not yet found. Part of it is
the layer tree staying frozen until the first visually non-empty layout,
which on a cold load waits for fonts the page's CSS asks for late (the
Text Sans faces, about 1 s in), as in Safari; Firefox shows text in a
fallback font meanwhile. The image decoder's queue runs at normal
priority, so it is not starved by the page.

**The first frame after a pause went through app_server.** Direct
presentation still sends a changed page through app_server a few times a
second, read back from the GPU, so that app_server's copy of the screen
(screenshots, the VNC server, windows moved away) stays current. The rule
picked the first changed frame after a quarter second of stillness, which
is the frame a reader waits for (a page's first paint, the answer to a
click): on The Guardian that frame took 134 ms to composite, 84 of them
reading it back. Now a frame goes through app_server only once changes
have waited 250 ms for it; a page that stops changing gets its last frame
there 300 ms later as before. Screenshots of settled pages are unchanged.

### Compiled scripts kept between visits

On a warm load of YouTube (everything in the cache) Summit reached
DOMContentLoaded at 1.27-1.35 s and painted first at 1.78-1.80 s, while
Firefox painted at 1.23 s. The HTML was in by 135 ms; the parser then
spent a second in YouTube's scripts. A system profile of the web process
(`profile -a`, warm load) had JavaScriptCore compiling for 720 ms of the
835 it spent evaluating scripts, 470 of them in the parser: 240 ms
checking the 10.8 MB main script's syntax, 230 ms parsing functions again
as each first ran.
Every visit compiled them from scratch; Firefox and Chrome keep compiled
code in their caches.

JavaScriptCore can already keep its bytecode on disk (`CachedBytecode`:
the program's bytecode when it is compiled and each function's as it is
compiled later, read back lazily), but only through four `SourceProvider`
hooks that the jsc shell and the JSScript API implement and WebCore does
not. `CachedScriptSourceProvider` (scripts a page loads, classic and
module) now implements them on Haiku
(`CachedScriptSourceProviderHaiku.cpp`):

- Scripts of 20 KB and more, in persistent profiles only, one file each in
  `<profile>/WebKit/Cache/JavaScript/<build>/`, named by the source's hash
  and length. JavaScriptCore compares the whole source text before using
  a file, so a different script with the same name is only a miss.
- Its own version check uses the library's GNU build ID, which these
  libraries do not have; the directory is named after libJavaScriptCore's
  size and time too, and other builds' directories are deleted.
- Written once per script and process, five seconds after it first ran:
  the program and every function compiled by then. The next visit reads
  that file and adds the functions it compiles. A function's entry is
  patched inside the old file, which other processes (and this one) may be
  decoding from as mapped, so the new file is put together in memory (44
  MB for YouTube's main script, 23-30 ms) and a thread of its own writes it
  beside the old one, syncs it and renames it over it (100-150 ms). BFS
  journals only metadata: without the sync, a power cut could leave a
  renamed file holding stale blocks, which JavaScriptCore would decode.
  A visit that adds less than a twentieth to a file does not rewrite it.
- Encoding is the cost. The program takes little (6.6 ms for YouTube's
  main script), but each function compiled from it took 70 µs to encode:
  402 ms for the 5,869 that YouTube compiles in its first seconds. Done as
  each was compiled, that put YouTube's first visit 130-290 ms later to
  DOMContentLoaded. The functions are now only noted as they compile and
  encoded when the file is written, 4 ms at a time between the main loop's
  other work.
- 320 MB at most (oldest files go first, down to 240 MB), checked 30 s
  after a web process first runs a script. Clearing the cache in
  Preferences removes it. `SUMMIT_BYTECODE_CACHE=0` turns it off;
  `SUMMIT_BYTECODE_CACHE_TRACE=1` reports each read and write.

Two alternating pairs of `run-pageload.py --browser summit --rounds 3`
against `SUMMIT_BYTECODE_CACHE=0` (medians, ms; warm = rounds 2 and 3,
which read the files the first round wrote):

| YouTube | DOMContentLoaded | largest paint | first paint |
| --- | --- | --- | --- |
| warm, cache | 1050, 1118 | 1452, 1638 | 1452, 949 |
| warm, no cache | 1342, 1326 | 1937, 1875 | 1164, 1875 |
| cold, cache | 2173, 2119 | 2793, 2795 | 1173, 1136 |
| cold, no cache | 2169, 2192 | 2826, 2900 | 1192, 1209 |

First paint on warm YouTube loads varies widely either way. The other
five pages moved within their spread (their scripts are smaller or
cached in memory by the time they matter). Within one browser
JavaScriptCore already kept compiled scripts in memory, so the files help
a site's first visit in each new web process: after a restart, in a new
tab, after browsing elsewhere.

YouTube's first paint is still later than Firefox's for another reason.
WebKit keeps the previous page on screen until the new one is "visually
non-empty" (`LocalFrameView::checkAndDispatchDidReachVisuallyNonEmptyState`:
200 characters of text, an image or a fixed-size SVG, or parsing done with
no style sheets or fonts pending; text in a web font that is still loading
does not count). YouTube's skeleton is grey boxes, so nothing is shown
until its scripts have rendered text, where Firefox paints the skeleton.
Safari behaves as Summit does; it was left alone.

## 1 October 2026: page loads against Firefox (issue #2)

`tools/bench/run-pageload.py` loads complex sites in Summit and in Firefox
155 on the X399, alternately, three rounds, each load in a fresh browser on
a profile kept for the run (round one meets an empty cache). The same
script reads the page's own timings in both browsers after the load event
and five seconds more: Summit through summitctl and its Developer Tools
console (opened after the load), Firefox through Marionette
(`tools/bench/marionette_client.py`). Window 1912x1052 logical, 200%;
bundle `bundle-5wfht_91`
(`.vm/bench/pageload-20261001-001217-compare/`). Medians, ms:

| page | browser | FCP cold / warm | LCP cold / warm | load cold / warm |
| --- | --- | --- | --- | --- |
| Reddit | Summit | 934 / 954 | 1604 / 1964 | 19606 / 19950 |
| Reddit | Firefox | 878 / 1103 | 1315 / 1296 | 4265 / 4142 |
| YouTube | Summit | 1537 / 1226 | 3253 / 2099 | 3700 / 1524 |
| YouTube | Firefox | 927 / 1050 | 2588 / 2888 | 2312 / 2490 |
| Wikipedia (Haiku) | Summit | 2071 / 972 | 2180 / 1146 | 4670 / 1302 |
| Wikipedia (Haiku) | Firefox | 1356 / 1142 | 1584 / 1270 | 2039 / 1346 |
| BBC News | Summit | – / 403 | – / 1200 | – / 3931 |
| BBC News | Firefox | 303 / 359 | 1046 / 564 | 1361 / 3027 |
| GitHub (WebKit) | Summit | 1116 / 1236 | 1116 / 1236 | 1895 / 2028 |
| GitHub (WebKit) | Firefox | 1246 / 1250 | 2134 / 1732 | 1827 / 1489 |

The Guardian produced one Summit reading of three (the console did not
answer while the page was busy) and Firefox's load events of 6-96 s, so it
is left out; BBC's cold Summit run failed the same way.

What it shows. With a warm cache Summit paints first as soon as Firefox or
sooner on every page but YouTube's cold load, and its largest paint is
earlier on YouTube and GitHub. Three gaps remain:

- **Reddit's load event, about 20 s against 4 s.** The page itself is on
  screen as early as in Firefox; the load waits for two Google iframes
  (reCAPTCHA, the sign-in button) whose documents arrived after 0.2 s but
  were not taken until 13 s later, because the web process's main thread
  was blocked. A watchdog in the page recorded one block of 16.9 s from
  6.2 s into the load, and a snapshot of the thread in it
  (`Debugger --save-report --thread <team>`) showed forced layout: a script
  in a microtask calls `getBoundingClientRect()`, and each call lays out
  six nested flex containers and three grids again
  (`FlexFormattingContext` sizing items for grid track sizing, 158 frames
  deep). It depends on the viewport: in a 1121x538 window the load event
  came at 5.7 s. This is upstream WebKit layout cost and the next thing
  to work on for complex pages.
- **Time to first byte on GitHub, about 500 ms more** (540-740 against
  157-203). `curl` on the same machine has the page's first byte after
  about 0.2 s, and `SUMMIT_NET_TRACE` shows each request's own DNS, connect
  and TLS times normal, so the extra time is before the request reaches
  libcurl; not yet traced.
- **Cold loads** of YouTube and Wikipedia paint 0.6-0.7 s later than
  Firefox's.

Improvements made with this work: canvas shadows through a blur mask
filter instead of an offscreen layer per shape (50 blurred shapes 19.4 ->
0.7 ms a frame on an accelerated canvas; xbitlabs.com's FPS test 11 ->
51 frames/s, "PERFECT"), WebGL on the GPU ([webgl.md](webgl.md); Firefox
has none here), and JavaScriptCore timers that now fire in workers
(asynchronous WebAssembly compilation there, and the collector's timers).

## 30 September 2026 (night): what stops a wheel scroll while a page loads

Continues the evening below, on the same machine and window. Installed at
the end, at 04:32: `bundle-z4_2wfc6` with the private Mesa in
`/boot/home/summit-mesa/prefix-20260930`. Before it, each with what the
sections below had reached by then: `bundle-gqejpqgz` at 01:20;
`bundle-t4ybypkf` at 02:57, which showed masked layers where their masks
should have hidden them; `bundle-mbzrp7ac` at 03:10, without that fault;
`bundle-iukouw8h` at 03:54; and since the evening `bundle-etksauzr` with
`prefix-20260929`.

| switch | default | what it does |
| --- | --- | --- |
| `SUMMIT_WHEEL_LATCH` | 1 | a burst of notches scrolls what its first notch scrolled |
| `SUMMIT_SCROLL_WITHOUT_MAIN` | 1 | a scroll is shown though the main thread has scheduled nothing |
| `SUMMIT_TILE_BUFFERS_LATE` | 1 | a tile's pixels are allocated by the thread that paints it |
| `SUMMIT_SKIP_EMPTY_TILES` | 1 | no texture and no draw for a tile without a pixel |
| `SUMMIT_TILE_SCALING` | 1 | tiles of 512 pixels at a scale of 1.5 and more |
| `SUMMIT_REPAINT_ON_RESIZE` | 0 | a layer that changes its size is painted again whole |

### The last build against the evening's

Measured from 04:33 to 04:44, alternated, two runs each. Frames the window
received during a scroll (notches at the middle of the page):

| page, scroll | evening: frames/s, gaps over 33 ms, longest | last build |
| --- | --- | --- |
| The Guardian, wheel while loading | 41.5, 29.5 / 7, 8 / 268, 525 ms | 54.4, 56.0 / 5, 3 / 83, 59 ms |
| The Guardian, fling | 41.6, 40.0 / 8, 8 / 143, 107 ms | 48.3, 50.3 / 4, 4 / 61, 74 ms |
| CNN, wheel | 45.0, 49.9 / 11, 6 / 47, 56 ms | 48.4, 47.0 / 8, 9 / 61, 63 ms |
| CNN, fling | 46.8, 50.4 / 8, 5 / 54, 55 ms | 52.0, 49.9 / 1, 4 / 35, 53 ms |
| Wikipedia (Australia), wheel | 47.0, 57.4 / 2, 3 / 384, 51 ms | 56.4, 57.4 / 3, 2 / 46, 35 ms |
| Wikipedia (Australia), fling | 55.9, 51.9 / 2, 4 / 36, 86 ms | 54.5, 56.6 / 2, 1 / 55, 41 ms |

YouTube, Big Buck Bunny at 1080p60, compositions of the page by the
compositor's clock:

| seconds of playback | | compositions/s | ms each | draw calls | over 16.7 ms |
| --- | --- | --- | --- | --- | --- |
| 0 to 5 | evening | 48.9, 52.3 | 10.5, 12.6 | 147, 173 | 18 of 244, 23 of 261 |
| | last build | 52.1, 51.2 | 9.6, 10.0 | 51, 46 | 13 of 259, 18 of 255 |
| 5 to 12 | evening | 54.7, 55.5 | 16.0, 15.6 | 201, 202 | 102 of 382, 89 of 387 |
| | last build | 59.5, 59.3 | 11.4, 12.3 | 78, 80 | 5 of 416, 12 of 414 |
| from 12 | evening | 59.1, 59.3 | 14.2, 14.2 | 194, 194 | 44 of 664, 31 of 658 |
| | last build | 59.4, 59.6 | 11.5, 11.5 | 77, 77 | 4 of 656, 3 of 656 |

Tiles uploaded in the 26 s: 7425 and 7520 in the evening's build, 594 and
741 in the last. The freeze of a third of a second in the first seconds is
in both (394 and 159 ms; 363 and 339 ms).

### The scrolling thread waited for the main thread, twice

After the evening's work a wheel scroll of The Guardian while it loads still
had gaps of 60 to 285 ms in which the compositor was asked for nothing. Two
traces name what holds the scrolling thread (`SUMMIT_SCROLL_LOCK_TRACE=1`,
anything over 20 ms) and what each refresh finds
(`SUMMIT_SCROLL_SYNC_TRACE=1`).

**Every notch hit-tested the layers, twice.** A wheel on Haiku has no
gesture phases, so WebKit's latching never applied: each notch asked what
listens under the pointer (on the event dispatcher's thread) and what
scrolls there (on the scrolling thread, with the tree's lock held). Both
walk the layers under `m_layerHitTestMutex`, and the main thread holds that
mutex for the whole of `RenderLayerCompositor::flushPendingLayerChanges()`:
30 to 200 ms at a time while The Guardian loads. Notches took 35 to 176 ms
to handle, the ones behind them arrived late, and the scroll animation
stopped for as long.

A burst of notches (each within 300 ms of the last, within 24 pixels of the
first, in the same direction) now scrolls what its first notch scrolled. It
asks again what listens under the pointer only when the main thread does not
hold the layers, and takes the answer of the notch before it otherwise. A
burst that began scrolling freely goes on doing so: a box with its own
scrolling, or with a wheel listener that cancels, that comes to pass under
the pointer hears of the notches and does not take the scroll over. Firefox
and Chrome scroll the same way. `SUMMIT_WHEEL_LATCH=0` is the old behaviour.

**A scroll was not shown until the main thread had scheduled an update.**
The scrolling thread applies layer positions itself only when a rendering
update is scheduled and late, and it is the main thread that schedules it
when it hears of the scroll. A main thread that is busy for a tenth of a
second schedules nothing; the tree stays `Idle` and each refresh does
nothing. A scroll that has not been applied now counts as a scheduled
update: one millisecond for the main thread, then the scrolling thread
shows it. `SUMMIT_SCROLL_WITHOUT_MAIN=0` waits as before.

The Guardian, 40 notches in 2 s from 1.5 s after navigation, consent
dismissed, compositions in the web process from the first notch that
scrolled, by the compositor's own clock, four runs alternated:

| | compositions a second | gaps over 40 ms | notches that took over 20 ms |
| --- | --- | --- | --- |
| both switches off | 50.6, 57.5, 54.0, 57.6 | 62 42 41 52 43 53 58 54; 49; 160 43; none | 51 50 58 52; 42 25 31; 158 25; 38 124 |
| new | 59.5, 59.5, 58.6, 58.7 | none; none; none; 44 | none |

Frames the window received, against the build installed in the evening,
two runs each, notches at the middle of the page in both:

| page, scroll | evening: frames/s / gaps over 33 ms / longest | new |
| --- | --- | --- |
| The Guardian, wheel while loading | 45.9, 52.0 / 6, 2 / 216, 205 ms | 54.9, 54.9 / 3, 2 / 87, 87 ms |
| The Guardian, fling | 46.2, 46.2 / 4, 5 / 109, 96 ms | 52.4, 50.4 / 2, 2 / 63, 54 ms |
| CNN, wheel | 58.4, 54.9 / 2, 5 / 36, 61 ms | 58.0, 55.9 / 1, 4 / 46, 42 ms |
| CNN, fling | 53.5, 54.0 / 4, 6 / 43, 39 ms | 56.1, 54.5 / 3, 2 / 47, 35 ms |
| Wikipedia (Australia), wheel | 57.4, 58.4 / 2, 1 / 57, 36 ms | 59.3, 57.9 / 0, 2 / 30, 37 ms |
| Wikipedia (Australia), fling | 57.1, 57.1 / 2, 3 / 47, 39 ms | 58.2, 58.1 / 1, 2 / 36, 38 ms |

That table is the build of 01:20. The last build was measured the same
way at 02:45, when the pages were heavier for both (The Guardian's fling is
43 frames/s in the evening's build, 46 an hour before): The Guardian's
wheel 53.0 and 56.4 frames/s against 57.9 and 49.9, its fling 45.2 and
44.2 against 43.1 and 43.1, CNN's wheel 51.4 and 45.9 against 46.5 and
49.9, its fling 49.9 and 51.9 against 52.5 and 52.4, Wikipedia's wheel 56.9
and 57.8 against 56.0 and 57.9, its fling 55.0 and 56.0 against 54.0 and
54.6. Two runs of a live page do not tell builds apart that are this
close; the tables of alternated runs in this section do. Three iterations
of Speedometer 3.1, as a check that nothing broke: 8.0 ± 2.8 against 7.5
± 2.2.

The 87 ms that remain on The Guardian are before the first frame. The
middle of that page has a wheel listener that may cancel, so the first
notch of a burst goes to the main thread, and the scroll starts when the
second notch arrives 50 ms later (below).

After the same scroll the evening's build and the new one show the same
picture on Wikipedia and GitHub (0 of 20,736 cells differ) and on The
Guardian (127, an advertisement). Test pages:
`tools/bench/pages/wheel-nested-scroller.html` (a box that scrolls, under
the pointer from the start: it scrolls to its end, then the page does, with
the latch and without), `wheel-scroller-passes.html` (the box passes under
the pointer: the page reaches 12,000 px with the latch; without, the box
takes two notches and the page reaches 11,400), `wheel-listener-passes.html`
(a box whose listener cancels every notch passes: it hears 11 notches and
the page reaches 12,000).

### The main thread zeroed every tile before it was painted

`profile -a -k` with callers, four seconds of The Guardian loading and
being scrolled: the web process's main thread is busy 3535 of 4000 ms.
Scripts are 31% of that (12% parsing them), style 15%, layout 15%, and the
layer flush, which is what keeps the layers from hit tests, 9%: 329 ms, of
which 233 in `SkiaPaintingEngine::createBuffer()`. It allocated each tile's
pixels zeroed, 200 tiles of about a megabyte for one repaint at 200%,
before handing the tile to a painting thread, whose first act is to clear
the canvas. The pixels are now allocated by the painting thread when it
asks for the canvas, and not zeroed (`SUMMIT_TILE_BUFFERS_LATE=0`). Over
four alternated runs the main thread kept the layers for more than 20 ms
3 to 5 times a run and 134 to 243 ms in all, against 7 to 9 times and 305
to 584 ms. The scroll itself does not change (58.3 to 59.6 compositions a
second either way): it no longer waits for the main thread.

The same profile has the network process's cache thread at 1533 to 1707 ms
of the 4000, three quarters of it in bfs `BlockAllocator::AllocateBlocks()`.
A new file costs 6.2 ms on the workstation's volume whatever its size
(docs/kunanyios-platform-issues.md), and the cache writes one for every
resource. That is a processor's time, not the page's; it is with the OS
session.

### Tiles with nothing in them

YouTube's page has layers that are mostly nothing: the container of its
header (1904x1056, a bar at the top), the overlay of the player (1308x718).
Every tile of such a layer was uploaded, kept as a texture and blended into
every frame. The painting thread now looks whether a tile has any pixel
that is not transparent, and the compositor keeps no texture for a tile
without and draws nothing for it (`SUMMIT_SKIP_EMPTY_TILES=0`).

Big Buck Bunny at 1080p60, 26 s, two runs each way, by the compositor's
clock:

| seconds of playback | | compositions/s | ms each | draw calls | Mpx blended | over 16.7 ms |
| --- | --- | --- | --- | --- | --- | --- |
| 0 to 5 | before | 47.8, 48.1 | 11.7, 11.0 | 158, 153 | 4.5, 4.6 | 35 of 238, 24 of 240 |
| | new | 50.6, 50.7 | 12.3, 10.8 | 115, 119 | 5.1, 2.8 | 19 of 252, 18 of 252 |
| 5 to 12 | before | 55.8, 54.7 | 15.4, 16.2 | 205, 204 | 8.1, 8.1 | 76 of 390, 101 of 382 |
| | new | 57.1, 56.8 | 13.9, 13.8 | 136, 144 | 4.4, 4.5 | 65 of 399, 69 of 397 |
| from 12 | before | 59.5, 59.2 | 13.7, 14.3 | 194, 194 | 7.8, 7.8 | 23 of 669, 31 of 669 |
| | new | 59.5, 59.5 | 11.9, 12.0 | 130, 130 | 4.1, 4.1 | 4 of 642, 3 of 667 |

`tools/bench/pages/transparent-tiles.html` paints a small box into an empty
tile, moves it to another, removes it, and paints a translucent area over
several; the four pictures are the same pixel for pixel with the switch on
and off. So are Wikipedia's and GitHub's after a scroll.

**A mask is the exception, and the first build did not make it.** A mask
hides what lies under its transparent pixels, so its empty tiles have to be
drawn. Without them a layer with `mask-image` or `clip-path` showed from
the first tile boundary on where it should have been hidden
(`tools/bench/pages/masked-layers.html`). The pages compared before the
change went in have no such layer in view; the fault was found 13 minutes
after the build was installed, by going through what else draws with a
blend function in which a transparent source changes the picture (only the
mask mode does), and the build that replaced it was installed at 03:10.
Tiles of mask layers are kept as before. `layers-with-effects.html`
(backdrop filter, `mix-blend-mode`, drop shadow, blur, opacity with a
rotation) is the same pixel for pixel with the switch on and off.

### Tiles of 512 pixels

What the graphics card takes to draw a frame was measured apart from the
browser (`tools/mesa-vm/tiles-probe.cpp`, the time `glFinish()` waits after
the draw calls, 3840x1756):

| one layer over the window as | draw calls | ms |
| --- | --- | --- |
| tiles of 256 | 105 | 2.40 |
| tiles of 512 | 32 | 1.86 |
| one quad | 1 | 1.41 |
| two layers, tiles of 256 | 210 | 4.90 |
| two layers, tiles of 512 | 64 | 3.28 |
| two layers, two quads | 2 | 2.36 |
| tiles of 256, 30 of them uploaded before | 105 | 3.18 |
| tiles of 256, 120 of them uploaded before | 105 | 5.27 |

A layer over the window costs 0.95 ms to fill, each tile drawn 9 us more
whatever its size, each tile uploaded 26 us (and 16 of the processor's). A
fling of The Guardian draws 190 tiles and 16 Mpx a frame and uploads 24;
with `SUMMIT_READBACK_SPLIT=1` the card takes 7.1 ms for that and the read
of the frame 7.6.

A tile of 256 pixels is 128 of the page's at 200%. At a scale of 1.5 and
more a tile is now 512 pixels, the part of the page a tile covers at 100%
(`SUMMIT_TILE_SCALING=0` keeps 256). Frames the window received in a fling
of the settled Guardian, nine alternated pairs:

| tiles | frames/s | gaps over 33 ms |
| --- | --- | --- |
| 512 | 50.4 49.3 53.0 47.3 52.5 50.9 50.3 47.2 49.6 | 5 4 4 5 4 5 4 7 4 |
| 256 | 40.5 50.4 45.6 48.2 45.1 44.0 44.6 49.4 42.1 | 17 3 12 7 12 8 11 5 12 |

YouTube from the twelfth second: 75 to 77 draw calls and 11.4 to 11.5 ms a
composition against 130 and 11.8 to 12.2. CNN's fling (47.2 and 50.2
against 47.7 and 47.7), Wikipedia's (56.5, 56.1, 50.4 against 55.6, 55.6,
55.5) and The Guardian's wheel while it loads (55.9, 57.4, 54.5 against
52.9, 57.4, 53.5) do not tell the two apart. The evening found no
difference with tiles of 512 on The Guardian; it measured the wheel while
the page loads, which was held up by what the first section of this entry
removed.

### A layer that grew was painted again whole

From the fifth to the twelfth second of YouTube's playback the page's root
layer was painted again 3.4 times a second, 124 tiles and 7.85 Mpx each
time, and each of those compositions took 25 ms and cost the video a frame.
The layer grew by 192 px each time: an entry more in the list beside the
player. `SUMMIT_FULL_REPAINT_TRACE=2` prints who asks for a layer of two
megapixels to be painted whole: `GraphicsLayer::setSize()`, which does so
for every layer that draws content unless `shouldRepaintOnSizeChange()`
says no, and only the tiled layers of the Core Animation port said no. The
Guardian's root layer is painted whole about ten times while it loads.

A coordinated layer keeps its content in tiles that stay where they are
when the layer grows. `CoordinatedBackingStoreProxy` paints the tiles at
the edge whose extent changes and the new ones, and renderers ask for what
they changed, so the layer no longer asks for everything
(`SUMMIT_REPAINT_ON_RESIZE=1` does). YouTube, seconds 5 to 12, seven
alternated pairs:

| | compositions a second | over 16.7 ms, of about 410 | tiles uploaded in 26 s |
| --- | --- | --- | --- |
| whole layer | 57.7 57.4 57.6 57.7 54.2 54.8 57.8 | 51 61 56 65 103 100 57 | 2230 to 2300 |
| new | 57.9 58.5 59.4 59.0 59.2 59.4 59.3 | 46 30 9 16 7 12 11 | 730 to 770 |

In two of the seven runs of either kind the compositions after the twelfth
second took 13 to 15 ms instead of 11 to 12, with the same draw calls and
the same 6.22 Mpx read; what differs between such runs was not found.
`tools/bench/pages/layers-that-resize.html` is the same pixel for pixel
both ways, as are The Guardian, Wikipedia and GitHub after a scroll.

### What is left of YouTube's start

- **A freeze of 330 to 400 ms about 1.6 s after the first frame**, in every
  run. The page takes the video's layer away and gives it a new one
  ("video goes to the compositor" three times in each load); between the
  rendering update that removes the old one and the one that brings the
  new one the main thread is busy, and nothing is composited. Not
  addressed.
- **The decoder is flushed and set up two to four times** in the first half
  second, each time from the first sample.
- **The Guardian changes the style of `html` and of `body` 11 times each
  while it loads**, and 28 layouts ask for a full repaint; each paints the
  root layer again. Those are WebCore's rules for the background of the
  page, and were left alone.
- **One `vkCreateImage` fails in every load** (`nvRes: 0x1f`, then "ZINK:
  vkCreateImage failed"). It comes from Skia's GL context
  (`SUMMIT_SKIA_GL_CONTEXT=0` has none), not from the compositor's
  textures, and nothing is missing on screen. Not explained.

### Other changes of the night

- **A burst waits 50 ms for the page, once.** A page with a wheel listener
  that may cancel gets the first notch of a burst on its main thread. When
  the answer takes over 50 ms, the following notches scroll without it and
  the browser is answered then, so that it sends the next notch. On a test
  page whose main thread is busy 400 of every 450 ms
  (`wheel-busy-main-thread.html`) the first frame comes within 80 ms and
  the scroll runs at 56 frames/s, against 29 to 31. A page that has
  cancelled a notch is waited for as before: without that rule a page that
  cancels every notch scrolled. On The Guardian five alternated pairs gave
  54.0 against 54.7 frames/s: the wait bounds the start of a scroll and
  changes nothing after it.
- **18 shader programs are compiled when the compositor is created**, not
  8: the antialiased and filter variants were compiled in the frame that
  first used them. Compiling them in stages after start-up collided with
  the first scroll and was taken out.
- **The private Mesa is built with its shader cache**
  (`-Dshader-cache=enabled`, cache in `/boot/home/.cache/mesa_shader_cache`).
  A program that has been compiled once on the machine takes 0.3 to 3 ms
  in a new process, against 8 to 24 ms.

### What did not pay

- **Uploading no more than a few visible tiles per composition**: no
  difference in five pairs, reverted. The experiment counted only
  compositions for scrolling; slow frames are as often rendering updates,
  so it says little.
- **Keeping zink's staging buffers between frames**: no difference,
  reverted.

### Traps in the measurements

- **`summitctl scroll` without a point put the notches at the page's top
  left corner**, not at its middle as the code meant:
  `BMessage::FindPoint()` clears the point it does not find. Every scroll
  measured before 30 September was taken there. On The Guardian the corner
  has no wheel listener and the middle has one. Fixed; `run-scroll.py
  --wheel-point X,Y` names the point for builds that still have the fault.
- **A VNC screenshot stops the window's frames for 4 s.** During YouTube
  playback each of three screenshots left a gap of 4.0 to 4.4 s in the
  frames the window drew, and 300 frames the second after. Without
  screenshots the longest gap in 30 s was 259 ms, in the first seconds.
  No screenshot belongs in a timed run, and none should be taken while the
  owner uses the machine: it stops their picture too. This session took
  them during the evening without knowing.
- **Reddit and BBC News are 700 to 830 px longer than the view two seconds
  after navigation.** 40 notches reach that end after three, and 22 to 29
  of the 40 go unhandled until the feed arrives
  (`SUMMIT_SCROLL_POSITION_TRACE=1`). Their "gaps" of 300 to 800 ms are
  that, in the old build and the new.
- Page loads are bimodal in both builds: CNN asks hosts that do not answer,
  and a load that meets one waits 30 s for the connection.
- **The log's arrival times are not the compositor's.** The test launcher
  stamps each line when it reads it from the pipe, and lines bunch: four
  compositions "at the same millisecond", then none for 40. Intervals
  between compositions are taken from the `at=` of the timing lines.
  The first table above was first made from arrival times (48.4 to 58.2
  against 57.3 to 59.6 compositions a second); the conclusion is the same.
- **An empty name in a copy command copied every bundle on the
  workstation** into the test directory, 40 GB, until the volume was 97%
  full (02:05 to 02:10 on 30 September). The command took the name of the
  bundle from the output of a build that had failed. The copy was stopped
  and the directory removed; nothing else was touched, and the test
  scripts now check the name.

### YouTube, before the two sections above

Big Buck Bunny at 1080p60, 30 s without screenshots, in the build of
01:20: the first video frame 1.2 s after the page creates its player; 52
to 56 frames/s on screen for the first eight seconds, with gaps of 84 to
350 ms in the first three; 59 frames/s and no gap over 33 ms from the
fourteenth second on. A composition takes 14.5 ms (95th percentile 19.0),
11.8 of them the read and the hand over to the window. The evening's build
and that one do not differ.

## 29 September 2026 (evening): video that stood still, web processes that died, scrolling at 200%

Goal: the stutter the owner saw on The Guardian, YouTube that is choppy and
slow to start, scrolling that is choppy while pages load. Measured on the
X399 (window 1920x1020 at 200%, a 3840x1756 frame), against the build that
was installed in the morning (`bundle-kqj5c78l`). Installed at the end:
`bundle-etksauzr` with the private Mesa in
`/boot/home/summit-mesa/prefix-20260929`.

### Two faults behind "choppy"

**The video did not move.** Two screenshots 2.5 s apart during YouTube
playback were identical in the installed build, while the player counted 60
presented frames a second. A video frame reaches the compositor on its own
(`CompositionReason::VideoFrame`), and `CoordinatedPlatformLayer` hands a
layer's damage to the compositor only in rendering updates and scrolls.
Until 28 September TextureMapper damaged every layer with contents on every
composition, which hid that; commit 115b935 took the special case out, and
from then on the compositor drew each video frame and read none of it
back. The picture changed when something else damaged its place: the
pointer over the player, a scroll. A new contents buffer now damages its
layer when it is committed. Checked the same way: the video's rectangle
differs between the screenshots.

**Web processes exited.** Two of the first four test loads (YouTube, The
Guardian) ended with `input in flex scanner failed` and a crash report: the
libnetwork resolver race that killed the network process on 28 September
(docs/kunanyios-platform-issues.md), this time in the web process, whose
media loaders fetch with libcurl. The page reloads when that happens. The
network process made its first lookup on its main thread; web processes and
the browser now do too, and the browser keeps `nsswitch.conf` newer than the
directories above it. Over the evening 3 of 57 runs of the installed build
ended this way and none of 161 runs with the change.

### Where a scrolled frame's time went

Haiku's `profile -a -k` samples every thread, kernel included
(`run-scroll.py --system-profile 4`). During a Guardian scroll the
compositor thread was busy, not waiting: 38% of it one `memcpy` a frame,
the 27 MB that `glReadPixels` copies out of the driver's staging buffer. The
window's thread in app_server spent 8 ms a frame in `memcpy` as well.

| what | before | after |
| --- | --- | --- |
| copy of a read-back frame (27 MB) | 4.1 ms, one `memcpy` | 1.3 ms, four threads, non-temporal stores |
| `glReadPixels` of 3840x1756, standalone | 8.0 ms | 3.8 ms |
| of that, the RGBA target converted to BGRA on the GPU | 1.2 ms | none, the target is BGRA |
| read of a video's 2664x1498 inside the frame | 5.9 ms | 3.2 ms, read as a band 3840 wide |
| upload of a 256x256 tile | 42 us | 31 us |
| draw calls for tiles outside the clip | 25 us each | none |
| tiles uploaded in the composition that receives them | all, 3-90 ms | those it shows, and 3 ms of the others |
| shader programs | compiled in the frame that first uses one, 8-24 ms | compiled when the compositor is created |

The copy is in the private Mesa
(`tools/mesa-vm/mesa-25.3.6-summit-05-frame-copies.patch`,
`summit_copy_rows()`, `SUMMIT_COPY_THREADS`); the rest is in the engine, each
with a switch: `SUMMIT_BGRA_TARGET=0`, `SUMMIT_READBACK_BANDS=0`,
`SUMMIT_TILE_UPLOAD_BUDGET_MS=0`, `SUMMIT_WARM_SHADERS=0`.

Frames the view received during the burst, two runs each, consent banner
dismissed (below). "Wheel" is 40 notches in 2 s starting 2 s after
navigation, "fling" 120 notches in 1.9 s on the settled page:

| page, scroll | installed: frames/s, gaps over 33 ms, longest | new |
| --- | --- | --- |
| The Guardian, wheel | 42.4, 52.0 / 5, 6 / 419, 105 ms | 59.0, 57.5 / 0, 2 / 31, 43 ms |
| The Guardian, fling | 41.6, 45.7 / 8, 6 / 144, 83 ms | 53.5, 51.9 / 3, 3 / 37, 56 ms |
| CNN, wheel | 53.0, 49.9 / 9, 8 / 72, 84 ms | 56.4, 57.9 / 4, 3 / 52, 44 ms |
| CNN, fling | 52.5, 51.9 / 7, 8 / 68, 68 ms | 56.6, 56.1 / 2, 1 / 37, 33 ms |
| Wikipedia (Australia), wheel | 54.0, 54.4 / 6, 6 / 64, 53 ms | 57.9, 58.9 / 2, 0 / 43, 30 ms |
| Wikipedia (Australia), fling | 45.2, 45.2 / 17, 17 / 77, 104 ms | 57.6, 56.1 / 1, 2 / 45, 96 ms |

With a copy of the owner's profile (signed in, extensions running) The
Guardian's wheel scroll is 58.4 frames/s with 0 and 1 gaps, against 51.0 and
51.5 with 5 and 7. After the same scroll the two builds show the same
picture on The Guardian, Wikipedia and GitHub; on CNN only the
advertisements differ.

YouTube (Big Buck Bunny, 1080p60) plays at 56-58 frames/s on screen, with
about one gap over 33 ms every three seconds; a composition takes 13-15 ms,
7 to 8 of them the GPU drawing the frame and 4 the read. From navigation to the
first picture is 2.9 s in a new profile, 1.9 s of it until the page creates
its player.

### What did not pay, and what was wrong

- **The GPU is not held at its boot clocks**, as this file said on 28
  September. NVIDIA's resource manager reports P-state P0 in every sample:
  idle, under a browser scroll, under a GL probe (170 samples), and a
  `PERF_BOOST` to the maximum is accepted and changes no timing
  (`tools/mesa-vm/rmperf.c`). No clock frequency was read, so this shows
  that the explanation had no measurement behind it, not what the clocks
  are.
- **A fill rate of 2.7 Gpx/s**, which this session first gave the OS
  session as the GPU's, was a division of a browser's frame time. A
  standalone probe draws eight more layers of 4 Mpx in 1.1 ms
  (`video-probe.cpp`). What a page costs follows its draw calls as well as
  its pixels: the same 27 Mpx in 420 draws of 256x256 take 7.7 ms, in 4
  draws 3.8 ms (`tiles-probe.cpp`). Tiles of 512 made no difference to The
  Guardian's scroll.
- **Wheel events that wait for the main thread** were taken for the cause
  of the long gaps on BBC News. The cause was the test: 40 notches reach
  the end of that page, and a page that has stopped scrolling presents
  nothing. Notches in a burst now scroll without the main thread once the
  page has let the first one through (`SUMMIT_WHEEL_BURSTS`), which is 54
  frames/s against 29 on a test page whose main thread is busy 400 of every
  450 ms, and no different on BBC, ABC, Ars Technica and GitHub: the
  animation of a notch outlasts their stalls.
- **Layers with a filter** are damaged in every composition. Damaging them
  only when something changed takes 40 surface switches and 10 blur passes
  out of each YouTube frame and leaves the frame rate where it was, so it
  is off (`SUMMIT_FILTER_LAYER_DAMAGE=changed`).

### Traps in the measurements

- A new profile shows The Guardian's consent banner, which dims the whole
  page: 5 ms of the GPU's time in every frame that the owner, who has
  dismissed it, does not pay. `run-scroll.py --profile-template` starts
  from a prepared profile (`/boot/home/summit/claude-guardian-cold`).
- 120 notches at 16 ms scroll 21,000 pixels in two seconds. It is a fling,
  and on most pages it ends at the bottom. Trace the wheel
  (`SUMMIT_WHEEL_ROUTE_TRACE=1`) and discard runs with `handled=0`.
- The player's count of presented frames says what reached the compositor,
  not the screen.

### Left to the OS

app_server copies each frame twice (into its back buffer, then to the
screen), with libroot's `memcpy`, which is `rep movsb` on a processor
without ERMS: the 8 ms a frame above. A partial-width
`vkCmdCopyImageToBuffer` costs 1.5 ms a megapixel against 0.56 for rows
that span the image. Both are with the OS session, as is its fix for the
resolver.

### The pointer

The launcher has recorded the owner's own sessions since the morning. In
408 seconds with pointer motion, events reached the browser's window 1.8 ms
after input_server stamped them on average; 24 of those seconds had one
over 30 ms, three had one of 1.4 to 1.7 s. Opening, closing and switching
tabs and windows through `summitctl` stalled the window's thread for at
most 72 ms, so those three are not explained; the launcher now records
every dispatch over 50 ms (`Summit stall:` lines).

189 of the 211 seconds with 30 or more moves had a gap of 40-400 ms in the
mouse's reports. The morning's entry below read those as the link stalling.
They may as well be the hand: a pointer that slows, stops and starts again
leaves the same gap. The trace now counts a gap as "in motion" only when
the two reports before it and the one after it moved the pointer by 4, 4
and 12 pixels or more, and counts reports that arrive within 3 ms of each
other after a gap; nothing has been recorded with it yet.

## 29 September 2026: pointer stutter while loading The Guardian

The owner saw the mouse pointer stutter while The Guardian loaded and
asked whether the browser or the Bluetooth LE mouse was to blame. The
X399 has no USB mouse: the pointer is a bonded BLE device (peer
`CB:8C:04:89:64:E7`, interval 7.5-11 ms) on the MediaTek MT7922 combo
radio, read by the OS's new `bluetooth_le_mouse` input_server add-on
(2026-09-28 in the OS log, "not yet seen with the mouse in hand").

What could make the browser responsible: app_server moves the hardware
cursor from its event thread, and `AccelerantHWInterface::MoveCursorTo`
takes the frame buffer's *exclusive* lock to do it, so a cursor move waits
for every drawing operation in flight, including Summit's frame draws.
Measured instead of assumed: `SUMMIT_INPUT_LAG_TRACE=1` (new) prints once a
second how late `B_MOUSE_MOVED` reaches the browser window after
input_server stamped it, which is the time spent in input_server and in
app_server's event thread (cursor move included), plus the gaps between
consecutive moves as stamped, which is the device's own cadence. With the
pointer driven over VNC at 125 Hz in a circle over the page while The
Guardian loaded (frames of 1.8-2.4 Mpx drawn at up to 40 a second):

| per second, 60 s | moves | mean lag | max lag | over 30 ms |
| --- | --- | --- | --- | --- |
| idle page | 125-126 | 0.6-0.9 ms | 3-4 ms | 0 |
| during the load (10-25 s) | 125-126 | 0.7-1.1 ms | 5.5-10.4 ms | 0 |

So input_server, app_server's event thread and the window keep up during
the load, and Summit's own input handling never waited over 20 ms
(`SUMMIT_INPUT_DELAY_TRACE`). What the VNC path does not exercise is the
radio and the add-on: the stutter must come from before input_server,
where this session cannot move the mouse. Two leads for the OS session:
the MT7922 shares its radio between Wi-Fi and Bluetooth, and the Wi-Fi
interface is up on the same LAN as the Ethernet (it received 2,000
packets during the load, the browser's traffic goes over Ethernet); and
the add-on reads the mouse battery over GATT every ten minutes, which
once dropped the link while the mouse moved. The installed launcher now
runs with `SUMMIT_INPUT_LAG_TRACE=1` and keeps stderr in
`/boot/home/summit/summit-stderr.log`, so the owner's own sessions record
the source gaps (40-400 ms between reports in the middle of a motion) for
whoever looks next.

## 28 September 2026 (evening): extension start-up, the 1Password popup, background tabs, the network process

Goal: multiple tabs, load times, scrolling under load, and extension
loading, with the 1Password toolbar button opening its popup near
instantly. Milestones on both sides are printed by `SUMMIT_EXTENSION_TIMING=1`
("Summit browser timing" from Summit, "Summit extension timing" from the
engine, both on `system_time()`), which is how the numbers below were read.
Test setup on the X399: a private copy of the installed bundle and of the
owner's profile (1Password signed in), launched by `tlaunch.py` which
stamps every stderr line with seconds since launch.

### Unpacking 1Password on every launch: 6 s, now 0.13 s

Every launch prepared each installed extension by extracting its archive
into `ExtensionStaging/<pid>/` and then copying the tree again into a
snapshot directory, one `fsync` per file. 1Password's package is 1,001
files (47 MB) and BFS takes about 3 ms to create a file, so the staging
directory appeared 0.4 s after launch, the last snapshot file 6.4 s after
launch, and the background page's first console line at 7.4 s. Hashing the
same tree in place costs 0.18 s.

Archives are now unpacked once into
`<profile>/WebKit/ExtensionPackageCache/<sha256 of the archive>/package`,
beside a `record` with the archive's path, size and modification time, the
resource fingerprint and the verified CRX signer. The unpacked tree is
hashed once (no second copy) to get the fingerprint. A later launch hashes
the archive to find the entry, hashes the cached tree and compares it with
the record; on a match the extension loads from the cache, and
`WebExtension` no longer deletes the directory when unloaded. Entries whose
archive was replaced or removed, and unpacking directories older than an
hour, are pruned after each preparation.

| Launch of the owner's profile | before | first launch (cache miss) | later launches |
| --- | --- | --- | --- |
| package prepared | 6.4 s | 3.6 s (unpack 3.0, hash 0.08) | 0.60 s (verify 0.13) |
| extension loaded | 6.8 s | 3.6 s | 0.63 s |
| background page loaded | | 4.5 s | 1.85 s |
| "Finished initializing 1Password" | 8.4 s | 6.0 s | 2.9 s |

What remains is the background page itself: 1.2 s from creation to
`load` (a 2.9 MB `background.js`) and about 1 s of the extension's own
initialization (WebAssembly core, IndexedDB, feature flags).

### The popup: created on the click, now pre-loaded on hover

A toolbar click created the popup page, loaded `popup/index.html`, waited
for its `load` event, then showed the window. From the click:

| | page created | content 600x450 | document loaded, shown | first frame drawn |
| --- | --- | --- | --- | --- |
| first open | 7 ms | 214 ms | 352 ms | 428 ms |
| second open | 1 ms | 293 ms | ~300 ms | 321 ms |

Firefox starts loading a browser action's popup when the pointer reaches
its button, so the click only has to show it. Summit now does the same:
`ExtensionActionButton` posts a pre-load message when the pointer enters
it (and on mouse down), `BWebKitContext::PreloadExtensionAction` reaches
`ExtensionActionPopupHaiku::preload`, which creates the page and the
native window as before but does not show them when the document loads.
The click then calls `present()`: shown at once if the document is ready,
otherwise as soon as it is. A pre-loaded popup nobody clicks closes after
30 s; `action.openPopup()` closes one before opening its own; a click while
another extension's popup is open still supersedes it. No user gesture or
`activeTab` grant comes from the pre-load.

| click to first frame | |
| --- | --- |
| hovered 1.5 s before the click | 89-93 ms |
| hovered 0.15 s before the click | 42-79 ms |
| pointer arrives and clicks within 0.2 s (document still loading) | 126-141 ms |

The remaining 40-90 ms is `Show()` and the first draw through app_server.
One first pre-load after launch took 3.9 s between the pre-load and the
page creation in one run and 5 ms in the next; the finer timing lines in
`start()` are there for the next time it shows.

### Background tabs' processes yield the CPUs

WTF already maps quality of service onto Haiku priorities (scrolling,
compositing and event dispatch at `B_DISPLAY_PRIORITY`), but every web
process ran that way whether its page was on screen or not. As Linux does
with `HighPriorityThreads`, the web process now reacts to page visibility:
when none of its pages is visible, audible, capturing media or an
extension page (the process hosting an extension's background page also
serves its popup and its content scripts' messages), every thread of the
team below real time is set to `B_LOW_PRIORITY` and remembered; threads
created meanwhile are lowered as they start (`ThreadingPOSIX.cpp`), and
the recorded priorities come back when a page becomes visible
(`setProcessBackgroundedHaiku`, `WebProcess::pageActivityStateDidChange`,
re-evaluated on commit because an extension page's URL is only known
then, and when a page becomes audible: a hidden tab that starts playing
sound gets its priority back, verified with a clip that has an audio track
while a muted copy in another hidden tab stayed low). `SUMMIT_BACKGROUND_PROCESS_PRIORITY=0` turns it off. Summit's own
application thread, which runs WebKit's main loop for every tab, now runs
at `B_DISPLAY_PRIORITY` like window threads (`SUMMIT_UI_PRIORITY=normal`
reverts).

Three windows of twelve real sites (`run-multitab.py`), alternated with
the installed build:

| 3x12, X399 | installed (`bundle-ztn5nkzi`) | with priorities |
| --- | --- | --- |
| scroll under load (24 tabs reloading) | 33.3 / 30.5 fps, 11 / 14 long gaps | 35.0 fps, 15 long gaps |
| tab switch, median | 292 / 273 ms | 247 ms |
| load, median / p90 / all done | 19.3 / 49.6 / 82 s, 18.1 / 49.8 / 69 s | 24.3 / 52.6 / 75 s |

A modest gain in the scroll and the switch, within the run-to-run spread;
loading was not CPU-bound in the first place (see below), so the
priorities do not change it. The first run with the change lost 17 tabs at
once when the network process died; that turned out to be the next item.

### The network process dying in libnetwork's parser

The crash report (`NetworkProcess-137701`, the same shape as the two
older ones): a libcurl resolver thread printed `input in flex scanner
failed` and called `exit()` from `getaddrinfo` -> `nsdispatch` ->
`_nsconfigure` -> `_nsyyparse`, libnetwork's flex scanner for
`/etc/nsswitch.conf`; the main thread then segfaulted in
`BMessage::_SendMessage` after libbe's statics had been destroyed. The
parse runs once per process, on the first name lookup, in whichever thread
makes it; when 36 tabs start at once that is one of a dozen resolver
threads while sockets and cache files open and close all around it, and a
read of the file failed (a descriptor closed underneath it fits best).
`NetworkProcess::platformInitializeNetworkProcess` now resolves
`localhost` (answered from `/etc/hosts`) before any other thread exists,
so the parse happens in a quiet process. That was not enough: with four
curl workers resolving names at once the process still died in three of
four 36-tab starts, and the real mechanism is in
`docs/kunanyios-platform-issues.md`: `nsswitch_conf_file_path()` builds
its path in a static buffer on every call, a concurrent thread can `stat()`
the half-built path, find the settings *directory*, open it and die in the
lexer. No `nsswitch.conf` existed on the X399 at all. The workaround, now
made by `tools/install-on-workstation.sh`: a real
`/boot/system/settings/network/nsswitch.conf` (`hosts: files dns`, the
sources used without a file) newer than its directory, so the racy check
sees an older directory and returns without parsing. Two 36-tab starts
after it: 36/36 loaded, no resolver failure, load medians 8.1 and 7.0 s.
The fix belongs in libnetwork.

### Where 24 tabs' requests spend their time

`SUMMIT_NET_TRACE=1` now also prints the disk cache lookup time. Loading
24 sites at once in a fresh profile (3,172 requests in 45 s, 78 MB/s
available downstream):

| per request, ms | median | p90 | p99 |
| --- | --- | --- | --- |
| disk cache lookup | 0 | 0 | 6 |
| queue (main thread `add` to the curl thread's `setupTransfer`) | 103 | 498 | 990 |
| DNS | 0 | 219 | 979 |
| connect | 0 | 778 | 1,822 |
| TLS handshake (673 of them) | 337 | 807 | 1,228 |
| time to first byte | 489 | 2,011 | 6,178 |
| total | 2,011 | 4,847 | 11,514 |

The machine was not busy: 3-4 of 32 cores over the load, the network
process's main thread 0.13 cores. The one `curlThread`, which drives every
transfer of every tab through a single multi handle, used 0.59 cores, 78%
of it in the kernel: each iteration polls every socket and then
`curl_multi_perform` walks every handle, so the cost of one arriving chunk
grows with the number of open transfers, and a new request waits a median
103 ms (p90 half a second) just to be started. The cache's write queue
(`Cache.Storage.background`) used another 0.46 cores, 88% kernel, creating
files on BFS; lookups are not queued behind it.

The scheduler now runs several worker threads, each with its own multi
handle, and a request goes to the worker chosen by its host, so a host's
connections and HTTP/2 streams stay together and are reused (cookies, DNS
and TLS sessions are shared through the share handle as before).
`SUMMIT_CURL_THREADS` sets the count (default 4).

The same 24 sites with four workers: median request 2,011 -> 1,042 ms,
time to first byte 489 -> 218 ms, TLS handshake 337 -> 181 ms, but the
queue's tail grew (p99 990 -> 2,867 ms): a worker was still stalling for
seconds at a time. A per-iteration trace (`SUMMIT_NET_TRACE=1` prints
"slow iteration" lines) showed the poll and `curl_multi_perform` taking
0-7 ms and the queued *tasks* taking 10-15 ms each ("tasks 26 in 359
ms"). A task is a transfer's set-up or its clean-up, and the cost was
`CURLOPT_ALTSVC`: every easy handle is given the alt-svc cache *file*,
which libcurl reads when the option is set and writes back when the
handle is cleaned up -- two file operations on BFS per request, on the
transfer thread. The cache only matters for HTTP/3 discovery (HTTP/2 is
negotiated by ALPN) and this libcurl has no HTTP/3, so on Haiku it stays
off unless `SUMMIT_CURL_ALTSVC=1`.

Three windows of twelve sites, alternated with the installed build, with
the priorities, the worker threads and the resolver warm-up (before the
alt-svc change):

| 3x12, X399 | installed, run 1 / 2 | new, run 1 / 2 |
| --- | --- | --- |
| load, median | 22.0 / 25.8 s | 10.3 / 25.7 s |
| load, all done | 85 / 88 s | 56 / 76 s |
| tab switch, median | 299 / 274 ms | 272 / 346 ms |
| scroll under load | 37.6 / 37.1 fps | 39.6 / 32.9 fps |

Loading 36 real sites at once varies by more between two runs of the
same build than between the builds (Cloudflare challenges, ad auctions
and the sites' own variance); the request-level numbers are the more
reliable measure.

With alt-svc off, the 24 sites again (3,266 requests):

| per request, ms | one thread, alt-svc on | four threads | four threads, alt-svc off |
| --- | --- | --- | --- |
| queue, median / p90 / p99 | 103 / 498 / 990 | 51 / 617 / 2,867 | 1 / 10 / 72 |
| TLS handshake, median | 337 | 181 | 44 |
| time to first byte, median / p90 | 489 / 2,011 | 218 / 1,306 | 71 / 318 |
| total, median / p90 / p99 | 2,011 / 4,847 / 11,514 | 1,042 / 5,936 / 14,627 | 190 / 766 / 2,430 |
| requests completed in the busiest 5 s | 435 | 536 | 1,102 |

and three windows of twelve sites against the installed build, one pair:

| 3x12, X399 | installed | final (`bundle-n_0eipp3`) |
| --- | --- | --- |
| load, median / p90 / all done | 24.4 / 66.0 / 86.5 s | 7.8 / 20.3 / 60.7 s |
| the three selected tabs | 32.1 / 18.8 / 49.5 s | 6.6 / 8.2 / 16.8 s |
| tab switch, median | 268 ms | 324 ms |
| scroll under load | 37.6 fps, 16 long gaps | 36.1 fps, 14 long gaps |

Scrolling while 24 tabs reload is unchanged: it is bounded by the 7 Mpx
readback per frame at 200% (above), not by the CPU. Two traces of that
phase: the compositor's frames in the burst spend a median 14 ms in the
readback and present ("send"), and the gaps over 33 ms come with a tile
flush of up to 119 ms or a readback of up to 59 ms; a new
`SUMMIT_UI_STALL_TRACE=1` (dispatches over 50 ms on Summit's application
and window threads, and profile saves) shows nothing longer than 90 ms on
the UI side during the burst. The "longest gap" of 1.75-1.8 s that every
3x12 run reports is not a stall: 240 notches reach the end of the
Wikipedia article before the burst ends, and the next frame is the
scrollbar fading out, the same artifact `animationTailScroll` has. Read
the count of gaps over 33 ms (11-16 of ~250 frames) instead. One page at a time, in a fresh profile, first visit and a second visit
after closing the tab (`loadFinishedAt - loadStartedAt`):

| page | installed, cold / warm | final, cold / warm |
| --- | --- | --- |
| Wikipedia (Haiku article) | 2.63 / 2.12 s | 2.63 / 0.90 s |
| The Guardian | 11.7 / 1.35 s | 11.2 / 1.35 s |
| CNN | 38.4 / 11.3 s | 37.4 / 10.2 s |
| GitHub (WebKit) | 1.56 / 0.34 s | 1.06 / 0.35 s |
| haiku-os.org | 0.98 / 0.96 s | 1.03 / 0.43 s |
| Ars Technica | 4.43 / 3.91 s | 4.32 / 3.28 s |
| BBC News | 1.56 / 1.48 s | 1.56 / 1.25 s |
| apple.com | 3.57 / 3.44 s | 3.45 / 3.33 s |

A single page's first visit is bound by the network (CNN's and The
Guardian's load events wait tens of seconds on hosts this network drops,
as The Verge's did); the second visit, mostly cache revalidations, gains
from the request set-up no longer costing two file operations each. The
change is largest with many tabs, where those set-ups queued.

Settled tabs switch in 17-60 ms, or instantly when the page has not
changed since it was hidden (the kept frame is shown); the 250-350 ms
medians in the 3x12 runs are switches to tabs still loading, waiting for
their own first frame.

Video start (`media.html`, the 1080p NVDEC clip from the bench server):
`playing` fires 153-170 ms after the page starts and the first advancing
`timeupdate` arrives at about 400 ms, the same with both builds.

Speedometer 3.1 (local copy, 5 iterations, two alternated pairs) is
unchanged by the priorities and the network changes: 7.67 / 7.66 installed,
7.68 / 7.84 final. Installed as the desktop launcher (`Summit-current.sh`)
at 22:17; the same build carries the extension package cache, the popup
pre-load and the resolver warm-up. `SUMMIT_BACKGROUND_PROCESS_PRIORITY=0`,
`SUMMIT_UI_PRIORITY=normal`, `SUMMIT_CURL_THREADS=1` and
`SUMMIT_CURL_ALTSVC=1` each restore one previous behaviour for comparison.

## 28 September 2026: installed build, and a profile retrain that lost

Installed on the X399 at 13:03: `bundle-u3r1x1qd`, built in `SkiaCGMiPGO`
with the 25 September profiles (Speedometer 8.01; the 12:49 install scored
7.86 / 7.88). Over yesterday's install (`bundle-05tk1w24`) it adds
contents-layer damage, the compositor's live check before waiting for a
buffer, pausing invisible muted autoplay, the larger HTTP connection pool,
`SUMMIT_NET_TRACE`, a memory pressure monitor and reused video frame buffers
(all below). The launchers it replaced today are kept as
`Summit-current.pre-20260928-1029.sh`, `-1138.sh`, `-1249.sh` and
`-1303.sh`. The table compares `bundle-c1ue7ry9`, the first of today's
installs, with yesterday's.

| X399, 200% | `bundle-05tk1w24` | `bundle-c1ue7ry9` |
| --- | --- | --- |
| Speedometer 3.1, three interleaved pairs | 7.93 / 7.76 / 7.60 | 8.14 / 7.99 / 7.86 (+3%) |
| 12 tabs, resident after 20 s idle | 3.8 GB (27 Sept run) | 3.8 GB |
| 12 tabs, idle cores | 0.09 (27 Sept run) | 0.08 |
| Tab switch, median / worst | 240 / 582 ms (27 Sept run) | 220 / 716 ms |
| Scroll fixture | | 58 fps, worst frame 29 ms |
| 4-ref H.264 (NVDEC), seek to 6.5 s | | plays, seeks, ends |
| 16-ref H.264 (libavcodec fallback), seek | | plays, seeks, ends |
| MSE fixture | | 360 frames, 0 dropped |

The MSE fixture page does not post its result to `run-probe.py`, so the probe
waits for its timeout; read the page title or a screenshot instead.

**Crash reports.** None since yesterday evening. The last ones on the X399
Desktop (27 September, 20:27-20:37 local) are the NVDEC `memcpy` at
`nvdec_h264.c:883`, fixed and installed by the OS session later that night.
Two older NetworkProcess reports died while the process was already exiting:
one in OpenSSL (`CRYPTO_THREAD_read_lock` from a libcurl thread after exit
had started), one in `WebSWServerToContextConnection::terminateWorker`. The
second was not a shutdown at all: a libcurl resolver thread was in
`_kern_exit_team`, called from `getaddrinfo` -> `nsdispatch` ->
`_nsconfigure` -> `_nsyyparse`/`_nsyylex`, i.e. libnetwork's flex scanner
for `/etc/nsswitch.conf` called `exit()`. Its teardown ended the main run
loop, and `platformFinalize` then crashed posting IPC through libbe's
already-destroyed token space. The parse is locked (`_REENTRANT` is defined),
so this is not two threads parsing at once; flex exits on a failed read,
which fits a descriptor closed underneath it. One report in a week; left as
a lead (a libnetwork parser should not `exit()` its host process).

### Video: a fifth less CPU per stream

A looping 1080p NVDEC clip (`media.html?src=refs-media/refs4.mp4&loop=1`,
new `loop=1`), per-thread CPU over 5 s from `ps -a`: the web process used
0.87-0.88 cores, of which the compositor thread 0.36-0.39 (upload, composite,
readback), the video decoder thread 0.23-0.24, Skia's painters 0.09 (the
controls) and the main thread 0.07; app_server another 0.25-0.33 for the
window.

The file engine hands each decoded `BBitmap` to the compositor as a copy,
and allocated that copy for every frame: 8 MB at 1080p, fresh pages the
kernel had to fault in and zero each time (freed memory goes back with
`MADV_FREE`). The copies now go into buffers reused once the compositor has
released them, as the MSE player already did. The decoder thread fell to
0.10 cores and the web process to 0.67-0.68 (two alternated pairs; the clips
still play, seek and end, the MSE fixture shows 360 frames with none
dropped).

Next for video: the NVDEC add-on converts every picture to RGB32 on the CPU
and the compositor uploads 8 MB per 1080p frame. TextureMapper can draw NV12
(`drawTextureSemiPlanarYUV`, `CoordinatedPlatformLayerBufferYUV`), which is
what NVDEC produces, at 3 MB a frame and no conversion; that needs an NV12
output mode in the add-on (it offers RGB32 and YCbCr422 today, and the
packed-YUV shader only takes AYUV) and a buffer that uploads the two planes.

### Memory: what a tab is made of, and a pressure monitor

Resident memory by area (`listarea`), one site per browser at 200%, 30 s
after loading, for the page's web process:

| Page | total | mimalloc + JS heap | GPU host memory (NVRM sysmem) | libroot heap | frame buffers |
| --- | --- | --- | --- | --- | --- |
| about:blank (a new tab) | 171 MiB | 32 MiB | 62 MiB | 28 MiB | 27 MiB |
| Wikipedia | 231 MiB | 87 MiB | 32 MiB | 35 MiB | 54 MiB |
| GitHub | 345 MiB | 185 MiB | 38 MiB | 45 MiB | 54 MiB |
| YouTube | 735 MiB | 467 MiB | 136 MiB | 53 MiB | 54 MiB |
| CNN | 851 MiB | 599 MiB | 76 MiB | 81 MiB | 54 MiB |

(Anonymous areas are named after the library that mapped them; mimalloc and
the JavaScript heap live in JavaScriptCore's. The frame buffers, two
3840x1826 buffers while visible, are shared with the UI process and counted
in both. A prewarmed process with no page yet holds 31 MiB.)

- **Hiding a tab frees almost nothing**: CNN hidden for 30 s held 898 MiB
  against 855 visible. One frame buffer goes back, but the page's GL
  contexts, tiles and zink's caches stay (`suspendPainting()` only pauses
  rendering).
- **Every tab pays 60-80 MiB of GPU host memory before loading anything**
  (a blank tab in its own browser: 193 MiB, 81 of it GPU host memory): 21
  zink buffer slabs of 2 MiB, a 26.75 MiB staging copy of the frame for
  readback, and a 9.2 MiB buffer. `ZINK_DESCRIPTORS=lazy` and one GPU
  painting thread instead of two changed nothing. Mesa's readpixels cache
  (`ST_DEBUG=noreadpixcache`) is not the staging buffer's owner and made
  readback no faster (a 0.08 Mpx read costs ~4 ms either way: that is the
  wait for the GPU to finish the frame).
- **`WTF::memoryFootprint()` on Haiku was wrong**: it summed the loaded
  images' data segments (and passed a null cookie to `get_next_image_info`).
  Nothing uses it on Haiku today (the periodic memory monitor is off); it now
  returns resident memory, as on Linux.

**Memory pressure now reaches the web processes.** WebKit only releases
memory when told the system is short, and on Haiku nobody told it: the
Linux UI process polls `/proc/meminfo` and sends
`DidReceiveMemoryPressureEvent`, which was compiled for Linux only.
`UIProcess/haiku/MemoryPressureMonitorHaiku.cpp` polls `get_system_info()`
(file cache counts as available) with the Linux policy: every 5 s below 50%
in use, down to every second at 85%, pressure from 90%, critical from 95%.
The web and network processes then collect garbage, drop compiled code and
caches, and give free heap back through mimalloc. `WEBKIT_DISABLE_MEMORY_PRESSURE_MONITOR=1`
turns it off; `SUMMIT_MEMORY_PRESSURE_PERCENT=<n>` moves the thresholds to
<n> and <n>+5 for testing. Held permanently at critical, twelve tabs idled
at 3460 / 3429 MiB against 3728 / 3725 MiB without it: about 280 MiB (7.5%)
is reclaimable, the rest is live. That is what a machine with less memory
gets back before it runs out; on the X399 (64 GB) the monitor never fires.

### Page loads: what the network trace shows

`SUMMIT_NET_TRACE=1` (new) makes the network process log each request when
it is queued and when it completes (status, time queued, DNS, connect, TLS,
time to first byte, total, bytes, protocol), fails or is cancelled.

**The Verge's 41 s.** Its load event waits on three ad user-sync requests
(`ads.yieldmo.com`, `cm-supply-web.gammaplatform.com`, `ads.stickyadstv.com`)
that end with curl's 30 s connect timeout. Their addresses are real, but no
connection to them ever completes from the X399 or from the Linux host: this
network drops them. Any browser here waits on them (Firefox's connect timeout
is 90 s), so this is not a Summit problem; `WEBKIT_CURL_CONNECT_TIMEOUT=<s>`
shortens it if wanted. Everything else on the page: 570 requests, time
queued at most 9 ms, median time to first byte 136 ms; the slowest responses
are ad auctions (up to 2.1 s).

**Connections closed between bursts.** Those 570 requests to 141 hosts made
203 TLS handshakes, four to www.theverge.com alone. libcurl sizes its
connection cache at four per transfer in progress unless `CURLMOPT_MAXCONNECTS`
is set, and WebKit did not set it, so when a page pauses between bursts its
HTTP/2 connections are closed and the next burst pays for new handshakes
(`CURLOPT_PIPEWAIT` and TLS session sharing were already on). With a cache
of 128 (the Haiku default now; `WEBKIT_CURL_MAXCONNECTS` overrides it) the
same page made 129 handshakes for 132 hosts, two to www.theverge.com. Reading
a Wikipedia article, idling 15 s and following a link: the second load made
2 / 2 new handshakes before and 0 / 1 after (0.80 / 1.23 s against
2.23 / 1.23 s). Twelve sites loading at once were within noise (median 2.55 /
3.29 s against 3.7 / 2.9 s, p90 5.3 against 6.2 s); with that many transfers
in flight the default cache is already large.

### Scrolling real pages at 200%

`run-scroll.py --ui-frame-stats` (120 wheel notches at 16 ms, window
1920x1056 at 200%), 2 s after navigating (still loading) and after 20 s.
Frames the view received during the 1.9 s burst (`frameSnapshot`):

| Page | while loading | settled |
| --- | --- | --- |
| CNN | 52 fps, longest gap 125 ms | 54 fps, longest gap 61 ms |
| The Guardian | 38 fps, longest gap 49 ms | 39 fps, longest gap 94 ms |
| Wikipedia (Haiku article) | 48 fps, longest gap 72 ms | 50 fps, longest gap 88 ms |

Read the burst numbers, not `animationTailScroll`: its window runs past the
last notch, so a frame that arrives a second later (a scrollbar fading out)
shows up as a 0.5-1.4 s "worst interval" and drags its fps down; none of those
gaps happened while the page was scrolling. Across 26 Wikipedia bursts today
the longest gap inside a burst was 74-151 ms and the rate 30-50 fps, with
every wheel event handled on the scrolling thread (`SUMMIT_WHEEL_ROUTE_TRACE=1`:
118/118 `route=scrolling handled=1 needsMain=0`). Painting on the CPU instead
(`SUMMIT_DISABLE_GL_COMPOSITING=1`) was not faster (31.9 against 38.1 fps
over three alternated pairs).

With `SUMMIT_COMPOSITOR_TIMING_TRACE=1 SUMMIT_PRESENT_STATS=1` the 110
scroll frames of the Wikipedia burst cost 20.2 ms each on average: readback
and present 14.2 ms (p90 19.4, max 41.8; the full 7 Mpx, since scrolling
moves everything), flushing new tiles 3.7 ms (p90 12.4, max 48.5), and
compositing 2.1 ms. The standalone probe reads the same 7 Mpx in 8.3 ms;
inside a scrolling page the GPU is also compositing and taking tile
uploads, at the boot clocks NVK leaves a GTX 1070 on (an assumption; the
GPU reported its fastest state when it was read on 29 September). That puts 200% scrolling
at 40-55 fps on this machine until frames can reach app_server without a
readback (or NVK can raise the clocks), both on the OS side.

A harness trap: `screenshot` takes 4.6-5.0 s on the X399 and holds up
drawing while it runs, so `run-scroll.py`'s before and after screenshots
show up in the UI frame statistics as 3.6-4.3 s stalls with frames queued
(`queueMax=` in `browser.log`). They are not Summit stalls; compare the burst
numbers, which the screenshots do not overlap.

### Tried and taken out: coalescing frame messages to the view

While `screenshot` holds app_server (4.6 s), the UI keeps accepting frames
and queues one `viewFrameHaiku` message per frame for the window: 212-274 of
them, drained at about 3.6 ms each once drawing resumes. Sending at most one
message while one is pending (damage merged in the view state) cut the
backlog to 15. But across eight Wikipedia bursts alternated with the build
before it, the view received 39.2 frames/s during the burst against 46.6,
and a variant that kept one message per frame and only merged damage
measured the same. Unexplained, and a multi-second app_server stall is rare,
so both were reverted.

### The retrain: lost merges, so the old profiles stay

The engine's profile-guided build uses profiles from 25 September, before
MSE, damage tracking and today's work. `tools/bench/train-pgo.sh` runs a
training against an instrumented bundle: Speedometer 3.1, the scroll fixture,
a Wikipedia wheel burst, two windows of real sites with a scroll under load,
and video through NVDEC, libavcodec and MSE. Build directory `SkiaCGMiPGO2`
(llvm-ar), instrumented with `-fprofile-generate -fprofile-update=atomic` and
linked with `-u__gcov_dump`, trained with `SUMMIT_GCOV_DUMP=1` (1,982 profile
files; a copy is in `/boot/home/summit/pgo2-profiles-20260928.tar`), then
rebuilt in place with `-fprofile-use -fprofile-correction
-fprofile-partial-training` and empty linker flags.

The result, `bundle-ooanr2gr`, lost: 7.20 / 7.33 / 7.37 on Speedometer
against 7.83 / 7.87 / 7.76 for the installed build, interleaved (-6.7%).
The same engine commit built with the *old* profiles (`bundle-c1ue7ry9`,
only TextureMapperLayer.cpp and ThreadedCompositor.cpp recompiled) scored
8.14 / 7.99 / 7.86 against 7.93 / 7.76 / 7.60 (+3%), so the code is fine and
the new profiles are not.

`gcov-dump -l` shows why. Every old profile file carries the same summary
(`runs=6, sum_max=1051265313`); the new ones do not agree with each other:

| Unit | old profile | new profile |
| --- | --- | --- |
| dom-1 | runs=6 | runs=2, sum_max 31 M |
| JSBindings-1 | runs=6 | runs=3, sum_max 31 M |
| page-1 | runs=6 | runs=1, sum_max 0.4 M |
| style-1 | runs=6 | runs=39, sum_max 350 M |
| html-1 | runs=7 | runs=40, sum_max 597 M |

DOM and bindings code runs in every web process, so it should have as many
runs as style. **Haiku's `libgcov.a` never locks a `.gcda` file** (it calls
`fopen` and imports no `fcntl`; GCC only locks where the target defines
`TARGET_POSIX_IO`, which Haiku's apparently does not). When a window of tabs closes, a dozen processes read,
merge and rewrite the same files at once and the last writer wins; the
Speedometer web process's counts for the hottest code were among those
lost, so GCC treated that code as cold. The same race left 33 files
half-written: "profile data for function ... is corrupted" / "checksum is
(x,0) instead of (x,y)" (21 in WebCore, 12 in WebKit; one crashed the
compiler).

Fix, for the next training: instrumented processes now take an `fcntl` lock
on `SUMMIT_GCOV_LOCK` (default `/tmp/summit-gcov.lock`) around
`__gcov_dump()` (WTFProcess.cpp; the UI process through `atexit`), the
harness gives helpers `SUMMIT_BENCH_LEFTOVER_GRACE=180` seconds to take
their turns, and `train-pgo.sh` ends by printing the run count of sample
units. They should all be close; if DOM or bindings units show a handful of
runs while style shows dozens, merges were lost again.

Other things the attempt ran into:

- **Compiles that never finish.** Six units (WebCore html-9, html-11,
  platform-47, svg-15, workers-8; WebKit NetworkProcess-11) kept `cc1plus`
  busy for hours under the new profiles, with more kernel time than user
  time. They were still going after 4½ hours, and the X399 hung outright once
  while they ran. Without a profile each compiles in minutes (profiles moved
  to `/boot/home/summit/pgo2-stall`; the corrupted ones to `pgo2-bad`). Maybe
  a side effect of the damaged merges; if a clean training does it again,
  ninja sitting on a handful of jobs with a load of exactly that many is the
  sign.
- A hard power cut on Haiku loses recently written file data even when the
  directory entries survive: after the first hang 25 objects were zero bytes
  and ninja's log was 90 steps behind. `find WebKitBuild/... -name '*.o'
  -size 0` after any reset, and `sync` after anything that must survive.

## 27 September 2026 (evening): memory per tab, idle cores, NVDEC, crashes

Measured on the X399 at 200% with `tools/bench/run-multitab.py` (one window
of twelve real sites unless noted), `listarea` per web process, and the
engine's new traces. Everything below is in the PGO engine (`SkiaCGMiPGO`).

| 12 tabs, one window | before | after |
| --- | --- | --- |
| Resident after loading | 7.5 GB | 3.8 GB |
| After 20 s idle | 15.7 GB, growing | 3.8 GB, flat |
| After switching through every tab | 18.9 GB | 6.7 GB |
| Frame buffers held by a hidden tab | up to 223 (6 GB) | 1 (27 MB) |
| Tab switch, median / worst | 321 / 1642 ms | 240 / 582 ms |

### Frame buffers that never came back

Most of a tab's memory was **27 MB frame buffers** (one frame at 200%),
hundreds per web process, mapped twice (web and UI process) and each with
frame-sized GPU buffers beside it, until the GTX's memory ran out too.
Three faults, found with `SUMMIT_SWAPCHAIN_TRACE=1` (every buffer lent,
returned and trimmed, in both processes):

- **Surface identifiers collide.** Every web process numbers its surfaces
  from 1, and the UI process kept its backing stores in a map keyed by that
  number. A buffer handed back after a tab was shown again went to another
  tab's store and was dropped, so that tab's swap chain never saw it again.
  Stores are now keyed by an identifier unique in the UI process.
- **Release after FrameDone.** With zero-copy presentation the UI returns a
  buffer when a newer frame replaces it, but it posted that release behind
  every other task on its main thread while FrameDone went out at once; with
  many tabs loading the web process repeatedly found no free buffer. The
  release now goes first when the replacement happens on the main thread.
- **No limit.** The swap chain only `ASSERT`s its four-buffer limit, so
  release builds allocated one more buffer per frame. The compositor now
  waits for a buffer when all four are out, and paints into a fifth only if
  none has come back for two seconds (logged).

Hidden tabs now keep one frame (the kept frame shown on switching back) and
trim the rest ten seconds after hiding, as upstream intends.

### Freed memory that never left

Haiku accepts `madvise(MADV_DONTNEED)` and ignores it (the kernel leaves it
a TODO); only `MADV_FREE` discards pages. mimalloc's purge and WTF's
`OSAllocator::decommit` both used `MADV_DONTNEED`, so a web process's heap
could only grow. Both use `MADV_FREE` on Haiku now: 12 tabs went from 5.96
to 5.32 GB of web-process memory. What remains per tab is mostly the page's
own heap: 40-150 MB for light pages, 400-650 MB for heavy news and video
pages, plus about 25 MB of GPU driver memory and one frame when hidden.

36 tabs in three windows (the previous benchmark): 10.5 GB after loading
and 9.9 GB idle, against 13.7 GB for 38 processes before; all 36 loaded
(median 10.1 s, all within 49.8 s, previously 75 s for the slowest).

### An idle page that took a core

Stack Overflow kept one web process at 100% of a core while idle.
A trace (`SUMMIT_IDLE_TRACE=1`, since removed, see below) showed 47,000 event-loop turns a second, each
running one microtask queued by a mutation record from an image load event;
the image trace then named the script: the sign-up modal
(`signup-modal.en.js`) sets an image's `src` again when it fails, and
Cloudflare answers Summit's requests for that logo with its "Verify you are
human" page. Firefox got the image and never looped. The challenge was
Cloudflare's reaction to this machine at the time, not to Summit as such:
three hours later a fresh Summit profile and curl with Summit's exact
headers both got the PNG. (The X399 had loaded hundreds of pages in rapid
automated runs that afternoon.) After three errors for one URL on one element within two seconds,
further error events now wait 10 ms, doubling to one second; the page still
gets each one. Stack Overflow idles at 0.04 cores, and any page that retries a failing
image the same way stays cheap.

### Reading back only what changed

smh.com.au kept its process at 1.2-1.4 cores while idle: an animated element
and the page's scrollbar changed every frame, and every frame read all
7 Mpx back from the GPU (11.3 ms at 200%, `SUMMIT_PRESENT_STATS=1`). The
damage was right but widened on the way: each render target kept its damage
as one bounding box (GL surfaces were not marked as walking damage
rectangles, though the Haiku readback does), pages united each frame's
damage into one box (`UnifyDamagedRegions`, now off; `SUMMIT_UNIFY_DAMAGE=1`
restores it), and past 16 rectangles the readback read their bounds. Now
touching grid cells join into exact strips and bands, and pairs merge only
while that costs fewer pixels than another read (up to 32 pieces; beyond
that most of the frame changed anyway). The coordinated scrollbar also stopped
repainting when it would draw the same pixels. smh.com.au reads 0.08 Mpx a
frame; the read still takes 5.8 ms because zink's `glReadPixels` waits for
the GPU, which TextureMapper has made redraw the whole bounding box (a 2x2
speck at the top-left and the scrollbar at the right). Its process went from
1.3 to 0.97 cores. Final frames of a Wikipedia scroll match the old build.

### Contents layers damaged every frame

`TextureMapperLayer::collectDamageSelf()` treats every layer with a contents
layer as wholly damaged on every composition ("fully damaged for now.
FIXME: Remove that special case."). Page scrollbars, canvases and directly
composited images therefore joined the damage, the GPU redraw and the
readback of every frame, whatever changed: on smh.com.au a 2x2 speck and the
scrollbar made the bounding box the whole frame. `CoordinatedPlatformLayer`
already damages a layer when a new contents buffer or image arrives (the
region it is given, or all of it), so on Haiku the special case is gone;
`SUMMIT_CONTENTS_LAYER_DAMAGE=full` restores it.

### Scrolling while 24 other tabs reload

`run-multitab.py` scrolls the first window while every tab of the other two
reloads: about 20 frames/s during the burst and gaps up to 0.5-0.7 s, in
every build tried today. Traced (`SUMMIT_COMPOSITOR_TIMING_TRACE=1` now tags
lines with `pid=`; `SUMMIT_INPUT_DELAY_TRACE=1`; `SUMMIT_IPC_SLOW_TRACE=1`):

- It is not the UI process: it used 0.13 cores, wheel events never waited
  over 20 ms for its main thread, and its slowest IPC handler
  (`DidFinishLoadForFrame`) took 58 ms.
- In the scrolled page's process each frame costs about 20 ms (a full 7 Mpx
  readback, since scrolling changes everything), new tiles pile up (up to
  244 pending, one flush spent 80 ms uploading them), and for stretches of
  up to 150 ms the scrolling thread asked for nothing.
- Two, four or six Skia painting threads made no difference beyond noise.

On Haiku WebKit's thread quality of service was ignored: the scrolling
thread (`UserInteractive`) ran at the same priority as 24 pages loading. It
now maps to Haiku priorities as Linux maps it to scheduling classes
(user-interactive 15, user-initiated 12, utility 7, background 5), and the
compositor's queue is user-interactive. That did not change the numbers
above measurably; lowering the page thread of processes whose pages are all
hidden seemed to slow loading and was taken out again. The remaining costs
are structural: a full-frame readback per scrolled frame at 200% on a GPU
whose clocks NVK cannot raise, and painting and uploading the newly exposed
tiles.

### Speedometer, and what the traces cost

Interleaved 10-iteration runs (default window, 1913x933) put today's build
2-4% behind the installed `r6ti1k5v` (7.91/8.02 against 8.11/8.27; earlier
pairs 7.64-7.86 against 8.14-8.22). Switching off the buffer wait
(`SUMMIT_SWAPCHAIN_WAIT=0`), MADV_FREE (`SUMMIT_DECOMMIT=0`,
`MIMALLOC_PURGE_DELAY=-1`) or fine damage (`SUMMIT_FINE_DAMAGE=0`,
`SUMMIT_UNIFY_DAMAGE=1`) did not recover it. The engine is profile-guided
with profiles from the previous code, and a function whose code changes
loses its profile: the diagnostic traces had gone into the event loop,
microtask queueing, attribute changes, IPC dispatch and run-loop turns.
Taking them out of those paths halved the gap; the rest is the fixes
themselves in the compositor, readback and image loader until the profiles
are retrained. The traces (`SUMMIT_IDLE_TRACE`, `SUMMIT_IPC_SLOW_TRACE`)
are in commit `cbac2fd`'s engine patch for the next time they are needed.

### What a readback costs, and why the pixel buffer path stays off

One playing 1080p video (NVDEC) costs its web process 1.15 cores; most of
every animated frame is the readback. A standalone probe on the X399
(`tools/mesa-vm/readback-verify.cpp`) clears to a new colour each round and checks what
comes back:

| 3840x1826 (7 Mpx) | time | right |
| --- | --- | --- |
| `glReadPixels` into memory, BGRA or RGBA | 8.3 ms (4.3 of it the 28 MB copy) | all |
| same, 2560x1440 part | 5.6 ms | all |
| into a pixel buffer, reused / orphaned / fresh / ring / with a fence | 4.6-5.7 ms | **none after the first** |

A mapped pixel buffer holds some earlier frame the first time and zeros
after that, even after `glFinish`, a fence and half a second: zink's copy
into the buffer never lands where the map reads on NVK (a Mesa/NVK bug,
not reuse or caching). Until that is fixed the readback stays direct, and
its copy runs at one core's memory bandwidth. Video start is quick: the
local 1080p clip reports `playing` 160 ms after `loadstart`.

### Videos nobody can see

CNN's front page kept 2.35 cores busy while idle: five muted autoplay videos
decoding at once, most of them below the fold. WebKit can pause a muted
video that started itself while none of it is on screen and resume it when
it comes back (`InvisibleAutoplayNotPermitted`, on by default only on iOS);
it is now on (`SUMMIT_PAUSE_INVISIBLE_AUTOPLAY=0` turns it off). One video
plays and the page takes 1.42 cores. The rest is that video and a full-width
band at the top that repaints every frame (2.4 Mpx read back per frame).

### NVDEC and 16 reference frames

A probe on the X399 (1080p x264 clips, level 5.1, 3 B-frames,
`tools/bench/make-refs-fixtures.sh`) showed the NVDEC add-on decoding all 300
pictures with up to 15 reference frames and 22 with 16. Haiku gives a format
to one decoder add-on with no fallback, so Summit now picks libavcodec itself
(`BMediaDecoder::SetTo()` with the ffmpeg add-on's encoder identifier) when
the chosen decoder is `nvdec h264` and the stream's SPS asks for more than
15 references (`SUMMIT_NVDEC_MAX_REFERENCES` overrides). Both media engines
use it; the file engine reads the track's chunks raw. The 16-reference clip
now plays all 300 pictures and seeks (`media.html?src=refs-media/refs16.mp4&seek=6.5`);
15 references stay on NVDEC. The OS session knows: renaming the add-on
when it can hold 16 references switches the fallback off.

### Crashes

- 15 of the 65 crash reports on the X399 were NVK crashing in
  `vk_graphics_pipeline_state_merge`: zink linked graphics pipeline libraries
  without checking that they had been built, and one that failed (under the
  memory pressure above) was a null handle. The private Mesa now builds a
  whole pipeline instead (`tools/mesa-vm/mesa-25.3.6-summit-03-zink-missing-library.patch`;
  the previous prefix is kept as `/boot/home/summit-mesa/prefix.pre-20260927`).
- Most of the rest were zink's error path: when creating a screen failed
  (the GPU out of memory, again from the leak), `zink_destroy_screen` tore
  down a buffer cache whose mutex was never initialized and asserted in
  `mtx_lock` inside `eglInitialize`
  (`mesa-25.3.6-summit-04-zink-failed-screen.patch`).
- Force-quitting did not cause them by itself: web processes already `_exit`
  when the UI process goes away; they crashed because the GPU had run out of
  memory. No run after the buffer fixes (dozens of launches and quits)
  produced one of these again.
- One more crashed four times in the 36-tab runs: the NVDEC add-on's slice
  copy (`nvdecH264Decode`, `nvdec_h264.c:883`). The add-on kept its NAL table
  in a function-level `static`, so two videos in one web process rewrote
  each other's table. The OS session fixed it (Haiku 1323d41adc, installed on
  the X399 21:50) and committed Summit's own add-on changes with it
  (8b671d6b06 `fReason`, a33277609e skipping conversion before a seek
  target), so `tools/build-nvdec-plugin.sh` in the OS tree is safe again.

## 27 September 2026: installed on the X399, and what it measured

The desktop launcher (`/boot/home/Desktop/Summit-current.sh`) points to
`bundle-r6ti1k5v`: the profile-guided engine (`SkiaCGMiPGO`) with everything
in the list below; the previous launcher is `Summit-current.pre-20260927-1011.sh`
(`bundle-q2h888fj`). Measured on the X399 at 200% against `bundle-6lhzlu2f`
(the last X399 build, zero-copy only):

| | before (`6lhzlu2f`) | now (`d4ull89i`/`r6ti1k5v`) |
| --- | --- | --- |
| Wikipedia scroll burst, per-second frame rates | 50-60, 10 long frames | 52-61, 7 long frames |
| Readback per frame while scrolling (web process) | 11.4 ms, 7.0 Mpx | 10.7 ms, 5.5 Mpx |
| Readback per frame, small changes (banner, caret) | 11.3 ms, 7.0 Mpx | 1.25 ms, 0.08 Mpx |
| 3 windows x 12 tabs: pages that loaded | 14 of 36 | 35 of 36 |
| Video (1080p MP4): time to playing | 0.78 s | 0.32-0.92 s |

- **22 of 36 tabs never loaded before.** Web processes could not reach the
  network process ("internallyFailedLoadTimerFired" 62 times): Haiku's 256
  descriptors per process. With 4096 they all connect. Loading real pages
  costs what it costs: 13.7 GB resident for 38 web processes; loads took a
  median 8.7 s, 26 s at the 90th percentile and 75 s for the slowest
  (weather.com), all 36 at once.
- **Every page was black** on the first X399 build of these changes: the
  pixel-buffer readback (fine on llvmpipe) returns zeros or an old frame on
  zink/NVK; `tools/mesa-vm`-style probe in the commit. Changed rectangles are
  now read straight into the shared frame (`SUMMIT_PBO_READBACK=1` restores
  the pixel buffer).
- **Video on the X399**: the clip is decoded by NVDEC, which reports real
  times; a pacing guard for Haiku's ffmpeg time-base bug had ended it after
  2.5 s and now only applies when the decoder's time lags. NVDEC itself shows
  only 74 of the clip's 300 frames: the stream is level 5.1 with 16 reference
  frames and the add-on's 17 hardware picture slots (the engine's luma
  offsets 0-16) run out, so it skips to the next IDR at 8.33 s. More slots
  corrupt the picture; the fix belongs in the add-on (show waiting pictures
  from a copy), reported to the OS session. Streaming makes no difference to
  start on the X399's LAN (the whole 30 MB arrives in 0.6 s); it helps on
  slower links (VM: 2.0 s against 3.1-3.6 s).
- Quitting a browser with many tabs by force can leave web processes crashing
  in zink's teardown (`vk_create_graphics_pipeline`, `zink_destroy_screen`),
  which only shows as crash reports.

## 26 September 2026 (evening): 200% screens, many tabs, loading

The X399 now drives two 4K monitors at 200% (docs/hidpi.md). Summit renders
pages at the screen's density, which makes each frame four times larger, so
the frame path was measured stage by stage (`SUMMIT_PRESENT_STATS=1`) while
scrolling Wikipedia in a 1920x1056 window:

| Stage | 1x | 2x before | 2x after |
| --- | --- | --- | --- |
| GPU composite + readback (web process) | 5.3 ms | 11.5 ms | 11.5 ms |
| Copy into a BBitmap (UI process) | 1.1 ms | 4.3 ms | gone (zero-copy) |
| Frames/s during the wheel burst (per-second samples) | 59-60 | 41-52 | 53-59 |

`tools/mesa-vm/readback-probe.cpp` shows a 4K readback through a pixel buffer
object at 5.3 ms (0.9 ms GPU copy + 4.4 ms memcpy) against 8.9 ms direct; in
the browser the stage stays near 11.5 ms because mapping waits for the GPU to
finish compositing. `run-scroll.py`'s overall fps includes a two-second gap
when the page reaches its end; use the per-second `frameSamples`.

Built on X399 only as far as zero-copy (`bundle-6lhzlu2f`). X399 then went
down (another session's Wi-Fi driver hung it before POST), and the following
changes wait for a build there; a full build in the VM (`SkiaCGMi`, non-PGO)
checks that they compile:

- **Damage tracking** (`ENABLE_DAMAGE_TRACKING`, GTK's defaults): the
  compositor redraws, the web process reads back and app_server redraws only
  what changed. Check with `tools/bench/pages/spinner.html` and the
  `pixels read (web)` stat. `SUMMIT_DAMAGE_TRACKING=0` reverts.
- **256 connections in total** instead of curl's 17 (six per host stays).
- **4096 file descriptors per process** instead of Haiku's 256: web
  processes ran out and lost frame buffers ("Failed to create handle for
  shared memory buffer" in 15 of 32 extension runs in the VM), and 256
  connections would exhaust the network process. The engine's process
  launcher raises the limit before the first child starts.
- **A prewarmed web process** after each page load (`SUMMIT_PREWARM_PROCESS=0`).
- **Background tabs throttled** as in Safari and GTK: hidden-page timer
  throttling (auto-increasing) and CSS animation suspension were off on
  Haiku. A page with a 10 ms timer, hidden for 20 s, ran it 1743 times
  before and 97 times after (VM); `SUMMIT_HIDDEN_PAGE_THROTTLING=0` reverts.
  Hidden tabs already reported `visibilityState` hidden and stopped
  requestAnimationFrame. Switching back shows the tab's kept frame at once.
- **Lazy session restore**: background tabs load when first selected.
- **Streaming media**: the file engine reads over range requests instead of
  downloading whole files first (`SUMMIT_MEDIA_STREAMING=0`);
  `tools/streaming-media-test/run.sh` tests it on the host. Measured in the
  VM (`run-probe.py --page "media.html?src=…"`, a 30 MB 10 s 1080p MP4 from
  test-videos.co.uk): playing after 2.02 s streaming against 3.13-3.20 s
  downloading first; the gap grows with file size and a slower network.
- **Software-decoded video played at the wrong speed or stopped**, found
  while measuring that. A decoder parked while a paused page loads was
  resumed without a seek and started mid-stream, missing its reference
  frames: playback ended after 50 frames. And Haiku's ffmpeg decoder
  reports frame times in the stream's time base (512 per frame at 1/15360;
  Haiku 45bd581b70), so every frame looked late and a 10 s clip ran in half
  a second. A resumed track is now always sought to the current time, and
  video is paced from the frame counter when the decoder's time disagrees
  with it; both clips now play all 299 frames in 10 s. NVDEC (X399) reports
  real times and keeps them.

To measure them: `tools/bench/run-multitab.py` (three windows of twelve real
sites: load times, busy cores, idle cost, tab switch latency, scrolling while
the other windows reload, memory, `--restore` for a relaunch), with
`--env SUMMIT_LIBRARY_PATH_PREFIX=/boot/home/summit-mesa/prefix/lib`.
Skia's CPU tile painters are limited to two on Haiku (chosen at 1x); at 200%
compare `WEBKIT_SKIA_CPU_PAINTING_THREADS=2/4/6`.

## Handoff, 26 September 2026

The X399 desktop launcher (`/boot/home/Desktop/Summit-current.sh`) points to
`bundle-av2hgf30`: the PGO engine (`SkiaCGMiPGO`) now built with Media Source
Extensions, the wheel fixes, MSE preroll, NVDEC skip-conversion seeks and
video frames sent straight to the compositor (both media engines). Earlier
launchers are kept beside it as `Summit-current.pre-*.sh`. The sections that
follow, from "YouTube through Media Source Extensions" to "Mouse wheel
delivery with real input", describe what changed and how it was measured.
`WebKitBuild/SkiaCGMiMSE` is a non-PGO development build of the same sources.

Rebuilding the PGO engine after source changes:
`SUMMIT_REMOTE_SHELL=tools/ws.sh SUMMIT_REMOTE_TAG=ws SUMMIT_ENGINE_BUILD_NAME=SkiaCGMiPGO
SUMMIT_ENGINE_CXX_FLAGS="-ftrack-macro-expansion=0 -fprofile-use -fprofile-correction
-fprofile-partial-training -Wno-error=coverage-mismatch -Wno-coverage-mismatch"
SUMMIT_ENGINE_CMAKE_EXTRA="-DENABLE_MEDIA_SOURCE=ON -DUSE_SKIA=ON
-DUSE_HAIKU_GL_COMPOSITING=ON -DENABLE_ASYNC_SCROLLING=ON -DENABLE_SMOOTH_SCROLLING=ON"
bash tools/build-webkit-in-vm.sh --modern-extensions all`, then
`build-modern-browser-in-vm.sh --browser --bundle --modern-extensions` with
the same build name. The profiles predate MSE; functions that changed build
without profile data. A fresh training run would recover the rest.

Summit is single-launch per bundle path. While someone uses the installed
bundle, test from a copy (`cp -a bundle-X test-X`), or a test launch opens
tabs in their window.

Open items: a clean Speedometer run on an idle machine. Progressive start
for the file engine, which still downloads a whole file before it plays.
VP9/Opus MSE (YouTube currently gets H.264 up to its avc1 limit). A training
pass for the new media paths.

## Current coordinated Skia result (September 25, 2026)

The workstation launcher uses profile-guided `bundle-ggja7ufo`: Skia CPU tile
painting with two workers, GL Canvas, raster coordinated scrollbars, mimalloc,
asynchronous scrolling, display-rate composition pacing, guarded reuse of exact text
widths, preserved font registrations for simple CSS rule insertions, and
coalesced video repaint callbacks. The Haiku media backend also reports fully
downloaded videos as buffered. It precompiles the rounded solid-color shader
before scrolling, and caches grid item block contributions within a sizing
pass. Muted videos do not allocate audio output, and audible output is
created only while playing. Paused preload videos release their NVDEC
decoders until playback or seeking needs them. It completes all 580 steps of
Speedometer 3.1. The installed bundle's latest 30-iteration run is matched
to the Firefox comparison at 1280×887
(`.vm/bench/speedometer-20260925-140133-parked-video-repeat-thirty/`
and `.vm/bench/firefox-speedometer-20260925-092147-current-thirty-iterations/`).

| Browser | Content viewport | Speedometer 3.1 |
| --- | ---: | ---: |
| Summit, installed parked-decoder PGO, 30 iterations | 1280×887 | **8.601 ± 0.132**; earlier repeat **8.362 ± 0.127** |
| Summit, prior mute-aware PGO, 30 iterations | 1280×887 | **8.520 ± 0.129**; same-session control **8.342 ± 0.127** |
| Summit, prior grid-cache PGO, 30 iterations | 1280×887 | **8.377 ± 0.121** |
| Summit, prior grid-cache PGO, 10 iterations | 1280×887 | **8.457 ± 0.417** |
| Summit, buffered-media PGO predecessor, 30 iterations | 1280×887 | **8.391 ± 0.117** |
| Summit, prior PGO, same-session 30-iteration repeat | 1280×887 | **8.500 ± 0.123** |
| Summit, prior PGO, earlier 30-iteration run | 1280×887 | **8.561 ± 0.125** |
| Firefox 155, 30 iterations | 1280×887 | **8.405 ± 0.179** |
| Summit, prior smooth PGO and rounded-solid prewarm, 10 iterations | 1280×887 | **8.574 ± 0.468** |
| Summit, prior smooth PGO and media coalescing | 1280×887 | **8.475 ± 0.414** |
| Summit, prior smooth PGO | 1280×887 | **8.338 ± 0.398**, repeat **8.544 ± 0.434** |
| Summit, prior PGO and handled scroll refresh | 1280×887 | **8.347 ± 0.506** |
| Summit, prior PGO CPU tiles and GL Canvas | 1280×887 | **8.512 ± 0.400**, repeat **8.444 ± 0.456** |
| Summit, prior installed CPU tiles and GL Canvas | 1280×887 | **7.185 ± 0.353** |
| Summit, prior synchronous media seek build | 1280×887 | **7.288 ± 0.359** |
| Summit, earlier CPU-tile build with GL Canvas off | 1280×887 | **6.730 ± 0.302** |
| Summit, earlier batched CSS and mimalloc build | 1280×887 | **6.896 ± 0.293** |
| Firefox 155 on the same workstation | 1280×887 | **8.338 ± 0.374** |
| Firefox 155, fresh comparison | 1280×887 | **8.238 ± 0.368** |

These Summit runs were uncontended. The installed build's 30-iteration point
score is 0.196 above Firefox's, but its earlier repeat was 0.043 below.
Their uncertainty intervals overlap. The same-session control scored 8.342
between the two installed-bundle runs, so these results do not establish a
whole-browser Speedometer gain. Its
largest remaining suite gaps against Firefox are Preact, CodeMirror, and
Svelte complex DOM. Older score comparisons in this log used different
viewport sizes and should be treated as directional.
`tools/bench/compare-runs.py` flags that mismatch.
`tools/bench/compare-runs.py` gives the per-suite synchronous and asynchronous
split from the saved results.

The workstation reports 32 logical processors (16 physical cores) through
both Haiku's system information and `sysconf`. WebKit's JSC and garbage
collector discover that count independently. Summit caps Skia CPU tile
painting at two workers on Haiku because the controlled sweep below found no
Speedometer gain from four and a large regression from eight. With GL Canvas
on, the current 400-card scrolling probe ran at 59.47 fps over 600 frames,
with an 18 ms maximum and no frame above 33 ms
(`.vm/bench/probe-20260925-025454-summit-handled-refresh-scroll-fixture/`).
Earlier live Reddit scrolls showed occasional 650–790 ms gaps attributed to
page update, layout, or script work; subsequent live Reddit attempts sometimes
received a JavaScript challenge instead of the feed.

## YouTube through Media Source Extensions (September 25, 2026)

YouTube showed "Your browser can't play this video": its player needs Media
Source Extensions, and `ENABLE_MEDIA_SOURCE` was off. The only media it could
fetch were its short sound-effect clips. The file engine could not have
helped much anyway: it downloads a whole file before it plays.

The Haiku port now has an MSE backend (`MediaPlayerPrivateHaikuMSE`,
`FragmentedMP4ParserHaiku`). WebCore's generic `SourceBufferPrivate` still does
buffering, eviction and seeking. The port parses the appended fragmented MP4
and hands each track's samples to a renderer. Video is decoded with
`BMediaDecoder`, so H.264 reaches the NVDEC add-on, and is presented against
the player's clock as SkImages. AAC audio is decoded on its own thread into a
PCM queue that a `BSoundPlayer` drains in step with the same clock. It
advertises `video/mp4` with 8-bit `avc1`/`avc3` and `audio/mp4` with
`mp4a.40.x`, for MSE only. VP9, AV1 and Opus are refused, so YouTube falls back
to H.264. `navigator.mediaCapabilities.decodingInfo()` answered "unsupported"
for every format on Haiku (there was no factory); it now answers from the same
engine checks.

Two Media Kit details cost time:

- `BMediaFormats::MakeFormatFor()` with a single description registers a
  new encoding (1660 for H.264 instead of 1000), which no decoder claims, so
  `BMediaDecoder::SetTo()` fails with `B_ENTRY_NOT_FOUND`. `GetFormatFor()`
  returns the format the add-ons registered.
- Loading a MediaSource asks each engine about an empty type (the URL is a
  `blob:`). Looking an empty container up in a `HashSet<String>` crashed the
  web process.

`pages/mse.html` appends a 12-second 720p30 H.264 High-profile clip with
B-frames, plus AAC, in 256 KiB chunks. NVDEC decoded it, `loadeddata` fired at
0.26 s, a seek to 8 s settled, and `ended` fired at 12.07 s. 213 frames were
presented and 8 dropped around the seek.

On `youtube.com/watch?v=aqz-KE-bpKQ` (Big Buck Bunny, 60 fps), YouTube added
`avc1.4d4020` and `mp4a.40.2` source buffers and autoplayed. The first frame
appeared 1.26 s after the player was created; about 1 s of that is YouTube's
own script before it adds source buffers, and play to first frame took 0.11 s.
Over the next 32 s of media time the renderer presented 1,918 frames (60.0
per second) and dropped none. This is the non-PGO `SkiaCGMiMSE` build; set
`SUMMIT_MSE_TRACE=1` for the decoder, clock and 5-second statistics lines.

### Starting and seeking quickly

The PGO build of the same engine (`SkiaCGMiPGO` with MSE on, reusing the
earlier profiles with `-Wno-error=coverage-mismatch`) exposed a startup flaw.
The clock started as soon as data was buffered, the first picture was
decoded 0.86 s later, and every picture decoded in between arrived late:
68 were dropped. The player now prerolls. It starts the clock once the
renderer has shown the picture at the current position, waiting at most a
second, and the first presented frame re-evaluates the state. On
`bundle-1j8l2elb` the clock started 94 ms after `play`, with the first picture
at 0.000 s, and 4 frames were dropped over 18 s of 60 fps playback.

Starting from a link timestamp (`&t=120`) seeks before playing. Every picture
from the keyframe up to the target is decoded, and the NVDEC add-on used to
convert each to RGB although none is shown: the first picture arrived 1.13 s
after the seek settled. With `tools/nvdec/skip-conversion.patch` the renderer
tells the add-on the first time it will show, and conversion is skipped before
it. Now 0.61 s, with 0 dropped frames afterwards (two runs, `bundle-zu4mf8mm`).

Speedometer 3.1 on the MSE PGO bundle scored 8.19 ± 0.45 in ten uncontended
iterations. The harness's default window gave a 1913×945 viewport, not the
1280×887 used above, so this is only a check against a large regression
(`.vm/bench/speedometer-20260925-232809-mse-pgo-ten/`).

A two-minute run on a 25 fps video (`dQw4w9WgXcQ`) presented 3,131 frames
with none dropped and no stalls; media time kept pace with wall time.

### Video frames straight to the compositor

The MSE renderer used to repaint the video element every frame: a main-thread
repaint, a rendering update and a tile repaint for each picture. When the page
thread is busy, pictures wait. The player now reports accelerated rendering.
`GraphicsLayerCoordinated::setContentsToMediaPlayer()` (until now
GStreamer-only) gives it a `CoordinatedPlatformLayerBufferProxy`, and the
decoder thread hands each SkImage to the compositor as a native-image layer
buffer. `paint()` remains for `drawImage(video)` and similar.

With `pages/mse.html?busy=400`, which blocks the page thread for 400 ms every
second, the native view received 18–26 frames/s with 422–432 ms gaps on the
previous bundle, and about 33 frames/s (the 30 fps clip plus page updates) with
at most 40 ms gaps on `bundle-h3v4tvze`. On YouTube the main web process fell
from about 1.7 to 1.35 cores. Total system load was unchanged at about 3.2
cores, because the texture upload moved to the compositor.

The file engine (`MediaPlayerPrivateHaiku`, used for Reddit, the Guardian and
other plain video) now does the same. It copies each decoded `BBitmap` into
an immutable SkImage and pushes that to the compositor. With
`pages/media.html?src=mse-media/plain.mp4&busy=400`, the longest gap between
delivered frames fell from 411–424 ms to 37–42 ms (`bundle-av2hgf30`). The
Guardian startup stalls measured on 25 September came from exactly these
page-thread waits.

A matched-viewport (1280×887) ten-iteration run of the final
`bundle-av2hgf30` scored 7.684 ± 0.446, but the harness classified it
contended: the owner's own Summit session was playing YouTube, about 2 cores
of foreign load. It is not comparable to the uncontended scores above; repeat
it on an idle machine (`.vm/bench/speedometer-*-final-av2-matched/`).

### Scrolling a loading Reddit feed

With the burst starting about 3 s after navigation (`run-scroll.py --settle
0.5`, 150 notches at 25 ms, both at 120 px per notch), the previous installed
bundle delivered 55.4 frames/s and the MSE PGO bundle 54.6. Each had 4 frames
over 33 ms, with worst gaps of 137 and 223 ms. Both final captures show painted
posts, not blank tiles. One live sample each cannot separate the builds; the
point is that a loading feed already scrolls near the refresh rate
(`.vm/bench/scroll-20260925-233442-loading-old/`,
`.vm/bench/scroll-20260925-233504-loading-new/`). Reddit still plays most feed
videos through the native HLS path; one ad player now uses MSE and decoded
through NVDEC.

## Mouse wheel delivery with real input (September 25, 2026)

The benchmark harness scrolls with `summitctl scroll`, whose synthesized
`B_MOUSE_WHEEL_CHANGED` messages carry a `_view_token`. That token sends each
notch straight to the page view, so those runs never exercised the routing a
real wheel goes through. `tools/bench/vnc-input.py wheel` sends real wheel
button events through the workstation's VNC server (InputEventInjector), and
`pages/wheel-targets.html` shows the positions of a document, an
`overflow: auto` box, an iframe and a box with a non-passive `wheel` listener.
Two faults showed up that the harness could not see.

**A window that opens under a still pointer drops every notch.** Haiku's
`BWindow` sends a wheel message to the view the pointer last moved over and
drops it when there is none. After Summit opened on haiku-os.org with the
pointer left where it was, the first 10-notch burst never reached the web
process (8 routed events from two bursts); moving the pointer by 3 px first
delivered both (16). The same applies to a tab view that replaces another
under the pointer. `BrowserWindow::AnnouncePointer()` now calls
`set_mouse_position()` with the current position on activation and tab
selection; input_server turns that into a genuine `B_MOUSE_MOVED`, and the
stationary-pointer run delivers both bursts (16 events).

**Scroll animations waited for an unrelated scroll.** Haiku has no display
link, so `EventDispatcher` services scrolling-thread animations with its own
refresh timer, and only a wheel event handled on the scrolling thread started
it. A notch over a non-passive listener is dispatched to the main thread
first; the listener box fired four `wheel` events but stayed at
`scrollTop=0` until the document itself was wheel-scrolled, when it jumped to
200. A page with such a listener on `window` or `document` would barely
scroll. `ThreadedScrollingTree` now calls a Haiku hook when any scroll
animation starts, `EventDispatcher` restarts its pulses from it, and the
pulses default to 16 ms (`SUMMIT_SCROLL_REFRESH_TIMER=0` disables them;
the launcher already set 16).

The other scrollers behaved: the overflow box, the iframe and the document
each moved 200 px for five notches, all on the scrolling thread. On a loading
Reddit feed the first notches arrived while the document was only 941 px tall
(16 px of scroll range), so that burst had nothing to move; later bursts
scrolled normally.

The NanoKVM on the workstation presents an absolute USB pointer that Haiku does
not recognise (no `/dev/input/tablet` node), so neither its clicks nor its wheel
reach applications. `tools/bench/nanokvm-input.py` is kept for when that is
fixed.

## Scrolling (September 21, 2026)

Scrolling a busy page was the owner's first complaint, so the drawing area now
reports what it does with every frame. `SUMMIT_FRAME_STATS=<seconds>` prints one
line per period to the WebProcess's stderr:

```
Summit frames: 27.4/s over 1.0 s (painted=28 unchanged=0 dropped=0) update=0.70 ms
  paint=33.8 ms flush=… present=2.1 ms interval=36.7 ms (max 43.8) longest frame=41.7 ms
  dirty=0.14 Mpx/frame
```

`update` is style, layout and script; `paint` is walking the page and sending
drawing commands; `flush` is waiting for app_server to carry them out;
`present` is the UI process copying and showing the frame. It works on any page,
including one that cannot be instrumented from JavaScript.

Two changes came out of it, measured in the VM (1280x800, software painting)
with `tools/bench/pages/scroll.html`, a 400-card feed that scrolls itself from
`requestAnimationFrame` and reports its own frame intervals:

| Configuration | Scrolling | Median frame | p95 |
| --- | --- | --- | --- |
| **everything below, together** | **34.8 fps** | 28 ms | 37 ms |
| without the shadow scratch buffer | 27.4 fps | 36 ms | 43 ms |
| without the presentation bitmap pool (`SUMMIT_PRESENT_POOL=0`) | 26.5 fps | 37 ms | 45 ms |
| without frame pacing (`SUMMIT_FRAME_PACING=ack`) | 18.3 fps | 54 ms | 62 ms |
| without scroll copying (`SUMMIT_SCROLL_COPY=0`) | 15.4 fps | 64 ms | 76 ms |
| **as before this work** (pacing, copying and pool all off) | **12.2 fps** | 81 ms | 91 ms |

That is 2.8x on the same page and machine. An idle page reaches 59.5 fps in
every configuration, so the cost is all in producing changed frames.

1. **Frame pacing.** A frame used to be scheduled 16 ms after the UI process
   acknowledged the previous one, so painting and presentation were added to
   every interval: a page that paints in 10 ms was held near 30 fps however much
   headroom the machine had. A frame is now due one display period after the
   previous frame *started*.
2. **Scroll copying.** The frame bitmap survives between frames, so what a
   scroll keeps on screen is moved inside it (`DrawingAreaHaiku::scroll()` keeps
   the scrolled rectangle and offset, as `DrawingAreaCoordinatedGraphics` does,
   and `copyScrolledPixels()` moves the rows) and only the strip that came into
   view is painted. Painted area per frame fell from 0.90 Mpx to 0.14 Mpx and
   painting from 65 ms to 34 ms.
3. **Presentation bitmap pool.** The UI process used to construct a `BBitmap`
   for every frame, which asks app_server for a shared area and registers it.
   Frames are now recycled through a small pool as views release them, and
   `BWebKitView::Draw()` copies only the invalidated rectangle.
4. **Shadows** (below) stopped allocating an `ImageBuffer` per shadow per
   frame.

Painting is what is left: 34 ms for 0.14 Mpx of this page. Waiting for
app_server to finish (`flush`) is 0.1 ms of that, so the cost is all in the web
process. Turning parts of the page's style off says which part:

| Page style | Painting | Scrolling |
| --- | --- | --- |
| cards with `box-shadow` and gradients (as above) | 34.0 ms | 27.4 fps |
| `style=nogradient` (gradients replaced by flat colour) | 33.7 ms | 27.8 fps |
| `style=noshadow` (no `box-shadow`) | **10.6 ms** | **59.2 fps** |
| `style=flat` (neither) | 11.7 ms | 59.2 fps |

`box-shadow` costs 23 ms a frame — more than twice everything else put
together — and gradients cost nothing. The reason is `ShadowBlur`: upstream
keeps the blurred shadow template in a scratch `ImageBuffer` between draws, but
only `#if USE(CG)`. Every other port allocates an `ImageBuffer` for every shadow
of every frame, and on Haiku an `ImageBuffer` is an app_server offscreen window
with a `BView` and a thread, about 0.84 ms each. The scratch buffer is written
in port-independent terms, so the Haiku port now enables it too, stops redrawing
the template for every differently sized element (only the template goes into
the buffer, and it does not depend on that size), and asks app_server to finish
once per buffer instead of once for each of the nine pieces a tiled shadow is
drawn from (`ImageBufferHaikuSurfaceBackend::syncDrawing()`).

## The workstation, 1913x935 (September 21, 2026)

Same page, same harness, on the GeForce machine (`docs/workstation.md`), with a
viewport 2.2x the area of the VM's:

| Path | Scrolling | Painting | Flush | Present | Dirty |
| --- | --- | --- | --- | --- | --- |
| software | **42.1 fps** | 19.5 ms | 0.05 ms | 3.9 ms | 0.21 Mpx |
| GL compositing (zink on NVK) | 10.0 fps | 105 ms | 0.00 ms | 4.4 ms | whole view |

An idle page reaches 59.6 fps on both.

**GPU compositing works on this hardware and is four times slower.** The
compositor is selected automatically and reports
`zink Vulkan 1.3(NVIDIA GeForce GTX 1070 (NVK GP104-A) (MESA_NVK)),
OpenGL ES 3.1 Mesa 25.3.6`, renders correctly, and reads back a frame in about
4 ms — the readback is not the problem, and an idle page still reaches 59.6 fps
through it. Scrolling is: `DrawingAreaHaiku::scroll()` invalidates the scrolled
area of the non-composited content layer, so TextureMapper repaints every tile
that intersects the viewport, each through the same app_server drawing path,
and only then composites. Nothing about the GPU is used for the part that
costs. The software path keeps its scroll copy and wins.

Making it pay off needs the tiles to live in page coordinates and move when the
page scrolls, as CoordinatedGraphics does, so that a scroll paints only the
newly exposed tiles. Until then software painting stays the default, which it
is in practice: the stock Haiku Mesa cannot initialize EGL, so GL is only
chosen when the private Mesa is on `LIBRARY_PATH`.

## What painting is made of (September 21, 2026)

`SUMMIT_FRAME_STATS` now also tallies what each frame asks the port to draw,
which is how the guesses above were replaced with numbers. A second line goes
with the frame line:

```
Summit painting: per frame drawing calls 14.37 ms of which 11 text runs
  (410 glyphs) 0.07 ms, 83 images 2.05 ms; 32 fills, 0 strokes, 41 clips,
  558 state changes 0.01 ms
```

Measuring it costs about 3.5% (44.9 fps becomes 43.4), and nothing at all when
`SUMMIT_FRAME_STATS` is unset.

On the scroll page, at 1913x935, painting a frame was 20.65 ms and **text was
0.07 ms of it** — drawing glyphs, the thing that looked most suspicious in the
code, is not worth optimising. What the tally did show:

- **Shadow templates were thrashing a one-slot cache.** The cards carry two
  shadows (`0 1px 3px` and `0 8px 20px`), and `ShadowBlur`'s scratch buffer held
  exactly one template, so the two evicted each other and *every* draw redrew
  and reblurred its template, with a `getPixelBuffer`/`putPixelBuffer` round
  trip each time. The scratch buffer now keeps four templates, chosen by what
  they were drawn from and evicted least-recently-used
  (`SUMMIT_SHADOW_CACHE=0` restores an `ImageBuffer` per shadow).
- **Every `PopState()` flushed the link to app_server.** `BView::PopState()`
  calls `_FlushIfNotInTransaction()`, and WebKit brackets nearly everything in
  a state saver: 558 save/restore pairs a frame, so 558 forced `write_port`
  calls and no batching at all. Painting a frame, and drawing into an
  `ImageBuffer`, now runs inside a `BWindow` view transaction, which holds the
  flushes back until the drawing is done (`SUMMIT_BATCH_DRAWING=0` turns it
  off).

| Scroll page, workstation | Painting | Scrolling |
| --- | --- | --- |
| before this round | 20.65 ms | 42.1 fps |
| four shadow template slots | 19.5 ms | 42.7 fps |
| + batched drawing | **17.2 ms** | **44.9 fps** |

What is left of those 17.2 ms: 14.4 ms inside the port's drawing calls and
2.8 ms of WebCore deciding what to draw. Inside the drawing calls, images are
2.1 ms, text is 0.1 ms, and the remaining ~12 ms belongs to 32 fills, 41 clips
and **558 state changes** — about seventeen `PushState()`/`PopState()` pairs for
every drawing operation. Most of those pairs enclose no drawing at all, which
suggested holding the pushes back and applying them only when a call needs the
state. That was tried next, and measured at 0.07 ms; the section below has what
the time is really spent on.

## Two hypotheses, measured and dropped (September 21, 2026)

The paint tally above ended with a prediction: that the 558 state changes a
frame were the next thing worth removing. The tally was extended to time them,
and to count the one cost it had not yet looked at — the calls that *read* state
back out of app_server. Both halves of the prediction turned out to be wrong,
and one of them turned out to be dangerous.

```
Summit painting: per frame drawing calls 14.65 ms of which 11 text runs
  (420 glyphs) 0.07 ms, 83 images 2.04 ms; 32 fills 18.02 ms, 0 strokes,
  41 clips 0.05 ms, 558 state changes 0.01 ms (0.07 ms in save/restore),
  75 app_server round trips 2.09 ms (10 for the clip)
```

- **State save/restore is 0.07 ms a frame.** Once the flushes were held back by
  the view transaction, the 558 `PushState()`/`PopState()` pairs cost almost
  nothing: they are a few bytes each into an already-batched `PortLink`. A lazy
  scheme that kept the pushes pending and applied them only when a drawing call
  needed the state was written, measured at 0.07 ms, and then **reverted** — see
  below.
- **Text is still 0.07 ms.** Confirmed a second time, with glyph counts.
- **Reading state back is 2.09 ms a frame.** This is the one real finding. A
  `BView` getter like `HighColor()`, `PenSize()`, `DrawingMode()`, `FillRule()`
  or `Transform()` is not a local field read: it is `FlushWithReply()`, a
  synchronous round trip to app_server. Each one measured about 28 µs, and a
  frame was making 75 of them. They also defeat the batching, because every one
  flushes the link.

Six of those call sites were reading back a value the port already knows, to
save and restore it around a drawing call, and were replaced with the value from
`GraphicsContext`'s own state: `drawLine`, `strokeRect` and `drawLinesForText`
now restore the pen with `strokeThickness()`, `fillRoundedRectImpl` and
`fillPath` compute the colour with `withGlobalAlpha(...)` and the drawing mode
with a new `drawingModeForCompositeHaiku()`, and `clipPath` restores the fill
rule from `fillRule()`. Rendering is unchanged: the scroll page and Wikipedia
both came back **0 pixels different**.

| Workstation | Scrolling | Speedometer 3.1 |
| --- | --- | --- |
| after the shadow and batching round | 44.94 fps | 3.23 ± 0.083 |
| + six read-backs removed | **46.33 fps** | 3.28 ± 0.058 |

Scrolling moved; Speedometer did not. The two confidence intervals overlap, so
the honest reading is that removing 0.2 ms of round trips per frame is not
measurable on a benchmark whose frames are 100–120 ms long. Both runs were on a
quiet machine (0.05 foreign cores, `vncserver` stopped).

That is 3%, which is what a 2.09 ms cost is worth against a 21 ms frame — and
most of it is still there. Of the 75 round trips, 65 are `Transform()` inside
`getCTM()` and `concatCTM()` and 10 are `GetClippingRegion()` inside
`clipBounds()`. `GetClippingRegion()` genuinely has to ask the server, because
app_server owns the clip; `Transform()` does not, and mirroring the CTM in the
port would remove the remaining 1.8 ms. It is left for the drawing-model work
rather than done here, because a mirrored CTM has to stay correct across
`PushState`/`PopState`, layers and `ImageBuffer` switches, and the payoff is
under two milliseconds.

**The lazy save/restore experiment was reverted.** Making `PushState()` lazy
rendered Wikipedia nearly blank, with truncated text; `SUMMIT_LAZY_STATE=0`
produced a pixel-identical image to the previous bundle, which both isolated the
cause and cleared the read-back removals. The likely reason is that
`beginTransparencyLayer()` needs its `PushState()` to have reached the view
before `BeginLayer()`, and there are probably other such orderings. Since the
whole mechanism was worth 0.07 ms, it was removed rather than fixed.

### What this round actually says

Per frame, painting is 14.6 ms inside the port's drawing calls. Text is 0.07 ms,
state changes are 0.07 ms, clips are 0.05 ms, images are 2.0 ms and round trips
are 2.1 ms. Everything else — about 10 ms — is inside 32 fill calls, spread
across gradients, rounded rectangles, shadows and image scaling, with no single
call worth removing. Three successive micro-optimisation hypotheses (glyphs,
state changes, read-backs) have now been tested; the first two were worth
nothing and the third was worth 3%. There is no fourth one that is worth more.

The remaining structural facts are unchanged and much larger: painting is
~95 ms per megapixel inside the web process, one thread does all of it, and 31
cores are idle while it does. The next step is the drawing model itself —
painting in parallel — not another pass over the drawing calls.

## Speedometer 3.1 against Firefox (September 21, 2026)

Workstation, local copy of the benchmark, 10 iterations, full-screen window:

| Browser | Score |
| --- | --- |
| Firefox 155.0 | **8.34 ± 0.37** |
| Summit, after removing the app_server read-backs | 3.28 ± 0.058 |
| Summit, after the shadow and batching work above | 3.23 ± 0.083 |
| Summit, before it | 3.05 ± 0.085 |

Firefox is 2.6x faster. A run is only comparable on a quiet machine: with
`vncserver` polling the frame buffer (about one core) the same build scored
1.94 ± 0.40, and `run.json` records the load so a contended run can be told
apart from a real change. The frame statistics say where Summit's time goes: per
frame about 40 ms of style, layout and script, about 40 ms of painting for only
0.3–0.5 Mpx of dirty area, and 9–30 ms of presentation, for a frame every
100–120 ms. Waiting for app_server is 0.04 ms, so painting is 95 ms per
megapixel *inside the web process*: the cost is building and issuing drawing
commands, not rasterising them. That, and the fact that all of it happens on one
thread while 31 others idle, is the next thing to attack.

Measured 2026-09-19 in Summit's Haiku test VM (docs/VM.md). Everything here can be
re-run with the scripts in `tools/bench/`; raw artifacts are under `.vm/bench/<run-id>/`.

## Current results (September 19, afternoon)

Uncontended runs (no compiler active in the VM; recorded per run in `run.json`),
local copy of Speedometer 3.1 unless noted, 10 iterations unless noted.

| Bundle | Changes | Score |
|---|---|---|
| bundle-la4ytsek | earlier engine, default settings | 0.575 ± 0.015 |
| bundle-la4ytsek | same with `JSC_useConcurrentJIT=0` | 2.30 ± 0.08 |
| bundle-ggdk304g | in-process timers, dirty-region painting, frame bitmap reuse; app set `JSC_useConcurrentJIT=0` | 2.42 ± 0.08 |
| bundle-ggdk304g | same, `JSC_useConcurrentJIT=1` (5 iterations) | **2.70 ± 0.11** |
| bundle-ggdk304g | same, `SUMMIT_FULL_REPAINT=1` (5 iterations) | 2.34 ± 0.08 |
| bundle-ggdk304g | **https://browserbench.org/Speedometer3.1/** (read from screenshot) | **2.35 ± 0.085** |
| bundle-fo_egzax | + frame drawing context kept, fill/text fixes (5 iterations) | 3.07 ± 0.28 |
| bundle-gewxjzbi | + **mimalloc** instead of Haiku `malloc`, software painting (5 iterations) | **3.53 ± 0.21** |
| bundle-gewxjzbi | same, GL compositing forced on llvmpipe (5 iterations) | 3.46 ± 0.31 |
| bundle-gewxjzbi | **https://browserbench.org/Speedometer3.1/**, default settings | **3.67 ± 0.099** |

Overall, from the start of the session: 0.575 → 3.53 locally (6.1x), and the
official page went from crashing to 3.67. mimalloc alone is +15%. GL
compositing on llvmpipe — CPU rasterisation — is level with software painting,
so it costs nothing to ship; a real GPU is expected to help the half of each
step that is rendering, but that is unmeasured.

What this shows:

1. The official site now runs to completion. It previously killed its page
   process through the Screen Wake Lock API, which is now reported as absent.
2. After timers stopped going through `BMessageRunner`, the concurrent-JIT
   "slow mode" described below disappeared: concurrent JIT is now 11% *faster*,
   so the app no longer disables it. Best configuration: 2.70, about 4.7x the
   la4ytsek default.
3. Painting only invalidated regions is worth about 3%. Half of each step is
   still "async" time (rendering update, paint, frame hand-off), so the frame
   path remains the largest target. The following bundle also keeps the frame's
   app_server drawing context alive (0.84 ms to 0.09 ms per frame in isolation)
   and is not measured yet.
4. The next full rebuild switches from Haiku's `malloc` to WebKit's bundled
   mimalloc (7.8x on a threaded allocation stress test) and adds optional GL
   compositing; see [gpu-compositing.md](gpu-compositing.md).

Reference: headless Chromium 151 on the VM's host CPU scores 28.4 ± 1.3 on the
same local copy (host, not the VM, so an upper bound for comparison only).

The analysis below was written earlier in the day, while builds were running,
and describes bundles j6jimkn8 and la4ytsek.

## Earlier results (contended)

**Every number in this section was taken while other engineers were compiling in
the same VM ("contended").** Treat absolute scores as lower bounds and trust only
the large effects (2x and more).

### Headline results (earlier)

| What | Bundle | Result |
|---|---|---|
| Speedometer 3.1, local copy, 10 iterations, default settings | bundle-la4ytsek | **0.537 ± 0.014** (contended) |
| Same, 10 iterations | bundle-j6jimkn8 | **2.27 ± 0.42** (contended; iteration 1 = 0.63, iterations 2-10 = 2.24-2.56) |
| Same with `JSC_useConcurrentJIT=0`, 3 iterations | bundle-la4ytsek | **2.22 ± 0.05** (contended) - 4.1x the default |
| Same with `JSC_useConcurrentJIT=0`, 10 iterations | bundle-j6jimkn8 | 1.96 ± 0.34 (contended; iteration 1 = 1.90 instead of 0.63) |
| Reference: headless Chromium 151 on the VM's host CPU, same local copy | - | 28.4 ± 1.3 |
| https://browserbench.org/Speedometer3.1/ | bundle-la4ytsek | **cannot run**: the page process exits when the benchmark requests a screen wake lock |
| Stress test, 10 min, 157 tab/navigation actions | bundle-la4ytsek | no crash, no hang, no load timeout |

The three findings that matter most:

1. **Concurrent JIT compilation puts the WebProcess into a slow mode** whenever a page's
   scripts are freshly loaded. One environment variable (`JSC_useConcurrentJIT=0`)
   removes it: 4.1x on the Speedometer score of the current bundle, 13x on
   TodoMVC-jQuery. The mechanism is not identified (no profiler could be used).
2. bundle-la4ytsek is **4.6x slower than bundle-j6jimkn8 from the second iteration on**
   in default settings, although both contain the byte-identical `libJavaScriptCore.so`.
   la4ytsek behaves in every iteration the way j6jimkn8 behaves only in its first
   (cold) iteration, and the way j6jimkn8 behaves in *all* iterations when the server
   forbids caching. See [The slow mode](#1-the-slow-mode-concurrent-jit--freshly-loaded-scripts).
3. Once the slow mode is out of the way, **60-95% of most suites is "async" time**
   (rendering update, paint, frame hand-off, zero-delay timer), about 20x the reference,
   and it is a fixed per-frame cost: it does not shrink with the viewport.

## Harness

| File | Purpose |
|---|---|
| `tools/bench/serve-speedometer.py` | Threaded static server (127.0.0.1) for the pristine checkout in `.cache/speedometer-3.1`; injects the result collector into the served `index.html`; receives results |
| `tools/bench/run-speedometer.py` | One benchmark run: load check, server, browser launch with a fresh profile, window sizing, contamination/crash/hang detection, screenshots, JSON, shutdown of its own instance, summary table. `--official` for browserbench.org |
| `tools/bench/run-probe.py` + `pages/probe.html` | Port-level diagnostic page: JIT tier check, allocation/DOM/layout kernels, timer and task latency, rAF cadence, the cost of Speedometer's "async window" |
| `tools/bench/stress-browser.py` | Seeded random tab/navigation stress with memory/thread sampling and crash/hang detection |
| `tools/bench/compare-runs.py` | Per-suite ratio table (sync/async split) between two results |
| `tools/bench/summitctl.cpp` | Native guest helper (built on demand in the guest): scripts **one Summit team by id** (`state`, `navigate`, `newtab`, `closetab`, `selecttab`, `frame`, `sidebar`, `quit`), lists a process group with full image paths, samples system and per-team CPU/threads/area memory |
| `tools/bench/guest.py` | Shared plumbing: ssh, launch in an own session/process group, single-launch guard, contention report, crash watch, display wake |
| `tools/bench/pages/wakelock.html` | Minimal reproduction of the page-process exit that blocks the official site |

Source of the benchmark: `git clone --depth 1 --branch release/3.1 https://github.com/WebKit/Speedometer .cache/speedometer-3.1`
(commit `1386415be8fef2f6b6bbdbe1828872471c5d802a`, 2025-03-11; the repository has a
`release/3.1` branch and no `v3.1` tag).

```sh
python3 tools/bench/run-speedometer.py                         # 10 iterations, default bundle (la4ytsek)
python3 tools/bench/run-speedometer.py --iterations 3          # quick check
python3 tools/bench/run-speedometer.py --wait-quiet 120        # poll up to 2 h for an idle VM first
python3 tools/bench/run-speedometer.py --bundle /SummitExtensions/summit/build-modern-browser/bundle-j6jimkn8
python3 tools/bench/run-speedometer.py --env JSC_useConcurrentJIT=0
python3 tools/bench/run-speedometer.py --official              # browserbench.org; score is read from final.png
python3 tools/bench/run-speedometer.py --annotate .vm/bench/<run-id> --score 4.2 --ci 0.1
python3 tools/bench/run-probe.py [--env NAME=VALUE]
python3 tools/bench/stress-browser.py --duration 600 [--seed N] [--local-only] [--max-tabs N]
python3 tools/bench/compare-runs.py .vm/bench/<run-A> .vm/bench/<run-B>
```

How results are collected without touching measured code: the server adds one inline
`<script type="module">` before `</head>` of the top-level `index.html` only. Module
scripts run in document order, so it runs after `resources/main.mjs` has created
`globalThis.benchmarkClient` and before `DOMContentLoaded` starts the run. It wraps the
client callbacks `willStartFirstIteration`, `didFinishLastIteration` and `handleError`
on that instance; nothing is installed inside the timed sync/async windows, no timers,
no observers. After the last iteration it POSTs the score, confidence interval, every
per-suite/per-test/per-iteration metric, `params` and the page environment to
`/__bench/result`. `--progress-beacons` (off by default) additionally reports each test
start and marks the result `instrumented`.

Details that turned out to matter:

- The server mirrors browserbench.org's response headers (ETag + Last-Modified, no
  Cache-Control, COOP same-origin, COEP require-corp). `Last-Modified` is the release
  commit date, not the clone's mtime, otherwise heuristic freshness would be seconds.
  `--cache-policy revalidate|no-store` exist for experiments.
- `http://10.0.2.2` is not a secure context, so COOP/COEP are ignored there:
  `crossOriginIsolated` is false and `performance.now()` has 1 ms resolution in the local
  runs. The same is the reason the local copy does not hit the wake-lock bug below.
- Do not send results with `fetch(..., {keepalive: true})`: keepalive bodies are capped
  at 64 KiB and a 10-iteration result is well over that (`result.json` is ~180 KiB
  indented); this lost the JSON of one run.
- The browser starts on `summit:home`, the window is set to `4,1,1270,797` and the sidebar
  is closed, then the URL is loaded: viewport 1267x666 (Speedometer asks for >= 850x650;
  the default window only gives 915x550).
- The guest screen saver blanks the display; the runner moves the pointer at the screen
  edge (no click, no key, outside the browser window) once a minute.
- **Summit is `B_SINGLE_LAUNCH` per executable file.** Launching `<bundle>/Summit` while
  that same file is already running only forwards the URL into the running instance as a
  new tab, whatever `--profile` says. Two of my runs were destroyed this way by a
  colleague's launch. The tools now refuse to start if `<bundle>/Summit` is already
  running, check once a minute that the window still has exactly the one expected tab,
  and mark a run `contaminated` otherwise. Instances from *other* bundle directories
  coexist and only appear as foreign CPU load.
- `ps` and `team_info.args` cut command lines at 64 bytes, which is shorter than the
  bundle path; roles are taken from the application image path instead.
- Contention: `summitctl sample` reads per-CPU active time and per-team CPU time over an
  interval. A run is labelled contended if a compiler/build tool is seen, other teams use
  more than 0.25 cores, or more than 0.5 cores of busy time cannot be attributed (short
  compiles that start and exit inside the interval).

## Test environment

- VM: QEMU/KVM, 12 vCPUs, 20 GiB RAM, host is a 32-core AMD Ryzen AI MAX+ 395;
  Haiku R1/beta6 hrev59866+79 x86_64; 1280x800 framebuffer, app_server software
  rendering, no GPU. `navigator.hardwareConcurrency` reports 8.
- Bundles (guest): `/SummitExtensions/summit/build-modern-browser/bundle-j6jimkn8`
  (built 2026-09-18 23:32 UTC, engine patch sha256 `2ff87cb1...`) and `bundle-la4ytsek`
  (2026-09-19 00:16 UTC, patch `87f133e1...`: opacity-layer paint fix, BFS disk-cache fix,
  extension APIs). WebKit upstream `00991b6cdc59`. `libJavaScriptCore.so.18.7.4` is
  byte-identical in both (sha256 `53596aab293e3a79...`, built 2026-09-14).
- Engine build (`/SummitExtensions/WebKit/WebKitBuild/Modern`): `CMAKE_BUILD_TYPE=Release`,
  `-O3 -DNDEBUG` (plus `-ftrack-macro-expansion=0 --param ggc-min-expand=10`), GCC 13.3,
  no LTO. `ENABLE_JIT=1`, `ENABLE_DFG_JIT=1`, `ENABLE_FTL_JIT=1`, `ENABLE_C_LOOP=0`,
  `ENABLE_WEBASSEMBLY=1` (BBQ and OMG on), `ENABLE_SAMPLING_PROFILER=0`,
  **`USE_SYSTEM_MALLOC=1`** (`Source/cmake/OptionsHaiku.cmake:90`; bmalloc/libpas off, so
  all of WTF/WebCore/DOM allocation goes to libroot's OpenBSD-derived malloc),
  `USE_ISO_MALLOC=1`, `USE_SKIA=0`, `USE_TEXTURE_MAPPER=0`, `USE_COORDINATED_GRAPHICS=0`,
  `ENABLE_GPU_PROCESS=0`, `ENABLE_ASYNC_SCROLLING=0`, `ENABLE_WEBGL=0`, `ENABLE_RELEASE_LOG=0`.
- The JIT tiers work at run time: the probe's integer kernel (20M iterations) takes 86-93 ms
  per round with the JIT and 620-660 ms with `JSC_useJIT=0`; headless Chromium on the host
  needs 140-160 ms for the same loop.

## Results

All local-copy runs: `?startAutomatically=true`, `measurementMethod=raf`, viewport 1267x666.
"Foreign cores" is CPU used by other people's processes in the VM during the run (median/max
of the 60 s samples).

| Run id (`.vm/bench/`) | Bundle | Settings | Iter. | Score ± 95% CI | Wall | Foreign cores | Label |
|---|---|---|---|---|---|---|---|
| speedometer-20260919-102837-la4ytsek-run1 | la4ytsek | default | 10 | **0.537 ± 0.014** | 602 s | 3.4 / 7.2 | contended |
| speedometer-20260919-105928-la4ytsek-noconcjit | la4ytsek | `JSC_useConcurrentJIT=0` | 3 | **2.218 ± 0.047** | 63 s | 2.5 / 2.5 | contended |
| speedometer-20260919-110826-j6jimkn8-run2 | j6jimkn8 | default | 10 | **2.274 ± 0.418** | 268 s | 3.3 / 5.5 | contended |
| speedometer-20260919-111325-j6jimkn8-noconcjit | j6jimkn8 | `JSC_useConcurrentJIT=0` | 10 | 1.964 ± 0.341 | 246 s | 4.3 / 7.4 | contended |
| speedometer-20260919-111902-j6jimkn8-nostore | j6jimkn8 | server sends `Cache-Control: no-store` | 3 | 0.538 ± 0.106 | 190 s | 2.8 / 5.9 | contended, diagnostic |
| speedometer-20260919-100209-baseline-a | j6jimkn8 | default | 10 | 2.03 ± 0.42 (from screenshot) | 357 s | 6.1 / 7.0 | contended, JSON lost (keepalive limit) |
| speedometer-20260919-095043-smoke2, -095305-smoke3 | j6jimkn8 | default | 1 | 0.570, 0.556 | 65 s | 5.1 | contended, harness bring-up |
| speedometer-20260919-110123-official-la4ytsek, -110408-...-retry | la4ytsek | browserbench.org | - | none | - | - | page process exited (see below) |
| reference-host-chromium-headless-* | Chromium 151 headless on the host | default | 10 | 28.37 ± 1.33 | ~2 min | - | host busy with the VM's builds |
| DISCARDED-contaminated-* (2), speedometer-20260919-101601-profiled | | | | | | | discarded: foreign tab injected / guest kernel panic |

Per-iteration scores: la4ytsek default `0.54 0.50 0.53 0.55 0.53 0.53 0.55 0.57 0.53 0.55`;
j6jimkn8 default `0.63 2.46 2.40 2.54 2.24 2.56 2.54 2.40 2.42 2.56`;
j6jimkn8 `JSC_useConcurrentJIT=0` `1.90 2.27 2.07 1.54 0.79 2.31 2.06 2.27 2.35 2.07` (the 0.79
coincides with 7 foreign cores); j6jimkn8 no-store `0.55 0.49 0.57`.

Per-suite means in ms (± 95% CI half-width where it fits). "sync" is time inside the
benchmark's rAF callback (script plus forced style/layout), "async" is from there to a
zero-delay timer after the frame (rendering update, paint, frame hand-off, timer).

| Suite | la4ytsek default | sync / async | la4ytsek no-concurrent-JIT | j6jimkn8 iterations 2-10 (median) | sync / async | Host Chromium | j6 steady / Chromium |
|---|---|---|---|---|---|---|---|
| TodoMVC-JavaScript-ES5 | 1251 ± 122 | 1088 / 163 | 265 | 263 | 87 / 170 | 33.8 | 7.8 |
| TodoMVC-JavaScript-ES6-Webpack-Complex-DOM | 1098 ± 89 | 850 / 248 | 348 | 331 | 76 / 254 | 33.0 | 10.0 |
| TodoMVC-WebComponents | 758 ± 50 | 539 / 219 | 240 | 236 | 35 / 199 | 20.3 | 11.6 |
| TodoMVC-React-Complex-DOM | 2634 ± 487 | 1719 / 915 | 912 | 378 | 125 / 243 | 32.7 | 11.6 |
| TodoMVC-React-Redux | 2075 ± 359 | 1423 / 652 | 426 | 292 | 136 / 155 | 31.0 | 9.4 |
| TodoMVC-Backbone | 1540 ± 116 | 1019 / 520 | 282 | 272 | 85 / 180 | 25.3 | 10.7 |
| TodoMVC-Angular-Complex-DOM | 3075 ± 530 | 1628 / 1446 | 641 | 404 | 170 / 233 | 30.8 | 13.1 |
| TodoMVC-Vue | 1417 ± 174 | 250 / 1167 | 274 | 261 | 25 / 237 | 24.5 | 10.7 |
| TodoMVC-jQuery | 7188 ± 821 | 7032 / 156 | 557 | 492 | 308 / 156 | 89.6 | 5.5 |
| TodoMVC-Preact-Complex-DOM | 980 ± 123 | 181 / 799 | 264 | 260 | 14 / 245 | 14.5 | 17.9 |
| TodoMVC-Svelte-Complex-DOM | 674 ± 67 | 170 / 505 | 274 | 260 | 13 / 242 | 12.9 | 20.1 |
| TodoMVC-Lit-Complex-DOM | 912 ± 65 | 191 / 721 | 307 | 298 | 15 / 285 | 16.8 | 17.7 |
| NewsSite-Next | 3570 ± 233 | 2290 / 1280 | 701 | 492 | 119 / 365 | 80.9 | 6.1 |
| NewsSite-Nuxt | 3007 ± 378 | 302 / 2705 | 364 | 381 | 73 / 268 | 63.5 | 6.0 |
| Editor-CodeMirror | 870 ± 59 | 562 / 308 | 279 | 248 | 96 / 151 | 25.9 | 9.6 |
| Editor-TipTap | 3144 ± 143 | 3048 / 96 | 2037 | **1988** | **1888** / 96 | 58.0 | **34.3** |
| Charts-observable-plot | 2390 ± 209 | 1971 / 418 | 559 | 498 | 95 / 408 | 44.3 | 11.2 |
| Charts-chartjs | 2715 ± 419 | 2016 / 699 | 515 | 410 | 205 / 200 | 73.4 | 5.6 |
| React-Stockcharts-SVG | 6330 ± 391 | 4690 / 1640 | 1178 | 997 | 212 / **768** | 77.9 | 12.8 |
| Perf-Dashboard | 1598 ± 185 | 1412 / 186 | 579 | 557 | 418 / 142 | 53.6 | 10.4 |
| **Geomean / score** | 1864 ms / 0.537 | 69% / 31% of total | 451 ms / 2.22 | ~400 ms / ~2.5 | | 35 ms / 28.4 | geomean 10.9 |

`python3 tools/bench/compare-runs.py <A> <B>` regenerates such tables, including the sync and
async ratios.

### Official site: the page process exits

`run-speedometer.py --official` never gets past the first second of the run on
bundle-la4ytsek (twice, reproducible): the tab's title falls back to the URL, the status
bar shows "The page process exited. Reload to try again.", `loadOutcome` is
`process-exited`. There is **no crash report** on the guest Desktop, **no debug_server
line** in `/var/log/syslog`, and `browser.log` is empty, so the process was terminated or
left deliberately rather than crashing (e.g. a failed IPC `MESSAGE_CHECK` in the UI
process, or the helper's own exit on connection loss).

Narrowing down (own instance, 10 s per page): the Speedometer home page, `about.html` and a
suite page opened directly all load; starting the benchmark with a single suite kills the
page. The first thing `BenchmarkRunner._runAllSuites()` does is
`navigator.wakeLock.request("screen")` (resources/benchmark-runner.mjs:324-334, 423),
which exists only in secure contexts. `tools/bench/pages/wakelock.html` opened as a
`file:` URL reproduces it without the benchmark: title goes `secure=true api=true` ->
`requesting` -> page process exited within ~2.5 s.

Where to look: `WebPageProxy::queryPermission` (Source/WebKit/UIProcess/WebPageProxy.cpp:15324,
"screen-wake-lock" at :15376, the default `API::UIClient::queryPermission` answers
`std::nullopt`, APIUIClient.h:242) and `WebPageProxy::didCreateSleepDisabler`
(WebPageProxy.cpp:19418, has a `MESSAGE_CHECK_BASE`); there is no Haiku `PAL::SleepDisabler`
(`Source/WebCore/PAL/pal/system/haiku/` only has `SoundHaiku.cpp`). I did not identify the
exact failing check. Any site that asks for a screen wake lock (video players, recipe
sites, benchmarks) will lose its tab the same way. Also note that `loadOutcome` reads
`process-exited` after a harmless COOP process swap too; only the status text tells the
two apart.

### Rendering correctness in the screenshots

Speedometer's own UI (suite name, progress counter, score, gauge) and the suites' content
painted with text in every screenshot I kept for both bundles, so the paint times above
are not flattered by missing glyphs on these pages. The lead reported that
`https://example.com/` rendered without any text glyphs in bundle-j6jimkn8 (only the link
underline); I did not capture that page in j6jimkn8. If pages inside CSS opacity layers
lost their text in j6jimkn8 (fixed in la4ytsek), j6jimkn8's async times for such content
are optimistic.

## Where the time goes

No sampling profiler could be used (see [Platform findings](#platform-findings)); the
analysis below uses Speedometer's sync/async split, the probe page, JSC/malloc
environment variables, a same-host reference browser and source reading.

### 1. The slow mode: concurrent JIT + freshly loaded scripts

Probe kernels on bundle-la4ytsek, one run each, all contended (2-8 foreign cores), so
read only the large differences. Times in ms.

| Kernel (tools/bench/pages/probe.html) | default | default #2 | `JSC_useConcurrentJIT=0` | `JSC_useJIT=0` | `JSC_useDFGJIT=0` | `JSC_useFTLJIT=0` | `JSC_useConcurrentGC=0` | `MALLOC_OPTIONS=jj` / `++` | host Chromium |
|---|---|---|---|---|---|---|---|---|---|
| integer loop, 20M iterations, steady state | 92 | 86 | 86 | 623 | 401 | 91 | 91 | 90 / 89 | 140 |
| 2M objects + closures + array methods | 1507 | **2658** | **321** | 744 | 487 | 679 | 2555 | 1536 / 1356 | 61 |
| 4M plain objects (GC heap only) | - | 89 | 118 | 238 | 149 | 99 | 90 | 87 / 93 | 28 |
| 2M `String(i)` (malloc-backed strings) | - | 298 | 134 | 161 | 226 | 192 | 549 | 565 / 540 | 30 |
| 200k detached DOM nodes | - | 524 | **149** | 506 | 197 | 228 | 677 | 616 / 585 | 49 |
| 20 x 2000 spans created + laid out | 970 | 862 | 776 | 1069 | 864 | 920 | 993 | 939 / 892 | 175 |
| layout only, 50 x 2000 spans | - | 453 | 493 | 492 | 465 | 458 | 486 | 540 / 497 | 41 |

- With the default settings, allocation- and call-heavy script is **slower with the JIT than
  with the interpreter** (2658 vs 744 ms) and 5-8x slower than with `JSC_useConcurrentJIT=0`.
  The optimising tiers themselves are fine (integer loop).
- Speedometer shows the same thing at full scale (table above): la4ytsek 0.537 -> 2.22.
- It is tied to **fresh script loads**. j6jimkn8 is slow only in iteration 1 (0.63) and then
  runs at ~2.5; with `JSC_useConcurrentJIT=0` its iteration 1 is 1.90. When my server sends
  `Cache-Control: no-store`, j6jimkn8 is slow in every iteration (0.55/0.49/0.57) - exactly
  la4ytsek's profile.
- la4ytsek is slow in **every** iteration although its requests.log shows every resource
  fetched once only (so iterations 2-10 are served from a cache) and although JavaScriptCore
  is byte-identical to j6jimkn8's. In j6jimkn8 the disk cache could not store large bodies
  (hard-link failure, below) and the repeat iterations came from WebCore's memory cache;
  la4ytsek has the disk cache repaired. My inference, **not verified**: in la4ytsek the
  repeat loads are now re-delivered by the NetworkProcess from the disk cache, which counts
  as a fresh script load and re-enters the slow mode each iteration. Whatever the path, a
  4.6x regression from iteration 2 on between the two bundles is real and reproducible.
- Not the cause, by experiment: junk-filling in libroot malloc (`MALLOC_OPTIONS=jj`), number
  of malloc pools (`++`), number of DFG compiler threads (`JSC_numberOfDFGCompilerThreads=1`),
  concurrent GC (`JSC_useConcurrentGC=0`).
- Candidates I could not separate without a profiler: (a) the compiler threads' malloc/mmap/
  munmap churn serialising the main thread on the per-team address-space lock or the malloc
  pool locks (the system allocator is in use, `OptionsHaiku.cmake:90`); (b) `WTF::Lock`'s
  `sched_yield` spinning against a compiler thread that was preempted by the builds in the VM;
  (c) the main thread staying in a low tier because concurrent plans are installed late.
  Re-running `run-probe.py` with and without the variable on a mimalloc build separates (a)
  from the rest.

### 2. Shape against a healthy engine on the same CPU

Reference: Chromium 151 headless shell on the VM's host, same local copy, 28.4 ± 1.3. It is
Blink/V8, not WebKit, and the host is not a VM, so only factor-of-several differences mean
anything. I did not use published Safari/WebKitGTK sub-scores: I could not fetch a citable
per-suite table, and a same-hardware run is the stricter comparison. (The host also has
WebKitGTK's MiniBrowser; running it would give a same-engine reference but needs a window on
the owner's desktop session, so I left it.)

- la4ytsek default is 52x the reference in geomean (30x-100x per suite), sync 61x, async 47x:
  a uniform, systemic slowdown - that is the slow mode.
- In the fast mode (j6jimkn8 iterations 2-10, or `JSC_useConcurrentJIT=0`) the gap is 10.9x
  in geomean and the shape becomes informative:
  - **async is 150-290 ms in every TodoMVC suite, against 6-19 ms in the reference**, and is
    60-95% of the suite for Vue, Preact, Svelte, Lit, WebComponents, Nuxt, observable-plot.
    That is ~80 ms per benchmark step, close to the probe's 70-75 ms for "replace 100 small
    nodes and paint".
  - **Editor-TipTap is the sync outlier: 1888 ms sync, 34x the reference**, unchanged by the
    JIT setting (2037 vs 1988 ms). TipTap/ProseMirror work on a contenteditable with
    selection and DOM-range geometry, so look at editor-state updates and at text
    measurement: `ComplexTextControllerHaiku.cpp:119` and `SimpleFontDataHaiku.cpp:128` call
    `BFont::GetEscapements`, which is a synchronous app_server round trip per call.
    Unmeasured; source reading only.
  - React-Stockcharts-SVG async 768 ms (SVG path painting), Perf-Dashboard sync 418 ms
    (canvas 2D drawing inside the callback), Charts-observable-plot async 408 ms: all
    painting through the app_server-backed graphics context.
  - TodoMVC-jQuery and the NewsSite/Chart.js suites are the *closest* to the reference
    (5-6x): plain DOM + script throughput is the least of the problems once the slow mode
    is gone.

### 3. Run loop, timers, rAF, frame path

Measured by the probe (la4ytsek and j6jimkn8 agree):

| Quantity | Summit | Host Chromium |
|---|---|---|
| nested `setTimeout(0)` interval (spec minimum 4 ms) | p50 8 ms, mean 9.4-9.7 ms, max 98-212 ms | 4.07 ms |
| MessageChannel task round trip | <= 1 ms (clock resolution is 1 ms) | 0.01 ms |
| rAF interval, nothing changing | 37-42 ms (24-27 fps) | 16.67 ms |
| Speedometer async window, nothing changed | 17.6-23 ms | 0.11 ms |
| same, one text node changed | 18-20 ms | 0.37 ms |
| same, 100 small nodes replaced | 70-76 ms | 1.09 ms |
| async window at 641x389 vs 1267x666 viewport (3.4x the pixels) | p50 23 ms vs 23 ms | - |

The last row is the important one: **the per-frame cost is fixed overhead, not pixel
work.** Source (bundle-era code; the host copy of `DrawingAreaHaiku.cpp` was being
rewritten during this session, line numbers are the host copy at 2026-09-19 11:30):

- Timers. In the bundles measured, every `RunLoop::TimerBase::start()` created a
  `BMessageRunner`, i.e. a synchronous request to the registrar, and deleted it again on
  fire; `MainThreadSharedTimer::setFireInterval()` did the same plus a second synchronous
  `SetInterval()` and leaked the `new BMessage('shrt')` on every arm. That is the extra
  ~4 ms per `setTimeout(0)`. The registrar's old 50 ms floor is not the issue
  (`kMinimalTimeInterval` is 1 us since 2020). The lead has since replaced both with an
  in-process scheduler thread (host copy `RunLoopHaiku.cpp:96-114`); not in either bundle.
- Frame pacing. `DrawingAreaHaiku::scheduleDisplay()` arms a fixed `16_ms` one-shot
  (`DrawingAreaHaiku.cpp:226`) only after the UI process has acknowledged the previous
  frame, so the period is 16 ms + update + paint + presentation round trip = the measured
  ~40 ms. This costs wall-clock time and smoothness, not Speedometer score (waiting for
  the frame is outside the timed windows).
- Painting. Bundle code: `display()` allocated a new full-viewport `ShareableBitmap` per
  frame and painted `page->bounds()` whatever was dirty; `setNeedsDisplayInRect()` dropped
  its rectangle. The host copy now keeps `m_frameBitmap`, unites a dirty region and skips
  unchanged frames (`DrawingAreaHaiku.cpp:259-325`) - not in either bundle, not measured.
  Still per frame in the host copy: `bitmap->createGraphicsContext()` (`:311`) builds a
  `BBitmap` with `B_BITMAP_ACCEPTS_VIEWS` over the shared area plus a `BView`
  (`ShareableBitmapHaiku.cpp:91-96`), i.e. an offscreen window with its own app_server
  thread, ports and a cloned area, and tears it down with a `Sync()` (`:70`); then
  `createReadOnlyHandle()` + `UpdateBitmapHaiku` (`:327-329`).
- UI process, per frame: `DrawingAreaProxyHaiku::updateBitmapHaiku()` maps the handle
  (`DrawingAreaProxyHaiku.cpp:58`); `BitmapPresenterHaiku::publish()` allocates a new
  `BBitmap` and `memcpy`s the whole frame (`BitmapPresenterHaiku.h:39,43`);
  `viewFrameHaiku` -> `Invalidate()` of the whole view (`WebKitView.cpp:384-385`);
  `BWebKitView::Draw()` fills the update rect white, draws the whole bitmap and calls
  `Sync()` (`WebKitView.cpp:287-300`). None of this is WebProcess CPU, but the WebProcess
  cannot start the next frame until it is acknowledged.
- IPC (`ConnectionHaiku.cpp`, `SocketMonitorHaiku.cpp`): SOCK_SEQPACKET with one poll thread
  per connection that hands each readable event to the work queue and waits on a pipe for
  the callback; up to 64 packets per wakeup. No sleeps or busy yields in the send path; the
  only delay is a `dispatchAfter(1_ms)` retry when the socket buffer is full
  (`ConnectionHaiku.cpp:188`). Not a Speedometer factor.
- During a run the WebProcess uses 0.94 cores and app_server 0.06 (5 s sample), so the
  benchmark is bound by one WebProcess thread, not by app_server or the registrar.

### 4. "Failed to create hard link ... NetworkCache"

`BlobStorage::add()` (Source/WebKit/NetworkProcess/cache/NetworkCacheBlobStorage.cpp) stores a
body under `Blobs/<sha1>` and hard-links it into the record; BFS has no hard links. In
bundle-j6jimkn8 that means: every response body above `maximumInlineBodySize()`
(NetworkCacheStorage.cpp:62, :1035-1037) is written to disk, the link fails, the record
never gets its body, and the response is fetched again next time, while the orphan blob
waits for the next cache sweep. Cost observed: 93 log lines for one Speedometer iteration,
174 for ten; in the 10-iteration run the nine Perf-Dashboard JSON files (8 KiB-595 KiB)
were downloaded 10 times each, in la4ytsek once. It is wasted disk writes and network
traffic outside the timed windows, so it does not move the score.
The fix (`#if OS(HAIKU)`: `data.mapToFile(path)` directly into the record) is in the guest
and host source and in bundle-la4ytsek: 0 such lines in its logs, including 10 minutes of
stress browsing. Side effect to keep in mind: with a working disk cache la4ytsek exposes
the slow mode in every iteration (section 1).

## Ranked speedup candidates

Expected impact is on the local Speedometer 3.1 score unless stated otherwise.

1. **Disable concurrent JIT until its cost on Haiku is understood.** Measured: 0.537 -> 2.22
   (4.1x) on la4ytsek; j6jimkn8's cold iteration 0.63 -> 1.90. Zero-risk form:
   `export JSC_useConcurrentJIT=0` in the bundle's `run-browser.sh` (helpers inherit the
   environment: `execve(..., environ)` in `UIProcess/Launcher/haiku/HaikuProcess.h:85`).
   Engine form: `Options::useConcurrentJIT() = false` for `OS(HAIKU)` next to the existing
   assignments in `Source/JavaScriptCore/runtime/Options.cpp` (option declared at
   `OptionsList.h:289`). Re-measure after mimalloc lands; if the gap is gone, turn it back on.
2. **Find out why la4ytsek re-enters the cold path every iteration** (section 1). If the
   memory cache is being bypassed for resources the disk cache can serve, fixing that gives
   ordinary page reloads the same 4x even with concurrent JIT on. Check with
   `run-speedometer.py --iterations 3` per-iteration scores: healthy is slow-fast-fast.
3. **Per-frame fixed cost in the WebProcess (~18-23 ms per frame, ~80 ms per Speedometer
   step, 60-95% of most suites in fast mode).**
   a. Ship the dirty-region/unchanged-frame work already in the host copy.
   b. Keep the graphics context alive with the frame bitmap: store the
      `std::unique_ptr<GraphicsContext>` next to `m_frameBitmap`, create it only when the
      bitmap is (re)created, wrap each frame in a `GraphicsContextStateSaver`, and replace the
      destroy-to-synchronise at `DrawingAreaHaiku.cpp:321` with
      `context->platformContext()->Sync()`. Saves one offscreen window + BView + area clone +
      teardown per frame. Low risk: single thread, same lock held as today.
   c. Send the dirty rects with the frame: add `Vector<WebCore::IntRect> dirtyRects` to
      `UpdateBitmapHaiku` (`DrawingAreaProxy.messages.in:30`). UI side: keep one `BBitmap` per
      geometry in `BitmapPresenterHaiku` instead of `make_unique<BBitmap>` per frame
      (`BitmapPresenterHaiku.h:39`), copy only the dirty rows/rects (`:43`), `Invalidate(rect)`
      instead of `Invalidate()` (`WebKitView.cpp:385`), and in `Draw(update)` drop the white
      `FillRect` when a frame covers `update` and draw `DrawBitmap(image, update, update)`
      (`WebKitView.cpp:294-297`). The reader/writer race on the persistent bitmap is already
      excluded by the present acknowledgement: the WebProcess does not paint while
      `m_pendingFrame` is set and the UI only reads inside `publish()`.
   d. Cache the mapping of the shared frame in the UI process (map once per
      `m_frameBitmap`, e.g. keyed by a bitmap generation number in the message) instead of
      `ShareableBitmap::create(handle)` per frame (`DrawingAreaProxyHaiku.cpp:58`).
   Expected: no-change frames to ~0 ms and small updates to a few ms; on the fast-mode
   numbers that is up to ~2x on the score, more on Svelte/Preact/Lit/Vue.
4. **mimalloc instead of libroot malloc** (in progress by the lead; `OptionsHaiku.cmake:90`).
   Probe kernels that isolate malloc are 3-12x the reference even in fast mode
   (strings 134 vs 30 ms, detached DOM 149 vs 49 ms, create+layout 776 vs 175 ms).
5. **In-process timers instead of BMessageRunner** (done by the lead, not in a bundle yet).
   Removes ~4 ms from every zero-delay timer; each Speedometer step's async window ends with
   one, so roughly 58 x 4 ms = 0.23 s of ~11 s per iteration in fast mode (~2%), more for
   timer-heavy pages.
6. **Text measurement and drawing through app_server** (`GetEscapements` per complex text run
   and per new glyph; every fill, string and path is a message to an app_server window
   thread). Editor-TipTap (34x) and the SVG/canvas suites point here. Needs measurement once
   profiling is possible; the GL compositing path behind the build flag or an in-process
   rasteriser is the structural fix.
7. **Frame pacing**: arm the display timer for `max(0, 16 ms - time since the last frame
   started)` instead of a fixed 16 ms after the acknowledgement (`DrawingAreaHaiku.cpp:226`).
   25 fps -> up to 60 fps rAF; no Speedometer effect.
8. **Memory, not speed**: Haiku's `madvise(MADV_DONTNEED)` is a no-op
   (`src/system/kernel/vm/vm.cpp`, "TODO: Implement!") while `MADV_FREE` works, so
   `OSAllocator::decommit()` / `hintMemoryNotNeededSoon()` (`OSAllocatorPOSIX.cpp:261-285`)
   never return JIT, Wasm or GC pages. Use `MADV_FREE` for `OS(HAIKU)`.
9. **Correctness blockers found on the way**: the wake-lock page-process exit (blocks
   browserbench.org entirely) and `loadOutcome` being `process-exited` after a COOP swap.

## Stress test

`stress-20260919-104136-la4ytsek`: bundle-la4ytsek, 600 s, seed 1, max 4 tabs, 12 internet
sites (Wikipedia, HN, BBC, Guardian, GitHub, MDN, haiku-os.org, Reddit, YouTube,
OpenStreetMap, example.com) plus 9 local pages from the Speedometer checkout; 6-8 foreign
cores busy throughout.

- 157 actions: 47 new tabs, 46 closed tabs, 29 navigations, 14 back/forward, 11 tab
  selections, 10 reloads. **No crash report, no debugger entry in syslog, no hang** (the UI
  answered every scripting request, slowest reply 2.3 ms), **no load timeout** (45 s limit;
  slowest loads: YouTube 26-43 s, BBC News 23-28 s), no load errors, clean quit on request.
  0 hard-link errors; browser.log is 399 bytes.
- Memory (sum of area `ram_size` per team, sampled every ~20 s):

  | Process | start | end (one idle tab, 45 s settle) | max | trend |
  |---|---|---|---|---|
  | Summit (UI) | 27.0 MiB | 38.2 MiB | 38.2 MiB | +0.4 MiB/min, 11 -> 20 threads |
  | NetworkProcess | 21.8 MiB | 80.7 MiB | 80.7 MiB | +3.6 MiB/min overall, flattening (56 MiB at 80 s, 75 at 430 s, 80 at 600 s), 10 -> 25 threads |
  | WebProcesses (sum) | 34.4 MiB | 502 MiB in 4 processes | 882 MiB in 6 | up to 393 MiB for one page |

- After closing all tabs but one, four WebProcesses were still alive (115, 298, 65 and
  24 MiB; the last is the prewarmed spare). That is consistent with WebKit's process and
  back/forward caches, but with `MADV_DONTNEED` being a no-op (candidate 8) nothing they
  decommit is returned either. Ten minutes are not enough to call any of the three growth
  curves a leak; a `--duration 3600 --local-only` run would settle NetworkProcess and UI.
- Known, by source: the bundles' `MainThreadSharedTimer::setFireInterval()` leaks one
  `BMessage` per timer arm (fixed by the lead's timer rewrite).

## Platform findings

- **Haiku's `profile` tool panicked the guest kernel.** I ran one 2-iteration Speedometer
  under `profile -k` (my addition; no such option exists in the tools any more). The
  benchmark finished, and when the profiled teams exited the kernel stopped with
  `PANIC: page fault, but interrupts were disabled. Touching address 0x20 from ip
  0xffffffff800c16fc`, backtrace `profiling_event(timer*) + 0x3c <- timer_interrupt` on an
  idle thread, hrev59866+79. Every process in the shared VM froze, including two other
  engineers' builds; the lead had to hard-reset the guest, and `/SummitExtensions` had to
  be mounted again by hand. Screens: `.vm/claude-kernel-panic-profiler-20260919.png`,
  `.vm/bench/speedometer-20260919-101601-profiled/guest-kernel-panic.png`.
  **Do not use `profile`, `scheduler_recorder` or kernel tracing in this VM.** That is also
  why this document has no function-level profile.
- Summit's single-launch behaviour and the 64-byte `team_info.args` limit (see Harness).
- `/SummitExtensions` does not auto-mount after a reset; the tools check for it and stop
  rather than write to the 95%-full boot volume.

## Noise and what the numbers can and cannot support

- I polled for a quiet VM with `--wait-quiet` from 11:18 (after the reset at 10:21 the
  builds restarted within minutes); 4-8 compiler processes were active at every sample. No
  uncontended number exists. The quiet-window attempt (`chain-3`/`chain-4` logs in
  `.vm/bench/`) was still waiting when this was written; if it completes, its run
  directories end in `-quietwait`.
- The benchmark is bound by one WebProcess thread and the VM has 12 vCPUs, so moderate
  foreign load costs little: two j6jimkn8 default runs an hour apart gave 2.03 ± 0.42 and
  2.27 ± 0.42, la4ytsek's ten iterations span 0.50-0.57. Single iterations do get hit
  (0.79 among 2.0-2.3 with 7 foreign cores).
- Run-to-run evidence is thin: one 10-iteration run per bundle and setting, 3 iterations
  for two of the diagnostic runs, one run per probe variant. The conclusions rest on
  effects of 3x and more that repeat across Speedometer and the probe; the smaller probe
  differences (for example `JSC_useFTLJIT=0` vs default on the DOM kernels) are within the
  noise and I draw nothing from them.
- The wide confidence interval of the j6jimkn8 runs is the cold first iteration, not
  measurement noise; without it the iterations span 2.24-2.56.

## Skia builds on Haiku (September 21, 2026)

The measurements above closed out micro-optimisation: three hypotheses tested,
two worth nothing and one worth 3%, with the remaining ~10 ms a frame spread
diffusely across 32 fill calls. What is left is structural -- one thread paints
at about 95 ms per megapixel while 31 cores idle -- so the next work is the
drawing model.

That work turns out to be mostly *adoption*, not invention. The pinned engine
tree already carries Skia (`Source/ThirdParty/skia`, 216 MB, fully vendored)
and `SkiaPaintingEngine`, which is upstream's parallel tile painter: a
`WTF::WorkerPool` sized to half the cores, tunable with
`WEBKIT_SKIA_CPU_PAINTING_THREADS`, plus a second pool for GPU painting. GTK and
WPE ship it. It is gated on `USE(COORDINATED_GRAPHICS) && USE(SKIA)`, but the
Windows port sets `USE_SKIA` with `USE_TEXTURE_MAPPER` and no coordinated
graphics, so the two can be adopted in stages.

Adopting it answers all three goals with one architecture rather than three:
tiles make a scroll a compositor translate instead of a repaint (which is also
exactly why GPU compositing measured four times *slower* -- `DrawingAreaHaiku`
invalidates the whole layer on scroll), TextureMapper composites through the
zink/NVK GL stack that already works on the GTX 1070, and painting moves off the
single web-process thread. It also removes the cost structure this round was
chipping at, since Skia rasterises in-process: no app_server IPC per drawing
call and no state round trips at all.

So the first step is the cheapest possible test of the riskiest assumption --
does Skia even build on Haiku?

**It does.** `libSkia.a`, 787 objects, from the vendored sources with one
one-line port fix. The build needs `freetype_devel`, `fontconfig_devel`,
`harfbuzz_devel`, `glib2_devel`, `expat_devel` and `libwebp_devel`, all of which
exist in HaikuPorts and had to be fetched with `curl` (see
[workstation](workstation.md) -- `pkgman` still cannot download on that machine).

The port fix: Skia's `include/private/SkFeatures.h` tests for `linux`,
`__FreeBSD__`, `__GLIBC__`, `__unix__` and friends, none of which Haiku defines,
so it fell through to its last resort and chose `SK_BUILD_FOR_MAC`. Three files
then asked for Grand Central Dispatch (`dispatch/dispatch.h`), `xlocale.h` and
`malloc/malloc.h`. The other 784 objects compiled unmodified. Everything Skia
wants on Haiku -- pthreads, mmap, dlopen, FreeType, fontconfig -- is its Unix
configuration, so `Source/ThirdParty/skia/CMakeLists.txt` now defines
`SK_BUILD_FOR_UNIX` for Haiku. It has to be a command line definition: several
sources include `SkFeatures.h` before the `SK_USER_CONFIG_HEADER` is read, so
putting it in `WebKitSkiaConfig.h` is too late, which the first build proved.

Build it with:

```
SUMMIT_REMOTE_SHELL=tools/ws.sh SUMMIT_REMOTE_TAG=ws \
SUMMIT_ENGINE_BUILD_NAME=SkiaSpike SUMMIT_ENGINE_CMAKE_EXTRA="-DUSE_SKIA=ON" \
tools/build-webkit-in-vm.sh --modern-extensions Skia
```

### What stage two needs

`WebCore`'s Skia support is wired as one `include(platform/Skia.cmake)`, the
same line GTK, WPE, Windows and PlayStation use; it brings 81 sources and the
private headers. The Haiku port would add that line, drop the roughly 25 files
in `platform/graphics/haiku` that Skia supersedes (`GraphicsContextHaiku`, the
`Font*` family, `Path`, `Gradient`, `NativeImage`, `Image`, the `ImageBuffer`
backends, the tile store), and keep the dozen that Skia has no opinion about
(the `IntRect`/`FloatRect` conversions, `IconHaiku`, `MediaPlayerPrivateHaiku`,
`PlatformDisplayHaiku`, `ShareableBitmapHaiku`, `SystemFontDatabaseHaiku`). The
new Haiku-specific code is the present path: get the pixels out of Skia's
surface and into the `BBitmap` that `DrawingAreaHaiku` already hands to the UI
process. That is the stage that has to be gated on a pixel comparison.

## Skia draws the page (September 21, 2026)

`USE_SKIA=ON` now builds the whole port: `libWebCore.a`, `libWebKit.so`,
`WebProcess`, `NetworkProcess` and a browser bundle, and it **renders**. The
scroll benchmark and a live Wikipedia article both come out correct -- text,
images, gradients, rounded rectangles, shadows, SVG icons and form controls.

The port keeps both renderers. `USE(HAIKU_GRAPHICS)` is the new name for "the
app_server backend": the port had been using `PLATFORM(HAIKU)` and `USE(HAIKU)`
to mean that, which stopped being true the moment Skia became selectable. 26
sites moved to the new macro; the ones that are about the platform rather than
the renderer -- the `BRect`/`BPoint` conversions, the event loop, the network
stack -- kept `USE(HAIKU)`.

What the switch involved, beyond the source lists:

- **The themes.** `ScrollbarThemeHaiku`, `ThemeHaiku` and `RenderThemeHaiku`
  paint with `BControlLook`, which needs a `BView` to draw into, and under Skia
  `platformContext()` is an `SkCanvas`. Under Skia the port now includes
  `platform/Adwaita.cmake` -- the theme GTK, WPE, Windows and PlayStation all
  use, painted entirely through `GraphicsContext`. The native Haiku look stays
  in the app_server configuration.
- **Fonts.** Skia finds fonts through fontconfig, which on Haiku already knows
  about `/boot/system/data/fonts`: `fc-list` reports 117 fonts and `fc-match
  sans-serif` resolves to Noto Sans. `USE_HARFBUZZ` is on, because Skia shapes
  text with `SkiaHarfBuzzFont`.
- **IPC.** `FontPlatformSerializedData` is a different struct under each
  renderer, so the WebKit layer includes `Platform/Skia.cmake` for the Skia
  coders instead of `WebCoreFontHaiku.serialization.in`.
- **GL.** `GraphicsContextSkia` and `SkiaGPUAtlas` call `GLContext`, `GLFence`
  and `BitmapTexture` even when rasterising on the CPU, and the port only
  compiled those with TextureMapper. They now build under Skia too, with
  `USE_EGL` set so they are not compiled away to nothing.
- **Images.** `IconHaiku` and `MediaPlayerPrivateHaiku` still get a `BBitmap`
  from Tracker and from the media decoder; both now wrap those pixels as an
  `SkImage` (`kUnpremul`, because `B_RGBA32` holds straight alpha). The video
  frame borrows the pixels rather than copying them, since the draw finishes
  before the call returns.
- **The present path needed nothing.** `DrawingAreaHaiku` already paints through
  `ShareableBitmap::createGraphicsContext()`, and the UI process copies raw
  pixels into a `BBitmap`. `ShareableBitmapSkia` wraps the same shared memory
  with `SkSurfaces::WrapPixels`, and Skia's N32 is BGRA on little-endian, like
  `B_RGBA32`.

### Scroll copying does not survive the change

| Scroll page, workstation | Scrolling | Rendering |
| --- | --- | --- |
| app_server backend | 44.4 fps | correct |
| Skia, scroll copy on | 41.0 fps | **bands of doubled text** |
| Skia, scroll copy off | 7.7 fps | correct |

Moving the pixels a scroll keeps on screen assumes the previous frame, shifted
by a whole number of device rows, is still what the page would paint. app_server
lays glyphs out on whole pixels, so it is. Skia positions them at subpixel
offsets, so it is not, and over a couple of hundred frames the difference
collects into bands where a line of text is drawn over the ghost of itself. The
same page on the same machine is clean under app_server, so this is about the
renderer and not about the benchmark.

Two fixes were tried and neither explained it, so both were removed rather than
left in as folklore: clearing the uncovered strip before repainting it changed
nothing at all (the affected rows are not in any repaint rectangle), and
widening every repaint rectangle by a pixel made the tag pills legible again but
left the text bands. The clip is not antialiased -- `GraphicsContextSkia::clip`
passes `false` -- so a blend at the boundary was not the mechanism either.

Scroll copying is therefore off by default when Skia draws (`SUMMIT_SCROLL_COPY=1`
turns it back on to look at the artefact). The honest cost is 41 fps to 7.7, and
the honest reading is that a full repaint of 1.8 megapixels through one thread is
what scrolling costs without it. That is the problem tiles solve: with tiled
compositing a scroll is a translate of already-painted tiles and nothing is
repainted or moved at all, which is stage three.

### Haiku's fontconfig has no conf.d, and that crashed the web process

Speedometer killed the web process every run, in
`FontCache::fontForPlatformData` -> `WTF::HashTable::validateKey`: an empty font
was being used as a hash key. The path is
`FontCache::lastResortFallbackFont()`, which asks for `serif`, and when that
does not resolve falls back to `SkTypeface::MakeEmpty()` -- a font whose
`FontPlatformData` is indistinguishable from the hash table's empty value.

`serif` did not resolve because Haiku's `fontconfig` package installs
`conf.avail` and no `conf.d`, so none of the standard configuration is active,
including `45-generic.conf` and `60-generic.conf`, which are what define the
`serif`, `sans-serif` and `monospace` aliases. `fc-match` hides this -- it
answers every query with a best effort -- but an in-process family match returns
nothing. Before: `fc-match serif`, `sans-serif` and `monospace` all answered
"Noto Sans". After linking the standard set into
`/boot/system/settings/fonts/conf.d`: Noto Serif, Noto Sans and Noto Sans Mono.

This is a property of the machine, not of Summit, and it affects any fontconfig
client on Haiku. It is written up in [workstation](workstation.md).

### Where Skia stands

| Workstation | Scrolling | Speedometer 3.1 |
| --- | --- | --- |
| app_server backend | 44.4 fps | 3.28 ± 0.058 |
| Skia | 7.7 fps | 2.59 ± 0.551 |
| Firefox 155 | -- | 8.34 ± 0.37 |

Skia is behind on both, and neither number is the point yet: this is the first
build, it paints on one thread, scroll copying is off, and none of the work that
made Skia worth adopting -- tiles, a worker pool, compositing through GL -- is
turned on. What the round establishes is that the engine's Skia port runs on
Haiku and draws real pages correctly, which is the thing stage three needs.

The Speedometer confidence interval is worth noting: ± 0.551 against ± 0.058 for
the app_server backend, on an equally quiet machine. Something in this
configuration is far less consistent run to run, and that is worth understanding
before reading much into the mean.

## Coordinated graphics: what it takes on Haiku (September 21, 2026)

Stage three is the one that matters for the cores: `SkiaPaintingEngine` paints
tiles on a `WTF::WorkerPool`, and it is compiled only with
`USE(COORDINATED_GRAPHICS) && USE(SKIA)`. The port's own comment said coordinated
graphics "must stay off ... its tile painting requires Skia or Cairo"; with Skia
that reason is gone, and the option now follows `USE_SKIA` whenever GL
compositing is built.

The configuration is `-DUSE_SKIA=ON -DUSE_HAIKU_GL_COMPOSITING=ON
-DENABLE_ASYNC_SCROLLING=ON`, built in its own directory (`SkiaCG`). What it took
to get WebCore through:

- **The portable render target.** `AcceleratedSurface` can back a frame with a
  DMA-BUF, a GL texture or a `ShareableBitmap`, and only the last is portable.
  Haiku selects `Type::SharedMemory`, which is the buffer the port already
  carries to the UI process. The surface's shared-memory paths were written
  `#if PLATFORM(GTK) || ENABLE(WPE_PLATFORM)`; Haiku is now in that condition,
  and the DMA-BUF includes that came with it are guarded by `USE(GBM)`.
- **Async scrolling and the scrolling thread.** The coordinated scrolling nodes
  only exist with `ENABLE(ASYNC_SCROLLING)`, as on GTK, and the tree they build
  runs on its own thread -- `PlatformEnableGlib.h` turns that on for every port
  that reaches coordinated graphics through GLib, and the same rule now applies
  to Haiku. The tree also asks wheel events which part of a gesture they are,
  which is `ENABLE(KINETIC_SCROLLING)`; the answers are inline in
  `PlatformWheelEvent.h`, so it costs the port nothing. That define has to be
  set before `PlatformEnable.h` reaches its own default, which is 0.
- **libepoxy.** The coordinated renderers include `<epoxy/egl.h>` directly. It
  is in HaikuPorts (`libepoxy_devel`). Haiku's build of it declares the Khronos
  enums itself instead of including `KHR/khrplatform.h`, so whichever of the two
  a translation unit sees first wins and the other conflicts; `GLContext.h` and
  the port's own two GL files now prefer epoxy when `USE(LIBEPOXY)` is set, as
  the rest of WebCore already did.
- **`RenderProcessInfo`.** Every coordinated renderer includes it, and it is a
  plain WTF struct that happens to live in `Shared/glib` beside the GLib
  argument coders. The port puts that directory on the search path; nothing else
  in there is picked up, because the rest is named for GLib.

WebCore builds and links in this configuration, which is the half that contains
the tile painting. The UI process half is where the work remains.

### It builds, runs and loads pages

The coordinated configuration now links -- `libWebKit.so`, `WebProcess`,
`NetworkProcess` -- and a bundle built from it starts, opens its window, loads
`http://example.com`, sets the tab title from the page and reports "Ready". The
content area is blank, and the reason is known rather than mysterious: nothing
constructs the `AcceleratedBackingStore`. The web process paints a frame into a
shared memory buffer and publishes it; in the UI process no one is listening,
because `AcceleratedBackingStore::create` has no caller and `updateSurfaceID`
is never called. Wiring it to the Haiku view, and implementing `presentFrame`
on top of the `BBitmap` path the port already has, is the next piece.

What it took to get from "WebCore compiles" to that:

- **The right drawing area.** The port had been given
  `DrawingAreaCoordinatedGraphics.cpp`, which is what Windows and PlayStation
  build. GTK and WPE build `DrawingAreaCoordinatedGraphicsGLib.cpp`, and that is
  the one that matches this `LayerTreeHost` -- despite the name it contains no
  GLib at all, only its own `#include`.
- **The port's own compositor stands down.** `LayerTreeHostHaiku` is built on
  `GraphicsLayerTextureMapper`, and coordinated graphics supplies
  `GraphicsLayerCoordinated` instead; only one of the two may define
  `GraphicsLayer::create`. `DrawingAreaHaiku` and `LayerTreeHostHaiku` are
  therefore not compiled in this configuration, and `DrawingArea::create`
  returns the coordinated one.
- **The DMA-BUF render target stays behind.** Haiku had been put in the same
  condition as GTK and WPE, which brought the texture target that exports the
  framebuffer as a DMA-BUF along with it. That one is now guarded to the
  platforms that have one; Haiku keeps only the shared memory target.
- **Smaller platform lists that predate a third coordinated port:** the
  `WebDisplayRefreshMonitor` override needs `HAVE(DISPLAY_LINK)` and without it
  the base returns nullptr and WebCore drives rendering from a timer;
  `dispatchAfterEnsuringDrawing` and its pending-callback partner are pure for
  GTK and WPE and now have defaults for the Haiku drawing area, whose own
  message for them is not declared either; and `ScrollbarsController::create` is
  defined both by the base and by the generic controller that
  `ScrollbarsControllerCoordinated` derives from, so the base stands down for
  this configuration.

### Coordinated graphics renders

The whole pipeline runs and the page appears:

```
web process display created      EGL, through the private Mesa's zink on NVK
renderer layer tree surface 1    the coordinated layer tree, not the fallback
layer tree updateRendering       the rendering update reaches the layer tree
compositor renderLayerTree       the compositor thread renders it
willRenderFrame 1276x711         the surface hands it a render target
buffer 1, buffer 2               shared memory buffers, published to the UI process
frame 1 bitmap=1                 a frame arrives and is presented
```

`AcceleratedSurface`'s counterpart in the UI process is
`AcceleratedBackingStore`, which GTK and WPE each implement. Haiku's
(`Source/WebKit/UIProcess/haiku/AcceleratedBackingStore.cpp`) keeps the shared
memory buffers by identifier, presents the committed one with
`presentBitmapHaiku` -- the hook the software drawing area already uses, so the
pixels reach the `BView` the way they always did -- and answers `FrameDone` so
the web process may paint again. The DMA-BUF message is guarded by `USE(GBM)`
so the generated receiver does not ask Haiku for Linux buffer types.

Three more things had to be true, and each was silently absent:

- **The view has to own one.** It is created when the drawing area reports a
  surface, through the page client's `enterAcceleratedCompositingMode`.
- **Accelerated compositing has to be on.** The port turned it off in the UI
  process, from when GL compositing was opt-in and slower. With it off the
  coordinated drawing area quietly chooses its non-composited renderer, which
  paints the whole page on one thread: no tiles, and so no `SkiaPaintingEngine`
  and no worker pool. That one line was holding up the point of the exercise.
- **The web process needs a shared `PlatformDisplay`.** The coordinated drawing
  area asks for one as soon as a page is created, and `sharedDisplay()` asserts
  rather than making one; `LayerTreeHostHaiku` used to create it on the way into
  GL compositing and is not built any more. The GLib and PlayStation ports set
  theirs in `platformInitializeWebProcess`, and Haiku now does too. There is no
  software fallback: without EGL the web process dies on that assertion, and on
  this machine EGL comes from the private Mesa, so `LIBRARY_PATH` must include
  `/boot/home/summit-mesa/prefix/lib`. It is `LIBRARY_PATH` on Haiku, not
  `LD_LIBRARY_PATH`.

And the one that hid longest: **`RunLoopObserver` has to do something.**
`RunLoopObserver::schedule()` is an empty stub on any port that is neither CF
nor GLib, and `isScheduled()` answers false forever. The coordinated renderers
drive *every* rendering update through that observer, so the layer tree, the
compositor and the surface all started correctly and then waited on something
that could not fire. Haiku's implementation dispatches to the run loop, which
runs the callback on the next turn rather than as the loop goes idle -- close
enough, and it keeps the update off the caller's stack. (That first
implementation was not enough on its own -- see "Rendering updates run" below,
where it had to learn which run loop it was asked for.)

The worker pool is running: the web process has eight `SkiaCPUWorker` threads,
which is `SkiaPaintingEngine` painting tiles on half the cores.

## Rendering updates run (September 22, 2026)

The coordinated build painted two or three frames and then froze. Both of the
things the previous round left open turned out to be the same kind of mistake --
work ending up on a thread that cannot do it -- and fixing them is what turned
the configuration from "renders once" into a browser that scrolls at the
refresh rate.

### The rendering update was running on the compositor thread

The symptom was that `RenderingUpdateScheduler::scheduleRenderingUpdate()` was
called 642 times and returned early every time, because its fallback timer said
it was already scheduled; the timer itself never fired. Instrumenting it showed
the timer `isActive()` and overdue by a growing margin, which a `WebCore::Timer`
can only be when it is in a `ThreadTimers` heap that nothing services -- that
is, when it was started on a thread other than the one it belongs to. Logging
`isMainThread()` in `startTimer()` confirmed it: the first two updates came from
the main thread, the third from a thread named `ThreadedCompositor`, and from
then on the page was dead.

The backtrace named the path exactly:

    WebCore::Page::scheduleRenderingUpdateInternal()
    WebCore::Page::renderingUpdateCompleted()
    WebKit::WebPage::finalizeRenderingUpdate(...)
    WebKit::LayerTreeHost::updateRendering()
    WTF::LoopHandler::MessageReceived(BMessage*)     <- compositor's run loop

`LayerTreeHost::updateRendering()` is reached from a `RunLoopObserver`, and
`ThreadedCompositor::renderLayerTree()` -- which runs on the compositor thread
-- schedules its did-composite observer like this:

    m_didCompositeRunLoopObserver->schedule(&RunLoop::mainSingleton());

The run loop is an argument because the observer is armed from one thread and
has to fire on another. Haiku's implementation ignored it and used
`RunLoop::currentSingleton()`, so the callback ran on the compositor thread, and
with it `didComposite`, `updateRendering`, the whole rendering update and every
timer it starts. The fix is to honour the argument: `PlatformRunLoop` is
`RunLoop*` on Haiku, and `schedule()` targets the loop it is given.

Two details matter in that implementation:

- **The flag, not the timer, answers `isScheduled()`.** The timer can fire on
  the target thread before `dispatchAfter()` has even returned, so a `RefPtr`
  member is not a reliable record of whether the observer is pending. A bool
  under a lock is, and the callback checks it before running.
- **Only the owning thread may arm a timer.** `RunLoop::TimerBase::start()`
  takes the target `BLooper`'s lock, and the thread being armed holds that lock
  for as long as it is dispatching a message -- including while it waits on the
  thread doing the arming. Cross-thread scheduling therefore uses
  `RunLoop::dispatch()`, which needs no lock. The timer exists only to dodge
  `breakToAllowRenderingUpdate()`'s one-cycle dispatch suspension, and that
  suspension lifts by itself, so a dispatched observer is at worst one turn
  late.

### Image decodes finish on the main thread

`AsyncImageDecoder` hops back to `RunLoop::currentSingleton()` as captured when
the decode was requested. Once tiles are painted on a worker pool, a decode
requested from a paint completes on that worker, and the completion walks into
`CachedResource::setDecodedSize` -> `MemoryCache::singleton()`, which is
`RELEASE_ASSERT(isMainThread())`. The reply run loop is now the current one only
when the request came from the main thread, and `RunLoop::mainSingleton()`
otherwise.

### Buffers were never returned to the swap chain

With frames actually flowing, the web process started dying with "Failed to
create handle for shared memory buffer". `AcceleratedSurface::SwapChain` hands
out a render target per frame and only gets one back when the UI process sends
`AcceleratedSurface::ReleaseBuffer`. Haiku's `AcceleratedBackingStore` presented
the frame and answered `FrameDone`, but never released the buffer, so the chain
allocated a new `ShareableBitmap` -- two file descriptors -- for every frame
until the process ran out. (The upstream guard is an `ASSERT` on
`s_maximumBuffers`, which is nothing in a release build.) `presentBitmapHaiku`
copies the pixels into a `BBitmap` before it returns, so the buffer is free the
moment the frame call is done and is released right there.

### A dropped wake-up could suspend dispatch for good

`RunLoop::wakeUp()` posted `'loop'` to the handler and ignored the result. That
is the one message that must not be lost: `performWork()` is what lifts
`suspendFunctionDispatchForCurrentCycle()`, and nothing posts again to a queue
that is already non-empty, so a single dropped wake-up stops every
`RunLoop::dispatch()` in the process permanently. It now retries from the timer
thread, the same way a dropped timer notification already did. Retrying on the
calling thread would deadlock when the caller is the looper itself.

### Where the coordinated build stands

Measured with `tools/bench/run-probe.py --page scroll.html` on the workstation,
1913x935 viewport:

| Workstation | Scrolling | Speedometer 3.1 |
| --- | --- | --- |
| app_server backend | 44.4 fps | 3.28 ± 0.058 |
| Skia, single-threaded, no compositor | 7.7 fps | 2.59 ± 0.551 |
| Skia + coordinated graphics | **62.3 fps** | not yet (see below) |
| ... with async scrolling off | 59.3 fps | 6.41 on the one suite measured |
| Firefox 155 | -- | 8.34 ± 0.37 |

The scroll run is 600 frames at 62.3 fps, mean 16.05 ms, p95 17 ms, p99 18 ms,
longest 19 ms, **zero frames over 33 ms**. Idle is 65.6 fps. That is the frame
pacing the rendering-update timer asks for, held for the whole burst on a page
with 400 cards and an 84,000 px document -- 1.4x the app_server backend and 8x
the first Skia build, with the jitter gone.

`AsyncImageDecoder`'s fix is verified on the page that first showed the
problem: `en.wikipedia.org` loads, lays out and draws its text, links, infobox
and images with the web process intact.

### Speedometer does not finish yet

Speedometer 3.1 runs but stops partway -- reproducibly inside
`Editor-CodeMirror`, at the same step -- and the process then sits at 0% CPU.
The watchdog added for this says the rendering pipeline is not the thing that is
stuck:

    Summit stall: 3.4 s with no rendering update; waitingForRenderer=0
      scheduledWhileWaiting=0 frozen=0 suspended=0 observerScheduled=0 updating=0
      | state=Idle reasons=0 waitingForTiles=no renderTimerActive=no
        didCompositeFn=none pendingTiles=no suspended=0
    Summit stall: page | pageScheduled=no remainingSteps=0 unfulfilled=0
      visible=yes throttling=0 updateInterval=15 rafDocuments=0 | ...

Everything is idle and consistent: the compositor has nothing pending, no
document has a `requestAnimationFrame` callback waiting, and nothing has asked
for a rendering update. Reporting the main thread's dispatch queues and timer
heap from the same watchdog said the same thing -- `dispatchSuspended=0
hasSuspended=0 currentQueue=0 nextQueue=0`, DOM timer throttling off, and the
timer heap's next entry a legitimate second away.

That reading was wrong, and the page said so. With `--progress-beacons` the
benchmark reports each test as it starts and finishes, and a two-second
`setInterval` reports that the page's own event loop is still turning. At the
hang the test beacons stop *and so does the interval* -- and because beacons
travel through the network process, the same tick also goes into
`document.title`, which the harness reads over the browser's own control
channel. That title froze at `[tick 4]` and stayed there while the control
channel kept answering in 50 microseconds.

So the page is not waiting on its own JavaScript: **its DOM timers have
stopped**. That is consistent with the engine dump after all -- a timer heap
whose front is a second away, a shared timer that says it is armed, and nothing
firing -- and it makes the notification that drives
`MainThreadSharedTimer` the place to look. Note the asymmetry worth keeping in
mind: the watchdog's own `RunLoop::Timer` kept firing every second throughout,
so it is not that Haiku's timers stopped, it is that one particular timer's
notification went missing.

One of the two causes is known. The suite that hangs, `Editor-CodeMirror`, has
a scroll step, and it completes with the scrolling thread out of the picture:

| Editor-CodeMirror alone, 1 iteration | Result |
| --- | --- |
| Skia, no compositor (`bundle-xknrxz_v`) | completes, 3.030 |
| coordinated, async scrolling on | **hangs**, same step every time |
| coordinated, `SUMMIT_DISABLE_ASYNC_SCROLLING=1` | completes, **6.410** |

So where the coordinated build finishes, it is 2.1x the build it replaces. The
switch is new and Haiku-only: upstream's `WEBKIT_DISABLE_ASYNC_SCROLLING` is
behind `ENABLE(DEVELOPER_MODE)`, which release builds do not set.

It costs about 5% of scroll frame rate and none of the pacing -- 59.3 fps,
p99 19 ms, still no frame over 33 ms -- so it is a usable default until the
scrolling tree is fixed.

With it set, the full ten-iteration run still stops, at a different suite
(`TodoMVC-Lit-Complex-DOM`, and in other runs the last test of the first
iteration) with the benchmark's iframe blank. Where it lands varies, which is
the signature of a race rather than of a particular test.

### Which timer stops, and where it stops

The beacons narrowed it to "DOM timers stop"; two rounds of instrumentation in
`RunLoopHaiku.cpp` then said exactly which timer and on which side of the port.

The deadline thread keeps its own time, so it can still speak when a looper has
gone quiet. Asked what it was holding at the hang, the web process answered:

    Summit sch: deadlines=1 front=JSRunLoopTimer::Manager::PerVMData::Timer in 3655939 ms
    Summit reg: handler 0xc92fa73aa0 holds 1 timers: JSRunLoopTimer::Manager::PerVMData::Timer -3655.9s

One deadline, an hour out, and **`MainThreadSharedTimer::timer` is not there at
all** -- not overdue, not waiting, not registered. It was stopped and never
armed again.

That rules out the two explanations that looked most likely. It is not a dropped
`'tmrf'`: a recovery pass that re-posts notifications for timers that are
registered, overdue and unknown to the scheduler was added, and it never fired
once, because there was nothing registered to recover. And it is not the
rejected notifications the log is full of -- those are the ordinary kind, a
stale identifier superseded by a newer arming.

So the loss is above the port, in WebCore. `ThreadTimers::updateSharedTimer()`
is the only thing that arms the shared timer, and it stops it here:

    if (m_firingTimers || m_timerHeap.isEmpty()) {
        m_pendingSharedTimerFireTime = MonotonicTime { };
        protect(m_sharedTimer)->stop();
    } else { ...arm... }

and `TimerBase::setNextFireTime()` only calls `updateSharedTimer()` again when
the heap's *front* changes:

    if (wasFirstTimerInHeap || isFirstTimerInHeap)
        threadGlobalDataSingleton().threadTimers().updateSharedTimer();

Those two together are the trap. The earlier engine dump caught the heap in
exactly the fatal shape -- seven timers, front about a second away -- with the
shared timer stopped. No DOM timer can run, so nothing can change the front, so
nothing ever calls `updateSharedTimer()` again. The process is wedged by
construction, and every DOM timer in it is dead.


### The fire loop never finishes, so no timer ever fires again

Tagging the trace with the process id -- several processes write to the same
log, which had made one earlier reading ambiguous -- finishes the story. These
are the last things the *web* process's `ThreadTimers` did:

    Summit tt[242096]: stop with 61 queued, firing=1, front in -1 ms
    Summit tt[242096]: stop with 60 queued, firing=1, front in -1 ms
    Summit tt[242096]: stop with 59 queued, firing=1, front in -0 ms

and then nothing. No `fire loop done`. `sharedTimerFiredInternal()` never
reached its end, so the main thread is still inside `item->timer().fired()`.

That single fact explains everything seen so far, and it is worse than a stuck
page:

    m_firingTimers = true;
    while (...) { ... item->timer().fired(); ... }
    m_firingTimers = false;          // <- plain assignment, no RAII

While the loop is running, `updateSharedTimer()` takes its first branch --
`if (m_firingTimers || m_timerHeap.isEmpty())` -- and *stops* the shared timer,
which is safe only because the loop arms it again on the way out. If the loop
never gets out, the shared timer stays stopped with 59 timers queued, and
`TimerBase::setNextFireTime()` only calls `updateSharedTimer()` when the heap's
front changes, which nothing can now do. **Every DOM timer in the process is
dead, and stays dead even if the blocking call later returns.** That is why the
page's `setInterval` stops, why `document.title` freezes, and why the engine
looks perfectly idle and healthy while it does.

It also explains why the Haiku-side recovery never fired: the shared timer is
not "registered but unnotified", it is deliberately stopped. Nothing below
WebCore is broken.

So the remaining question is a much smaller one than it looked: **what does the
timer callback block on?** The process sits at 0% CPU, so it is a wait, not a
spin, and the coordinated-graphics work supplies the obvious suspects -- a
synchronous wait on the compositor such as
`CoordinatedSceneState::waitUntilPaintingComplete()`, a `sendSync` to the UI
process, or one of the scrolling-tree waits. A backtrace of the web process's
main thread at the hang answers it outright, and the hang reproduces in about
seven minutes:

    Debugger --cli --thread <main thread id>      # then: thread <id>, bt

with no other debugger attached. Note that the control channel keeps answering
in microseconds throughout, because that is the UI process; it says nothing
about the web process's main thread.

Worth fixing on its own account, whatever the blocking call turns out to be:
`m_firingTimers` deserves a `SetForScope`, so that a callback that exits the
loop by any route cannot leave the process with no timers at all.


Two measurement traps were fixed along the way, both in the harness rather than
the browser:

- `guest.wake_display()` built its kill command with `awk "{print $2}"` inside a
  single-quoted Python string, so the remote login shell ate `$2` and the screen
  blanker was never killed. A blanked screen stops app_server drawing, the UI
  process stops acknowledging frames, and the browser looks frozen -- which is
  exactly what the first three scroll runs recorded as "timeout".
- `run-probe.py` woke the display once, before the run. It now does so every
  30 seconds, because the blanker comes back on every idle period.

### Real Reddit scroll, 2026-09-22

An opt-in `SUMMIT_FRAME_STATS=1` build reported UI-process coordinated frame delivery over
one-second windows, including median, p95, maximum inter-frame time, and the
number of gaps over 33 ms. The synthetic 400-card scroll probe measured 62.0
frames/s in the UI process, matching the page's 62.02 frames/s. Its 60 fps
result does not predict real Reddit scrolling.

On `https://www.reddit.com/r/popular/`, VNC wheel input moved the feed in both
directions. In `.vm/bench/reddit-vnc-down-up-20260922/`, the downward pass fell
from 32.9 and 28.4 frames/s to 2.4 frames/s, with a maximum 897 ms gap. The
upward pass over previously seen content began at 56.4 frames/s, then fell to
3.7 and 2.8 frames/s, with a maximum 638 ms gap. Screenshots in that directory
confirm page movement. These measurements implicate work beyond loading new
posts; the source of the stalls still needs isolation.

The follow-up `SUMMIT_COMPOSITOR_TIMING=1` trace measured compositor preparation,
scene flush, painting, and frame send. In
`.vm/bench/reddit-compositor-trace-20260922/`, the compositor's median frame
took 2.0 ms and p95 took 3.4 ms. The 590–867 ms gaps occurred between frames,
so most of that lost time was not spent inside the compositing stages. The
trace also exposed an uncontrolled redraw loop: Reddit produced 350–363
coordinated frames/s while no user input was being sent. Haiku's shared-memory
target does not wait for vertical sync during `swapBuffers()`.

An experiment scheduled Haiku compositor renders at least 1/60 second after
the last render started. Two fresh `/r/popular/` wheel passes used a title-bar
focus click and confirmed feed movement without navigating away. The unpaced
bundle (`.vm/bench/reddit-unpaced-titlebar-20260922/`) rendered 250–350
frames/s in most windows. The paced bundle
(`.vm/bench/reddit-paced-titlebar-20260922/`) rendered mostly 56–61 frames/s
with about 16.8 ms median spacing. It still had two approximately 775 ms
inter-frame gaps, so pacing removes excess redraw work but does not yet make
real Reddit scrolling reliably smooth. The temporary compositor trace logged
every frame and was removed after this measurement.

The paced build also completed the controlled 400-card probe at 59.79
frames/s over 120 scroll frames, with p95 19 ms, maximum 20 ms, and no gap
over 33 ms (`.vm/bench/probe-20260922-225543-summit-paced-scroll/`).

Additional temporary timing narrows the remaining long gaps. With
`SUMMIT_MEDIA_CANCEL_TIMING=1`, one media cancellation took 128 ms, including
119 ms in `BSoundPlayer::Stop()`
(`.vm/bench/reddit-media-cancel-trace-20260922/`). It occurred near a 798 ms
frame gap, but other gaps had no matching cancellation. With
`SUMMIT_RENDER_UPDATE_TIMING=1`, `LayerTreeHost::updateRendering()` took up to
1042 ms, 593 ms, and 304 ms during a wheel pass. Almost all of that time was
inside `Page::updateRendering()`, while scene flush took less than 0.1 ms
(`.vm/bench/reddit-render-update-trace-20260922/`). This located the stall
inside the WebCore page update.

`SUMMIT_PAGE_UPDATE_TIMING=1` performed that split in
`.vm/bench/reddit-page-update-trace-20260922/`. A 390 ms page update spent
360 ms in initial layout. A later 731 ms update spent 436 ms before animation
frame callbacks and 262 ms in a second layout. Another 467 ms update spent
186 ms before callbacks, 213 ms in animation frame callbacks, and 55 ms in
final layout. Thus both layout and page callback work cause the remaining
stalls; the compositor frame itself is comparatively cheap. The media
cancellation next to these updates spent 118 ms in `BSoundPlayer::Stop()`.
Haiku's local SoundPlayer implementation shows that blocking `Stop()` sleeps
for the audio output latency after stopping, whereas `Stop(false)` skips that
sleep. Summit now uses the nonblocking form during teardown.

The same 12-second H.264/AAC file played for one second and then had its
`video.src` cleared in `media-cancel.html`. With blocking stop, that JavaScript
operation took 154 ms, of which 142 ms was the sound stop. With nonblocking
stop it took 15 ms (`.vm/bench/probe-20260922-231754-summit-blocking-cancel/`
and `.vm/bench/probe-20260922-231832-summit-nonblocking-cancel/`). The new
bundle still played the Reddit CMAF H.264 video to its 12.7-second `ended`
event without a media error
(`.vm/bench/probe-20260922-231448-summit-nonblocking-media/`). A further
Reddit feed trace still had a 727 ms frame gap attributable to long page
updates, so the media stop fix removes one independent pause but does not
resolve the heavy page layout and script work.

The 60 Hz pacing experiment reduced Speedometer 3.1 from a 5.17 ± 0.24
unpaced control rerun to 3.96 ± 0.21 (ten iterations each, uncontended).
Pacing compositor-only animation frames scored 4.29 ± 0.19. Waiting until a
page had made no rendering update request for a second scored 4.87 ± 0.14,
but Reddit still rendered 200–250 frames/s while idle because it kept making
WebCore updates. Those results are in `.vm/bench/speedometer-20260922-233001-baseline-rerun/`,
`.vm/bench/speedometer-20260922-231944-paced-media/`,
`.vm/bench/speedometer-20260922-232702-animation-only-pacing/`, and
`.vm/bench/speedometer-20260922-233638-idle-animation-pacing/`. The pacing
policy was reverted: it did not remove the measured long page-update stalls
and it made the benchmark slower. Avoid reintroducing a broad 60 Hz timer
without a different rendering hand-off design.

The one-off frame, compositor, page, rendering-update, and media-cancel timing
hooks were subsequently removed from the production path. Their captured
logs remain in the bench directories above.

Removing those probes did not restore the 5.17 control score. The unpaced
build with all probes scored 4.40 ± 0.40; removing the Page probe scored
4.12 ± 0.19; removing the remaining rendering probes scored 4.20 ± 0.35;
and removing the frame statistics hook scored 4.01 ± 0.13. Each was a
completed, uncontended 10-iteration local Speedometer run. The cause of the
slow warmup in these newer bundles remains open, separate from the rejected
pacing policy.

An older bundle produced just after HTTP media repair and before the frame
probe scored 5.18 ± 0.26 on a fresh control run
(`.vm/bench/speedometer-20260923-000923-http-media-control/`). A later bundle
with the frame probe scored 4.42 ± 0.38
(`.vm/bench/speedometer-20260923-000635-post-media-control/`), and the current
probe-free bundle scored 4.01 ± 0.13. The JavaScriptCore library hashes are
identical. Disabling concurrent JIT on the current bundle scored 3.69 ± 0.14
(`.vm/bench/speedometer-20260923-001215-no-concurrent-jit/`). A temporary
rebuild with the old blocking media stop scored 4.11 ± 0.25
(`.vm/bench/speedometer-20260923-001802-blocking-stop-control/`), ruling out
the nonblocking stop as the cause. The current code retains `Stop(false)` and
the verified media cancellation improvement. The JavaScriptCore binaries are
identical; the newer WebKit builds have different `.text` contents from the
older 5.18-point bundle, though a rebuild of the same current source produced
identical `.text`. The remaining Speedometer warmup difference needs
investigation at runtime as well as the build level. The restored nonblocking
bundle is `bundle-viu0mveh` in the workstation's latest-bundle manifest.

### Skia worker count on the 32-logical-core workstation

WebKit obtains Haiku's online processor count through `sysconf` and normally
uses half of it, capped at eight, for CPU tile painting. With the same
`bundle-viu0mveh`, Mesa prefix, 800×600 Speedometer viewport, fresh profile,
and ten iterations per run, the worker-count sweep was:

| CPU paint workers | Speedometer 3.1 score | 400-card scroll fps | Gaps over 33 ms |
| --- | ---: | ---: | ---: |
| 1 | not run | 56.91 | 27 |
| 2 | 5.19 ± 0.22; 5.14 ± 0.27 | 60.43 | 0 |
| 4 | 5.20 ± 0.24; 4.32 ± 0.40; 3.01 ± 0.12 | 62.32 | 0 |
| 8 | 2.85 ± 0.12 | 60.78 | 0 |

All runs were classified uncontended by the harness. The four-worker scores
varied substantially on the *same bundle*, so the slow-bundle explanation
above is incomplete. No Summit/WebKit process was left running between tests;
`sysinfo` showed normal CPU frequencies. Two workers gave both stable
Speedometer scores and a scroll result without long gaps. Haiku now caps its
default at two CPU paint workers while still scaling down on smaller machines
and honoring `WEBKIT_SKIA_CPU_PAINTING_THREADS` for experiments. The paths are
`.vm/bench/speedometer-20260923-00*skia-workers*/` and
`.vm/bench/probe-20260923-00*skia-workers*/`. This controlled page does not
reproduce Reddit's layout and animation callback stalls.

The fast and slow four-worker result files also show a runtime-wide change,
not just a first-iteration warmup difference. Every iteration total in the
3.01-point run was 1.6–2.0 times the corresponding total in the 5.20-point
run. React Stockcharts panning grew from 73 to 329 ms; its synchronous and
asynchronous portions grew 4.4 and 4.5 times respectively. CodeMirror's long
edit grew from 65 to 162 ms, with its synchronous portion growing 3.1 times.
The same bundle and worker count produced both results. A future controlled
run should capture process CPU and clock state during the test before
attributing this variation to a WebKit code change.

The rebuilt default bundle `bundle-kou9exyv` scored 5.11 ± 0.21 in an
uncontended 10-iteration run with no worker override
(`.vm/bench/speedometer-20260923-004746-skia-two-default/`). Its 400-card
scroll probe reached 61.21 fps, p95 18 ms, maximum 23 ms, and no gaps over
33 ms (`.vm/bench/probe-20260923-005006-summit-skia-two-default/`).

Against Firefox's 8.34 ± 0.37 run at the same 800×600 viewport, Summit's
editor suites are among the largest remaining gaps: TipTap took 375.5 versus
124.8 ms (3.01×) and CodeMirror 120.2 versus 51.5 ms (2.33×). Chart.js was
2.25× slower. The sum of Summit's suite means was 4525 ms per iteration, with
36.3% in Speedometer's asynchronous portion, versus Firefox's 3050 ms and
28.9%. These are per-suite timings from the two completed local-copy runs,
not a function-level profile; allocation, text, and layout costs still need
separate measurement before attributing the gaps.

The `summitctl scroll` synthetic wheel burst now visibly moves a real Reddit
feed. In `.vm/bench/scroll-20260923-011441-reddit-synthetic-wheel/`, 100 wheel
notches took `/r/popular/` from its initial top post to several later posts;
`before.png` and `final.png` preserve the positions. This run used the
production bundle while an engine compile was active, so it establishes input
delivery and page movement only. No per-frame timing was emitted. The
real-site harness now labels such a capture `captured-uninstrumented` instead
of reporting an empty frame-rate result as a completed measurement.

The native scroll test interface now reports burst completion, delivered wheel
messages, elapsed delivery time, and the final send status through
`summitctl state`. The real-site harness waits for that completion before its
final capture. This matters on a busy page because each wheel send can block
for up to two seconds; the requested interval alone cannot prove that every
notch was delivered. A failed or incomplete burst is reported separately from
a completed frame-rate measurement. The browser and controller compile checks
passed.

The bundled runtime check passed on `/r/popular/`: both allocator variants
delivered all 300 requested wheel notches in about 4.81 seconds, and the
before/after screenshots show different posts. The opt-in native view counter
is reset at burst start so a long quiet period before scrolling is not counted
as a scroll stall. In the system-allocator capture
(`.vm/bench/scroll-20260923-023428-reddit-system-reset/`), the longest gap
between delivered frames was 1725 ms, with seven gaps over 33 ms. The mimalloc
capture (`.vm/bench/scroll-20260923-023806-reddit-mimalloc-awake/`) had a
650 ms longest gap and nine gaps over 33 ms. Different live feed posts loaded
in the two runs, so this is evidence of remaining real-site jank, not a
controlled allocator win. The view counted frame deliveries, which can exceed
the display refresh rate and should not be read as visible presentation fps.

An intervening mimalloc capture produced black before/after screenshots after
Haiku's screen blanker reactivated. Its frame numbers are excluded. The
real-site harness now wakes the display during settling and rejects a black
capture instead of recording it as a completed measurement.

### Mimalloc allocator comparison

Two alternating, uncontended ten-iteration Speedometer 3.1 pairs used the
same Summit source and native view, the same Mesa prefix and 800×600 benchmark
viewport, and separately bundled `SkiaCG` and `SkiaCGMi` engines. The system
allocator scored 5.08 ± 0.24 and 5.28 ± 0.23; mimalloc scored 6.26 ± 0.28
and 6.31 ± 0.31. The mean score rose from 5.18 to 6.28, about 21%. Results
are in `.vm/bench/speedometer-20260923-023954-skia-two-system-control/`,
`.vm/bench/speedometer-20260923-024212-skia-two-mimalloc/`,
`.vm/bench/speedometer-20260923-024414-skia-two-system-repeat/`, and
`.vm/bench/speedometer-20260923-024630-skia-two-mimalloc-repeat/`. The
previous same-bundle 4-worker variation is why the allocator was tested in
alternating pairs.

The matched 600-frame static scroll probes reached 61.81 fps with system
malloc and 62.18 fps with mimalloc, with no interval over 33 ms in either
(`.vm/bench/probe-20260923-024839-summit-system-scroll-control/` and
`.vm/bench/probe-20260923-024927-summit-mimalloc-scroll/`). The mimalloc
bundle also played the Reddit H.264 CMAF video to its 12.7-second `ended`
event without a media error
(`.vm/bench/probe-20260923-025013-summit-mimalloc-media/`). Haiku now
defaults to mimalloc while retaining CMake overrides for allocator experiments.
The 6.28 mean is still below Firefox's measured 8.34; the remaining editor,
layout, and callback costs need further work.

A fresh `AllocatorDefaultsCheck` CMake configuration, without allocator
override flags, resolved `USE_MIMALLOC=ON` and `USE_SYSTEM_MALLOC=OFF`
(`.vm/configure-allocator-defaults.log`). The temporary configure directory
was removed after checking its cache. The updated mimalloc engine rebuilt and
linked cleanly.

With `SUMMIT_MEDIA_CODEC_TRACE=1`, its WebProcess identified the selected
decoder as `H.264 on the graphics card (NVDEC)` while the direct Reddit CMAF
file played to `ended`
(`.vm/bench/probe-20260923-025529-summit-mimalloc-browser-codec/`). A live
`/r/popular/` feed scroll logged 18 NVDEC selections and showed video imagery
in its screenshots
(`.vm/bench/scroll-20260923-025633-reddit-mimalloc-codec-feed/`). The same
scroll still had a 663 ms maximum delivered-frame gap and 13 gaps over 33 ms.
Several Media Kit messages said it could not detect HLS from a nonstandard
extension and MIME type. Those messages warrant investigation for feed videos
that fail despite working H.264 MP4 decoding; a still screenshot does not
prove complete playback of every feed video.

With mimalloc, forcing four Skia CPU paint workers scored 6.30 ± 0.31 in an
uncontended ten-iteration Speedometer run
(`.vm/bench/speedometer-20260923-030034-mimalloc-skia-workers-4/`). That is
effectively the same as the two-worker scores, so the lower worker count stays
the default for now.

An active-process Haiku sampling profile of a five-iteration CodeMirror and
TipTap run is saved at
`.vm/bench/speedometer-20260923-030521-mimalloc-profile-late/profile.txt`.
Its inclusive stacks include `Page::updateRendering` (8455 samples),
request-animation-frame callbacks (about 6350), layout (about 2800), a Skia
tile replay worker (2141), and a separate DFG JIT compile worker (5751).
These counts overlap along call stacks and are not a CPU-time partition; JIT
generated code is also not fully symbolized. They support profiling the page
update, script callback, and layout path before investing in Skia GPU tile
painting. An earlier system-wide profile started before WebProcess launched and
did not include it; its result is excluded.

An exclusive Haiku sampler run over the same editor suites was dominated by
unsymbolized JIT addresses, so it cannot attribute the remaining cost to a
specific engine function
(`.vm/bench/speedometer-20260923-031114-mimalloc-editor-exclusive/`).
Disabling FTL JIT scored 6.37 ± 0.31 in ten Speedometer iterations, within
the variation of the mimalloc baseline; FTL remains enabled. The JSC
SamplingProfiler flag emitted no report on WebProcess shutdown in this setup.

An opt-in wheel-route trace on live `/r/popular/` recorded all 100 synthetic
wheel events on WebKit's scrolling tree: `tree=1 sync=0 blockingDOM=0
route=scrolling`. Delivery completed in 1.604 seconds with no send error.
The before and after screenshots show different Reddit posts
(`.vm/bench/scroll-20260923-032157-reddit-wheel-route-native/`). That run did
not request view frame statistics. It establishes that these wheel events
reach the scrolling thread; it does not identify whether the long frame gaps
seen in earlier captures occur during composition, the native bitmap copy,
or view-message delivery. The next instrumented capture timestamps the view
message after bitmap publication and measures its queue delay separately.

The timestamped follow-up
(`.vm/bench/scroll-20260923-032824-reddit-queue-attribution/`) delivered all
300 wheel events. During the burst, the native-view counter saw 603 frame
messages at 111.58/s, an interval as long as 790.3 ms, and 11 intervals over
33 ms. The longest delay between bitmap publication and the view handler was
only 3.6 ms, with no queue delay over 33 ms. The idle sample likewise saw
133.9 messages/s while its maximum queue delay was 0.2 ms. The UI message
queue is therefore not causing the long gaps; Reddit is also driving the
compositor well above the display rate and spending CPU on frames that cannot
all be shown.

A same-bundle follow-up compared the optional 60 fps compositor limit with an
uncapped control. The uncapped capture delivered 175.22 frame messages/s idle
and 109.79/s during the wheel burst, with a 1037.5 ms worst burst interval
(`.vm/bench/scroll-20260923-033507-reddit-compositor-uncapped-control/`). The
60 fps capture delivered 45.34/s idle and 34.17/s during its burst, with a
649.2 ms worst interval
(`.vm/bench/scroll-20260923-033411-reddit-compositor-60/`). Live feed content
differed and the capped run had one 630.9 ms UI queue outlier, so the gap
change is directional rather than a controlled frame-latency result. The cap
does clearly coalesce redundant composition work. It scored 6.269 ± 0.301 in
an uncontended ten-iteration Speedometer run, matching the uncapped mimalloc
baseline (`.vm/bench/speedometer-20260923-033627-mimalloc-compositor-60/`).
Haiku now defaults to the screen's nominal refresh rate, or 60 fps when that
rate is unavailable. `SUMMIT_COMPOSITOR_MAX_FPS=0` disables the limit and a
value from 30 through 240 overrides it for diagnosis or high-refresh displays.

The suspected Reddit HLS failure is reproducible independently of the feed.
The current completed-file HTTP path downloads
`https://v.redd.it/u5pu7ad5rcdh1/HLSPlaylist.m3u8` to an extensionless file;
Media Kit rejects it with media error 4 at time zero and reports that it cannot
detect HLS without a standard extension or MIME type
(`.vm/bench/probe-20260923-034125-summit-reddit-hls-downloaded-control/`).
Passing the original HTTPS URL directly also returns error 4
(`.vm/bench/probe-20260923-034501-summit-reddit-hls-direct/`). An experimental
local mirror downloaded the master playlist, child playlists, and CMAF assets.
Giving Media Kit the mirrored file URL removed its format-detection warning,
but still produced no tracks and error 4
(`.vm/bench/probe-20260923-040418-summit-reddit-hls-file-url/`). That experiment
was removed because it did not restore playback. Reddit's direct CMAF MP4 path
continues to play to `ended` with the NVDEC H.264 decoder, and live feed pages
select that decoder; complete HLS support needs a demux path that can retain
the playlist URL or supply its video and audio streams together.

The clean default-paced bundle `bundle-0xc87hol` passed both regressions. Its
600-frame 400-card scroll probe ran at 58.83 fps, p95 18 ms, maximum 31 ms,
with no interval over 33 ms
(`.vm/bench/probe-20260923-041115-summit-default-paced-scroll/`). The direct
Reddit 720p CMAF video selected `H.264 on the graphics card (NVDEC)` and reached
its 12.7-second `ended` event without a media error
(`.vm/bench/probe-20260923-041020-summit-reddit-cmaf-default-paced/`).

The same bundle reproduced its Speedometer result after the compiler was
stopped: 6.293 ± 0.302, with at most 0.05 foreign CPU cores
(`.vm/bench/speedometer-20260923-051716-mimalloc-generic-control-fresh/`).
Increasing JSC's global worklist limit from three to eight and setting four
Baseline, DFG, and FTL compiler threads scored 6.085 ± 0.307
(`.vm/bench/speedometer-20260923-051911-mimalloc-jsc-workers-8/`). The wider
pool provides no gain on this workload and is not enabled by default.

### Focused inline text layout result

`tools/bench/pages/layout-text.html` forces 50 width-dependent layouts over
2,000 inline blocks. It holds the box geometry constant while comparing empty,
repeated-text, unique-text, simplified-text, and fixed-width cases. Uncontended
runs on the workstation measured:

| Case, milliseconds per layout | Summit | Firefox 155 |
| --- | ---: | ---: |
| empty, automatic width | 10.80 | 5.90 |
| repeated text, automatic width | 39.92 | 6.56 |
| unique text, automatic width | 40.52 | 6.54 |
| repeated text, `optimizeSpeed` | 40.10 | 6.62 |
| unique text, `optimizeSpeed` | 40.80 | 6.66 |
| empty, fixed width | 9.76 | 6.64 |
| repeated text, fixed width | 33.66 | 6.80 |
| unique text, fixed width | 34.24 | 6.90 |

The artifacts are
`.vm/bench/probe-20260923-052334-summit-layout-text-fixed-generic/` and
`.vm/bench/probe-20260923-052420-firefox-layout-text-fixed-firefox/`.
Repeated and unique strings are effectively identical, and disabling kerning
and ligatures through `text-rendering: optimizeSpeed` does not move the result.
Fixed widths remove about 6 ms from Summit but leave about 24 ms of text work
above its empty-box case. This rules out the sampled string-width cache and
advanced shaping features as the main cause; the remaining gap is in repeated
inline text layout and display-content construction.

The probe harness can now run Summit under Haiku's inclusive sampling profiler
with `--haiku-profile`. Its focused profile records 15,323 samples in layout,
14,941 in `LineLayout::layout`, and 14,711 in
`InlineFormattingContext::layout`; width measurement itself has only 54
samples. These are inclusive stacks, not a CPU-time partition. The profile is
in `.vm/bench/probe-20260923-052551-summit-layout-text-profile/profile.txt`.
An exclusive profiler attempt hung Haiku during profiler shutdown and required
a NanoKVM power cycle, so the harness deliberately exposes only the verified
inclusive mode.

### Workstation-specific compiler result

A separate engine build targeted the workstation's first-generation Zen CPU
with `-march=znver1 -mtune=znver1`. Its clean bundle
`bundle-osto12r7` scored 6.279 ± 0.292 over ten uncontended Speedometer
iterations
(`.vm/bench/speedometer-20260923-061249-mimalloc-znver1-native-mesa/`). This is
indistinguishable from the generic bundle's 6.293 ± 0.302, so the native flags
are not enabled in the production build.

The focused text-layout probe did move: repeated automatic-width text fell
from 39.92 to 36.60 ms per layout, and repeated fixed-width text fell from
33.66 to 31.72 ms
(`.vm/bench/probe-20260923-061455-summit-summit-layout-text-znver1-native/`).
Those 8% and 6% improvements confirm that generated code affects the isolated
layout loop, but they did not improve the complete browser benchmark.

### Reddit HLS playback restored

The Haiku media backend now handles Reddit's finite, unencrypted byte-range
CMAF form of HLS. It parses the master playlist, selects the highest-bandwidth
variant and its audio group, verifies that each child playlist is a finite
same-resource byte-range playlist, then gives the completed video and audio
MP4 resources to separate Media Kit readers. Unsupported encrypted, live,
discontinuous, or multi-resource playlists still take the normal format-error
path rather than being partially decoded.

On `bundle-enb94pi0`, the previously failing Reddit master playlist loaded
metadata in 756 ms and played its 720×1280 video through to the 12.7-second
`ended` event with no media error. Codec tracing selected
`H.264 on the graphics card (NVDEC)` for video and `AAC` for audio
(`.vm/bench/probe-20260923-061923-summit-summit-reddit-hls-cmaf/`). The direct
CMAF MP4 regression also played to `ended` with NVDEC
(`.vm/bench/probe-20260923-062025-summit-summit-reddit-cmaf-hls-regression/`).
The same bundle's controlled 400-card scroll test reached 59.03 fps over 600
frames, p95 18 ms, maximum 23 ms, and no frame over 33 ms
(`.vm/bench/probe-20260923-062113-summit-summit-hls-scroll-regression/`).

A live `/r/popular/` follow-up visually moved from the first post to later feed
content after all 300 paced wheel events. Its trace selected NVDEC repeatedly
and exercised the new HLS path with separate NVDEC H.264 and AAC tracks. The
page still delivered only 26.30 native-view frames/s during the burst, with a
713.1 ms worst interval, while UI queue delay remained below 0.3 ms
(`.vm/bench/scroll-20260923-063455-reddit-hls-current/`). This confirms that
Reddit media playback is restored in the live feed and again locates the
remaining scroll stalls above the native event and presentation queue.

### Fixed-width inline layout reuse experiment

`SUMMIT_FIXED_INLINE_LAYOUT_REUSE=1` now enables a guarded Haiku experiment
that retains an already-laid-out inline block's contents when its containing
block changes width but the inline block has a fixed logical width. Replaced
content, relative sizes, percentage padding, dirty boxes, and block-level
boxes are excluded. The switch remains off by default.

On the same generic bundle, repeated fixed-width text fell from 33.18 to 4.70
ms per forced layout, and unique text fell from 34.12 to 4.70 ms. Automatic
width cases were unchanged
(`.vm/bench/probe-20260923-062552-summit-summit-fixed-inline-reuse-control/`
and
`.vm/bench/probe-20260923-062638-summit-summit-fixed-inline-reuse-enabled/`).
The complete Speedometer result remained neutral at 6.303 ± 0.312
(`.vm/bench/speedometer-20260923-062736-mimalloc-fixed-inline-reuse/`).

The dedicated correctness probe compares reused boxes with freshly created
equivalents through seven widths in both LTR and RTL. It found zero geometry
mismatches among the 3,360 boxes eligible for reuse, including nested
percentage-width content, relative offsets, and percentage margins
(`.vm/bench/probe-20260923-063228-summit-fixed-inline-correctness-candidates/`).
It also reproduced 144 nested-width mismatches in percentage-padding boxes in
both the disabled and enabled runs; those boxes are excluded from reuse, and
the mismatch is an existing layout behavior rather than a result of this
experiment. With the switch enabled, the controlled scrolling probe ran at
59.10 fps, p95 18 ms, maximum 23 ms, and no frame above 33 ms
(`.vm/bench/probe-20260923-063308-summit-fixed-inline-reuse-scroll/`).

`SUMMIT_FIXED_INLINE_LAYOUT_TRACE=1` reports one-second coverage counters
without changing layout decisions. The counters separate eligible fixed-width
inline boxes from dirty, block-level, replaced, relative-sized, automatic-width,
percentage-padding, and excluded descendants. `run-scroll.py` stores both the
per-second samples and an aggregate in `run.json`; this makes a live Reddit
wheel pass the gate for deciding whether the guarded reuse path has enough
coverage to warrant further work.

That gate found no coverage on live Reddit. During the refined wheel pass,
24,195 descendants were visited in full child relayouts. Of those, 21,115
were text or another non-box renderer and all 3,080 boxes were already dirty;
3,076 carried their own layout bit, often together with child-layout bits.
Zero boxes reached the fixed-width eligibility checks
(`.vm/bench/scroll-20260923-094303-reddit-dirty-inline-reasons/`). Broadening
the shortcut would therefore skip real Reddit style or content invalidation,
so the experiment stays disabled and is not a Reddit optimization.

The scroll harness now cuts its measured log before taking the final VNC
screenshot and flushes the initial capture before starting the wheel burst.
The old order could attribute app_server capture time to scrolling; one run
recorded a 659.3 ms UI queue delay from that final capture. With capture work
isolated and the layout trace disabled, the current bundle delivered 39.56
native-view frames/s, had a 702.9 ms worst page interval, and kept UI queue
delay below 0.3 ms. The same run selected NVDEC H.264 repeatedly and resolved
Reddit's finite HLS video and audio
(`.vm/bench/scroll-20260923-095232-reddit-current-capture-isolated/`). The
remaining visible pauses are still page work rather than wheel delivery or
the native presentation queue.

### Cross-suite TipTap slowdown isolation

The current HLS-capable bundle `bundle-jiito6mn` scored **6.526 ± 0.324** in
ten uncontended iterations
(`.vm/bench/speedometer-20260923-065031-current-full-after-hls/`). TipTap took
512 ms cold and 252–286 ms thereafter. The same bundle running only TipTap took
535 ms cold and 128–139 ms thereafter
(`.vm/bench/speedometer-20260923-064333-tiptap-alone-control/`). Running either
half of the preceding default application suites before TipTap preserved its
fast steady state; combining both halves reproduced the slowdown. This is a
cumulative working-set effect rather than one conflicting suite.

Controlled diagnostics ruled out several plausible global causes:

- Recreating the benchmark iframe for every suite did not improve TipTap.
- Disabling WebKit's back-forward cache reduced the overall score by about
  3.5% and left TipTap slow.
- A forced full JSC collection after every frame load and a ten-second idle
  before every TipTap pass both left its steady time at 269 ms or more.
- Reversing all 20 suites moved TipTap near the front but left its warm passes
  at 259 ms or more.
- Raising the inline text breaking cache from 500 KB to 4 MB and raising the
  shared shaped-text cache from 3,000 to 12,000 entries did not help. The latter
  scored 6.281 ± 0.769 over five iterations with 267 ms as its best TipTap pass
  (`.vm/bench/speedometer-20260923-080501-shaped-text-cache-12000/`). Both
  changes were reverted.

Repeating TipTap immediately after its normal full-suite pass changes the
picture: the normal passes took 498, 268, and 267 ms, while the duplicate took
149, 141, and 142 ms
(`.vm/bench/speedometer-20260923-080822-full-immediate-tiptap-repeat-diagnostic/`).
An isolated duplicate remained fast after a deliberate ten-second delay at
149, 146, and 145 ms
(`.vm/bench/speedometer-20260923-084238-tiptap-ten-second-repeat-diagnostic/`).
The loss therefore requires intervening application work; it is not a simple
ten-second expiry or persistent machine state.

JavaScriptCore cache reuse is beneficial but is not the source of the extra
full-suite cost. TipTap alone with `JSC_useCodeCache=0` settled at 163–178 ms
instead of 128–139 ms
(`.vm/bench/speedometer-20260923-074203-tiptap-no-jsc-code-cache-diagnostic/`).
The complete benchmark with that cache disabled fell to 5.504 ± 0.285 and
TipTap settled at 312 ms or more
(`.vm/bench/speedometer-20260923-074724-full-no-jsc-code-cache-diagnostic/`).
Disabling shared Baseline JIT code likewise made the isolated suite modestly
slower. Retaining those caches is the correct direction.

Increasing the unlinked-code working set to 60 seconds, 64 MB, and 8,000
entries scored 6.300 ± 0.754 and left TipTap at 268 ms or more. Lowering DFG
tier-up thresholds to 200 scored 6.279 ± 0.719 and left it at 266 ms or more.
Disabling JSC collection entirely for a bounded three-iteration diagnostic
scored 5.862 ± 1.780 and left the warm TipTap passes at 279 ms or more
(`.vm/bench/speedometer-20260923-085005-full-no-jsc-gc-diagnostic/`). These
tests rule out unlinked-code cache capacity, late tier-up, and full collection
as the state transition. All diagnostic settings were reverted.

Request logs later confirmed that both the isolated and full benchmarks fetch
TipTap's HTML and JavaScript exactly once, excluding resource eviction or a
second network load. JSC compile-time logging also found 467 of the 468
isolated compile signatures in the full run. For that shared subset, Baseline
compilation was effectively identical: 298 functions and 21.45 ms isolated,
versus 299 functions and 21.17 ms in the full run. The full benchmark did,
however, emit roughly 34 MB of Baseline, DFG, and FTL code over three
iterations, compared with roughly 1.5 MB for isolated TipTap. Raising the DFG
threshold to 10,000 scored 6.179 and left TipTap at 288.8 ms; forcing
synchronous JIT compilation scored 4.321 and made TipTap slower at 381.7 ms.
Increasing the JSC compiler worker pool had already scored 6.085 and left
TipTap unchanged. The remaining evidence points to the accumulated hardware
code and data working set, rather than repeat compilation or contention with a
background compiler. There is no validated setting change to retain
(`.vm/bench/speedometer-20260923-161248-tiptap-compile-times/`,
`.vm/bench/speedometer-20260923-161336-full-compile-times/`,
`.vm/bench/speedometer-20260923-161655-full-dfg-threshold-10000/`, and
`.vm/bench/speedometer-20260923-161901-full-no-concurrent-jit-tiptap-diagnostic/`).

Archived inclusive profiles remain useful for locating broad costs. A
50-iteration TipTap-only profile shows
`Document::updateLayout` on 26.8% of all one-millisecond samples, flex layout
on about 22%, selection canonical-position work on about 13%, intrinsic width
work on about 10%, and DFG compilation on a separate worker on about 20%.
Painting was about 1%. A complete three-iteration profile independently puts
layout near 9%, style near 14%, JIT work near 19%, and Skia tile work near 8%.
The percentages are inclusive and overlap; they identify the remaining work
above painting without forming an exclusive CPU-time partition. Artifacts are
`.vm/bench/speedometer-20260923-063838-tiptap-profile/` and
`.vm/bench/speedometer-20260923-065529-full-profile-cross-suite/`.

Haiku's profiler shutdown later hung the workstation after a second complete
inclusive run, just as the earlier exclusive attempt did. No profile was
written for that run. Both benchmark harnesses now reject `--haiku-profile` on
the workstation; it remains available in the disposable VM.

### Cache Skia system typeface matches

The full profile also put `SkFontMgr_fontconfig::onMatchFamilyStyle` on 5.15%
of samples and fontconfig's pattern filtering on 4.53%. WebCore already caches
complete platform font data, but that key includes size and other properties.
Creating another size of the same family and style therefore repeated the
expensive fontconfig search.

The Skia font cache now retains the matched system `SkTypeface` by family,
weight, width, and slant. Font size, OpenType features, synthesis, orientation,
and metrics are still computed for every `FontPlatformData`, and normal font
cache invalidation clears the new map. This cache stores both successful and
failed matches so fallback family lists do not repeatedly query a missing
face.

The first ten-iteration result was 6.238 ± 0.229 with steady iterations from
6.28 to 6.38
(`.vm/bench/speedometer-20260923-090242-skia-typeface-match-cache/`). That is
neutral against the long-term generic control of 6.293 ± 0.302 and below the
unusually fast 6.526 run above. The cold TipTap pass fell from the usual
roughly 510 ms to 362 ms in this run, while steady full-suite passes remained
270–321 ms.

A recovered-host A-B-A comparison then produced 6.438 ± 0.240 with the cache,
6.243 ± 0.321 on the pre-cache HLS bundle, and 6.431 ± 0.220 after returning
to the cached bundle. The corresponding sums of suite means were 3,676,
3,804, and 3,682 ms per iteration
(`.vm/bench/speedometer-20260923-094406-inline-coverage-trace-off/`,
`.vm/bench/speedometer-20260923-094604-pre-font-cache-alternating-control/`,
and
`.vm/bench/speedometer-20260923-094800-post-font-cache-alternating-repeat/`).
This is a reproducible roughly 3% gain in the alternating comparison, while
the absolute score remains below Firefox 155's 8.338 ± 0.374 baseline.

### Live Reddit page-update phase trace

`SUMMIT_PAGE_UPDATE_TRACE=1` now records only rendering updates slower than
33 ms and splits them across each HTML rendering step. On Haiku it also emits
slow `Document::runScrollSteps()` dispatches, JavaScript scroll listeners, and
layouts so the nested work can be correlated. The disabled path reads the
environment switch once and does not read the clock. `run-scroll.py` stores
the page-update and scroll-step samples plus aggregate phase totals in
`run.json`.

The first capture-isolated `/r/popular/` pass found five slow updates totaling
1,741.5 ms. The scroll step accounted for 1,084.8 ms, ahead of second layout
at 204.2 ms, animation-frame callbacks at 186.6 ms, and initial layout at
185.5 ms
(`.vm/bench/scroll-20260923-100942-reddit-page-update-phases/`). A refined
repeat found that three slow scroll steps spent all 1,523.2 ms dispatching one
document scroll event each. Native scroll animation service, scroll anchoring,
target lookup, and visual-viewport delivery rounded to 0.0 ms. The individual
event dispatches took 455.9, 595.3, and 472.0 ms
(`.vm/bench/scroll-20260923-101608-reddit-scroll-step-phases/`).

A third pass traced the listener itself. Every slow dispatch invoked the same
single bubbling JavaScript listener; the slow calls took 64.1, 180.6, 270.9,
and 241.7 ms. Layouts nested in those calls accounted for only part of their
duration. The 270.9 ms call also emitted two failed Media Kit stream-cookie
allocations, so media creation remains a candidate for some of that handler's
non-layout time, but the log does not establish how much. The measured wheel
pass still moved the feed, delivered all 300 events, kept the native queue
below 0.4 ms, and reached 41.74 native-view frames/s with a 533.4 ms worst
interval (`.vm/bench/scroll-20260923-102211-reddit-listener-layout/`).

These runs move the primary Reddit stall from generic rendering or input
delivery to synchronous work initiated by Reddit's document scroll listener.
Skipping or delaying the standards event would change page behavior; the next
useful split is inside its JavaScript and synchronous media work.

`SUMMIT_MEDIA_LIFECYCLE_TRACE=1` now records media loads and cancellations
that block for at least 10 ms. Cancellation is divided into loader-thread join,
video-thread join, audio stop, and cleanup; the disabled path caches the
environment check and takes no clock readings. `run-scroll.py` stores the
individual samples and per-operation totals in `run.json`.
Set the value to `2` to record every call for a focused probe.

A correlated live pass found one 342.2 ms Reddit scroll listener that destroyed
a media player while its download was still active. The synchronous
`cancelLoad()` spent 51.2 ms waiting for the loader thread, and a 94.7 ms layout
also ran before the listener returned. The loader wait therefore explains a
measurable part of the pause, while roughly 196 ms remains in other page work.
The later unsupported stream-2 Media Kit diagnostic was outside this listener;
NVDEC H.264 and AAC initialization still succeeded. The complete pass reached
43.55 native-view frames/s, had a 633.2 ms worst interval, and kept native queue
delay below 0.9 ms
(`.vm/bench/scroll-20260923-125311-reddit-media-lifecycle/`).

The completed-file downloader now uses libcurl's multi interface. While a
download is active, `cancelLoad()` calls the thread-safe `curl_multi_wakeup()`
before joining the loader, with a lock protecting the multi handle's lifetime.
On the same direct Reddit CMAF file, clearing the source after 50 ms blocked
JavaScript for 31 ms in the old bundle; the lifecycle trace attributed all
30.9 ms to the loader join
(`.vm/bench/probe-20260923-130753-summit-media-active-cancel50-control/`).
The wakeable downloader returned in 2 ms, and the all-call lifecycle trace
confirmed that its active loader join took only 0.6 ms
(`.vm/bench/probe-20260923-131324-summit-media-active-cancel50-wakeup-all/`).

A live `/r/popular/` pass with the new downloader had no media lifecycle call
above the 10 ms threshold. It resolved Reddit HLS video and audio and selected
NVDEC H.264 and AAC successfully
(`.vm/bench/scroll-20260923-130308-reddit-wakeable-media-cancel/`). A separate
direct HLS probe played the complete 12.7-second clip to `ended` at 720 by
1280 with no media error, again using NVDEC H.264 and AAC
(`.vm/bench/probe-20260923-130915-summit-reddit-hls-wakeup/`).

The page-update trace now also divides each layout pass into style and
preparation, render-tree layout, view sizing, and post-layout work.
`run-scroll.py` stores the individual passes and aggregate phase totals. In a
live measured burst, 48 slow layout passes took 2,730.2 ms: render-tree layout
accounted for 2,698.9 ms (98.9%), while preparation took 8.9 ms, view sizing
0.4 ms, and post-layout work 20.4 ms. The worst 593.3 ms Reddit scroll
listener forced three full layouts of 78.3, 134.1, and 100.4 ms; about 300 ms
of their combined time was the render-tree walk. Style resolution is therefore
not the source of these forced-layout pauses
(`.vm/bench/scroll-20260923-132029-reddit-layout-phases/`).

The same opt-in trace now records dirty renderer layouts that take at least
10 ms. These timings are inclusive: a parent includes the children it lays
out, so their totals must not be added. A follow-up live pass found one stable
hot hierarchy across the burst: 47 slow `RenderFlexibleBox` calls contained
47 slow calls on the same `RenderGrid`, and that grid laid out the same
`RenderBlock` twice on nearly every pass (92 slow block calls). The flex and
grid calls each covered about 2.2 seconds of the 2.28 seconds spent in slow
render-tree passes. This narrows the next investigation to the grid's intrinsic
and final item sizing passes
(`.vm/bench/scroll-20260923-132721-reddit-renderer-layout/`).

Focused grid call-site timing confirms that both layouts of the hot block are
real work on the same grid/item pair. Of 88 slow child layouts, 46 intrinsic
row-sizing calls took 1,184.3 ms and 42 final placement calls took 765.5 ms.
All intrinsic calls were the first row-sizing iteration. The grid formatting
context rejected the hot grid because its template columns are outside the
currently supported subset, so it stayed on the legacy algorithm. The measured
burst reached 43.87 native-view frames/s, with a 601.9 ms worst interval and a
0.3 ms maximum native queue delay
(`.vm/bench/scroll-20260923-133824-reddit-grid-phases/`). The next change needs
to preserve the intrinsic and stretched sizes while avoiding one of these
full child layouts; their overlapping renderer totals cannot be treated as
independent savings.

The intrinsic-height cache cannot be reused for this case. On every hot-grid
invalidation the block had dirty normal-flow descendants, so its content height
could have changed. A second trace also classified the grid formatting context
rejection: the hot grid uses `minmax()` columns. An opt-in experiment admitted
that supported sizing primitive through the remaining coverage checks, which
then correctly rejected the same multi-column grid for
`justify-content: space-between`. The modern grid path does not yet apply those
between-track distribution offsets. The experiment therefore never selected
the modern path and was removed rather than weakening layout correctness
(`.vm/bench/scroll-20260923-134538-reddit-grid-invalidation/`,
`.vm/bench/scroll-20260923-135547-reddit-grid-minmax-justify/`, and
`.vm/bench/scroll-20260923-140008-reddit-grid-gfc-single-column/`). A useful
modern-grid optimization now requires implementing content-distribution
positions and corresponding layout coverage, rather than bypassing its guard.

The modern grid formatter now implements `space-between` track distribution
on both axes. It carries the start and between-track offsets with the used
track sizes, includes distributed gutters in grid-area sizes, and applies the
same offsets to final item positions. Haiku's additional coverage admits the
exact features observed on Reddit: `minmax()` columns, horizontal and vertical
`space-between`, percentage grid-item widths, and the validated hidden-X / auto-Y
overflow pair. `SUMMIT_EXTENDED_GRID_INTEGRATION=0` restores the previous
coverage for comparison or rollback.

`tools/bench/pages/grid-space-between.html` compares deterministic legacy and
modern geometry for positive and negative free space on each axis and for a
50% item width. All five cases matched exactly, and the trace confirmed that
the candidate fixture grids used the modern formatter
(`.vm/bench/probe-20260923-144209-summit-grid-percent-legacy/` and
`.vm/bench/probe-20260923-144232-summit-grid-percent-gfc/`). Successive live
Reddit probes then advanced the hot grid through the `minmax()`, justify,
align, and percentage-width guards. Its next rejection is reason 32, a grid
item with non-visible overflow, so the hot hierarchy still uses the legacy
algorithm and the observed frame-rate differences are not attributable to the
new formatter. The latest complete pass reached 36.19 native-view frames/s,
with a 569.8 ms worst interval and 0.3 ms maximum native queue delay; it also
selected NVDEC H.264 and AAC successfully
(`.vm/bench/scroll-20260923-144313-reddit-grid-gfc-percent-width/`).

The hot item resolves to `overflow-x: hidden` and `overflow-y: auto`. A first
guarded attempt to admit that pair exposed why the existing coverage check was
needed: the modern path initially produced a 302 px item where legacy produced
52 px, and changed its 150 px scroll width to 300 px
(`.vm/bench/probe-20260923-145340-summit-grid-overflow-legacy/` and
`.vm/bench/probe-20260923-145403-summit-grid-overflow-gfc/`). The integration
layer set the grid area's 100 px containing-block width only during the
immediate item layout, then cleared it. A later overflow-layer relayout
therefore resolved the item's 50% width against the 600 px grid container.

Final modern grid layout now keeps each in-flow item's grid-area width on its
renderer, as the legacy grid does. If coverage later rejects that grid, the
fallback path clears the retained sizes before rebuilding the legacy grid.
With this lifetime fix, the modern and legacy paths match every fixture
rectangle, client size, 150 by 60 scroll extent, and overflowing child
rectangle. A dynamic test also switches an initially modern grid to an
unsupported border-box item; its before and after geometry matches legacy and
the trace records fallback reason 26, validating the cleanup path
(`.vm/bench/probe-20260923-150654-summit-grid-dynamic-fallback-legacy/` and
`.vm/bench/probe-20260923-150718-summit-grid-dynamic-fallback-gfc/`).

The corrected guarded path moves Reddit's hot grid to the modern formatter.
In an A-B-A sequence, modern runs reached 57.79 and 55.25 native-view frames/s
with 207.9 and 261.6 ms worst intervals, while the intervening legacy control
reached 41.51 frames/s with a 565.3 ms worst interval. Native queue delay
stayed at 0.3 ms in all three runs. The tradeoff is front-loaded layout work:
the modern traces recorded 5.14 and 4.43 seconds of slow layout versus 1.79
seconds in the control. During the measured scroll, however, Reddit's repeated
slow scroll-listener layouts disappeared and only two frame intervals exceeded
33 ms in each modern run, compared with eight in the control. The rendered
feed remained visually coherent, and the first modern run selected NVDEC
H.264 and AAC successfully
(`.vm/bench/scroll-20260923-150154-reddit-grid-gfc-overflow-fix/`,
`.vm/bench/scroll-20260923-150340-reddit-grid-legacy-overflow-control/`, and
`.vm/bench/scroll-20260923-150443-reddit-grid-gfc-overflow-repeat/`).

Two uncontended ten-iteration Speedometer 3.1 runs with the extended coverage
scored **6.315 ± 0.249** and **6.346 ± 0.320**, both with a 3,748 ms sum of
suite means. The intervening exact opt-out control scored **6.178 ± 0.185**
with a 3,827 ms suite sum. This is a neutral-to-positive result against
Summit's longer-term 6.3–6.4 range and gives no benchmark reason to withhold
the Reddit improvement
(`.vm/bench/speedometer-20260923-150923-extended-grid-speedometer/`,
`.vm/bench/speedometer-20260923-151131-extended-grid-speedometer-control/`, and
`.vm/bench/speedometer-20260923-151340-extended-grid-speedometer-repeat/`).

The coverage is therefore enabled by default on Haiku. A final no-flag
geometry run matched the exact `0` opt-out result, including overflow metrics
and the dynamic fallback transition
(`.vm/bench/probe-20260923-151859-summit-grid-default-optout-legacy/` and
`.vm/bench/probe-20260923-151924-summit-grid-default-modern/`). The final
default `/r/popular/` validation reached 55.27 native-view frames/s with a
261.8 ms worst interval, two intervals over 33 ms, and a 1.6 ms maximum native
queue delay. The hot grid had no fallback reason, and media tracing selected
NVDEC H.264 and AAC successfully
(`.vm/bench/scroll-20260923-152006-reddit-grid-default-modern/`).

### libstdc++ assertion experiment

The normal Release configuration still enables libstdc++ container assertions.
A clean `SkiaCGMiNoStdAssert` engine build tested
`-DUSE_CXX_STDLIB_ASSERTIONS=OFF` while retaining Skia, coordinated graphics,
mimalloc, the same engine patch, and the private Mesa runtime. This rebuilds
JavaScriptCore and WebCore, so it directly covers the JIT, DOM, style, and
layout paths that dominate Speedometer rather than changing only the browser
shell.

The assertion-free bundle scored **6.363 ± 0.226** over ten uncontended
Speedometer 3.1 iterations, with a 3,732 ms sum of suite means
(`.vm/bench/speedometer-20260923-123625-no-stdlib-assertions/`). An immediate
ten-iteration assertion-enabled control scored **6.471 ± 0.245** with a
3,652 ms suite sum
(`.vm/bench/speedometer-20260923-123829-stdlib-assertions-control-post/`). Both
runs began with 0.05 foreign CPU cores of load. Disabling the assertions was
1.7% slower by score and 2.2% slower by aggregate suite time, consistent with
the two earlier assertion-enabled controls at 6.438 and 6.431. The production
build therefore keeps its current setting.

The candidate also completed a live 300-notch `/r/popular/` pass at 47.94
native-view frames/s. Its native UI queue stayed below 0.3 ms while the worst
visual interval reached 598.7 ms, again matching the document-listener stall
rather than an input queue delay. Media tracing selected NVDEC H.264 six times
and AAC once, all with status 0
(`.vm/bench/scroll-20260923-124028-reddit-no-stdlib-assertions/`). This confirms
that the clean candidate retained accelerated Reddit playback, but provides no
reason to adopt the slower assertion setting.

### Canvas 2D acceleration control

The remaining Speedometer gap is concentrated rather than uniform. Against the
same-workstation Firefox baseline, Chart.js costs about 206 ms more per full
iteration, TipTap about 167 ms more, and Perf Dashboard about 106 ms more;
several news and jQuery suites already match or beat Firefox. Chart.js draws
5,366 scatter points, making it a useful focused Canvas 2D workload.

Haiku now accepts `SUMMIT_CANVAS_ACCELERATION=0` to disable the generated
`CanvasUsesAcceleratedDrawing` preference, or any other value to enable it.
Leaving the variable unset preserves WebKit's Skia default. This makes CPU/GPU
comparisons repeatable without maintaining separate engine builds.

Twenty-iteration Chart.js runs measured 369.0 and 384.1 ms with the default
accelerated path around an intervening 376.2 ms CPU run. The per-test split
moved in the expected direction in the first comparison (for example, scatter
draw sync time was 140.3 ms accelerated and 142.1 ms CPU), but the default
repeat drifted beyond that difference. Canvas acceleration is therefore not
the cause of the roughly twofold Firefox gap, and remains enabled by default
(`.vm/bench/speedometer-20260923-153400-chartjs-canvas-default/`,
`.vm/bench/speedometer-20260923-153449-chartjs-canvas-cpu/`, and
`.vm/bench/speedometer-20260923-153542-chartjs-canvas-default-repeat/`).

### Canvas damage coalescing

The larger Chart.js cost was coordinated layer invalidation. Each canvas fill
or stroke sent a dirty rectangle through the canvas element, renderer,
compositing layer, and graphics layer. The 5,366-point scatter plot therefore
sent more than ten thousand notifications per draw even though they all belong
to one frame.

Haiku now unions HTML canvas damage within a frame and skips a notification
when the accumulated rectangle already contains the new damage. Drawing and
cached-image invalidation still happen for every operation. Offscreen canvases
retain the existing behavior, and `SUMMIT_CANVAS_DAMAGE_COALESCING=0` provides
an exact opt-out. This trades some extra pixels in the bounding damage area for
far fewer main-thread calls into the renderer and layer tree.

A 30-iteration Chart.js control averaged 356.7 ms. Two coalesced candidates
averaged 250.1 and 247.3 ms, and the final default implementation averaged
244.3 ms. In the first comparison, translucent scatter drawing fell from
136.6 to 99.9 ms, tooltip redraw from 111.0 to 77.2 ms, and opaque scatter
drawing from 109.1 to 72.9 ms
(`.vm/bench/speedometer-20260923-154323-chartjs-damage-control/`,
`.vm/bench/speedometer-20260923-154417-chartjs-damage-coalesced/`,
`.vm/bench/speedometer-20260923-154518-chartjs-damage-coalesced-repeat/`, and
`.vm/bench/speedometer-20260923-160041-chartjs-damage-default-final/`).

On the full benchmark, the coalesced candidate scored **6.605 ± 0.237** with a
3,509 ms sum of suite means. The immediate same-bundle opt-out control scored
**6.366 ± 0.222** with a 3,717 ms sum. Chart.js fell from 391.9 to 255.3 ms and
Perf Dashboard from 362.3 to 312.4 ms, while TipTap was unchanged at about
289 ms. This is a 3.8% score increase and a 5.6% reduction in aggregate suite
time. A final default-on build repeated the improvement at **6.508 ± 0.235**
and a 3,561 ms suite sum
(`.vm/bench/speedometer-20260923-154611-canvas-damage-coalesced-full/` and
`.vm/bench/speedometer-20260923-154847-canvas-damage-full-control/`, and
`.vm/bench/speedometer-20260923-160224-canvas-damage-default-final/`).

The deterministic `canvas-damage.html` fixture draws 5,000 dispersed marks and
four corner blocks across two display cycles. The default candidate rendered
all marks, returned the exact expected RGBA values at all four corners, and
produced a visually complete screenshot
(`.vm/bench/probe-20260923-160145-summit-canvas-damage-default-final/`). A live
Reddit pass retained the grid improvement at 54.64 native-view frames/s, a
257.2 ms worst interval, two intervals over 33 ms, and 0.3 ms maximum native
queue delay. Its media log selected NVDEC H.264 twice and AAC once with status
0 (`.vm/bench/scroll-20260923-155049-reddit-canvas-damage-coalesced/`).

### Fast full-circle Canvas fills

Perf Dashboard's range-selection test remained about 55 ms slower than
Firefox in its synchronous phase. Application-level timers isolated most of
that phase to Canvas chart redraws. A warmed range selection submitted 6,530
`fill()` calls and 252 `stroke()` calls; its time-series rendering consumed
about 68 ms in Summit and 59 ms in Firefox. Each plotted point uses the common
`beginPath(); arc(..., 0, 2 * Math.PI); fill()` sequence.

WebCore already recognizes single arcs in the Skia backend, but sent a complete
arc without an explicit `closePath()` through `SkCanvas::drawArc`. Canvas fill
semantics implicitly close the subpath, so a complete filled arc is exactly an
oval. The Skia path now uses `drawOval` for that case. Partial arcs and all
unclosed strokes keep the existing path so cap, join, and chord behavior stay
unchanged.

The deterministic `canvas-paths.html` probe submits 5,000 circles per round.
The old bundle took a 21 ms median for open full-circle fills, versus 18 ms for
the already optimized explicitly closed form. The new bundle reduced the open
form to 19–20 ms while leaving closed fills and open strokes essentially
unchanged. A complete 64 by 64 pixel comparison between the two fill forms was
exact. Firefox measured an 18 ms median for the open form on the same machine
(`.vm/bench/probe-20260923-164634-summit-canvas-fill-oval-control/`,
`.vm/bench/probe-20260923-164728-summit-canvas-fill-oval-final/`, and
`.vm/bench/probe-20260923-165007-firefox-canvas-paths-baseline/`).

In matched ten-iteration Perf Dashboard runs, the candidate reduced the suite
from 315.5 to 308.0 ms. Render fell from 52.7 to 48.8 ms and point selection
from 122.6 to 118.8 ms; range selection was unchanged at about 140 ms, leaving
its non-Canvas event and drag work for later investigation
(`.vm/bench/speedometer-20260923-164354-perf-dashboard-fill-oval-control/` and
`.vm/bench/speedometer-20260923-164251-perf-dashboard-fill-oval/`). The full
benchmark scored **6.419 ± 0.217**. Its aggregate suite time was neutral within
normal run variance, while Perf Dashboard improved from the previous final
bundle's 314.7 to 303.7 ms
(`.vm/bench/speedometer-20260923-164803-canvas-fill-oval-full/`).

### Cached Haiku window geometry

Speedometer's Perf Dashboard driver synthesizes 23 mouse events per iteration.
For every event it reads `screenX` and `screenY` while constructing the DOM
event. Each property used to call `WebChromeClient::windowRect()`, which made a
synchronous IPC request to the UI process. The Haiku UI client did not supply a
window frame, so the two round trips also returned the wrong zero origin.

Stage timing around those 23 events attributed 11--14 ms per iteration to
event construction before the change. Haiku now records `BWindow::Frame()` in
the view geometry snapshot and sends it to the WebProcess whenever that
snapshot changes. `windowRect()` reads the cached value there. With the cache,
the benchmark observed the correct `(4, 1)` origin from its requested window
frame, and event construction fell to 1--2 ms. Total measured mouse-event work
fell from 36--37 to 24--25 ms
(`.vm/bench/speedometer-20260923-165401-perf-dashboard-mouse-event-timers/`
and
`.vm/bench/speedometer-20260923-172205-perf-dashboard-window-frame-cache-timers/`).

In clean ten-iteration focused runs, Perf Dashboard fell from 308.0 to
287.0 ms. Its range-selection test, which contains most of these synthesized
events, fell from 140.4 to 124.4 ms. The full ten-iteration candidate scored
**6.582 ± 0.239**, with 3,498 ms of aggregate suite time and a 289.5 ms Perf
Dashboard mean. The previous final bundle scored **6.508 ± 0.235**, with
3,561 ms aggregate time and a 314.7 ms Perf Dashboard mean
(`.vm/bench/speedometer-20260923-172405-perf-dashboard-window-frame-cache/`
and
`.vm/bench/speedometer-20260923-172723-window-frame-cache-full/`).

A live `/r/popular/` check delivered all 48 paced wheel events at 60.7 native
view frames/s. Its worst interval was 26.6 ms, no interval exceeded 33 ms, and
the native queue delay stayed below 0.1 ms. Before and after screenshots confirm
that the feed moved
(`.vm/bench/scroll-20260923-172917-window-frame-cache-reddit/`). The same
candidate played the direct Reddit 720x1280 CMAF regression stream through its
12.7-second `ended` event with no media error. The browser trace selected
`H.264 on the graphics card (NVDEC)` with status 0
(`.vm/bench/probe-20260923-173116-summit-window-frame-cache-reddit-nvdec/`).

### Perf Dashboard layout investigation

After removing the mouse-event IPC cost, 23 `getBoundingClientRect()` calls in
Perf Dashboard still spent about 18--20 ms per iteration in layout. Phase
timing showed that virtually all of this time was inside
`RenderBlockFlow::layout()`; coordinate conversion itself took about 0.006 ms
per call. A trial that allowed a clean canvas leaf through WebKit's partial
layout eligibility check did not improve the focused result (285.4 ms versus a
287.0 ms control).

A one-run dirty-renderer trace explained why. Before the canvas rectangle is
read, the application changes normal-flow text in its heading, navigation,
status view, and table cells. The canvas is clean, but these nodes share its
ancestor chain and carry real `selfNeedsLayout` and `normalChildNeedsLayout`
state. The relevant ancestor also has an out-of-flow child awaiting layout.
Skipping layout for left/top geometry in this state could return stale canvas
coordinates, so the trial shortcut and all tracing were removed
(`.vm/bench/speedometer-20260923-174935-perf-dashboard-layout-phase-split/`,
`.vm/bench/speedometer-20260923-180135-perf-dashboard-clean-replaced-geometry/`,
and
`.vm/bench/speedometer-20260923-182839-perf-dashboard-dirty-renderers/`).

### Fixed-repeat grid coverage trial

A fresh `/r/popular/` pass on the clean production bundle selected NVDEC H.264
for direct video and for an HLS video with separate AAC audio. Its 180-notch
burst delivered 51.14 native-view frames/s with a 256.0 ms worst interval and
0.3 ms maximum UI queue delay. The current bundle also played a direct Reddit
HLS playlist to its 12.7-second `ended` event at 720 by 1280, with no media
error and NVDEC H.264 plus AAC selected
(`.vm/bench/scroll-20260923-183852-reddit-media-current/` and
`.vm/bench/probe-20260923-184005-summit-current-reddit-hls-nvdec/`).

The remaining burst gap coincided with a 212.8 ms page update, including
203.6 ms of final layout. The slow render-tree calls again involved a grid and
its flex item. This grid's first modern-coverage rejection was a fixed
`repeat(...)` column list. A guarded trial admitted fixed repeats whose tracks
were supported breadth or `minmax()` values and whose line-name entries were
empty. WebKit already expands those tracks into `GridTemplateList::sizes` for
the modern formatter. The new `grid-space-between.html` cases confirmed exact
legacy geometry for a three-column repeat; a repeat with named lines stayed on
the legacy path.

On Reddit, that change advanced the hot grid to rejection reason 25, an item
margin, without improving scrolling: 50.75 frames/s and a 280.6 ms worst
interval (`.vm/bench/scroll-20260923-185138-reddit-fixed-repeat-candidate/`).
The modern formatter also has code for fixed margins, so a second guarded
trial admitted fixed margins while leaving auto and other unresolved margins
on the legacy path. The fixed-margin fixture matched legacy geometry and its
auto-margin counterpart still fell back. Reddit's hot item continued to
reject on a nonfixed margin, while its burst reached 35.69 frames/s with an
894.2 ms worst interval. Live feed content varied, so this scroll comparison
alone does not quantify the candidate's cost
(`.vm/bench/scroll-20260923-185606-reddit-repeat-margin-candidate/`).

The full ten-iteration Speedometer comparison settled the decision. Production
scored **6.557 ± 0.243**, fixed-repeat-only scored **6.589 ± 0.226**, and the
fixed-repeat plus fixed-margin build scored **6.236 ± 0.140**. Aggregate suite
time was 3,513, 3,506, and 3,680 ms respectively. These were uncontended
runs on the same workstation and Mesa runtime; several application suites
slowed under the margin candidate. Both engine coverage changes were removed.
The geometry fixture remains for a later implementation that can demonstrate
a real improvement
(`.vm/bench/speedometer-20260923-185904-fixed-repeat-margin-production-control/`,
`.vm/bench/speedometer-20260923-190055-fixed-repeat-only-full/`, and
`.vm/bench/speedometer-20260923-185710-fixed-repeat-margin-full/`).

### Why the legacy grid lays out the item twice

The hot Reddit grid runs its flex item once to measure intrinsic row height and
again for final placement. A focused `RenderGrid` trace found that the grid
area width remained 1121 px across those passes. Row sizing temporarily
cleared the area's height, and final placement restored a definite height.
The item has percentage-height descendants; WebKit's stretch requirement
therefore requested another layout even when the requested outer height
matched its current height. Those descendants can resolve differently between
the indefinite intrinsic pass and the definite final pass. Skipping the second
layout based only on unchanged outer dimensions would be unsafe.

The temporary area and stretch logs were removed after the check. They
produced tens of thousands of lines and severely distorted frame timing, so
their scroll fps is excluded from performance comparisons. The dependency is
visible in `.vm/bench/scroll-20260923-190821-reddit-grid-area-sizes/` and
`.vm/bench/scroll-20260923-191117-reddit-grid-stretch-values/`.

### TipTap selection canonicalization phases

The earlier TipTap sampler put `VisiblePosition::canonicalPosition` on 13.4%
of inclusive samples. A temporary Haiku timer separated its mandatory
`Document::updateLayoutIgnorePendingStylesheets()` call from the subsequent
upstream/downstream candidate search. In a 15-iteration TipTap-only run, the
first 160 recorded canonicalizations spent 656.6 ms in layout and only 1.35 ms
in candidate search. The suite averaged 153.5 ms including its cold pass
(`.vm/bench/speedometer-20260923-192145-tiptap-canonical-position-phases/`).

A three-iteration full-suite trace recorded 420 canonicalizations across all
suites. Their cumulative layout time was 270.7 ms and candidate search was
0.81 ms; almost all layout time arrived in two large bursts after calls 130
and 280. The full run's TipTap mean was 321.3 ms, but this aggregate trace
does not tag individual calls by suite, so it cannot assign those bursts or
the entire cross-suite regression to TipTap
(`.vm/bench/speedometer-20260923-192259-full-canonical-position-phase-trace/`).

Canonicalization's search is already cheap. The useful target is the forced
layout of the editor's flex and inline content after DOM changes, subject to
selection correctness. The diagnostic timer was removed and the production
WebProcess and NetworkProcess rebuilt from the committed engine source.

The production bundle's existing page-update trace confirms the layout shape
in five focused TipTap iterations. Each warm pass produced two slow
render-tree layouts of about 31--37 ms, nested in animation-frame callbacks
lasting about 59--72 ms. A `RenderFlexibleBox` contained a 16--21 ms
`RenderBlock` layout in each pass; the rest of the render-tree time is outside
that inclusive subtree. The cold pass had two roughly 100 ms layouts. These
samples point to repeated flex and inline layout of the editor view, not a
slow candidate-position walk
(`.vm/bench/speedometer-20260923-192710-tiptap-layout-renderers/`).

### Why TipTap repeats its nested flex layout

An opt-in phase trace split the outer one-item flex box's warm layout into
roughly 15 ms of base sizing and 16--22 ms of content sizing. Its nested
two-item flex box consumed nearly all of each phase; alignment and final rect
placement were under 0.1 ms. The first cold pass took about 100 ms per layout
(`.vm/bench/speedometer-20260923-193145-tiptap-flex-phase-trace/`).

The expensive final child layout reported an unchanged row main size,
percentage-height descendants, and a request to relayout. A narrow Haiku
trial suppressed that percentage-height request for row items, since their
unchanged inline-size override does not itself change block-size definiteness.
The five-step `flex-row-percent-height.html` fixture matched the opt-out
exactly for auto-height, definite-height, flex-start, and column controls
(`.vm/bench/probe-20260923-194213-summit-flex-row-percent-control/` and
`.vm/bench/probe-20260923-194244-summit-flex-row-percent-candidate/`).

The trial did not save a layout. In matched 20-iteration TipTap runs on the
same bundle, the opt-out averaged 146.5 ms (132.8 ms warm) and the candidate
147.1 ms (134.0 ms warm). The candidate's layout trace retained the same
16--22 ms nested flex calls. A final dirty-state trace found that the child
already had `needsLayout` on entry to final sizing, before either the
unchanged-width override or the percentage-height rule. Intrinsic-width
measurement in base sizing is the likely source of that invalidation, so
clearing only the percentage-height relayout request cannot avoid the final
pass. The ineffective engine change and all temporary traces were removed;
the workstation production engine was rebuilt clean
(`.vm/bench/speedometer-20260923-194320-tiptap-row-flex-control/`,
`.vm/bench/speedometer-20260923-194406-tiptap-row-flex-candidate/`,
`.vm/bench/speedometer-20260923-194502-tiptap-row-flex-candidate-layout/`, and
`.vm/bench/speedometer-20260923-194803-tiptap-flex-candidate-dirty/`).

A narrower cross-size trace ruled out one proposed cause. On each warm TipTap
pass, the nested flex item's intrinsic-width measurement entered its
cross-size scope clean and remained clean after both setting the temporary
size and invalidating cached content widths. Its prior block-size override
was present. The dirty state seen at final row sizing therefore does not
originate in that particular cache-invalidation call
(`.vm/bench/speedometer-20260923-195422-tiptap-flex-cross-size-invalidation/`).

A second trace around flex-base and min/max measurement found the hot row
child already dirty at the start of base sizing and still dirty after both
measurements. Its measured base was zero while its current content width was
790 px. This establishes why merely suppressing the percentage-height
request had no effect: base sizing has not completed a final layout for that
row child. A faster implementation would have to reuse intrinsic work while
still laying out real dirty content, or optimize that layout itself
(`.vm/bench/speedometer-20260923-195744-tiptap-flex-base-dirty/`).

### Live Reddit CPU paint-worker check

The production bundle was run through a live `/r/popular/` 180-notch A/B/A
sequence with two, four, then two Skia CPU painting threads. The measured
native-view scroll windows reached 55.54, 58.05, and 58.07 frames/s; their
worst intervals were 68.5, 61.9, and 47.6 ms, with maximum native queue
delays of 0.5, 1.0, and 0.1 ms. The harness captured two seconds in the first
two scroll windows and three seconds in the last, while the live feed and idle
load also varied. These runs do not establish a four-worker advantage, so the
two-worker production default remains unchanged. They do show that native
queueing is still short during these bursts
(`.vm/bench/scroll-20260923-200020-reddit-workers2-control/`,
`.vm/bench/scroll-20260923-200104-reddit-workers4-candidate/`, and
`.vm/bench/scroll-20260923-200144-reddit-workers2-repeat/`).

### Complete native-view burst snapshots

The prior `SUMMIT_UI_FRAME_STATS` log reported a window only when another
frame arrived. If Reddit stopped presenting near the end of a wheel burst,
`run-scroll.py` could cut its log before the ongoing gap was reported. The
browser's opt-in native view now keeps cumulative counts from the exact moment
the burst starts. `summitctl framestats` reads the count, elapsed time,
completed gaps, pending gap, and UI queue delay on the window thread. The
harness prefers that snapshot and also records one 1.1 seconds later. Older
bundles still use the periodic logs; that fallback completed on
`bundle-h0em65eq`
(`.vm/bench/scroll-20260923-201311-reddit-old-bundle-frame-fallback/`).

Two uninstrumented 300-notch `/r/popular/` passes on the snapshot bundle
delivered 147 frames in 5.21 seconds and 165 frames in 5.04 seconds: **28.2**
and **32.8 native-view frames/s** across the full bursts. At burst end they
had pending presentation gaps of 2.62 and 2.05 seconds, with UI queue delays
below 0.4 ms. In the second pass no new frame arrived during the following
1.1 seconds, so its pending gap grew to at least 3.28 seconds. Before/after
screenshots confirm that the feed moved. Earlier periodic-window scroll rates
can overstate long-burst smoothness when their reported windows cover only a
small part of the burst
(`.vm/bench/scroll-20260923-201123-reddit-frame-snapshot-check/` and
`.vm/bench/scroll-20260923-201529-reddit-frame-snapshot-uninstrumented-repeat/`).

An opt-in page trace during another 300-notch run saw one 1.71-second
document-scroll event dispatch, with repeated flex, grid, and block layouts
inside it. Its 15,000 renderer lines materially affect timing, so its frame
rate and 4.08-second completed gap are diagnostic rather than production
comparisons. The uninstrumented snapshots independently prove a multi-second
presentation stall above the native queue
(`.vm/bench/scroll-20260923-201421-reddit-frame-snapshot-page-phases/`).

`SUMMIT_PAGE_UPDATE_TRACE=2` keeps the page, scroll-dispatch, listener, and
layout-phase records but leaves the renderer and grid-item records disabled.
A 300-notch pass produced 82 browser log lines and caught one 1,582.0 ms
JavaScript document-scroll listener. Its containing rendering update took
1,867.3 ms, including 1,620.6 ms in scroll steps. The slow layout calls
included 225.3, 157.1, and 108.1 ms in their post-layout slices; nested
calls must not be summed as independent work. The native-view snapshot saw
118 frames in 5.19 seconds (22.7 frames/s), with a 2.81-second pending gap
and 0.1 ms maximum UI queue delay. On the same bundle with tracing off, a
separate live-feed pass saw 131 frames in 5.07 seconds (25.8 frames/s), a
2.77-second pending gap, and 0.1 ms queue delay. Feed variation prevents an
overhead estimate, but the long stall remains with tracing off
(`.vm/bench/scroll-20260923-202313-reddit-page-phases-low-volume/` and
`.vm/bench/scroll-20260923-202403-reddit-page-phases-off-control/`).

The post-layout trace is now split into `postDidLayout`, `postTasks`, and
`postOther`. Another low-volume pass caught a 1,521.3 ms document-scroll
listener and 115 frames in 5.19 seconds (22.2 frames/s), with a 3.12-second
pending presentation gap. Its three large outer post-layout slices were
222.2, 154.9, and 105.2 ms. The corresponding task slices were 217.8,
152.2, and 103.5 ms; each contained a synchronous follow-up render-tree
layout of 217.7, 152.0, and 103.4 ms. `didLayout` itself took only 4.4,
2.7, and 1.7 ms. The reported 490.3 ms total of slow post-layout slices is
therefore mostly nested layout, not an independent layer-position walk.
Avoiding that work requires understanding why the page repeatedly dirties
the render tree during scroll, while preserving its synchronous geometry
queries (`.vm/bench/scroll-20260923-202951-reddit-post-layout-split/`).

The trace now reports the first post-layout callback that changes the layout
state from clean to dirty. In two further 300-notch Reddit passes it found
four such transitions during the viewport section, then four specifically in
`updateLayoutViewport()`; `viewportContentsChanged()` did not dirty layout.
The latter pass delivered 123 frames in 5.07 seconds (24.3 frames/s) but had
a 2.90-second pending gap. `updateLayoutViewport()` can change the layout
viewport origin, which calls `setViewportConstrainedObjectsNeedLayout()` for
fixed or sticky content. This is the likely route to the observed follow-up
layouts, although the trace does not yet separate that call from visual
viewport notification. Skipping these layouts without a fixed-element
correctness check would risk visible position errors
(`.vm/bench/scroll-20260923-203515-reddit-post-task-dirty/` and
`.vm/bench/scroll-20260923-203856-reddit-viewport-dirty/`).

An opt-in trial skipped layout invalidation when the layout viewport origin
changed. The new `viewport-origin-fixed.html` probe recorded exactly the same
fixed and sticky rectangles at five scroll positions in control and trial,
and final screenshots differed only in a small bottom-right desktop region.
On live Reddit, traced A/B/A 300-notch passes reduced the slow document-scroll
dispatch from 1,223.7 ms in the control to 857.3 and 896.1 ms in the two
trial runs; slow post-layout slices fell from 349.9 ms to 12.9 and 15.6 ms.
However full-burst native-view rates were 26.4 frames/s in the control versus
22.4 and 22.6 in the trial, and all three retained 2.7--3.0 second gaps.
Live-feed variation limits that comparison. The trial did not demonstrate
smoother scrolling, and the fixture covers only a small fixed/sticky case, so
the production layout invalidation remains in place
(`.vm/bench/probe-20260923-204405-summit-viewport-origin-control/`,
`.vm/bench/probe-20260923-204718-summit-viewport-origin-candidate/`,
`.vm/bench/scroll-20260923-204754-reddit-viewport-origin-candidate-trace/`,
`.vm/bench/scroll-20260923-204841-reddit-viewport-origin-control-trace/`, and
`.vm/bench/scroll-20260923-204926-reddit-viewport-origin-candidate-repeat/`).

Media remains verified for the finite Reddit CMAF case: the current bundle
loaded its HLS metadata in 468 ms, played 720×1280 video to `ended` at 12.7
seconds with no media error, and selected NVDEC H.264 plus AAC. A live feed
trace also selected those decoders with status 0, though Media Kit emitted a
stream-2 decoder warning; decoder selection alone does not prove that every
feed post completes playback
(`.vm/bench/probe-20260923-200527-summit-reddit-current-direct-hls-media-check/`
and `.vm/bench/scroll-20260923-200427-reddit-live-media-codec-lifecycle/`).

### Advertise the finite Reddit HLS playback path

The Haiku media backend could already play a direct finite Reddit HLS URL,
but `video.canPlayType("application/vnd.apple.mpegurl")` and the alternate
`application/x-mpegURL` both returned empty. A typed `<source>` fixture
therefore skipped a working Reddit HLS playlist and selected an unusable MP4
fallback. The baseline stayed at time zero with no video dimensions
(`.vm/bench/probe-20260923-205355-summit-reddit-media-capabilities/` and
`.vm/bench/probe-20260923-205459-summit-hls-source-selection-control/`).

The Haiku engine now includes both HLS MIME types in its supported list and
returns `maybe` for them. `Maybe` is deliberate because the implementation
handles finite, unencrypted, same-resource byte-range CMAF playlists rather
than every HLS form. The candidate selected the typed HLS source, loaded
metadata in 480 ms, and reached `ended` at 12.7 seconds with no media error;
the chosen decoders were NVDEC H.264 and AAC. `MediaSource` remains unavailable
(`.vm/bench/probe-20260923-205751-summit-hls-capabilities-candidate/` and
`.vm/bench/probe-20260923-205821-summit-hls-source-selection-candidate/`).

A live `/r/popular/` 180-notch pass on the candidate selected NVDEC H.264
and resolved separate HLS video and AAC audio twice, with no logged media
lifecycle stall over 10 ms. It still delivered only 36.15 native-view
frames/s during its 3.29-second burst and had a 1.15-second pending frame
gap. This verifies live feed decoder selection, not completion of every feed
post; the scrolling bottleneck remains
(`.vm/bench/scroll-20260923-205917-reddit-hls-capability-live/`).

### Matched-viewport Speedometer and timer investigation

On the current HLS-capable bundle, a fresh full-screen, 1913×945 content-view
Speedometer run scored 6.591 ± 0.240. The saved Firefox 155 baseline used a
1280×887 content view, so Summit was rerun at exactly that size and scored
6.779 ± 0.258 against Firefox's 8.338 ± 0.374. TipTap took 282.8 ms versus
Firefox's 124.8 ms; Preact and Svelte took 70.6 and 64.1 ms versus 43.4 and
39.6 ms. `compare-runs.py` now warns if the two content viewports differ
(`.vm/bench/speedometer-20260923-210142-hls-capability-current/` and
`.vm/bench/speedometer-20260923-210422-matched-firefox-viewport/`).

An A/B/A 20-iteration Preact/Svelte/Lit check at the matched viewport scored
14.828 with display-rate compositor pacing, 14.281 with its cap disabled,
then 13.747 after restoring the cap. The downward drift makes the pacing
comparison inconclusive; no compositor setting changed
(`.vm/bench/speedometer-20260923-210831-dom-async-pacing-control/`,
`.vm/bench/speedometer-20260923-210941-dom-async-pacing-uncapped/`, and
`.vm/bench/speedometer-20260923-211053-dom-async-pacing-repeat/`).

The platform probe's 50 forced layouts over 2,000 inline blocks took 1,966 ms
in Summit versus 361 ms in Firefox, and nested zero-delay timers had an 8 ms
versus 4 ms median. The existing inline-reuse coverage trace in a 20-pass
isolated TipTap benchmark found zero eligible inline boxes: its hot layout is
flex content, so extending that guarded inline shortcut would miss TipTap
(`.vm/bench/probe-20260923-211312-summit-current-async-window/`,
`.vm/bench/probe-20260923-211353-firefox-current-async-window-firefox/`, and
`.vm/bench/speedometer-20260923-211557-tiptap-inline-coverage/`).
The Preact/Svelte/Lit focused run likewise found only 18 eligible fixed inline
boxes across 3,597 renderer visits; 2,970 were non-box renderers and 609
were dirty. The large synthetic inline-layout gap is real, but extending the
current clean-box shortcut has too little coverage to close these suite gaps
(`.vm/bench/speedometer-20260923-214643-dom-inline-coverage/`).

A temporary run-loop trace showed that the main shared timer's 8 ms samples
were mostly deadlines armed at about 7.87 ms; median delivery lateness was
roughly 0.06 ms. Raising the timer thread's priority did not change the
8 ms nested-timer median. An opt-in trial removed only the extra 4 ms
alignment for visible, maximally nested Haiku timers while retaining WebKit's
4 ms minimum and hidden-page throttling. It moved the nested-timer median to
4 ms and the shared-timer deadline to about 3.97 ms. The benchmark's small
asynchronous-window probe was unchanged, however. Full matched-viewport
Speedometer A/B/A runs scored 6.513, 6.447, and 6.676, with TipTap around
287–289 ms in all three. The change did not improve the requested benchmark;
the trial and trace were removed and the production workstation engine was
rebuilt with the committed timer behavior
(`.vm/bench/probe-20260923-211857-summit-timer-priority-control/`,
`.vm/bench/probe-20260923-211936-summit-timer-priority-candidate/`,
`.vm/bench/probe-20260923-212957-summit-named-timer-latency/`,
`.vm/bench/probe-20260923-213454-summit-timer-no-alignment-probe/`,
`.vm/bench/speedometer-20260923-*-no-alignment-*/`).

### Skia GL tile painting trial on the GTX 1070

The current Haiku coordinated renderer composites through Mesa Zink/NVK, but
`PlatformDisplay::skiaGLContext()` returns null on Haiku. That keeps Skia's
page tiles on its two CPU painting threads. A temporary opt-in trial allowed
Haiku to create Skia GL contexts; an engine diagnostic confirmed `GPU` tile
painting in the WebProcess. The trial was built and tested, then removed.

At the 1913×945 content viewport, the 400-card, 300-frame scroll probe ran at
59.25 fps with GPU tile painting and 58.96 fps with CPU painting. Both had no
frame over 33 ms, so this probe showed no useful gain
(`.vm/bench/probe-20260923-215242-summit-skia-gl-trial/` and
`.vm/bench/probe-20260923-215321-summit-skia-cpu-control/`).

On the live `/r/popular/` 300-notch burst, GPU painting delivered 19.61
native-view frames/s with a 3.35-second pending presentation gap, versus
23.21 frames/s and a 3.01-second gap with CPU painting. Reddit load varied
even before the bursts, so these two runs do not establish a precise
regression, but they do not support enabling GPU tile painting. Mesa logged
`ZINK: vkCreateImage failed (VK_ERROR_UNKNOWN)` in the GPU run, but not the
CPU control. Both runs visibly scrolled
(`.vm/bench/scroll-20260923-215402-reddit-skia-gl/` and
`.vm/bench/scroll-20260923-215446-reddit-skia-cpu/`).

At the matched 1280×887 Speedometer viewport, GPU tile painting scored
6.888 ± 0.280 over ten uncontended iterations, compared with the existing
CPU-painted 6.779 ± 0.258. The confidence ranges overlap; TipTap remained
278.1 ms versus the CPU baseline's 282.8 ms. No Speedometer improvement is
established. The explicit diagnostic run logged `GPU` from the
`SkiaPaintingEngine` constructor, verifying the tested path
(`.vm/bench/speedometer-20260923-215530-skia-gl-trial/` and
`.vm/bench/probe-20260923-215908-summit-skia-gl-diagnostic/`).

The workstation engine was rebuilt with the original CPU tile path after the
trial. Vulkan through Zink is already exercised for compositing; a native
Skia Vulkan backend would require separate buffer sharing and synchronization
work rather than a backend switch alone.

### Reject media containers with no usable tracks

The Haiku media loader treated a recognized container as successfully loaded
even when Media Kit could not decode any of its tracks. A 775-byte MP4 with
only a `mov_text` subtitle track reproduced this: Media Kit rejected stream
zero, while the page emitted `loadedmetadata`, `canplay`, and `playing` with
zero dimensions and time stuck at zero. A following Reddit HLS `<source>` was
never tried (`.vm/bench/probe-20260923-220915-summit-unsupported-track-control/`
and `.vm/bench/probe-20260923-221045-summit-unsupported-track-fallback-control/`).

The loader now requires at least one decoded video or audio track before it
reports `Loaded`. With the same source list, Summit skipped the unsupported
MP4, selected Reddit's HLS playlist, decoded 381 H.264 frames through NVDEC
with AAC audio, and reached `ended` at 12.7 seconds without a media error.
The fixture is `tools/bench/pages/unsupported-track.mp4`, generated from a
single subtitle cue with `ffmpeg -f srt -i unsupported-track.srt -c:s mov_text`;
the browser probe is `tools/bench/pages/media-track-fallback.html`
(`.vm/bench/probe-20260923-221321-summit-unsupported-track-fallback-candidate/`).
The final rebuilt bundle repeated that `ended` result with the diagnostic
trace disabled (`.vm/bench/probe-20260923-221823-summit-unsupported-track-fallback-final/`).

The new opt-in `SUMMIT_MEDIA_PLAYBACK_TRACE=1` records each player's usable
track count, first decoded video frame, five-second progress, and decode end.
It is disabled by default. A direct Reddit HLS control reached its 12.7-second
`ended` event with 381 decoded NVDEC frames. Live `/r/popular/` passes before
and after the track fix each loaded NVDEC video plus AAC for HLS posts and
decoded a visible video past five seconds. No loaded container without usable
tracks appeared in those two feed samples, so the fix is verified for the
reproduced source-fallback failure, not as an explanation for all Reddit
posts. Scrolling still had 2.78- and 2.97-second presentation gaps in those
passes (`.vm/bench/probe-20260923-220638-summit-playback-trace-hls/`,
`.vm/bench/scroll-20260923-220732-reddit-playback-trace/`, and
`.vm/bench/scroll-20260923-221449-reddit-playback-track-fix/`).

### Async wheel scrolling and coordinated composition

The workstation build enables `ENABLE_ASYNC_SCROLLING`, Haiku's scrolling
thread, and Summit's threaded-scrolling preference. A 300-notch live Reddit
trace found a scrolling tree for every event and routed all 300 to that
thread with no blocking DOM-dispatch step. The expanded result trace showed
132 events handled there and 168 unhandled, with none requesting main-thread
wheel processing. The burst still delivered 26.77 native-view frames/s and
had a 2.76-second pending presentation gap with a 0.6 ms maximum UI queue
delay (`.vm/bench/scroll-20260923-223047-reddit-wheel-result/`).

An opt-in compositor timing trace measured 367 composed frames in a separate
run. Its median frame time was 4.7 ms, p95 was 9.9 ms, and the maximum was
108.6 ms; there was only one explicit async-scrolling composition request.
That run's longest completed-frame gap was 0.81 seconds and its burst snapshot
had a 0.81-second pending gap, while other live passes still had multi-second
gaps. The timing trace therefore points to missing or delayed composition
requests and page content production more than cost inside an ordinary
compositor frame (`.vm/bench/scroll-20260923-222641-reddit-compositor-timing/`).

Haiku does not define `HAVE_DISPLAY_LINK`; WebKit normally uses its display
refresh callback to advance `ThreadedScrollingTree` layer positions. A
temporary opt-in trial pulsed that tree after handled wheel events. It raised
explicit async-scrolling composition requests from one to 136 in the traced
300-notch pass. In an uninstrumented A/B/A sequence on the same bundle,
native-view rates were 23.32, 48.50, and 21.45 frames/s, with pending gaps
of 3.05, 0.94, and 2.85 seconds. However, both trial screenshots showed a
blank Reddit feed at its footer where the controls showed posts. The trial
may have reached the end of currently loaded posts before Reddit supplied
more; the captures do not distinguish lazy loading from missing paint. The
pulse was removed. A future refresh source needs a content-visible result
under sustained scrolling before enabling it by default
(`.vm/bench/scroll-20260923-223705-reddit-wheel-refresh-candidate/`,
`.vm/bench/scroll-20260923-223819-reddit-refresh-control/`,
`.vm/bench/scroll-20260923-223905-reddit-refresh-candidate/`, and
`.vm/bench/scroll-20260923-223955-reddit-refresh-control-repeat/`).

A shorter 80-notch follow-up rendered feed posts with and without the pulse.
The trial yielded 48.28 frames/s versus 54.85 for the safe build, both with
no substantial pending presentation gap. This pass does not establish a
benefit for ordinary-length bursts. An initial follow-up run was invalid:
the private Mesa path was omitted and the WebProcess exited before scrolling.
The two reported runs both used `SUMMIT_LIBRARY_PATH_PREFIX` to select the
workstation's Mesa (`.vm/bench/scroll-20260923-224807-reddit-refresh-80-valid/`
and `.vm/bench/scroll-20260923-224905-reddit-control-80/`).

`SUMMIT_SCROLL_POSITION_TRACE=1` now records the main-frame scrolling tree's
position, current maximum, content height, and wheel handling result under
the tree lock. `run-scroll.py` summarizes the first event at the current
maximum, subsequent content-size changes, and the first event at the final
maximum. This distinguishes a long pause while a finite feed is at its
boundary from a scrolling tree that is still advancing; frame rate alone
cannot make that distinction.

In a 300-notch safe-build run, the first 3,038-px Reddit document reached its
scroll maximum on event 21. The document expanded to 17,736 px on event 126,
and the tree reached its new maximum on event 248. Overall 158 of 300 wheel
events found the tree at its then-current maximum, 144 were handled, and the
native view delivered 24.47 frames/s with a 2.94-second pending gap. The final
image still showed a post rather than the new maximum; this confirms that
scrolling-tree position and visible layer position can diverge without display
refresh pulses (`.vm/bench/scroll-20260923-225629-reddit-position-300/`).

An additional 180-notch pair illustrates the unresolved tradeoff. With refresh
pulses, the native view delivered 54.85 frames/s but the final feed was blank.
Without pulses, it delivered 34.97 frames/s and showed a post. In that safe
run the first maximum arrived at event 21, content expanded at event 131, and
the tree advanced to 8,488 px by event 180. Live content differed between the
two passes, so their frame rates do not isolate the pulse's cost. The pulse
remains disabled until scrolling can advance visibly without leaving a blank
feed (`.vm/bench/scroll-20260923-225828-reddit-refresh-180/` and
`.vm/bench/scroll-20260923-225929-reddit-safe-180/`).

### Periodic Haiku scrolling refresh trial (September 24)

The current text-width build still produced a 1.48-second pending frame gap in
a 300-notch `/r/popular/` burst. Its scrolling tree advanced to 16,256 px of
a 16,652-px maximum while the final image showed an earlier post. A Haiku-only
trial calls `ThreadedScrollingTree::displayDidRefresh()` on the event queue at
`SUMMIT_SCROLL_REFRESH_TIMER=16` (or `=1`) during recent wheel activity; `=33`
tests a slower cadence. The timer stops when the trees become inactive and is
**disabled by default**. Haiku has no system display-link callback for this
path.

On one trial bundle, a 300-notch timer run delivered 55.96 native-view
frames/s with a 10.3 ms pending gap, versus 36.29 frames/s and a 1.42-second
gap with the timer off. The timer screenshot showed a post and Reddit's loading
spinner at the then-current scroll maximum. An 180-notch pair delivered 54.45
versus 47.51 frames/s, with visible feed posts in the timer screenshot. These
passes establish that missing refreshes account for much of the long frame
gap (`.vm/bench/scroll-20260924-071341-width-cache-reddit-long/`,
`.vm/bench/scroll-20260924-072114-reddit-refresh-timer-candidate/`,
`.vm/bench/scroll-20260924-072229-reddit-refresh-timer-control/`,
`.vm/bench/scroll-20260924-072340-reddit-refresh-timer-180-candidate/`, and
`.vm/bench/scroll-20260924-072422-reddit-refresh-timer-180-control/`).

The timer is not ready for general use. A packaged default-on 16 ms run
delivered 54.02 frames/s but ended with a blank feed at 13,084 px, still 2,337
px short of the current document maximum. A 33 ms trial also ended blank and
delivered only 37.02 frames/s with an 826 ms gap. A later 16 ms run reproduced
the blank feed. Opt-in `SUMMIT_COMPOSITOR_TIMING_TRACE=1` now includes the
scene's pending-tile count: 69 of 70 async composition requests in that blank
run reported zero pending tiles. The count does not prove the page had painted
content at the newly exposed position; it rules out a simple wait on the
compositor's counted tile jobs as the full explanation. Follow-up work needs
to distinguish Reddit's content production from tile invalidation and
composition at the tree's current position
(`.vm/bench/scroll-20260924-073417-reddit-refresh-default-300/`,
`.vm/bench/scroll-20260924-073758-reddit-refresh-33ms-300/`, and
`.vm/bench/scroll-20260924-074350-reddit-refresh-pending-tiles/`).

Matched-viewport ten-iteration Speedometer scores with the opt-in timer were
6.214 and 6.967, bracketing a 6.727 timer-off control. A complete traced
Speedometer iteration routed zero wheel events, so the refresh timer was not
started by that benchmark; the score spread should not be attributed to the
timer without further evidence
(`.vm/bench/speedometer-20260924-072513-scroll-refresh-timer-speedometer-candidate/`,
`.vm/bench/speedometer-20260924-072705-scroll-refresh-timer-speedometer-control/`,
`.vm/bench/speedometer-20260924-072906-scroll-refresh-timer-speedometer-candidate-repeat/`,
and `.vm/bench/speedometer-20260924-073108-scroll-refresh-timer-wheel-count/`).

The packaged diagnostic build, with the timer left off, completed an
80-notch Reddit smoke at 56.54 native-view frames/s with a 5.9 ms pending gap;
its final screenshot showed rendered posts
(`.vm/bench/scroll-20260924-074549-reddit-refresh-diagnostic-safe/`).

An opt-in `SUMMIT_SCROLL_DOM_TRACE=1` probe now hit-tests two points in the
WebCore viewport when a timed scroll burst stops. It logs the element ancestry
and bounds, without page text. During a reverse 80-notch burst after a 300-notch
forward burst, the final screenshot showed a blank Reddit feed at scrolling-tree
position 4,674 px (maximum 26,630 px). The corresponding DOM trace found an
image inside `SHREDDIT-POST` at viewport y=200 and a link inside another
`SHREDDIT-POST` at y=400, with rectangles covering those points. A later
screenshot, without additional input, showed the post image at that position.
Thus Reddit had produced the post elements while the feed was blank; this is
strong evidence of delayed painting or layer presentation after asynchronous
scrolling, rather than an empty DOM. The trace and screenshot are close in time
but are not atomic. The next investigation should compare the committed layer
state and tile damage with the scrolling tree's position
(`.vm/bench/scroll-20260924-080715-reddit-refresh-dom-reverse/`).

The follow-up ruled out two direct flush requests. An opt-in final rendering
update after wheel activity ended still produced a blank reverse-scroll feed on
repeat (`.vm/bench/scroll-20260924-081518-reddit-final-render-update-repeat/`).
Scheduling a flush when `GraphicsLayerCoordinated::syncPosition()` dirtied tile
coverage also failed on repeat and had no consistent frame-pacing gain
(`.vm/bench/scroll-20260924-082030-reddit-coverage-flush-repeat/`). Both
experiments were removed.

Opt-in `SUMMIT_TILE_COVERAGE_TRACE=1` logs the visible and cover rectangles,
tile count, and creation state for large coordinated backing stores.
`SUMMIT_LAYER_UPDATE_TRACE=1` logs scroll-update notification delivery and
the layer host's schedule, render, and composite handoff. A blank capture at
scrolling-tree y=4,474 had the large content layer's most recent visible
rectangle at y=14,074. During the reverse burst the compositor kept presenting
asynchronous frames, but no main-thread rendering update or new tile coverage
reached the log before the screenshot. The UI frame trace recorded a 648 ms
queue delay. Three seconds later, the queued scroll notifications ran, the
layer's visible rectangle changed to y=4,474, and a settled screenshot showed
the post image. This identifies delayed main-thread scroll reconciliation and
backing-store coverage as the immediate source of that blank interval. The
next trial should reduce or coalesce redundant main-thread scroll-update
notifications while preserving wheel-event test deferrals
(`.vm/bench/scroll-20260924-083224-reddit-layer-handoff/`).
An opt-in notification-coalescing trial kept those deferrals but reduced
forward-burst main-thread notifications from 115 to 13 in same-bundle runs.
The 300-notch candidate and control delivered 47.02 and 47.69 native-view
frames/s, with 699 and 726 ms pending gaps respectively. Both reverse-scroll
screenshots still contained blank feed regions. Fewer notifications did not
make the renderer catch up, so the trial was removed
(`.vm/bench/scroll-20260924-084114-reddit-notification-coalesce/` and
`.vm/bench/scroll-20260924-084235-reddit-notification-control/`).
With both new traces disabled and the refresh timer left off, the same bundle
completed an 80-notch Reddit smoke at 56.02 native-view frames/s with a 9.5 ms
pending gap and visible feed posts; it emitted no layer or tile trace lines
(`.vm/bench/scroll-20260924-083607-reddit-layer-diagnostics-safe/`).

### TipTap intrinsic-width rebuilding

An opt-in `SUMMIT_FLEX_WIDTH_TRACE=1` probe narrowed TipTap's flex sizing
cost. In eight isolated iterations, 17 `min-content` contributions exceeded
0.5 ms and totaled about 404 ms. The two cold calls took roughly 98 and 88
ms; warm calls on the editor's flex item took about 14--16 ms. Every slow
call entered with invalid intrinsic widths and a dirty renderer, then returned
the same 598.8-px minimum width. Equality of the answer does not make the
cached value reusable after the editor's content changes. The nearby explicit
max-content and table min-content helpers produced no calls over 0.5 ms; the
cost is in `computeMainAxisExtentForFlexItem`'s ordinary minimum-size branch
(`.vm/bench/speedometer-20260923-230957-tiptap-minimum-width-trace/` and
`.vm/bench/speedometer-20260923-231515-tiptap-intrinsic-phase-trace/`).

`SUMMIT_INTRINSIC_WIDTH_TRACE=1` split nested inline width work. Among 119
`minimumMaximumContentSize` calls over 0.5 ms in the same eight-iteration
run, 130.6 of 131.6 ms was rebuilding inline-item lists, including lists of
over 3,000 items. These are inclusive nested timings with a reporting
threshold, so they cannot be subtracted from the outer flex call total. The
119 slow records identified 118 distinct inline roots; most expensive lists
were fresh, which also limits the value of reserving from a previous list.

A guarded trial reserved a fresh inline-item vector from the previous list's
length, capped at 4,096 entries. In same-bundle, uncontended 20-iteration
TipTap control/candidate/control runs, suite means were 148.2, 150.4, and
147.6 ms. The hint did not improve the editor and was removed. Future work
needs to reduce per-item rebuilding or reuse unaffected inline content while
preserving edits, instead of changing vector capacity
(`.vm/bench/speedometer-20260923-231939-tiptap-inline-reserve-control/`,
`.vm/bench/speedometer-20260923-232026-tiptap-inline-reserve-candidate/`,
and `.vm/bench/speedometer-20260923-232114-tiptap-inline-reserve-control-repeat/`).

### Reusing measured text widths

The archived TipTap profile assigned 8.65% of samples to rebuilding text items
from cached breaking positions. Disabling that cache increased isolated
20-iteration TipTap time from 149.8 to 161.6 ms; a repeated cached control
was 149.7 ms. The break-position cache is useful even though most expensive
inline roots are fresh
(`.vm/bench/speedometer-20260923-232953-tiptap-break-cache-control/`,
`.vm/bench/speedometer-20260923-233041-tiptap-break-cache-disabled/`, and
`.vm/bench/speedometer-20260923-233131-tiptap-break-cache-control-repeat/`).

`SUMMIT_TEXT_BREAK_STATS=1` showed 9,600 hits in 10,835 TipTap lookups, but
zero width-cache hits. All 9,600 hit text boxes lacked the simplified
measuring flag, while none had glyph overflow, content adjustment, letter or
word spacing, first-line font mismatch, or position-dependent content. The
Haiku width-cache rule now also accepts simple-font-code-path text when these
conditions hold, the text is not a synthesized glyph or combined text, and
it has no bidi override. Existing font-generation invalidation and the
breaking-position cache's text, context, origin, and font-cascade keys remain
in force. `SUMMIT_VERIFY_TEXT_WIDTH_CACHE=1` remeasures each cache hit and
reports mismatches; an eight-iteration TipTap run reused widths on 3,454 of
3,700 break-cache hits with no mismatch. A three-iteration full Speedometer
run also reported no mismatch across all suites
(`.vm/bench/speedometer-20260923-234457-tiptap-text-width-verify/` and
`.vm/bench/speedometer-20260923-234824-full-text-width-verify/`).

On the same trial bundle, uncontended isolated TipTap control/candidate/control
means were 147.3, 130.8, and 153.4 ms over 20 iterations each. Full
Speedometer at the Firefox-matched 1280×887 content viewport scored
5.891 ± 0.172 with the existing rule and 6.787 ± 0.286 with width reuse,
both over ten iterations on that bundle. The clean default bundle scored
6.923 ± 0.333 in ten iterations; it remains below Firefox's 8.338 ± 0.374.
Its TipTap mean was 148.2 ms versus 307.0 ms in the same-bundle control.
The isolated result is a more direct measure of the editor's cache benefit;
the full runs also reflect interactions among suites
(`.vm/bench/speedometer-20260923-234550-tiptap-text-width-control/`,
`.vm/bench/speedometer-20260923-234637-tiptap-text-width-candidate/`,
`.vm/bench/speedometer-20260923-234725-tiptap-text-width-control-repeat/`,
`.vm/bench/speedometer-20260923-235421-text-width-full-control-matched/`,
`.vm/bench/speedometer-20260923-235613-text-width-full-candidate-matched/`,
and `.vm/bench/speedometer-20260923-235224-text-width-default-matched/`).

On the packaged default bundle, a live `/r/popular/` 80-notch smoke delivered
53.68 native-view frames/s over 1.34 seconds, with a 16.5 ms pending frame
gap. Before and after screenshots show different, rendered posts. This short
burst does not establish sustained Reddit scrolling performance. The typed
Reddit HLS fixture loaded 720×1280 metadata, played to `ended` at 12.7 seconds
without a media error, and selected NVDEC H.264 plus AAC with status 0. The
decoder still logged its usual last-buffer warning at end of stream
(`.vm/bench/scroll-20260923-235918-text-width-reddit-smoke/` and
`.vm/bench/probe-20260924-000155-summit-text-width-hls-codec-smoke/`).

On the current September 24 diagnostic bundle, a fresh live `/r/popular/`
pass selected NVDEC H.264 for six videos. An HLS player with separate video
and audio decoded its first frame, but the scrolling pass moved past it before
five seconds of playback. A second 60-notch pass again selected NVDEC for six
live videos; its visible endpoint was an image post. These feed samples show
successful decoder selection and at least one decoded frame, not sustained
playback of a stationary Reddit post
(`.vm/bench/scroll-20260924-085039-reddit-live-media-current/` and
`.vm/bench/scroll-20260924-085150-reddit-live-video-stationary/`).
The same bundle completed the typed Reddit HLS fixture at 720×1280 through
its 12.7-second `ended` event with no media error. The backend decoded 381
frames using NVDEC H.264 plus AAC and reported five- and ten-second progress
(`.vm/bench/probe-20260924-085410-summit-reddit-hls-current/`).

The current diagnostic bundle also completed a fresh, uncontended ten-iteration
Speedometer 3.1 run at the Firefox-matched 1280×887 content viewport. It scored
6.760 ± 0.286 against the saved Firefox 155 result of 8.338 ± 0.374. Svelte,
CodeMirror, and Preact took 1.68×, 1.66×, and 1.66× Firefox's time. Preact's
`Adding100Items` and `CompletingAllItems` async steps took 30.0 and 24.5 ms
versus Firefox's 16.9 and 13.0 ms; Svelte's took 27.2 and 21.0 ms versus
14.4 and 10.4 ms. CodeMirror's `Long` step took 26.2 ms sync and 22.7 ms
async, versus 15.5 and 11.1 ms. This identifies the steps to investigate;
it does not by itself assign the delay to the frame scheduler, JavaScript,
or layout (`.vm/bench/speedometer-20260924-085646-current-diagnostics-matched/`).

A 20-iteration focused run of those three suites with
`SUMMIT_PAGE_UPDATE_TRACE=2` scored 15.080 ± 0.520 and recorded 22 page
updates over 33 ms. Their 850.9 ms of traced time included 519.1 ms inside
animation-frame callbacks and 293.1 ms in intersection observers; 40 layouts
crossed the separate 10 ms layout-trace threshold. These are aggregate page
logs, without a suite identifier, and the trace omits shorter updates. They
are useful leads but cannot establish which part of each measured async step
is spent in a callback or waiting for the next frame
(`.vm/bench/speedometer-20260924-085957-focused-page-update-trace/`).

Suite-only traced repeats clarified that attribution. Preact and Svelte each
had just one page update over 33 ms across 20 iterations; each also had 20
roughly 72 ms layouts, longer than a whole scored iteration and therefore
apparently in suite preparation outside its measured steps. CodeMirror had
21 slow page updates across 20 iterations, totaling 844.1 ms, including
519.7 ms in animation-frame callbacks and 302.6 ms in intersection-observer
processing. No CodeMirror layout crossed the 10 ms trace threshold. This
supports investigating CodeMirror's frame callback and observer work before
altering global frame pacing. The current opt-in `SUMMIT_PAGE_UPDATE_TRACE=2`
additionally splits intersection-observer update and notification time to
identify which part is expensive
(`.vm/bench/speedometer-20260924-090259-preact-page-update-trace/`,
`.vm/bench/speedometer-20260924-090531-svelte-page-update-trace/`, and
`.vm/bench/speedometer-20260924-090359-codemirror-page-update-trace/`).
For the next isolated CodeMirror diagnostic, `run-speedometer.py
--suites Editor-CodeMirror --intersection-trace` serves an instrumented iframe
that records JavaScript IntersectionObserver callback duration at unload in
`progress.jsonl`. The source benchmark checkout stays unchanged and the
result is marked instrumented. Comparing those callback times with the
native observer-phase trace should separate JavaScript notification work
from geometry updates before changing WebCore's observer path.

The first instrumented 20-iteration CodeMirror run uploaded one iframe record
per iteration. Its 60 constructed observers made 60 callbacks in total; the
callbacks accumulated 304 ms, while the browser's page-update trace recorded
305.0 ms in the intersection-observation phase. The third constructed
observer contributed almost all callback time, usually 14–15 ms per
iteration (22 ms at most). The two totals are independently measured and
close enough to make JavaScript callback work the leading explanation for
CodeMirror's observer-phase cost; optimizing native observer geometry alone
is unlikely to close this gap. The instrumented suite scored 13.471 ± 0.780,
similar to the earlier traced CodeMirror-only 13.447 ± 0.760, but instrumented
scores are diagnostic rather than benchmark baselines
(`.vm/bench/speedometer-20260924-092520-codemirror-observer-callbacks/`).
The CodeMirror bundle has observers for tooltips, the editor content, and
virtualized document gaps. Its gap-observer callback calls `onScrollChanged()`,
which can synchronously call `view.measure()`. The slow third observer is
consistent with that path, but the existing run did not label targets. The
opt-in trace now records a few observed target tags and classes so a follow-up
can establish that mapping before optimizing the measurement path.

### Reddit HLS seek range regression

On the current workstation bundle, `tools/bench/pages/media-seek.html` loaded a
12.7-second Reddit HLS clip but exposed an empty `video.seekable` range at
metadata, then `[0, currentTime]` while playing. Setting `currentTime = 8` at
metadata briefly reported 8 seconds, but playback started from zero
(`.vm/bench/probe-20260924-093312-summit-reddit-hls-seek-control/`).
WebCore's seek task aborts when `seekable` is empty. The Haiku backend reported
`maxTimeSeekable()` as `currentTime()` even though its finite HLS source has
been downloaded and its duration is known. Report the duration as the end of
the seekable range, as the other finite-file backends do. The first rebuilt
bundle exposed `[0, 12.7]` and fired `seeked`, but Media Kit returned the
preceding keyframe at 5.97 seconds for an 8-second seek
(`.vm/bench/probe-20260924-*/` with label `reddit-hls-seek-fixed`).

Keep the requested audio and video seek times separate, then decode video from
its keyframe to the requested time. The final workstation bundle
`bundle-1soo54tz` reached 8.0 seconds after decoding 61 preroll frames from
the 6-second keyframe, reached `ended` at 12.7 seconds, and selected NVDEC
H.264 plus AAC (`.vm/bench/probe-20260924-*/` with label
`reddit-hls-seek-preroll`). The test sets `currentTime` through HTMLMediaElement; the user's
original Adobe ad and its pointer interaction were not available to retest.

### Guardian article video check

The inline self-hosted clip on the reported Guardian article is 480×384 H.264
and AAC at 25 fps, 19.56 seconds long. The standalone page
`tools/bench/pages/media-guardian-loop.html` completed on both the previous
bundle and `bundle-1soo54tz`: each decoded 489 frames through NVDEC and had
no playback-clock stalls before the end. The new bundle releases the media
lock before the video thread waits for its next frame, so audio callbacks and
seeks no longer queue behind that intentional wait. This check does not prove
that every frame was presented smoothly.

On the full article, an idle five-second window with the inline video visible
reported 59.6 native-view frames/s with no interval over 33 ms. A cookie
consent layer covered the video in the screenshot, so this run cannot establish
visual playback quality (`.vm/bench/scroll-20260924-100927-guardian-article-video/`).
The standalone clip also stopped at 19.56 seconds despite its `loop` attribute.
The Haiku backend discarded its media tracks at end-of-stream, leaving no
tracks for WebCore's loop seek. Keep the tracks and mark each ended instead.
In `bundle-7dosgek7`, the clip reached 19.56 seconds, fired `seeked`, and
continued playing from zero without pausing; NVDEC remained selected
(`.vm/bench/probe-20260924-101812-summit-guardian-loop-candidate/`). The
same bundle still sought a non-looping Reddit HLS clip to exactly 8 seconds
and fired `ended` at 12.7 seconds
(`.vm/bench/probe-20260924-101909-summit-reddit-seek-loop-candidate/`).
The X399 Desktop launcher was initially moved to the tested `bundle-7dosgek7`.

### CodeMirror observer measurement comparison

The opt-in CodeMirror callback trace identified the third constructed
IntersectionObserver as the virtualized gap observer: its construction stack
points to CodeMirror's `DOMObserver` at asset line 8919. The tooltip observer
had no targets, the editor intersection observer had one fast callback per
iteration, and the gap observer ran twice per iteration. At the matched
1280×887 viewport, the gap callbacks accumulated 180 ms across ten Summit
iterations. A Firefox 155 run of the same instrumented suite and viewport
recorded 15 gap callbacks totaling 1 ms. These are diagnostic timings, not
standard Speedometer scores (`.vm/bench/speedometer-20260924-103121-codemirror-measure-matched/`
and `.vm/bench/firefox-speedometer-20260924-102344-codemirror-targets/`).
The construction stacks are in
`.vm/bench/speedometer-20260924-103457-codemirror-observer-origin/`.

Within Summit's 180 ms, `view.measure()` accounted for 179 ms. Instrumented
stages summed to 28 ms in `viewState.measure`, 10 ms in measurement reads,
20 ms in plugin updates, and 20 ms in document-view updates. A repeat that
also timed measurement writes found only 2 ms in those writes, with 198 ms
total callback time. The remaining time is spread through the rest of the
measurement loop, and timer resolution prevents exact subtraction. The gap
observer's synchronous CodeMirror measurement path is substantial
(`.vm/bench/speedometer-20260924-103246-codemirror-write-phase/`).

A later loop and timeline trace showed why the callbacks differ. Summit's
first gap callback starts before CodeMirror's initial animation-frame measure,
with `viewState.contentDOMHeight` still zero. It runs four measurement loops,
two updates, and one redraw. Firefox's first animation-frame measure runs
first; its subsequent gap callback finds a stable content height and runs one
loop without an update. The observer's entry geometry was effectively the
same in both browsers. See `.vm/bench/speedometer-20260924-104723-codemirror-schedule-order/`
and `.vm/bench/firefox-speedometer-20260924-104808-codemirror-schedule-order/`.

An opt-in diagnostic deferred only the CodeMirror gap callback to the next
animation frame. This eliminated its duplicate measurement (one loop, no
updates), but the ten-iteration suite mean rose to 186 ms. The initial layout
moved into a later frame inside the measured portion of the test. This is a
diagnostic score, not a standard Speedometer result, and does not support
changing observer dispatch as a performance fix
(`.vm/bench/speedometer-20260924-105328-codemirror-defer-gap/`).

### Guardian frame presentation diagnosis

The standalone Guardian playback probe now serializes
`getVideoPlaybackQuality()` fields explicitly and attempts
`requestVideoFrameCallback()`. On `bundle-7dosgek7`, the clip still reached
19.56 seconds and looped, but the quality fields stayed zero and the frame
callback API was unavailable. Neither browser API measures presentation on
this build. An opt-in backend trace records decode time, the delay before
the main-thread repaint request, and the age of the frame when `paint()` runs
(`.vm/bench/probe-20260924-105512-summit-guardian-frame-quality/`).

On the standalone clip, the trace logged 683 decoded frames and 681 painted
frames over the observed run. Median decode time was 2.3 ms, the repaint
callback's 95th-percentile queue delay was 0.2 ms, and median frame age at
paint was 15.4 ms. On the full article after dismissing the cookie overlay
and scrolling the inline video into view, NVDEC remained selected, but the
first 306 decoded frames produced only 235 paints. The repaint callback
waited over 40 ms on the main thread 43 times, with a 152.9 ms maximum.
Across a longer article window, the painted/decoded ratio stayed near 77%.
These observations point to page/main-thread contention, not slow NVDEC
decoding of the 480×384 clip. The article run was interactive and is not a
repeatable benchmark (`.vm/bench/probe-20260924-110000-summit-guardian-frame-handoff/`
and `.vm/bench/scroll-20260924-110339-guardian-interactive-frame-handoff/`).

An opt-in trial coalesced video repaint requests while one was waiting on
the main thread. The standalone clip still painted 681 of 683 frames and
looped. On the full article, callbacks fell from one per decode to 846 per
999 decoded frames, but only 764 frames painted and callback queue delays
still exceeded 40 ms frequently. This did not improve the observed choppiness,
so the trial was removed. Its diagnostic bundle was `bundle-eg5ddl7f`; the
production launcher was then moved to the tested trace-only
`bundle-2t4h8ozv`.

Further tracing on the full article with `SUMMIT_PAGE_UPDATE_TRACE=2` found
nine rendering updates over 33 ms during playback. Their initial-layout
phases totaled 702 ms of 731 ms. With trace level 1, more frequent layouts
were visible: 127 over 10 ms during an inline-video window, totaling 4.33 s;
4.26 s was render-tree work. They were full-tree layouts, usually around
34 ms, with no single renderer above 10 ms accounting for most of the cost.
When the video scrolled out of view and stopped, repeated layouts and frame
handoffs stopped as well
(`.vm/bench/scroll-20260924-111641-guardian-page-update/` and
`.vm/bench/scroll-20260924-111844-guardian-renderer-layout/`).

An opt-in `SUMMIT_LAYOUT_INVALIDATION_TRACE=1` now records the renderer and
element that requests each full layout. On the same article it showed a
positioned `span` changing style repeatedly; roughly every quarter second,
the root `html` renderer and a positioned link and span were invalidated
together. Those bursts coincide with video repaint queue waits above 40 ms.
The trace identifies renderers but does not identify the JavaScript call site
or prove which invalidation is necessary, so no layout behavior was changed
(`.vm/bench/scroll-20260924-112843-guardian-invalidation-source/`).
The current workstation launcher uses `bundle-3yuxtup5`, which adds only this
disabled-by-default diagnostic to the same playback code.

Document-aware tracing narrowed the recurring full layouts to the Guardian
article's main document. After video playback began, 139 layouts over 10 ms
totaled 4.85 s; each belonged to that document, and each coincided with
invalidation of its `html` renderer, skip link, and screen-reader span.
Earlier JavaScript stack traces showing PubMatic user-sync activity were from
other documents and did not explain these main-document layouts. A sampled
native stack instead shows WebCore applying a newly resolved style to the
`html` renderer and classifying the difference as requiring full layout.
This still does not establish which computed property changed, so layout
invalidation remains intact
(`.vm/bench/scroll-20260924-114522-guardian-document-layout/` and
`.vm/bench/scroll-20260924-115151-guardian-layout-native-stack/`).

Further opt-in style probes on the same inline video found that every
sampled root layout difference was in inherited font data. Line height,
letter spacing, word spacing, and the font description remained unchanged;
the cached font-set pointer changed. The document's font selector version
advanced by 41 between most root updates. A stack sampled during those
updates shows `CSSStyleSheet::insertRule()` repeatedly clearing and rebuilding
the document style resolver. This replays approximately 40 font-face rules,
advances the selector version, and creates a fresh font-set cache entry for
the root. The rule being inserted and whether these resolver rebuilds can be
handled incrementally remain under investigation
(`.vm/bench/scroll-20260924-131952-guardian-font-cache-entries/` and
`.vm/bench/scroll-20260924-132556-guardian-resolver-reset/`).

CSSOM tracing identified the repeated insertions as generated class rules
setting the video progress-bar width. The stylesheet sits in the middle of
the active author sheets. An opt-in trial rebuilt author rules in their
original order while preserving the unchanged font registrations. In a
same-bundle interactive comparison, decoded frames 101–300 produced 162/198
paints in the baseline and 195/200 in the trial. Repaint handoffs above
40 ms fell from 24 to zero, and their 95th percentile fell from 57.2 ms to
16.3 ms. The trial was restricted to the Guardian progress-bar rules and is
evidence for a general simple-rule insertion optimization; the runs are not
automated performance benchmarks
(`.vm/bench/scroll-20260924-135017-guardian-preserve-font-trial/` and
`.vm/bench/scroll-20260924-135201-guardian-preserve-font-baseline/`).

### General CSS rule insertion and Guardian playback

The production change handles a top-level ordinary `CSSStyleSheet::insertRule()`
when the document has an active, unshared resolver and no pending style update.
It rebuilds the author rule set in stylesheet order and invalidates style, while
keeping unchanged font-face and other resolver registrations. Nested, mutating,
and unsupported rule changes retain the existing resolver rebuild path. A CSSOM
probe verified insertion at the end of an earlier stylesheet, insertion in the
middle of a later stylesheet, ordinary width application, and deletion order
(`.vm/bench/probe-20260924-150230-summit-css-insert-order-egl/`).

On the reported Guardian article, three consecutive 200-frame decoded windows
painted 194, 193, and 196 frames. None had a repaint handoff over 40 ms;
their 95th percentile queue delays were 17.8, 15.4, and 11.8 ms. The earlier
baseline painted 162 of 198 frames in its first window, with 24 handoffs over
40 ms. These are interactive page traces, not an automated playback benchmark
(`.vm/bench/scroll-20260924-150322-guardian-general-css-insert/`).

The existing Reddit HLS fixture sought to 8 seconds, emitted `seeking` and
`seeked`, resumed playback, and finished its 12.7-second clip with no media
error. It tests programmatic seeking rather than dragging the Adobe ad's custom
progress control (`.vm/bench/probe-20260924-150450-summit-reddit-hls-seek-general-css/`).

A later interactive probe used an HTML range progress control with the same
Reddit HLS clip on the installed `bundle-wzdplo3u`. A VNC pointer drag moved
the range from 0 to 8.2 seconds, generated seven `input` events and the
corresponding `seeking`/`seeked` events, and ended with `video.currentTime` at
8.2 seconds and no media error. This verifies pointer delivery and repeated
seeks through a generic custom control; it cannot verify Adobe's ad player
without that ad's page URL
(`.vm/bench/probe-20260924-181521-summit-reddit-hls-pointer-drag-verified/`).

The first general implementation rebuilt the author rule set immediately on
every simple insertion. Its bundle accidentally used the cached `SkiaCG`
configuration with Haiku system malloc, which explained most of an apparent
absolute Speedometer drop. Paired ten-iteration runs on that configuration
scored 5.652 ± 0.211 for the candidate and 5.469 ± 0.257 for the installed
bundle at 1913×945; at 1280×887, they scored 5.645 ± 0.285 and
5.554 ± 0.254. The engine build wrapper now passes mimalloc defaults
explicitly so a reused cache cannot retain system malloc, and bundle manifests
record the actual allocator and graphics flags
(`.vm/bench/speedometer-20260924-150558-general-css-insert-matched/`,
`.vm/bench/speedometer-20260924-150830-css-insert-control-matched/`,
`.vm/bench/speedometer-20260924-151112-general-css-insert-1280x887/`, and
`.vm/bench/speedometer-20260924-151331-css-insert-control-1280x887/`).

The mimalloc rebuild exposed a cost hidden by the allocator mismatch: the
eager CSS path scored 6.334 ± 0.195 against 6.938 ± 0.292 for the older
mimalloc bundle at 1280×887. Disabling just the simple insertion path on the
new engine scored 6.813 ± 0.310. Consecutive simple insertions now schedule
one pending author rule set rebuild at WebKit's ordinary style flush. A later
active-sheet or non-simple content change upgrades that pending work to the
existing full rebuild. Forty insertions in one script task produced the correct
final width and preserved precedence from a later stylesheet. Adding a sheet
and deleting a rule during a pending insertion also produced the expected
computed widths. The batched
build scored 6.896 ± 0.293 in ten uncontended iterations at 1280×887
(`.vm/bench/speedometer-20260924-161217-guardian-css-mimalloc-1280x887/`,
`.vm/bench/speedometer-20260924-161437-guardian-css-mimalloc-control-1280x887/`,
`.vm/bench/speedometer-20260924-162121-simple-css-insert-disabled-1280x887/`,
`.vm/bench/probe-20260924-164559-summit-css-insert-mixed-mutations/`, and
`.vm/bench/speedometer-20260924-164011-batched-css-insert-mimalloc-1280x887/`).

On the full Guardian article with the batched mimalloc build, frames 101–700
painted 196/200, 196/200, and 194/200 across three windows, with one handoff
over 40 ms in all 600 frames and no long layout phase. The Reddit HLS fixture
still sought to 8 seconds, emitted `seeked`, and finished at 12.7 seconds
without a media error (`.vm/bench/scroll-20260924-164212-guardian-batched-css-mimalloc/`
and `.vm/bench/probe-20260924-164401-summit-reddit-hls-seek-batched-css/`).

On the 400-card scrolling fixture, the final bundle produced 58.88 fps with
an 18 ms 95th percentile frame interval and no interval over 33 ms. An older
mimalloc bundle in the same session produced 58.82 fps, also with no interval
over 33 ms. A live `/r/popular/` 80-notch pass delivered all 80 scroll events,
but Reddit returned a JavaScript challenge URL instead of the feed, so that
pass cannot verify live Reddit smoothness
(`.vm/bench/probe-20260924-164813-summit-scroll-batched-css-mimalloc/`,
`.vm/bench/probe-20260924-164904-summit-scroll-mimalloc-control/`, and
`.vm/bench/scroll-20260924-164723-reddit-batched-css-mimalloc/`).

### Speedometer asynchronous style-resolution trace

The same ten-iteration 1280×887 Speedometer run scored 6.896 ± 0.293 for
Summit and 8.338 ± 0.374 for Firefox. Preact and Svelte complex DOM suites
are among the largest gaps. An optional `--raf-phase-trace` mode now times
each benchmark step's animation-frame callbacks and zero-delay timer by
patching only the served runner response. It marks these runs as instrumented;
their scores are diagnostic and are not production benchmark results. The
generic 100-node mutation probe found a Summit/Firefox timer wait of 4/3 ms,
so the large suite-specific gap is not explained by timer dispatch alone
(`.vm/bench/probe-20260924-165402-summit-raf-timer-summit/` and
`.vm/bench/probe-20260924-165439-firefox-raf-timer-firefox/`).

In five focused iterations, Preact and Svelte add/complete steps spent
approximately 19–20 ms between the second animation callback and the timer
in Summit, versus 8–10 ms in Firefox. The delete steps spent 4–5 ms versus
3 ms. The native trace places the Summit timer wait inside WebKit's rendering
update, with roughly 13–15 ms in `Page::layoutIfNeeded()` after animation
callbacks for add/complete steps. Its `Document::updateLayout()` split shows
7–14 ms of that in `updateStyleIfNeeded()` and 1–8 ms in layout, with no
material compositing time. A deeper style trace of the steady 8–20 ms style
updates shows median 10.4 ms in `Style::TreeResolver::resolve()` and 3.2 ms
in render-tree commit. This points the next optimization work at style
resolution and its invalidation scope; Skia/GPU paint changes will not close
this measured Preact/Svelte gap
(`.vm/bench/speedometer-20260924-165801-raf-phases-summit/`,
`.vm/bench/firefox-speedometer-20260924-165857-raf-phases-firefox/`,
`.vm/bench/speedometer-20260924-172854-document-layout-phases/`, and
`.vm/bench/speedometer-20260924-173419-style-detail-phases/`).

The X399 workstation reports 32 logical CPUs (`sysinfo -cpu`). WebKit's
`WTF::numberOfProcessorCores()` already uses Haiku's online-processor count,
and its parallel work queue creates one fewer worker than that count.
`navigator.hardwareConcurrency` reports 8 because WebKit deliberately caps
the web-visible value for fingerprinting. The traced style resolution happens
on the page's main thread, so adding drawing workers alone does not divide
this particular 10 ms step across the workstation's cores.

### Preact/Svelte style-work split

A later opt-in diagnostic engine measured the native style tree on the same
five-iteration, pinned Preact/Svelte runs. Each slow complete-items update
restyled 422 elements. Preact visited 631 nodes and Svelte visited 1,038;
roughly 9.5 ms of the 10 ms tree-resolution phase was inside per-element
resolution, leaving about 0.5–0.7 ms in traversal. Across 100 changed
elements, the measured batches spent about 1.1 ms in style construction,
0.8–0.9 ms in animation/change handling, and 0.2 ms resolving pseudo-elements.
The rule-matching and property-application subparts of style construction
accounted for about 0.4–0.5 ms and 0.7–0.8 ms per 100 elements. Animation
handling had about 0.4–0.5 ms in transition checks and 0.2 ms applying
animations. These are diagnostic timings with instrumentation overhead, and
they identify no single large operation that can safely be skipped
(`.vm/bench/speedometer-20260924-174909-style-match-batch/`,
`.vm/bench/speedometer-20260924-175359-tree-resolve-detail/`,
`.vm/bench/speedometer-20260924-175852-element-resolve-detail/`, and
`.vm/bench/speedometer-20260924-180547-animated-phase-detail/`). The per-element
native hooks were removed after this measurement so they add no cost in
production.

The benchmark runners also accept `--source` for controlled diagnostic copies
of Speedometer. Removing the large shared stylesheet from Preact and Svelte
improved Summit's focused run, but increased Firefox's delete-step times
substantially because it changed page layout. That experiment does not isolate
selector matching or support a standard score comparison
(`.vm/bench/speedometer-20260924-174336-no-large-css/` and
`.vm/bench/firefox-speedometer-20260924-174422-no-large-css/`).

### Chart.js callback and Skia oval cost

Chart.js is the largest remaining single-suite gap in the latest standard
full run: 258.0 ms per iteration in Summit versus 189.1 ms in Firefox. In
matched eight-iteration focused traces, Summit's scatter draw callback took
96 ms versus Firefox's 75.5 ms, its opaque draw took 70.5 versus 58 ms, and
the tooltip's animation-frame work took 74 versus 56 ms. WebKit's page trace
placed the tooltip delay in `requestAnimationFrame` JavaScript; second layout,
intersection observers, and after-rendering work were effectively zero in
that update. The timer after the callback took about 1 ms. These runs are
instrumented and their focused scores are not standard Speedometer results
(`.vm/bench/speedometer-20260924-181934-chartjs-frame-phases/` and
`.vm/bench/firefox-speedometer-20260924-182102-chartjs-frame-phases/`).

A temporary native trace measured a median 27.6 ms per 5,000 Canvas `fill()`
calls in the same Chart.js suite. Damage notifications took about 0.5 ms and
the Skia drawing call about 26.4 ms. A second trace measured a median 22.3 ms
inside `SkCanvas::drawOval()` per 5,000 ovals. The oval batches also include
other Canvas path calls, so these figures are not an exact additive partition.
Per-call instrumentation increased the measured time and was removed after
collection. The result points at actual Skia oval rasterization and the
JavaScript work around drawing, rather than further canvas-damage coalescing
(`.vm/bench/speedometer-20260924-182602-chartjs-fill-native/` and
`.vm/bench/speedometer-20260924-183308-chartjs-oval-native/`).

### Opt-in Skia GL Canvas on Haiku

`SUMMIT_SKIA_GL_CONTEXT=1` now permits a Skia GL context on Haiku for Canvas.
The default remains CPU Canvas. Tile painting and tile sizing remain on the
CPU path even when the switch is set: the earlier all-GL tile experiment
produced Mesa Zink image-creation errors and did not improve scrolling.
The same bundle (`bundle-zk7kro3v`) was tested at a 1280×887 viewport with
ten standard Speedometer 3.1 iterations and no competing CPU load. With the
switch off it scored 6.730 ± 0.302; with the switch on it scored 7.178 ±
0.346. Chart.js fell from 277.9 to 150.3 ms per iteration and Perf Dashboard
from 296.1 to 205.3 ms. A Canvas probe with the switch on rendered 5,000
circles correctly and measured roughly 3 ms per fill batch, versus about
27.6 ms in the prior instrumented CPU trace. The full score remains below
the matched Firefox result of 8.338 ± 0.374
(`.vm/bench/speedometer-20260924-185726-skia-gl-canvas-cpu-tiles-control/`,
`.vm/bench/speedometer-20260924-185330-skia-gl-canvas-cpu-tiles-full/`, and
`.vm/bench/probe-20260924-184600-summit-skia-gl-canvas-paths/`).

On the 400-card scroll fixture at the same viewport, GL Canvas ran at
57.85 fps versus 59.02 fps with the switch off; neither run had a frame over
33 ms. The GL run still logged two `ZINK: vkCreateImage failed` messages.
For that reason the switch stays opt-in and is not in the workstation launcher.
The standalone Guardian clip visibly played with the switch on and off and
completed a loop without a media error. Its Web video-quality counters were
zero in both modes, so that probe cannot establish presentation frame rate
(`.vm/bench/probe-20260924-185540-summit-skia-gl-canvas-cpu-tiles-scroll/`,
`.vm/bench/probe-20260924-185637-summit-skia-gl-canvas-cpu-tiles-scroll-control/`,
`.vm/bench/probe-20260924-185938-summit-skia-gl-guardian-loop/`, and
`.vm/bench/probe-20260924-190056-summit-skia-gl-guardian-loop-control/`).

### Live Guardian playback on the installed build

The installed `bundle-zk7kro3v` was revisited on the reported Guardian
article. After dismissing consent and scrolling the inline video into view,
native frame tracing found 191–196 paints in five consecutive 200-frame
decoded windows. The latest window painted 196/200, with a 15.9 ms
95th-percentile repaint handoff and no handoff above 40 ms. One earlier
window had two handoffs above 40 ms, the worst 81.5 ms. A second article run
selected `H.264 on the graphics card (NVDEC)` with status 0 and painted
198/200 in its latest decoded window. The video was visible in the captured
screenshots. This confirms the current build's hardware decoder selection
and a much better paint ratio than the original roughly 77% trace; it does
not prove that every frame is presented on time
(`.vm/bench/scroll-20260924-190747-guardian-current-live/after-video.log`
and `.vm/bench/scroll-20260924-191305-guardian-current-codec/after-video.log`).

On the same live article with opt-in GL Canvas, three 200-frame windows
painted 191, 195, and 193 frames. NVDEC remained selected and the video was
visible, but Mesa again logged two Zink image-creation errors. The GL switch
did not improve this playback check, so it remains off in the workstation
launcher (`.vm/bench/scroll-20260924-191457-guardian-gl-live/after-video.log`).

### Matched-declarations cache check

The opt-in GL Canvas build improves Chart.js and Perf Dashboard beyond the
saved Firefox 155 times, but the remaining Speedometer gap spans TodoMVC
frameworks, CodeMirror, and Observable Plot. Against Firefox at 1280×887,
the GL Canvas full run was 39.4 ms slower in Observable Plot, 31.7 ms in
CodeMirror, and about 29 ms in each of Preact and Svelte. A broad style or
DOM improvement is needed to reach the Firefox score; another Canvas-only
change cannot close these gaps
(`.vm/bench/speedometer-20260924-185330-skia-gl-canvas-cpu-tiles-full/`
and `.vm/bench/firefox-speedometer-20260921-102926-ws/`).

A temporary matched-declarations cache trace counted 500 applications per
report, including setup and measured work. Across five focused iterations,
Preact and Svelte each reported 27,500 applications with usable cache entries
on 93.5% and 93.1%, respectively. Observable Plot reported 23,500 with
98.3% usable entries. CodeMirror reported only 2,000 applications with 62.1%
usable entries; its previously measured gap lies mainly in synchronous
editor measurement from an IntersectionObserver callback. A second temporary
Preact trace spent 0.010, 0.013, and 0.012 ms per 100 applications in cache
hashing, lookup, and cacheability checks. Timing hooks add overhead, but these
costs are far below the roughly 10 ms tree-resolution phase. Raising cache
hit rate or optimizing its lookup is therefore not the next Speedometer fix.
Both temporary traces were removed and the engine patch returned to its
committed digest
(`.vm/bench/speedometer-20260924-192514-preact-style-cache/`,
`.vm/bench/speedometer-20260924-192622-svelte-style-cache/`,
`.vm/bench/speedometer-20260924-193005-codemirror-style-cache/`,
`.vm/bench/speedometer-20260924-193110-observable-style-cache/`, and
`.vm/bench/speedometer-20260924-193712-preact-style-cache-timing/`).

### Accelerated Canvas cutoff and Guardian Zink errors

A temporary setting varied the minimum accelerated 2D Canvas area while
keeping the same GL-enabled bundle, viewport (1280×887), and ten focused
Speedometer 3.1 iterations. Chart.js / Perf Dashboard mean times were
148.3 / 203.3 ms at the WebKit default of 16,512 pixels, 152.1 / 195.6 ms
at 65,536 pixels, and 155.7 / 289.0 ms at 262,144 pixels. The high cutoff
removed most of the Dashboard GPU benefit; the middle cutoff retained it
(`.vm/bench/speedometer-20260924-194857-canvas-area-default/`,
`.vm/bench/speedometer-20260924-194950-canvas-area-65536/`, and
`.vm/bench/speedometer-20260924-195047-canvas-area-262144/`).

On the reported Guardian article, GL-enabled runs at 65,536, 262,144, and
1,000,000,000 pixels each logged two
`ZINK: vkCreateImage failed (VK_ERROR_UNKNOWN)` errors. A temporary stack
trace at Skia GL-context creation, with the one-billion-pixel cutoff, located
both first requests in `ScrollerCoordinated::updateValues()`, reached while
WebKit finalized rendering. Thus reducing accelerated Canvas use does not
address the observed Zink errors; coordinated scrollbar painting creates GL
contexts even without an accelerated Canvas on that page. The cutoff and
trace hooks were removed. GL Canvas remains opt-in pending a scrollbar/driver
fix (`.vm/bench/scroll-20260924-195146-guardian-canvas-area-65536/`,
`.vm/bench/scroll-20260924-195226-guardian-canvas-area-262144/`,
`.vm/bench/scroll-20260924-195341-guardian-canvas-area-high/`, and
`.vm/bench/scroll-20260924-195802-guardian-gl-callsite/browser.log`).

### Raster coordinated scrollbars on Haiku

Haiku's coordinated scrollbar painter now draws into a raster Skia surface and
passes its immutable image through the existing native-image layer upload.
The prior texture-mapper path requested a Skia GL context on every scrollbar
update, even with GL Canvas disabled, then returned without painting when
that context was unavailable. Raster painting removes that dependency and
makes the scrollbar visible with the normal launcher configuration. On the
reported Guardian article with GL Canvas enabled, the rebuilt candidate logged
no Zink image-creation error, versus two in the previous build
(`.vm/bench/scroll-20260924-200436-guardian-cpu-scrollbar/`).

Matched, uncontended 600-frame 400-card scroll probes at 1913×945 ran at
58.92 fps with raster scrollbars and 58.93 fps on the installed control, both
with zero frames over 33 ms. With GL Canvas enabled, the raster-scrollbar
candidate ran at 58.94 fps with zero frames over 33 ms and no Zink error
(`.vm/bench/probe-20260924-200814-summit-cpu-scrollbar-candidate/`,
`.vm/bench/probe-20260924-200859-summit-cpu-scrollbar-control/`, and
`.vm/bench/probe-20260924-201313-summit-cpu-scrollbar-gl-scroll/`).

At 1280×887, a full ten-iteration Speedometer 3.1 run with GL Canvas enabled
scored 7.288 ± 0.359, close to the previous 7.178 ± 0.346. It remains below
Firefox's 8.338 ± 0.374. At this stage the workstation launcher was moved to
`bundle-s8a06mrs` with GL Canvas still off, pending the live media check
(`.vm/bench/speedometer-20260924-201118-cpu-scrollbar-gl-full/`).

### Guardian playback after raster scrollbar fix

Matched standalone runs of the reported article's 480×384 H.264 clip used
NVDEC with status 0 and completed a loop without a media error. With GL
Canvas off and on, each decoded 683 frames and painted 681; both had a
roughly 15.8 ms 95th-percentile frame age at paint. Neither run logged a
Zink error (`.vm/bench/probe-20260924-201640-summit-guardian-cpu-scrollbar-gl-off/`
and `.vm/bench/probe-20260924-201734-summit-guardian-cpu-scrollbar-gl-on/`).

On the full reported Guardian article, after dismissing consent and bringing
the inline video into view, GL Canvas on painted 195 of the latest 200
decoded frames. GL Canvas off painted 196 of 200 in a separate article run.
Both selected NVDEC with status 0 and had no repaint queue delay over 40 ms
in those windows. The 95th-percentile queue delay was 17.4 ms with GL on and
15.2 ms with GL off. The video was visible in both screenshots. These native
traces count decode and paint, not exact presentation timing, and the live
page can change between runs. They do not show a material playback regression
from GL Canvas after the scrollbar fix
(`.vm/guardian-gl-raster-live-after-video.log`,
`.vm/guardian-cpu-raster-live-after-video.log`).

The workstation desktop launcher now sets `SUMMIT_SKIA_GL_CONTEXT=1` by
default to use the measured Canvas gain. `SUMMIT_SKIA_GL_CONTEXT=0` overrides
it for comparison. The underlying bundle is still `bundle-s8a06mrs`; no
engine rebuild was needed for this launcher change.

### GPU tile painting revisited after raster scrollbars

An opt-in Haiku trial enabled Skia GPU tile painting while retaining GL
Canvas and the new raster scrollbar. This isolates the previous scrollbar
GL-context failure from tile painting. On the 600-frame 400-card fixture,
two GPU paint workers reached 59.59 fps and one reached 59.68 fps, versus
58.94 fps for CPU tiles, with no frame over 33 ms in any run. Mesa still
logged two Zink image-creation errors with two GPU workers and one with a
single worker
(`.vm/bench/probe-20260924-203040-summit-gpu-tiles-raster-scroll/` and
`.vm/bench/probe-20260924-203339-summit-gpu-tiles-one-worker-scroll/`).

At 1280×887, ten uncontended Speedometer 3.1 iterations scored
7.172 ± 0.341 with two GPU paint workers and 7.096 ± 0.360 with one,
against the matched CPU-tile 7.288 ± 0.359. The confidence ranges overlap,
so this does not establish a precise slowdown, but it provides no benchmark
gain and the GPU runs logged five and three Zink errors, respectively. The
trial was removed and the workstation engine restored to the committed
CPU-tile path. The installed desktop launcher was unchanged
(`.vm/bench/speedometer-20260924-203129-gpu-tiles-raster-full/` and
`.vm/bench/speedometer-20260924-203438-gpu-tiles-one-worker-full/`).

### Zen 1 compiler target trial

The workstation's Threadripper 1950X has 16 Zen 1 cores and 32 hardware
threads. The installed engine uses release `-O3` without a CPU-specific
target. An isolated `SkiaCGMiZen1` engine was built with
`-march=znver1` and the same Skia, coordinated graphics, asynchronous
scrolling, and mimalloc options as the installed engine. The build uses its
own directory and did not change the installed launcher. The build finished
successfully with release `-O3`; its separate bundle is `bundle-o19kil6q`.
At the matched 1280×887 viewport, its uncontended ten-iteration Speedometer
3.1 run scored 7.178 ± 0.335 versus 7.288 ± 0.359 for the installed build.
The 600-frame 400-card scroll probe ran at 58.79 fps with no frame over
33 ms, versus 58.94 fps for the installed build. Both differences are small
and provide no evidence of a CPU-target gain, so the workstation launcher
stays on `bundle-s8a06mrs` (`.vm/zen1-engine-build.log`,
`.vm/bench/speedometer-20260924-220312-zen1-full/`, and
`.vm/bench/probe-20260924-220530-summit-zen1-scroll/`).

### Repeated video seeking under build load

`media-seek-sweep.html` now makes five rapid forward/backward `currentTime`
changes, like a custom video progress control. On the installed workstation
bundle, both the reported Guardian clip and the Reddit HLS fixture loaded and
reached the final seek target without a media error. The Guardian clip's
three completed seeks decoded 74, 116, and 43 preroll frames, respectively;
the Reddit fixture decoded 58, 14, and 27. Several intermediate requests
were coalesced. JS property setters returned immediately, while seek events
arrived roughly 100–440 ms later. These runs overlapped 23–24 active compiler
teams and are explicitly marked contended
(`.vm/bench/probe-20260924-213829-summit-guardian-seek-sweep-build-load/`
and `.vm/bench/probe-20260924-213907-summit-reddit-seek-sweep-build-load/`).

The same two remote sources stalled before metadata in the VM. Those probe
files contain no seek and cannot validate the seek path there
(`.vm/bench/probe-20260924-213630-summit-reddit-seek-sweep-vm/` and
`.vm/bench/probe-20260924-213707-summit-guardian-seek-sweep-vm/`).

After the build stopped, the same probe on both the Zen 1 and installed
bundles used NVDEC H.264 with status 0 and reached the final Guardian seek
target without a media error. It also recorded page-thread heartbeat gaps
of 210–265 ms in each run, coinciding with synchronous preroll of up to
119 frames. The installed bundle's Reddit HLS run likewise used NVDEC,
reached its final target, and recorded a 265 ms gap while prerolling 58
frames for the first seek. This provides an engine-level cause for sluggish
progress-bar dragging, although it does not identify the Adobe ad player's
own behavior. This measurement led to moving preroll off the page thread
while keeping the seek target and paused-frame behavior correct
(`.vm/bench/probe-20260924-220617-summit-guardian-seek-sweep-zen1/`,
`.vm/bench/probe-20260924-220651-summit-guardian-seek-sweep-control/`, and
`.vm/bench/probe-20260924-220738-summit-reddit-seek-sweep-control/`).

### Asynchronous Media Kit seek preroll

The Haiku media backend now seeks tracks on the page thread, then decodes
video keyframe preroll on its existing decoder thread. It takes and releases
the media lock for each decoded frame, allowing a new drag position to cancel
an old seek. The seek promise settles when the requested frame is ready;
paused video also runs this preroll and repaints that frame. Load cancellation
rejects a pending promise, and a seek generation prevents a stale completion
from settling a newer request.

On the idle workstation, the repeated-seek probe recorded **no heartbeat gap
over 80 ms** with the new bundle on either the Guardian or Reddit clip. The
prior build recorded gaps of 210–265 ms on Guardian and up to 265 ms on
Reddit. Both new runs used NVDEC H.264 with status 0, reached their
last target, and reported no media error. A separate paused Guardian run
remained paused at 13.692 seconds, emitted `seeked`, had no gap over 80 ms,
and showed the sought frame in its screenshot. A 22-second Guardian playback
probe completed a loop with 684 decoded frames, 682 painted frames, no media
error, and no Zink error. These are controlled clips; the Adobe ad player
itself remains unverified because its URL is unavailable. The workstation
launcher now points to the verified `bundle-w9ti9d76`; its uncontended
Speedometer result is 7.298 ± 0.333, and the 600-frame scroll probe is
58.88 fps with no frame over 33 ms. The media change did not measurably
change these two performance results. Evidence:
(`.vm/bench/probe-20260924-221508-summit-guardian-seek-sweep-async/`,
`.vm/bench/probe-20260924-221557-summit-reddit-seek-sweep-async/`,
`.vm/bench/probe-20260924-221654-summit-guardian-paused-seek-async/`, and
`.vm/bench/probe-20260924-221753-summit-guardian-loop-async/`).

### Preact and Svelte style invalidation scope

An opt-in diagnostic counted why elements entered style resolution in focused
Preact and Svelte complex DOM runs. Each suite repeated three large passes per
iteration. The largest resolved 3,672 elements and took about 34–36 ms after
warmup; 3,666 elements were still individually marked valid, but six
subtree-invalid roots caused full descendant resolution. The benchmark page's
`html.spectrum` root was among those invalidated roots. The add-items pass
resolved about 515 elements in 4–5 ms, nearly all newly subtree-invalid.
The complete-items pass resolved 422 elements in 10–11 ms: 181 were directly
invalid, 240 were individually valid but reached through parent changes, and
80 of those used WebKit's fast inheritance path. The pattern was stable across
both frameworks and repeated iterations. These counts explain why adding
tile paint workers cannot divide this main-thread style cost; they do not
prove that any particular descendant can be skipped. The diagnostic hooks
were removed and the production engine source rebuilt from the committed
patch (`.vm/bench/speedometer-20260924-223104-style-reasons/`,
`.vm/bench/speedometer-20260924-223632-style-reasons-url/`,
`.vm/bench/speedometer-20260924-224117-style-root/`, and
`.vm/style-reason-restore-build.log`).

### Current live Reddit scroll check

The installed async-seek bundle loaded a visible `/r/popular/` feed. An
80-notch, 25-ms wheel burst moved it at 57.22 native-view frames/s with a
47.3 ms worst frame interval. A 300-notch burst delivered all 300 wheel
events but recorded only 16.26 frames/s and a 5,719 ms presentation gap;
the before/after screenshots still showed feed posts. A second 300-notch
run with page-update and media-lifecycle traces reached 23.47 frames/s and
had a 4,519 ms worst interval. In that trace, one bubbling JavaScript scroll
listener occupied 2,140.6 ms. Across the run, 32 slow layouts totaled
5,368.2 ms, including 4,579.8 ms in render-tree layout and 641.7 ms in
post-layout tasks; these totals include layouts nested in the listener and
must not be added to its time. Media cancellation accounted for only 12.6
ms. Native frame queue delay stayed below 0.3 ms in both long runs. The tab
URL carried Reddit's `js_challenge=1` query even while the feed was visible,
so site challenge activity may have affected these measurements. The current
bottleneck is still synchronous page work and layout, not wheel delivery or
the native view queue (`.vm/bench/scroll-20260924-224550-reddit-current-live/`,
`.vm/bench/scroll-20260924-224651-reddit-current-long/`, and
`.vm/bench/scroll-20260924-224809-reddit-current-long-trace/`).

### Profile-guided build trial

GCC 13.3 on the X399 workstation successfully compiled a small C++ program
with `-fprofile-generate`, wrote its `.gcda` file on normal exit, and then
accepted that profile with `-fprofile-use -fprofile-correction` at the same
output path. The compiler reports `single` as its default profile-counter
update mode, so the browser build explicitly uses `-fprofile-update=atomic`
for its multiple threads. The initial non-atomic build was stopped before
profiling, and the isolated `SkiaCGMiPGO` build is being rebuilt with that
flag. Training will include Speedometer 3.1 and the 400-card scroll fixture.
The optimized pass will use `-fprofile-partial-training`, which tells
[GCC 13.3](https://gcc.gnu.org/onlinedocs/gcc-13.3.0/gcc.pdf) to optimize
untrained functions normally instead of favoring size. This matters because
two workloads cannot cover every browser path. The installed launcher stays
unchanged until a matching full benchmark and scroll probe show a gain; the
earlier Zen 1 target alone did not.

Haiku's `exitProcess()` and `terminateProcess()` use `_exit()`, so GCC's
ordinary exit handler cannot write browser-process profiles. A three-module
Haiku probe called `__gcov_dump()` from an instrumented shared library just
before `_exit()` and wrote `.gcda` files for both libraries and the executable.
The port now has an opt-in `SUMMIT_GCOV_DUMP=1` hook on both exit paths. The
training link includes the dump routine with `-u__gcov_dump`; normal builds
leave the weak reference unresolved. The WebCore PGO archive also exposed a
build-tool bottleneck: GNU `ar` consumed one core for over 13 minutes on 742
objects, while `llvm-ar` assembled and listed the same objects in under one
second. The isolated Ninja archive rules now use `llvm-ar` and `llvm-ranlib`.
After CMake regenerates those rules, the tracked
`tools/use-llvm-ar-in-webkit-build.py` helper reapplies the switch to the
existing build directory before Ninja runs.
The instrumented WebKit link and browser training completed
(`.vm/pgo-generate-atomic-engine-build.log`,
`.vm/pgo-generate-llvm-hook-build.log`, and
`.vm/pgo-generate-llvm-resume.log`).

The instrumented engine linked successfully and was packaged as
`bundle-vvu0w_wt`. A full ten-iteration Speedometer 3.1 run at the matched
1280×887 viewport completed, followed by all 600 frames of the 400-card
scroll fixture. Its observed score and frame rate include GCC's atomic
counter overhead and are training data, not performance comparisons. Before
training there were no `.gcda` files; afterwards there were 1,972, including
643 under WebCore, 189 under JavaScriptCore, and 311 under WebKit. The scroll
run updated the files again. A copy of the profiles was saved outside the
build directory before reconfiguring the same object paths for profile use.
The optimized build completed without profile mismatch errors. Missing-profile
warnings remain for paths not exercised by the two training workloads.
(`.vm/bench/speedometer-20260925-004337-pgo-train-full/`,
`.vm/bench/probe-20260925-005022-summit-pgo-train-scroll/`, and
`.vm/pgo-use-build.log`).

The profile-use engine passed a `jsc` smoke test and was packaged as
`bundle-1a8lpbnq`; the installed launcher still points to `bundle-w9ti9d76`.
At the same 1280×887 viewport, its first full ten-iteration Speedometer 3.1
run scored **8.512 ± 0.400**, compared with the saved installed-bundle run of
**7.298 ± 0.333** and Firefox 155 run of **8.338 ± 0.374**. The point score
exceeds the saved Firefox score, but the reported intervals overlap. A fresh
installed-bundle run scored **7.185 ± 0.353** under the same settings; a
second profile-use run scored **8.444 ± 0.456**. Both profile-use scores are
about 17% above the fresh installed-bundle score. The 400-card fixture
delivered all 600 frames at **59.65 frames/s**, with no frame interval over
33 ms and a 19 ms
maximum, versus 58.88 frames/s on the earlier installed-bundle run
(`.vm/bench/speedometer-20260925-020140-pgo-use-full/` and
`.vm/bench/probe-20260925-020348-summit-pgo-use-scroll/`,
`.vm/bench/speedometer-20260925-021142-pgo-control-full/`, and
`.vm/bench/speedometer-20260925-021356-pgo-use-full-repeat/`).

The Guardian loop fixture played past its 19.56-second duration and continued
without a media error; H.264 selected NVDEC successfully. The Reddit HLS seek
fixture reported a seekable interval of 0–12.7 seconds, completed a seek to
8 seconds, and ended normally with NVDEC selected. These checks show no media
regression in the profile-use build; they do not reproduce the unavailable
Adobe ad or prove that the live Guardian article plays smoothly
(`.vm/bench/probe-20260925-020510-summit-pgo-use-guardian-loop/` and
`.vm/bench/probe-20260925-020613-summit-pgo-use-reddit-seek/`).

Three matched live `/r/popular/` 300-notch wheel bursts, alternating the
profile-use build, installed bundle, and profile-use build, measured 32.93,
27.92, and 28.85 frames/s respectively. Their worst presentation gaps were
3.31, 4.01, and 3.81 seconds. Native UI queue delays over 33 ms remained
zero on the profile-use runs. Feed variation and the overlapping results make
this inconclusive as a scrolling gain. Long main-thread stalls remain the
principal live Reddit problem
(`.vm/bench/scroll-20260925-020711-pgo-reddit-long-a/`,
`.vm/bench/scroll-20260925-020827-pgo-reddit-long-control/`, and
`.vm/bench/scroll-20260925-020933-pgo-reddit-long-b/`).

A fresh same-workstation Firefox 155 run scored **8.238 ± 0.368**
(`.vm/bench/firefox-speedometer-20260925-021551-pgo-fresh-comparison/`).
The workstation desktop launcher was updated to `bundle-1a8lpbnq` after the
repeated Summit scores, scroll probe, and two media fixtures passed. The old
launcher was saved as `Summit-current.pre-pgo-20260925.sh`; launching through
the desktop script started the optimized Summit executable. The live Reddit
stalls and the untested Adobe ad remain open.

The reported full Guardian article was also loaded in the installed PGO build.
After accepting consent and bringing the inline video into view, it selected
NVDEC H.264 with status 0. Two capture-free windows each painted **198 of the
latest 200 decoded frames**, with no decode or repaint queue delay over 40 ms;
their maximum queue delays were 24.9 and 28.8 ms. The video looped, and the
trace had no Zink error. A separate window that included a VNC screen capture
painted only 178/200 and queued frames for up to 758.5 ms, so screen capture
must be excluded from playback pacing measurements. These native traces count
decode and paint, not exact monitor presentation timing
(`.vm/bench/scroll-20260925-022247-pgo-guardian-frame-trace/`,
`.vm/guardian-pgo-frame-idle-video.log`, and
`.vm/guardian-pgo-frame-idle-video-2.log`).

A further live Reddit `/r/popular/` diagnostic on the PGO build traced the
same 300-notch wheel burst. The feed was visible and moved, but verbose layout
instrumentation emitted over 57,000 log lines and reduced the measured burst
to 1.03 native-view frames/s; that rate is not comparable with the untraced
runs above. The 32 slow layout passes totaled 7.78 seconds, of which 7.38
seconds were render-tree layout. Their worst individual pass took 627 ms.
Inclusive renderer timing was concentrated in nested flex and grid layout;
these sums overlap and cannot be added. Media cancellation took 38 ms, and
native UI queue delay stayed below 1 ms. The document still carried Reddit's
`js_challenge=1` parameter, so challenge behavior may affect this trace.
The repeated layout work remains the next area to investigate; this trace
does not isolate a safe layout step to skip
(`.vm/bench/scroll-20260925-022551-pgo-reddit-long-trace/`).

The opt-in 16 ms scrolling refresh timer was retried on the PGO build in a
300-notch A/B/A sequence. Timer-on, timer-off, and timer-on runs delivered
27.72, 28.47, and 15.50 native-view frames/s, with worst gaps of 1.25, 1.62,
and 2.62 seconds. The timer did not produce a repeatable long-burst
improvement on that bundle. The scrolling-position trace gives an important limit on
these long-burst numbers: each run reached the current document maximum around
wheel event 110, and about 190 of 300 events occurred at that maximum. The
first timer-on screenshot showed a post and loading spinner; the second showed
a post with a black, not-yet-playing video. A long gap after reaching the
current feed end is not by itself evidence that moving content was presented
slowly. Shorter bursts within loaded content are needed to measure motion
separately from Reddit's content loading
(`.vm/bench/scroll-20260925-022913-pgo-reddit-refresh-300-a/`,
`.vm/bench/scroll-20260925-023022-pgo-reddit-refresh-300-control/`, and
`.vm/bench/scroll-20260925-023123-pgo-reddit-refresh-300-b/`).

An 80-notch burst avoids that feed boundary. With the PGO build and timer
off, two runs delivered only 2.50 and 2.47 native-view frames/s; the prior
non-PGO bundle delivered 2.90 frames/s in the same test. All 80 wheel events
were handled, none reached the scrolling tree's maximum, and screenshots
confirmed movement to later posts. With the opt-in 16 ms timer, two PGO runs
delivered 34.72 and 34.74 frames/s and still showed posts, although their
worst gaps were 454 and 349 ms. Thus the missing refresh is a substantial
part of the **active** Reddit scroll stall, even though the timer alone does
not achieve smooth 60 fps (`.vm/bench/scroll-20260925-023310-pgo-reddit-active-80/`,
`.vm/bench/scroll-20260925-023424-pgo-reddit-active-80-repeat/`,
`.vm/bench/scroll-20260925-023520-pgo-reddit-active-80-old-control/`,
`.vm/bench/scroll-20260925-023614-pgo-reddit-active-80-refresh/`, and
`.vm/bench/scroll-20260925-023819-pgo-reddit-active-80-refresh-repeat/`).

The Haiku refresh timer now starts only after a scrolling-tree wheel event was
actually handled. This avoids restarting it for the many events at a finite
feed's maximum, while preserving nested-scroller wheel handling. The isolated
PGO rebuild archived the stale GCC profile for the one changed unified
WebProcess object; all other trained objects retained their profiles. The
candidate is `bundle-m2di58k1`. Its 80-notch opt-in run delivered 34.86
frames/s with all events handled below the maximum and a visible post. Its
300-notch run delivered 15.73 frames/s, reached the feed maximum at event
112, and still had a 1.65-second pending gap; it does not resolve long-feed
content production. A full Speedometer 3.1 run scored **8.347 ± 0.506**.
The Guardian clip looped with NVDEC and no media error. The Reddit HLS clip
sought to 8 seconds, emitted `seeked`, and ended at 12.7 seconds with NVDEC
and no media error (`.vm/bench/scroll-20260925-024607-handled-refresh-reddit-80/`,
`.vm/bench/scroll-20260925-024704-handled-refresh-reddit-300/`,
`.vm/bench/speedometer-20260925-024821-handled-refresh-pgo-full/`,
`.vm/bench/probe-20260925-025031-summit-handled-refresh-guardian-loop/`,
and `.vm/bench/probe-20260925-025153-summit-handled-refresh-reddit-seek/`).

The candidate's 400-card fixture completed 600 frames at **59.47 frames/s**,
with no frame interval over 33 ms. The workstation desktop launcher now points
to `bundle-m2di58k1` and enables the 16 ms timer by default; setting
`SUMMIT_SCROLL_REFRESH_TIMER=0` disables it for comparison. A desktop-script
smoke launch started the packaged candidate. The previous launcher is saved
as `Summit-current.pre-handled-refresh-20260925.sh`. Active Reddit motion is
substantially better in the measured short bursts, but the 349–454 ms gaps,
grey media placeholders, and long feed-boundary stalls remain open
(`.vm/bench/probe-20260925-025454-summit-handled-refresh-scroll-fixture/`).

A low-volume trace of an 80-notch run on the installed build delivered all
80 wheel events below the feed maximum and 86 native-view frames over the
2.42-second snapshot. Nine slow layouts totaled 2.74 seconds of inclusive
time, nearly all in render-tree layout; the slowest was 374 ms. Its completed
frame gaps stayed below 35 ms, but the snapshot was taken about 0.4 seconds
after the last wheel delivery and counted that trailing idle as a 429 ms
pending gap. A separate compositor trace showed 80 async composition requests
with a 56 ms maximum request interval and a 58 ms maximum completed native
frame gap, despite a 439 ms pending gap after motion stopped. Other active
passes still recorded completed gaps of 103 and 349 ms, so the timer does not
eliminate every moving-content stall. The scroll harness now reports completed
and pending gaps separately while retaining its previous combined maximum
for historical comparisons
(`.vm/bench/scroll-20260925-025649-handled-refresh-reddit-80-phases/` and
`.vm/bench/scroll-20260925-025816-handled-refresh-reddit-80-compositor/`).

### Smooth wheel scrolling trial

The Haiku engine had WebKit's `ENABLE_SMOOTH_SCROLLING` compiled out, and the
`ScrollAnimatorEnabled` preference defaulted to false. In the coordinated
scrolling path, a non-precise wheel event can retarget `ScrollAnimationSmooth`
when both are enabled; the scrolling tree then remains active while the
animation has frames to present. This may fill some of the 25 ms spaces
between discrete wheel events, but it requires an isolated comparison because
the animation also changes perceived wheel latency and compositor work.

The preference's Haiku default is now true in the port patch. An isolated
Release engine is being built as `WebKitBuild/SkiaCGMiSmooth` with
`ENABLE_SMOOTH_SCROLLING=ON`, asynchronous scrolling, Skia, GL compositing,
and mimalloc. The installed PGO bundle remains `bundle-m2di58k1` during the
trial. The 400-card scroll fixture now accepts `?manual=1` to leave a fixed
page ready for paced wheel input; it will be the first A/B comparison before
testing live Reddit.

The isolated engine and separate `bundle-5brd8erg` built successfully. On the
fixed 400-card page, matched 80-notch/25 ms bursts moved to the same visible
card. The installed PGO bundle presented 117 frames through its last moving
frame (58.09 fps, 33.7 ms worst completed gap); the smooth trial presented
129 (58.45 fps, 25.3 ms worst completed gap) and continued animating for
about 0.2 seconds after the last notch. On live `/r/popular/`, the smooth
trial presented 130 and 129 frames in two runs (57.35 and 58.77 fps through
the last frame), versus 85 frames (42.66 fps) for a matched installed-bundle
run. The Reddit runs landed on different feed content and showed media
placeholders, so this is evidence of a useful short-burst improvement rather
than a guarantee of consistently smooth 60 fps. All runs delivered all 80
notches and screenshots showed movement
(`.vm/bench/scroll-20260925-044227-manual-fixed-baseline/`,
`.vm/bench/scroll-20260925-044319-manual-fixed-smooth/`,
`.vm/bench/scroll-20260925-044405-reddit-80-smooth/`,
`.vm/bench/scroll-20260925-044508-reddit-80-baseline-matched/`, and
`.vm/bench/scroll-20260925-044610-reddit-80-smooth-repeat/`).

The unprofiled smooth bundle scored **7.153 ± 0.340** on a full ten-iteration
Speedometer 3.1 run, below the installed PGO bundle's **8.347 ± 0.506** and
the fresh Firefox run's **8.238 ± 0.368**. It is therefore still a trial
bundle; a PGO rebuild is needed before considering installation. Its Guardian
clip looped with NVDEC H.264 and no media error. The Reddit HLS clip sought to
8 seconds, emitted `seeked`, played to 12.7 seconds, and ended with NVDEC and
no media error. These controlled clips do not resolve the reported Adobe ad's
dragging behavior, so `media-drag.html` is available for a direct pointer and
seek event check when that player can be reproduced
(`.vm/bench/speedometer-20260925-044733-smooth-scroll-full/`,
`.vm/bench/probe-20260925-045020-summit-smooth-guardian-loop/`, and
`.vm/bench/probe-20260925-045123-summit-smooth-reddit-seek/`).

The profile-use engine was rebuilt with `ENABLE_SMOOTH_SCROLLING=ON`. GCC
rejected the stale profile for the unified platform object containing
`ScrollingEffectsController::handleWheelEvent`, because enabling the animator
changes its branches. That single `.gcda` file was archived as
`/boot/home/summit/tmp/SkiaCGMiPGO-stale-smooth-platform-9.gcda`; the rebuild
then completed, retaining the other trained objects. The resulting separate
bundle is `bundle-k7atp8la`, and a `jsc` smoke test returned 42. Full
ten-iteration Speedometer 3.1 runs scored **8.338 ± 0.398** and
**8.544 ± 0.434** at 1280×887. The point scores match or exceed the installed
PGO build's **8.347 ± 0.506** and Firefox's **8.238 ± 0.368**, though the
intervals overlap. The unprofiled smooth bundle's 7.153 score is not the
installed result
(`.vm/smooth-pgo-build.log`, `.vm/smooth-pgo-resume.log`,
`.vm/bench/speedometer-20260925-060409-smooth-pgo-full/`, and
`.vm/bench/speedometer-20260925-061105-smooth-pgo-full-repeat/`).

On the fixed 400-card wheel test, the optimized smooth bundle delivered 132
frames at 59.48 fps through the last frame, with a 24.6 ms worst completed
gap and the same final card as the control. Two live Reddit 80-notch bursts
delivered 129 frames each at 58.82 and 58.38 fps through the last frame,
versus 85 frames at 42.66 fps in the matched installed-bundle run. The live
smooth runs still had completed gaps of 41.2 and 52.6 ms; they do not prove
fully steady 60 fps or address the long feed-boundary stalls. The 600-frame
programmatic scroll fixture ran at 59.72 fps with no interval over 33 ms
(`.vm/bench/scroll-20260925-060609-manual-fixed-smooth-pgo/`,
`.vm/bench/scroll-20260925-060657-reddit-80-smooth-pgo/`,
`.vm/bench/scroll-20260925-061307-reddit-80-smooth-pgo-repeat/`,
`.vm/bench/probe-20260925-060806-summit-smooth-pgo-scroll-fixture/`).

The Guardian clip looped with NVDEC H.264 and no media error. The Reddit HLS
clip sought to 8 seconds, emitted `seeked`, played to 12.7 seconds, and ended
with NVDEC and no media error. These controlled checks still do not reproduce
the reported Adobe ad drag or establish the presentation cadence of the live
Guardian article. The workstation desktop launcher now points to
`bundle-k7atp8la`, keeps GL Canvas and the 16 ms scrolling refresh timer on,
and can override either through its existing environment variables. Its
previous version is saved as `Summit-current.pre-smooth-pgo-20260925.sh`.
The Haiku port now defaults `ENABLE_SMOOTH_SCROLLING` to ON so subsequent
builds retain the feature
(`.vm/bench/probe-20260925-060857-summit-smooth-pgo-guardian-loop/` and
`.vm/bench/probe-20260925-061012-summit-smooth-pgo-reddit-seek/`).

A manual seek-drag check used the installed bundle and
`tools/bench/pages/media-drag.html` with the same Reddit HLS clip. Through
VNC, dragging the page's range control from the start to 8.52 seconds
delivered `pointermove` events with `buttons=1`, changed `currentTime`,
emitted `seeking` and `seeked`, and repainted the sought frame while paused.
The video is given a fixed 640×360 viewport so the diagnostic timeline stays
visible for portrait clips. This verifies the browser's generic pointer-to-
media-seek path, but the specific Adobe ad timeline remains untested
(`.vm/media-drag-sized.png` and `.vm/media-drag-after.png`).

### Repeated Reddit scrolling within loaded content

`run-scroll-cycles.py` alternates 80-notch, 25 ms down/up bursts on one loaded
`/r/popular/` page. Each burst resets the native-view frame counter, verifies
all wheel deliveries, and saves a separate log. This avoids treating wheel
events after the current feed maximum as scrolling work. It captures the page
before, immediately after, and after a three-second settle; VNC capture stays
outside the measured bursts. The initial four-burst trial handled all 320
events without reaching the feed maximum. Three bursts presented near 59 fps,
but the fourth had a 203.6 ms completed-frame gap with only 0.2 ms of native
view queue delay (`.vm/bench/scroll-cycles-20260925/run.json`).

The first low-volume page-update trace reproduced a 251.8 ms reverse-burst
gap, but compositor records placed it at the transition from asynchronous
scroll frames to the next page rendering update. That update included a
230 ms render-tree layout. A later active-frame snapshot caught a 324.6 ms
gap in a reverse burst without page-update tracing. The benchmark now records
the active snapshot before the smooth-scroll tail, so those two kinds of
pause can be interpreted separately
(`.vm/bench/scroll-cycles-20260925-063644-trace/` and
`.vm/bench/scroll-cycles-20260925-063818-active-tail/`).

In a 20-burst compositor trace on the installed build, all 1,600 wheel events
were handled and the final feed was visibly populated. The median active
burst rate was 58.8 fps, with a 263.5 ms worst completed gap. That gap
matched one compositor frame whose **paint phase took 246.2 ms**; refresh
requests kept arriving and the native view queue maximum was 0.1 ms in that
burst. Two other reported gaps of 143.4 and 118.4 ms had no matching long
compositor paint and could include the delay to the first frame after the
counter reset. Native-view snapshots now expose first-frame delay and longest
interframe gap separately, with an availability flag for older bundles
(`.vm/bench/scroll-cycles-20260925-064032-compositor-repeats/`).

An opt-in `SUMMIT_COMPOSITOR_TIMING_TRACE=1` extension reports setup, damage,
clear, layer paint, and finish phases when a TextureMapper paint exceeds
30 ms. It built in a separate PGO-derived bundle, `bundle-q_th1ydp`; GCC's
stale profile data for only `ThreadedCompositor.cpp` was archived before the
rebuild. In two 20-burst trials on that diagnostic bundle, all 3,200 wheel
events were delivered and there was no interframe gap over 80 ms. The
longest interframe gaps were 74.3 and 59.4 ms, respectively, so the 246 ms
paint did not recur and its inner phase is still unknown. One 146.2 ms
first-frame delay demonstrates why the split counter matters. The first
trial's immediate final screenshot had a blank main feed even though it
started with visible posts; two subsequent 20-burst captures showed the feed
both immediately and after settling. This is an intermittent content
handoff symptom, not evidence that the diagnostic build improved scrolling.
The workstation launcher remains on the prior `bundle-k7atp8la`
(`.vm/bench/scroll-cycles-20260925-064832-paint-phases/`,
`.vm/bench/scroll-cycles-20260925-065132-settle-feed/`, and
`.vm/bench/scroll-cycles-20260925-065351-paint-phases-repeat/`).

A follow-up on the reported full Guardian article used the installed smooth
PGO bundle with opt-in media frame tracing. The article loaded and the cookie
panel was dismissed, but navigation through the page encountered its photo
lightbox and subscription overlay. A second visit reused the consented
profile. Neither visit brought the inline video into view or emitted a media
frame or codec trace, so these runs provide no new playback measurement.
The media path still rests on the controlled Guardian clip and the earlier
full-article PGO playback window above. Both browser instances were closed
after their screenshots and logs were saved
(`.vm/bench/guardian-live-smooth-pgo-20260925/` and
`.vm/bench/guardian-live-smooth-pgo-retry-20260925/`).

### Guardian inline video startup repaint backlog (25 September 2026)

The reported article's `#:~:text=A%20surprise%20three-hour%20meeting`
fragment lands directly above its self-hosted inline video. This provided a
repeatable live-page route that avoided accidental photo-lightbox navigation.
The installed `bundle-k7atp8la` selected NVDEC H.264 with status 0 and decoded
the 25 fps clip normally, but its first visit queued a repaint callback for
every decoded frame while Guardian page work delayed the WebProcess main
thread. In the first run, 157 of 2,229 callbacks waited over 33 ms, with a
2,172.5 ms maximum; 2,051 decoded sequences reached paint. After startup,
the next 1,800 frames had normal callback delay. A separate 30-second run
reproduced the startup burst: 71 of 739 callbacks over 33 ms, maximum
1,881.7 ms, and 649 sequences painted. These are callback-to-main-thread
latencies and paint traces, not monitor presentation measurements. Both runs
selected NVDEC and looped or reached the video end without a media error
(`.vm/bench/guardian-fragment-playback-20260925/` and
`.vm/bench/guardian-fragment-baseline-repeat-20260925/`).

`MediaPlayerPrivateHaiku` now coalesces pending video repaint callbacks. The
decoder continues following the media clock, but only one main-thread repaint
request waits at a time. If a frame arrives during that repaint, it enqueues
another request after the callback finishes. Older callbacks could not paint
their original images because the shared bitmap already contained the newest
decoded frame. An opt-in frame trace now reports the latest decoded sequence
for each coalesced request. The PGO rebuild archived only the stale
`MediaPlayerPrivateHaiku.cpp.gcda` profile and produced the separate
`bundle-7rru9cb6`.

On that bundle, the same full article had 2,232 decoded sequences, 2,134
repaint callbacks, 2,111 painted sequences, and four callbacks over 33 ms;
maximum callback delay was 38.7 ms. The shorter repeat had 744 decoded
sequences, 682 callbacks, 662 painted sequences, and five callbacks over
33 ms; maximum 39.6 ms. The callback count and queue percentiles are not
directly comparable with the old one-callback-per-decode trace, but both
candidate runs avoided the multi-second main-thread queue and the inline
video kept playing through its loop. Site ads and scripts still vary between
visits, so these results do not prove every Guardian playback stall is fixed
(`.vm/bench/guardian-fragment-coalesced-20260925/` and
`.vm/bench/guardian-fragment-coalesced-repeat-20260925/`).

Repeated `currentTime` seeks on the Guardian MP4 and Reddit HLS fixtures
completed with `seeked`, NVDEC status 0, no media error, and no delayed
50 ms heartbeat in either candidate run. The Reddit fixture ended at 12.7 s.
This checks the media seek path but does not reproduce the unidentified
Adobe ad's native progress-bar drag. The X399 desktop launcher now points
to `bundle-7rru9cb6`; its previous script is saved as
`Summit-current.pre-media-coalesce-20260925.sh`
(`.vm/bench/probe-20260925-072556-summit-coalesced-guardian-seek/` and
`.vm/bench/probe-20260925-072632-summit-coalesced-reddit-seek/`).

A ten-burst live `/r/popular/` scroll check on the installed candidate
delivered all 800 wheel events and had a median **58.7 active fps**. The
worst active interframe gap was 63.9 ms, and the largest native view queue
delay was 3.2 ms. The feed remained visible after settling. Several longer
gaps appeared only in the quarter-second animation-tail snapshots, after
the wheel burst's active interval; they do not establish a long active
scroll stall. No `SUMMIT_COMPOSITOR_TIMING_TRACE=1` paint phase exceeded its
30 ms reporting threshold in this run. The earlier intermittent 246 ms
compositor paint on Reddit remains unresolved
(`.vm/bench/scroll-cycles-20260925-072832-media-coalesce-reddit/`).

An extended 40-burst repeat delivered all 3,200 wheel events at a median
**58.77 active fps** and ended with visible feed content. One burst recorded
a 134.3 ms active-snapshot interframe gap, with only 0.1 ms native queue
delay. Its compositor trace had a 119.4 ms gap between frame starts near the
end of the burst, but no composition request during that interval and no
paint phase over 30 ms. The gap overlaps the burst's animation tail, so the
counters alone cannot assign the delay to active input or page work. It
differs from the earlier 246 ms layer-paint
stall and leaves the long-tail Reddit problem open
(`.vm/bench/scroll-cycles-20260925-073040-media-coalesce-reddit-long/`).

The installed media candidate completed a ten-iteration local Speedometer
3.1 run at the comparison viewport of **1280×887**, scoring
**8.475 ± 0.414** without measured foreign CPU contention. This is within
the spread of the prior smooth-PGO runs, 8.338 ± 0.398 and 8.544 ± 0.434,
and its point score remains above the fresh Firefox 155 run's
8.238 ± 0.368; their uncertainty intervals overlap. An earlier candidate
run at **1913×945** scored 8.451 ± 0.432 but is excluded from the
same-viewport comparison
(`.vm/bench/speedometer-20260925-073426-media-coalesce-full/` and
`.vm/bench/speedometer-20260925-073640-media-coalesce-matched/`).

### Wheel-burst completion snapshots (25 September 2026)

The earlier scroll-cycle harness read native-view frame counters after
polling Summit over SSH for burst completion. That poll arrived roughly
250–300 ms after the last notch and could include a quiet animation-tail
frame in its "active" result. Summit now saves a frame snapshot in the UI
thread when it receives the burst-completion message; `summitctl framestats`
returns it as `atScrollCompletion`, and `run-scroll-cycles.py` uses it when
available while keeping the later poll for comparison. In the four-burst
smoke run, snapshot elapsed time matched each 2.003–2.006 s delivery window
within 0.3 ms. The second burst had a 447.2 ms delay to its first frame,
but its longest subsequent interframe gap was 31.1 ms. That isolated startup
delay did not repeat in the longer run
(`.vm/bench/scroll-cycles-20260925-074313-completion-snapshot-smoke/`).

The diagnostic bundle `bundle-wekouxn2` uses exactly the same WebKit and
WebProcess binaries as the installed media candidate; only the native
browser and benchmark tool changed. In a 40-burst live Reddit run it
delivered all 3,200 wheel events, with **59.14 median active fps**, a
37.5 ms worst first-frame delay, a 42.4 ms worst active interframe gap,
and at most 4.0 ms native-view queue delay. Tail interframe gaps reached
536.6 ms after input ended. The earlier 134.3 ms "active" gap came from a
later poll and cannot be used as evidence of an input-time stall. The feed
remained visible in the final capture. Four compositor layer paints over
30 ms occurred during initial loading, before the bursts; the earlier
246 ms paint did not recur.
The desktop launcher now points to `bundle-wekouxn2`, with its predecessor
saved as `Summit-current.pre-scroll-completion-20260925.sh`
(`.vm/bench/scroll-cycles-20260925-074420-completion-snapshot-long/`).

On the installed bundle, a live `/r/popular/` 14-second cat video was
allowed to finish, then its native progress thumb was dragged backward
with the workstation pointer from 0:14 to 0:05. The player showed the
scrub preview at 0:05; pressing play advanced the displayed time to 0:10
five seconds later. The media log recorded the intermediate drag seeks and
a final 5.335 s request, with NVDEC H.264 selected at status 0. This
validates a real Reddit progress-bar drag for this video, but it does not
identify or reproduce the owner's Adobe ad, whose player may use a different
control path (`.vm/bench/reddit-live-native-drag-20260925/`).

### Reddit solid-color shader hitch (25 September 2026)

The single-burst scroll harness now reads the native frame counters saved
at wheel-delivery completion, as the cycle harness does. A 300-notch live
`/r/popular/` burst reached the current feed maximum after about 136
handled events; the later 164 events could not move the page. Its 400.2 ms
worst frame interval and 50.57 fps average therefore include feed-boundary
loading rather than continuous scrolling
(`.vm/bench/scroll-20260925-075409-current-300-completion/`).

Shorter 120-notch bursts stayed within the loaded feed and handled all 120
events. An opt-in `SUMMIT_LAYER_PAINT_TRACE=1` compositor trace isolated a
slow 219×5 solid-color layer. Its 19.3 ms `paintSelf` duration was entirely
in the contents draw, not its transform or texture-mapper setup
(`.vm/bench/scroll-20260925-080515-layer-paint-phases-reddit-120/`).
Tracing `TextureMapper::drawSolidColor` then showed that the first
`SolidColor | RoundedRectClip` shader creation took 11.6 ms during the
burst, while the subsequent GL draw took 0.1 ms. The run delivered 120/120
events at 58.92 fps, with a 35.4 ms worst native frame interval
(`.vm/bench/scroll-20260925-080947-solid-paint-trace-reddit-120/`).

The Haiku GL texture mapper now compiles this shader variant on the first
compositor paint for each context. In the follow-up run, it compiled during
initial page loading (20.9 and 25.3 ms in two contexts) and did not compile
during scrolling. All 120 events were handled at 58.55 fps, with a 36.8 ms
worst frame interval. This removes one identified scroll-time cause but
does not establish an overall frame-rate gain from these two noisy live
samples (`.vm/bench/scroll-20260925-081332-prewarm-rounded-solid-reddit-120/`).

At the same 1280×887 viewport, the prewarm bundle scored **8.574 ± 0.468**
in a ten-iteration local Speedometer 3.1 run without measured foreign CPU
contention. The previous installed bundle scored 8.475 ± 0.414; the
intervals overlap. The X399 desktop launcher now points to
`bundle-glk_tobh`, with the earlier `bundle-wekouxn2` launcher saved as
`Summit-current.pre-rounded-solid-prewarm-20260925.sh`
(`.vm/bench/speedometer-20260925-081513-rounded-solid-prewarm-matched/`).

### Guardian visible playback on the installed prewarm build

A fresh visit to the reported Guardian article on `bundle-glk_tobh` selected
NVDEC H.264 and decoded three 19.56-second loops without a media error. A
trace-only visit with the consent panel still covering the video had a
996.3 ms repaint callback delay at a loop boundary; it cannot measure
visible playback (`.vm/bench/scroll-20260925-081908-guardian-fragment-current-media/`).

In a separate visit, the consent panel was dismissed and the inline video
was fully visible. A 15.9-second sequence of 120 VNC samples of the video
center changed at every sample, with at most 177 ms between captures. Across
the visit, 1,784 decoded frames had a 4.4 ms median decode time and 13.6 ms
95th-percentile repaint-queue delay; five queue delays exceeded 33 ms, with
a 39.3 ms maximum. The 1,764 paint traces had an 8.9 ms median decoded-frame
age and 37.0 ms maximum. This checks visible motion and the decode-to-paint
path, but the sampled screenshots do not measure the display's actual frame
rate (`.vm/bench/guardian-visible-current-20260925/`).

### CPU tile-size sweep on the installed build

`WEBKIT_LAYERS_TILE_SIZE` permits a tile-size sweep without rebuilding. On
`bundle-glk_tobh`, the 400-card page at a 1280×887 viewport completed 300
scroll frames in each uncontended run:

| CPU tile size | Scroll fps | 95th-percentile interval | Long frames (>33 ms) |
| --- | ---: | ---: | ---: |
| 256×256 (default), first / repeat | 59.80 / 59.76 | 17 / 17 ms | 0 / 0 |
| 512×512, first / repeat | 57.64 / 57.74 | 26 / 25 ms | 0 / 0 |
| 128×128 | 59.58 | 18 ms | 0 |

The larger tiles repeatably cost about two frames per second on this
controlled page, while smaller tiles show no useful gain. The default remains
256×256. This page is a paint/scroll control and cannot replace a live Reddit
check for any future tile-policy change
(`.vm/bench/probe-20260925-082732-summit-tile-256-a/`,
`.vm/bench/probe-20260925-082806-summit-tile-512/`,
`.vm/bench/probe-20260925-082843-summit-tile-128/`,
`.vm/bench/probe-20260925-082920-summit-tile-256-b/`, and
`.vm/bench/probe-20260925-082953-summit-tile-512-b/`).

### Frame-timer alignment trial

The matched current Preact trace spent a median 24 ms waiting for its first
animation callback in add-items steps and 14 ms in complete-items steps,
versus 11 and 2 ms in the saved Firefox trace. Haiku uses WebKit's one-shot
rendering-update timer because it has no display refresh monitor. A separate
PGO bundle, `bundle-x2jmvbd3`, tested an opt-in alignment of its 60 Hz timer
to a shared monotonic cadence (`SUMMIT_ALIGNED_RENDER_TIMER=1`).

The alignment reduced the median wait for Preact's first animation callback
from 15 to 5 ms in complete-items steps, but Speedometer starts measuring
after that callback. The measured step times and focused eight-iteration
scores were effectively unchanged: 16.849 ± 1.412 off and 16.584 ± 1.302
on. In ten full iterations on the same bundle, alignment scored
8.547 ± 0.408 versus 8.625 ± 0.453 with it off. These intervals overlap and
do not support a benchmark gain. The experiment was removed; the engine
patch digest returned exactly to its pre-trial value, and the workstation
launcher stayed on `bundle-glk_tobh`
(`.vm/bench/speedometer-20260925-083157-preact-raf-current-pgo/`,
`.vm/bench/speedometer-20260925-083743-aligned-timer-preact-off-a/`,
`.vm/bench/speedometer-20260925-083832-aligned-timer-preact-on/`,
`.vm/bench/speedometer-20260925-083950-aligned-render-full-on/`, and
`.vm/bench/speedometer-20260925-084126-aligned-render-full-off/`).

### One CPU paint worker on the current PGO bundle

The earlier worker-count sweep measured scrolling with one worker but had no
one-worker Speedometer result. On the installed `bundle-glk_tobh`, forcing
`WEBKIT_SKIA_CPU_PAINTING_THREADS=1` scored **8.508 ± 0.409** in ten
uncontended iterations at the same 1280×887 viewport. The normal two-worker
bundle scored **8.574 ± 0.468**. The intervals overlap and the one-worker
point score is lower. Combined with the earlier one-worker scroll result
(56.91 fps and 27 intervals over 33 ms on the 400-card page), this gives no
reason to change the two-worker default or add a dynamic one/two-worker
policy (`.vm/bench/speedometer-20260925-084855-one-cpu-paint-worker-current/`).

### Absent pseudo-style shortcut trial

An opt-in Haiku trial skipped pseudo-element resolution calls when neither
the new nor existing style had that pseudo-element. `::first-line` and
`::first-letter` retained their ancestor-resolution path. The separate PGO
bundle `bundle-b2dgls9w` ran twenty focused Preact/Svelte iterations in an
off/on/off/on sequence at 1280×887. Scores were **17.917**, **18.369**,
**18.545**, and **18.621**, each with roughly ±0.5 uncertainty. The first
on run improved both suites by about 1 ms, but the following off run improved
further. The shortcut has no repeatable gain above run-to-run drift. It was
removed, returning the engine patch to its exact pre-trial digest; the
workstation launcher remains on `bundle-glk_tobh`
(`.vm/bench/speedometer-20260925-085527-absent-pseudo-off-a/`,
`.vm/bench/speedometer-20260925-085615-absent-pseudo-on/`,
`.vm/bench/speedometer-20260925-085703-absent-pseudo-off-b/`, and
`.vm/bench/speedometer-20260925-085757-absent-pseudo-on-b/`).

### TextureMapper paint-trace cleanup trial

The rounded-solid shader diagnosis left opt-in timing hooks in
`TextureMapperLayer` and `TextureMapper`. A PGO rebuild removed these hooks
while retaining the validated shader prewarm in `TextureMapper::beginPainting`.
The separate `bundle-uq74d6ow` passed a 300-frame 400-card scroll probe at
**59.61 fps**, versus **59.74 fps** on the installed `bundle-glk_tobh`; both
had zero frames over 33 ms. In adjacent 120-notch live Reddit runs, both
screenshots showed movement through the feed. The installed and trial bundles
presented **58.64** and **58.58 fps**, respectively, with two frames over
33 ms each. The worst intervals were 47.7 and 37.5 ms, too little evidence
from one variable live feed to establish an improvement.

The trial bundle's two uncontended ten-iteration Speedometer 3.1 runs scored
**8.206 ± 0.399** and **8.266 ± 0.401**, versus **8.387 ± 0.399** for the
installed bundle between them at the same 1280×887 viewport. The confidence
intervals overlap, but both trial point scores were lower and neither scroll
test showed a useful gain. The cleanup was reverted. The launcher stays on
`bundle-glk_tobh`, and the engine patch digest is again
`9d1c310a8c93e81b2baecfc9370f122badae68c86e3ff4c896ea9bd2db68c539`
(`.vm/bench/probe-20260925-090556-summit-paint-trace-cleanup-control/`,
`.vm/bench/probe-20260925-090630-summit-paint-trace-cleanup-candidate/`,
`.vm/bench/speedometer-20260925-090705-paint-trace-cleanup-candidate/`,
`.vm/bench/speedometer-20260925-090844-paint-trace-cleanup-control/`,
`.vm/bench/speedometer-20260925-091020-paint-trace-cleanup-candidate-b/`,
`.vm/bench/scroll-20260925-091157-paint-trace-cleanup-control/`, and
`.vm/bench/scroll-20260925-091310-paint-trace-cleanup-candidate/`).

### Longer matched Speedometer comparison and Reddit page work

At a 1280×887 content viewport, the installed `bundle-glk_tobh` scored
**8.561 ± 0.125** in 30 uncontended Speedometer 3.1 iterations. Firefox 155
scored **8.405 ± 0.179** in the same local benchmark and viewport. Summit's
point score is higher, but the reported uncertainty intervals overlap.
Firefox's run included isolated Svelte and Lit steps of 892 and 2,506 ms;
their suite means should not be read as stable per-step differences. The
repeated gaps favoring Firefox are Preact (46.8 versus Summit's 63.4 ms),
CodeMirror (50.8 versus 70.6 ms), and Web Components (58.6 versus 71.9 ms).
Summit was faster in several other suites. These runs strengthen the baseline
without establishing a statistically clear overall lead
(`.vm/bench/speedometer-20260925-091757-current-thirty-iterations/` and
`.vm/bench/firefox-speedometer-20260925-092147-current-thirty-iterations/`).

In a 120-notch live Reddit burst with `SUMMIT_PAGE_UPDATE_TRACE=2`, all wheel
input was delivered, screenshots showed movement to later posts, and the
native view reached **58.90 frames/s**. An earlier level-1 run also reported
58.91 frames/s, but its per-renderer logging left the feed blank near its
end; that run is unsuitable as a content-readiness measurement. In the
level-2 burst, 15 layouts over 10 ms totaled 3.20 s,
mostly render-tree work. The corrected page-update parser found four updates
over 33 ms totaling 1.81 s, including 688 ms of initial layout, 497 ms of
intersection-observer processing, 403 ms of final layout, and 209 ms of
second layout. These page-update phases overlap the separately reported
layouts and must not be added to them. The native view queue stayed at or
below 0.3 ms, so asynchronous composition kept moving the visible feed while the
WebProcess did this work. Main-thread layout and observer work remain
plausible constraints on content readiness and concurrent video presentation;
the scrolling compositor itself is already close to display cadence
(`.vm/bench/scroll-20260925-092700-page-update-trace-active-120/` and
`.vm/bench/scroll-20260925-092825-page-update-light-active-120/`).

The corrected trace parser enabled a level-2 A/B/A check of the Haiku
extended grid integration on the installed PGO bundle. Two default-path
120-notch bursts reached **58.90** and **58.55 fps**, with two and one frame
over 33 ms; slow layouts totaled **3.20** and **2.82 s**. Between them, the
`SUMMIT_EXTENDED_GRID_INTEGRATION=0` legacy run reached **57.26 fps**, with
four frames over 33 ms and **1.44 s** of slow layout. All three final
screenshots showed later feed posts. The live feed and ad content varied, so
these are directional rather than a controlled layout microbenchmark. The
modern grid path still favors visible scroll pacing while doing more main
thread layout work. It remains enabled; reducing that layout cost without
losing scroll pacing is the next useful grid target
(`.vm/bench/scroll-20260925-093211-grid-legacy-light-active-120/` and
`.vm/bench/scroll-20260925-093308-grid-modern-light-active-120-repeat/`).

### Guardian playback recheck on the installed bundle

The standalone Guardian clip still completed and looped on `bundle-glk_tobh`
with NVDEC H.264 selected at status 0. Among 683 decoded frames, the median
decode time was 2.3 ms, maximum repaint queue delay was 0.4 ms, and 681
paint traces had a 16.8 ms maximum decoded-frame age. It reported no media
error (`.vm/bench/probe-20260925-093639-summit-guardian-nvdec-current/`).

The full article was then opened in the previously consented profile. A
Guardian sign-in panel initially hid the later article text; closing its
visible X restored the inline video. The reusable
`tools/bench/vnc-input.py` helper delivered the pointer click and scrollbar
drag without storing the VNC password in artifacts. Before and after
screenshots show the video fully visible. In a 25-second window with **no
screen captures during playback**, one NVDEC H.264 player decoded 627 frames
and painted 624, including one loop. Decode time was 4.8 ms median and
11.9 ms maximum. Repaint queue delay was 7.5 ms at the 95th percentile and
24.2 ms maximum, with none above 33 ms. Painted-frame age was 8.9 ms median,
18.1 ms at the 95th percentile, and 36.5 ms maximum; four ages exceeded
33 ms and none exceeded 40 ms. The end-of-buffer messages coincided with
the normal loop. These native traces support sustained decode-to-paint
playback on the reported page, while leaving exact monitor presentation
cadence and rarer intermittent stalls unmeasured
(`.vm/bench/guardian-live-20260925-current/`).

### Four CPU tile painters on the current PGO bundle

Forcing `WEBKIT_SKIA_CPU_PAINTING_THREADS=4` on the installed bundle
completed the 300-frame, 400-card scroll probe at **59.82 fps**, with an
18 ms maximum interval and no frame over 33 ms. An earlier
two-worker control was **59.74 fps** with the same maximum and no long
frames. At the matched 1280×887 viewport, four workers scored
**8.579 ± 0.467** in ten uncontended Speedometer 3.1 iterations, effectively
the same as the default two-worker **8.574 ± 0.468** result. A live
120-notch Reddit burst with four workers showed movement to later posts at
**58.88 fps**, with one interval over 33 ms and a 36.4 ms maximum; the
recent two-worker bursts were about 58.6–58.9 fps. The page showed some
loading placeholders after the burst, so this single live run cannot
measure content readiness. Four workers give no repeatable rendering or
benchmark gain here. The default remains two, scaling down with the
detected processor count on smaller systems
(`.vm/bench/probe-20260925-095613-summit-four-cpu-paint-workers-current/`,
`.vm/bench/speedometer-20260925-095648-four-cpu-paint-workers-current/`, and
`.vm/bench/scroll-20260925-095828-four-cpu-paint-workers-reddit-120/`).

### Fully downloaded video ranges and seek controls

The Haiku media backend downloads each supported remote MP4, or the complete
finite byte-range HLS rendition, before opening its Media Kit tracks. It had
nevertheless returned an empty `video.buffered` range for every video. The
backend now publishes `[0, duration]` after a successful load, on the page
thread before media events, and clears it when a load is cancelled or
replaced. The diagnostic media pages record the browser-visible buffered
range as well as seekable time.

On the new `bundle-_3yrr8z2`, Reddit's finite HLS clip reported buffered
`[0, 12.70]` at `loadedmetadata`, sought to 8 seconds, emitted `seeked`,
and played to the end without a media error. The Guardian MP4 reported
buffered `[0, 19.56]`, selected NVDEC H.264 at status 0, and looped without
a media error. Its 22-second trace contained 684 decoded frames and 683
paints, with no paint age above 33 ms. These checks verify the media API and
controlled playback; the original Adobe ad's custom timeline remains
unverified because its URL was not available
(`.vm/bench/probe-20260925-100811-summit-buffered-reddit-hls/` and
`.vm/bench/probe-20260925-100900-summit-buffered-guardian-nvdec/`).

At the matched 1280×887 viewport, the candidate scored **8.391 ± 0.117**
over 30 uncontended Speedometer 3.1 iterations. The prior bundle scored
**8.500 ± 0.123** in a same-session repeat, versus **8.561 ± 0.125**
earlier; all three intervals overlap. Firefox scored **8.405 ± 0.179** in
its saved 30-iteration run. The candidate's 300-frame 400-card scroll probe
reached **59.50 fps**, with an 18 ms maximum interval and no frame over
33 ms, close to the prior **59.74 fps** control. The X399 desktop launcher
now points to `bundle-_3yrr8z2`, and its previous script is saved as
`Summit-current.pre-buffered-media-20260925.sh`
(`.vm/bench/speedometer-20260925-101601-buffered-media-pgo-thirty/`,
`.vm/bench/speedometer-20260925-101948-prior-pgo-thirty-bracket/`, and
`.vm/bench/probe-20260925-101452-summit-buffered-media-scroll-400/`).

### NanoKVM capture and grid row sizing

`tools/bench/nanokvm-capture.py` can authenticate to the workstation's
NanoKVM without recording credentials, query capture status, and save a timed
MJPEG frame sequence for future presentation-cadence checks. Its control
connection succeeds, but direct, H.264, and MJPEG capture currently report
`No image captured`. An HDMI reset succeeded without restoring a frame, so
there is no independent monitor-cadence measurement yet. The benchmark
numbers below use browser and native-view timing rather than NanoKVM video.

The deterministic `grid-reflow.html` page changes text inside 60 two-item
grids and forces layout 50 times. Modern-grid A/B/A runs measured **11.12**
and **11.50 ms** per forced layout, versus **9.14 ms** with
`SUMMIT_EXTENDED_GRID_INTEGRATION=0`; representative geometry matched.
Fixed-height rows improved both paths to **9.54** and **7.68 ms**. Coverage
traces confirmed that the default runs used the modern formatter for all
fixture grids. `SUMMIT_GRID_PHASE_TRACE=1` showed that row sizing accounted
for about **12.57 ms per 100 grid layouts**, out of **15.03 ms** for all
modern-grid phases. The integration path laid out each item's intrinsic
block height twice per sizing pass: once for min-content and once for
max-content contribution, although both use the same block-size layout
operation (`.vm/bench/probe-20260925-103517-summit-grid-reflow-modern-a/`,
`.vm/bench/probe-20260925-103552-summit-grid-reflow-legacy/`,
`.vm/bench/probe-20260925-103628-summit-grid-reflow-modern-b/`, and
`.vm/bench/probe-20260925-105102-summit-grid-sizer-auto-trace/`).

The block-axis contribution is now cached only for one track-sizing call,
keyed by grid item and inline constraint. The trace dropped from four
height layouts per two-item grid to two. In an uncontended, untraced
120-grid A/B/A run, the controls measured **22.30** and **21.92 ms** per
forced layout, and the candidate measured **20.92 ms**, about a **5%**
reduction. Initial and final geometry matched exactly in all three runs.
The broader `grid-space-between.html` output also matched the control
exactly, including overflow, repeat tracks, margins, and dynamic fallback.
Both diagnostic comparison bundles had the same partial PGO coverage for
edited unified layout units after their stale profile files were archived;
a fresh profile training pass remains useful for a later release build.
(`.vm/bench/probe-20260925-110734-summit-grid-cache-control-a/`,
`.vm/bench/probe-20260925-110805-summit-grid-cache-candidate/`,
`.vm/bench/probe-20260925-110835-summit-grid-cache-control-b/`,
`.vm/bench/probe-20260925-110926-summit-grid-cache-geometry/`, and
`.vm/bench/probe-20260925-111007-summit-grid-cache-geometry-control/`).

At the same 1267×769 content viewport, the cache bundle scored
**8.303 ± 0.399** over ten full Speedometer 3.1 iterations; the same-session
control scored **8.451 ± 0.387**. Their confidence intervals overlap. The
candidate's 300-frame, 400-card scroll ran at **60.05 fps** with a 17 ms
maximum interval and no long frames, versus **59.77 fps** and an 18 ms
maximum in the control. A live 120-notch `/r/popular/` burst reached
**58.92 fps**, with one 33.3 ms interval and later feed content visible in
the final screenshot. The Guardian fixture again selected NVDEC H.264,
exposed its full buffered range, looped, and reported no error; the Reddit
HLS fixture sought to eight seconds, emitted `seeked`, and ended without
error. The workstation desktop launcher now points to `bundle-ydvalf4q`,
with the previous script saved as
`Summit-current.pre-grid-cache-20260925.sh`
(`.vm/bench/speedometer-20260925-111051-grid-cache-candidate/`,
`.vm/bench/speedometer-20260925-111239-grid-cache-control/`,
`.vm/bench/probe-20260925-111421-summit-grid-cache-scroll-400/`,
`.vm/bench/probe-20260925-111509-summit-grid-cache-scroll-control/`,
`.vm/bench/scroll-20260925-111616-grid-cache-reddit-120/`,
`.vm/bench/probe-20260925-111816-summit-grid-cache-guardian/`, and
`.vm/bench/probe-20260925-111942-summit-grid-cache-reddit-hls/`).

### Persistent grid contribution cache trial

With the one-pass contribution cache installed, a new trace found that 100
of every 200 intrinsic-height requests in `grid-reflow.html` arrived with
items that did not need layout. The renderer's grid-area width had been
cleared by earlier column-sizing work, so that property alone could not
validate reuse. A trial cache on `RenderGrid` therefore stored the measured
inline constraint and contribution, invalidating entries for dirty items and
on grid style changes. The trace showed roughly half as many forced height
layouts, and the trial matched the control's initial and final fixture
geometry (`.vm/bench/probe-20260925-112800-summit-grid-reuse-potential/`
and `.vm/bench/probe-20260925-113943-summit-grid-persistent-cache-trace/`).

The untraced 120-grid A/B/A comparison did not support shipping the larger
cache: the controls averaged **20.40** and **20.54 ms** per forced layout,
while the trial averaged **21.26 ms**. All runs were uncontended and the
sampled geometry matched. Rebuilding the trial also required archiving three
stale PGO files for edited rendering units, so the timing does not isolate
cache lookup cost from changed profile coverage. The persistent cache was
removed; the installed launcher remains on `bundle-ydvalf4q`. The fixture's
new `varyColumns`, `varyStatic`, and `geometryEachPass` controls completed
and produced two distinct geometry states for future correctness checks
(`.vm/bench/probe-20260925-114040-summit-persistent-cache-control-a/`,
`.vm/bench/probe-20260925-114114-summit-persistent-cache-candidate/`,
`.vm/bench/probe-20260925-114149-summit-persistent-cache-control-b/`,
and `.vm/bench/probe-20260925-114342-summit-grid-variation-fixture/`).

### Inline storage trial for one-pass grid contributions

A second trial kept the safe one-pass cache but placed up to four item
contributions in inline vector storage, using a hash table only for larger
grids. The trial removed a per-grid hash allocation without changing the
number of intrinsic-height layouts. Its 120-grid A/B/A means were **20.64**
and **20.32 ms** for the controls and **20.62 ms** for the trial, with matching
geometry and no workstation contention. The whole-browser check was worse:
at the matched 1280×887 viewport, ten Speedometer 3.1 iterations scored
**7.762 ± 0.328** on the trial versus **8.457 ± 0.417** on the installed
control. The earlier diagnostic bundle with the same missing layout-unit PGO
profiles scored **8.427 ± 0.414**, so those missing profiles alone do not
explain the trial's lower score. The inline-storage change was removed and
the engine patch returned to its pre-trial hash; the workstation launcher
remains on `bundle-ydvalf4q`
(`.vm/bench/probe-20260925-115559-summit-inline-cache-control-a/`,
`.vm/bench/probe-20260925-115631-summit-inline-cache-candidate/`,
`.vm/bench/probe-20260925-115711-summit-inline-cache-control-b/`,
`.vm/bench/speedometer-20260925-115852-inline-cache-candidate/`,
`.vm/bench/speedometer-20260925-120052-inline-cache-control/`, and
`.vm/bench/speedometer-20260925-120340-grid-trace-baseline-pgo-control/`).

### Web Components frame-phase comparison

The current 30-iteration full benchmark puts Summit's Web Components suite
at 73.9 ms versus Firefox's 58.6 ms. In a matched 1280×887, 20-iteration
focused diagnostic, the gap narrowed to **70.5 versus 66.35 ms**. For its
`Adding100Items` step, Summit waited a mean 16.45 ms for the first animation
frame and spent 17.55 ms in the synchronous callback; Firefox spent 21.25
and 22.8 ms. The timer after the second animation-frame callback took
22.9 ms in Summit versus 12.05 ms in Firefox. These phases partly cancel in
the focused suite, and this diagnostic does not reproduce the full-run gap.
It does not support a global timer change, particularly given the earlier
neutral timer-alignment trial
(`.vm/bench/speedometer-20260925-120644-webcomponents-raf-summit/` and
`.vm/bench/firefox-speedometer-20260925-120742-webcomponents-raf-firefox/`).

### Reddit packaged-media track error

An opt-in codec trace on a live 120-notch `/r/popular/` scroll associated
`MediaExtractor::CreateDecoder can't create decoder for stream 2` with
`packaged-media.redd.it`. The first run still selected AAC and NVDEC H.264
and reached **58.83 active fps**. The trace now reports source host, file
track count, and selected track index, and guards against a null `TrackAt`
result. It excludes URL paths and query strings from the log.

The repeat found a three-track packaged-media file. Media Kit logged the
decoder error for stream **2**, while Summit selected AAC on stream **1**
and NVDEC H.264 on stream **0**, both with success status. It reached
**58.57 active fps**, with two intervals above 33 ms and a 45.4 ms worst
interval. The trace did not report an unavailable, unreadable, or rejected
track. Stream 2 is therefore outside the selected audio/video pair in this
run; its precise content type remains unknown. This does not establish the
cause of the owner's intermittent video issue or reproduce the unidentified
Adobe ad's seek controls. The workstation launcher remains on the validated
`bundle-ydvalf4q`; `bundle-kdx8jkqj` is diagnostic
(`.vm/bench/scroll-20260925-121506-reddit-rejected-media-trace/` and
`.vm/bench/scroll-20260925-122434-media-track-count/`).

### Installed grid-cache Speedometer repeat

The installed `bundle-ydvalf4q` completed 30 local Speedometer 3.1
iterations without measured foreign CPU contention. At the matched
1280×887 content viewport it scored **8.377 ± 0.121**, versus Firefox 155's
**8.405 ± 0.179** at that viewport. The 0.028 point gap is smaller than
either confidence interval, so this result does not establish a difference.
The buffered-media predecessor scored **8.391 ± 0.117** in its matched
30-iteration run. The grid contribution cache has not produced a measured
whole-browser gain, though it reduced the grid fixture's row-sizing work.

An initial repeat scored **8.364 ± 0.126**, but its recorded content viewport
was 1280×854 because the window frame was 33 pixels too short. It is kept as
a diagnostic and excluded from the matched comparison
(`.vm/bench/speedometer-20260925-122812-installed-grid-thirty/` and
`.vm/bench/speedometer-20260925-123222-installed-grid-thirty-matched/`).

### Media audio output lifetime and mute state

A 40-burst alternating Reddit run on the prior installed bundle exposed
repeated Media Kit buffer failures and NVDEC timeouts. One traced run logged
6,334 `SoundPlayNode::FillNextBuffer` failures, 27 buffer-group allocation
failures, and 144 `nvdec: the decoder did not answer` messages. A video-only
player's `cancelLoad` took 62.8 seconds, mostly waiting for its decoder
thread. The burst completion snapshots still showed a 59.57 median active
fps, but one burst had a 495 ms first-frame delay. The long cancellation
ended during that run; its log alone does not locate when the wait began
(`.vm/bench/scroll-cycles-20260925-123914-installed-grid-long-reddit/` and
`.vm/bench/scroll-cycles-20260925-124341-media-lifecycle-long-reddit/`).

The Haiku backend previously constructed `BSoundPlayer` while identifying
every audio track, including paused and muted videos. It also had no
`setMuted(bool)` override. It now creates audio output when audible playback
starts, releases it on pause or mute, and seeks the audio track to the video
clock when sound is enabled later. The fixed eight-video preload probe loaded
all eight clips in both builds. The prior bundle attempted eight audio
buffer groups during preload. The first lazy-output candidate attempted
only two when two clips played; the mute-aware bundle attempted **zero**
while all clips stayed muted. Pausing and resuming the first clip advanced
its current time from about 2.25 to 4.74 seconds. Unmuting it at 3 seconds
created one sound player, and remuting released it at 4.5 seconds. The
workstation's Media Kit still reported one buffer-group failure for that
audible interval, also seen in a single-video prior-bundle control. Muted
Guardian playback and Reddit HLS seeking made no audio allocation and
reported no media or NVDEC error
(`.vm/bench/probe-20260925-130717-summit-preload-eight-control/`,
`.vm/bench/probe-20260925-133531-summit-mute-aware-pgo-restored-preload/`,
`.vm/bench/probe-20260925-132223-summit-mute-aware-unmute/`,
`.vm/bench/probe-20260925-132310-summit-mute-aware-reddit-seek/`, and
`.vm/bench/probe-20260925-132355-summit-mute-aware-guardian-loop/`).

A second 40-burst Reddit control and mute-aware candidate each loaded 142
media players and delivered all 3,200 wheel events. The control logged 73
buffer-group failures and 322 audio buffer failures; the candidate logged
**zero of either**, with no NVDEC timeout in those two visits. Median active
scroll rate was 58.61 versus 59.03 fps, and worst active interframe gap was
810.0 versus 46.8 ms. Live feed content and Media Kit state varied across
visits, so the frame-rate difference is directional; the fixed preload probe
isolates the audio allocation reduction
(`.vm/bench/scroll-cycles-20260925-130334-media-lifecycle-control-repeat/`
and `.vm/bench/scroll-cycles-20260925-132458-mute-aware-long-reddit/`).

After restoring the trained profile for the unified WebCore unit whose
inline method locations had changed, the mute-aware bundle scored
**8.520 ± 0.129** over 30 uncontended Speedometer iterations at 1280×887.
Firefox 155 scored **8.405 ± 0.179** at that viewport; the intervals overlap.
The first mute-aware build without that unit's profile scored 8.236 ± 0.150,
so it was not installed. The workstation launcher now points to
`bundle-swpotbhy`, and the previous launcher is saved as
`Summit-current.pre-muted-audio-20260925.sh`
(`.vm/bench/speedometer-20260925-132800-mute-aware-thirty/` and
`.vm/bench/speedometer-20260925-133616-mute-aware-pgo-restored-thirty/`).

### Park NVDEC decoders for paused preload videos

Haiku's [`BMediaFile::TrackAt()`](https://github.com/haiku/haiku/blob/master/src/kits/media/MediaFile.cpp#L209-L248)
constructs a track and its decoder, while `ReleaseTrack()` destroys the
track. Summit kept that decoder even when a page preloaded a video and left
it paused. The backend now retains the duration, video size, and track index
but releases the decoder in that state. It reacquires the track when playback
or a paused seek needs decoded frames. It keeps a decoder after a video has
played, so this change specifically reduces paused preload residency.

In the fixed eight-video probe, all eight clips still reached ready state.
The trace counted **eight parked decoders** and **two reacquisitions** for the
two clips played; pausing and resuming the first clip advanced from about
2.25 to 4.74 seconds. The Reddit HLS probe sought to eight seconds while
paused, emitted `seeked`, and reached 12.7 seconds with NVDEC and no error.
The Guardian clip looped with NVDEC and no media error
(`.vm/bench/probe-20260925-134806-summit-parked-video-preload/`,
`.vm/bench/probe-20260925-134854-summit-parked-video-reddit-seek/`, and
`.vm/bench/probe-20260925-134938-summit-parked-video-guardian-loop/`).

A 40-burst live Reddit run delivered all 3,200 wheel events, loaded 156
media players, parked 135 video decoders, and reacquired 100. It logged no
NVDEC timeout or audio buffer failure. Median active scroll rate was
**58.705 fps**, and the worst first-frame delay was **42.8 ms**. The prior
mute-aware visit loaded 142 players at 59.03 median fps with a 162.1 ms
worst first-frame delay. Feed content differed, so the frame difference is
directional; the fixed probe proves the decoder lifetime change
(`.vm/bench/scroll-cycles-20260925-135040-parked-video-long-reddit/`).

Matched 1280×887, uncontended 30-iteration Speedometer runs in an A/B/A
sequence scored **8.362 ± 0.127** for the parked-decoder bundle,
**8.342 ± 0.127** for the installed mute-aware control, and
**8.601 ± 0.132** for the parked-decoder repeat. The spread does not prove a
whole-browser gain or regression. The workstation launcher now points to
`bundle-ggja7ufo`; its predecessor is saved as
`Summit-current.pre-parked-video-20260925.sh`
(`.vm/bench/speedometer-20260925-135345-parked-video-thirty/`,
`.vm/bench/speedometer-20260925-135744-parked-video-control-thirty/`, and
`.vm/bench/speedometer-20260925-140133-parked-video-repeat-thirty/`).

### Workstation audio service recovery

An unmuted clip still produced `BBufferGroup` and `SoundPlayNode` failures
after Summit began allocating audio output only while needed. The failure
also reproduced with Haiku's own `media_client test`, outside Summit. A
standalone `BSoundPlayer` using an explicit 3,840-byte, 48 kHz stereo buffer
likewise failed to register its buffer group and flooded `RequestBuffer`
errors even though `InitCheck()` returned success. The media roster reported
an audio output and positive suggested buffer sizes. The zero-byte error from
`media_client` therefore did not establish that Summit's decoded audio format
was invalid; the explicit-size failure showed a separate Media Kit problem.
Haiku's [`SoundPlayNode::AllocateBuffers()`](https://github.com/haiku/haiku/blob/master/src/kits/media/SoundPlayNode.cpp#L757-L780)
creates this group and its [`FillNextBuffer()`](https://github.com/haiku/haiku/blob/master/src/kits/media/SoundPlayNode.cpp#L784-L797)
prints the repeated request errors when it cannot get a buffer.

With no browser or media player running, restarting the workstation's media
services restored the standalone sound player. The installed Summit bundle
then passed the eight-video preload and unmute probe: all eight clips loaded,
the first resumed past five seconds, one audio output was created on unmute
and released on remute, and there were **no** buffer-group, sound-node, NVDEC,
or media errors. The workstation again has one `media_server` and one
`media_addon_server` process. This recovery may be temporary; repeat the
standalone test if audible playback regresses
(`.vm/bench/probe-20260925-141144-summit-media-audio-after-server-restart/`).

A fresh visit to the reported Guardian article with the saved consent profile
landed on the inline video. It selected NVDEC H.264 and reached its 19.56-second
end. The media trace recorded 465 timed decoded frames: 4.8 ms median decode,
11.0 ms at the 95th percentile, and 15.4 ms maximum. Repaint queue delay was
18.3 ms at the 95th percentile, 38.5 ms maximum, with three delays above
33 ms. There were no NVDEC timeouts or Media Kit buffer errors. A second
visit played only about two seconds of the clip during a 30-second window,
so it does not replicate the full playback result. The UI's `loadOutcome`
read `process-exited` from the first sample at ten seconds, but three
WebProcess teams remained active through 30 seconds and no new crash report
appeared; that field alone does not identify a video-end crash. These are
decode-to-paint timings, not measured monitor presentation cadence
(`.vm/bench/guardian-article-audio-recovered-20260925/` and
`.vm/bench/guardian-article-audio-recovered-repeat-20260925/`).

### Paused playback and scroll benchmark correction

After a clip has started, pausing it for two seconds now parks its NVDEC
decoder. The next play request reacquires the track, seeks to the saved time,
and resumes decoding. In the fixed eight-video probe, all eight clips loaded,
three decoders parked after playback, and three resumed; audio output was
created and released once, with no NVDEC or buffer-group error. The Guardian
clip and a Reddit HLS clip both accepted three rapid seeks while paused and
resumed from the final seek position (`.vm/bench/probe-20260925-142546-summit-parked-after-pause-eight-unmute/`,
`.vm/bench/probe-20260925-142725-summit-parked-after-pause-guardian-seek/`,
and `.vm/bench/probe-20260925-142817-summit-parked-after-pause-reddit-seek/`).

An initial direct-bundle Reddit scroll probe reported zero frames because the
benchmark omitted `SUMMIT_SCROLL_REFRESH_TIMER=16`, which the installed
desktop launcher already sets. The scrolling tree accepted wheel events but
its smooth animation could not advance without refresh ticks. A Wikipedia
control moved 730 px and presented 17 frames over 0.3 seconds when the timer
was enabled. The scroll harnesses now default to the installed launch setting;
`--env SUMMIT_SCROLL_REFRESH_TIMER=0` still selects the control case. The
earlier zero-frame direct-bundle runs are invalid as scrolling comparisons
(`.vm/bench/scroll-20260925-151249-first-nav-wheel-webprocess-wikipedia/` and
`.vm/bench/scroll-20260925-151953-timer16-wikipedia/`).

With the installed refresh setting, the paused-decoder candidate delivered
all 320 wheel events across eight live Reddit down/up bursts. The scroll
position ranged from 0 to 4,800 px and the screenshots changed. Active
presentation was 57.49–60.34 fps, median 58.35 fps, with a 54.3 ms worst
completed gap. Nine repeated MediaExtractor stream-2 decoder allocation
errors occurred during the feed visit; the trace does not identify that
stream's media type or establish whether a visible clip failed. This run
does not reproduce the unidentified Adobe ad's seek drag failure
(`.vm/bench/scroll-cycles-20260925-152109-paused-decoder-reddit-timer16/`).

The `process-exited` tab outcome on an initial navigation was caused by
`PageClient::processWillSwap()` calling `processDidExit()` by default.
The Haiku view now preserves the pending load during a normal navigation
process swap. A fresh Wikipedia navigation reported `loadOutcome=succeeded`
with the override. The earlier Guardian `process-exited` field therefore
cannot be taken as evidence of a WebProcess crash.

The final PGO bundle is `bundle-h4vkwpuo`. A fresh Wikipedia navigation
reported `loadOutcome=succeeded`; a 12-notch wheel burst moved the root
722 px and presented 17 frames at 57.99 fps through the last frame. The
Guardian and Reddit pause/rapid-seek/resume probes both completed at their
final target times with no reported media error
(`.vm/bench/scroll-20260925-153206-final-wikipedia/`,
`.vm/bench/probe-20260925-153259-summit-final-guardian-seek/`, and
`.vm/bench/probe-20260925-153345-summit-final-reddit-seek/`).

The final bundle scored **8.017 ± 0.130** on 30 Speedometer 3.1 iterations
at the same 1280×887 content viewport as the installed control, which scored
**8.043 ± 0.127**. Their total suite time was 2,804 versus 2,801 ms per
iteration, so this comparison does not show a meaningful regression. Both
were below the earlier 8.601 run. Firefox 155 scored **7.811 ± 0.145** in a
subsequent 30-iteration run on the same workstation. Summit's point score is
higher, while the uncertainty ranges overlap. The desktop launcher now points
to `bundle-h4vkwpuo` and retains `SUMMIT_SCROLL_REFRESH_TIMER=16`; its
predecessor is saved as `Summit-current.pre-paused-decoder-20260925.sh`
(`.vm/bench/speedometer-20260925-153614-final-parked-paused-pgo-thirty/`,
`.vm/bench/speedometer-20260925-153946-installed-control-thirty/`, and
`.vm/bench/firefox-speedometer-20260925-154325-current-firefox-thirty/`).

### Live Reddit ad seek and media track diagnosis

An opt-in codec trace on the installed `bundle-h4vkwpuo` visited a live
`/r/popular/` feed. The repeated `MediaExtractor` failure was tied to
`packaged-media.redd.it` files with three streams: Media Kit could not
allocate stream 2, while Summit selected NVDEC H.264 video at index 0 and
AAC audio at index 1 from those same files. Other `v.redd.it` files selected
NVDEC video, sometimes with a separate AAC rendition. Four 40-notch bursts
delivered all 160 wheel events and reached 58–59 active frames/s. The third
stream remains unidentified, so the trace does not establish whether it is
metadata, subtitles, or another media track; it does show that the error did
not prevent the main video and audio decoders from being selected in this run
(`.vm/bench/scroll-cycles-20260925-155111-reddit-media-track-trace/`).

The same feed exposed a live Aldi video ad with Reddit's progress control.
After dismissing the sign-in popover, replaying the 15-second ad, and dragging
its progress thumb back toward the start through VNC, the video displayed a
different frame and continued at 0:08 / 0:15. The saved screenshots show the
control before and after the drag. This is a live ad pointer-path check,
although it does not reproduce the reported Adobe ad or prove every ad player
uses the same control (`.vm/reddit-ad-scrolled.png`,
`.vm/reddit-ad-afterdrag.png`).

Reanalysis of the saved direct Guardian article trace found 488 decoded video
frames but 465 paint callbacks. The 23 coalesced frames were concentrated near
startup: 10 before media time 1 second, 16 before 2 seconds, and none after
10 seconds. After 10 seconds the largest interval between painted media times
was 67 ms; before 2 seconds it reached 204 ms. Decode time and repaint-queue
delay alone therefore do not explain the reported choppiness; the next useful
measurement is actual frame presentation during the first seconds of the full
article while its page work is active
(`.vm/bench/guardian-article-audio-recovered-20260925/browser.log`).

### Final X399 test build

A fresh PGO rebuild of WebProcess, NetworkProcess, and Summit produced
`bundle-or5rko68` (engine patch SHA
`dbf9402cbda75c901b27306aaabd1528782ffd520b69e0b330eef6964b7e96e2`).
Its `libWebKit.so.1.10.0` hash matches the previously benchmarked
`bundle-h4vkwpuo`. A Wikipedia smoke run loaded successfully, accepted all
eight wheel events, scrolled 356 px, and reached 57.69 frames/s in its last
sample (`.vm/bench/scroll-20260925-160359-release-smoke-wikipedia/`). A
Guardian paused-seek probe resumed at 10.758 seconds, advanced to 13.004
seconds, selected NVDEC H.264 and AAC, and reported no media error
(`.vm/bench/probe-20260925-160434-summit-release-guardian-seek/`).

The X399 desktop launcher points to `bundle-or5rko68`; its previous version
is saved as `/boot/home/Desktop/Summit-current.pre-final-rebuild-20260925.sh`.
Build logs are `.vm/final-release-webprocess.log`,
`.vm/final-release-networkprocess.log`, and `.vm/final-release-browser.log`.

### Guardian startup pacing on the installed build

An isolated `bundle-or5rko68` visit to the reported article used the text
fragment above its inline video. NVDEC H.264 was selected and decoding began,
but the native view delivered only 4–9 frames/s during the first two seconds
of media playback, with gaps of 262–555 ms across two runs. The Guardian
sign-in panel then paused the clip. A page-update trace during this startup
included 45–54 ms initial layouts and a 74.8 ms intersection-observer phase;
compositor sends were mostly 8–21 ms. These observations implicate live page
work in the startup stalls but do not assign all idle time to those measured
phases (`.vm/bench/guardian-presentation-20260925/` and
`.vm/bench/guardian-startup-trace-20260925/`).

After dismissing the sign-in panel, a capture-free run resumed the visible
video near 2.3 seconds, decoded through its 19.56-second end, and recorded
454 painted frames from 472 traced decodes. Painted media-time intervals in
the first two seconds had a 376 ms maximum and three gaps above 120 ms;
between 7 and 12 seconds their median was 37 ms, 95th percentile 56 ms,
and maximum 61 ms. During the sustained portion the native view reported
roughly 48–52 frames/s, including other page updates, with queue delay
below 3 ms. Capturing screenshots during an earlier visit created artificial
native queue delays near one second, so that visit is excluded from pacing
assessment. These are media paint and native-view delivery measurements,
not physical display scans (`.vm/bench/guardian-dismissed-clean-20260925/`
and `.vm/bench/guardian-dismissed-trace-20260925/`).

### Guardian repaint wait and process-isolation trial

The earlier `SUMMIT_MEDIA_FRAME_TRACE` labeled the age of the **latest**
decoded frame as `queue`. Coalescing can replace the frame many times while
one repaint callback waits, hiding the callback's true age. The corrected
trace records the time when that callback was first enqueued, its wait on the
WebProcess main thread, the latest decoded-frame age, and the number of
newer frames folded into it. In a capture-free full-article run on the
separate `bundle-9kh4ra5r`, first-two-second callbacks waited up to 226.2 ms
and one folded in five newer frames. Between 7 and 12 seconds the maximum
wait was 32.4 ms and no callback folded in another frame. NVDEC H.264 stayed
selected and the clip reached its end. Earlier `queue` values in this file
measure only the newest decoded frame's age and cannot rule out a longer
coalesced callback wait (`.vm/bench/guardian-truequeue-20260925/`).

A temporary event-loop trace showed 194 and 222 ms timer callbacks during
one startup and, after dismissing the sign-in panel, a 1,431 ms posted-message
task aligned with a 1,405 ms video repaint wait. A later run attributed long
tasks to both the Guardian document and frames whose initial URL had no host.
This establishes that page-thread tasks can delay video presentation despite
fast NVDEC decoding; the log does not identify their JavaScript call sites
(`.vm/bench/guardian-eventloop-20260925/` and
`.vm/bench/guardian-eventloop-host-20260925/`). The event-loop probe was
removed after diagnosis to avoid adding work to every browser task.

An opt-in WebKit site-isolation trial put Guardian's cross-origin frames in
separate processes. A same-bundle off/on/off sequence launched 3/9/3
WebProcesses, but first-two-second maximum repaint waits were
227.7/262.2/188.5 ms and the corresponding coalesced-frame counts were
17/20/11. All three runs selected NVDEC and reached video end. The extra
processes did not improve startup in this small, variable live-page sample;
the trial switch was removed. The X399 desktop launcher remains on
`bundle-or5rko68` (`.vm/bench/guardian-siteiso-off-a-20260925/`,
`.vm/bench/guardian-siteiso-on-20260925/`, and
`.vm/bench/guardian-siteiso-off-b-20260925/`).

### X399 final handoff build, 2026-09-25

Rebuilt `WebProcess` and `NetworkProcess` with the `SkiaCGMiPGO` profile-use
configuration, then packaged Summit as
`/boot/home/summit/build-modern-browser/bundle-ueu9tgxp`. The bundle records
engine patch SHA `debb4fa018a767757fbfdc0a79a4b65399911a976c41857883298a690b80bc78`.
The build logs are `.vm/final-handoff-webprocess.log`,
`.vm/final-handoff-networkprocess.log`, and `.vm/final-handoff-browser.log`.

An isolated Wikipedia scroll smoke completed with 8 accepted wheel notches,
55.85 native-view frames/s through the last frame, and no frame interval over
33 ms during the burst
(`.vm/bench/scroll-20260925-170235-final-handoff-wikipedia/`). The Guardian
paused-seek probe reached 10.758 s, resumed to 12.999 s, selected NVDEC H.264
and AAC, and reported no media error
(`.vm/bench/probe-20260925-170309-summit-final-handoff-guardian-seek/`).
These are smoke checks; the earlier full-article startup pacing observations
remain the more representative evidence for Guardian choppiness.

The X399 desktop launcher `/boot/home/Desktop/Summit-current.sh` now points to
`bundle-ueu9tgxp`. Its previous version is preserved as
`/boot/home/Desktop/Summit-current.pre-handoff-20260925.sh`.

## GitHub issues #6, #7, #9 and #12 (2 October 2026)

Measured on the X399 (Threadripper 1950X, GTX 1070 through zink/NVK, two 4K
monitors at 200%) with private copies of the test bundles, the window at
1660x940 logical pixels.

### #6: WebAssembly memories were slow to create

Haiku's `mprotect()` visits every page of its range, and JavaScriptCore
changed the protection of a WebAssembly memory's whole 4 GiB reservation twice
(the inaccessible tail when created, everything writable again before
freeing), about 60 ms each, while holding the address space lock that the
process's other threads also need. The reservation is now mapped
inaccessible from the start (`MAP_NORESERVE`), only the pages the memory uses
are opened, and freeing it unmaps it (`BufferMemoryHandle.cpp`,
`WasmMemory.cpp`, `ArrayBuffer.cpp`; `SUMMIT_WASM_RESERVE_INACCESSIBLE=0`
restores the old way). `tools/bench/pages/wasm-start.html`: instantiating a
module with a 64 KiB memory took 125 ms, now 1.9 ms, in a worker or on the
page; a module without a memory was already 0.8 ms.

### #7: 1Password after the password

`tools/bench/pages/crypto-speed.html` runs PBKDF2-HMAC-SHA256 with 650,000
iterations, as 1Password's unlock does: through WebAssembly 700 ms in Summit
against 510-690 ms in Firefox 155 on the same machine, through WebCrypto
140 ms against 900 ms. Neither explains a long wait. The #6 fix removes about
130 ms from every WebAssembly instantiation with a memory, where 1Password's
background page was seen spending its start-up, and the frame pacing below
speeds up every animated page. The unlock itself needs the owner's account to
measure.

### #9: frames were paced from the request, not the refresh

The Haiku port has no display refresh monitor (`HAVE_DISPLAY_LINK` is defined
for Mac, GTK and WPE only), so `RenderingUpdateScheduler` falls back to a
one-shot timer of one frame interval, started when the page asks for the next
frame. A page that calls `requestAnimationFrame()` 2 ms into its callback got
a frame every 18.7-19 ms: WebGL Aquarium at 52 frames/s with its own work at
4 ms a frame, the compositor at 10.8 ms and the rest of the frame idle. The
interval is now counted from the start of the previous update, as a display's
refresh would be, and an overdue update runs at once
(`SUMMIT_REFRESH_FROM_REQUEST=1` restores the old timing).

| Page | Before | After |
| --- | --- | --- |
| WebGL Aquarium, 500 fish | 52-53 frames/s | 60 (frames presented: 120 per 2 s) |
| WebGL Aquarium, 5,000 fish | 28 | 40 (its own JavaScript and WebGL calls take 21 ms a frame) |
| xbitlabs.com FPS test, Low | 52.9 average, 18.5 ms frames | 55-58, 16.3-17.1 ms (its ads vary) |

At 500 fish each composited frame reads 5.3 Mpx back from the GPU (6.6 ms
waiting for the GPU, 3.8 ms copying, `SUMMIT_PRESENT_STATS=1
SUMMIT_READBACK_SPLIT=1`), which leaves room for 60 frames/s now that the page
and the compositor overlap. At 5,000 fish the page's own thread is the limit:
JavaScript, WebKit's WebGL bindings, ANGLE's validation and Mesa's GL front
end, without one dominant cost.

Rendering updates also come from layer flushes, style changes and frames'
loads, and each ran the page's `requestAnimationFrame` callbacks, so pages saw
63-66 a second, some 2 ms apart; xbitlabs' test takes the rate it measures for
the refresh rate. Callbacks that ran less than half a frame ago now wait for
the next refresh (`SUMMIT_RAF_MIN_INTERVAL_MS`, 0 to run them in every update).
Half a frame rather than a whole one: an update that waited for the compositor
can be followed by one that did not, closer than a frame, and skipping that
one would drop a frame. With a 13.7 ms gap the page saw 60.6 callbacks a
second, with half a frame 66 but none closer than 14 ms.

### #9: canvases that froze for seconds

At its "High" load, xbitlabs.com's test (500 rotated squares with a 15 px
shadow blur each frame) stopped for 450-820 ms every couple of seconds.
`tools/bench/pages/canvas-shadow-stress.html` draws the same: 47.7 frames/s
with a 99th percentile frame of 460 ms. `SUMMIT_CANVAS_TRACE=1` showed why:
the canvas was flushed and prepared for display about once in two seconds,
not every frame, so its recorded drawing grew to 518,000-889,000 operations
(75-127 frames) and was then replayed at once, while the canvas on screen did
not change. A Haiku change of 23 September ("canvas damage coalescing") skips
the damage notification when the accumulated dirty rectangle already holds the
new one, and only `HTMLCanvasElement::paint()` cleared that rectangle; a
composited canvas is not painted that way, so after one full-canvas
`clearRect()` every later draw was "contained" and the canvas was never queued
again until something else repainted it. The rectangle now starts again once
the canvas has been prepared for display (the frame boundary), so the
Chart.js gain stays. The probe now runs at 53.8 frames/s with a 99th
percentile of 20 ms and a worst frame of 79 ms; 1,000 squares run at a steady
30 frames/s. On the site, "High" went from 35 frames/s with a 1% low of 1 to
42 with a 1% low of 14.

### #12: YouTube

Big Buck Bunny (`aqz-KE-bpKQ`, 720p60 H.264 through NVDEC) during playback:
60 frames/s with none dropped; a zero-delay timer chain on the page ran 632
times in 5 s with a worst gap of 37 ms.

Seeking was slow because a seek decodes every picture from the keyframe before
the target, up to 5.6 s of pictures (335) on YouTube, which took 745 ms after
YouTube's data had arrived (55 ms). Pictures before the target that no other
picture refers to (`nal_ref_idc` 0 in every slice) are now left out, as
FFmpeg's `AVDISCARD_NONREF` does (`SUMMIT_MSE_SKIP_DISPOSABLE=0` decodes them
again); on this video that is 55-60% of them.

| Seek target | Pictures before it | Data to playing, before | After |
| --- | --- | --- | --- |
| 89.9 s | 331 | 783 ms | 301 ms (199 left out) |
| 149.9 s | 117 | 285 ms | 145 ms |
| 209.9 s | 339 | 762 ms | 290 ms |

From `currentTime` being set to the video playing past the target: 1,158 ms
before, 598 ms after (the 89.9 s seek).

Switching to another video from the page (a recommended video's link,
YouTube's own navigation) took about 1.3 s to the first picture: 0.8 s of
YouTube's script and requests before it creates the new player, 0.3 s until
the first data is appended, 0.2 s to decode the first 1080p picture. What made
it feel slow is the page's main thread afterwards: a zero-delay timer chain
was blocked for 5.5 of the 6 seconds after the click. The profile
(`profile -a -f`) put the largest single cost in CSS filters:
`ytd-thumbnail.player-container-background-image`, YouTube's blurred
thumbnail of the next video behind the player, has `filter: blur(15px)` over
1202x676 CSS pixels, and since the element is not composited, the blur was done
in software (`Raster8888BlurAlgorithm`) on the main thread whenever its tiles
were recorded, for every tile it touches and every repaint. WebKit2 never makes
filters a compositing trigger; ports that paint tiles on the GPU do this blur
cheaply, Haiku paints tiles on the CPU. Elements with a blur of at least 5 CSS
pixels (outsets of 15) over at least 200x200 are now composited on Haiku, so
their tiles are painted without the filter and the compositor blurs the layer
on the GPU, where TextureMapper scales a wide blur down first
(`SUMMIT_COMPOSITE_WIDE_BLURS=0` turns this off). Small filtered elements,
such as YouTube's icons with a drop shadow, are left alone.

| Switching videos (same target), main thread | Before | After |
| --- | --- | --- |
| Time in `RenderLayer::applyFilters` | 1,387-1,411 ms | 580-649 ms |
| Tile recording | 868-874 ms | 192-281 ms |
| Timer chain blocked in the 6 s after the click | 5.5-5.6 s | 4.0-4.1 s |
| Timer chain turns in those 6 s | 44-45 | 141-167 |

The rest is YouTube's own script (about 1 s of main-thread time), its View
Transition snapshot of the old page (0.4-0.5 s, painted in software) and style
resolution (0.3-0.4 s).

Comparing the composited blur with the software one (`pages/wide-blur.html`)
showed that TextureMapper applied filter values in CSS pixels to surfaces in
device pixels: at 200% every composited blur, including every
`backdrop-filter`, was half as strong as specified. Blur and drop-shadow values
are now scaled by the layer's scale before TextureMapper applies them. One
difference remains: TextureMapper samples the layer's edge pixels beyond its
edge, so a composited blur keeps hard outer edges where the software blur
fades them into transparency.

### Stale and black tiles (found while checking the above)

GitHub's issue list showed parts of an earlier layout (titles doubled at a
tile boundary, a toolbar with its buttons twice) and black rectangles, for as
long as the page stayed still; the build installed before this work did the
same. Two causes:

- **Tiles in view were deferred.** The upload budget of 29 September
  (`SUMMIT_TILE_UPLOAD_BUDGET_MS`, 3 ms) uploads at once only the tiles that
  the last composition showed and leaves the rest for later compositions. It
  took "showed" from that composition's clip, which with damage tracking is
  often only what changed, and is reset when the layer moves; tiles in view
  were then taken for tiles outside it, and a composition that had used its
  budget before it reached the layer left them again, every time. The tiles
  in the viewport (plus 256 pixels) are now uploaded at once. With the budget
  switched off the page was right, which pointed at it.
- **Black where no tile was painted yet.** The fix for #13 drew a layer's
  tiles without its background colour, so the page's root layer showed black
  wherever a tile was missing (a window that was made wider). The colour is
  now drawn under the tiles: what is not painted yet shows the page's
  background.
