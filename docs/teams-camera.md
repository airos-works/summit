# Microsoft Teams: the camera, the microphone and the device check (#79)

[Issue #79](https://github.com/jmgasper/summit/issues/79): in a Teams meeting
the camera's preview stayed empty with either of the laptop's cameras, a test
call said Teams could not use the camera, and Teams' device check
(`devicetest.teams.microsoft.com`) did not load, while webcam.org's camera
test worked. Looked into on the laptop on 20 October 2026 with a test copy of
Summit on a test engine.

## Follow-up: missing frame callbacks (#126, 21 October 2026)

[Issue #126](https://github.com/jmgasper/summit/issues/126) reports a blank
self-preview and “Couldn't apply background” after the camera and microphone
can be selected. A separate MediaStream defect is fixed in `831a8dc`: a video
element could play a live stream, but `requestVideoFrameCallback()` never
called the page. A preview or processing pipeline driven by that API therefore
stalled even though the stream was live.

The Haiku MediaStream player now exposes metadata for each new frame, including
its dimensions, presentation count and timing. WebRTC receive/decode timing and
RTP timestamps are preserved when supplied by the source. The metadata is
shared under the frame lock; callbacks consume each new count once. The
accelerated video path continues to deliver frames directly to the compositor.

The native airOS workstation reproduced the failure with an animated canvas
captured into a video: 69 source frames, a playing 160×120 video, and **zero
callbacks** on engine `31a91fe`. Engine `831a8dc` passes all **11 local-stream
checks** and **7 WebRTC receive checks** in
`tests/fixtures/media-stream-frames.html`, served from localhost. The checks
cover a callback-driven canvas containing actual video pixels, repeated frames,
dimensions and timing, cancellation, pause/resume, size changes and stream end.
Two local WebRTC peers also confirm decoding, video pixels and RTP/receive/decode
metadata; they use generated video and no external signalling or STUN service.

To repeat, load the fixture and run `await testStreamFrameCallbacks()` and
`await testRemoteStreamFrameCallbacks()` in the console. Reload before repeating
the local-stream test, which stops its source at the end.

**#126 remains open.** This proves the frame-callback defect and its fix; it
does not establish that it is the only cause of Teams' reported failure. The
workstation currently enumerates no camera, and the laptop used for #79 was
unreachable during this check. Teams' self-preview and background effect still
need verification with the reported camera and user-agent setting.

## What the user sees now

- **A page can open a camera or a microphone as often as it likes.** Teams
  asks for the microphone four times and for the camera five times at once
  when it starts, keeps the first streams and asks again for the same devices
  by their ids, and keeps the meeting's preview while the call opens its own
  stream. Every one of those requests now gets a working stream; before, all
  but the first failed with `NotReadableError` ("The I/O read operation
  failed") and the tracks Teams kept ended with "capture failure". Streams of
  one camera can have different sizes (a 640x480 track next to a 1280x720
  one gets the middle of the picture, not a squashed one) and stop or change
  size on their own. webcam.org's test, which opens each device once, works
  as before.
- **Teams' notification channel connects, and the meeting page opens.**
  Teams talks to its servers over a WebSocket (its "Trouter" channel) on
  which the server speaks first; that first message used to wait in the
  network process until the next packet came, Teams' servers gave up after
  five seconds ("timeout"), and Teams connected again and again. Its meeting
  page waits for that channel: with the 0816 engine the page of an anonymous
  join stayed white for minutes (six timeouts, the camera never asked for);
  now the channel is up in 0.2 s and the pre-join screen shows the camera's
  live preview, the microphone (Realtek HD Audio) and the name field. A
  message a server sends right after a connection opens now arrives at once.
- **Teams treats Summit as Safari 26**, a supported browser: the meeting
  launcher offers *Continue on this browser*, and Microsoft's calling library
  (Azure Communication Services, which Teams on the web is built on) reports
  `isSupportedEnvironment: true` for `browser: Safari, browserVersion: 26.0`.
  Nothing in the user agent needed changing.
- **The device check at its bare address is broken for every browser** at the
  moment, from Australia at least: Microsoft's CDN answers
  `https://devicetest.teams.microsoft.com/` with a copy of its page from
  January 2024 whenever the request accepts Brotli (Chrome, Firefox, Safari
  and Summit all do), and that copy's scripts no longer exist (400), so the
  page stays white. Any other address of it gets the current page:
  **`https://devicetest.teams.microsoft.com/index.html`** (or `/?x`) loads
  and its check passes in Summit (network, camera, microphone and speaker
  all green). Nothing a browser can fix; the stale copy is on Microsoft's
  edge servers (`x-cache: TCP_HIT`, `Last-Modified: Fri, 05 Jan 2024`).

## How it works

### One connection per device, for every stream of it

A camera's Media Kit node (`usb_webcam`) has one output and serves one
consumer; a sound card's input is the same. Capture runs in the web process
(`WebCore/platform/mediastream/haiku/HaikuCaptureSources.cpp`), and each
`getUserMedia()` makes a new capture source, which used to connect a
consumer node of its own: the second source of a device found it in use.
Other browsers let any number of tracks share a device (Cocoa clones the
camera's source per page in the GPU process, and Core Audio's capture unit is
shared).

Now `HaikuSharedCamera` and `HaikuSharedMicrophone` hold the one connection
to a device for every source of it in the process (keyed by the device's
persistent id, made when the first source starts and closed when the last
one stops):

- **Camera.** The camera runs at the largest size a source asks for, and the
  connection is made again when that changes (a source asking for more, the
  largest one stopping or changing its size); if the camera cannot give the
  new size, the last one is kept. Each picture is converted to I420 once, and
  only when some source wants it (producing, not muted, due at its frame
  rate). A source at the camera's size gets it as before; a smaller one gets
  it cut to its shape around the middle and scaled (`fitPicture`, libyuv's
  `I420Scale`).
- **Microphone.** Samples are converted to float once and handed to every
  source, which applies its own volume. Voice processing (echo cancellation,
  noise suppression, gain control: one libwebrtc audio processing module for
  the process) keeps the state of a single stream, so the samples go through
  it once, in 10 ms chunks, when any source wants it; sources that turned
  echo cancellation off get the same chunks unprocessed.
- The sources keep their own settings, sizes, frame rates and timestamps;
  the shared object's lock is held while a buffer is handed out, so a source
  that stopped gets no more. Pages in other processes (another tab that
  landed in a different web process, another program) still find the device
  in use, as before.

### WebSocket data curl already holds

`CurlStream::tryToReceive()` (`WebCore/platform/network/curl/CurlStream.cpp`,
the network process's WebSocket stream) read once each time `select()` said
the socket was readable. curl reads ahead of what it hands out (a whole TLS
record, and its own socket buffer), and data it already holds does not make
the socket readable again, so the frame that came in the same packet as the
handshake's answer waited for the next packet. It now reads until curl
answers `CURLE_AGAIN`, as `curl_easy_recv()` asks, at most 1 MiB per pass
(a stream with more is read again on the next pass without waiting for the
socket).

## Checking it

- `tools/bench/pages/camera-share.html` (from `localhost` or with
  `SUMMIT_INSECURE_MEDIA_CAPTURE=1`; allow the camera and microphone): the
  Teams pattern (four microphones and five cameras at once, again by id),
  then 640x480 next to 1280x720, one stopping, `applyConstraints` on one
  track, and the camera free again at the end. Laptop, built-in camera:
  **6/9** with the 0816 engine (the three Teams steps fail), **12/12** with
  the new one. One camera connection was made for the whole Teams pattern
  (`SUMMIT_CAPTURE_TRACE=1`: "video connected: 1280x720" once).
- The Teams device check at `/index.html`: with the 0816 engine it passed
  but logged "A MediaStreamTrack ended due to a capture failure" 14 times;
  with the new engine every `getUserMedia()` succeeded and none ended.
- Microsoft's calling library 1.46.1 (from jsDelivr, in a page on
  `localhost`): `askDevicePermission`, `getCameras`, a `LocalVideoStream`
  rendered by `VideoStreamRenderer`, then a second preview of the same camera
  while the first keeps playing: 6/6. Its camera list fills in after the
  permission through `videoDevicesUpdated`, as in Safari (WebKit shows
  device names only once capture has started in the page).
- `tools/bench/pages/rtc-selftest.html?camera=1&size=1280x720`: 22/22 on the
  laptop. webcam.org's camera (1920x1080) and microphone tests run. Two
  microphone streams, one with echo cancellation and one without, both
  deliver samples at the same time (the unprocessed one at the level a
  single stream had with the 0816 engine, about -71 dB RMS in a quiet room;
  the processed one about 10 dB lower, noise suppressed).
- `tools/bench/pages/websocket-greeting.html`: three connections to
  `wss://echo.websocket.org/`, each timing the server's greeting and an echo:
  6/6 in three runs with the new engine (greeting 0-1 ms after the
  connection opened); 4/5 with the 0816 engine (the second greeting never
  came).
- The anonymous Teams join described under *What the user sees now*: white
  page with the 0816 engine, the pre-join screen with the camera's preview
  with the new one.

## Not done

- A Teams meeting with an account was not tried (no test account). The
  anonymous join of a made-up meeting (`teams.microsoft.com/meet/<id>?p=…`,
  *Continue on this browser*) reaches the pre-join screen with the camera's
  preview; joining fails there because the meeting does not exist.
- Teams' pre-join lists the speaker as *None*: the engine lists no audio
  outputs (`enumerateDevices()` has no `audiooutput`, and there is no
  `setSinkId()` to choose one), so sound plays on the system's default
  output.
- If Teams' request for the camera and microphone waits for the permission
  question longer than Teams allows, Teams gives up ("Media Stream request
  timed out") and asks whether to continue without audio or video; once
  Allow is remembered for `teams.microsoft.com` there is no question.
- Only the laptop's built-in camera was attached on 20 October; the C920 was
  not plugged in. The sharing is per device, so two cameras are two shared
  connections.
- Sharing across processes (two tabs in different web processes, or Summit
  and another program) would need capture in one process for all pages, as
  Cocoa's GPU process does. Going straight from a page that has the
  microphone (webcam.org) to a page of another site that asks for it can
  find it still in use for a moment, while the old page's process lets go.
