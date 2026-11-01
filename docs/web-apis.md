# Web APIs added for Safari parity (November 2026)

The Safari gap review of 1 November 2026 (issues #141-#159) found web
platform features that Safari ships but Summit lacked. This page describes the
ones Summit now has, how they work on air/OS, and how to test or turn them off.

## Gamepad API (#142)

`navigator.getGamepads()`, `gamepadconnected`/`gamepaddisconnected`.
Built with `ENABLE_GAMEPAD`. The UI process polls `BJoystick`
(`/dev/joystick/...`) every 8 ms on a thread of its own
(`Source/WebKit/UIProcess/Gamepad/haiku/UIGamepadProviderHaiku.cpp`) and hands
changes to the main run loop; WebKit sends them to the focused page of the
active window. As in other browsers a pad is visible to pages only after one
of its buttons is pressed.

- Pads keep Haiku's axis and button order (`mapping` ""); hats become two more
  axes. The Xbox 360 controller (which `usb_hid` describes itself) gets the
  "standard" mapping, its first hat as d-pad buttons 12-15.
- Rumble (`vibrationActuator`) is not available: `BJoystick` has no output
  path. Xbox One/Series pads (GIP) and Bluetooth pads are OS work:
  airos-works/haiku#53.
- `SUMMIT_GAMEPAD_MOCK=1` adds a pretend standard pad that moves its sticks and
  presses A every two seconds; `SUMMIT_GAMEPAD_TRACE=1` logs devices;
  `SUMMIT_GAMEPAD=0` turns the provider off.

## Pointer Lock (#143)

`Element.requestPointerLock()`. Built with `ENABLE_POINTER_LOCK`. WebCore
asks only from a user gesture in a focused page. The view
(`BWebKitView`, `WebKitView.cpp`) hides the pointer, keeps receiving pointer
events outside its bounds, and after every move sets the pointer back to its
middle, so it never stops at a screen edge; the page gets the moves as
`movementX`/`movementY`. Escape, the window losing focus or the view leaving
its window give the pointer back. Summit's status line says "Press Esc to show
your mouse pointer." while it is locked (`B_WEBKIT_POINTER_LOCK_CHANGED`).

`movementX`/`movementY` are now filled for every mouse event, not only while
locked (they were always 0).

## Media Session (#144)

`navigator.mediaSession` (metadata, `setActionHandler`). Built with
`ENABLE_MEDIA_SESSION`. Keyboard media keys reach Summit as unmapped keys
(`(0x0c << 16) | usage`, from `usb_hid`'s consumer page). The browser window
sends them to the tab playing sound, else to the selected tab, with
`BWebKitView::SendMediaCommand` ("toggle", "play", "pause", "stop", "next",
"previous", "forward", "backward"). The web process passes them to the page's
media session manager as remote control commands, so the page's action
handlers run (next/previous track on YouTube playlists, for example), else its
media element plays or pauses.

Media keys work while a Summit window is active. System-wide media keys and a
"now playing" display need an OS service: airos-works/haiku#55.

## Speech synthesis (#145)

`speechSynthesis`, `SpeechSynthesisUtterance`. Built with
`ENABLE_SPEECH_SYNTHESIS`. `PlatformSpeechSynthesizerHaiku.cpp` loads eSpeak NG
(`libespeak-ng.so.1`) or eSpeak (`libespeak.so.1`) at run time; neither is a
build or package dependency. eSpeak writes the utterance's audio into a buffer
on a worker thread; a `BSoundPlayer` plays it, and start, word and sentence
boundaries, pause, resume and the end are reported from the main run loop as
the sound reaches them. Rate 1 is eSpeak's 175 words a minute, pitch 1 its 50,
volume 1 its 100. Text that starts with `<speak` is read as SSML.

Without eSpeak installed `getVoices()` is empty and `speak()` ends with an
error event. air/OS images don't ship an engine yet: airos-works/haiku#52
(`pkgman install espeak` adds one). `SUMMIT_SPEECH_LIBRARY` names another
library file; `SUMMIT_SPEECH_TRACE=1` logs to stderr.

## Origin private file system (#146)

`navigator.storage.getDirectory()`, `FileSystemFileHandle.createWritable()`,
`createSyncAccessHandle()` in workers. WebKit's implementation is
cross-platform; its preferences were on only for Cocoa, GTK and WPE. Files are
kept in the profile's website data (`generalStorageDirectory`), counted by
`navigator.storage.estimate()` and removed with the site's data.
`SUMMIT_OPFS=0` hides the API.

## Smaller features (#147)

Turned on in `UIProcess/haiku/WebView.cpp`: scoped custom element registries
(`new CustomElementRegistry()`), `element.currentCSSZoom`, `font-variant-emoji`,
`ManagedMediaSource`, WebRTC encoded transforms (`RTCRtpScriptTransform`).
`canPlayType("audio/flac")` now answers like `audio/x-flac`.
