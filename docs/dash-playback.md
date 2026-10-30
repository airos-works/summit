# MPEG-DASH playback

Summit's Haiku media backend can load `application/dash+xml` directly in an
HTML media element. It uses WebCore's media resource loader for the manifest,
initialization segments, and media segments, then feeds fragmented MP4 samples
to the same Media Kit audio and video renderers used by MediaSource.

## Supported presentations

- H.264 video and AAC audio in separate MP4 representations, and since 30
  October WebM representations (`video/webm`, `audio/webm`: VP9, VP8, AV1,
  Opus, Vorbis, demuxed by the Media Source WebM parser). Representations
  are offered to the Media Source player's `supportsType`, so its other MP4
  codecs are accepted too (not checked separately for DASH).
- `SegmentTemplate` with durations or `SegmentTimeline`, including inherited
  attributes, presentation offsets, and `$Number$` / `$Time$` substitutions.
- `SegmentList`, including exact HTTP byte-range requests.
- `SegmentBase` (one file per representation, 30 October): the player loads
  each playable representation's index before playback starts, an MP4
  `sidx` box (`indexRange`) or WebM `Cues` (with the initialization range,
  for the Segment's data offset and timecode scale), and turns its entries
  into byte-range segments. Static presentations only; hierarchical `sidx`
  indexes are refused. The MPD namespace is matched without regard to case
  (FFmpeg's WebM manifests write `urn:mpeg:DASH:schema:MPD:2011`).
- Static presentations and dynamic manifests with live-window refresh.
- Multiple periods, including periods that change the track set (since 30
  October): an ad period with video only, a period without video, or a
  period with other codecs.
- Adaptive representation changes at aligned segment boundaries. Selection
  starts with the lowest bandwidth and uses measured transfer throughput.

The decoder queues apply backpressure to segment fetching. Seeking cancels
outstanding segment requests, discards stale completions, flushes decoder
queues, and fetches the segment containing the target. Seek completion waits
for a decoded video frame. Live streams expose a finite seekable window.

A kind (video, audio) is enabled when any period has a playable
representation of it, and one renderer per kind is kept. Across a period
without that kind, the track moves on to the next period that has it: its
next segment is fetched early and the renderer waits for its timestamps, so
audio plays silence and video keeps its last picture (after a seek into
such a period, or at the start of an audio-only first period, the next
period's first picture shows at once, as the first frame after a seek
always does). A track whose kind no later period has ends there and no
longer holds up the other (`hasCurrentData`, `bufferedEnd`). A static
presentation's seekable range starts where its first period starts, even if
one kind only begins later (live presentations still start where both kinds
can be fetched). A period with other codecs or parameters gets a new parser,
and the renderer a new decoder at the configuration change. Encrypted
representations (`cenc`, `cbc1`, `cens`, `cbcs` MP4 and encrypted WebM) can
use temporary Clear Key sessions through
[Encrypted Media Extensions](encrypted-media.md), which CI's engine
configuration currently compiles out; commercial DRM systems are not
supported.

## Network and parsing boundaries

Requests retain the media element's cookies, TLS decisions, CORS mode, and
content security policy. Every response contributes to media-origin checks,
so cross-origin segments without CORS permission taint canvas even when the
manifest itself is same-origin. Redirects and resolved MPD URLs must use HTTP or HTTPS
without embedded credentials. Byte-range responses must have status 206 and
the exact requested `Content-Range` and body length. Requests have a 30-second
timeout; the manifest is limited to 4 MiB and each fetched segment to 64 MiB.

The XML parser refuses DTDs and external entities, and bounds representation
counts, expanded segment counts, dimensions, and numeric conversions. Its
validation exceptions are caught inside the portable parser; only that source
file is built with exception handling enabled.

## Verification tools

`tools/bench/dash-fixture.py` generates an 18-second H.264/AAC presentation
with two video resolutions using FFmpeg. It serves normal, throttled, live,
byte-range, malformed, cookie-protected, and missing-segment variants, plus
a JavaScript MediaSource regression fixture.

`tests/EngineDASHManifestTests.cpp` checks manifest semantics, URL and XML
rejection, and 5,000 malformed mutations. `tests/EngineDASHPlaybackTests.cpp`
exercises actual generated MP4 fragments, adaptive switches, backpressure,
seeking, cancellation, live refresh, and network errors under ASan/UBSan.

Build those portable tests with the three Haiku parser/playback sources from
the prepared engine tree, its `platform/graphics/haiku` include directory,
and libxml2. The manifest test needs only `DashManifestHaiku.cpp`; the playback
test also needs `DashPlaybackHaiku.cpp` and `FragmentedMP4ParserHaiku.cpp`.
Pass the generated `manifest.mpd` to the first test and its directory to the
second.

`tests/ModernDASHTests.cpp` drives the real native WebKit view through its
public Inspector API. Build with a frozen bundle's `source/include`, the
repository's `vendor` includes, and `-lWebKit -lbe`. It shares Inspector and
screenshot helpers with `ModernPDFTests.cpp`. Run with:

```text
test-dash BASE_URL BUNDLE SCREENSHOT_PPM
```

Set `WEBKIT_EXEC_PATH`, the bundle/Mesa `LIBRARY_PATH`,
`SUMMIT_ENABLE_INPUT_SYNTHESIS=1`, and `SUMMIT_MSE_TRACE=1`. The native suite
checks rendered pixels, frame counts, user-initiated playback, pause, seek,
quality changes, range validation, cookies, live refresh, media-src policy,
MediaSource playback, and helper teardown. Its screenshot should be visually
inspected after a successful run.

Verified on X399 with frozen bundle `bundle-eaoxfedz`: all 82 native checks
passed, with no remaining helpers or new crash reports. The engine patch
SHA-256 was
`edbc6c6e911d3a6729992077b78ad8a478cdcbfac39d07e6d0f5d8007f823670`.
The screenshot shows decoded video and native media controls. Native traces
confirm NVDEC video decoding, 48 kHz AAC audio output, and quality changes in
both directions under throttling. The final bandwidth estimator responds
immediately to decreases and increases its estimate gradually.

Both portable tests pass with `-fsanitize=address,undefined` and
`-Wall -Wextra -Werror`. The playback test checks 2,280 video and 3,321 audio
sample deliveries across its scenarios (3,444 before 30 October, when audio
ending before the playback position after a seek began to be dropped instead
of queued), including two consecutive periods,
end-of-duration seeks, expired live-window seeks and stale completions. Since
30 October it also plays the fixture as three periods with an ad period
without audio (1,620 video samples; audio only from the first and third
periods, current data throughout, a seek into the ad fetches the third
period's audio) and as an audio-only period followed by a video-only one
(each track ends after its own period); it used to check that such
manifests were rejected.

Checked on the X399 (30 October, engine `eme115-a` on CI's tree) with the
same 18 s fixture made into three manifests, plain `<video src>`: three
periods with a video-only ad between (1,619 frames in 54 s on NVDEC, the
clock never stopped, `ended` at 54; a seek to 20 s played on), audio-only
then video-only (540 frames, `ended` at 36), and a last period without audio
(1,080 frames, `ended` at 36). The previous code refused all three ("DASH periods
with changing audio/video track sets are not supported".

The same player plays HLS playlists ([streaming-media.md](streaming-media.md),
"Native HLS"). `tests/EngineHLSPlaybackTests.cpp` also covers `SegmentBase`
with the MP4 fixture of `tools/bench/native-streaming-fixture.py` (720 video
frames, a seek). On the X399 with that tool's fixtures: `dashsb` (MP4,
global `sidx`) played 299 frames in 10 s on NVDEC and seeked to 18 s;
`dashwebm` (VP9 + Opus, FFmpeg's `webm_dash_manifest`, Cues after the
clusters) played 302 frames in 10 s with the VP9 software decoder and seeked
to 18 s.
