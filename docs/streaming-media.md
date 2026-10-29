# MediaSource streaming

Summit uses incremental container parsers and Haiku Media Kit decoders. The
native integration is checked on X399 with actual playback, moving video
pixels, seeking, pause/resume and end events. Direct DASH and encrypted media retain
their separately documented H.264/AAC scope.

| Byte-stream MIME type | Supported codecs |
| --- | --- |
| `video/mp4`, `audio/mp4` | Existing H.264 and AAC-LC; HEVC Main/Main10/Main Still Picture, VP9, AV1, AC-3 and E-AC-3 |
| `video/webm`, `audio/webm` | VP8, VP9, AV1, Opus and Vorbis; audio MIME types reject video codecs |
| `audio/aac` | ADTS AAC-LC, including MPEG-2 AAC-LC |
| `audio/mpeg` | MPEG audio Layers I, II and III |
| `video/mp2t` | H.264, HEVC, AAC-LC, MPEG audio Layers I/II/III, AC-3 and E-AC-3 |

Raw AAC and MPEG audio use MSE sequence mode and reject a `codecs` parameter,
including an empty parameter. MIME type matching is case insensitive. Other
containers validate codec strings and reject incompatible combinations. Vorbis
requires the installed FFmpeg library's public packet-duration API. Unsupported
WebM content compression or encryption produces an append error. The new clear
codecs do not extend the [Clear Key](encrypted-media.md) support matrix.

The decoder registry can select installed hardware. Existing NVDEC/Raspberry Pi
H.264 selection and software fallback remain enabled. On X399, the additional
video codecs listed here were tested with software decoding. Since 30 October
H.265 also goes to NVDEC where it is installed (a named add-on, as for the
Raspberry Pi), and NVDEC's pictures reach the compositor as NV12
([performance.md](performance.md), 30 October). HEVC/AV1 presentation timestamps are kept
from the container because the installed decoders do not preserve them reliably.

WebM parsing supports fixed, Xiph and EBML lacing, Opus pre-skip, discard
padding and audio seek preroll. Fully discarded packets still prime the decoder.
PCM clipping uses exact sample counts for append windows and seeks. Changing
between stereo floating-point Opus and mono 16-bit MPEG audio drains the old
output before replacing the native sound player.

Raw MPEG audio handles incremental headers and frames, free-format MPEG frame
sizing, ID3v1/v2 and ICY header metadata. Layer III uses sufficient seek preroll
for the bit reservoir in low-rate streams. Parser storage is bounded.

