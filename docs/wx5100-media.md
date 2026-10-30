# WX 5100 media decoding

The port patch explicitly selects the `amduvd` Media Kit add-on on x86-64 for
progressive, eight-bit, 4:2:0 H.264 Baseline/Main/High streams within its size and
reference limits. The add-on performs the complete bitstream/device validation.
It registers no formats with Media Kit, leaving software decoding available.
`SUMMIT_HARDWARE_VIDEO=0` disables named hardware selection.

AMD supplies RGB32 using each picture's declared color range and matrix, or,
since 30 October, NV12 for the compositor (air/OS's add-on from
jmgasper/haiku@b8f1a18638; `SUMMIT_AMD_NV12=0` asks for RGB again):
converting to RGB is one thread's work in the add-on, and a 2160p60 VP9
stream came at 61 pictures a second that way, at 137 as NV12. Ten-bit
pictures come as NV12 only from an add-on that narrows them (one with an AV1
decoder); an older one is asked for RGB for HEVC Main 10. The
Raspberry Pi retains its existing I420 path.

VP9 (profiles 0 and 2, decoder 2) and AV1 Main (decoder 3) are listed for
`amduvd` too, for an add-on that has them: the proposed air/OS change in
`haiku-amdgpu_va.diff` (libavcodec's VP9 and AV1 VA-API hwaccels in the
add-on's static libavcodec; VCN 1 and later decode VP9, VCN 3 and later AV1:
the Framework 13's Radeon 760M both, the WX 5100's UVD 6 neither, where
the add-on fails the stream and it goes to software). An add-on without
them (NewDecoder(2) returns NULL) leaves those entries unused.
`SUMMIT_HARDWARE_VIDEO_ADDONS=DIR` looks for named add-ons in DIR first, to
try a new one in one copy of Summit before it is installed. Media
capabilities report `powerEfficient` for VP9/AV1 where such an add-on would
take them, so YouTube picks them for the sizes only they come in. Timeout and slow-close state is
tracked independently for each backend. Negotiated dimensions and row sizes are
checked before allocating the output buffer.

File playback recovers from a hardware decode error by seeking the extractor to
the keyframe preceding the next output frame, initializing software decoding,
and discarding pictures already returned. The replacement format must fit the
original bitmap. MSE retains compressed samples back to a keyframe, preserving
the preceding GOP while reordered output is pending. It replays those samples
through software and suppresses already consumed timestamps. The cache switches
to software at 512 samples or 64 MiB, retaining at most one additional demuxed
packet needed for recovery. Cancellation, interruption and normal EOF remain
separate from decoder failure.

## Native qualification, 11 October 2026

Focused tests compiled the actual amended `MediaDecoderSelectionHaiku.cpp` and
linked to the idle reference engine's WTF/Media Kit dependencies on X399. These
are decoder tests, not yet full-browser qualification. Both amended decoder
translation units pass native syntax checks. The complete private engine build and final incremental update now pass under
`/boot/home/wx5100/summit-webkit/WebKitBuild/WX5100`, with port patch SHA-256
`1a2476f9afeeaf0a50e335c52bb8f5af0070a3f65a6a24079e9ab7e344c1ac0e`.
The resulting private browser bundle is `bundle-6mitguy5`.

The focused native results are:

- A six-second 320×240 H.264 clip returns all 144 pictures through AMD UVD.
- Forced failures after 17, 47, 49 and 141 output pictures each resume in
  software and return all 144 pictures, with no duplicate or missing pictures.
  Output guards, monotonic track time, decoder identity and EOF checks pass.
- All hardware RGB pixels match independently decoded YUV and floating-point
  color conversion within one channel value of fixed-point rounding. Every
  software suffix matches FFmpeg RGB output exactly, including frame order.
- High 10 returns all 50 pictures in software; disabling hardware returns all
  144 pictures in software. A 2.75 s seek reaches the 2.5 s keyframe and returns
  all 84 remaining hardware pictures, with complete RGB/order comparison.
- Four additional 24-frame clips declare BT.601/BT.709 and limited/full range.
  All 96 pictures pass the same complete RGB comparison with maximum error one.

The production add-on was restored and its SHA-256 verified after fault tests:
`3f28945cffd89cad4cc5f093088c97b3e175b13355ffb63d1c9392d49728a9e6`.
The driver remains the qualified UVD clock build `49537626…`; these tests do not
change the driver or qualify general GPU rendering.

