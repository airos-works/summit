# Summit for Android

Summit builds as a native Android app (`android/`) from the same sources as
Summit for air/OS. This page describes how the two builds share code, how to
build the APK, and what is still to do.

## What is shared, and how

| Layer | air/OS | Android | Shared |
| --- | --- | --- | --- |
| Engine | WebKit, upstream pin + `engine/patches/0001-haiku-port.patch`, `PORT=Haiku` | The same tree, `PORT=WPE` with `ANDROID` | Everything outside the platform directories: WebCore, JavaScriptCore, the WebKit process model, the WebExtensions implementation |
| Engine embedding | `Source/WebKit/UIProcess/API/haiku` (BWebKitView) | WPE's GLib API, through `android/wpeview` (Igalia's WPE Android view, vendored at 205f39ae1) | — |
| Browser logic | `src/core` | `src/core`, compiled into `libsummit.so` | All of it: profile.json (session, bookmarks, history, reading list, settings), address resolution and bookmark keywords, search engines, type-ahead, user agents |
| Interface | `src/ui` (BeAPI) | `android/app` (Kotlin, Jetpack Compose) | — |

The rule that keeps the two in step:

- **Engine changes go into the one engine tree** (`.cache/WebKit`, saved as the
  one patch by `tools/update-engine-patch.py`). A fix in shared engine code
  reaches both builds with no extra work. Android-only engine code lives under
  WPE/Android directories or `#if OS(ANDROID)`.
- **Browser logic goes into `src/core`**. The Android build compiles every
  file in `src/core` (a glob, like air/OS's Makefile), so new core code is
  built for Android automatically. Only its JNI surface
  (`android/app/src/main/cpp/SummitCoreJni.cpp`) needs a function when the
  Android interface should call something new.
- Interface code is per platform. Keep decisions (what an address means, what
  to suggest, what the profile holds) in `src/core`, not in either interface.

Examples already shared: the touch layouts' user agent (`summit::MobileUserAgent`,
used by air/OS Mobile and by Android), search engines and bookmark keywords
in the address field, history type-ahead, the words of the error page for a
failed load (`core/LoadError`), saved site permissions, and profile.json,
which has the same format on both systems.

The Clang build for Android also catches engine code that GCC (air/OS)
accepts, so a change that breaks it is a bug in the shared tree: attributes
before declaration specifiers (`NODELETE static ...`), unnamed parameters in a
constrained template's declaration that lambdas capture, and POSIX calls
bionic lacks (`shm_open`).

## Building

Everything large lives under `SUMMIT_ANDROID_ROOT` (default
`/data3/airos/summit-android`): cerbero, its toolchain and dependency
prefixes, and the engine build directories. The Android SDK and NDK are set
up as described in `~/android/README.md` (`. ~/android/env.sh`).

```sh
tools/android/build-deps.sh x86_64        # once (~1-2 h): GLib, libsoup, GStreamer, ICU, ... via cerbero
tools/android/build-engine.sh x86_64      # the engine (incremental); installs into the cerbero prefix
tools/android/stage-engine.py x86_64      # copies what the engine loads into android/wpeview
cd android && ./gradlew :app:assembleDebug   # app/build/outputs/apk/debug/app-x86_64-debug.apk
```

Use `arm64` for phones and tablets and `x86_64` for the emulator. The Gradle
build produces one APK per staged ABI.

