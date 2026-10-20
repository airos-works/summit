# Media fixes #83, #84 and #88 (20 October 2026)

Three things about videos that every other browser does: the browser's own
video and audio controls have their icons, a video that has not played shows
its first picture, and the screen stays on while a video plays. Everything was
tried on the X399 with a test copy of the browser on a test engine built from
CI's tree (`/boot/home/summit/claude-s36/bundle-mediaapp1`, engines `media2` to
`media5`).

## #83 Icons in the native video and audio controls

`<video controls>`, `<audio controls>` and a video or sound opened directly in
a tab now show their buttons: the big play button, play/pause, skip back and
forward 15 s, mute (with the muted icon), full screen and Picture in Picture.
Before, the buttons were empty grey shapes.

With Skia the engine uses WebKit's Adwaita theme, and the media controls ask
the theme for each icon by name (`RenderThemeAdwaita::
mediaControlsImageDataForIconNameAndType`). GTK and WPE read them from
GResources and Windows from files beside the DLL; Haiku had neither and got
nothing. The icons (`Modules/modern-media-controls/images/adwaita/*.svg` and
the two PNG placards) are now built into libWebKit as a table,
`Source/WebCore/platform/haiku/MediaControlsImagesHaiku.h`, written by
`Tools/Scripts/generate-media-controls-images-haiku.py` (run it again after
the images change; nothing in the build runs it, so no CMake change was
needed).

Two buttons needed more than icons:

- **Full screen** was disabled: WebKit only offers it when the media player
  says it supports full screen, and the Haiku players (file and MSE) said
  no. They now say yes (`supportsFullscreen()`), and the button makes the
  video element full screen through the Fullscreen API, as a page's own
  button does. Esc or the button leaves.
- **Picture in Picture**: the Adwaita controls have no PiP button of their
  own, and the controls run in an isolated script world that cannot call the
  browser's PiP page script (#74). The controls now ask the page with a
  cancelable `summitpictureinpicturequery` event on the video; Summit's PiP
  script cancels it, so the button shows, and a click sends
  `summitpictureinpicturetoggle`, which the script answers by entering or
  leaving Picture in Picture (`media/pip-support.js`, `src/ui/PageScripts.cpp`).
  Without the script (another embedder) the button stays hidden.

What screen readers are told for the times and the scrubber
(`mediaControlsFormattedStringForDuration`) was the raw number of seconds;
it is now "1 minute, 33 seconds" in the user's language (the Locale Kit's
`BTimeUnitFormat`, as `BDurationFormat` joins units).