The workstation still had the obsolete NVIDIA `nvdec` add-on ahead of the
software decoder. With the GTX 1070 removed, it failed during track setup and
left Media Kit without a usable native track decoder. It is preserved outside
the add-on search path at `/boot/home/wx5100/retired-nvidia/nvdec`, SHA-256
`7c3e23e6b91d2c878d4d605d444c3227a498e761e0631f30338e269bf2b3b460`.
No Media Kit library replacement or media-server restart was needed.

Evidence is in the X399 lab's `evidence/wx5100` directory:
`summit-decoder-syntax.log`,
`summit-track-hardware-after-nvdec.log`, `summit-track-faults.log`,
`summit-track-rgb-comparison.log`, `summit-track-colors.log`, and
`summit-track-color-comparison.log`. Raw BGRA captures and MP4 fixtures are kept
alongside them. The complete build log is `summit-private-build.log`.

## Focused test tools

`tools/amduvd-test/build.py` builds the real file decoder test against an idle
reference build, writing only its specified output directory. Supply the amended
engine checkout with `--source` and the reference's generated Ninja directory
with `--reference-build`. Its `track-decode` executable accepts a video, expected
frame count, expected final decoder short name and a BGRA capture path; an
optional final argument seeks to a time in microseconds before decoding.

`make-fixtures.sh OUTPUT-DIRECTORY` creates the four color fixtures with FFmpeg.
`verify-rgb.py INPUT.mp4 CAPTURE.bgra` checks the entire picture sequence with
NumPy (tested with 2.2.6). `--fallback-after N` additionally checks the software
suffix, and `--start-frame N` checks a capture beginning at a seek keyframe.

`fault-addon.cpp` builds a **test-only** proxy. It loads the real add-on from
`SUMMIT_TEST_AMDUVD_REAL` and fails after the output count specified by
`SUMMIT_TEST_AMDUVD_FAIL_AFTER`. It must never ship or remain in a Media Kit
add-on directory. The lab run held its workstation lock, saved the real add-on,
used a restoration trap and checked the restored hash before proceeding.

The host SPS selection-parser regression builds the actual parser with ASan
and UBSan (`sps-parser-test.py --output DIRECTORY`). It reproduces an overflow
from an out-of-range signed scaling delta, then verifies rejection before
addition, both legal boundary values and 25,000 malformed configurations.
The correction and early cancellation check for cached MSE replay are included
in the completed private engine build; the earlier focused results above
describe commit `fc9cd5a`. `mse.html` now accepts `timeoutMs` for sustained playback and
long-GOP recovery checks.

Additional native fault tests reproduced two file-recovery failures: a failed
hardware seek reset stopped playback, and a decoder format rejected on one
read could still write into the caller's undersized buffer on the next read.
The corrected module switches to software after reset failure and latches
incompatible output geometry before any subsequent decode. It preserves the
bitmap's original format and retains fallback preroll across interrupted reads.

The reset-failure test seeks to the 2.5 s keyframe and returns all 84 remaining
software pictures, exactly matching FFmpeg RGB. The format test advertises a
smaller hardware output, fails before hardware writes, then checks three reads:
all reject the larger software output without changing any byte of the guarded
buffer. Both tests fail against the prior module and pass after the correction.
The normal 144-picture hardware run and all four mid-stream failure runs also
pass again, with the same complete RGB comparisons. Logs are
`summit-seek-format-{before,after}.log`, `summit-failed-seek-rgb.log`,
`summit-track-{faults,hardware}-v2.log` and `summit-recovery-v2-rgb.log`.

The fault proxy's additional test controls are `SUMMIT_TEST_AMDUVD_FAIL_SEEK`
and `SUMMIT_TEST_AMDUVD_BAD_WIDTH` (the latter only takes effect with
`SUMMIT_TEST_AMDUVD_FAIL_AFTER=0`). The track test's expected decoder value
`format-error` checks repeated rejection without output writes. The proxy is
restored to the real add-on and its hash verified after each test group.

