# Development verification

> A dated log, newest first. What older entries call missing or not done
> (multiple windows, private browsing, certificate screens, permissions,
> developer tools, updates, the downloads panel) has mostly been done since:
> see [REQUIREMENTS.md](REQUIREMENTS.md), the README's feature list and the
> GitHub issues for what is left.

## 22 October 2026: Inspector CPU and memory recordings

The unprofiled airOS candidate `eefce05` passes 218 native resource-usage
checks: worker CPU and target identities, allocation/GC accounting, recording
restart and overlap, detach cleanup and normal shutdown. All five loaded
candidate binaries match their manifest, with clean crash coverage and stable
input/runtime hashes. Published packages and a stock-Haiku candidate still need
acceptance; both published baselines confirm the domains were unavailable.
See [the resource-usage report](resource-usage-2026-10-22.md). JIT symbols,
extension background attachment and the native Timelines UI remain unfinished.

## 2 October 2026, evening: NanoKVM screen stayed black

Reported by the owner: the NanoKVM at 192.168.1.8 (attached to the Rock 5)
showed its controls in Summit on the X399 but not the remote screen, while
Firefox showed it. Fixed in `bundle-ywvpzj7_` (`SkiaCGMiPGO`, engine patch
`7655417f`), installed as the launcher; the previous one is
`launcher-backups/Summit-current.pre-20261002-1758.sh`.

- **How NanoKVM sends the screen.** Without `RTCPeerConnection` (WebRTC is
  compiled out) its page uses MJPEG: an `<img>` on
  `/api/stream/mjpeg`, a `multipart/x-mixed-replace` response that never
  ends. While the machine shows no picture (booting, blanked, changing mode)
  the server sends nothing at all, and the connection stays open.
- **Cause.** libcurl's request timeout in WebKit is an idle timeout
  (`CURLOPT_LOW_SPEED_TIME`, 60 s by default), and it covered the body too.
  After about 66 s without a frame the stream failed ("Timeout was
  reached"), the image went broken and frames sent afterwards never
  arrived. NanoKVM's page also hides the screen for good after an image
  error, so only a reload brought it back.
- **Fix.** Once a `multipart/x-mixed-replace` or `text/event-stream`
  response has started, the idle timeout no longer applies to it
  (`CurlRequest::didReceiveHeader`). Other responses keep it.
- **Verified** with `tools/bench/mjpeg-stall-server.py` (frames, 70 s of
  silence, frames): `bundle-7txch8aq` failed at 69 s and stayed on the first
  picture; the new build and Firefox 156 both showed the later frames from
  73 s. The real NanoKVM streams the Rock 5's desktop in the new build.

## 2 October 2026: GitHub issues #6-#15

All on the X399, built incrementally in `SkiaCGMiPGO` (profile-guided, engine
patch `26a0ad8f`) and installed as `bundle-7txch8aq`: the desktop launcher and
`/boot/home/summit/Summit-installed` point at it, the previous launcher is in
`/boot/home/summit/launcher-backups/Summit-current.pre-20261002-1541.sh`. The
Summit that was running keeps its build until it is restarted.

- **#6, WebAssembly start-up.** Creating a module's memory cost about 125 ms
  (Haiku's `mprotect()` walks the whole 4 GiB reservation, twice); now 1.9 ms.
  [performance.md](performance.md).
- **#7, 1Password after the password.** Not reproducible without the owner's
  account. Key derivation is as fast as in Firefox (WebCrypto much faster);
  #6 and the frame pacing of #9 remove costs 1Password hit. Left for the
  owner to time again.
- **#8, address field type-ahead.** Inline completion of visited and
  bookmarked sites and path segments, a suggestion list (history, bookmarks,
  search), ranking by visits and recency ([browser-ui.md](browser-ui.md)).
- **#9, WebGL Aquarium and xbitlabs' FPS test.** Frames were paced from the
  page's request rather than the refresh (no display link on Haiku): Aquarium
  52 -> 60 frames/s (5,000 fish 28 -> 40). Canvases were redrawn only every
  couple of seconds after a full-canvas `clearRect()` (a Haiku canvas damage
  optimisation never reset its rectangle for composited canvases): xbitlabs
  "High" 35 -> 42 frames/s with its 1% low 1 -> 14. `requestAnimationFrame`
  no longer runs twice within half a frame. [performance.md](performance.md).
- **#10, notifications.** Permission prompt, Haiku notifications with the
  site's icon, clicks focus the tab and reach the page, decisions in
  Preferences › Site Permissions ([site-permissions.md](site-permissions.md)).
- **#11, location.** `navigator.geolocation` through BeaconDB (Wi-Fi networks
  or the network address), asked per site; what Haiku lacks is listed in
  [site-permissions.md](site-permissions.md) and on the issue.
- **#12, YouTube.** Seeks decode only the pictures needed to reach the target
  (783 -> 301 ms from data to playing); a large blurred element (the
  thumbnail behind the player) is composited and blurred on the GPU instead
  of on the main thread for every tile while a new video loads.
- **#13, topcoder.com and the router dashboard.** A page background set after
  the first paint hid the page (TextureMapper drew the colour alone); fixed
  for topcoder, the router dashboard needs the owner's login to confirm. The
  colour is now drawn under the page's tiles, so a tile not painted yet shows
  the page background instead of black.
- **Found on the way: stale tiles.** GitHub's issue list kept showing parts of
  an earlier layout: the tile upload budget took tiles in view for tiles
  outside it. Tiles in the viewport are now always uploaded at once.
- **#14, #15, dropping and pasting files.** Files dropped from Tracker reach
  drop zones and file inputs (`files` and `items`), or open in the tab;
  pasted BBitmaps arrive as `image.png`; rich paste into editable content
  ([clipboard-and-drag.md](clipboard-and-drag.md)).

## 1 October 2026, afternoon: tabs that stopped drawing

Reported by the owner with `bundle-xgce7ywe`, again after restarts on
`bundle-hwd4u30l` and `bundle-swhextn6`, the last time after opening WebGL
Aquarium in a new tab: in a GitHub issue tab, typed text did not appear
until another tab was selected and then this one again. Not fixed yet:
installed now as `bundle-aesogsiz`, which reports where the next one is
stuck and restarts it.

- **What the stalled tab showed.** The page still ran (keys reached it,
  `setTimeout` and promise callbacks ran, the textarea held the text) but no
  rendering update happened: a `requestAnimationFrame` callback waited 35
  minutes, and neither the compositor thread nor Skia's painting threads used
  any CPU when keys were pressed. A tab switch suspends and resumes the page's
  rendering, after which it drew again.
- **Not reproduced.** A copy of the owner's profile typed into GitHub's issue
  form for 30 minutes (51 rounds, with tab switches); the Aquarium was opened
  in a new tab from a link and with Summit's new-tab command, left in front
  for 15 seconds, and run in a second window, with the issue form or
  `tools/bench/pages/render-liveness.html` (frames per second, no input) in
  the first tab. Every time the first page went back to drawing at once.
- **A false lead.** The first stall report (`render timer active and armed`,
  61 seconds) came from a tab returning from behind another: the time it
  had spent hidden counted as waiting. Hidden time no longer counts. Also
  seen: Summit's window thread blocking for about 4 seconds at a time; that
  is a full-screen capture (VNC or a screenshot tool) holding app_server, not
  the Aquarium (`BPrivate::get_display_layout()`, which the window thread
  asks for the scale, took 4 to 6 ms with the Aquarium running).
- **What reports the next one.** Once a second, a page in view that has had
  no rendering update for two seconds while something asked for one prints a
  `Summit page stall` line to standard error: whether WebCore's rendering
  update scheduler has a refresh pending and whether its timer is running,
  whether this page's renderer was asked to draw, whether its update observer
  is pending and whether it is waiting for the compositor; a page in view
  whose rendering stays suspended or frozen is reported too. It then
  restarts each pending step. A page waiting for the compositor gets the
  compositor's own `Summit compositor stall` report, which ends with the
  render timer as the run loop and the process's timer thread see it
  (`describeTimerHaiku()`: armed or not, when due, how it was posted, which
  notifications the loop received and rejected, what the timer thread
  holds), and the compositor renders the due composition at once.
- **Checked.** `SUMMIT_TEST_COMPOSITOR_STALL=late` (the render timer armed
  an hour late), `=timer` (not armed) and `=frame` (a frame confirmation
  lost) each made one stall after 100 compositions; every one cost a single
  second without frames before the liveness page went back to 60 frames per
  second. Without a stall, and across the Aquarium tab switches, nothing was
  reported.

## 1 October 2026, later: a frozen tab, 1Password's popup and a browser that would not close

Reported by the owner with `bundle-5wfht_91`: 1Password's toolbar popup only
spun, and Summit hung so that the machine was restarted. Fixed and installed
as `bundle-xgce7ywe` (same `SkiaCGMiPGO` engine, incremental), started from
the Deskbar with the owner's profile: the restored Proton tab answers and
1Password's popup offers to unlock there.

- **A deadlock between JavaScriptCore timers.** On Proton Mail's sign-in page
  the tab's main thread stopped for good, waiting for JavaScriptCore's one
  timer lock (`JSRunLoopTimer::Manager`). Proton's crypto worker compiles
  WebAssembly; the compiler thread that finishes it takes that lock and arms
  the worker's RunLoop timer, and in the Haiku port arming a timer took the
  looper lock of the timer's thread. The worker held its looper while firing a
  timer and waited for the timer lock; the main thread followed on the same
  lock. The page looked normal but ran nothing any more, so 1Password's
  content scripts never answered its popup (details in
  [webextensions-1password.md](webextensions-1password.md)). The worker
  run-loop fix of #4 below made it possible: worker threads never cycled their
  looper before. `RunLoopHaiku.cpp` now keeps timer state under a lock of its
  own, and no thread waits for another thread's looper to arm, stop or query a
  timer. `tools/bench/pages/worker-timers.html` (four workers compiling
  WebAssembly while the page creates frames) never posted its result on
  `bundle-5wfht_91` and completes now; the Proton tab answers Developer Tools
  and the popup offers to unlock there, as the build before #1-#4 did.
- **A hung tab could not be closed.** The port cancelled a close when a page
  did not approve it within 30 seconds, so the frozen tab kept its window and
  the application open (a copy of the owner's profile would not quit), which
  is the likely reason for the restart. A page that has not approved closing
  within 10 seconds is now checked with a ping; if its process does not answer
  that either (3 seconds), the page closes and the process is ended once its
  last page is gone. A process that answers keeps the close cancelled as
  before. `tools/bench/pages/hang.html` (a script that never ends) closes after
  14 seconds and its spinning process ends; quitting the browser with it open
  takes 15 seconds. Before, neither ever finished: the owner's instance on
  `bundle-5wfht_91` still had not quit 45 seconds after being asked.
- **The restart itself.** The previous boot's system log has no kernel panic
  and no crash report, and Summit's log ends at 06:55 with ordinary input
  traces while the popup was being clicked. WebGL Aquarium had been open 7
  seconds before Proton; that session logged no GPU or WebGL errors.
- **Found on the way, not fixed:** asynchronous `WebAssembly.instantiate()`
  is slow in workers and stalls the page. `worker-timers.html` with one worker
  managed 156 instantiations in 20 seconds (four workers: about 40 each), and
  the main thread paused for up to 0.45 s (2.5 s with four workers); Firefox
  155 managed about 11,000 per worker with pauses under 0.7 s. Without
  workers the page's main thread never paused longer than 52 ms.

## 1 October 2026: GitHub issues #1-#4

Engine patch and browser built on the X399 in the `SkiaCGMiPGO` directory
(profile-guided, now with WebGL; a full rebuild), installed as
`bundle-5wfht_91`.

- **#1, a router's self-signed certificate.** A warning with the reason,
  subject, issuer, names, dates and fingerprint; Continue trusts that
  certificate for that host and is remembered in the profile
  ([certificate-warnings.md](certificate-warnings.md)). Verified with a
  self-signed test server (trusted after Continue and after a restart;
  another certificate on the same host asks again) and with the owner's
  UniFi router at 192.168.1.1: the warning said "it is self-signed, it is
  not issued for this address", names and fingerprint matched `openssl`,
  and Continue opened the UniFi OS login. Found on the way: WebCore's
  certificate names all read "true" on every port but Windows.
- **#1, not fixed: a blank page on the router's root.** `https://192.168.1.1/`
  (the application moves itself to `/login`) often stays white until the
  window is resized or uncovered, although the page is laid out, painted
  (first contentful paint 0.9 s) and presented with the right damage; the
  installed build does the same through a plain HTTP proxy to the router
  (`http://…:8950/`), and `/login` loaded directly is fine. Not caused by
  certificates, damage tracking, zero-copy presentation, the read-back or
  the tile budget (each switched off in turn); `SUMMIT_SWAPCHAIN_TRACE=1`
  now also logs each presented frame's damage and the view's draws for the
  next attempt.
- **#3, WebGL.** WebGL 1 and 2 on the GPU through ANGLE and the private
  zink Mesa ([webgl.md](webgl.md)); WebGL Aquarium at 52 frames/s with 500
  fish. Firefox 155 has no WebGL on this machine.
- **#4, Proton Mail's sign-in stopping after the second factor.** Proton's
  crypto worker derives keys with Argon2 in WebAssembly and waits on
  `WebAssembly.instantiate()`, which never settled in a worker on Haiku:
  worker threads never cycled their RunLoop, so JavaScriptCore's deferred
  work timer never fired there. Fixed in `WorkerRunLoop` (and
  `RunLoop::cycle()` on a standalone looper; it exposed a timer deadlock,
  fixed later the same day, see above);
  `tools/bench/pages/worker-wasm.html` settles now and hung before. The
  sign-in itself could not be tried without an account; the steps before
  the second factor work (an unknown user is reported). WebCrypto Ed25519
  and X25519, which Proton's OpenPGP.js uses when present, now exist on the
  OpenSSL backend.
- **#4, xbitlabs.com's FPS test.** 11 frames/s ("CRITICAL") before, 51
  ("PERFECT") now: canvas shadows no longer take an offscreen layer per
  shape. Its advertising frames also left empty files in Downloads; frames
  inside a page now only download attachments.
- **#2, Firefox comparison.** `tools/bench/run-pageload.py` and the
  results in [performance.md](performance.md): Summit paints as soon as
  Firefox on most pages; Reddit's load event (20 s against 4 s, forced
  nested flex/grid layout in the web process) and GitHub's first byte
  (+0.5 s) are the gaps found.

## September 21, 2026: rendering performance on real hardware

Work moved to the owner's Haiku workstation (Threadripper 1950X, GeForce GTX
1070), which is now a build and measurement target of its own
([workstation](workstation.md)). Details and tables in
[performance](performance.md).

- **Scrolling is 2.8x faster** on a 400-card feed (12.2 -> 34.8 fps in the VM;
  42.1 fps on the workstation at 1913x935). The drawing area now reports what it
  does with every frame (`SUMMIT_FRAME_STATS`), schedules a frame one display
  period after the previous frame *started* rather than after the UI process
  acknowledged it, moves the pixels a scroll keeps on screen inside the frame
  bitmap instead of repainting them, and recycles presentation bitmaps.
- **`box-shadow` was two thirds of a frame's painting.** Upstream keeps the
  blurred shadow template in a scratch `ImageBuffer`, but only for Core
  Graphics; every other port allocated one per shadow per frame, which on Haiku
  is an app_server offscreen window each time. Enabled for Haiku.
- **OpenGL ES on the GeForce works** through a private Mesa 25.3.6 built with
  the `zink` driver over NVK (Haiku's own Mesa has EGL disabled and the
  installed Zink add-on only serves `BGLView`). GPU compositing is selected
  automatically and renders correctly — and is **four times slower** than
  software, because a scroll invalidates the whole non-composited layer and
  every tile is repainted through the same CPU path. Software stays the default.
- **Speedometer 3.1: Summit 3.23 ± 0.083, Firefox 155 8.34 ± 0.37** on the same
  machine and the same local copy. Painting is inside the web process, on one
  thread; measuring what it consists of showed that drawing glyphs costs
  0.07 ms a frame, that a one-slot shadow template cache was thrashing between
  an element's two shadows, and that `BView::PopState()` flushed the app_server
  link 558 times a frame. Fixing the last two took the scroll page from 42.1 to
  44.9 fps and Speedometer from 3.05 to 3.23.
- **Reading state back from app_server was 2.09 ms a frame.** `BView::HighColor()`
  and friends are not field reads, they are synchronous round trips
  (`FlushWithReply`), and a frame made 75 of them at ~28 µs. Six call sites were
  restoring a value the port already knew and now take it from
  `GraphicsContext`'s own state; rendering is pixel-identical on the scroll page
  and on Wikipedia. Scrolling 44.9 -> **46.3 fps**, Speedometer 3.23 ± 0.083 ->
  3.28 ± 0.058 (i.e. unchanged). The remaining round trips are 65 `Transform()`
  and 10 `GetClippingRegion()`.
- **Making `PushState()` lazy was tried and reverted.** It rendered Wikipedia
  nearly blank; `SUMMIT_LAZY_STATE=0` was pixel-identical to the prior bundle,
  which isolated it. `beginTransparencyLayer()` appears to need its push to have
  reached the view before `BeginLayer()`. It was worth 0.07 ms, so it was
  removed rather than fixed.
- **Micro-optimisation is done.** Glyphs (0.07 ms), state changes (0.07 ms) and
  read-backs (2.1 ms, 3% of a frame) were each measured; ~10 ms of the 14.6 ms
  of per-frame drawing is diffuse work inside 32 fill calls with no single
  culprit. Painting is still ~95 ms per megapixel, on one thread, with 31 cores
  idle — the next work is the drawing model itself.
- **NVDEC is reached by Summit** (0.11–0.25 cores decoding 1080p H.264), but the
  plugin crashes in its own error path: `NVDecPlugin.cpp` keeps a pointer to a
  stack-local `char reason[256]` (`nvdec_h264.c:237`). YouTube additionally
  needs Media Source Extensions, which are not built.

## September 21, 2026: Skia draws the page

`USE_SKIA=ON` builds the whole Haiku port and renders correctly -- the scroll
benchmark and a live Wikipedia article both come out right, with text, images,
SVG icons, gradients, shadows and form controls. Details in
[performance](performance.md).

- **Why Skia.** Three micro-optimisation rounds had run out: glyphs 0.07 ms a
  frame, state changes 0.07 ms, app_server read-backs 3%. The engine tree
  already vendors Skia and `SkiaPaintingEngine`, upstream's parallel tile
  painter, so the remaining structural problem -- one thread painting at 95 ms
  per megapixel while 31 cores idle -- has an answer that is adoption rather
  than invention.
- **Both renderers stay.** `USE(HAIKU_GRAPHICS)` now names the app_server
  backend; the port had used `PLATFORM(HAIKU)` and `USE(HAIKU)` for it, which
  stopped being true once Skia became selectable.
- **Adwaita under Skia.** The Haiku themes paint with `BControlLook`, which
  needs a `BView`. Under Skia the port uses the same Adwaita theme as GTK, WPE,
  Windows and PlayStation. The native look stays in the app_server build.
- **Scroll copying is off under Skia.** Moving the pixels a scroll keeps on
  screen assumes the previous frame, shifted by whole device rows, is still what
  the page would paint. Skia positions glyphs at subpixel offsets, so it is not,
  and the error collects into bands of doubled text. The same page is clean
  under app_server. Two candidate fixes were tried, neither explained it, and
  both were removed rather than kept as folklore. Tiles make the optimisation
  unnecessary, which is the next step.
- **Haiku's fontconfig ships no `conf.d`**, so `serif`, `sans-serif` and
  `monospace` never resolved. Skia's last-resort fallback then used an empty
  typeface as a hash key and the web process died on every Speedometer run.
  Linking the standard configuration in fixes it; see
  [workstation](workstation.md).
- **Numbers so far:** scrolling 7.7 fps against 44.4 for app_server, Speedometer
  2.59 ± 0.551 against 3.28 ± 0.058. Skia is behind on both, and that is
  expected of a first build that paints on one thread with tiles, the worker
  pool and GL compositing all still off.

## September 19, 2026 continuation (in progress)

