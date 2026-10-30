# Summit

<img src="docs/summit-icon.png" alt="Summit mountain and flag icon" width="128">

Summit is a native WebKit browser for **air/OS and Haiku**, with native controls,
a compact toolbar, and a multiprocess engine.

**Development preview.** Summit is used and tested on air/OS; web-platform and
extension compatibility are still evolving. Feature notes below describe the
current source. Download packages identify the commit they were built from.

There are two builds of every version:

- **air/OS** (x86_64 and arm64): [the latest release](https://github.com/jmgasper/summit/releases/tag/latest).
  It uses air/OS's GPU drivers, app_server and video decoders where the
  machine has them.
- **Haiku R1/beta6** (x86_64): [the latest Haiku release](https://github.com/jmgasper/summit/releases/tag/latest-haiku),
  for a stock Haiku installation without air/OS's changes. It has the same
  browser, but some features are better on air/OS: rendering and WebGL on
  the GPU, high-density screens, hardware video decoding and Web Bluetooth.
  See [Summit for air/OS and for Haiku](docs/haiku-build.md).

Choose the Summit and Summit WebKit packages for your system from the build
tables below, and install both together.

![Summit running WebKit natively](docs/screenshots/current-webkit.png)

<!-- airos-ci:latest-builds:start -->
## Latest builds

Built automatically by air/OS CI from commit `586f525` on 2026-10-30 ([all files](https://github.com/airos-works/summit/releases/tag/latest)).

| Architecture | Package |
|---|---|
| arm64 | [summit-0.1.0.git20261030.0312-1-arm64.hpkg](https://github.com/airos-works/summit/releases/download/latest/summit-0.1.0.git20261030.0312-1-arm64.hpkg) |
| arm64 | [summit_webkit-1.10.1.git20261030.0312-1-arm64.hpkg](https://github.com/airos-works/summit/releases/download/latest/summit_webkit-1.10.1.git20261030.0312-1-arm64.hpkg) |
| x86_64 | [summit-0.1.0.git20261030.0312-1-x86_64.hpkg](https://github.com/airos-works/summit/releases/download/latest/summit-0.1.0.git20261030.0312-1-x86_64.hpkg) |
| x86_64 | [summit_webkit-1.10.1.git20261030.0312-1-x86_64.hpkg](https://github.com/airos-works/summit/releases/download/latest/summit_webkit-1.10.1.git20261030.0312-1-x86_64.hpkg) |

Install with `pkgman install <file>`, or copy the file into `/boot/system/packages`. Haiku SDK: x86_64 hrev60207-2114-g2646c1fb83, arm64 hrev60207-2115-g8e97a85464.
<!-- airos-ci:latest-builds:end -->

<!-- airos-ci:haiku-builds:start -->
## Latest Haiku builds

Built automatically by air/OS CI from commit `586f525` on 2026-10-30 ([all files](https://github.com/airos-works/summit/releases/tag/latest-haiku)).

| Architecture | Package |
|---|---|
| x86_64 | [summit-0.1.0.git20261030.0312-1-x86_64.hpkg](https://github.com/airos-works/summit/releases/download/latest-haiku/summit-0.1.0.git20261030.0312-1-x86_64.hpkg) |
| x86_64 | [summit_webkit-1.10.1.git20261030.0312-1-x86_64.hpkg](https://github.com/airos-works/summit/releases/download/latest-haiku/summit_webkit-1.10.1.git20261030.0312-1-x86_64.hpkg) |

For a stock Haiku installation, not air/OS. Install the packages together with `pkgman install ./<file>...`, which also installs what they need from HaikuPorts. Haiku SDK: the R1/beta6 release (r1~beta6_hrev59866_79), not air/OS.
<!-- airos-ci:haiku-builds:end -->

## Browser features

- Tabs that can be dragged between windows, pinned and muted (a speaker shows
  which tab plays sound), saved sessions, back/forward history, downloads,
  page find, configurable search, and per-site zoom.
- [Printing, Save as PDF, spell checking, saved passwords in the Haiku key
  store, Picture in Picture, websites installed as apps](docs/issue-fixes-2026-10-19b.md),
  an autoplay setting, blocked pop-up notices and a page for crashed or
  hung tabs.
- [Bookmarks in folders](docs/bookmarks.md), on the bookmarks bar (folders open
  as menus, drag to reorder or drop tabs) and in the Bookmarks menu, edited on
  the Bookmarks tab or from the star; import from WebPositive, Firefox
  (places.sqlite or HTML) and Chrome with their history, and export as
  bookmarks HTML.
- [Private windows and browsing-data controls](docs/browser-privacy.md),
  [certificate warnings and connection details](docs/certificate-warnings.md),
  and native website permission prompts.
- [User-agent presets](docs/user-agent.md) in Preferences › Compatibility:
  Chrome, Firefox, Safari and Edge identities for sites that reject Summit.
- [Bundled emoji fonts](docs/emoji.md), including color, skin tones, flags and joined sequences.
- [Reader mode](docs/reader-mode.md) for articles, with a book button in the
  address field, sanitized content, and the original page's URL retained in history.
- A bundled [PDF viewer](docs/pdf-viewer.md) with navigation, search, zoom,
  password entry, and downloads of the original document.
- Native [color, date, month, week, time, and local date/time pickers](docs/native-form-pickers.md).
- [Developer tools](docs/developer-tools.md): an Elements panel (the live DOM
  tree, matched and computed styles, the box model, an element picker and
  Inspect Element), JavaScript console output, network requests and storage,
  and View › View Page Source (Alt+U) for the HTML as the server sent it.
- [Updates from inside the browser](docs/updates.md): File › Check for
  Updates… and a daily check find newer builds of the release for your
  system, list what changed, and Install and Restart installs the summit and
  summit_webkit packages together and reopens your windows and tabs.
- A [built-in MCP server](docs/mcp.md) (Preferences › Developer, off by
  default) that lets debugging tools such as Claude Code inspect tabs, pages,
  processes, memory, rendering, media and extensions on the machine itself.
- [Extension installation from Chrome Web Store and Firefox Add-ons](docs/extension-stores.md),
  native permission review, toolbar actions and popups, and restoration of
  approved extensions across restarts. Local CRX3, ZIP/XPI, and folder packages
  are also supported.

Published uBlock Origin and Dark Reader have passed native runtime suites;
1Password has verified sign-in, inline filling, and
[Web Authentication/passkey integration](docs/webauthn-passkeys.md). This is
not a promise that every Chrome, Firefox, or Safari extension works. See the
[verification record](docs/STATUS.md) and [1Password notes](docs/webextensions-1password.md)
for tested versions and limitations.

## Web features

| Area | Current implementation |
| --- | --- |
| Rendering | Modern WebKit HTML/CSS/JavaScript, [WebGL](docs/webgl.md), and accelerated compositing when a suitable EGL/GLES driver is available; software rendering otherwise. |
| Canvas | [2D hit regions](docs/canvas-hit-regions.md), plus main-thread and worker OffscreenCanvas with 2D, WebGL/WebGL2, and ImageBitmap transfer. |
| Images | Standard web formats plus JPEG XL and HEIC decoding, and JPEG XR decoding/encoding. |
| Media | HTML audio/video and [MediaSource streaming](docs/streaming-media.md) for MP4, WebM, ADTS AAC, MPEG audio and MPEG-TS. Direct [MPEG-DASH](docs/dash-playback.md) supports H.264/AAC, seeking, adaptive quality and live refresh. |
| Encrypted media | [Clear Key EME](docs/encrypted-media.md) with temporary CENC sessions for H.264/AAC through MediaSource or DASH. Widevine, PlayReady, FairPlay, and persistent licenses are unsupported. |
| Devices | [WebUSB](docs/web-platform-issues.md) and [Web Bluetooth](docs/bluetooth-transport.md), with native selection, origin-scoped grants, protected-device checks, and cancellation on page closure. |
| Integration | Native fullscreen, approved external-app and website protocol handlers, and Web Authentication with [passkeys kept by Summit](docs/webauthn-passkeys.md) in the key store (password-manager extensions first). |

[Web-platform notes](docs/web-platform-issues.md) document the supported subsets.
Device access requires a secure context and user approval. WebUSB physical
transfers and successful physical Bluetooth GATT reads still need dedicated
device validation; the Bluetooth busy-controller check passed while preserving
X399's active mouse link. Bluetooth currently supports the top-level origin and
same-origin children, session-only grants, and the air/OS stack's existing LE
connection facilities. New pairing and advertisement observation APIs are not
implemented.

## Build and run

Use matching application and engine packages from the same build table. Native
engine builds need the air/OS/Haiku toolchain and private dependencies described
in the [engine notes](engine/README.md). air/OS CI
([jmgasper/airos-ci](https://github.com/jmgasper/airos-ci)) makes both builds:
the air/OS one against the air/OS SDK, and the Haiku one against an SDK made
from the R1/beta6 release itself (`AIROS_TARGET=r1beta6`).

The full multiprocess, extension-enabled development build is:

```sh
bash tools/build-webkit-in-vm.sh --modern-extensions all
bash tools/build-modern-browser-in-vm.sh --browser --bundle --modern-extensions
```

The scripts record the resulting bundle path. Copy the entire bundle and run
its `run-browser.sh`; it carries the matching engine, process helpers, private
libraries, source archive, and build manifest. See the [native test VM](docs/VM.md);
released builds are CI's packages (above).

For UI development the Makefile builds against an engine whose API headers
(`develop/headers/WebKit/WebKitView.h`) and library are under `WEBKIT_PREFIX`,
such as the staging directory of an engine build (the `summit_webkit` package
carries the library only):

```sh
pkgman install gcc make
make -j6 WEBKIT_PREFIX=/path/to/engine/staging
make check
./build-modern-haiku/Summit
```

The build against HaikuWebKit's WebKitLegacy library is retired (#120).

Portable tests also run on Linux. Install a C++ compiler, CMake, and OpenSSL
development headers/libraries, then run:

```sh
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host -j6
ctest --test-dir build-host --output-on-failure
```

## Using Summit

Haiku's default Command modifier is **Alt**: Alt+L focuses the address field,
Alt+T opens a tab, Alt+W closes it, Alt+Shift+T reopens the last closed tab,
Alt+D bookmarks the page, Alt+F finds text, and Alt+R reloads. Ctrl+Tab cycles
tabs. Choose the search provider in Preferences.

Profiles live in `~/config/settings/Summit/`; `--profile /path` selects a
separate profile. Downloads default to `~/Downloads/`.
[Privacy notes](docs/browser-privacy.md) describe storage and private windows.

The application icon is the native vector resource `resources/Summit.hvif`.
Editable toolbar artwork, generated image sizes, and regeneration commands
are documented in [resources/artwork](resources/artwork/README.md).

See [current verification](docs/STATUS.md),
[the October issue work](docs/issue-fixes-2026-10-07.md),
[requirements and remaining work](docs/REQUIREMENTS.md), and
[performance measurements](docs/performance.md) for implementation evidence.

Summit's application code is MIT-licensed. WebKit and the Haiku port retain
their upstream per-file licenses; engine changes are in `engine/patches`.
Bundled dependencies retain their own licenses and notices.