Two additional native file fixtures pass using the current selection module
(`c96cb9c`). A 36-picture stream changes coded height 240 → 256 → 240 at IDRs,
with cropping preserving visible 320×240 throughout. A twelve-picture
Baseline stream uses POC type 2 and MMCO 5 on each of its eleven P pictures.
Both finish entirely on `amduvd h264`, preserve frame counts and timestamp
order, and match independently decoded RGB within one channel value.
The latter fixture also passes a direct native NV12 comparison with zero
differences; the cropped-height fixture passes 204 Media Kit output pictures
in NV12, I420 and packed YUV with exact pixels and timestamps. These fixtures
and comparisons live under `format-transition/` and `mmco5-fixture/` in the
WX5100 evidence directory. They do not yet qualify the full browser/MSE path.

For browser pixel checks, `mse.html` accepts `capturePausedFrame=1` with its
pause controls and includes a full-resolution PNG in the JSON result.
`verify-rgb.py --start-frame N --frame-count 1` checks the corresponding
decoded BGRA capture against that exact source picture without searching for
a matching frame.

The full browser's first MSE H.264 run completes all 144 pictures with zero
drops and no media errors. The trace names the AMD UVD decoder and contains
144 ordered output timestamps; the page reaches normal end, the browser quits
cleanly and the crash monitor reports no new crashes or syslog events. Evidence:
`probe-20261011-025740-summit-wx5100-mse144-llvmpipe` in this checkout's
`.vm/bench/`, and `summit-browser-mse144-llvmpipe.log` in X399 evidence.

Page rendering for these media tests uses a separate software Mesa prefix at
`/boot/home/wx5100/summit-mesa/prefix`: llvmpipe (LLVM 20.1.8), Mesa 25.3.6.
The original installed Mesa contains only Zink and cannot initialize EGL with
the old NVIDIA card removed. The private stack passes independent surfaceless,
pbuffer and threaded GLES pixel checks with zero mismatches. Its build log is
`summit-software-mesa-build.log`. This is CPU page compositing with hardware
video decoding; it does not establish AMD GPU rendering.

File playback also selects AMD UVD and reaches normal end with one startup
preroll picture plus 143 playback pictures, no media error and clean shutdown
(`probe-20261011-025822-summit-wx5100-file144`). File playback does not currently
implement the browser's playback-quality frame counters.

The MSE paused seek to 2.75 seconds holds an unchanged picture for 1.5 seconds
and resumes to normal end. Its full-size PNG matches source frame 64 within
one RGB value; the first MSE timestamp is 0.083333 seconds. Evidence:
`probe-20261011-025903-summit-wx5100-mse-paused-seek` and
`summit-browser-mse-paused-pixels.log`.

The analogous file test reproduced a seek-boundary error: at 2.75 seconds,
frame 65 was shown instead of frame 66 (file timestamps start at zero).
The saved PNG fails comparison with frame 66 and matches frame 65 within one
RGB value. The substitute decoder's current time denotes the returned
picture's end, so seek completion now requires that end to be strictly past
the target. This retains the native track's start-timestamp comparison.
The corrected hardware and software paths both pass the repeated pixel
check below. Before-fix evidence: `probe-20261011-025950-summit-wx5100-file-paused-seek`,
`summit-browser-file-paused-pixels-before.log` and
`summit-browser-file-paused-confirm65.log`.

With hardware disabled, the file seek has the same pre-fix boundary error:
its PNG matches frame 65 exactly and fails frame 66. The software timestamp
workaround also supplies a frame-end counter. It now reports that distinction
to the seek loop, which applies the strict boundary comparison only to such
counters and preserves start-timestamp semantics otherwise. Evidence:
`probe-20261011-030538-summit-wx5100-file-paused-sw-before`,
`summit-browser-file-paused-sw-pixels-before.log` and
`summit-browser-file-paused-sw-confirm65.log`. The independent pixel test ran
while browser packaging was active; no performance result is claimed for it.

The updated hardware file seek now captures frame 66 at 2.75 seconds with
maximum RGB error one, holds it unchanged and resumes to normal end
(`probe-20261011-030640-summit-wx5100-file-paused-seek-after`,
`summit-browser-file-paused-pixels-after.log`).

Full-browser High 10 fallback returns all 50 pictures in software. An injected
MSE hardware failure after picture 47 returns all 144 pictures with no drops;
every output timestamp matches the source in order, without gaps or repeats.
Both runs had concurrent build activity, so these establish correctness, not
uncontended throughput. Runs: `probe-20261011-030742-summit-wx5100-high10-software`
and `probe-20261011-030839-summit-wx5100-mse-fault47`.