Work resumed with three owner priorities: GPU-accelerated rendering where the
hardware supports it, a working 1Password extension, and Speedometer 3.1
benchmarking with speedups. This section is a summary; details are in the linked
notes. Nothing here has been through the exhaustive per-bundle evidence process
used below, and items marked *unverified* have not run yet.

- **1Password (Firefox MV2 8.12.32.33).** The unmodified package, in the
  owner's existing profile and without reinstalling, now completes background
  start-up, and its toolbar button opens the welcome/permission-setup page,
  which renders as designed ([screenshot](screenshots/1password-welcome.png)).
  Missing pieces were found with a new API gap tracer and implemented:
  `notifications`, `idle`, `management`, `privacy.services`, `webRequest`
  listener options, clean native-messaging failure, WebAssembly MIME types,
  Firefox menu contexts, extra `runtime` and `webNavigation` members, a
  restore-time permission upgrade, and the `tabs`/`windows`/`scripting`
  mutation APIs with an engine-to-app command protocol. Sign-in, unlock and
  autofill need a real account and are *unverified*. See
  [1Password](webextensions-1password.md) and
  [tab commands](webextensions-tab-commands.md).
- **Owner-reported issues (`bundle-nr1le5v_`).** Opening reddit.com crashed
  `media_addon_server`: Haiku's audio mixer dereferences a null output when a
  sound player connects on a machine without sound hardware (the VM). Summit now
  creates a sound player only when an audio output exists and the decoded format
  is fully specified; reddit loads and plays its video muted
  ([platform issue 6](kunanyios-platform-issues.md)). 1Password's "Sign in"
  button navigates its extension tab to `start.1password.com`, which extension
  views refused. Such http(s) navigations are now handed to the app, which
  replaces the tab with an ordinary web tab in the same position; the same path
  serves `tabs.update(url)` between extension and web pages. The user agent
  now reports Safari 26, which removes 1Password's "update your browser"
  banner. uBlock (97) and Dark Reader (79) still pass on this bundle.
- **Extensions silently absent after start-up.** Loading an extension copies its
  package to a staging directory, and that copy went to the system temporary
  directory. In the VM `/tmp` is on the boot volume, which was 99.5% full, so
  staging the two installed extensions (about 110 MB) either failed or crawled,
  and the browser came up with no extension icons and nothing logged. Staged
  copies now go to `<profile>/ExtensionStaging/<team>`, on the profile's own
  volume; a run that is killed leaves its directory behind, and the next start
  removes the directories of teams that are gone
  (`WebExtensionStagingDirectoryHaiku.cpp`, set from the profile in
  `WebViewContextHaiku::create`). Runs that ended abruptly were a red herring:
  they left staged copies behind, which made the next start more likely to run
  out of room. The full boot volume also wedged BFS (unkillable `rm` threads
  blocked every write), which needed a VM reset.
- **Extension origin churn (data loss).** Each load gave an extension a fresh
  `webkit-extension://<uuid>` origin and renamed its website data to the new
  origin at start-up. A run that ended before that rename stranded the data
  under the previous origin, which is how 1Password lost its account and asked
  to sign in again. The persisted `LastSeenBaseURL` is now restored at load
  (`WebExtensionController::restoreNativeBaseURL`), so the origin is stable and
  nothing has to be renamed. Verified: the origin and the IndexedDB directory
  survive consecutive launches and a `kill -9`, with `storage.local` intact.
- **Passkeys (Web Authentication).** `ENABLE_WEB_AUTHN` was off, so
  `PublicKeyCredential` and `navigator.credentials` did not exist and 1Password
  reported "WebAuthn isn't supported". The feature is now built for the Haiku
  port with an authenticator coordinator that declines natively, because the
  credential comes from the password manager, which can only extend an API the
  page exposes. On `bundle-dxx4tx8e` the page sees the complete surface
  ([screenshot](screenshots/webauthn-surface.png)) and 1Password offers a
  passkey on github.com
  ([screenshot](screenshots/1password-passkey-prompt.png)); completing the
  sign-in needs the owner's unlocked vault and is *unverified*. Enabling the
  feature needed eight upstream portability fixes (Cocoa keychain types in
  shared headers, Xcode-only include forms, DER helpers filed under
  `crypto/cocoa`, Touch ID strings, sources listed only for Apple's CMake
  build) and leaves out Apple's authenticator stack. uBlock (90 + 7) and Dark
  Reader (79) pass on this build. See [passkeys](webauthn-passkeys.md).
- **1Password on login pages.** With the extensions loaded, the published
  package fills its inline field icon on an ordinary login form and offers to
  unlock ([screenshot](screenshots/1password-inline-icon.png)). The earlier
  report of nothing appearing was the staging failure above, together with the
  Deskbar entry still launching the September 13 legacy build, which has no
  extension support and its own empty profile. That package is retired; the
  desktop entry now starts the current bundle with the profile that holds the
  installed extensions.
- **Published extensions.** On `bundle-ivx_6ahj` the unmodified uBlock Origin
  1.74.0 CRX passes its complete native suite, 90 install-mode + 7 restart
  checks: signed install, EasyList filtering where the matched request never
  reaches the server, the popup power switch off/on, disable/re-enable and
  filtering after restart. On theguardian.com it blocks 29 requests and its
  popup renders ([screenshot](screenshots/ublock-popup.png)). The published
  Dark Reader passes its 79 checks. This needed blocking `webRequest`
  ([design](webextensions-webrequest-blocking.md)), `declarativeNetRequest` and
  `downloads` ([notes](webextensions-dnr-downloads.md)), API functions callable
  without their namespace object (`const f = browser.runtime.getURL; f("")`),
  a real `sender.origin` for extension pages, `requestIdleCallback`, tolerant
  menu URL patterns, presenting frames painted for an earlier geometry,
  damped popup size oscillation, and not reporting pre-existing tabs as created.
  The Dark Reader harness now returns to the original tab after installation,
  because Dark Reader opens its help page in a new tab (as in Chrome).
- **Rendering fixes.** Text and images inside `opacity < 1` elements were not
  painted (app_server layer bounding-box bug, worked around); fills ignored the
  context's global alpha (SVG `fill-opacity`, canvas `globalAlpha`); SVG/canvas
  pattern fills were unimplemented and drew black. See
  [platform issues](kunanyios-platform-issues.md).
- **Performance.** Speedometer 3.1, uncontended: 0.575 → **3.53 ± 0.21**
  (local copy, 6.1x); the official browserbench.org page, which previously
  crashed its page process through the Screen Wake Lock API, now completes at
  **3.67 ± 0.099**. Changes: timers no longer go through `BMessageRunner` (2000
  chained zero-delay timers 484 ms → 2.5 ms); the drawing area repaints only
  invalidated regions into a reused bitmap and keeps its app_server drawing
  context; WebKit's bundled mimalloc replaces Haiku's `malloc` (+15%); the disk
  cache stores bodies on BFS (no hard links). See [performance](performance.md).
- **GPU compositing.** TextureMapper (OpenGL ES via EGL) compositing with
  read-back into the frame bitmap and runtime fallback is built in the
  `ModernGL` configuration (with mimalloc) and **runs**: with the private Mesa
  25.3.6 in the VM (llvmpipe, forced) it renders the compositing fixture like the
  software path and scores the same on Speedometer. It is selected automatically
  only on a hardware GL renderer; the stock Haiku Mesa cannot initialize EGL, so
  those systems keep software painting. Real-GPU validation is pending. See
  [GPU compositing](gpu-compositing.md), [validation](gpu-validation.md) and
  [VM EGL stack](vm-egl-stack.md).
- **VM.** Haiku's `profile` tool kernel-panics this guest (`profiling_event`);
  do not use it. `/SummitExtensions` needs a manual `mount` after a reset.

# Development verification — September 16, 2026

The original full-browser objective is **not complete**. Summit now builds and
runs with the pinned latest upstream WebKit in Haiku. Extensions and the modern
WebKit process architecture remain under development.

## Native preview

Built on Haiku R1/beta6 hrev59866+79 x86_64, GCC 13.3, in QEMU/KVM.
The current browser links its private build of upstream
`00991b6cdc59937da6076c69a22ae6a9460a4445`, reporting WebKit **626.1.6** and
the full commit in its Haiku port identity. Native `listimage` output verifies
both loaded library paths. The portable suite also runs on Linux with GCC 13.3.
Both native Make and CMake builds compile; CMake's application resources and
portable test execution were verified in the guest as well.

- **38 portable checks pass on Linux and Haiku**: address/search resolution,
  UTF-8 escaping, rejection of control characters and executable address
  schemes, profile round trips, replacement of saved sessions, private file
  permissions, deduplicated history, and preserving malformed profile data.
- **37 native browser checks pass against the current engine; 32 were also verified in the earlier system-engine package**: native scripting responsiveness, a real
  HTTP/DOM/JavaScript/CSS/storage/cookie fixture, independent tabs, back/forward,
  closing selected and background tabs, reopening closed tabs, and repeated
  creation and destruction of web views, and focusing an empty address field
  when opening the start page. A delayed-image fixture confirms that DOM
  readiness does not prematurely clear loading, completed pages return to the
  Reload state, and selecting a finished tab does not restart its indicator.
- Native paths and file-panel references open filenames containing spaces,
  URL delimiters (`#`, `?`, `%`) and Unicode without changing the target file.
  The native fixture loads an actual HTML file with those characters in its name.
- The HTTP fixture verifies Promise execution, DOM queries, external CSS grid,
  localStorage, document.cookie, HttpOnly exclusion from JavaScript, fetch and
  sending cookies in a real request. Results are generated by the running page
  and posted to the host; they are not static expected-page text.
- Loaded and visually inspected `https://webkit.org/` in the current engine;
  its final native state reports Ready and loading=false.
- **11 of 12 advanced platform probes pass**: localhost secure context, canvas
  pixels and PNG round trips, PNG decoding, SVG rendering, font metrics, shadow
  DOM, ES modules, Worker/ArrayBuffer transfer, IndexedDB and SHA-256 Web Crypto.
  **BroadcastChannel is unavailable.** Upstream disables it in WebKitLegacy
  because its registry needs PageGroup partitioning. The fixture remains failing
  until the feature and isolation behavior are implemented.
- Downloaded `/download` from the fixture server and checked the saved
  `~/Downloads/summit-test.txt` contents (`Summit download fixture` plus newline).
  The automated suite now uses a unique download filename, verifies the exact
  contents and completion notification, and resumes browsing afterward.
- Quit and relaunched the earlier system-engine package with its showcase profile. Both tabs,
  their selected index and the WebKit bookmark survived the restart.
- Built and installed the earlier system-engine HPKG. The package creates a Deskbar launcher;
  profile directories are mode 0700 and the cookie database is mode 0600.
- Launched with a saved session containing only a rejected executable URL.
  The app showed the rejection and opened a usable start tab without crashing.
- An unavailable profile directory now shows a native startup error and exits
  with status 1 before initializing WebKit. An unavailable Downloads directory
  cancels the download and shows an error while browsing remains responsive.
  Both failures were exercised in the guest, with the original Downloads
  directory restored afterward; all 37 normal browser checks then passed.
  Logs: `.vm/folder-startup-error-fixed.log`, `.vm/download-folder-error.log`,
  `.vm/ui-folder-errors-smoke.log`. This app update is built in
  `build-ui-current` and is not yet included in the preserved distribution.

The current-engine distribution is `artifacts/current-browser/`, with private
libraries, resources, licenses, patched engine source and a manifest recording
source inputs and original/bundled SHA-256 hashes. Native relocation and actual
library resolution were checked. **The existing `dev1` HPKG still contains the
earlier system-engine preview**; packaging and restart checks need repeating
for the current distribution.

After a clean guest shutdown and VM resource upgrade, the current-engine
bundle relaunched with its saved WebKit page and bookmark intact. The page
returned to Ready with the current engine identity. Evidence:
`.vm/current-engine-before-restart.log`, `.vm/current-engine-after-restart.log`.

Actual QEMU capture: [current WebKit over HTTPS](screenshots/current-webkit.png).
The earlier [start page](screenshots/start.png) and
[WebKit capture](screenshots/webkit.png) show the system-engine preview.

Current evidence in ignored `.vm/`: `current-browser-build.log`,
`current-browser-copy.log`, `current-browser-build.json`,
`current-browser-loaded-images.log`, `current-browser-smoke.log`,
`current-platform-smoke.log`, `current-https-smoke.log`,
`current-browser-state.log` and `relocated-jsc-smoke.log`.
The current guest log is `/boot/home/summit/current-browser.log`.

Raw logs and guest state are retained in ignored `.vm/`: `browser-build.log`,
`browser-smoke.log`, `loading-smoke.log`, `package-smoke.log`,
`session-restart.log`, `rejected-profile.log`, `local-file-build.log`,
`local-file-smoke.log`, `local-package-smoke.log`, `http.log`,
`engine-reports.jsonl`, `full-sync.log`, `build-dependencies.log`, and
`llvm-install.log`. The app's guest log is
`/boot/home/summit/browser.log`.

## Current upstream engine

`engine/sources.lock.json` pins upstream commit
`00991b6cdc59937da6076c69a22ae6a9460a4445` and the Haiku platform patch.
The patch was checked for application against a fresh Git index of that exact
upstream commit. Native CMake configuration completed with the Haiku port.
The first JavaScriptCore build failed because the generic ICU development
capability selected version 66. Installing `icu74_devel`, requiring ICU 74 in
CMake and rebuilding all dependencies corrected that selection. The current
**JavaScriptCore library and `jsc` shell now build successfully**. Its 16 runtime
checks pass, including BigInt, modern arrays/objects, private fields, buffers,
Unicode regular expressions, ICU list/number/date/word processing, Promise
microtasks, hot JavaScript and WebAssembly. Fifteen JavaScript checks also pass
with JIT disabled. The full 16-check run passes with `DISABLE_ASLR` unset.
These are port smoke checks, not an upstream conformance suite.

ICU **78.3** was subsequently built into a private prefix from its verified
upstream source archive. Its `testdata`, `intltest`, `iotest`, `cintltst` suites
and configuration self-check all pass on Haiku. The initial data-packaging and
installation attempts failed because their relative runtime library paths did
not resolve; absolute `LIBRARY_PATH` entries corrected that without source
changes. Logs: `.vm/icu78-build.log`, `.vm/icu78-build-fixed.log`,
`.vm/icu78-install-check.log`. `engine/icu.lock.json` and
`tools/build-icu-in-vm.sh` record the source and reproducible private build.
The preserved working browser still uses ICU 74. JavaScriptCore rebuilt against
ICU 78.3 in the separate modern-engine directory and passes 16 JavaScript/JIT/
WebAssembly checks plus 15 interpreter checks with normal address randomization.
Its frozen copy includes private ICU libraries; loaded-image paths and hashes
were verified. Logs: `.vm/jsc-icu78-freeze.log` (smoke results),
`.vm/jsc-icu78-loaded-images.log` (corrected image probe).

The upstream Test262 runner also passes **232 runs from 116 test files**, with
zero skips and zero failures, for `Intl.ListFormat`, `Array.prototype.toSorted`
and `Object.groupBy`. Both strict and default modes were exercised. Expectations
and skip configuration were ignored so they could not hide failures. This is a
selected part of Test262, not the complete suite. The log is `.vm/test262.log`;
native reports are under `/boot/home/summit-webkit/test262-results/`.

The first full Test262 run was interrupted after its progress counter reached
4,950 of 53,578 files. It encountered native crash dialogs and repeated
`Atomics.waitAsync` timeouts; it produced no full-suite result report.
The harness now creates a separate working directory for every invocation,
so an interrupted run cannot reuse reports from an earlier successful run.

The full run completed in 4,718 seconds against an isolated shell containing
the event-loop and explicit-exit fixes below. **101,755 executions passed and
234 failed, from 101,989 executions; 486 of 53,578 files were skipped.**
There were 232 assertion/exception failures and two timeouts in the staging
deep-WeakMap regression. No process crash exit codes were reported. Both shell
and library hashes matched before and after the run, and actual loaded-library
resolution was verified. The run keeps upstream's skip configuration and
ignores failure expectations; failures have not been suppressed.

Of the 234 failures, 138 have the same path, mode and exit code in pinned
upstream Linux expectations (120 match its default expectations). The other
96 comprise 58 Intl executions, 32 Unicode 16/17 identifier executions, four
Unicode case-mapping executions and two deep-WeakMap timeouts. Matching upstream
expectations does not make a failing test pass. The
[machine-readable report](test262-2026-09-13.json) records every
failure, the comparison and source/report hashes. Raw reports remain in
`.vm/test262-full-results/`; the log is `.vm/test262-full-fixed.log`.

The ICU 78 comparison reran all Intl tests, language identifiers and the staging
String group: **7,293 passed, 22 failed, 7,315 executions, 14 skipped files**.
It fixes all **94** previous locale/Unicode failures in that scope, with no
passing-to-failing transitions. The remaining 22 executions match pinned Linux
expectations: four locale hour-cycle cases and 18 Chinese-calendar cases.
This was the initial scoped comparison; the subsequent full run is recorded below.
The shell, JSC library and all three private ICU libraries retained their hashes.
Report: [ICU 78 comparison](test262-icu78-intl-2026-09-13.json); raw evidence:
`.vm/test262-icu78-intl.log`, `.vm/test262-intl-results/`,
`.vm/icu78-scope-comparison.json`.

The full ICU 78 run completed in **6,291.58 seconds**, with **101,849 passes,
140 failures and 486 skipped files** across the same 101,989 executions and
53,578 files. All **94** locale/Unicode failures are fixed, and the complete
comparison has **no passing-to-failing transitions** or changed execution/skip
keys. The remaining 138 assertion/exception failures match the pinned Linux
expectations by path, mode and exit code. The other two are the same deep-WeakMap
staging test timing out in strict and default modes; no crash exit codes appear.
The shell, JSC library and all private ICU libraries retained their hashes.
Report: [full ICU 78 conformance](test262-icu78-full-2026-09-13.json). Raw evidence:
`.vm/test262-icu78-full.log`, `.vm/test262-icu78-full-results/` and
`.vm/icu78-full-comparison.json`.

Logs are `.vm/jsc-icu66-failure.log`, `.vm/jsc-build.log`,
`.vm/jsc-shell-build.log`, `.vm/jsc-smoke.log`, `.vm/jsc-default-aslr.log` and
`.vm/jsc-loaded-images.log`. The last record checks the loaded library path;
the shell resolves the newly built library under `WebKitBuild/Release/lib`.
Two runs also show different load addresses with `DISABLE_ASLR` unset.

Focused native cookie tests exposed and now cover an overly broad domain
match and cookie names incorrectly parsed as attributes. **18 checks pass in
both a console process and a native application loop**. The tests link the
actual current WebCore cookie object and newly built JavaScriptCore library.
The original three regression cases failed before the parser changes.

That harness also exposed a double free during process shutdown: Haiku ran
WebKit's thread-local `BHandler` destructor after libbe's global token table
was destroyed. The Haiku run loop now releases its native handler before
static teardown. Console exit, native application exit and main-thread exit
before process exit all complete successfully. JavaScriptCore's 16 JIT and
15 interpreter checks still pass after this change. Logs are
`.vm/engine-cookie-tests.log`, `.vm/cookie-probe-baseline.log` and
`.vm/jsc-runloop-smoke.log`; the original crash report is
`.vm/cookie-probe-crash.report`.

Further Test262 failures exposed missing timer delivery and a worker shutdown
race. The revised Haiku run loop accepts timers before entering the loop,
handles zero-delay scheduling, rejects stale notifications after cancellation
or rearming, and returns from standalone loops without terminating their thread.
**12 native checks pass**, including native application dispatch and worker
thread completion. The original timer implementation fails the first check
and hangs. Ten explicit process-exit probes with a finishing worker also pass;
Haiku's explicit exit path flushes output and terminates threads together,
avoiding libbe static teardown while worker handlers are still being destroyed.

With both fixes linked into an isolated shell, **760 upstream Atomics test
executions pass**, with nine upstream skips and zero failures. The first run
with only the timer fix had one timeout and captured a worker double-free
report. Both JIT and interpreter smoke checks still pass. These source changes
are included in the resumed main engine build. Logs are `.vm/runloop-isolated.log`,
`.vm/runloop-atomics.log`, `.vm/runloop-atomics-fixed.log` and
`.vm/runloop-atomics-crash.report`.

