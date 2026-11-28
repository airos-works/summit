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

Working on the emulator: installing uBlock Origin from its XPI (approval
dialog), loading it at startup, its action in the page menu, and its popup as a
tab. Still to do:

1. Action popups as a bottom sheet instead of a tab, and the stores' "Add to
   Summit" card (`core/StoreOffer`).
2. The Haiku-only parts, each marked `FIXME (Summit, Android)`: notifications,
   `tabs.captureVisibleTab`, debugger input, `cookies.onChanged` on libsoup,
   idle state, opening downloads. Native messaging has no Android counterpart.
3. Safari web extensions (`.appex` bundles) as on air/OS.

## Status and plan

Done:

- [x] The engine's dependencies cross-built for Android with cerbero (x86_64, arm64).
- [x] Summit's engine tree builds as WPE for Android (x86_64) and runs the
      app on the API 36 emulator. Five fixes to the shared tree (all no-ops on
      air/OS) and two upstream backports (GLib FrameInfo, WPE press counts).
- [x] The app: tabs, private tabs, the address field with shared resolution
      and type-ahead, bookmarks, reading list, history, settings, sharing,
      links from other apps, phone and tablet layouts, new windows, site
      permissions, notifications, downloads, fullscreen, error and crash
      pages, and the tab memory policy.

Next, in order:

1. arm64 engine build and a run on a phone (location delivery, which the
   emulator's provider never gave a page, is unverified).
2. Browser features still missing on Android: find in page, file choosers,
   context menus (long press on links and images), a downloads list,
   favicons in the tab switcher and history, per-site zoom.
3. Media: hardware decoding through GStreamer's `androidmedia` (MediaCodec)
   with zero-copy AHardwareBuffer frames; MSE playback on YouTube; WebRTC.
4. WebExtensions: enable `ENABLE_WK_WEB_EXTENSIONS` for WPE and compile the
   Haiku port's extension implementation (`UIProcess/Extensions/haiku`,
   mostly platform-neutral C++) for WPE as well. The few BeAPI-bound files
   (tab commands, action popups, downloads) need an embedder interface with
   a Haiku and an Android implementation. The goal is the same extension
   support as on air/OS: Firefox (XPI, MV2 and MV3), Chrome (CRX, MV2 and
   MV3) and Safari web extensions, on phones and in desktop mode.
5. Performance: browserbench.org (Speedometer 3.1, JetStream, MotionMark)
   on a phone against Chrome and Firefox, with the same harness as air/OS
   (`tools/bench`); PGO and LTO for the engine; battery and CPU use when
   idle and while scrolling.
6. Interop 2024, 2025 and 2026 through WPE WebDriver on Android
   (`android/wpeview` has the WebDriver service), compared with the air/OS
   scores (`tools/interop2026`).
7. CI: build the APK in airos-ci next to the air/OS packages, with a release
   signing key kept outside the repository.