Failure on the first Decode call exposed a different MSE recovery gap: the
addon had requested no packets, so the empty replay cache incorrectly caused
a fatal media error. Recovery now starts software directly from the untouched
queue in that case; after any consumed input/output it still requires a saved
keyframe. Before-fix evidence is
`probe-20261011-030919-summit-wx5100-mse-fault0-seek`. Final bundle
`bundle-uza681dk` (commit `8b5369e`, patch `d386c022...`) passes the repeated
startup/seek failure case and captures source frame 64 exactly in software,
then ends normally (`probe-20261011-031916-summit-wx5100-mse-fault0-seek-after`).
Each injected-failure run restored the real addon and verified its original
SHA-256 before ending.

The software file seek also passes at exactly 2.75 seconds (source frame 66,
exact RGB). Hardware at 2.74 seconds correctly shows frame 65 within one RGB
value. File failure after 47 hardware pictures recovers to software and ends
with one preroll plus 143 playback pictures. An injected failed startup seek
reset also recovers; the later paused seek still shows frame 66 exactly.
This reset injection occurs at startup, not after playback has begun. Evidence:
`probe-20261011-031211-summit-wx5100-file-paused-sw-after`,
`probe-20261011-031536-summit-wx5100-file-fault47`,
`probe-20261011-031605-summit-wx5100-file-seek-reset-fallback`, and
`probe-20261011-031632-summit-wx5100-file-seek-between-frames`.

The 540-picture long-GOP MSE test reaches the bounded recovery cache limit,
replays 513 samples in software and emits all source timestamps exactly once.
It drops two late pictures while a build is active, so establishes recovery
correctness rather than uncontended performance
(`probe-20261011-031114-summit-wx5100-mse-cache512`).

Full HD exposed scalar RGB conversion as a throughput limit: native decoding
of 900 pictures takes 33.866 seconds and browser playback drops frames.
Haiku commit `fb4755dc24`, addon SHA `818ef96c...`, adds exact SSE4.1 RGB
conversion, reducing direct decode to 27.568 seconds. Native 144-picture
small and 24-picture HD captures match independent references within one RGB
value. With this candidate the browser plays all 900 1080p30 pictures and
unmuted AAC with zero drops and exact source timestamps
(`probe-20261011-033006-summit-wx5100-hd900-audio-simd`).

The three-minute test with the same candidate reaches normal end and decodes
all 5,400 source timestamps, but drops 72 pictures during presentation
(`probe-20261011-033151-summit-wx5100-hd5400-audio-simd`). There is no foreign
load, media error, new crash or GPU fault. Trace timing shows short decode
bursts exceeding one frame interval. The video loop currently waits for each
picture's presentation before decoding its successor, leaving no accumulated
margin despite average decode capacity above 30 fps. Production addon
`3f28945c...` was restored and verified after both candidate test groups.

The next engine candidate decouples decoding and presentation with a separate
presentation thread and a queue bounded to six pictures and 64 MiB (an
otherwise empty queue can accept one larger picture). Flush clears queued
pictures, wakes blocked producers, and serializes generation changes against
publication so an old picture cannot satisfy a new seek's preroll. The
existing clock and 80 ms late-frame policy are unchanged.

This engine (`3c739a4`, patch `b782abfd...`, `bundle-x50jiwz8`) passes the same
three-minute HD/AAC test with all **5,400 frames and zero drops**, all source
PTS in order, normal end and clean shutdown. All 17 load samples are
uncontended; mean owned CPU load is 0.746 cores. AAC queues 8,650,752 PCM
frames at 48 kHz stereo. The 116.3 ms reported output latency is not a
physical A/V measurement. No new crash, GPU fault or leftover process is
recorded. The saved screenshot shows the movie and `q=5400/0`.

Paused seeking still holds the correct source frame 64 at 2.75 seconds
(maximum RGB error one), then resumes without drops. High 10 software
fallback returns all 50 pictures with zero drops. Runs:
`probe-20261011-034254-summit-wx5100-readahead-paused-seek`,
`probe-20261011-034320-summit-wx5100-readahead-hd5400-audio`, and
`probe-20261011-034657-summit-wx5100-readahead-high10`.
The test group restores and verifies production addon `3f28945c...`.