Checked: the icons of a 1080p `<video controls>` before and after playing, a
300 and a 600 px `<audio controls>` (play, pause, mute, muted), a video opened
directly; full screen in and out; PiP in and out from the controls' button;
the duration text for 0, 1, 12, 60, 93.4, 3600, 3725 and 5400.5 s ("0
seconds" … "1 minute, 33 seconds" … "1 hour, 2 minutes, 5 seconds").

The overflow (gear) button that appears after playback sits close to the mute
button at the right end of a narrow bar; that is the Adwaita layout, not the
icons.

## #84 First frame of a video that has not played

A paused video that has not played yet (`preload="auto"` or `"metadata"`, no
poster) shows its first picture once it has loaded, inline and opened
directly; before, it was an empty box until it played. A seek while paused
shows the new picture. `preload="none"` still shows nothing.

The file/HTTP player (`MediaPlayerPrivateHaiku.cpp`) releases the video
decoder of a paused video as soon as it has identified the tracks, so nothing
was ever decoded until play. It now decodes one picture on the loading thread
first (`decodeFirstFrameHaiku()`), before the page hears that the media
loaded, so `readyState` 4 really has a current picture, and then releases the
decoder as before. The picture stays in the frame buffer for painting and is
handed to the compositor. The cost is part of a paused video's load now: one
1080p H.264 picture took about 0.33 s on the X399 (Media Kit's FFmpeg decoder
with 32 frame threads, over HTTP). A page of eight paused 1080p videos had
its last `loadedmetadata` after 20.3 s instead of 17.0 s. That page was slow
before too, with the CPU mostly idle: each video's `BMediaTrack::
DecodedFormat` already decodes a picture (`AVCodecDecoder::
NegotiateOutputFormat`), and the eight loads wait on each other inside the
decoder (a limit of two first-picture decodes at a time made it worse, 24 s,
so there is none).

Seeks while paused already decoded the target picture (the decoder's preroll)
and needed nothing new. The MSE player already showed the first picture after
a flush, paused or not; checked with a fragmented MP4 appended without
playing, and a seek to 6.5 s while paused.

Seen while testing, not changed: `bbb12s.mp4` over HTTP plays its video about
three times too fast on the X399 with the released 0816 engine as well (the
resumed track's seek to 0 lands on a keyframe "at -7.9 s" and frame times
advance 1 ms per frame); a seek elsewhere is fine.

## #88 The screen stays on while a video plays; Screen Wake Lock

While a video with sound plays in a tab that can be seen, inline or full
screen, the screen blanker does not start; once it is paused or ends, the
blanker comes on its usual schedule. `navigator.wakeLock.request("screen")`
works on secure pages (https, file:) after a click and keeps the screen on
until the page releases the lock or is hidden (another tab selected, the
window minimized), as the specification asks.

How it works:

- WebKit already asks for this: `HTMLMediaElement` makes a "display" sleep
  disabler while a video with sound plays visibly (a hidden page's video asks
  only for the system, a muted or looping one for nothing), and the Screen
  Wake Lock API makes one per locked document. Haiku was excluded from the
  media part (`shouldDisableSleep`), and its web process had no
  `SleepDisablerClient`, so the requests went nowhere.
- The web process now sends them to the page's proxy in the browser process,
  as Cocoa does (`WebSleepDisablerClient`, installed in
  `WebProcessHaiku.cpp`). There a client of the UI process
  (`ScreenAwakeRequestsHaiku`, `UIProcess/haiku/WebView.cpp`) counts the
  display requests per page and the view's state says `keepScreenAwake`
  (`B_WEBKIT_STATE_CHANGED`, documented in `WebKitView.h`). A page's
  requests are also dropped when its main frame commits a new document:
  after a process swap the old document's "done" messages go to its
  suspended page, which ignores them, and the screen would stay on.
- The browser keeps the screen on while the current tab of a window asks:
  every 20 seconds it puts the pointer where it already is
  (`set_mouse_position`), which input_server counts as use of the computer,
  as airTime and MediaPlayer do. Not while the window is hidden, minimized or
  on another workspace, nor while a mouse button is held
  (`BrowserWindow::UpdateKeepScreenAwake`, `KeepScreenAwake`).
  `SUMMIT_KEEP_AWAKE_TRACE=1` logs each step.
- The Screen Wake Lock API is on again. It had been turned off because the
  official Speedometer page lost its page process when it asked for a lock;
  with the API on, its Start Test asks for one and the suites run. A lock
  needs a click first, as in Safari: without one, the permission query
  answers "prompt" and the request is refused. `SUMMIT_SCREEN_WAKE_LOCK=0`
  turns the API off.

Checked on the X399 (the shared desktop's blanker is set to 15 minutes, so
the blanker itself was not shortened): a probe reading `idle_time()` every
half second while a wake lock was held showed the idle time reset within
0.1 s of each of the browser's moves, 20.0 s apart (it never passed 20 s);
the keep-awake turned on for a playing video (inline, opened directly,
YouTube) and a granted lock, and off on pause, at the end, on release, when
the tab was hidden (the page saw `release` then `visibilitychange` hidden)
and when the tab went to another site while the video played; back again,
the video resumed from the back/forward cache and the keep-awake with it.