WebProcess startup initializes WebKit before calling `BApplication::Run`.
At that point Haiku has not assigned the application's looper thread, so the
old selection code incorrectly created a separate, undriven loop. The revised
selection recognizes an initialized application still locked by its startup
thread, while preserving worker isolation. A native baseline fails four of
**15 checks**; the fixed production code passes all 15, the existing 12 checks
and ten worker-exit runs. Frozen libraries and native engine inputs remain
unchanged. Run `python3 tools/test-engine-runloop-prerun.py`; evidence:
`.vm/runloop-prerun-inputs.y2cx33hl/result.json`.

The new native download filename helper passes **16 checks** for path
components, control characters, empty names, Unicode and filesystem length
limits. It also preserves Unicode extensions when avoiding filename collisions.
The helper and an explicit completion-status field are included in the current
engine; actual downloads, exact contents and completion status pass the browser suite.
The installed system engine still has its original filename handling.

The modern process port now has a native spawn helper and executable resolver.
**18 native process/IPC checks pass**: creating a child with exact arguments,
preserving descriptor flags, resetting its signal mask, sequenced packets,
transferring real descriptors with close-on-exec, sharing writable mappings,
reaping the child and reporting launch errors. **12 path checks pass** against
the production resolver, including executable directories with spaces and
Unicode, overrides, installation fallback and missing helpers. Run these with
`tools/test-engine-process-in-vm.sh`; logs are `.vm/native-ipc-tests.log` and
`.vm/native-process-paths.log`. The modern WebKit process launcher and shared
process entry code now compile. This does not establish WebKit process isolation.

The Haiku IPC socket monitor passes **25 native checks** against production
code: ordered sequenced packets, serial-queue callbacks, idle cancellation,
peer closure, destruction inside a callback, suppression of queued callbacks,
descriptor reuse, close-on-exec, write backpressure and lifecycle leak checks. A dedicated
polling thread only detects readiness; protocol processing stays on the
connection queue. Cancellation wakes and joins the polling thread, including
when the peer is not reading a full socket. Integration is wired into the
modern connection backend, which now compiles; its linked runtime integration
remains pending.
The test explicitly joins its detached connection-queue thread before process
static teardown; without that wait, an earlier passing run produced a late
native debugger dialog. Ten complete process runs now pass with no new crash
reports. Run `tools/test-engine-socket-monitor-in-vm.sh`; logs:
`.vm/native-socket-monitor-fixed.log`, `.vm/native-socket-monitor-repeat.log`.
The expanded write-readiness checks are in `.vm/native-socket-monitor-write-full.log`.

The native IPC semaphore and event implementations now pass **54 checks**.
The previous Unix source compiled on Haiku but supplied no working semaphore
operations or event factory. The replacement keeps a 64-bit permit count in
shared memory and uses a native process-shared semaphore for wake notifications.
Event endpoints use a separate Unix socket only to observe the last signal
owner closing or dying, with a 100 ms polling interval.
Tests compile the real encoder, decoder and generated serializers. They cover
200,000 queued permits, concurrent producers/waiters, move ownership, finite
and interrupted deadlines, malformed imports, corrupted bookkeeping, actual
descriptor transfer, 20,000 cross-process signals and interruption after killing
the remote owner. Descriptor counts return from four to four. The test warms
WTF's process-lifetime random-device descriptor before measuring cleanup.
These checks validate the native transport; writable shared memory is not an
immutable boundary against a hostile peer. Full WebKit connection integration
still requires the linked engine. Run `python3 tools/test-engine-ipc-signals.py`;
evidence: `.vm/ipc-signal-inputs.SVhp7jdS/result.json`.

The new Haiku packet transport passes **43 native checks** using real sequenced
sockets and the production shared-memory implementation. It rejects malformed
flags, counts, truncated bodies, extra/missing descriptors and invalid shared
memory while closing received descriptors on every failure path. Inline and
out-of-line bodies, attachment order, atomic close-on-exec, peer closure and
repeated malformed-message cleanup are covered. Haiku limits control data to
1024 bytes and each kernel ancillary record to 32 descriptor pointers; grouped
records successfully transfer the resulting **224-descriptor** maximum.
Run `tools/test-engine-message-packets-in-vm.sh`; final log:
`.vm/native-message-packet-tests-final.log`.

The native storage-path adapter passes **18 checks** for separate cache/settings
roots, application namespaces, explicit profiles, Unicode paths, private new
directories, creation failures and rejected relative/traversal paths. Empty
explicit profiles cannot fall back to the working directory. Run
`tools/test-engine-storage-paths-in-vm.sh`; log:
`.vm/native-storage-path-tests-private.log`. These are path-adapter checks;
complete website-data separation still requires the modern browser integration.

The shared temporary-directory helper now passes **11 native checks**. A
baseline using the original production source fails three checks: a parent
without a trailing slash produces sibling directories rather than children.
The fix joins path components and gives `mkdtemp` a NUL-terminated mutable
buffer. Checks cover Unicode and spaces, twenty unique directories, private
permissions under a permissive umask, invalid parents and preservation of
existing files. Both versions compile the complete production `FileSystem.cpp`
and link the frozen ICU 78 JavaScriptCore library, whose hashes remain unchanged.
Run `python3 tools/test-engine-temporary-directories.py`; evidence:
`.vm/temporary-directory-inputs.0jmr9jsu/result.json`.

The native module loader passes **10 checks** for missing images and symbols,
Unicode paths, repeated loads, explicit unloading and destructor cleanup.
Failed native image IDs no longer report success, and repeated loads do not
leak loader references. Run `tools/test-engine-modules-in-vm.sh`; log:
`.vm/native-module-tests.log`.

Modern WebKit configuration succeeds with private ICU 78, Unix-domain sockets,
the current Curl network-process sources and native WebProcess application
resources. The new connection backend validates complete packets and retains
unsent messages while asynchronously waiting for capacity, with a short yield
between retries to handle readiness for less than a complete packet. The actual
Connection, ProcessLauncher and AuxiliaryProcessMain objects compile. WebProcess
startup exposed an allocation-helper mismatch with native BApplication; the
corrected source is included in the next build. Log:
`.vm/modern-connection-build.log`. These component checks do not yet establish
a working WebKit process connection or browser process isolation.

The first complete modern compilation reached the final `libWebKit` link with
no compiler errors, then failed on **127 distinct unresolved symbols**. Of
these, **115** are present in the compiled WTF archive but absent from shared
JavaScriptCore: the static archive only contributed objects JSC itself used.
Haiku now follows the other ports' object-library configuration, placing all
WTF/bmalloc objects in JSC once. The legacy link workaround was adjusted to
avoid adding a second WTF copy. The corrected Modern JavaScriptCore has now
relinked and exports **115/115** missing WTF definitions plus the checked global
ownership sentinels. Its actual shell passes **31 smoke assertions** with ASLR
enabled; shell, library, private ICU and staged test hashes remain unchanged.
The complete `libWebKit`, `WebProcess` and `NetworkProcess` build subsequently
finished **277/277 steps with no compiler or linker errors**. The final
`libWebKit` ownership audit found **zero duplicate WTF definitions** and kept
the four checked global sentinels owned by JSC. Evidence:
`.vm/modern-process-link-build.log` and `.vm/modern-wtf-postlink-results.json`.
Evidence: `.vm/modern-font-view-build.log`, `.vm/modern-wtf-link-audit.json`,
`.vm/jsc-modern-validation.BnVSDlqx/results/result.json`.
The native drawing areas also needed the upstream message-sender template
definitions, and the bitmap adapter needed its platform memory-accounting
hook. All three corrected objects compile, and their targeted unresolved
references are gone: `.vm/modern-link-adapters.json`.
The five missing WebProcess platform hooks now compile and define the expected
symbols. Native initialization validates the application startup thread and
applies the common process parameters; website-data setup preserves the session
already established by common code. Cocoa-only asset and font capabilities are
asserted absent in this port. Evidence:
`.vm/webprocess-platform-inputs.gt2cayvD/result.json`.

The original area-based shared-memory backend loses its allocation when the
owner is destroyed, even if a handle remains. A native baseline probe confirms
that failure. Its mapping function also leaves the data pointer uninitialized.
The replacement uses owned file descriptors and native file-backed mappings;
**39 checks pass** against the production implementation. They cover handle
lifetime, independent mappings, read-only access, copy-on-write and copied data,
borrowed mappings, real child-process descriptor transfer, and native bitmap
import with shared pixel bytes. The child cannot write through a read-only
handle or bypass it by cloning the owner's ordinary memory area. Bitmap sharing
explicitly enables area cloning for app_server. An imported writable mapping
cannot currently export a separate read-only handle; it reports failure instead
of granting writable access. These changes are integrated into the working engine.
Logs: `.vm/native-memory-tests.log` and `.vm/native-memory-baseline.log`.

Shared bitmap drawing and native icons now pass **47 native checks** against the production
WebCore adapters. Drawing contexts retain their native views and mappings,
flush app_server writes on completion, and reject read-only backing memory.
Image copies remain stable while shared references observe later writes;
both images and contexts survive release of the original bitmap. Pixel checks
cover destination positioning, source rectangles, device scaling, transparent
copy compositing and restoration of per-image graphics state. The previous
copy operator incorrectly used source-over blending. Read-only images use
private snapshots without making their mappings writable or cloneable.
Frame checks reject invalid scales, oversized geometry, metadata mismatches and
oversized backing objects before mapping. Run
`tools/test-engine-bitmaps-in-vm.sh`; logs:
`.vm/native-bitmap-frame-tests-fixed.log` and
`.vm/native-bitmap-presenter-tests.log` and `.vm/native-icon-bitmap-tests.log`.
Native icons retain decoded images, paint scaled pixels inside the requested
rectangle and load real system file/folder icons. Tests also check ownership,
inclusive native image dimensions and missing-resource handling.
The native presentation helper copies
shared pixels into immutable window snapshots. Concurrent reads and frame
replacement preserve matching pixels and frame identifiers; outstanding
snapshots survive view closure, which rejects later publication. This helper
is integrated into the native modern view bridge.

Both modern Haiku drawing areas now compile with their generated IPC handlers.
They send complete software-rendered frames, associate them with viewport
generations and wait for presentation acknowledgment before sending another
frame. The native `BWebKitView` and its current `PageClient` controller also
compile. The bridge queues WebKit operations on the application thread and
gives window drawing immutable native snapshots; close, hidden-view and failed
delivery paths complete outstanding frame acknowledgments. A common preference
override was corrected so Haiku can retain software compositing. The complete
engine now links, and the preview verifies process-to-view presentation using
actual screen pixels from the loaded fixture.
Full-build warnings also exposed missing inline definitions in the native
drawing area and page editing adapter. Including the upstream inline headers
fixes both warnings; each corrected production object compiles natively.
Evidence: `.vm/drawing-area-inline-compile.json` and
`.vm/web-page-inline-compile.json`.

`tests/ModernBrowser.cpp` now passes its real runtime smoke test: the HTTP
fixture completes its DOM/JavaScript/storage/cookie checks, and its CSS
background appears in the captured native window. The process exits normally
with ASLR enabled. Evidence: `.vm/modern-preview-first-smoke.json` and `.log`;
frozen bundle `/boot/home/summit/build-modern-preview/bundle-u3r9_4z4`. The preview bundle helper
requires both completed process targets, a matching source patch and unchanged
input hashes, then preserves private WebKit/JSC/ICU libraries with relative
runtime paths. Native compilation, missing-engine refusal and build-lock refusal
were checked; `.vm/modern-preview-compile.json` records the compilation. Run
`tools/build-modern-browser-in-vm.sh --bundle` to freeze the completed preview.
The same tool accepts `--browser` to build the complete Summit interface and
freeze it with both helper processes, its start page and the same private
libraries. Browser and preview bundles use separate output directories.
`--compile-only --browser` passed for all five browser source files; it uses
isolated staged inputs and can run during an engine build. Bundling still
requires the engine and ICU build locks and completed, unchanged inputs.
The latest browser compile evidence is `.vm/modern-browser-compile.json`.

Native font reconstruction now passes **43 checks** against the production
WebCore sources. Installed fonts preserve native style, size, orientation and
text metrics across IPC; malformed metadata is rejected. Downloaded faces keep
their original bytes and CSS metrics overrides through reconstruction, and
remain usable until the final owning reference is released. Haiku rejects
duplicate private family/style names, so the port now uses WebKit's existing
OpenType name rewriter to assign each native load a unique identity. Tests cover
independent repeated loads, TrueType, CFF, unaligned trailing data and the
existing first-face behavior for font collections. WOFF2 is now accepted by CSS
format selection when its decoder is enabled; a real encoded TrueType fixture
passes the production WOFF2 decoder and loads through the native font manager.
Selecting a collection face
by its URL fragment remains unsupported. Run `tools/test-engine-fonts-in-vm.sh`;
logs: `.vm/native-font-tests-collections.log` and `.vm/native-font-tests-woff2.log`.
These checks include real native
font loading and metrics; they do not yet establish font IPC in a running
modern browser.

The existing Summit interface now has an optional `SUMMIT_MODERN_WEBKIT` build.
Both backend configurations and all six UI object compilations passed natively;
`.vm/modern-ui-compile.json` preserves inputs and results. The modern path uses
native message identities for tab state and queues view teardown before stopping
the application. Find, forward/backward match selection, page/text zoom, native
clipboard commands and undo/redo now reach the real page proxy. Stale find
results are suppressed after navigation or later searches. Both bridge command
objects and both UI modes compile; rendered find, zoom and DOM editing still
need runtime checks. The About dialog and browser probe now call exported
library functions for engine, port and source versions. The actual native
version implementation reports `626.1.6`, port `1.10.0` and the pinned upstream
revision in an isolated linked probe; `.vm/version-api-compile-runtime.log`
records that result. The full browser now links and runs with the modern
engine. Its loaded-image report verifies the private WebKit/JSC/ICU 78 paths;
native OS libraries also load their versioned system ICU 74 dependencies.

Native startup now uses `BApplication`'s error-return overload. The default
constructor exits with status zero on initialization failure, bypassing a later
`InitCheck`. A deterministic invalid-signature regression confirms the old
behavior and the corrected nonzero exit: **five native checks pass**. The
WebProcess entry object, preview, context harness and both browser entry modes
compile. Smoke-test failure also remains recorded after a content-process exit,
so a queued page-state update cannot turn it into a pass. Evidence:
`.vm/application-init-inputs.IbaOFcaq/result.json`,
`.vm/webprocess-main-inputs.qK5XyPGi/result.json`,
`.vm/modern-browser-compile.json`, `.vm/modern-preview-compile.json`,
`.vm/modern-context-ee907fa65fd9680186a41e17/result.json` and
`.vm/legacy-startup-compile.json`.

The browser smoke harness now accepts an explicit native team and required
backend before navigating. Four invalid-argument cases are rejected, and an
actual negative test against the running legacy team rejects the requested
modern backend before changing any page. Its explicit `--navigation-only`
mode reports downloads as skipped without counting them as passes; the default
suite retains the download checks. The revised harness compiles natively.
Evidence: `.vm/browser-smoke-targeting-results.json`.

The first modern full-browser run passed **30 of 33 navigation checks** against
`/boot/home/summit/build-modern-browser/bundle-exvpbdlz`, built with engine patch
`424867592959d1edd8aaee5b38c0bea1f63a8abe536f4a8f893e938bc5bc9d67`.
HTTP/DOM/JavaScript/storage, delayed-resource loading state, independent tabs,
back/forward, close/reopen and repeated tab lifecycles passed. Closing a newly
created Start Page immediately was cancelled. Two later local-file tab-count
checks also failed after that retained tab; direct navigation to the exact
Unicode local filename passed. Downloads were explicitly skipped because the
modern download integration is unfinished. Evidence:
`.vm/modern-browser-navigation-smoke.log`, `.vm/modern-browser-bundle.json` and
`.vm/modern-browser-first-loaded-images.log`.

Two original-bundle WebProcess crash reports identify the same shutdown fault:
Haiku's normal `exit()` runs main-thread pthread cleanup; destroying cached fonts
then recreates WebCore thread data and trips the shared-timer assertion.
`WebProcessMainHaiku` now routes its completed auxiliary-process result through
WTF's existing Haiku process-exit routine, preserving the exit status. The
corrected engine builds. The original early-close probe reproduces cancellation
on the first click and successful closure on a second attempt. Tracing found
that a delayed policy callback for the existing native request invalidated a
valid close approval. Matching the native navigation ID fixes that case: eight
consecutive immediate closes pass. A subsequent full navigation run passes
32/33 checks, including all three original tab/local-file close failures; one
forward-navigation assertion fails and remains under investigation. Evidence:
`.vm/modern-browser-first-crash.report`, `.vm/modern-browser-second-crash.report`,
`.vm/modern-process-exit-build.log` and `.vm/modern-early-close-baseline.log`.
Additional evidence: `.vm/modern-close-trace-events.log`,
`.vm/modern-early-close-fixed-probe.json` and `.vm/modern-browser-navigation-fixed.log`.
The current tested full bundle is
`/boot/home/summit/build-modern-browser/bundle-mjs1avo_`, with engine patch
`174f3a2d6f70c5e208b3fd9bf0033e5235b865cfe795c939febe60b6366a1ef4`.

The full native beforeunload scenario passes **143 checks**: real activation
and typing, Enter navigation from the address editor, multiple provisional
page approvals followed by Stay, preserved document identities, undo and saved
sessions, repeated prompting, background closes, draft text and selection
restoration, newer user selection, navigation that invalidates a pending quit,
and final approved browser exit with status zero. Fixture reports come from
actual HTTP/JavaScript and the frozen bundle remains unchanged. Evidence:
`.vm/modern-close-cce5ece7b5e849921a5e9f20/result.json` and
`.vm/modern-close-selection-ordered-runtime.log`.

The harness checks the native prompt before waiting for title updates queued
behind its synchronous DOM task, then verifies exact JavaScript counters after
resumption. It observes tab-selection completion before sending quit to the
separate application looper, and uses the browser probe's `closing` state for
cancellation completion. Production fixes prevent BTextControl's focus-loss
invocation from submitting a draft and restore text selection after native
focus acquisition, which otherwise selects all text. Both UI backends compile.

The current bundle passes all **12 context storage stages**, including shared
tabs, separate persistent/private contexts, retained persistent storage after
reopen, empty newly created private storage and sharing within that new private
context. Visible and HttpOnly cookies, network cookie headers, localStorage and
IndexedDB are checked against the live fixture. Each context has a distinct
network process; both cleanup phases wait for all owned helpers to exit, and
the private path hint stays absent. The test exits normally with status zero.
Evidence: `.vm/modern-context-89bb0f1daf35cc3182c428d3/result.json`.

Shutdown reliability remains under investigation. Two earlier context runs
using `bundle-cq94yd3n` passed eight stages and hung before reopen; the full
browser also hung after its earlier navigation run. Native stacks wait on
Haiku's global looper-list lock, and all-thread inspection showed a pending
writer handoff. Diagnostic intervention or timeout ended those failed runs.
Evidence: `.vm/modern-context-384358b51934885d2c48f5fa/result.json`,
`.vm/modern-context-7b4bfeec936a7526833e59a2/result.json`,
`.vm/modern-context-all-threads.report` and
`.vm/modern-browser-quit-hang.report`. A native-only probe completed 48,000
looper lifecycles; its first 30-second limit was too short, so that timeout is
not evidence of a reproduced deadlock. A separate probe linked to the current
frozen JavaScriptCore completed 1,600 WorkQueue lifecycles, observing every
worker's exit. Neither smaller probe reproduced the hang, and no speculative
native lock or RunLoop change has been applied. Evidence:
`.vm/native-looper-stress-long.log`, `.vm/workqueue-teardown-result.json`.

The same current bundle subsequently passes all **33 navigation checks**.
A repeated-history probe then reproduced the earlier forward failure on cycle
three: the restored document displayed `TypeError: Load failed`. The fixture
published PASS before awaiting its final reporting fetch, allowing the harness
to navigate away and abort that fetch. The fixture now awaits a successful
report before publishing its final title. With that ordering, **30 consecutive
back/forward cycles pass** (120 navigation operations), and the exact browser
team exits normally. No engine history change was needed. Evidence:
`.vm/modern-navigation-current.log`, `.vm/modern-history-probe.log`,
`.vm/modern-history-error.log`, `.vm/modern-history-fixed.json` and
`.vm/modern-history-quit.log`.

