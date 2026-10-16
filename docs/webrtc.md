# Peer to peer: camera, microphone, screen sharing, MediaRecorder, WebRTC, Web Audio

Summit's engine now has the pieces html5test.co lists under "Peer to peer"
(RTCPeerConnection, data channels, `getUserMedia`, `getDisplayMedia`,
`enumerateDevices`, `MediaRecorder`) and Web Audio, which call clients use for
level meters and noise processing. The goal is Zoom and Teams calls in the
browser with a webcam and a shared screen.

## What is built and where

The engine options (`Source/cmake/OptionsHaiku.cmake`): `ENABLE_MEDIA_STREAM`,
`ENABLE_WEB_RTC` (with `USE_LIBWEBRTC`), `ENABLE_MEDIA_RECORDER` and
`ENABLE_WEB_AUDIO` are on. An existing build directory keeps its cached
values, so they have to be passed once (`-DENABLE_MEDIA_STREAM=ON
-DENABLE_WEB_RTC=ON -DENABLE_MEDIA_RECORDER=ON -DENABLE_WEB_AUDIO=ON`); the
change rebuilds everything.

### libwebrtc

WebKit's vendored libwebrtc (`Source/ThirdParty/libwebrtc`, built by its
`CMakeLists.txt`) builds for Haiku with a branch of its own in that file:

- `WEBRTC_POSIX` without `WEBRTC_LINUX`: no epoll, prctl or `/proc`. The
  socket server uses `poll()` (select() is limited to descriptors below
  `FD_SETSIZE`, and WebKit processes have more).
- BoringSSL's symbols get the prefix `webrtc_bssl_`
  (`BORINGSSL_PREFIX`): the same processes load the system's OpenSSL (curl,
  WebCrypto), and an unprefixed BoringSSL archive member could satisfy a
  reference meant for OpenSSL.
- Opus is the bundled one; libevent and ALSA are not used (WebKit's
  `LibWebRTCAudioModule` drives audio, the stdlib task queue runs tasks).
- libvpx gets SIMD per file (`-mavx2` only on `*_avx2.c` and so on) and
  dispatches at run time, so the library stays baseline x86_64 and ARMv8.0:
  upstream compiles all of it with `-mavx2`, or `-march=armv8.2-a+dotprod` on
  arm64, which the Raspberry Pi 4's Cortex-A72 cannot run. On arm64 Haiku the
  configuration is NEON only (`vpx_config.h`, `aarch64_cpudetect.c`) and
  BoringSSL takes its CPU features from the compiler
  (`OPENSSL_STATIC_ARMCAP`; the Pi has no crypto extensions).
- Source changes, all marked `WEBRTC_WEBKIT_BUILD change`: thread ids and
  names (`find_thread`, `rename_thread`), CPU count, process and thread CPU
  time, resident memory (`get_next_area_info`), interfaces that are up
  (`IFF_LINK`), socket options Haiku does not have (no-ops), `IP_DONTFRAG`,
  `MSG_NOSIGNAL`; abseil's `GetTID()`.
- `getifaddrs()` comes from libbsd; the library links `bsd` and `network`.
- The WebM muxer (libwebm's mkvmuxer) is built into it for MediaRecorder.

Video codecs are libwebrtc's own, VP8 and VP9 through libvpx, and H.264
through libraries of the system, loaded when first needed
(`LibWebRTCH264DecoderHaiku.cpp`):

- Decoding through FFmpeg: only functions are looked up in libavcodec, and
  only the leading members of AVPacket and AVFrame are used, which have kept
  their places for many major versions.
- Encoding through OpenH264 (the `openh264` package, `libopenh264.so.7`):
  libwebrtc's own encoder for it (`h264_encoder_impl.cc`), built against
  OpenH264 2.4.1's API headers (in `third_party/openh264`, BSD), with the two
  functions it calls by name looked up in the library
  (`h264_encoder_haiku.cc`). Another version of the library is not used: the
  parameter structures are passed by pointer. Haiku's FFmpeg has no H.264
  encoder (neither x264 nor OpenH264).

Without the library, H.264 is not offered in that direction (calls fall back
to VP8 or VP9). `SUMMIT_WEBRTC_H264=0` turns both off.

The bundled libvpx, Opus and libsrtp are compiled with hidden visibility: the
same processes load the system's FFmpeg (through the Media Kit's ffmpeg
add-on), whose own libvpx and libopus calls must not bind to these copies.

### Capture (`WebCore/platform/mediastream/haiku`)

Capture runs in the web process, as on GTK and WPE; the UI process asks it for
devices and to check a request's constraints (`UserMediaCaptureManager`,
the GLib files, which are portable).

- `HaikuCaptureNode.{h,cpp}`: device lists from the Media Kit (live nodes that
  are physical inputs producing raw video or raw audio; the default one
  first) and a consumer node of ours connected to the chosen output.
  Persistent ids are `haiku:<node name>` (node ids change across restarts).
