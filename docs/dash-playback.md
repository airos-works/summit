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
- Multiple periods with consistent audio/video track sets.
- Adaptive representation changes at aligned segment boundaries. Selection
  starts with the lowest bandwidth and uses measured transfer throughput.

The decoder queues apply backpressure to segment fetching. Seeking cancels
outstanding segment requests, discards stale completions, flushes decoder
queues, and fetches the segment containing the target. Seek completion waits
for a decoded video frame. Live streams expose a finite seekable window.

Periods that change their audio/video track sets are outside this
implementation: one renderer per kind is kept across periods. CENC-encrypted H.264/AAC
representations can use temporary Clear Key sessions through
[Encrypted Media Extensions](encrypted-media.md); other encryption schemes
and commercial DRM systems are not supported.

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
end-of-duration seeks, expired live-window seeks, stale completions, and
explicit rejection of changing track sets.

The same player plays HLS playlists ([streaming-media.md](streaming-media.md),
"Native HLS"). `tests/EngineHLSPlaybackTests.cpp` also covers `SegmentBase`
with the MP4 fixture of `tools/bench/native-streaming-fixture.py` (720 video
frames, a seek). On the X399 with that tool's fixtures: `dashsb` (MP4,
global `sidx`) played 299 frames in 10 s on NVDEC and seeked to 18 s;
`dashwebm` (VP9 + Opus, FFmpeg's `webm_dash_manifest`, Cues after the
clusters) played 302 frames in 10 s with the VP9 software decoder and seeked
to 18 s.