The page's optional `maxDropped=0` makes zero drops an explicit pass
condition, independently of `expectedFrames` (whose total includes drops).
The queued renderer also passes four uncontended regressions with zero drops:
hardware failure after picture 47 (all 144 source timestamps, 13 compressed
samples replayed), first-call failure both at startup and after paused seek
(frame 64 exact software RGB), bounded-cache fallback (513 samples replayed,
all 540 output timestamps in order), and pause/resume without seeking (144
frames, time/pixels/counter unchanged during the pause). Every run ends
normally, drains its processes and records no crash or GPU fault. The real
addon is restored after each fault injection. Runs:
`probe-20261011-034728-summit-wx5100-readahead-fault47`,
`probe-20261011-034756-summit-wx5100-readahead-fault0-seek`,
`probe-20261011-034824-summit-wx5100-readahead-cache512`, and
`probe-20261011-034905-summit-wx5100-readahead-pause`.

HEVC selection now names decoder index 1 of `amduvd`; H.264 remains index 0.
The hvcC prefilter admits Main/Main 10, 4:2:0 and matching 8/10-bit planes,
with complete SPS/PPS/device validation still performed by the addon. An
older addon without index 1, unsupported configuration or missing device
keeps the existing software fallback. The native prerequisite is Haiku
`a5278702e5`, driver `8fc0041e...`, which corrects P010 firmware pitch units.
The combined addon `818ef96c...` has passed 648 captured Media Kit pictures
across formats, seeks and recovery, plus a 540-picture Main-10 POC-wrap test;
HEVC browser playback is awaiting its new bundle.

The MSE fixture accepts `videoCodec` so HEVC fragments are declared with their
actual codec string. The RGB reference checker now reads native ten-bit YUV
and applies ten-bit code ranges without reducing precision; it passes all
48 captured Main-10 RGB pictures within one channel value and repeats the
existing eight-bit paused-frame check. Reference decoding trims to a requested
frame interval, avoiding a whole HD clip allocation for one paused capture.

The first HEVC browser bundle (`cj29xks1`, `efaa9b9`) exposed a second,
H.264-only gate in MSE. Both short profiles and the 900-picture HD/AAC clip
completed without drops, but the trace identifies libavcodec: those runs
are software evidence only. MSE now asks the backend capability predicate
for every video codec and retains recovery packets for an accepted HEVC
backend too. Named decoders supply their own ordered picture timestamps;
input timestamp reconstruction starts only for software, including fallback
and replay. Native hardware qualification follows the rebuilt bundle.

The corrected MSE bundle is `tqyty51m`, from `6e3222c` (patch
`eac7ab9e...`). Both HEVC profiles return 48/0 frames in hardware, and
Main-10 paused seeking produces exact source picture 16 within one RGB value.
HD900 remains rejected: scalar addon `818ef96c...` counts 704 pictures with
688 drops; SIMD addon `e14a90ba...` improves throughput but still counts 876
with 810 drops. These runs are uncontended and have no crash or GPU fault.
Native P010-to-RGB conversion improves from 10.120 to 1.684 ms per HD frame
with identical checksums and full-precision pixel qualification, but this
alone is insufficient for the complete playback path.

The next engine change lets an RGB addon write directly into the caller's
owned buffer when both row strides match. Other layouts retain the existing
copy path. Decoder status, one-picture validation and seek-skip semantics
remain in place. Native playback/recovery qualification follows its build.

The direct RGB build is now qualified for the short application checks:
Summit `9649ad5`, patch `296f1b03...`, bundle `00kke_5j`, driver `8fc0041e...`
and addon `84351b42...`. MSE Main48, Main10-48, Main10 HD900/AAC and H.264-144
all complete with exact source timestamps and zero dropped frames. Every run
is uncontended, quits cleanly and records no crash or GPU fault. Native
interleaved tests also verify buffer reuse preserves all captured pixels.

Paused MSE Main10 seeking to 0.75 s matches source picture 16; the file path
matches picture 18 at the same time. A 1080p Main10/AAC file seek to 13.3 s
matches all RGB pixels of source picture 399 within one channel value,
using the independent full-precision ten-bit reference. Each picture and
clock stay stable during the pause and playback resumes to normal end.
File playback's browser quality counters are unimplemented (zero), so its
qualification uses decoder traces and captured pixels, without inferring a
drop count from those counters. Three-minute and injected-failure runs remain
pending. Browser composition still uses the private CPU Mesa stack.
Runs: `probe-20261011-044050` through `044405`. Evidence:
`summit-hevc-direct-qualification.log`, `summit-direct-timestamps.log`,
`summit-hevc-direct-{paused,file-paused,file-hd-paused}-pixels.log`, and
`summit-hevc-file-hd-qualification.log` in the X399 evidence directory.