- Camera (`HaikuVideoCaptureSource`): the Media Kit node of a USB camera
  (`usb_webcam` add-on) sends `B_RGB32` pictures of the size put in the
  connection's format; they become I420 frames (libyuv). Presets 160x120 to
  1920x1080 at up to 30 per second. One consumer at a time: a camera in use
  elsewhere fails with "The device is in use by another program."
- Microphone (`HaikuAudioCaptureSource`): a sound card's input or a USB
  microphone, samples converted to interleaved float. The sound card node is
  never stopped (it also plays). The default is the system's (Media
  preferences); on the X399 that is the motherboard's input, and the C920's
  microphone is "USB Audio".
- The media services do not break the connections of a program that ended,
  and Summit ends its web processes at once (`_exit`, or SIGKILL): a device
  would stay in use for every program until the services restart. The web
  process disconnects its devices when its connection to Summit closes
  (`HaikuCaptureConnection::disconnectAllBeforeExit`, at most half a
  second), and connecting to an input whose output still points at a port
  that is gone takes it back.
- Screen (`HaikuDisplayCaptureSource`): `BScreen::ReadBitmap` on a thread of
  its own at the frame rate asked for (15 by default), converted to I420 and
  scaled to the size asked for. Reading the screen holds app_server's drawing
  lock, and every window waits meanwhile, so the picture is read in bands of
  128 rows with a pause after each twice as long as the read: no wait is
  longer than one band, and the desktop keeps two thirds of app_server's
  time on any machine. At 200% app_server used to copy the high-resolution
  buffer and average it pixel by pixel (250 ms for the X399's 3840x1080,
  about 2 pictures a second shared); since the X399 fork's 348a5d8cc2 it
  averages straight from the buffer (10 ms), and the whole desktop is shared
  at the 15 a second asked for. A blanked screen reads back black.
- Window: the same, cropped to one window's frame, which is followed as the
  window moves (the private `get_window_info`); the stream ends when the
  window closes. Windows are listed as `Window: <title>` after the screens.
- Echo cancellation, noise suppression and gain control for the microphone
  (`HaikuVoiceProcessing`): libwebrtc's audio processing module (AEC3),
  with what the page plays (streams and Web Audio) as the echo reference.
  `SUMMIT_VOICE_PROCESSING=0` turns it off.
- The Media Kit sends no notice when a camera is plugged in or taken out:
  a change shows the next time the device list is computed (a page's
  `enumerateDevices()`, a `getUserMedia()`), and only a list that differs
  from the last one is announced as `devicechange`. WebCore's
  `haveDevicesChanged()` cannot be used for that: it answers "changed" for
  two equal lists, and announcing on its word made the observers enumerate
  again, which announced again, five times a second for as long as a page
  listened for `devicechange` (webcam.org's test page opens and closes the
  camera and microphone in its handler, 16 October 2026).
- The capture consumer keeps the system clock as its time source, and the
  process holds that clock's proxy object for good. The Media Kit keeps one
  proxy object per time source in a process and counts references to it;
  when the last one goes, the object is deleted on the roster's thread, and
  a thread asking for the same time source meanwhile is handed the object
  being deleted (`TimeSourceObjectManager::GetTimeSource` finds it in the
  map before `ObjectDeleted` removes it). Consumers used to adopt the
  producer's time source for each connection and release it when they
  went; a page opening and closing the microphone again and again crashed
  the web process in the new consumer's control thread, in
  `BTimeSource::GetTime` on the freed object (laptop, 16 October 2026).
  Neither a camera's producer nor a sound card's node waits for the start
  time it is given, and the buffers' stamps are not used for scheduling.
- OS-side, seen the same day: `hmulti_audio`'s `MultiAudioNode::Disconnect`
  deletes the output's buffer group without `fBufferLock`, while
  `_OutputThread` may be filling one of its buffers
  (`Resampler::_CopyInt2Int` in `_FillNextBuffer`, writing into the
  freed buffer, crashed the media_addon_server twice in about 125
  microphone connections and disconnections on the laptop, with either
  engine). The fix belongs in the add-on: take `fBufferLock` in
  `Disconnect()` and `SetBufferGroup()`. The media_server does not relaunch
  the add-on server by itself.
- `SUMMIT_CAPTURE_TRACE=1` traces device lists and connections.

### Frames and samples

- `VideoFrameHaiku` (`platform/graphics/haiku`): the port's video frame. I420
  pictures (cameras, screens, WebRTC) share libwebrtc's buffers without a
  copy either way; BGRA/RGBA ones come from pages (canvas, WebCodecs).
  Conversions, scaling and rotation with libyuv; images for painting are
  made once per frame.
- `HaikuAudioData` (`platform/audio/haiku`): interleaved 32-bit float, the
  format every Haiku audio source hands on.

### Playing streams