A separate crash report from a prewarm launch identifies a forked copy of
Summit failing in libroot's allocator while `posix_spawn` prepares `execve`
arguments. Haiku's spawn path uses `vfork` when file actions or signal attributes
are supplied; this path bypasses the normal fork heap reset. A launcher fix and
concurrent native regression test are pending. The original browser team
continued running. Evidence: `.vm/modern-network-alert-report.report` (report
team 411140; original browser 411012).

The launcher now uses Haiku's public `fork()` with native heap preparation
and reset, constructs all C++ arguments before forking, preserves the existing
child IPC endpoint and signal setup, and reports setup/exec errors through a
close-on-exec pipe. A native regression runs the real process/SCM_RIGHTS/shared
mapping checks while 12 threads allocate and free memory. The old launcher
hangs on its first launch and times out at 100 seconds; its surviving child's
stack waits in malloc during argument flattening. The replacement passes all
**900 checks across 50 rounds** in 1.33 seconds. Evidence:
`.vm/haiku-launch-fork-candidate/result.json` and
`.vm/haiku-launch-fork-candidate/baseline-child.report`.

The full engine rebuild succeeds with patch
`dfcb2f1b6c701f0c589920c5b4db859a480b545a35369647cb6bbfd993bd0002`.
The new native bundle `/boot/home/summit/build-modern-browser/bundle-eih3q6or`
passes the full **143-check close workflow** and exits normally. Its context
run passes eight storage stages, then repeats the separate global-looper-lock
hang before reopen. Diagnostic collection terminates that failed test. One
NetworkProcess also presents a crash alert; its report is incomplete because
the native debugger itself crashes while collecting it. The exact remaining
test helper and debugger were subsequently terminated after image-path checks.
Evidence: `.vm/modern-fork-launch-build.log`,
`.vm/modern-close-f7d3876e0c8ee0afe5fdeb64/result.json`,
`.vm/modern-context-f637ac2d077898045b583d59/result.json`,
`.vm/modern-fork-context-hang.report` and `.vm/modern-fork-network-crash.report`.

The lock hang now has a native-only signal reproduction. Eight workers each
complete 100 BLooper lifecycles without signals (800 total, 0.18 seconds).
With signal interruptions they stop progressing and fail the 15-second limit;
this also occurs with SA_RESTART and with ordinary SIGCHLD using its default
handler. The context stack shows the same rw_lock wait returning B_INTERRUPTED.
The OS wait loop retries without restoring its per-thread wait state; a writer
handoff can therefore remain unconsumed. An application-side compatibility
approach remains under investigation; no OS source has been changed. Evidence:
`.vm/native-looper-signal-probe.json` and
`.vm/native-looper-signal-variants.json`.

The application-side lock fix now passes the failing integration workflow.
Haiku's native wait state is restored while holding its queue mutex; after an
interruption the protected waiter list distinguishes a granted lock from a
pending wait. Only native acquisition routines are exported by libWebKit;
initialization/release stay in libroot and no WTF state is duplicated. The first
JavaScriptCore-only placement passed standalone tests but was ineffective in
the browser: libroot resolved first. A browser-order regression reproduces that
failure and passes with the corrected provider; the final test verifies both
actual symbol addresses belong to the frozen WebKit library. Private lock/TLS
headers are confined to the compatibility unit, which skips the precompiled
header to avoid the two libraries' MutexLocker alias collision. No OS sources
were changed. Evidence: `.vm/rwlock-binding-result.json`,
`.vm/modern-rwlock-context-hang.report` and
`.vm/native-lock-inputs.lgs637rx/result.json` (expected negative control).

Patch `67841526e8efee114f53c075446854de22ac3894d1a561d396f0212b867031fe`
rebuilds successfully and is frozen in native `bundle-ihenjatl`. It passes all
**14 native lock checks**: provider identity, recursion, 8,000 protected updates
alongside 8,000 reads, and 800 BLooper/BHandler lifecycles under process-wide
SIGCHLD. **Three consecutive 12-stage context runs** retain persistent storage,
clear private storage on reopen, share data only within their contexts, remove
all helpers during both teardowns and exit normally (0). The full browser again
passes **143 close-workflow checks** and exits normally. JavaScriptCore passes
**31 JIT/interpreter smoke checks** with ASLR enabled; all **115 WTF exports**
and ownership sentinels remain in JavaScriptCore, with zero WebKit duplicates.
These results address the reproduced launch and signal/looper shutdown faults;
they do not establish general platform or extension completeness. Evidence:

- `.vm/modern-webkit-rwlock-final-build.log`
- `.vm/native-lock-inputs.ioazvxwx/result.json`
- `.vm/modern-context-063ac14ee4e9c6614e74bd73/result.json`
- `.vm/modern-context-c1371b5e6afeee2cbc11c114/result.json`
- `.vm/modern-context-888c491a32534f49934ac103/result.json`
- `.vm/modern-close-5a33b5a066c5b585c4a8e1c0/result.json`
- `.vm/jsc-rwlock-validation.k8_m101o/results/result.json`
- `.vm/rwlock-wtf-postlink-results.json`

The corrected `bundle-ihenjatl` has also been copied to
`artifacts/modern-browser/bundle-ihenjatl` with the matching patched engine
source archive, native binaries, helpers, private libraries and rebuild inputs.
The exporter verified every payload digest and unchanged before/after source
fingerprints. Provenance SHA-256:
`fc9a91f509a269f0a8362b7a5b810841e8e306fc046ec23d94ad8347561e525d`.
Evidence: `.vm/modern-webkit-rwlock-bundle-copy.log` and the artifact's
`copy-provenance.json`. The earlier artifact below is preserved.

The tested frozen bundle has now been copied to
`artifacts/modern-browser/bundle-mjs1avo_`, with its app sources, private
libraries, helpers, exact patched native WebKit source trees, licenses and
rebuild support. Every declared payload hash and matching source fingerprint
was verified before publication to that new local directory. This is a
development artifact with the launch/shutdown limitations above, not a claim
that all helper processes are reliable. Copy evidence:
`.vm/modern-bundle-copy-result.json`, `.vm/modern-bundle-copy.log` and the
artifact's `copy-provenance.json`. The exporter also rejects traversal,
absolute paths, hard links and undeclared symbolic links in synthetic archive
checks; a declared local library alias is preserved. Git-mandated CRLF working
tree files are checked against their normalized index blobs while their exact
archived bytes retain separate SHA-256 digests.

Modern downloads now compile and run with patch
`53450f6f151527b8b1eafdc664c6867b232967589e5baaf06ff7b21d13022f33`.
The context registry owns download proxies, emits value-only native events,
chooses sanitized non-overwriting destinations, handles cancellation and
releases entries on network-process failure. Application-owned notifications
keep the listener alive while views close. The browser enables Downloads,
reports progress/results and waits for cancellation replies before shutdown.
The download quit prompt follows provisional page approvals; Keep Browsing
resets them without losing edits or address selection.

The API bundle `bundle-u1jofxlx` passes **eight real HTTP download scenarios,
180 checks**, with normal exit: explicit transfer, colliding attachment,
unsupported MIME response, progress/cancel/partial-file removal, truncated
response, deliberate interruption of its verified owned NetworkProcess,
subsequent recovery and completion after the originating view closes. The
shared filename helper passes **16 native checks**. The full-browser bundle
`bundle-lb1k0wql` passes **59 native UI checks**, including a trusted HTML
`download` link, exact saved bytes, real page and download quit prompts,
preserved edited document identity and address selection after cancellation,
fresh page approval on the next quit, cancellation drain and normal browser
exit with the partial file removed. Both bundles contain the same engine patch.
The same full-browser bundle also passes the existing **143 close checks** and
**12 context-storage stages**. The legacy app still builds and its **38 native
core checks** pass.

The first combined HTML-link fixture clicked before its second same-named
page finished navigating. That failed test is preserved; its browser required
forced cleanup and it is not counted as a pass. The harness now observes a new
document instance before interacting. Its uniquely named completed test file
was removed only after verifying the expected body. Evidence:

- `.vm/modern-download-registry-build.log`
- `.vm/modern-download-filename-tests.log`
- `.vm/modern-downloads-b57427e749df55f2fc6642c5/result.json`
- `.vm/modern-downloads-d93d31817cde3ee4ae0a1a8b/result.json`
- `.vm/modern-downloads-b02fdd46b65c824ebe07e78f/result.json` (failed fixture)
- `.vm/modern-download-failed-fixture-cleanup.json`
- `.vm/modern-close-78988a46ca7a790a339b07dd/result.json`
- `.vm/modern-context-b710a2e287b0485f2374e2b5/result.json`
- `.vm/modern-download-legacy-build-tests.log`

A killed network process can leave an incomplete destination file; the tested
crash reports failure and releases the registry entry, but does not restore or
remove that file. Pause/resume, persistent download management and crash
recovery remain unfinished. See [the download notes](modern-downloads.md).

These results do not establish complete web-platform or extension compatibility.

The native HTML select popup adapter compiles and its real widget passes
**104 checks** across eight scenarios. Tests cover keyboard navigation from the
current selection, disabled/group/separator rows, Escape and explicit cancel,
owner destruction during tracking, large RTL menus with native scrolling, and
exactly-once completion. Native tracking runs off the application thread and
its activity remains counted until menu destruction and completion submission.
Application shutdown waits asynchronously for native UI activity, then drains
one final queued turn. Haiku rounds pulse intervals to 100 ms; a smaller test
interval initially disabled polling, and corrected tests now pass. Evidence:
`.vm/modern-popup-inputs.KE5XgpFQ/result.json`. Run
`python3 tools/test-modern-popup.py`. HTML input/change events and multiple-list
behavior still need the linked engine; `tests/fixtures/native-popup.html`
supplies those pending interaction cases.

Native JavaScript dialogs pass **95 checks** for alert, confirm, prompt and
before-unload controls, origin display, Unicode, embedded NUL defaults, edited
empty values, Return-key acceptance, cancellation and close/reply races.
Both bridge objects compile and the common page-proxy close changes pass native
syntax checking. Prompt input uses a native `BTextView`: Haiku's
`BTextControl` truncated embedded NUL defaults through its internal `BString`
conversion. Completion ownership moves to the WebKit thread before native
callbacks are released. Evidence: `.vm/modern-dialog-inputs.lJCsSiTv/result.json`.
Critical close notifications retry native queue backpressure without blocking
the WebKit thread. The browser retains every tab until all approve a window
close; cancellation resets approvals and preserves documents and undo state.
Background tabs become selected only when they need a before-unload prompt.
Previous address drafts, text selection and focus are restored unless the user
has since selected another tab. The revised close flow passed all four native
UI object compilations across both backends; evidence:
`.vm/modern-close-ui-inputs.gjo0y9hr/result.json`.
Close approvals also track navigation and document identity. A final batch
validates every approval on WebKit's main thread before closing any engine
page. Haiku cancels a close after a 30-second process-response timeout and
rejects late replies; a visible prompt suspends that timer. (Since 1 October
2026 the wait is 10 seconds, and a page whose process does not answer a ping
either is closed; see the top of this file.) The revised bridge
and both UI backends compile; the 95 dialog checks still pass. Evidence:
`.vm/close-navigation-final-report.json`.
`tests/ModernCloseTests.cpp` compiles as an external native harness. Its pending
runtime scenarios cover cancellation after an earlier approval, live document
and undo preservation, saved sessions, repeated prompts, background address
draft/focus, newer selection and navigation during quit. Run
`python3 tools/test-modern-close.py --bundle NATIVE_FULL_BROWSER_BUNDLE`.
Actual JavaScript dialog IPC and these close scenarios still require the
matching linked engine.

The new `BWebKitContext` adapter and its six affected engine objects compile
natively. A context owns one process pool and website data store; persistent
paths derive from the selected profile, while private contexts use ephemeral
stores without creating profile directories. Haiku now creates a separate
NetworkProcess for each store because Curl keeps its alternative-services file
path in process-global state. The previous default network-process startup
also injected unrelated stores; Haiku is excluded from that path. Logs:
`.vm/context-native-compile-fixed.log`, `.vm/context-native-common-compile.log`.
Actual cross-profile/private storage and process-lifecycle isolation still need
the running engine and are not established by these compiler checks.
`tests/ModernContextTests.cpp` also compiles natively. Its twelve pending runtime
stages exercise visible/HTTP-only cookies, localStorage and committed IndexedDB
writes across shared tabs, separate persistent/private contexts, persistent
reopen and fresh private reopen. Native team enumeration checks distinct network
processes and waits for helper teardown before reopening. A dedicated HTTP
fixture checks complete ordered reports without changing the existing fixture
server. Run `python3 tools/test-modern-contexts.py --bundle NATIVE_BUNDLE_PATH`
after freezing a matching modern engine.

Seven production extension parser/resource units compile with
`WK_WEB_EXTENSIONS=1`; the real native resource resolver passes **27 checks**.
The shared parser and matcher retain upstream behavior, resource/error handling
is reused from the GLib implementation, and native icons use real WebCore
bitmap/SVG decoders. Path checks cover native filename encoding, canonical
containment and symlink escapes. Manifest execution and browser extension
execution have not yet run. Detailed evidence and remaining integration work
are in `tools/test-engine-extension-manifest-core-notes.md` and
`docs/webextensions-native-integration.md`.
The native ZIP/XPI extractor also passes **111 checks** using real libzip 1.11.4,
and the constructor accepting archive files compiles with extensions enabled.
It extracts regular resources into an owned private directory, rejects path
escapes, links/special entries and corrupt/encrypted containers, bounds expanded
data, and cleans up failed or unconsumed extractions. Native manifest execution,
installation UI and signature/trust handling remain unfinished. Libzip is
required only with extensions enabled; test dependencies were privately unpacked
without OS installation. Evidence and dependency hashes are in
`tools/test-engine-extension-archives-notes.md`.
The native extension controller configuration now supplies default/UUID paths,
temporary directories and copy behavior while sharing the configured website
data store. Its two production units and real-class assertion unit compile with
extensions enabled. The assertions cover factories, copy identity, custom paths
and temporary storage, but have not run against a linked engine. Evidence:
`tools/test-engine-extension-configuration-results.json` and the accompanying
notes. The full engine still builds with extensions disabled.

Native keyboard, mouse and wheel translation passes **38 checks** through the
production WebKit event classes and WebCore converters. Events own their native
messages, preserve Unicode, repeat, timestamps, modifiers, fractional positions
and multi-button transitions, and reject malformed input. Physical key names
are corrected for P, Enter, numeric keypad and Meta keys. Editing checks cover
caret and selection movement, word/paragraph/document boundaries, deletion,
clipboard commands, undo/redo and tab/newline handling. Run
`tools/test-engine-native-events-in-vm.sh`; log:
`.vm/native-event-editing-tests.log`. The page editing adapter is wired into the
modern port; complete DOM editing still needs the running modern browser.

The modern build reached its current Curl network backend and Haiku platform
objects. Obsolete Haiku inspector source files referred to removed interfaces;
the build now uses upstream's unsupported-frontend implementations until the
native inspector window is ported. Source synchronization now compares content,
retains timestamps for identical files and gives changed content a current guest
timestamp. This prevents generated IPC headers from remaining stale when host
edits predate a build that still used the old source. The previous affected
inputs were explicitly refreshed. Host checks cover changed/identical content,
file modes, removal of managed files and archive path boundaries. Subsequent
native builds regenerated the IPC messages and compiled the drawing-area and
view adapters. The full build is now compiling fixes for the collected
font-serialization, generic-platform include and native cache-filesystem
errors. The font argument coders also pass the upstream serializer generator.

All 32 Haiku legacy embedding source files pass the native compiler's syntax checks.
The initial pass found five failing files caused by two upstream API changes:
the color chooser's coordinate query and the frame loader's `std::expected`
callback. The adapted color chooser uses screen coordinates, and unsupported
HTTP/2 pings now complete with an error instead of leaving their callback pending.
The five affected files pass when compiled from an isolated corrected source
copy. Full object generation, linking and browser runtime checks subsequently
passed. Logs are `.vm/legacy-syntax.log` and `.vm/legacy-syntax-fixed.log`.

WebCore and WebKitLegacy compile and link successfully. The source transfer was
corrected to include root `Configurations/`, which supplies the version header
input. The current renderer build log is `.vm/webcore-build.log`.

The source changes address the initial preference-generator conflicts and
adapt the legacy Curl loader to current cookie-storage interfaces. Its cookie
and fetch behavior passes the real browser fixture. Native keyboard text, font
drawing and sizing, color spaces and canvas
buffer allocation have been adapted to current upstream interfaces and compile.
The media seek implementation now returns the required completion promise,
checks native seek errors and bounds its paused-video read to one frame;
media behavior still needs runtime verification. Native text/theme changes and
generated DOM bindings compile; the current renderer works in Summit as recorded
above. Broader upstream conformance and platform coverage remain incomplete.
The inherited Haiku engine feature switches also leave
substantial full-browser work, recorded in `REQUIREMENTS.md`.

## Material limits

There is no extension installation or execution runtime yet. Safari, Chrome
and Firefox extension support remains part of the objective, with no
compatibility claim at this stage.

The native preview currently has one window with up to 512 tabs, 25 recently
closed tabs, 2,000 history entries, and a fixed search provider. Tabs beyond the
visible strip remain reachable with Ctrl+Tab. Downloads have basic save-to-folder
behavior. The current engine reports success/failure explicitly; the older
system engine only reports that a download ended.

Private browsing, complete profile storage isolation, granular site permissions,
authentication and certificate UX, a full download manager, engine process
isolation, accessibility, media feature coverage, extension compatibility,
performance and long-duration browsing all require implementation or stronger
verification. The preview's tests do not establish those requirements.
The inherited Curl cookie database also lacks SameSite persistence/enforcement
and partitioned cookie storage; the new adapter does not claim to fix those
requirements and rejects partitioned access instead of mixing stores.
(SameSite is enforced since 20 October 2026, [networking.md](networking.md);
partitioned storage is still missing.)

## Native helper exit follow-up

The download bundle `bundle-lb1k0wql` has been copied with matching patched
engine sources to `artifacts/modern-browser/bundle-lb1k0wql`. Its copy
provenance SHA-256 is
`b10cc4793071e056b295939bf587387133b9a00c6c49de3bfcc8e76f59eafd3f`.
Evidence: `.vm/modern-download-bundle-copy.log` and the artifact's
`copy-provenance.json`.

A later inspection of native syslog found shutdown traps missed by the earlier
helper-disappearance assertions. Thread 432464 in `bundle-eih3q6or` faulted in
`BBlockCache::Save()` from a WTF worker RunLoop; thread 434486 in
`bundle-ihenjatl` reported a bogus pointer/double free. The UI process can kill
a helper after it has trapped, leaving the previous test's main exit status and
helper-count checks passing. These are separate from the corrected native
looper lock hang. `.vm/native-network-exit-kernel-stacks.log` preserves the
kernel diagnostics. The second event lacks a usable stack and is not yet
attributed to a specific destructor.

Haiku libbe's `terminate_after()` destroys the global BMessage cache. The curl
NetworkProcess entry point returns through ordinary `exit()` while
process-lifetime worker threads remain active. A Haiku-only correction now uses
`WTF::exitProcess()` after the normal auxiliary event loop and session teardown,
as WebProcess already does. This retains the existing cookie/localStorage sync
before the loop stops. Context, close and download runners now inspect only new native syslog
bytes during their runtime and reject native debugger events or a gap in log
coverage. This is a conservative VM-wide check; those tests must run serially.

The monitored baseline reproduced the shutdown fault in `bundle-lb1k0wql`
after four clean context runs. The fifth run stopped after eight storage
stages: NetworkProcess 438100's worker 438113 trapped in
`BTokenSpace::RemoveToken()` from its RunLoop TLS destructor while the main
thread had reached `_kern_exit_team`. The native Save report action terminated
that already failed helper. This run remains **failed** (harness exit 1 and
fresh debugger events), not a recovered pass. Evidence:
`.vm/modern-context-9aa04553d127a7e6cbd7a13e/result.json`,
`.vm/modern-network-exit-baseline-kernel-stack.log`,
`.vm/modern-network-exit-baseline.report`.

