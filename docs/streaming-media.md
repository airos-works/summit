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
requires the installed FFmpeg library's public packet-duration API. WebM
content compression produces an append error; WebM encryption is read
([Clear Key](encrypted-media.md)). The new clear
codecs do not extend the [Clear Key](encrypted-media.md) support matrix.

The decoder registry can select installed hardware. Existing NVDEC/Raspberry Pi
H.264 selection and software fallback remain enabled. On X399, the additional
video codecs listed here were tested with software decoding. Since 30 October
H.265 also goes to NVDEC where it is installed (a named add-on, as for the
Raspberry Pi), and NVDEC's pictures reach the compositor as NV12
([performance.md](performance.md), 30 October). Where AMD's `amduvd` add-on
has VP9 and AV1 decoders (decoders 2 and 3: a proposed air/OS change; the
Framework 13's Radeon 760M decodes both), YouTube's VP9 and AV1 go to the GPU
too, as NV12 for the compositor, with software as the fallback
([wx5100-media.md](wx5100-media.md), [performance.md](performance.md), 30
October). The add-on now on the systems has neither, and those entries stay
unused. NVDEC on the X399's GTX 1070 has no VP9 path in air/OS's add-on yet.

Software H.264, HEVC and AV1 decoders used to lose or misplace presentation
timestamps, so their pictures took the container's times in order. air/OS's
ffmpeg add-on keeps each chunk's time since jmgasper/haiku@5e72aaf0fe
(21 October; d14bd57365 of 27 October did the same for `BMediaTrack`'s
reader, which the file player's frame-counter fallback copes with either
way). Checked on the laptop's hrev60207+2112 with H.264 and HEVC B-frame
streams, 8- and 10-bit HEVC, AV1 and VP9, through the end of the stream.
Since 30 October the container's times are used only for a decoder's first
eight pictures: when the decoder's own times equal them, the container
timeline is dropped (`SUMMIT_MSE_TRACE=1`: "keeps timestamps: container
timeline dropped"), and later decoders of that codec in the web process are
trusted from their first picture; otherwise (an older system) the container
timeline is kept. Trusting the decoder also fixes a seek to an HEVC CRA
picture: the decoder rightly drops its leading (RASL) pictures, whose
container times then labelled the following pictures, 4 frames (67 ms)
early in the test stream. `SUMMIT_MSE_CONTAINER_TIMESTAMPS=1` always uses the
container's times, `=0` the decoder's from the first picture.

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

Until 30 October Summit's own HLS path (`MediaPlayerPrivateHaiku`) played only
finite, unencrypted, byte-range CMAF playlists: it downloaded the renditions
and played the files. `canPlayType("application/vnd.apple.mpegurl")` answered
`maybe` (since 23 September, when Summit had no Media Source and Reddit's
typed `<source>` needed it), so pages that ask before choosing a script
player, as hls.js's own example and video.js do, chose the native path and
failed on live and encrypted streams with a format error.

`HTMLMediaElement::canPlayType` now answers `""` for the HLS types while Media
Source is enabled, as Firefox does, and those pages play the stream with
their script player over Media Source. A typed `<source>` and a plain `src`
use the native player (source selection asks the media engine, not
`canPlayType`), which since 30 October plays live and encrypted playlists
too (next section). `SUMMIT_NATIVE_HLS_CAN_PLAY=1` answers `maybe` again.

Checked on the X399 with a page doing what hls.js's example does
(`hls-player.html`, hls.js 1.5.20): Unified Streaming's live demo
(`demo.unified-streaming.com/k8s/live/stable/live.isml/.m3u8`, MPEG-TS
segments) went to the native path and failed with error 4 before; now it
plays through hls.js (duration growing 601 → 618 s, 563 frames, 61 dropped).
JW Player's AES-128 test stream (`oceans_aes.m3u8`) plays through hls.js
(21.6 s in, 518 frames, 8 dropped). A video with a typed HLS `<source>` and an
MP4 one still tries the HLS one first.

## Native HLS (#115, 30 October)

A plain `src` or typed `<source>` HLS playlist (`.m3u8`, or a server type of
`application/vnd.apple.mpegurl` / `application/x-mpegurl`) goes to the Media
Source player's segment machinery, the one direct [DASH](dash-playback.md)
uses (`DashPlaybackHaiku.cpp`), instead of the file player. The playlists are
translated into that player's model (`HLSPlaylistHaiku.cpp`): each variant and
the audio rendition is a representation, each discontinuity sequence a
period. Requests use the media element's loader, so cookies, CORS mode, CSP
and canvas tainting follow the element as for DASH; a playlist is limited to
4 MiB and a segment to 64 MiB. `SUMMIT_HLS_FILE_PLAYER=1` gives HLS back to the
file player (finite byte-range CMAF only).

- **Multivariant playlists.** Variants whose video codecs the engine plays,
  at most 2160 lines and six of them (spread by bandwidth), with adaptive
  switches at segment boundaries as for DASH (starting with the lowest). The
  audio group used by most variants is chosen, and in it the `DEFAULT`
  rendition; one without a URI means the variants carry the audio. A media
  playlist given directly is probed: its first segment says whether it has
  video, audio or both. Old decimal AVC codec strings (`avc1.66.30`) and HE-AAC
  (`mp4a.40.5`, `.29`) are understood.
- **Segments.** MPEG-TS (the Media Source TS demuxer, below) and fragmented
  MP4 with `EXT-X-MAP` (which may change between segments), whole files or
  `EXT-X-BYTERANGE` ranges. Audio and video muxed in one segment feed both
  renderers from one download.
- **Live and event playlists.** Every playlist is reloaded after its target
  duration (half of it when it did not change), segment times are anchored by
  media sequence number so they stay put while the window slides (a newly
  loaded rendition lines up by `EXT-X-PROGRAM-DATE-TIME`, else by sequence
  number, else at the live edge), playback starts `HOLD-BACK` (else three
  target durations) behind the edge, and `seekable` is the window. An
  `EXT-X-ENDLIST` turns it into a finite presentation. A reload that fails is
  tried again at the next one.
- **AES-128.** Whole-segment AES-128-CBC with the key's URI fetched like the
  segments (the element's credentials and CORS mode), cached per URI, the IV
  from `EXT-X-KEY` or else the media sequence number. Key rotation works,
  and an encrypted `EXT-X-MAP` too.
- **Discontinuities.** HLS segments carry no presentation times, so each
  discontinuity sequence's media clock is mapped to the playlist's time by
  its first sample, shared by audio and video; a segment whose first sample
  lands more than 10 s away from its playlist time (a 33-bit clock wrap, an
  undeclared restart) is mapped again. A new sequence starts a new period,
  so the demuxer and the initialization segment start afresh.

Not supported, with a format error: `SAMPLE-AES` and `SAMPLE-AES-CTR`, any
non-identity `KEYFORMAT` (FairPlay and other DRM), packed-audio segments
(raw `.aac`/`.ac3`/`.mp3` with ID3 timestamps), I-frame playlists and a
rendition from a second audio group. (Periods whose track set changes are no
longer refused since DASH plays them, 30 October; for HLS that is not
tested.) Subtitles,
`CLOSED-CAPTIONS`, low-latency parts (`EXT-X-PART`, blocking reloads; the full
segments still play), delta updates, content steering and
`EXT-X-START` are ignored.

MPEG-TS segments are demuxed in an HLS mode of `SummitMPEGTS::Parser`
(`Parser::Mode::HLS`): each segment is demuxed afresh, keeping the program's
committed tables and codec configurations (packagers cut and encode segments
separately, and FFmpeg marks packets after a continuity jump corrupt), the
stream probe may read the whole segment (Unified Streaming's segments put the
first audio well after half a second of video), and transport that Media
Source must refuse is demuxed as players do: continuity counter jumps, PES
lengths that do not add up (segments cut inside an audio PES), payload flags
without payload, clocks with zero reserved bits (Mux's packager), PES before
the first PCR, and a packet the sample checks refuse is dropped. Two changes
apply to Media Source too: a PCR's six reserved bits are no longer checked,
and HE-AAC in ADTS is configured at its AAC-LC core rate (FFmpeg reports the
SBR output rate, which the ADTS headers then contradicted).

Checked on the X399 (GTX 1070, NVDEC) with fixtures from
`tools/bench/native-streaming-fixture.py` served by its own server (byte
ranges, CORS, simulated live windows that loop with a discontinuity) and
public streams; `currentTime`, frames presented (`getVideoPlaybackQuality`)
and seeks through the page, decoders through the MCP `media` tool:

| Stream | Result |
| --- | --- |
| Fixture fMP4, two variants + audio rendition | 299 frames in 10 s, seek to 18 s, NVDEC, 0 dropped |
| Fixture fMP4 byte ranges in one file (Reddit's form) | same |
| Fixture muxed fMP4 media playlist (tracks probed) | 302 frames in 10 s, seek |
| Fixture fMP4, AES-128 with sequence-number IVs | 300 frames in 10 s, seek |
| Fixture TS; TS AES-128 (explicit IV); typed `<source>` | 339 frames in 11.3 s, 0 dropped; 421 in 14 s; plays |
| Fixture TS with a discontinuity (new encoding, clock from 500 s) | 640×360 → 480×270 at 24 s, 25 fps after it, `ended` at 36 s |
| Simulated live TS, 16 s window looping every 24 s | starts at the edge − 12 s, `seekable` slides (8–24 → 44–60 s), 30 fps through three loops, 0 dropped |
| Simulated live multivariant fMP4 + audio rendition | 8 of 8 starts play (3 of 6 stalled before the late-audio fix below) |
| Apple `img_bipbop_adv_example_fmp4` (fMP4, aud1 group) | 1080p60 NVDEC, seek to 60 s |
| Apple `bipbop_16x9` (TS byte ranges, muxed default audio) | 416×234 → 1920×1080 by 25 s, 9 dropped |
| Mux `x36xhzz` (TS, HE-AAC variants, zero PCR reserved bits) | 1080p, seek to 60 s, 0 dropped |
| JW Player `oceans_aes` (TS, AES-128, IV from sequence) | 1264×528, seek to 60 s, 0 dropped |
| Unified Streaming live demo (TS, 10 min window) | 720p at the edge, 536 frames, 0 dropped |
| Mux `dai-discontinuity-deltatre` (key per segment, discontinuities into ads, SCTE-35) | 720p, seek to 34 s, into the 720×408 ad at 36.3 s with a 30 ms wait, 0 dropped |
| Fixture DASH `SegmentBase` MP4; WebM (VP9 + Opus) | see [dash-playback.md](dash-playback.md) |
| `SUMMIT_HLS_FILE_PLAYER=1` | byte-range CMAF goes to the file player ("Haiku Media Kit") again; TS fails there as before |
| Akamai `cph-p2p-msl…/test/master.m3u8` | its media playlist answers 404 (stream gone) |

`tests/EngineHLSPlaybackTests.cpp` drives the player on the host (ASan and
UBSan) with the fMP4 fixtures and a model of the renderers' queues (90 video
and 120 audio samples that drain only while the clock runs): multivariant,
byte-range, probed muxed, discontinuity (1,020 frames, continuous times),
AES-128, a live window over 70 s that loops (55 playlist loads, video and
audio within 0.2 s of each other), DASH SegmentBase, refused forms, and
playlist parsing. A sweep of 72 live start times over two loops stalled in
38 before the late-audio fix and in none after. The DASH playback test still
passes (2,280 video and 3,321 audio sample deliveries; audio ending before
the playback position is now dropped).

```sh
FFMPEG=ffmpeg python3 tools/bench/native-streaming-fixture.py \
  --directory .vm/native-streaming --port 8865
g++ -std=c++20 -fsanitize=address,undefined -fexceptions \
  -I<engine>/Source/WebCore/platform/graphics/haiku -I/usr/include/libxml2 \
  tests/EngineHLSPlaybackTests.cpp <haiku>/{DashManifestHaiku,DashPlaybackHaiku,\
  HLSPlaylistHaiku,FragmentedMP4ParserHaiku,CencCryptoHaiku}.cpp -lxml2 -lcrypto
./a.out .vm/native-streaming
```

An HLS audio rendition's segments need not line up with the video's, so
the audio segment holding the start position can lie almost entirely before
it; its samples filled the audio renderer's queue while the clock waited for
data past the position, and playback never started. The player now drops
audio that ends more than 0.2 s before the playback position instead of
queueing it (DASH too).