- `MediaPlayerPrivateMediaStreamHaiku`: a `<video>` or `<audio>` with a
  `srcObject`. The newest picture goes to the compositor as it arrives (I420
  as three textures converted by the YUV shader,
  `CoordinatedPlatformLayerBufferVideoFrameHaiku`).
- `AudioMediaStreamTrackRendererHaiku`: each audio track plays through a
  BSoundPlayer of its own, after a short FIFO that drops late samples.

### WebRTC glue (`platform/mediastream/libwebrtc/haiku`)

The provider (`LibWebRTCProviderHaiku`) and the incoming and outgoing audio
and video sources. Outgoing audio goes to libwebrtc as 10 ms chunks of 16-bit
samples at the capture rate; libwebrtc resamples. Sockets live in the network
process (`NetworkRTCProvider`, as on GTK).

Host candidates are not hidden behind mDNS names on Haiku
(`ICECandidateFilteringEnabled` is off in `UIProcess/haiku/WebView.cpp`, and
the provider says it has no mDNS): a random `.local` name needs a responder
on the network to answer for it, and Haiku has none, so neither this machine
nor the other peer could resolve it and peers on the same network never
connected. Pages that make peer connections can see the default route's
address, as in browsers before mDNS candidates.

### MediaRecorder

`MediaRecorderPrivateHaiku`: WebM with VP8 (default) or VP9 and Opus,
encoded on a work queue of its own (libvpx, Opus) and muxed as it goes
(libwebm). Audio is resampled to 48 kHz stereo. The video track carries the
stream's nominal frame rate, as Chrome writes it: a live file has no
duration or index, and Haiku's FFmpeg reader, which counts frames, otherwise
takes the timestamps' unit (a thousand a second) as the rate.

### Web Audio

- `AudioDestinationHaiku`: a BSoundPlayer at the system mixer's rate.
- `FFTFrameHaiku`: PFFFT from the libwebrtc tree (the stub asserted on every
  analyser, convolver and panner).
- `decodeAudioData`: PCM WAV directly, everything else through the Media
  Kit (`AudioFileReaderHaiku`).
- PannerNode's HRTF responses are built into the library.
- `MediaStreamAudioSourceNode` and `MediaStreamAudioDestinationNode` work
  with streams (`MediaStreamTrackAudioSourceProviderHaiku`).

## Permissions (engine and Summit)

`decidePolicyForUserMediaPermissionRequest` (UIProcess/haiku/WebView.cpp)
goes through `SitePermissionsHaiku::requestMedia`:

- Camera and microphone are decided for the site in the address bar and can
  be remembered (Block / Allow), like notifications and location. A request
  for both is one question ("camera-microphone"), saved as both.
- A screen is asked for every time. The request lists the screens; Summit
  shows a picker and answers with the one chosen
  (`BWebKitContext::RespondToMediaPermissionRequest`).
- Tabs that use the camera, microphone or a screen get a red dot
  (`capturingCamera`, `capturingMicrophone`, `capturingScreen` in
  `B_WEBKIT_STATE_CHANGED`).

`getUserMedia` needs a secure context (https, localhost, file:);
`SUMMIT_INSECURE_MEDIA_CAPTURE=1` allows plain http for testing.

## Testing

`tools/bench/pages/rtc-selftest.html` checks the html5test features, Web Audio
(an oscillator into an analyser), a canvas stream played locally, recorded
and played back, and a two-peer loopback with video, audio and a data
channel, again with H.264 when both directions offer it. It needs no camera;
`?camera=1` (with `&size=1280x720`, `&mic=USB`) and `?screen=1` add the real
devices. The title ends as `rtc selftest: N/M`. Sharing a screen needs a user
gesture: the page waits for its Share screen button, or for
`rtcShareScreen()` evaluated in the Developer Tools console (with
`SUMMIT_ENABLE_INPUT_SYNTHESIS=1`, `summitctl devtools-do evaluate`).
`summitctl permission allow|block|notnow [N]` answers the open permission
prompt (N: the screen or window to share).

Calls between machines: `python3 tools/bench/rtc-signal.py 8765` on any host,
then `http://HOST:8765/rtc-call.html?role=a&room=R` on one machine and
`role=b` on the other (`&camera=1`, `&codec=H264`, `&sendvideo=0`); each side
posts its outcome to the server (`GET /signal/R/result-a`). Results on 5
October 2026: the X399 (C920) and the Raspberry Pi call each other with VP8
both ways, or H.264 from the X399's OpenH264 to the Pi's FFmpeg (the Pi
receiving only: it has no encoder, and a send-and-receive line offers only
codecs both directions have); Summit calls Chromium (VP8 and VP9).

## Not done yet

- System audio capture (sharing what the computer plays).
- WebCodecs (WebKit's are Cocoa's or GStreamer's; Zoom's web client uses
  them when present).
- `devicechange` events: devices are listed when asked, but plugging one in
  is not announced.
- Not yet tried against Zoom or Teams themselves (they need meetings to
  join); Summit presents itself as Safari, which both support.