MPEG-TS uses installed FFmpeg public APIs behind transport validation. It
accepts one program, checks packet continuity, PSI CRCs, PES headers and PCR,
and drops exact duplicate transport packets. Arbitrary append boundaries do
not truncate access units. Explicit end-of-stream drains final reordered
frames before ending playback; truncated transport produces a decode error.
Abort preserves committed program and decoder configuration while discarding
partial input. The parser unwraps 33-bit clocks, joins declared discontinuities,
and resets its internal timestamp offset on abort or a successful MSE
`timestampOffset` change, following the
[TS byte-stream requirements](https://www.w3.org/TR/mse-byte-stream-format-mp2t/).
Changed programs or PMT track layouts require `changeType()` to initialize a
new parser and remain subject to MSE's track-count constraints. The TS path
retains NVDEC H.264 selection, verified on X399.

## Verification

The final streaming integration is `bundle-9_wz1wsy`, using engine patch SHA-256
`fdb082ac31077936391914e40fa2ad688d20fc49c2182c4345c6ceee8c852f32`.
All 56 browser scenarios pass (1,692 DOM checks), with 50 exact PCM seek checks,
five video presentation-order checks and exact Opus pre-skip/discard counts.
The suite covers pause/resume, append windows, format changes, abort/reuse,
clock rollover, both discontinuity directions, truncated transport, late EOF,
and detach/remove/reopen or settings changes during finalization. In particular,
a timestamp change cannot reuse the old clock offset for queued video frames.

AAC (252 DOM checks), Clear Key (207 checks), DASH (82 checks) and all five host
suites pass. The owned browser groups exit normally without new crash reports,
syslog crash events or remaining helpers. The user's existing browser stays open.

Portable TS checks pass 10 fixtures (988 packets) at nine append sizes,
26 structural cases and 128 mutations under ASan/UBSan. The native TS probe
passes 21 decode checks across 12 files, including both discontinuities and
NVDEC H.264. Earlier WebM/raw-audio/MP4 integration checks pass 43 native decode
checks across 29 files. WebM parser checks cover 12 fixtures, 26 structural
cases and 128 mutations; raw MPEG audio covers 12 fixtures, 14 structural
cases and 128 mutations.

```sh
python3 tools/bench/test-webm-parser.py --output .vm/webm-parser
python3 tools/bench/test-mpeg-audio-parser.py --output .vm/mpeg-audio-parser
python3 tools/prepare-ffmpeg-headers.py --prefix /absolute/private/ffmpeg-headers
python3 tools/bench/test-mpegts-parser.py \
  --headers /absolute/private/ffmpeg-headers --output .vm/mpegts-parser
SUMMIT_BENCH_HOST=workstation SUMMIT_WS_MULTIPLEX=0 \
  python3 tools/bench/test-streaming-codecs.py \
  --fixtures .vm/webm-parser .vm/mpeg-audio-parser .vm/mpegts-parser \
  --output .vm/streaming-codecs
SUMMIT_BENCH_HOST=workstation SUMMIT_WS_MULTIPLEX=0 \
  python3 tools/bench/test-streaming.py --bundle /absolute/frozen/bundle \
  --output .vm/streaming-browser
```

`test-streaming.py` generates its own WebM, raw-audio, fragmented-MP4 and TS
media. Its malformed TS fixtures are browser error tests; exclude them from
native decoding. The TS parser's `result.json` manifest selects valid transport
fixtures for the codec probe. Parser tests require FFmpeg, a C++ compiler and
sanitizer support on the host. Native tests compile their own driver and launch
an isolated browser profile through `tools/ws.sh`.

TS uses pinned FFmpeg 6.1.2 public headers and checks the installed
`libavformat.so.60`, `libavcodec.so.60` and `libavutil.so.58` ABI versions before
use. The header preparation script installs only headers in a private prefix;
it does not replace OS libraries.

Local evidence is retained in `.vm/issues43-49/` under
`streaming-verified-x399`, `ts-regressions`, `mpegts-parser-complete`,
`ts-native-complete`, `webm-parser-integrated`, `mpeg-audio-preroll` and
`streaming-codecs-timing-x399`. The earlier broad native-codec integration
used `bundle-e821ncjv`; the current browser suite repeats its playback cases.

## HLS: script players for live and encrypted streams (#115)

Summit's own HLS path (`MediaPlayerPrivateHaiku`) plays only finite,
unencrypted, byte-range CMAF playlists: it downloads the renditions and plays
the files. `canPlayType("application/vnd.apple.mpegurl")` answered `maybe`
(since 23 September, when Summit had no Media Source and Reddit's typed
`<source>` needed it), so pages that ask before choosing a script player,
as hls.js's own example and video.js do, chose the native path and failed on
live and encrypted streams with a format error.

`HTMLMediaElement::canPlayType` now answers `""` for the HLS types while Media
Source is enabled, as Firefox does, and those pages play the stream with
their script player over Media Source, which handles live playlists,
AES-128, discontinuities and multi-resource playlists. A typed `<source>`
and a plain `src` still use the native path (source selection asks the
media engine, not `canPlayType`). `SUMMIT_NATIVE_HLS_CAN_PLAY=1` answers
`maybe` again.

Checked on the X399 with a page doing what hls.js's example does
(`hls-player.html`, hls.js 1.5.20): Unified Streaming's live demo
(`demo.unified-streaming.com/k8s/live/stable/live.isml/.m3u8`, MPEG-TS
segments) went to the native path and failed with error 4 before; now it
plays through hls.js (duration growing 601 → 618 s, 563 frames, 61 dropped).
JW Player's AES-128 test stream (`oceans_aes.m3u8`) plays through hls.js
(21.6 s in, 518 frames, 8 dropped). A video with a typed HLS `<source>` and an
MP4 one still tries the HLS one first.