The corrected patch
`92a51ca2e5b29cdcd83706f3e125fd4a99525eef31fb3adb2b8f3a730b3866ec`
builds all targets and is frozen in native `bundle-g3wycs_1`. It passes five
consecutive 12-stage profile teardown/reopen runs, all 143 close checks, all
59 browser download checks and all eight API download scenarios. All runs exit
normally, preserve their frozen bundle hashes and report complete native log
coverage with no new debugger events. The API deliberately kills its own
verified NetworkProcess in one recovery scenario; that expected SIGKILL is
separate from a native debugger trap. Five host tests verify that the monitor
rejects new traps, rotation and truncation without retaining unrelated syslog
data. These checks address the reproduced shutdown failure; they do not establish
long-duration browser reliability.

Evidence: `.vm/modern-network-exit-build.log`,
`.vm/modern-network-exit-bundle.json`,
`.vm/modern-context-fd18f7021a611d1e27818a29/result.json`, and:

- `.vm/modern-context-0f7f0793418fbad3bb789e7f/result.json` (context-repeat-1)
- `.vm/modern-context-06d6a643e3484c2767ffe001/result.json` (context-repeat-2)
- `.vm/modern-context-5440f3881bc1ddf399e2c356/result.json` (context-repeat-3)
- `.vm/modern-context-a98adb465b9de8fc98db30d0/result.json` (context-repeat-4)
- `.vm/modern-close-5c264f3188a0e12c4b1298ec/result.json` (close)
- `.vm/modern-downloads-f1a46e71c14e4bc6ac7a9596/result.json` (download-ui)
- `.vm/modern-downloads-d72576c84112a53ead58ea6e/result.json` (download-api)

The corrected `bundle-g3wycs_1` is also preserved under
`artifacts/modern-browser/bundle-g3wycs_1` with the matching patched source
archive and rebuild inputs. Copy provenance SHA-256:
`5c5a9ec6a0603e926519553fce2d24cf429be793e635c02838a239fa36dcdd22`.
Evidence: `.vm/modern-network-exit-bundle-copy.log` and the artifact's
`copy-provenance.json`. Earlier bundles remain available with their recorded
limitations.

**22 October 2026 shutdown recheck:** release builds passed five stock Haiku
TLS/worker/WebSocket shutdown cycles, but a subsequent run trapped in libepoxy
from the WebProcess compositor. The airOS workstation passed one cycle and
then stalled during worker-request startup. These are failed acceptance runs,
not evidence to close #120. See [the report](network-shutdown-2026-10-22.md)
for the current workload, hashes and native stack.

**22 October follow-up:** `b22cb50` fixes curl requests stranded behind a
streaming HTTP/1.1 connection; published `e732dd0` stock-Haiku packages pass five
network cycles. The graphics abort is traced to libroot publishing uninitialized
locale tables. Early initialization now avoids that race: an isolated airOS
engine and stock packages with the candidate JavaScriptCore library each pass
20 complete cycles. Published `56f49a8` packages then pass five complete cycles
on each x86_64 target, without a preload or substituted library; cleanup,
persistence, native crash monitoring and unchanged-input checks all pass.
See [the locale report](locale-startup-2026-10-22.md) and
[release evidence](network-release-2026-10-22.json). The network shutdown recheck
is complete; #120 has other unfinished items.

## Native navigation outcomes and history

Patch `b6ffb54286c7f699389f0502021540bd8b79d9a7b100d242b68e09b8e8889686`
builds all modern targets and is frozen with the browser in `bundle-yqhmejfw`.
Main-frame failures now retain the actual URL/domain/code/description and
distinguish failures before and after document commit. Errors stay with their
tabs, clear on another navigation, and appear in native status text/tooltips.
Stop and attachment navigation handoffs finish as cancellations. History records
successful documents and fragments, excludes failed/stopped requests and
preserves a successful visit's title when a reload fails. The focused native
address control now submits unchanged text on Enter; focus loss still preserves
unsubmitted drafts. See [the navigation error notes](modern-navigation-errors.md)
for the native notification fields and test commands.

The original document's NavigationIdentifier can accompany an anchor callback.
The adapter checks an explicit fragment target and its unchanged document
identity without treating a newer unrelated request or reload as complete.
Native close-handshake identity remains separate from load bookkeeping.

The frozen bundle passes **90 native navigation-error assertions**, with actual
HTTP fixture requests and JavaScript reports verified independently. Cases cover
dropped connections, a committed truncated response, retrying an unchanged URL,
a failed reload, a valid 404 document, cancellation after request dispatch,
overlapping requests to the same URL, tab-local errors, fragment replacement,
back/forward navigation and saved history. All **143 close checks**, **63 browser
download checks** and **12 context-storage stages** also pass. Download checks
confirm that handoffs do not show load errors, attachment navigation settles as
cancelled while its download continues, and download URLs are absent from saved
page history. Every run exits normally, preserves frozen bundle hashes and
reports complete native log coverage with no new debugger events. The updated
application also builds with the legacy engine and passes its 38 portable checks.

Passing evidence:

- `.vm/modern-load-errors-fragment-build.log`
- `.vm/modern-load-errors-fragment-bundle.json`
- `.vm/modern-load-errors-dba6147b8774d4cd0eaf71ba/result.json`
- `.vm/modern-close-367320962a730451efc16e7a/result.json`
- `.vm/modern-downloads-6e039080359c03cd509312e0/result.json`
- `.vm/modern-context-156fde849d22b5339bd4c3b2/result.json`
- `.vm/modern-load-errors-enter-legacy-build.log`

Three earlier runs remain failed. `bundle-z8o5tug8` exposed Haiku text control's
suppression of Enter for an unchanged address; `bundle-f0em3ep4` exposed the
original-document ID in anchor callbacks. A first `bundle-yqhmejfw` run passed
all 90 native assertions but failed its fixture gate because Stop cancelled the
slow request before HTTP dispatch. The harness now allows dispatch time and
still requires independent server receipt. These are recorded in:

- `.vm/modern-load-errors-432aea1d587fa6efb14e21ad/result.json`
- `.vm/modern-load-errors-c57fa8f10a43a7cfac72624e/result.json`
- `.vm/modern-load-errors-5981023ddbef8c440e8f48d9/result.json`

Certificate/authentication UX, a dedicated recovery error page, wider network
failure coverage and extension runtime remain unfinished.

The verified navigation bundle is copied to
`artifacts/modern-browser/bundle-yqhmejfw` with its matching patched engine
sources and rebuild inputs. Copy provenance SHA-256:
`2afb85bbcb6566bca96340d22b72eb9fdb9159c3f892f765d24417eb9470487d`.
Evidence: `.vm/modern-load-errors-bundle-copy.log` and the artifact's
`copy-provenance.json`.


2026-09-14 extension lifecycle port: the native context header now supplies
WebPageProxy/API configuration/navigation types and JSON state ownership.
Five common controller/context/process-pool/proxy units compile with both
extension features enabled in an isolated configuration. The C++ process
context lifecycle, permission expiry, localization and page tracking moved
from Cocoa into the common source and passed a final native compile.
The normal browser still has extensions disabled; no extension executes.
A separate full feature-enabled source/build tree is being compiled through
`tools/build-webkit-in-vm.sh --modern-extensions all`. Native controller/context,
resource serving, bindings and browser delegates remain unfinished.
Compile evidence and exact limits: [native integration](webextensions-native-integration.md).


The first full extension build configuration failed because the privately
unpacked libzip used by archive tests was absent from pkg-config's search path
(`.vm/modern-extensions-first-build.log`). The build now prepares a private,
checksum-locked libzip prefix and configures successfully. Its separate
source tree was verified onto a new 64 GiB build volume before compilation
started (`.vm/modern-extensions-private-zip-build.log`). No full feature-enabled
link or extension runtime has passed yet.


2026-09-14 native extension adapters: the common load/unload/reload sequence,
storage invalidation, storage-level broadcasting and script-error formatting
were extracted from Cocoa. Native context construction/state/access-level
migration/origin migration and the resource scheme handler compile. Controller
startup expiry now uses RunLoop, and native context receivers preserve both
identifiers. Native JSON state uses private temporary files and atomic rename.
Its standalone test passes **33 checks**, including 100 replacements observed
through **3,455 concurrent reads**, with no new debugger event and unchanged
source/dependency hashes. No WebCore or WebKit library is linked by that helper
test, and no context/controller/browser extension runtime was exercised.

Evidence and unresolved native dependencies:
[extension state and resources](webextensions-native-state-and-resources.md).
Patch: `fd009b76e291c55a5ba28f2400c9071c4e620667ccd46b1fadcb860c19f8c192`.
The first full extension attempt used its frozen earlier patch
`55fe568fdb25b99463eb0519208f969a9b71b1cc7126d784e0ffa1af150d637c`;
that attempt ended after the VM failure described below.
The verified browser artifact remains `bundle-yqhmejfw` with extensions off.

The first full feature-enabled attempt reached step 6746/7527, with
JavaScriptCore, jsc and PAL linked, then stalled in native filesystem waits.
Kernel traces show a compiler in `ModifiedPageQueue::WaitIfOverQuota` and
other workers waiting on BFS journal locks; another compiler had trapped at
a null PC. The concurrent page-hook compile probe is also incomplete, and
its result is recorded as failed. A checkpoint of both disks and VM memory
was saved before restarting the test VM. It is responsive again, with both
source manifests intact. Complete the isolated probes before retrying the
full build with three workers. See [VM recovery evidence](VM.md) and
`.vm/extension-build-vm-stall.json`. This is not a successful feature-enabled
engine build or an extension runtime result.


2026-09-14 process/event port: common world/namespace binding, navigation sends
and script-error sends now compile alongside Haiku WebPage/frame hooks. The
event API and UI listener bookkeeping moved to common C++, with the event IDL
on the existing C++ generator path. Script-error and listener messages retain
their loaded-context validators. Five process/page units and four event/IPC
units passed after the VM restart; the wrapper fix also passes. These remain
compile checks, with no extension execution or feature-enabled WebKit link.

The promoted patch is
`1daf0679f17993a8988c64dcd33fa4e04e8ea4cb12e29a79f4a9866849a1d49f`.
A new separate full build uses three workers and this patch, recorded in
`.vm/modern-extensions-process-events-build.log`. The preview artifact remains
`bundle-yqhmejfw`. Exact source scope, failures, evidence and missing runtime
work: [native process bindings](webextensions-native-process-bindings.md).


2026-09-14 native port bindings: the JavaScript port API now uses common C++
and generated C++ bindings. Page ownership preserves listener multiplicity
and independent channels. Its native helper passes 22 assertions with no
fresh debugger events; restoring the former behaviors in an isolated
mutation fails nine assertions as expected. Port/namespace/runtime/context
units, the generated port binding and both IPC receivers compile. Incoming
port messages retain loaded-context validators and gesture restrictions.
The final UI disconnect-event route also compiles. No extension was run.

Patch: `91b1db7aefd1bc6212d452e2eeb3871e99ecb23febee1d8ea6472eca89d5b02f`.
The full three-worker engine build continues on its frozen `1daf0679...`
source tree; it has progressed beyond the WebCore JavaScript and module
units. The preview artifact remains `bundle-yqhmejfw`, with extensions off.
Remaining message routing, sender conversion and runtime requirements are
recorded with the precise test scope in [native ports](webextensions-native-ports.md).


2026-09-14 message reply port: the common runtime dispatcher now uses
JavaScriptCore native functions and C++ callback aggregation instead of
Objective-C blocks. Its helper passes 51 native JavaScript assertions,
including promise/thenable handling, reentrant and duplicate replies, forced
garbage collection of retained callbacks, unanswered context destruction,
and message-size boundaries. Both the helper and runtime dispatcher compile
with extension features enabled. No extension context or IPC path was run.

Patch: `fb28750f4eb0d93caf2c9190b2c09e152af29c914fd2f475bb94180e5a9dd90c`.
The separate full build continues on `1daf0679...` through WebCore DOM units;
its accessibility unit repeated the previously observed non-void-return
warning, with no reported compile failure so far. The preview artifact is
still `bundle-yqhmejfw`, with extensions off. See [message replies](webextensions-native-message-replies.md)
for exact evidence and the remaining sender/UI/background work.


2026-09-14 extension string bridge: conversion now preserves UTF-16 code
units directly instead of passing through NUL-terminated UTF-8. All 46
native assertions pass, including all 65,536 code-unit values and actual
JavaScript evaluation. Restoring the old conversion fails 21 assertions
as expected. The string helper, wrapper and runtime unit also compile.
No fresh debugger events or source/dependency changes occurred in the
successful helper run. See [string evidence](webextensions-native-strings.md).

Patch: `afeb9db214b575cddb072d8f65e890cb1cb10d0508b6a55bae32a00089e1074e`.
The separate full engine build remains frozen on `1daf0679...` and is
progressing through WebCore HTML units. The verified preview remains
`bundle-yqhmejfw`, with extensions disabled.


2026-09-14 message metadata: sender conversion now lives in common C++ and
uses the new JavaScript tab converter. All 27 native tab assertions pass,
including missing/false properties, identifier sentinels, NaN indices,
URL fragments and UTF-16 titles. The helper, runtime sender conversion
and port unit compile with both extension features enabled. No extension
context or metadata IPC path was run. See [message metadata](webextensions-native-message-parameters.md).

Patch: `23064dc4cd0304e24b9c5924d1aa7238c727d76439502b37887affdc1162516c`.
The separate full engine build continues on frozen `1daf0679...` through
WebCore page units. The verified preview remains `bundle-yqhmejfw`, with
extensions disabled.


2026-09-14 message routing: eight UI runtime handlers and outgoing port
delivery now use common C++. Six runtime message declarations and their
process sends retain loaded-context validation outside Cocoa. All four
native units compile, including the regenerated UI receiver. This is
compile coverage; no extension messages were run. Permission checks,
external-connection restrictions and gesture policy are preserved in the
extracted source. See [routing evidence](webextensions-native-message-routing.md).

Patch: `e3a4cf2b5baf68e779cb6cb3fcd8d3bf38f2e242f7aacd7a96645971efb65ea8`.
The full three-worker build continues on frozen `1daf0679...` through
WebCore platform units. The preview remains `bundle-yqhmejfw`, extensions off.


2026-09-14 native background adapter: the extension context can construct
and own a hidden native page client with extension-specific configuration,
document/worker load callbacks, background identities and unload/restart
paths. UI page creation now supplies the extension controller on Haiku;
page removal performs common port cleanup. Five selected translation units
compile with final sources and headers: the adapter, native constructor,
common context, native view and UI page proxy. Earlier failed probes are
retained separately. No background document or worker was executed.

Patch: `208a07045b9623b53979da1eab879dd1c16db9a88c39e59bc0b22c4181602426`.
See [background evidence and remaining lifecycle work](webextensions-native-background.md).
The full three-worker build continues on frozen `1daf0679...` through
WebCore style units. The preview remains `bundle-yqhmejfw`, extensions off.


2026-09-14 background listener persistence: native JSON state now stores
named events with exact listener counts, rejects malformed/stale caches
and preserves unrelated state. All 44 helper assertions pass in each of
two native enum layouts; the optional event shifts `RuntimeOnMessage` from
28 to 29 without changing decoded saved names. A count-collapsing mutation
fails five checks as expected. The native background adapter also compiles
with the new persistence helper and fresh-registration cache reset.

Patch: `a3972b1c21db2ef8fef493e3883a3e588064909cd30365febaaeb0317ec0f89a`.
See [listener-state evidence](webextensions-native-background-listeners.md).
The full three-worker engine build continues on frozen `1daf0679...` through
WebCore worker units. The preview remains `bundle-yqhmejfw`, extensions off.


2026-09-14 deferred background replies: pending runtime operations now retain
an explicit fallback until they start; cancelling the queue completes these
replies. Background task draining uses a local snapshot and page/context
identity guards around callbacks. All 18 native lifetime-helper assertions
pass, and the UI runtime, common context and native background units compile
with both extension features enabled. The context's cancellation timing and
reentrancy still need runtime checks. See [deferred reply evidence](webextensions-native-deferred-replies.md).

Patch: `c711e3e700a24caf8c796eee55989f36d12d139e3129f92aeea80fd5d3db26cb`.
The full engine build on frozen `1daf0679...` has created `libWebCore.a` and
continues through WebKit units. A feature-enabled WebKit link and extension
runtime remain unverified. The preview remains `bundle-yqhmejfw`, extensions off.


2026-09-14 media IPC enum correction: the generated validator now accepts
Haiku's native media-engine identifier. All 256 native wire-value checks
pass; the original definition fails only the Haiku value, reproducing the
full-build warning. This tests the actual generated validator function,
not media playback or IPC transport. See [media validator evidence](media-engine-ipc-validator.md).

Patch: `2ea935a16cbd980a09e5bc20229863c257a22e09e2fc48c29afcc8837d7936b6`.
The running full engine build remains frozen on `1daf0679...` and continues
through WebKit UI-process units. It does not yet contain this correction.


2026-09-14 registered script restoration: common C++ now parses saved JSON,
validates resources/match patterns and constructs injected content. A
restore generation and loaded/permission checks reject stale database
callbacks after unload or reset. All 46 native parser checks pass; the
parser, common context and corrected scripting unit compile with both
extension features enabled. SQLite restoration, callback invalidation and
script injection still need runtime verification. See [registered-script evidence](webextensions-native-registered-scripts.md).

Patch: `1d075f46ad782a384ae548bd1a2f1b390ace18e0ea342e8ea44ace3322cb0543`.
The full build remains on frozen `1daf0679...`, now through WebProcess units.
The verified preview remains `bundle-yqhmejfw`, with extensions disabled.


2026-09-14 network-rule unload cleanup: the existing rule-removal and
DNR-state-unload methods now live in common C++, preserving both bodies.
Removal visits all controller content stores, including any previously used
for private browsing; unload clears rule IDs/matches and releases the two
SQLite stores. The actual common DNR unit passes native compilation with
both extension features and regenerated IPC:
`.vm/extension-lifecycle-inputs.z9DsNtGX/result.json`. Input/configuration
snapshots and the final source hash match. This is compilation evidence;
DNR translation, native state loading and runtime removal remain unverified.

Patch: `1b839dea86a02bbebcf8210d82f7ba85c51c7b1fea0421f09ebea8622f254f27`.
The full build remains frozen on `1daf0679...` and is finishing WebProcess
compilation. No feature-enabled WebKit link has passed yet.


2026-09-14 full extension compile/link result: the three-worker native
attempt on `1daf0679...` completed its WebCore/WebKit compilation and failed
linking `libWebKit.so.1.10.0`, with 82 distinct unresolved symbols. The
record is `.vm/modern-extensions-process-events-build-result.json` and the
full diagnostic log is `.vm/modern-extensions-process-events-build.log`.
Missing symbols include platform content-rule storage/mapping support and
extension events, permissions, delegates and JS helpers. No feature-enabled
WebKit library or extension runtime passed this attempt.

A subsequent three-worker build is now frozen on
`1b839dea86a02bbebcf8210d82f7ba85c51c7b1fea0421f09ebea8622f254f27`,
which includes the intervening porting work. It reuses compiled dependencies
and rebuilds 267 steps after regeneration; log:
`.vm/modern-extensions-native-runtime-build.log`.


2026-09-14 content-rule platform closure: the native default store path now
uses application-specific settings storage, and mapped Curl cache data can
supply shared memory while retaining its file handle. Wrappers own a
duplicate descriptor and export read-only capabilities for read-only files.
All 17 focused mapping checks and all 39 existing shared-memory regression
checks pass natively; the store adapter compiles. An earlier failed runtime
probe caught a borrowed-descriptor ownership error, corrected before these
passes. See [platform evidence](content-rule-platform-support.md).

Patch: `55777c374a27e62895e7404aaea3efe33042b78fe1c7d4d459fe4bc057faf861`.
The full build remains on frozen `1b839dea...`, now through WebKit shared
units. No native content-rule store or DNR runtime has been exercised.