- `build-deps.sh` uses [wpe-android-cerbero](https://github.com/Igalia/wpe-android-cerbero)
  at a pinned revision. The dependencies are built with cerbero's NDK (r29)
  for API 31; the engine and the app target API 33 (minSdk).
- `build-engine.sh` builds `SUMMIT_ENGINE_SOURCE` (default `.cache/WebKit`)
  out of tree, with ccache. Options follow cerbero's `wpewebkit` recipe, with
  OpenSSL as the crypto backend (as on air/OS) and WebXR off.
  `SUMMIT_ENGINE_CMAKE_EXTRA` adds CMake options.
- `stage-engine.py` copies only the libraries the engine needs (the NEEDED
  closure of the engine, the injected bundle, the GIO TLS module and the
  GStreamer plugins Summit uses), not everything cerbero built. It renames
  versioned sonames (`libfoo.so.1` → `libfoo_1.so`), as Android installs only
  `lib*.so` files. `--sysroot` stages from a WPE Android prebuilt package
  instead, which helps when working on the app without an engine build.

## The app

- **Package** `works.airos.summit`, minSdk 33 (Android 13), targetSdk 36
  (Google Play's requirement since 31 August 2026), compileSdk 37.1. Native
  code is built for 16 KB pages, which Google Play requires.
- **Processes**: the engine's web content and network processes are services
  of the app (`:WPEWebProcessN`, `:WPENetworkProcessN`), started by
  wpeview's process manager. Android ranks and ends them like the app's own
  processes.
- **Window sizes**: phones get the address field at the top and Back,
  Forward, Share, Bookmarks and Tabs at the bottom (as on air/OS Mobile).
  Windows 600 dp and wider (tablets, unfolded foldables, desktop mode) get one
  toolbar with a row of tabs under it. The activity handles size, density
  and posture changes itself, so folding, rotating and resizing keep the
  pages.
- **Tabs**: each tab owns an engine page (`WebPage`); the window's one
  `WebView` shows the selected one. A grid switcher with Tabs | Private
  segments, swipe or ✕ to close, Close All, and pictures of the pages.
  Links with `target=_blank` and `window.open` open a tab next to the page;
  Back on its first page closes it and returns to the opener. Private tabs
  use an ephemeral engine context that is destroyed with the last private
  tab.
- **Pages**: permission prompts (location, notifications) remembered per
  site in the profile, Android runtime permissions asked for when a site is
  allowed, web notifications as Android notifications, downloads into the
  shared Downloads collection, fullscreen with the system bars hidden, error
  pages, and a page for a web process that ended.
- **Resource use**: see *Tabs and memory* below.

### Tabs and memory

Summit keeps at most a few pages loaded: 2 on low-memory devices and under
3 GB, then 3, 4, 6, and 8 at 11 GB and more. Showing another tab discards the
page that was shown least recently. The tab keeps its address, title, picture
and back/forward list, and loads again when it is shown. A page playing
sound is not discarded. Only the shown page has a surface;
the engine treats the others as hidden, so they stop painting and their
timers and animation frames are throttled. Android's memory pressure
(`onTrimMemory`) discards every page but the shown one. After five minutes
in the background, Summit discards that one too, so an idle Summit holds no
page processes.

## WebExtensions on Android

Summit's WebExtensions implementation (Firefox, Chrome and Safari web
extensions; see `docs/webextensions-*.md`) is engine code that air/OS and
Android share. It is compiled where `USE(SUMMIT_EXTENSIONS)` is set: on Haiku
whenever WebExtensions are on, on WPE for Android with
`-DENABLE_WK_WEB_EXTENSIONS=ON`.

- `Source/WebKit/SummitExtensions.cmake` lists its sources and IDL bindings;
  `PlatformHaiku.cmake` and `PlatformWPE.cmake` include it.
- Its guards are `USE(SUMMIT_EXTENSIONS)`, and IDL members only Summit
  implements are `Platform=SUMMIT` (the bindings generator turns that into the
  same condition). `PLATFORM(HAIKU)` is left for code that needs Haiku itself.
- Browser commands (the engine asking the browser to open, close, move or
  navigate tabs and windows; the protocol is documented in
  `UIProcess/API/haiku/WebKitContext.h`) are `NativeBrowserCommand`: a
  `BMessage` on Haiku, and on WPE a class with the same `Add*` methods that
  reaches the app as JSON. The command codes are generated for WPE from
  WebKitContext.h, so there is one list.

How it fits together on WPE:

- The shared registries (`BrowserTabRegistryHaiku`, `BrowserCommandBrokerHaiku`,
  `ExtensionPackageRegistryHaiku` and the rest) compile unchanged on WPE through
  `Platform/summit/SummitHaikuTypes.h`, which stands in for the few Haiku types
  they use (`BMessenger` is an embedder target ID, `BMessage` a JSON-able
  `NativeBrowserCommand`). Their file names keep the Haiku suffix.
- `UIProcess/Extensions/wpe/` has the WPE parts: background pages on a headless
  WPE view (`ExtensionWebViewWPE`), action popups as tabs
  (`ExtensionActionPopupWPE`), and the embedder API (`SummitExtensionsWPE`):
  - `webkit_summit_extensions_initialize(session, storageDir, sink, data)`
    starts extensions for a network session (before its first page).
  - `webkit_summit_extensions_request(json)` takes requests named after
    `BWebKitContext`'s methods: `prepare`, `load`, `unload`, `actions`,
    `activateAction`, `commands`, `performCommand`, `setWindowTabs`,
    `respond` (to a browser command), plus `pageFor` (below).
  - Answers and events come back through the sink as JSON, as the BMessages a
    Haiku host gets: `{"target", "what", "fields"}`.
  - `webkit_summit_extensions_page_identifier(view)` is the page's
    `WebPageProxyIdentifier`, which tab lists and browser commands use (not
    `webkit_web_view_get_page_id()`, the web process's page).
- An extension's own pages (its popup or options page in a tab) need a page
  made for that extension, as `BWebKitContext::CreateExtensionView` makes one:
  the app sends `pageFor` with the URL just before it makes the page, and the
  engine gives that page the extension's configuration. Such a page shows only
  that extension's pages; going to a web site from it, the engine asks the app
  (`EXTERNAL_NAVIGATION_REQUESTED`) to show the site in a normal page.
- The app (`works.airos.summit.extensions`) keeps the installed extensions in
  the same catalog as air/OS (`core/ExtensionCatalog`, through JNI), with the
  same identities (`core/ExtensionIdentity`) and installation records, and
  loads them at startup as air/OS's `ExtensionController` does: the saved state
  brings back granted sites; a changed package needs new approval.
- Debug builds read `files/summit.env` (`KEY=VALUE` lines) into the
  environment before the engine starts, for the engine's `SUMMIT_*` switches
  (for example `SUMMIT_TRACE_EXTENSION_CONSOLE=1`, extension pages' console
  in logcat).

- Action popups: the engine asks the app to show one (an event to the action
  listener); the app makes a page for it (`pageFor` with purpose `popup`),
  hands it back (`attachPopup`, which makes it the action's popup for the tab
  as `addPopupPage` does on Haiku) and shows it in a bottom sheet. It is no
  tab, so the extension's active tab stays the page. Either side closes it
  (`popupClosed` from the app).
- Extension pages report the mobile browsers' user agents on Android
  (Firefox for Android, Chrome for Android), so packages take their mobile
  layouts.

- The stores: on an extension's page in the Chrome Web Store or on Firefox
  Add-ons the app shows air/OS's card (`core/StoreOffer`); Install downloads
  the package with `core/ExtensionStoreDownload` (libcurl, staged for the
  app; Android's trusted roots from the Conscrypt APEX) and checks it as
  air/OS's installer does (the item's identity; a CRX3 signature for the
  Chrome Web Store) before the approval dialog.
- History, bookmarks, search and `i18n.detectLanguage` for extensions are
  `core/ExtensionBrowserData`, which air/OS's window uses too
  (`tests/ExtensionBrowserDataTests.cpp`).
- Browser commands the app answers: tabs (open, close, activate, navigate,
  reload, back, forward, move), `windows.create` (as tabs: the app has one
  window), history, bookmarks, search, language, favicons. Engine events reach
  the app after the sender finishes, as BMessenger delivery does.
- Every tab is a tab to extensions: one without a live page (discarded, or
  restored and not shown) keeps an empty placeholder page that never loads and
  costs no web process, reported with its address and title as air/OS's
  `SetBrowserTabDiscarded` does. The page budget leaves a page made in the
  last 30 s alone.

Working on the emulator with uBlock Origin 1.77.4: installing from its XPI
(approval dialog), loading at startup, blocking (webRequest, surrogates,
cosmetic filtering), the badge count, the popup in a sheet with its mobile
layout, and its dashboard in a tab. Still to do:

1. The Haiku-only parts, each marked `FIXME (Summit, Android)`: notifications,
   `tabs.captureVisibleTab`, debugger input, `cookies.onChanged` on libsoup,
   idle state, opening downloads. Native messaging has
   no Android counterpart.
2. Safari web extensions (`.appex` bundles) as on air/OS.

## Automated tests (WebDriver)

Debug builds have WebDriver, as air/OS's Summit does for Interop and WPT:
`works.airos.summit.automation.AutomationActivity` (in `android/app/src/debug`
only) starts the engine's WebDriver process beside an automation context, and
shows the windows WebDriver opens. Their pages are Summit's, with the web
features `Platform/summit/SummitWebPagePreferences.cpp` turns on, so results
compare with air/OS's.

`tools/android/summit-webdriver --port=4444` is the WebDriver binary for test
runners on the host: it starts the activity on the device or emulator, waits
for WebDriver (port 8888 there) and forwards the host port to it with adb.

WPT (Interop) runs: `tools/android/run-wpt.sh WPT_DIR OUT [tests or
--include-file=LIST]`, on a wpt checkout with `tools/interop2026/wpt-summit.patch`
(its `tools/*` part) and `tools/android/wpt-summit-android.patch` applied. It
sets the display to 800x600 at 1x (the viewport tests expect; the automation
window hides the system bars), and runs `tools/android/wpt-proxy.py`, through
which the device reaches the host's wpt server: web-platform.test and its
subdomains go to the host (the device has no hosts entries), passed to Summit
as WebDriver's proxy capability. WebTransport (HTTP/3 over UDP) does not go
through the proxy. WebDriver screenshots are painted in the web process on
Android (the view has no snapshot there).

## Status and plan

Done:

- [x] The engine's dependencies cross-built for Android with cerbero (x86_64, arm64).
- [x] Summit's engine tree builds as WPE for Android (x86_64) and runs the
      app on the API 36 emulator.
- [x] The app: tabs, private tabs, the address field with shared resolution
      and type-ahead, bookmarks, reading list, history, settings, sharing,
      links from other apps, phone, tablet and foldable layouts, new windows,
      site permissions, notifications, downloads, find in page, file
      choosers, fullscreen, error and crash pages, and the tab memory policy.
- [x] WebExtensions with air/OS's implementation (`USE(SUMMIT_EXTENSIONS)`):
      Firefox and Chrome packages, the stores' "Add to Summit" card, action
      popups in a sheet, extension pages in tabs, history, bookmarks, search
      and browser commands answered by shared core code. uBlock Origin and
      Dark Reader work.
- [x] Web page preferences shared with air/OS
      (`Platform/summit/SummitWebPagePreferences.cpp`).
- [x] Hardware video decoding through GStreamer's `androidmedia`
      (MediaCodec, to a surface): H.264 plays on `c2.goldfish.h264.decoder`
      on the emulator.
- [x] Release builds (R8) and WebDriver/WPT runs in debug builds; a first
      Interop sample against air/OS (#306).

Next, in order:

1. arm64 engine build and a run on a phone (location delivery, which the
   emulator's provider never gave a page, is unverified).
2. Interop parity (#306): WebDriver input on WPE views (scroll-snap input
   tests), HTTP/2 in the soup backend, the full Interop 2024-2026 lists.
3. The FIXMEs of the extension implementation on Android (notifications,
   captureVisibleTab, debugger input, cookies.onChanged, idle, opening
   downloads); Safari web extensions; sensors (`docs/sensors.md`).
4. Media: MSE playback on YouTube; WebRTC capture.
5. Performance: browserbench.org (Speedometer 3.1, JetStream, MotionMark)
   on a phone against Chrome and Firefox, with the same harness as air/OS
   (`tools/bench`); PGO and LTO for the engine; battery and CPU use when
   idle and while scrolling.
6. CI: build the APK in airos-ci next to the air/OS packages, with a release
   signing key kept outside the repository.