The three-minute Main-10/AAC run now passes with the same bundle and addon:
all 5,400 MSE pictures, zero drops, exact source timestamps in order and normal
end. It is uncontended with no crash, GPU fault or leftover process. Run
`probe-20261011-044448-summit-wx5100-hevc-direct-main10-hd5400-audio`;
`summit-hevc-direct-hd5400-timestamps.log`.

HEVC recovery is now qualified too. Failure after 19 output pictures replays
nine MSE samples and returns all 48 source timestamps once, with zero drops.
Failure on the first decode at startup and after seek recovers in software;
the paused 0.75 s picture is source frame 16 with exact software RGB. File
failure after 19 hardware pictures resumes in software, and its paused image
at 1.173352 s is source frame 28, also exact. Both pauses preserve pixels/time
before normal completion. The original addon without index 1 uses software
from selection and returns all 48 source timestamps with zero drops. All four
runs are uncontended, cleanly drained and free of crashes/GPU faults. File
quality counters remain unimplemented. Runs `044819`, `044842`, `044904` and
`044926`; evidence `summit-hevc-{fault19,fault0-seek,file-fault19,old-addon}.log`
and the two paused capture comparisons. Production addon `3f28945c...` was
restored and verified after every test. Composition remains CPU Mesa.

## amdgpu_drm, 12 October 2026

The workstation now runs the WX 5100 with amdgpu_drm (Linux 6.18's amdgpu
on Haiku, air/OS `src/forks/linux-drm`) instead of the native `amdgpu` driver.
GL is the system's Mesa with radeonsi, installed in `/boot/system/non-packaged`
(its glvnd vendor file is
`/boot/system/non-packaged/add-ons/opengl/egl_vendor.d/50_mesa.json`); the
private zink Mesa cannot start there, having no Vulkan driver for the card,
and web processes then stop with "no EGL display". `install-on-workstation.sh`
uses the system Mesa wherever it is installed (`SUMMIT_MESA_PREFIX=system`).

`amduvd` is now the air/OS `amdgpu_va` add-on (`src/add-ons/media/plugins/
amdgpu_va`), with the same decoder indices (0 H.264, 1 HEVC) and RGB32
output, decoding through VA-API (libavcodec, Mesa's radeonsi VA driver, UVD).
`bundle-6mitguy5` plays H.264 file video through it ("video decoded by AMD
H.264 hardware decoder (VA-API)"); it predates the HEVC selection (efaa9b9),
so its HEVC is software until a newer engine is installed. Composition reads
frames back: the system Mesa has no `summit_haiku_present_*` (zink only).

The workstation's Summit is now `bundle-ci0334`, assembled from the air/OS CI
packages of 803d1c9 (summit and summit_webkit 0.1.0/1.10.1~git20261012.0334)
rather than installed as packages: `apps/Summit`, `lib/summit-webkit/{WebProcess,
NetworkProcess,lib}` and `data/Summit/start.html` as `resources/start.html`,
with the RPATH of `Summit` (`/boot/system/lib/summit-webkit/lib`, the
installed and older summit_webkit) rewritten in place to `$ORIGIN/lib`. It
selects the AMD decoder for HEVC too: HEVC Main and H.264 file video play on
UVD through amdgpu_va. The packages' own Mesa (zink) is not used; the
launcher names the system's.

`bundle-ci1612` (the CI packages of be9f736, 13 October) replaced it as the
workstation's Summit, and `bundle-ci1631` (ce257e0, WebGL again, see
[webgl.md](webgl.md)) replaces that (`Desktop/Summit-current.sh` and
`summit/Summit-installed`; the launcher of `bundle-ci0334` is kept as
`summit/Summit-current.sh.ci0334`). A
looping `<video>` with a muted audio track used to stop at its end: the end
waited for the audio, which does not advance without a sound player. The
video's end now counts when the audio is not playing, and the sound player
runs on silence past the end instead of being stopped and started again.
`platform.html?video=...&loop=1` reports `loop=waiting:3.00s` on
`bundle-ci0334` and `loop=wrapped:4` on `bundle-ci1612` with a 3 s H.264+AAC
clip; muted and unmuted clips, and a video without audio, looped 15 times in
45 s, and a clip with sound still ends and plays again from the start.