2026-09-14 rejected-promise helper: the missing binding helper now uses
common C++ and JavaScriptCore's deferred-promise API. All 20 native checks
pass, covering rejection/error identity, formatted and UTF-16 reasons,
replaced global constructors, collection and repeated settlement observers.
The actual utilities, wrapper and string bridge compile and link against
frozen JavaScriptCore; the utilities unit also passes the full-feature
compile probe. No extension binding/context or IPC route was executed.
See [rejected-promise evidence](webextensions-native-rejected-promises.md).

Patch: `6338f83f047dcaa3b58be944341804d64cb421d092305cbd7b0cffeb083d6069`.
The full build remains frozen on `1b839dea...` and continues through UI-process
units. The preview remains `bundle-yqhmejfw`, with extensions disabled.


2026-09-14 registered-script ownership: the allocator, parameter updates and
script/style ownership methods move unchanged from Cocoa to common C++.
The frame document identifier helper also moves unchanged. All four selected
native units compile with both extension flags enabled and regenerated IPC;
record: `.vm/extension-lifecycle-inputs.F2WTQUFt/result.json`. No live frame,
script removal or injection was exercised. See
[registered-script evidence](webextensions-native-registered-scripts.md).

Patch: `d5c9a8c180f514ddebb4d39392d4df6b08873769d9e3f733c82b615afb0d6aa8`.
The full build remains frozen on `1b839dea...`, now finishing WebProcess
compilation. No feature-enabled WebKit link has passed.


2026-09-14 second full extension link result: the build on `1b839dea...`
completed compilation and failed at the WebKit shared-library link with 65
distinct missing symbols. Nineteen symbols from the prior attempt are
resolved; two additional dependencies are exposed. The detailed record is
`.vm/modern-extensions-native-runtime-build-result.json`. The cache mapping,
store path, promise and script-ownership fixes landed after this build's
source freeze and were not included. No extension runtime was reached.

The DNR database now limits its old NSKeyedArchiver migration to Cocoa,
matching the existing registered-script store. Haiku uses the common schema
version handling: current JSON databases are retained, and unsupported old
schemas are reset. This does not import legacy Apple archive data.
The actual DNR store compiles natively with both extension flags enabled;
`.vm/extension-lifecycle-inputs.GM67iVUr/result.json` records unchanged inputs
and configuration. Database migration has not been exercised at runtime.
Patch: `7be92cbcfb08a29914cf9f58eed2197bd4dd20f7f8755f10f88f5cb3b877a284`.


2026-09-14 sorted JSON helper: native extension test reporting can now
serialize JS values with recursively sorted object keys. All 28 native
checks pass, including JSON semantics, Unicode, getters, `toJSON`, errors,
precision and collection. The actual wrapper also compiles with both
extension features enabled. See [evidence](webextensions-native-sorted-json.md).
No extension test API or IPC runtime was exercised.

Patch: `7f17d7a6cc8b598ce1774299459c0e953e5002f938c75db14201bced7aff9029`.

A three-worker full build is now frozen on this patch, including the tested
cache mapping, store path, promise, ownership, migration and sorted-JSON
changes. It rebuilds 53 steps after regeneration; log:
`.vm/modern-extensions-storage-helpers-build.log`.


2026-09-14 navigation and cookie events: six existing context methods move
unchanged into common C++, and the cookie-store observer now forwards native
notifications to the controller. Both final units compile with both extension
flags enabled and regenerated IPC. No live extension event was exercised;
tab/action delegates and process dispatch remain incomplete.
See [event-forwarding evidence](webextensions-native-navigation-events.md).

Patch: `a3f30d1b01848597f29ffa1009bc53cee272ecb4dcc48fa09631d8e7fac2e469`.
The running full build remains frozen on `7f17d7a6...`.


2026-09-14 popup/tab page associations: four context methods move unchanged
to common C++, preserving process-specific page IDs and optional tab/window
IDs. The final context unit compiles natively with both extension flags and
regenerated IPC; `.vm/extension-lifecycle-inputs.y4RuJo8n/result.json`.
No page association or popup/tab delegate was exercised at runtime.

Patch: `b85b0ea4c1b19d2bf6b0c421e1916c5a7d90e6f183f0888f1995bd4320763101`.
The full build remains frozen on `7f17d7a6...`.


2026-09-14 third full extension link result: the `7f17d7a6...` build completed
compilation and failed at the WebKit shared-library link with 57 distinct
missing symbols. All eight storage/helper symbols targeted since the prior
attempt are resolved, with no additional unresolved symbols. Record:
`.vm/modern-extensions-storage-helpers-build-result.json`. Navigation/cookie
forwarding and page associations were added after this attempt's source
freeze. No feature-enabled WebKit link or extension runtime passed.

2026-09-14 runtime namespace gates: the extension and webpage namespace
bindings now expose their existing runtime getters on Haiku. Both generated
bindings and both C++ namespace units compile. A native object-symbol audit
confirms that the getters are present in the new objects and absent from the
previous full build. See [namespace evidence](webextensions-native-runtime-namespaces.md).
No `browser.runtime` call or external message was executed.

Patch: `75a0dfd1b1212c05e00e58ec22cd613289a90ac45284517fe47803024adf3b70`.

2026-09-14 fourth full extension link result: the frozen `75a0dfd1...` build
completed compilation and failed at the WebKit shared-library link with 53
distinct missing symbols. Eight navigation/cookie/page-association symbols
are resolved. Their callers expose four further dependencies: action
customization clearing, temporary user-gesture clearing, tab lookup by
extension ID and the tab's window delegate. Record:
`.vm/modern-extensions-namespaces-build-result.json`.
No feature-enabled WebKit link or extension runtime passed.

2026-09-14 native storage bindings: the storage namespace and area methods,
change dispatch and seven messages now use common C++ on Haiku. Two more
interfaces use C++ bindings, bringing the total to ten of 37. The value/key
adapter passes 73 native checks, including proxies, UTF-8 call/item quotas,
default-value identity, numeric/prototype-looking keys and garbage collection.
Nine integration units compile with both extension flags and regenerated IPC;
the final helper has an additional compile check. No storage IPC, SQLite
persistence, access-level update or extension event was run. Database quota
and key handling need further tests.
See [storage evidence and limitations](webextensions-native-storage.md).

Patch: `b330520c856f6a9a46eb9e787cba7acdd119772e062f68f96c06aed7c1d350a6`.

2026-09-14 context utilities: temporary user-gesture checking and clearing
move unchanged to common C++, and Haiku gets the compiled-rule file path
adapter with a directory-failure check. Both final context units compile;
no live permission revocation or rule-file lookup was run.
See [context utility evidence](webextensions-native-context-utilities.md).

Patch: `55798afee3c77e27b9dfc8ab1d46e7833d64568b5b6581a91fab79dfa6f3d5da`.
The ongoing three-worker full build remains frozen on `b330520c...`; log:
`.vm/modern-extensions-storage-api-build.log`.

2026-09-14 saved rule state: native context loading now restores valid saved
ruleset booleans over manifest defaults; missing, malformed and unknown entries
do not suppress unaffected defaults. The state helper passes 54 native checks,
including a persisted ruleset round trip and the existing 100-replacement
concurrency test. Both final helper/context units compile. No extension rule
was loaded, matched or applied. See [rule-state evidence](webextensions-native-rule-state.md).

Patch: `c6ab469dacb1003eddb43c03903945104fb676de3ad58b835b6c2eaa69c70e1b`.
The full build remains frozen on `b330520c...`.

2026-09-14 fifth full extension link result: the frozen `b330520c...` build
completed compilation and failed at the WebKit shared-library link with 52
distinct missing symbols. Storage-change dispatch is resolved, with no new
missing symbols relative to the preceding build. This build predates the
context-utility, saved-rule-state and website-data cleanup changes. Record:
`.vm/modern-extensions-storage-api-build-result.json`.
No feature-enabled WebKit link or extension runtime passed.

2026-09-14 stale extension website data: Haiku now adapts Cocoa's persistent
cleanup path, sharing its origin-selection loop. All 29 native selection checks
pass, including mixed records that retain ordinary website origins and exclude
host/domain deletion metadata. Both final selector/context units compile. No
website-data fetch/deletion, sentinel handling or extension context was run.
See [cleanup evidence and limitations](webextensions-native-website-data.md).

Patch: `adb8b391c6e42031b5ccd084b4898216e9634bfc929214306c7a58f63ec23ac6`.

2026-09-14 native test reporting: all seven controller test-message handlers
now emit JSON log records on Haiku, retaining test results, values and source
locations even when release-log channels are disabled. The existing testing-mode
IPC validators remain intact. The native unit compiles with both extension
features and regenerated IPC. No test-message IPC or extension test was run.
See [test-reporting evidence](webextensions-native-test-reporting.md).

Patch: `8ba00bb96388053d59053815dba4bea5c8d7e2ee28fb67c553a8a2c1949e3c57`.

2026-09-14 sixth full extension link result: the frozen `8ba00bb9...` build
completes compilation and fails at the WebKit shared-library link with 41
distinct missing symbols, down from 52. All eleven recent context-utility,
saved-rule-state, website-data cleanup and test-reporting symbols are resolved,
with no new missing symbols. Record:
`.vm/modern-extensions-context-cleanup-build-result.json`.
No feature-enabled WebKit link or extension runtime passed.

2026-09-14 native test delivery: contexts now retain JSON test arguments while
queued, then route them to page/content-script listeners and wake background
content for main-world delivery. Haiku checks the live weak listener registry
instead of stale duplicate counters. Drains stop when unavailable and have a
fixed message budget per call. All 70 helper checks and three final integration
compiles pass. No extension context, actual listener, background wake-up or IPC
was run. See [test-delivery evidence](webextensions-native-test-delivery.md).

Patch: `5732f4073311cf11bed8a00cb90e5aa3d87ae0bea6c0199dbf15bdf3dfb9464e`.

2026-09-14 promise assertions: `browser.test.assertRejects` and `assertResolves`
now use common C++ with intrinsic promise transformation and traced callback
captures. Forty native helper checks pass, including thenable competition,
exceptions, capture lifetime and rejection-message getters/proxies. Both final
helper/API units compile with extension features and regenerated IPC. No
assertion API, report IPC or extension was run; queued-test execution remains
Cocoa-only. See [promise assertion evidence](webextensions-native-promise-assertions.md).

Patch: `35dcd668ac188e1ef81ca581f5be6303a44db6f16a5614f5e0948d9f06838643`.

2026-09-14 seventh full extension link result: the frozen `5732f407...` build
completes compilation and fails at the WebKit shared-library link with 39
distinct missing symbols, down from 41. The listener-availability query and
queued test-message delivery are resolved, with no new missing symbols.
This build predates the common promise assertions. Record:
`.vm/modern-extensions-test-delivery-build-result.json`.
No feature-enabled WebKit link or extension runtime passed.

2026-09-14 eighth full extension link result: the frozen `35dcd668...` build
completes compilation and fails at the WebKit shared-library link with 37
distinct missing symbols, down from 39. Both promise assertion methods are
resolved, with no new missing symbols. This build predates queued-test execution.
Record: `.vm/modern-extensions-promise-assertions-build-result.json`.
No feature-enabled WebKit link or extension runtime passed.

2026-09-14 queued test executor: `addTest` and sequential execution now use
common C++, retaining named-function validation, result promises and reporting.
Thenables/exceptions use the intrinsic helper; following tests are scheduled on
the main queue, and unavailable execution contexts report failure. Settled
native callback state is released immediately. All 41 helper checks and both
final integration compiles pass. No queued test, API object, DOM context or IPC
was run. See [executor evidence and limits](webextensions-native-test-executor.md).

Patch: `ba1ee173c7963284ecede95c47830e7109b13fad531aca9c2c4495f2212f221f`.

2026-09-14 ninth full extension link result: the frozen `ba1ee173...` build
completes compilation and fails at the WebKit shared-library link with 36
missing symbols. Queued test execution resolves `WebExtensionAPITest::addTest`
and introduces no new unresolved symbols. Evidence:
`.vm/modern-extensions-test-executor-build-result.json`.
The feature-enabled engine has not linked or run an extension.

2026-09-14 native navigation bindings: the process-side API, URL-filter parser,
event payloads and frame-query messages now use common C++. **136 native helper
checks pass** and **11 units compile**, including generated bindings and IPC.
Two additional interfaces use C++ bindings, bringing the total to 12 of 37.
Native frame-query handling and tab integration are still missing; no extension
API or navigation event was run. See [navigation API evidence and limits](webextensions-native-navigation-api.md).
Patch: `5358cf199868ababff67cf59212eb911603ae30f75b7980e3fae366f1b993945`.

2026-09-14 native browser tab registry: browsing contexts now own window/tab
metadata, the native SDK queues whole-window updates and checks page ownership,
and Summit submits tab and focus changes. **543 registry checks pass**, **four
engine units compile** and **five browser units compile**. SDK/browser runtime
and extension delegate integration remain unverified. See [registry evidence
and limits](native-browser-tab-registry.md).
Patch: `0f24f49c6189020ec84b14732cd10bfa31b4c7f4a555ebaa224c18f9c63d5209`.

2026-09-14 native extension controller host: feature-enabled browsing contexts
now configure pages with a controller that shares the native tab registry and
their exact website data store. Extension storage is bound to that profile;
private contexts use nonpersistent configuration. **Three native units compile**;
controller/profile/page runtime remains unverified. Native storage-deletion
notifications and tab/window delegates are still missing. See [controller host
evidence and limits](webextensions-native-controller-host.md).
Patch: `7d5fe9b8cc4f898dae1bcda278c6468dc4eee5cedd4904d0b146d40f230188ce`.

2026-09-14 tenth full extension link result: the frozen `5358cf19...` build
completes compilation and fails at the WebKit shared-library link with 37
missing symbols. Navigation event dispatch is resolved; the two newly enabled
frame-query receiver methods remain unresolved in that frozen build. It predates
the browser registry, controller host and native tab/frame implementations.
Evidence: `.vm/modern-extensions-navigation-api-build-result.json`.
The feature-enabled engine has not linked or run an extension.

2026-09-14 native extension tabs and frames: context lookups now resolve actual
registered WebKit pages and windows, retain per-extension identifiers, prune
stale wrappers and enforce private access. Frame queries use the process frame
tree and recheck permissions before replying. **Eight final native units
compile**. Tab/window mutation and event APIs remain unfinished; no wrapper,
frame query, IPC or extension runtime was executed. See [tab/frame evidence and
limits](webextensions-native-tabs-and-frames.md).
Patch: `e9e643ec3a02d6b3f73f386db7b4a5e05eda99c45832c506dc1a34ce6db43502`.

2026-09-15 (local) eleventh full extension link result: the frozen `e9e643ec...`
build completes compilation and fails at the WebKit shared-library link with
29 missing symbols, down from 37. All eight targeted controller, tab/window
lookup, metadata and navigation-query symbols are resolved, with no new missing
symbols. Evidence: `.vm/modern-extensions-native-tabs-build-result.json`.
The feature-enabled engine has not linked or run an extension.

2026-09-15 (local) request-event bindings: the WebRequest namespace and event
interface now use common C++, including six process notification handlers,
request filters and upload-body conversion. **125 native helper checks pass**
with an explicitly isolated API initialization adapter; **nine final integration
units compile**. Native UI event forwarding, blocking requests, full compatibility
and actual extension runtime remain unfinished. See [request-event evidence and
limits](webextensions-native-web-request.md).
Patch: `c9587d431175525e047288be50e42c7b97fc365ad93831a1cf9e3c6a78073a71`.

2026-09-15 (local) twelfth full extension link result: the frozen `c9587d43...`
build completes compilation and fails at the WebKit shared-library link with
23 missing symbols, down from 29. All six request notification symbols are
resolved, with no new missing symbols. This build predates native UI resource
forwarding. Evidence: `.vm/modern-extensions-web-request-build-result.json`.
No feature-enabled engine link or extension runtime passed.

2026-09-15 (local) native resource forwarding: loader requests now record actual
loaded extension contexts, and page/controller/context notifications reach the
common request-event messages. Queued wake-up callbacks recheck live tab and
permission state before sending data. **Four native units compile**; no resource
notification, permission race, network request or extension runtime was executed.
Native CORS permission prompts remain unfinished. See [forwarding evidence and
limits](webextensions-native-web-request-forwarding.md).
Patch: `e9fa1eff70595ba5e03552f0d8fd811084b56861e630853866ccce3e2361f756`.

2026-09-15 (local) thirteenth full extension link result: the frozen `e9fa1eff...`
build compiles the native resource forwarding and reaches the WebKit library
link. It fails with the same 23 missing symbols; no new missing symbols appear.
Evidence: `.vm/modern-extensions-web-request-ui-build-result.json`.
The feature-enabled engine has not linked or run an extension.

2026-09-15 (local) request metadata: status lines now include the actual HTTP
version, status code and reason. Resource IPC carries main-frame identity
captured before background wake-up, allowing a known main-frame parent to map
to zero. **147 native helper checks pass and five integration units compile**.
No live frame lookup, navigation race, request event or IPC was executed.
See [metadata evidence and limits](webextensions-native-web-request-metadata.md).
Patch: `d577d67362ab609beaa6d87d67938b3f65eb3476190953d9c2af0313aff0882d`.

2026-09-15 (local) forwarding source correction: removed two leftover Cocoa
content-rule callback definitions that conflicted with the common C++ port.
A source audit finds exactly one definition of all 13 moved methods. This is
not a Cocoa compile result and does not alter Haiku-compiled sources.
Patch: `a2a94c217a4b41946fcaf638ff43a297e4a23723b6ab92c148fa88d0f20ae82b`.

2026-09-15 (local) fourteenth full extension link result: the frozen `d577d673...`
build regenerates the resource IPC and compiles the metadata change. The WebKit
library link fails with the same 23 missing symbols, with none newly introduced.
Evidence: `.vm/modern-extensions-web-request-metadata-build-result.json`.
The later Cocoa-only duplicate removal does not affect this build's native
compiled sources. No feature-enabled engine link or extension runtime passed.

2026-09-15 (local) native permission changes: named grants/revocations now update
extension processes and clipboard preferences; host-pattern changes update CORS,
injected content and tab metadata. Delayed events check the current load and tab
identity, and tab payloads read permissions at delivery. A native C++ host
observer interface is available but not yet wired into the SDK or extension UI.
**Four native units compile**; no permission or observer runtime was executed.
See [permission-change evidence and limits](webextensions-native-permission-changes.md).
Patch: `61626a3e1a2fcb8cf9b87008c6543921fa3b338666873e55df51254c11dbeaf5`.

2026-09-15 (local) native action state: default/window/tab actions now manage
labels, badges, icons, enabled state and popup paths, with an observer for
affected scopes. Clearing customizations resets unread state; blocked-resource
counts use bounded arithmetic. **Four final native units compile** after fixing
a reference-deduction error in the first context-action probe. No action or
observer runtime was executed. SDK/toolbar integration, clicks, popups and the
JavaScript action API remain unfinished. See [action-state evidence and
limits](webextensions-native-action-state.md).
Patch: `dc4675d8cdc40b87b0e5134c6d09fb58c5f2dfc2880d968d3bc98f8dd2282475`.

2026-09-15 (local) fifteenth full extension link result: the frozen `61626a3e...`
build compiles native permission changes and reaches the WebKit library link.
The two permission callbacks resolve, reducing missing symbols from 23 to 21,
with no new missing symbols. Evidence:
`.vm/modern-extensions-native-permissions-build-result.json`.
This build predates the native action-state patch. No feature-enabled engine
link or extension runtime passed.

2026-09-15 (local) native message-port lifecycle: host callbacks receive validated
JSON through an ordered, bounded queue. Disconnects carry errors and context
unload detaches ports before background-page closure. Common disconnect delivery
now selects actual channel-owning pages, including ports without disconnect
listeners. **57 native queue checks and 31 ownership checks pass; seven final
integration units compile.** No native port/context, unload callback, JavaScript
error/disconnect, IPC or executable-host runtime was executed. The native host
SDK and runtime connection path remain unfinished. See [message-port evidence
and limits](webextensions-native-message-ports.md).
Patch: `425e5c88a7cb5b221858163374430798efa40a166384bcde3f085be904e3df50`.

2026-09-15 (local) sixteenth full extension link result: the frozen `425e5c88...`
build compiles native action state and message-port handling, regenerates IPC
and reaches the WebKit library link. The two action symbols and message-port
disconnect symbol resolve, reducing missing symbols from 21 to 18, with none
newly introduced. Evidence: `.vm/modern-extensions-native-port-build-result.json`.
The feature-enabled library has not linked and no extension has run.

2026-09-15 (local) native package identity and install state: filesystem packages
now load from owned, fingerprinted resource snapshots. In-memory resources copy
caller-owned data before hashing. Context load uses saved version/content identity
to distinguish first install, update and reenable; a consumed host-supplied purpose
controls startup/browser-update/private behavior. Invalidated package state clears
listener/ruleset caches and requests registered-script removal. **64 native helper
checks pass and six final integration units compile.** No WebExtension constructor,
context, lifecycle event, database removal or browser runtime was executed. The
browser SDK must still wire startup/update intent. See [package identity evidence
and limits](webextensions-native-package-identity.md).
Patch: `341d7bd1bb1d8d5601a45cd83b1dc5af3eeb3b2f4039f7468d1b212ee32d6ef5`.

2026-09-15 (local) seventeenth full extension link result: the frozen `341d7bd1...`
package-snapshot build reaches the WebKit library link. Native install-state
selection now resolves, reducing missing symbols from 18 to 17, with none newly
introduced. The remaining symbols are the DNR loader and 16 process-side event
handlers. Evidence: `.vm/modern-extensions-package-snapshot-build-result.json`.
The feature-enabled library has not linked and no extension has run.

2026-09-15 (local) native DNR URL-filter foundation: C++ translation now handles
URL-pattern anchors, wildcards, end-of-URL separators, case sensitivity and
credential-safe authority matching. Unsupported raw-regex escapes return errors.
A shared WebCore parser fix preserves anchored nullable expressions instead of
making their actions unconditional. **120 native checks and 6,608 independent
matching comparisons pass; two integration units compile.** Tests use the actual
content-filter parser, NFA/DFA compiler and bytecode interpreter. This helper is
not yet connected to ruleset loading or browser enforcement; the DNR loader and
16 process-side event handlers still need implementation. See [DNR URL-filter
evidence and limits](webextensions-native-dnr-url-filters.md).
Patch: `4777dc25ab9f24d84579c57fb0a19a258a4c6e536725eb70d9f8fa1f899fb90e`.

2026-09-15 (local) eighteenth full extension link result: patch `4777dc25...`
compiles the updated shared filter parser, rebuilds `libWebCore.a`, and compiles
the native DNR URL helper. The WebKit shared-library link still fails on the
same 17 symbols as the package-snapshot build, with no newly introduced missing
symbols. Evidence: `.vm/modern-extensions-dnr-url-filter-build-result.json`.
No extension runtime was executed.

2026-09-15 (local) compound DNR URL conditions: the shared WebCore compiler and
backend can combine independent request, top-page and frame inclusions and
exclusions. URL alternatives share one action identity, and exclusions retain
unrelated rules. Legacy method-bearing conditions and the first empty-URL cache
lookup are corrected; compiled rule caches advance to version 22. **76 native
checks and 4,032 condition comparisons pass; eight integration units compile.**
The harness uses actual filter bytecode but does not execute the full parser,
cache, actions, extension loader or browser enforcement. See [compound condition
evidence and limits](webextensions-native-dnr-conditions.md).
Patch: `656209ed1a5461930864b0bc5ee25d29edce7c026f34754e2f4397797a5d16f0`.

2026-09-15 (local) nineteenth full extension link result: the compound-condition
patch rebuilds WebCore successfully. WebKit reaches its shared-library link and
fails on the same 17 unresolved symbols, with none newly introduced. Evidence:
`.vm/modern-extensions-dnr-conditions-build-result.json`. A separate executable
linked the rebuilt WebCore archive and tested its actual rule-list pipeline:
84 checks passed and two universal/specific overlap cases exposed duplicate
actions. No WebKit extension or browser runtime was executed by that test.

2026-09-15 (local) actual content-rule pipeline: the backend now deduplicates
serialized action locations when merging universal and URL-specific matches.
The unchanged native pipeline fixture passes **all 86 checks**, including the
two previously failing overlaps. It also verifies real parsing, compilation,
URL caches, action deserialization and a header change on `ResourceRequest`.
Distinct conditional rules remain separate. No native crashes or input/library
changes were detected. Evidence: `.vm/content-rule-pipeline.rzdrhT4E/result.json`.
The twentieth full extension link attempt rebuilt WebCore and still failed on
the same 17 WebKit symbols, with none new. No extension has run in Summit.
Patch: `dbfa7e97e15983e2e645a9b200e91472f3213239d3b7ae0157463e6ef1f2a7cc`.

2026-09-15 (local) native extension commands: `getAll`, Firefox-style `update`
and `reset`, persisted shortcut overrides, native command models and both
command event dispatchers are implemented. Ctrl maps to native Control;
shortcut validation, clearing, reset isolation and MV3 action migration share
helpers. Deferred events recheck the context's load and command identity;
command tab parameters are generated after liveness/access checks. The native
namespace allows an empty command query, with privileged IPC validation.

**161 native helper checks and 92 binding platform-guard checks pass.** Twelve
distinct native integration units compile with their source hashes audited.
The binding generator now supports platform guards on operations; commands is
the **15th of 37 extension interfaces** using C++ bindings. The full build of
patch `4665ca569eb1404566c3f87c095450f66ac374f3226af0fa38c1746b364cd855`
reached the WebKit library link and failed with **15 missing symbols, down from
17**: both command dispatchers are resolved, with no new missing symbols.

Keyboard routing, action invocation/popup UI and the extension SDK still need
integration. No command listener, promise, context state write or physical
shortcut has run in an extension. No extension-enabled browser build is yet
available. See [the command implementation and evidence](webextensions-native-commands.md).

2026-09-15 (local) native extension consent: added a controller-owned prompt
broker, public SDK listener/reply/cancellation methods, and Summit's native
permission dialog. Requests use opaque UUIDs, a bounded FIFO queue and deadlines;
unload cancels the old load's requests. The dialog lists every requested
permission/site, labels private profiles, sanitizes extension-supplied display
text, defaults to Deny, and closes safely on cancellation or application exit.

**165 native broker/UI checks pass**, with no crash-log events and unchanged
source/library hashes. The SDK/controller/context integration sources compile;
all six modern application sources and the two changed legacy application
sources compile. The actual SDK/profile sources also compile with WebExtensions
disabled. These checks exercise the production broker and native windows;
the runtime helper does not call the SDK wrapper or run a loaded extension.

At this consent checkpoint, permission API bindings, connecting
`permissions.request()` to the broker, manifest revalidation, and applying/
persisting grants were still unfinished. See
[the consent implementation and verification scope](webextensions-native-permission-prompts.md).

The full feature-enabled build of patch
`7bdd8bfd20284706a60fc1abf37b82341f6c843500de9c99607af82f138384fe`
reaches the WebKit library link with **the same 15 missing symbols** as the
preceding command port. There are no new missing symbols or compiler errors.
The extension-enabled browser still does not link, and no extension has run.

2026-09-15 (local) native permissions API: implemented `getAll`, `contains`,
`request`, `remove`, and the actual permission event dispatcher. Requests use
privileged IPC, manifest validation and the native consent broker. Allowed
grants are saved before becoming active; revoked or expired permissions cannot
be restored by a late prompt response. State restoration respects current
declarations and pre-load host changes, and stops on malformed saved state.

**90 native parser/state/atomic-writer checks pass**, with no crash-log events
and unchanged inputs. Nine additional native integration units compile, and
120 generated-binding preprocessing checks pass. Sixteen of 37 interfaces now
generate C++ bindings. These are separate helper/compile checks; they do not
execute permission APIs inside a browser. See
[the permissions implementation and limitations](webextensions-native-permissions.md).

The reviewed patch is
`cf7f9b4e1f7360a5fc273bbc97724e33c56fe7494bea227e3287a4a5474a3314`.
Its full extension-enabled build reaches the WebKit link with **14 missing
symbols (18 references), down from 15**. The permissions dispatcher is resolved;
there are no new missing symbols or compiler errors. Both native source
manifests match the patch, with the feature build enabled and the separate
baseline disabled. Full extension support remains unfinished, and no extension
has run. Evidence: `.vm/modern-extensions-permissions-build-result.json`.

2026-09-15 (local) native tab reads/events: implemented `tabs.get`,
`tabs.getCurrent`, `tabs.query`, and the nine upstream lifecycle event
dispatchers. Reads use the native registry, private-data checks and separate
metadata permission checks. The current-tab method excludes background and
unassociated extension views. Events reuse the existing JavaScriptCore tab
converter and protect arguments separately for each listener.

**136 native query/converter checks pass**, with no crash-log events and
unchanged input/library hashes. Nine native integration units compile, and
252 generated-binding checks pass. Seventeen of 37 interfaces now generate
C++ bindings. These checks do not execute tab API IPC or browser events.
Native mutation, navigation, capture, script and messaging methods remain
unported. See [the native tabs API scope](webextensions-native-tabs-api.md).

Patch `f0952d6df18433d24eff8f132fc8bc1cd5f66c860936e00ff74aa3c644515db6`
is promoted. Its full extension-enabled build reached linking with **5 missing
symbols (9 references), down from 14**. All nine tab dispatchers are resolved,
with no new missing symbols or compiler errors. The two native source/config
checks match this patch. Evidence: `.vm/modern-extensions-tab-api-build-result.json`.
Full extension support remains unfinished, and no extension has run.

2026-09-15 (local) native windows reads and lifecycle notifications: added `windows.get`,
`getCurrent`, `getLastFocused`, `getAll`, identifier constants and filtered
created/removed/focus dispatchers. A native registry observer now forwards
committed window transitions to loaded contexts; it preserves removed window
IDs, rejects invalid/no-op notifications, and checks the load identity and
private-data access again after waking background content. The native tab
lifecycle notification route remains unfinished.

The window query/JavaScriptCore helper passes **111 checks** and the extended
native registry helper passes **594 checks**, both with unchanged input/library
hashes and no native crash-log events. Nineteen of 37 interfaces generate C++
bindings; **318 binding checks pass**. **Seventeen native integration units compile** against the combined
candidate, with source/header hashes audited. State/geometry and window mutation APIs remain unavailable. See
[the native windows API scope](webextensions-native-windows-api.md).

Windows patch `6730e1ed2e25f7c9f9a8071634aed7e8dee6e4f374fce052230b745f627b155e`
is promoted. Its full extension-enabled build reached the WebKit link with
**four missing symbols (eight references), down from five**. The windows
dispatcher is resolved, with no new missing symbols or compiler errors. Both
native source/configuration checks match the patch. Evidence:
`.vm/modern-extensions-window-api-build-result.json`.
No extension has executed these APIs or received these native events in a
browser; the full-browser objective remains unfinished.

2026-09-15 (local) native action API: added title, badge text,
popup-path state, enable/disable, enabled-state reads and click-event dispatch.
Receivers validate the loaded privileged context and live accessible targets;
setters use the existing native action model. Raw JavaScript tab IDs preserve
numeric arguments, and dictionary conversion retains null resets separately
from empty overrides. SDK/toolbar clicks, temporary activeTab grants, native
icon/color APIs and popup presentation remain unfinished.

The isolated native parser/JavaScriptCore helper passes **125 checks**, with
unchanged source/library inputs and no native crash-log events. **Seven native
integration units compile** and **393 generated-binding checks pass**; 20 of
37 interfaces now generate C++ bindings. Source/header hashes
match the tested inputs. These checks do not execute action API IPC or extension
runtime. See [the action API scope and evidence](webextensions-native-action-api.md).
Patch `4b8e914e8509911d780d93e72bae8ccff5d56dd97b51da470dd09ec711bae201`
is promoted; its full extension-enabled engine build reached the WebKit link.
The action dispatcher is resolved, but its `LocalFrame::document()` call lacks
the header containing that inline definition. The result remains **four missing
symbols (eight references)**: this new inline dependency plus the existing DNR
loader, menus dispatcher and cookies dispatcher. There are no compiler errors;
the inline-definition warning explains the new link failure. Evidence:
`.vm/modern-extensions-action-api-build-result.json`.

The missing inline header is corrected in follow-up patch
`71a2c16ef1345fb2225b97bd8017adc654ab4aa923ae759785d67eb8f074dad5`.
The isolated Action source compiles without warnings using regenerated IPC,
and object-symbol inspection confirms the inline dependency is resolved. Its
full engine build reaches linking with **three missing symbols (seven references)**,
no new missing symbols and no compiler errors. The remaining symbols are the
DNR loader, menu-click dispatcher and cookie-change dispatcher. The Action
dispatcher and its inline dependency are resolved. Both native source trees
match the promoted patch; extension-enabled engine linking and extension
runtime remain unfinished. Evidence: `.vm/extension-action-inline-validation.json`
and `.vm/modern-extensions-action-inline-build-result.json`.

2026-09-15 (local) native cookie API and events: patch
`64e366546a2089731d72ba13c1681809f4db6563e83f00c64b5712f3f760ece1`
connects `get`, `getAll`, `set`, `remove`, `getAllCookieStores` and populated
`onChanged` payloads. The Curl backend preserves cookie metadata, migrates its
SQLite schema transactionally, reports actual mutation results and publishes
ordered committed change batches. A native run-loop timer expires persistent
cookies while observed. Typed IPC carries those batches through the network
cookie manager, owning UI data store and permission-filtered extension context
to fresh JavaScript listener values. Observers are reinstalled when a network
process is recreated. Those cross-process and lifecycle paths have compiled;
they have not run.

The isolated native checks pass **227 database/parser/event/IPC checks**,
**136 JSC/query/changeInfo checks** and **40 host-grant checks**, with unchanged
inputs/libraries and no native crash-log events. The IPC test uses production
codecs in one process; it does not execute NetworkProcess or extension
receivers. **24 unique native integration units compile** and **431 binding
checks pass** across Haiku/Cocoa surfaces. The source audit matches all 48
integrated files. Cocoa SDK compilation remains untested.

Both native source trees match the promoted patch. Its full Extensions-enabled
build has completed WebCore and WebKit compilation and reached linking with
**two missing symbols (six references)**: the DNR loader and menu-click
dispatcher. The typed cookie dispatcher is defined and resolves. There are no
new missing symbols or compiler errors; five existing `#import` warnings come
from `WebExtensionCommand.cpp`. No extension has executed an API or
received an event. Partitioned cookies, Firefox container/first-party isolation,
eviction, network SameSite enforcement and full extension/browser compatibility
remain unfinished. See [the cookie API port](webextensions-native-cookie-api.md)
and [event verification](webextensions-native-cookie-changes.md). Current evidence:
`.vm/extension-cookie-delivery-validation.json`,
`.vm/extension-cookie-api-promotion.json`, and
`.vm/modern-extensions-cookie-delivery-build-result.json`.


### Native menu API and callback ownership

Engine patch `e4ad8a2c8983a8606a261b1d54935ad334cdbaec4dbed55e4c21fa4bc37ce331`
connects native `menus` / `contextMenus` bindings, typed IPC, model operations and
menu-click dispatch. Both aliases use main-world permission gates. Create returns
a typed ID synchronously; update/remove/removeAll support optional promises.
Per-item handlers use unique tokens across renames and identifier reuse, register
before requests, and clear across processes on removal or replacement. Document
stop/destruction and extension-load changes clear retained callbacks while
listener removals remain tied to the original load.

**169 native helper checks, 501 binding checks and 13 connected translation
units pass.** The 25-file source audit matches the integrated patch. Native CMake
configuration succeeds. The full Extensions-enabled build compiled all sources
and resolved the menu dispatcher, then failed to link on **three missing native
functions (seven references)**: DNR rule loading, command dispatch and user-gesture
handling. No extension has run and no native extension menu has
been displayed. Model/callback lifecycle runtime, trusted native click routing,
activeTab access and background menu persistence remain unfinished. See the
[menu port](webextensions-native-menu-items.md) for the tested scope and evidence.

Patch `ee89662671ea95fac79949f6ef426a052d9939f96be7baf39d89d2f46a78dfb6`
corrects click-info construction so inherited JavaScript setters cannot swallow
event fields or run during native conversion. JSC's native JSON parser creates
own data properties and ignores replacements of global JSON functions. Two
regression failures were reproduced against the preceding patch; **181 native
checks pass** with the correction, and the changed helper compiles with Extensions
enabled. The incremental full-target build compiles the changed helper without
warnings and reaches the same three missing functions/seven references. The current 25-file source audit
combines the original menu evidence and the one-file correction; all current
native sources and generated target membership match. Evidence:
`.vm/extension-menu-current-validation.json` and
`.vm/modern-extensions-menu-own-properties-build-result.json`.

The live QEMU desktop is available through VNC on host port **5905**. A visible
Terminal now shows ongoing build output and milestones; see [VM access](VM.md).


### Native temporary tab access and command routing

Patch `ccacaaa8748dde7ba4432cd53707f5731332951c7b8d727f82d0d45599ff3b5d`
connects user gestures to per-tab grants from the committed security origin,
with exact scheme/host/port scope, private/file access gates, denial handling,
navigation and lifecycle revocation. Temporary grants bypass global permission
notifications. Native command dispatch retains a menu's clicked tab and rejects
inaccessible frontmost windows. Action commands reach the action entry point;
its native popup implementation and the DNR loader remain unfinished.

**61 native helper checks and nine connected translation units pass.** The
first full build exposed the missing `haiku/` prefix on the shared tab header's
new include. The corrected full build compiled without errors and reached the
linker: request-rule loading and `performAction` remain undefined (seven references).
No extension or trusted native input has executed this path. Automatic injection,
fetch/CORS behavior and actual asynchronous lifecycle checks remain unverified
or unfinished. See [temporary tab access](webextensions-native-active-tab.md)
for the implemented behavior, test scope and evidence.


### Native extension action popup host and page bridge

Patch `4bd2ca81e2ab0c0e2d5a10f9e776e0bf41cd04e392427c57961edf6c6775ba4b`
adds a floating native action popup with an extension-configured WebKit page,
content sizing, originating-tab lookup and lifecycle dismissal. New action
invocations supersede earlier popups in the same profile. Trusted calls validate
the current tab/action before granting the existing user gesture; actions without
a popup use the existing click-event producer.

**88 native window-host checks and ten engine translation units pass.** The full
Extensions-enabled build reached the linker without compiler errors; only the
DNR loader remained undefined, with five references. No extension HTML has rendered in
this host and no extension has executed the action path. Native toolbar input,
`action.openPopup()` bindings, real lifecycle/rendering tests and request-rule
loading still require work. See [native action popups](webextensions-native-action-popup.md)
for the exact evidence and remaining scope.


### Native declarative request-rule loader

Patch `c3ff5b56144df26ae0b6d85df42c1ca453867d6a77f3fefa8a87966cc092519b`
integrates session, dynamic and enabled static ruleset loading with the real
content-rule store. The initial translator supports URL/resource conditions and
priority-ordered `block`/`allow` rules. Unsupported semantics reject the ruleset;
host-access-only DNR and broader actions/conditions still require integration.
Generation checks and unique compiled paths prevent obsolete loads from
installing or deleting a newer list. Unload and permission revocation clear it.

**95 real WebCore pipeline checks and seven affected translation units pass.**
All ten originally promoted source files match that candidate. The preceding
build's only undefined function was this loader. DNR JavaScript bindings remain disabled pending transaction
error fixes, lifecycle tests and broader compatibility work. See
[native request-rule loading](webextensions-native-dnr-loader.md) for evidence
and the remaining scope.

The first combined DNR build exposed a missing `API::ContentRuleList` forward
declaration and was stopped after those compiler failures, before linking.
Correction patch `c8b960d698b09f68d906606c00f4ac52e7a2d14fda74a42b2bb7ac49037fb81b`
passes all three affected units and completes the first full Extensions-enabled
engine link: `libWebKit`, `WebProcess` and `NetworkProcess`. The successful build
has no compiler errors or undefined symbols. This does not establish extension
execution or compatibility.

### Persistent rule-store runtime and first extension startup attempt

The first real WebKit store run passed 108 checks and exposed three failures
caused by temporary files left after invalid regular expressions. Patch
`284266a9d79e372f7c617cbf1e6694b6d309035c9b725b850197da34e5a49011`
cleans up those partial files. Its full rebuild links and **all 111 store checks
pass**, including main-loop callbacks/capture destruction, persistence,
replacement, lookup, enumeration, deletion and outstanding-work ownership.
Sources and linked libraries are unchanged during the test, with no debugger
events. Evidence: `.vm/content-rule-pipeline.OnYVmSd0/runtime-result.json`.

The actual extension fixture accepts its package and native context, grants its
isolated storage permission and launches both helper processes. It then stalls
with a pending background URL, before a provisional URL or title is reported.
The first two attempts time out without JavaScript test reports, then cleanly
tear down the owned processes.

A subsequent run loads an ordinary offscreen page first and then successfully
executes both extension rounds: **15 native checks and ten JavaScript assertions
pass**, including `browser`/`chrome` bindings, storage promises and callbacks,
native test-message IPC, and storage after context recreation. Evidence:
`.vm/content-rule-pipeline.ZgViMefk/result.json`. There are no debugger events,
changed inputs/binaries or remaining helper processes. Direct and deferred
startup reproduce the stall at that patch. IPC tracing identifies a cookie-observer
dispatch-flag mismatch and a lock deadlock in invalid-message rejection.

Correction patch
`96ce44b447dd95dc56a01cbe6b016043500cbdf8b984fb4f6f77a79b392da1bb`
completes the full engine rebuild without compiler errors or undefined symbols.
The real IPC rejection/continued-traffic regression passes **23 checks** in
`.vm/content-rule-pipeline.XFrD2pQI/runtime-result.json`. Direct extension startup
now passes **14 native checks and ten JavaScript assertions**, with two matching
completion messages and two success reports, in
`.vm/content-rule-pipeline.ZJOxn6oV/runtime-result.json`. No ordinary page is
loaded first. Both runs have unchanged inputs/binaries, no debugger events and
clean process-group teardown.

The new real cookie-observer lifecycle test passes **13 checks and fails one**:
initial delivery works, but a confirmed cookie write after repeated observer
unregister/register produces no event. The final unregister-only phase is not
reached. Evidence: `.vm/content-rule-pipeline.F0ocC1xc/runtime-result.json`.
Registration uses the priority IPC queue and removal remains ordinary, allowing
later registrations to overtake removals. Patch `b42d85ea...` gives removal the
same dispatch priority. The full engine links, **all 21 observer checks pass**,
and the IPC and cold extension regressions also pass. This fixture does not test
JavaScript cookie listeners.
See [native runtime evidence](webextensions-native-runtime.md) for the exact
scope and next diagnostic boundary. Build/test milestones remain visible over
[VNC](VM.md).

The full browser bundler now accepts `--modern-extensions`, selects the separate
ON/ON engine tree, and includes the locked libzip library and its license notice.
It records both extension configuration flags and keeps the ordinary modern
bundle report separate. All **six full-browser native compilation units pass**
against the current public headers, including the extension permission prompt;
this initial check was compile-only. The existing
browser and preview bundle reports still pass copier manifest validation.
The subsequent native build links and freezes the actual extension-enabled full
browser as `bundle-9jahww_j`, with private WebKit/JSC/ICU/libzip libraries and
matching sources. Its host manifest validation passes, and the actual browser
passes **90 navigation checks** with isolated HTTP/JavaScript fixtures covering
successful loads, dropped/truncated responses, retry, HTTP errors, cancellation,
same-document navigation and history. Both the browser and harness exit normally,
the bundle remains unchanged and no debugger event occurs. Evidence:
`.vm/modern-load-errors-e805a3d821fb3354dbf995d0/result.json`.
Evidence: `.vm/modern-extensions-browser-bundle.json`,
`.vm/modern-extensions-browser-bundle-check.json` and
`.vm/modern-extensions-browser-compile-check.json`. See
[bundle instructions](modern-bundle-copy.md).

### Extension cookie APIs and incremental IPC generation

Patch `32b97d3a...` corrects IPC metadata for declarations whose dispatch flags
differ by platform, and preserves unchanged generated-file timestamps. All nine
host generator tests pass. The native engine rebuild completes in **seven steps**;
only `MessageNames.cpp` changes among 377 generated files. WebKit and both helper
executables link, and Ninja confirms no pending work. See
[generation evidence](ipc-message-generation.md).

The actual extension cookie package passes **14 native checks and 28 JavaScript
assertions** across two cold-start contexts. It verifies promise and Chrome
callback cookie APIs, secure HttpOnly fields, ordered overwrite and deletion
events, listener removal, and cookies surviving extension context recreation.
The storage package also passes **14 native checks and ten JavaScript assertions**
with the updated runner. Both have unchanged source/package/library/helper hashes,
clean crash-log intervals, and clean helper-process teardown. Evidence:
`.vm/content-rule-pipeline.CmGvDVb2/result.json` and
`.vm/content-rule-pipeline.kvjSYIF4/result.json`.

Browser extension installation, store packages and signatures, MV3 workers,
action UI, DNR enforcement and full Safari/Chrome/Firefox compatibility remain
unfinished. The public SDK still needs extension preparation/load/unload APIs
before the browser can install packages through its native interface.

## Application extension startup and native process exit

The modern extension-enabled browser now reads the installed catalog and
loads enabled, unchanged packages using saved WebKit approval. Native startup
verification passes across a setup process and two separate full-browser
processes: permissions and storage survive, background boot counters advance
1/2/3, disabled packages are skipped, changed packages require approval, and
all owned process groups drain without forced cleanup or debugger events.
The catalog now preserves the uppercase hexadecimal fingerprints emitted by
the SDK; 57 host and 57 native catalog checks pass.

A reproduced UI-process exit race also has a verified correction: after normal
application cleanup, the modern entry point uses the same final process-exit
behavior as the Haiku WebKit helpers, avoiding libbe static teardown while
worker TLS destructors finish. Five consecutive navigation runs (90 checks
each) and 143 native close checks pass with clean native log intervals.

The current verified guest bundle is `bundle-w5i95j6u`, with unchanged engine
patch `2972cc15e98b23f6dd04186631cf3f4370fe3a7d3a573175c6fd126a3fbdd3ca`.
[Startup behavior and evidence](modern-extension-startup.md) lists the exact
results, preserved failures, repeat command and remaining work. There is still
no native extension installation/management UI or broad Safari/Chrome/Firefox
compatibility claim. The original full-browser objective remains unfinished.

The frozen bundle and matching engine sources are now copied to
`artifacts/modern-browser/bundle-w5i95j6u`. Copy provenance SHA-256 is
`bd7049ffaefe54669d8a8c8514799a8cfa052d2aa2eb0f0b2cde126f9c4a71d9`;
the patched source archive SHA-256 is
`47cfdc4589a682fe7d987d0f177b6276b55e62b5b7d0f591bb02b4ee35e70c0a`.
Source and build-support fingerprints match before and after copying, and the
copied native report and executable digests match the tested guest bundle.
Evidence: `.vm/modern-extension-startup-bundle-copy.log` and the artifact's
`copy-provenance.json`.

## Native extension installer and manager

**Window → Extensions…** now opens a native manager with a real file picker,
permission review, installation and enable/disable/remove controls. Packages
are staged on a worker and prepared through the public SDK; generation and
fingerprint checks bind approval to the reviewed snapshot. Optional grants,
file access and private access stay off by default. Removal unloads and forgets
the startup record while explicitly retaining package files and saved data.

The actual picker and lifecycle test passes **123 checks** on
`bundle-tz1tfyzh`. A folder installation and a subsequent XPI installation run
both preserve approved grants and storage across distinct browser processes,
reject duplicate identities, and remove the extension from later startup.
Cancellation, stale approvals, closing during import, and quitting with the
manager/file picker still open pass. Browser/helper groups drain normally,
native crash-log intervals are clean, and all frozen/staged/host input hashes
stay unchanged. Both portable test suites also pass.

See [installer behavior and exact evidence](modern-extension-manager.md).
Package signatures and store authentication, updates, retained-data erasure,
Chrome key identity, CRX/Safari bundles, action UI and broad API/extension corpus
compatibility remain unfinished. Engine patch `2972cc15e98b23f6dd04186631cf3f4370fe3a7d3a573175c6fd126a3fbdd3ca`
is unchanged for this application integration.

The native manager bundle `bundle-tz1tfyzh` and matching engine sources are now
available in `artifacts/modern-browser/bundle-tz1tfyzh`. The successful copy's
provenance digest is
`b034304aa77ca4c250ad718c0b4dedd3e014ed81823ed8a93ed7b7c869b0bb3e`;
its source archive digest is
`47cfdc4589a682fe7d987d0f177b6276b55e62b5b7d0f591bb02b4ee35e70c0a`.
Source/build-support fingerprints and executable digests match. The earlier
copy attempt was discarded because the host Git revision changed during the
copy; `.vm/modern-extension-manager-committed-bundle-copy.log` records the
successful retry at revision `858221f`.

## Chrome manifest-key identity

Chrome packages with a manifest `key` and no declared Gecko ID now receive
Chromium's expected ID. The installer derives it with OpenSSL, rejects malformed
keys and persists it in the installation record. Existing Gecko and local
identity selection remains supported. Mixed Gecko/key manifests currently keep
Gecko precedence; explicit import provenance/format selection remains work.

All **29 identity checks** pass on Linux and native Haiku against independent
Chromium test vectors. On the new frozen browser `bundle-mo8zo6d4`, the keyed
package passes **123 native picker and lifecycle checks** with actual
`chrome.runtime.id`, restored grants and persisted storage across browser
processes. The Gecko-ID regression passes another 123 checks on that bundle.
Both runs have clean exits, drained groups, no native debugger events and
unchanged inputs. All three portable CTest suites pass. See
[identity semantics, exact results and limitations](extension-identity.md).

This is identity compatibility; CRX/signature validation, authenticated
updates, URL-scheme aliases and broad Chrome API compatibility remain
unfinished. Engine patch `2972cc15e98b23f6dd04186631cf3f4370fe3a7d3a573175c6fd126a3fbdd3ca`
is unchanged.

The verified identity-enabled bundle is copied to
`artifacts/modern-browser/bundle-mo8zo6d4`, with provenance SHA-256
`f0fc447700311f16763ae5f28f85b3f4fc8175b0a7b715c516da16dbe2cd6fbd`
and engine source archive SHA-256
`47cfdc4589a682fe7d987d0f177b6276b55e62b5b7d0f591bb02b4ee35e70c0a`.
The native report, executables, and before/after source/support fingerprints
match; the copy records commit `9c67f33`. Evidence:
`.vm/modern-chrome-identity-bundle-copy.log`. A separate isolated native build
of the system-WebKit Makefile target links successfully with OpenSSL and
passes its 38 core checks. This does not claim extension support in that
legacy backend.

## Public action SDK, native toolbar and real popup execution

Engine patch `063c724e002819e42a378056225eced55802f309254caf6640bed79c7534469e`
connects the existing action state and popup host to public snapshot,
invalidation and trusted activation APIs. Summit displays action icons, titles,
badges and enabled state with tab/load validation. The full feature-enabled
engine and browser bundle `bundle-p214ja9w` build successfully.

The Gecko-ID fixture passes 202 native checks. The Chrome-key fixture passes
217 native UI checks plus 29 identity checks across four actual browser
processes, including application shutdown with a popup open. These tests install
through the real picker and consent UI, execute real extension popup HTML/JS,
verify shared saved storage, click a DOM control, dispatch action click events
with the selected tab, preserve tab-specific overrides, reject stale page/load
activation and remove an extension with its popup open. All owned groups drain
normally; crash intervals and frozen-input checks pass. A QEMU screenshot shows
the actual rendered popup. Exact paths, commands, SDK contract and remaining
coverage are in [native extension actions](modern-extension-actions.md).

This supersedes earlier notes that the toolbar/popup path had only compile or
native-host evidence. Direct button anchoring, remaining action APIs,
activeTab grant transitions, private/multi-window behavior and the broader real
extension corpus remain unfinished. The original full-browser goal is active.

The tested toolbar/popup bundle and matching WebKit source are copied to
`artifacts/modern-browser/bundle-p214ja9w`. Copy provenance SHA-256 is
`fe620c5e42a9f5c783f3b62ec40c3854a4b99eaec38001b7acf3d9a01e2ba056`;
engine source archive SHA-256 is
`40b83599ce48235f5dc8b30dc2d743290081140a16cb3c22c976fafa64eae62e`.
The copy completed normally, with matching source/support fingerprints,
native report, binary and launcher digests. Earlier artifacts remain intact.

## Native extension overflow keyboard and lifetime fixes

Bundle `bundle-7ve4k1a6` fixes asynchronous native menu cancellation, quick
keyboard choices and focus-only action refreshes. The overflow menu keeps
Manage extensions reachable, skips disabled actions and bounds long titles.
Actual action-data changes cancel old menus, and browser shutdown wakes the
menu tracking loop before window destruction waits for it.

The six-extension fixture passes 169 native checks, including actual picker
installation, independent popup JavaScript, timed Enter/Escape behavior and
application shutdown with the menu open. The action regression passes another
217 native UI checks plus 29 identity checks. All processes exit normally,
groups drain, crash intervals remain clean and frozen-input checks pass. Exact
evidence is in [native extension actions](modern-extension-actions.md).

The engine patch remains `063c724e002819e42a378056225eced55802f309254caf6640bed79c7534469e`.
Overflow pointer input and broader compatibility coverage remain work; this
does not complete the full browser or cross-browser extension goal.

The verified bundle is copied to `artifacts/modern-browser/bundle-7ve4k1a6`,
including matching WebKit source and provenance. Provenance SHA-256 is
`1e64738d2b33617b4546e3762330bccc6de216b9efedaa1d4f0af4765cc87992`;
the unchanged engine archive digest is
`40b83599ce48235f5dc8b30dc2d743290081140a16cb3c22c976fafa64eae62e`.
The copy records host commit `954815f`. Native/host reports, executables,
launcher and source/support fingerprints match. The VNC preview uses a
separate copy of the six-extension profile after the test's clean exit.

## Native extension badge colors

The complete engine builds with patch
`23a657589a6f9ff50f14517951d8bfc880645945263a9d994118b4dabf0ea986`.
Browser bundle `bundle-lzht66nf` adds actual CSS/RGBA badge background and text
APIs, tab/window/global inheritance, automatic contrast and native alpha drawing.
The original accessor compile failure is fixed and the incremental engine build
exits zero.

The real Chrome-keyed MV2 fixture passes 238 native UI checks, 29 identity checks
and 58 JavaScript color assertions on each of four loads. Coverage includes
promise and callback bindings, invalid-input atomicity, navigation resets,
popup-driven updates and actual blue/yellow badge pixels. The overflow regression
passes 169 checks. All five browsers exit zero without forced cleanup, their
groups drain and native crash intervals remain clean. Input fingerprints match.
Another 509 binding-generation/preprocessing checks pass. Exact evidence and
compatibility limits are in [native badge colors](webextensions-native-badge-colors.md).

The verified artifact is `artifacts/modern-browser/bundle-lzht66nf/`, copied with
matching source and build support from host commit `466f4b1`. Its provenance
digest is `4892ae8ab988b1f8bc57c24256a5b293f1cfbc4df27ac2545821410b5b812b23`;
the WebKit archive digest is
`f92213370e7b2ab90373c317b9c2cfe2b8ef3525f5348de7ee69f16e402ea1dc`.
All 399 artifact entries match the recorded inventory. A separate VNC preview
profile again runs the verified extension and real popup; the test source profile
remains unchanged. Broader extension compatibility and the original full-browser
goal remain incomplete.

## Native extension localization and initial injected styles

Bundle `bundle-omqykez8` exposes five localization methods on Haiku and collects
injected author styles before the initial render-tree style resolution. This
fixes extension CSS on pages without their own stylesheets. The native suites
pass 329 localization checks, 342 programmatic popup checks and 455 icon checks.
All runs exit normally, drain their processes, retain their recorded input hashes
and cover the native crash log without debugger events.

The matching artifact is `artifacts/modern-browser/bundle-omqykez8/`, copied at
host revision `886704c`. All 399 artifact entries match, with unchanged source and
build-support fingerprints. Its provenance digest is
`2367a3fb5d5bb8b2c9f3df90b3de8daf9ae66c635072a1a0fb4911fe0ba13ef2`.
The matching WebKit archive digest is
`4c4d3311e203bb514517d6c833b29739f4e05634f43fd24e517a5b9e03b2a5be`.
The current VNC preview uses a separate profile with this bundle. See
[the localization record](webextensions-localization.md) for exact evidence,
the preceding CSS diagnosis and remaining locale/published-extension coverage.
Full Safari, Chrome and Firefox extension compatibility remains incomplete.

## Native extension script injection

The September 16 `bundle-zwmpgvis` builds and links with engine patch
`0ebce0e871f549817c218e0e8a92a4f441408201d46d3aedd2f60e66a6ace206`.
Manifest V2 extensions can execute scripts and packaged files in existing
tabs, with isolated worlds, document-bound targets, readiness waits and
permission checks. The native suite passes 113 unique JavaScript cases and
523 native checks. Published Dark Reader additionally themes an already-open
tab without reloading and retains working popup Off/On behavior.

All six native suites pass 2,026 checks with normal shutdown, drained process
groups, clean crash intervals and matching input hashes. Exact reports and
remaining compatibility work are recorded in
[native script injection](webextensions-tab-script.md). This milestone does
not complete the original full-browser or extension compatibility goal.

The script-injection bundle is exported to
`artifacts/modern-browser/bundle-zwmpgvis/`, with matching engine source and
copy provenance from host revision `1364589`. Independent validation checks
all 399 artifact entries, unchanged source/support fingerprints, and matching
native/host manifests. Exact artifact digests are in
[the copy record](modern-bundle-copy.md#native-extension-script-injection).

## Thenable script results

Bundle `o7hhvrm9`, patch
`5922459b937953f3744459c266e3cc5baf5d0240e92ef0c5c171c48fd83db54b`,
corrects Firefox-style injection results to assimilate arbitrary thenables.
The new fixture first reproduced the old bundle's uncloneable-result failure.
The full native build then passed 129 unique injection cases / 585 native
checks and 78 unmodified Dark Reader checks. Both processes shut down normally,
drained their groups and recorded clean crash intervals with unchanged inputs.

The [result compatibility investigation](webextensions-script-results.md)
also identifies package-origin handling needed for exact Chrome MV2 results;
choosing behavior from `chrome` versus `browser` alone would be insufficient.
The full browser and extension compatibility goal remains incomplete.

## Verified CRX3 import (September 16, 2026)

The modern engine now verifies CRX3 RSA/ECDSA signatures, preserves the signer
ID through worker preparation and immutable resource snapshots, and refuses
activation under a different ID. The native installer displays signature and
store-provenance status, requests consent, retains the exact CRX bytes, and
re-verifies them on startup. Changed signed resources require new approval.

The engine/browser build passes for `bundle-d0h_6xzg` with unchanged inputs
and patch `d5887e4f3e31b5c580a90cd501a9917853bfffccd32fe5a239fe6a8da6bd809c`.
Production signature/extraction tests pass 620 native checks. The new public
SDK and real browser picker/consent/restart tests pass 205 checks; unmodified
Chromium MV2 Dark Reader passes another 78 on this bundle. All integration
process groups drain normally, no native debugger event occurs, and fixture,
bundle, catalog and retained-package checks pass. The three host CTest suites
pass. [CRX3 behavior, evidence and reproduction](extension-crx.md) records the
exact reports, digests and source references.

Published uBlock Origin 1.74.0 CRX signature, all 779 resources and public SDK
preparation are verified; uBlock execution/filtering is still unverified.
CRX2, store authentication, XPI signature verification, Safari package support,
automatic updates, complete API compatibility profiles and the remaining full
browser requirements are unfinished. This checkpoint does not complete the
original objective.

The verified distributable is `artifacts/modern-browser/bundle-d0h_6xzg/`,
including all private libraries/helpers and the matching complete WebKit build
source trees. All 399 artifact entries and 375 native entries pass independent
inventory verification; [copy provenance](modern-bundle-copy.md#verified-crx3-import)
records their digests. The new bundle's live Dark Reader preview is restored
at VNC `127.0.0.1:5905`, with the progress terminal below it.
