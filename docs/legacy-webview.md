# BWebView on Summit's engine

Added 30 September 2026. Haiku's own web views — WebPositive, and any
program written against HaikuWebKit's `BWebView` — run on Summit's WebKit
instead of HaikuWebKit.

## What it is

`legacy/` is a `libWebKitLegacy.so.1` with HaikuWebKit 1.10.0's public
classes, implemented on Summit's engine API (`BWebKitView`,
`BWebKitContext`, from the `summit_webkit` package):

| HaikuWebKit | On Summit's engine |
| --- | --- |
| `BWebView` | A container holding the engine's `BWebKitView` as its child (in the slot `fOffscreenView` of the unchanged class layout). The engine view draws the page and takes the mouse and keyboard; `MakeFocus()` gives it the focus. |
| `BWebPage` | As before, a handler of the application looper. The engine's notifications for its view arrive there and go to the page's listener as the old messages: `LOAD_NEGOTIATING`/`LOAD_STARTED` when a load starts, `LOAD_COMMITTED` when the address changes, `LOAD_PROGRESS` (0-100), `LOAD_DOC_COMPLETED` and `LOAD_FINISHED`, or `LOAD_FAILED` and `MAIN_DOCUMENT_ERROR`, `TITLE_CHANGED`, `UPDATE_NAVIGATION_INTERFACE`, `UPDATE_HISTORY` for each successful load, `SET_STATUS_TEXT` for the link under the pointer, `ICON_RECEIVED`, `CLOSE_WINDOW_REQUESTED`. |
| New windows | `window.open()` and links with a target: the page makes a new `BWebView` and sends `NEW_PAGE_CREATED` with it (and a frame when the page asked for a sized pop-up). Once attached to a window, the view accepts the engine's opened page, preserving its opener and any script-written content. A middle click on a link sends `NEW_WINDOW_REQUESTED`. |
| The listener | As before, a `BWebView` makes its window its page's listener when it is attached (WebPositive never sets one itself). |
| HTTP authentication | Engine challenges become the old `AUTHENTICATION_CHALLENGE` message. A separate reply handler binds each asynchronous response to its request, keeping the engine application thread free while WebPositive's existing panel waits. Basic/Digest login, retry, cancellation and session reuse are [verified](legacy-authentication-2026-10-22.md). |
| Certificate failures | `SSL_CERT_ERROR` carries the failed host, certificate details and session scope to the existing `BWebWindow` dialog, which defaults to Stop. An asynchronous response can approve only the exact current failed navigation. See [native acceptance and limits](legacy-certificates-2026-10-22.md). |
| Context menu | The engine leaves the menu to its host; the old engine drew its own. `BWebView` shows one: open link or image in a new tab, copy its address, download it, cut/copy/paste, back/forward/reload. It is closed (as by Escape, nothing chosen) when the view leaves its window, is shut down or deleted: an asynchronous `BPopUpMenu` opened on a window's thread makes that window's destructor wait until the menu stops tracking. |
| `BWebDownload` | The engine reports downloads to a handler of the application looper, which makes `BWebDownload` objects and sends `B_DOWNLOAD_ADDED` to the download listener. The engine starts saving at once (to the Desktop); `Start(folder)` moves the file into the application's folder when it is complete. `B_DOWNLOAD_STARTED`, `B_DOWNLOAD_PROGRESS` and a synchronous `B_DOWNLOAD_REMOVED` follow as before; the file gets its `META:url` and MIME type. |
| `BWebSettings` | `SetPersistentStoragePath()` chooses where the engine keeps the application's website data: `<path>/WebKit` (WebPositive: `~/config/settings/WebPositive/WebKit`). `SendIconForURL()` answers from the site icons the engine reported. `Apply()` publishes font and JavaScript settings, with application defaults and per-page overrides. `SetUserAgent()` adds the same control for the user agent. Proxy settings remain unapplied. See [settings behavior and acceptance](legacy-settings-2026-10-22.md). |
| `BWebFrame` | The main frame: address, title, editing commands, zoom, find. `InnerText()`, `AsMarkup()` and `FrameSource()` ask the page with a script and wait for it (up to 10 s), so like `GetContentsAsMHTML()` they answer only off the application thread (empty on it, or when the page does not answer); `FrameSource()` is the source from the cache as `SendPageSource()` gets it. `SetEditable()` switches the document's `designMode`, for the page shown then (a new page starts not editable). There is no frame tree. |
| `SendPageSource()` | The source as the server sent it (from the cache), or the page's markup. |
| `GetContentsAsMHTML()` | A MIME archive of the document's markup as it stands, with a `<base>` so that its links and pictures still lead to the site. Waits for the engine; it cannot be called on the application thread. |
| `WebKitInfo` | "Summit" and the engine's versions. |
| Cookie window | WebPositive's cookie manager reads the `BNetworkCookieJar` of the `BUrlContext` it gives its views. The library keeps every such jar in step with the engine's cookie store (`legacy/src/Cookies.cpp`), on the application looper: every 2 s and soon after a page finishes loading it fetches the engine's cookies (`BWebKitView::GetCookies`) and makes the jar hold exactly those (stale ones, such as those WebPositive archived in its settings, are removed). A cookie it put in the jar that has gone from it since was deleted by the program (the window's Delete button adds an expired copy); the library then deletes exactly that cookie (name, domain, path) from the engine. `SUMMIT_LEGACY_COOKIES=0` turns this off. |
| Web inspector | Summit's own developer tools window (Elements, Network, Console, Storage, Sources, Performance: `src/ui/DevTools*`, built into the library), connected to the page through the engine's inspector protocol (`BWebKitInspectorSession`). The view's context menu has "Inspect element", which opens it with the element under the pointer selected; a second time it brings the page's window forward. A program can send `'sins'` (optional `"where"`, a `BPoint` in view coordinates) to the `BWebView` itself. HaikuWebKit's inspector was a stub and WebPositive has no menu item for one. |
| `BWebWindow` | HaikuWebKit's own code, unchanged apart from a completion handler Summit's engine does not use. |

Programs built for HaikuWebKit run without being rebuilt: the headers
(`legacy/headers`) are HaikuWebKit's, with the same class layouts and virtual
functions, and every function WebPositive imports is there (checked against
the installed WebPositive's undefined symbols). One source difference:
`WebFrame.h` no longer includes JavaScriptCore's and WebCore's headers, so
`BWebFrame::FindString()` takes option bits of its own (`B_FIND_BACKWARDS`,
`B_FIND_CASE_SENSITIVE`, `B_FIND_WRAP_AROUND`) and `GlobalContext()` returns
`NULL`.

## Packages

- `summit_webkit` (the engine: `libWebKit`, its processes, ICU, a Mesa with
  EGL, in `/boot/system/lib/summit-webkit`), made by
  `tools/package-summit-webkit.sh`; with `SUMMIT_REMOTE_SHELL=tools/ws.sh`
  it is made on the X399 from one of its bundles, with its zink Mesa.
- `summit_webkitlegacy` (`lib/libWebKitLegacy.so.1`, whose run-time path
  leads to the engine, and HaikuWebKit's header and development-library
  paths), made by `tools/package-summit-webkitlegacy.sh` on the X399 against
  the installed engine (or with the Haiku cross tools: `legacy/Makefile`
  works with any compiler given `ENGINE_HEADERS` and `ENGINE_LIB`). It
  provides `haikuwebkit` and `haikuwebkit_devel` (WebPositive's package
  requires `haikuwebkit`) and replaces and conflicts with both. Since the
  web inspector (30 October 2026) it is built from `legacy/` with Summit's
  `src/core`, `src/ui` and `vendor/nlohmann`, links Haiku's static
  `libnetservices.a`, `libshared.a` and `libcolumnlistview.a`, and needs
  `libscintilla` and `liblexilla` (building also `scintilla_devel` and
  `lexilla_devel`).

air/OS CI builds `summit_webkitlegacy` too (airos-ci, 30 October 2026):
`summit/build-summit.sh` makes it with `summit_webkit` and `summit`, for
x86_64 and arm64, by `build_summit_webkitlegacy` in
`apps/build-app-packages.sh`. That cross-compiles `legacy/Makefile` with the
SDK against the engine just built (its public headers and `libWebKit.so.1`),
Summit's `src/` and `vendor/` of the same checkout, the SDK sysroot's static
`libnetservices.a`, `libshared.a` and `libcolumnlistview.a` and private
headers, and Scintilla and Lexilla from CI's third-party prefix. The package
has the engine's version (`1.10.1~git<commit time>-<revision>`, newer than
the hand-made 1.10.0 ones), requires the `summit_webkit` of the same build
(on x86_64 also `lib:libscintilla` and `lib:liblexilla`; arm64 images carry
`airos_scintilla` and `airos_lexilla`, which have no `lib:` names), and goes
into the package pool and every air/OS image (`images/build-image.sh`
`IMAGE_APPS`). The stock Haiku R1/beta6 builds (`latest-haiku`) leave it out:
there it would take haikuwebkit's place under the release's WebPositive.

It does not provide what else `haikuwebkit` provides: `lib:libjavascriptcore`
(HaikuWebKit's `libJavaScriptCore.so.18` in `/boot/system/lib`) and `cmd:jsc`.
A program that links JavaScriptCore directly still needs HaikuPorts'
`haikuwebkit`, which conflicts with this package.

## In the Haiku fork

Commit 3c6c1ce347 on the fork's `x399-workstation` branch: when
`HAIKU_SUMMIT_PACKAGES_DIR` (UserBuildConfig, or `jam -s`) names a directory
with the two packages, the `webkit` build feature takes its headers and
library from `summit_webkitlegacy` instead of HaikuWebKit, and WebPositive's
link finds the engine's libraries through `-rpath-link`. Unset, the build is
as before. `jam -q WebPositive` was built both ways on the x86_64 build; the
Summit-built binary needs `libWebKitLegacy.so.1` like the other.

The air/OS images have `summit_webkitlegacy` (above) but no WebPositive: the
fork stopped building, packaging and adding it on 17 October 2026 (6a10afc026,
Summit is the browser), and its Haiku repository is left out of release
images. The image's packages need neither `haikuwebkit` nor
`lib:libwebkitlegacy`, so only `summit_webkitlegacy` satisfies those names
there; a program built for HaikuWebKit installed later (pkgman, HaikuDepot)
runs on the image's engine, and pkgman does not fetch HaikuPorts'
`haikuwebkit` for it. Checked on the X399 (30 October): with the CI-built
package installed, `pkgman install` of R1/beta6's `webpositive` package
(requires `haikuwebkit>=1.10.0`) planned that package alone, and that
WebPositive (unpacked, not installed) drew https://www.haiku-os.org on the
installed engine ("finished" in its status line) and quit cleanly. The
package was built by airos-ci's recipe from Summit 75b323f against the
image's `summit_webkit-1.10.1~git20261029.1957-1`. Twice, `pkgman uninstall
summit_webkitlegacy` was followed by the X399 freezing (no log line after
the command). The package is not the cause: in an air/OS VM of the same
image, about 30 install, upgrade, replace (over HaikuPorts' haikuwebkit) and
uninstall transactions ran without a fault, with the serial console
captured. That test found `package_daemon` crashing at boot when System
Updater's boot check asks for the package state before the daemon has
loaded it (`Volume::HandleGetLocationInfoRequest` reading a null
`fActiveState`); after that every `pkgman` change hangs, which matches the
X399's first uninstall hanging. Putting
WebPositive back into the images would be a change in the fork (its
profiles, with `HAIKU_SUMMIT_PACKAGES_DIR` naming CI's packages, which the
`webkit` build feature accepts on x86_64 only).

## Verified

In the build VM (llvmpipe, engine `bundle-r6ti1k5v`), with this library
first in `LIBRARY_PATH`, on 30 September 2026 — the X399 was unreachable
over SSH that afternoon:

| | |
| --- | --- |
| Programs | the installed WebPositive (built for HaikuWebKit, not rebuilt) and a WebPositive built from the fork against this library |
| Pages | the test site and https://www.haiku-os.org drawn; the window and tab titles, the status line ("… finished"), Back/Forward/Stop following the page; a restored session tab and its site icon |
| Navigation | a link, Back, Forward; Enter in the address field |
| Context menu | on a link: Open link in new tab (a background tab), Copy link address, Download linked file |
| New pages | `target=_blank`: a new selected tab; a sized `window.open()`: WebPositive's pop-up window at that size, drawn |
| Downloads | `download=` link: the Downloads window, the notification, the file on the Desktop with `META:url` and its MIME type |
| Dialogs | `alert()` through the engine's own dialog |
| Source, saving | View › Page source handed the source to WebPositive (which then could not start the VM's source viewer, a VM matter); Window › Save page as… wrote an MHTML archive with the markup and a `<base>` |
| New windows | Window › New window (Alt+N) |

The September build needed a fallback: it declined the engine's opened page
and loaded its address independently after attachment to a window. This drew
the child, but made `window.open()` return `null`, removed `window.opener`,
and discarded initially blank popups. Engine-created pages stayed blank at
that time; the cause was not isolated.

On 22 October, the current engine's opened pages rendered in both a native
airOS `BWebView` probe and the installed stock Haiku WebPositive. The shim now
always accepts the engine's page after attachment, removing that fallback and
the `SUMMIT_LEGACY_OPENED_PAGES` switch. The real WebPositive test covers a sized
popup, an ordinary new tab, and an initially blank popup populated with
`document.write()`: each preserves its opener, exchanges messages, renders on
screen, and closes through script. See [the acceptance record](legacy-popups-2026-10-22.md)
for exact builds, failed attempts and remaining limits. Live GitHub/Google
sign-in is not yet verified. `SUMMIT_LEGACY_TRACE=1` still logs what the pages
send to their listeners.

## Installed on the X399

At 13:51 on 30 September 2026, after the machine's restart:
`summit_webkit-1.10.0-4` (from `bundle-9x6fji8g`, with the X399's zink
Mesa `prefix-20260930`; it replaced -3, which Aurora also uses) and
`summit_webkitlegacy-1.10.0-1` (built there against it), which replaced
`haikuwebkit` and `haikuwebkit_devel`. The old packages are kept in
`/boot/home/summit/package-backups`; pkgman's previous activation state is
`state_2026-09-30_13:50:58`. The installed WebPositive then ran on Summit's
engine: its restored session (with site icons), the test page, a
`target=_blank` tab and a sized pop-up window drawn, tabs closed and the
program quit with no process left and no crash report.

To go back: `pkgman install` the three packages from
`/boot/home/summit/package-backups` (haikuwebkit, haikuwebkit_devel,
summit_webkit-1.10.0-3) and uninstall summit_webkitlegacy.

Verified on the X399 (25 Oct, fx106 engine, shim built on the device): a
`BWebView` program reading from its own thread got the German fixture's
text from `InnerText()`, the live DOM from `AsMarkup()` and the served
source from `FrameSource()`; `SetEditable(true)` reported editable.

### Cookie window and web inspector (30 October 2026)

On the X399 (air/OS hrev60207+2112), with WebPositive from the
`webpositive` package of the Haiku build (hrev60206_1449, built for
HaikuWebKit, not rebuilt), the shim cross-built with the airOS SDK against
main's engine headers and the engine of `bundle-x399a` (main, 29 October),
test pages on two host names (`a.` and `b.192.168.1.200.nip.io`) that set
two persistent cookies (paths `/` and `/legacy`) and a session cookie:

| | |
| --- | --- |
| Cookie window | Window › Cookie manager listed both hosts and, for each, the three cookies with their paths, expiry dates ("Session cookie" for the session one) and values, 2-3 s after the pages loaded |
| Deleting | Delete on host `a` removed its three rows; the library deleted the three from the engine within 2 s; a new tab on `a` then showed an empty `document.cookie`, one on `b` still `legacyB=beta; legacyA=alpha; legacySession=gamma`; the reopened window listed only `b` |
| Restart | the jar held the engine's four persistent cookies again (the session cookies ended with the engine) |
| Inspector | the context menu showed "Inspect element" (opened with a scripted `B_WEBKIT_CONTEXT_MENU`; the item's message, `'sins'`, was then sent by `hey`, as no pointer input was available); the developer tools window showed the page's DOM tree (`<html>`, `<head>`, `<body>`, `<h1 id="h">`...), its styles and the console's 404 for `favicon.ico`; asked at a point over the heading, it selected `<h1 id="h">`; closed and reopened (a second connection to the page) it worked again |
| Closing | closing the inspected tab closed the inspector; quitting WebPositive with the inspector open left no process and no crash |

Two failures seen in these runs, which also happened with the shim before
this change, are fixed (30 October, below).

### Quitting: closing the last window, an open context menu (30 October 2026)

- Closing WebPositive's last browser window while it had several tabs
  crashed it in an IPC `ReceiveQueue` thread (`WTF::RunLoop::dispatch`, at
  `this` + 0x48 of a null `RunLoop`). Closing the window shuts its pages down
  just before the application quits (the last window's `WINDOW_CLOSED` posts
  `B_QUIT_REQUESTED`), so the web processes' connections are still being
  released when `main()` returns. `exit()` runs the main thread's TLS
  destructors, and the engine's `RunLoop::threadWillExit()` (Haiku part) then
  stopped the main run loop and set it to null; a connection whose last
  reference an IPC thread dropped after that is destroyed through
  `RunLoop::mainSingleton().dispatch()` and followed the null pointer.
  Quitting from the menu did not crash because its tabs' shutdown messages
  are never handled (the pages live until the process ends). Summit itself
  never gets there (it leaves with `_Exit()`). The fix is in the engine
  (`Source/WTF/wtf/RunLoop.cpp`): the main run loop stays, stopped, and
  keeps a reference of its own, so that a late dispatch is queued and
  dropped.
- Quitting WebPositive (or closing its window) while the view's context
  menu was open hung it: `BPopUpMenu::Go()` with `async` on the window's
  thread gives the window the menu's semaphore ("window close lock"), and
  `~BWindow` waits on it until the menu stops tracking, which, with nobody
  to click, it never did (`BrowserApp::QuitRequested` waits for the window
  thread). `BWebView` now closes its menu when it is detached from its
  window, shut down or deleted (`legacy/src/WebView.cpp`): an Escape key for
  the menu ends the tracking, and a tracking hook covers a menu not yet
  tracking; items are disabled first so the one under the pointer is not
  chosen.

Verified on the X399 (WebPositive from the Haiku build, the shim cross-built
from this tree, the engine of `bundle-x399a` with the changed
`libJavaScriptCore`/`libWebKit` from a CI-tree test build), each run started
with three URLs plus the restored session's tabs: closing the browser window
(a scripted `quit` of the window) crashed 4 of 4 times with the unchanged
engine (with either shim), and left no crash and no engine process 6 of 6
times with the changed one; quitting with the context menu open (a scripted
`B_WEBKIT_CONTEXT_MENU`, then `hey WebPositive quit`) hung 3 of 3 times with
the old shim and quit within 1 s, leaving no engine process, 10 of 10 times
with the new one (5 on each engine);
closing a second window with its context menu open closed the window and
the menu and left the application running. Opening a second window
(`nwnd`), opening tabs (a second launch with a URL) and closing them
(`cltb`), loading pages and quitting still worked.

## Not done

- Proxy configuration and per-page storage paths remain unapplied.
  Font, JavaScript and user-agent settings pass native `BWebView` acceptance;
  installed stock WebPositive also passes font Apply/Revert and restart checks.
  See [settings acceptance](legacy-settings-2026-10-22.md).
- `BWebFrame`'s frame tree (subframes), and its document access on the
  application thread (it needs that thread to answer).
- The web inspector does not follow the old API's zoom steps: at a page
  zoom other than 100 %, "Inspect element" may select a neighbour of the
  element under the pointer. The window cannot dock into the program's
  window. `SetInspectorView()` is kept, unused. "Inspect element" is shown
  unless the program calls `BWebPage::SetDeveloperExtrasEnabled(false)` or
  `SUMMIT_LEGACY_INSPECTOR=0` is set (unlike the old engine, where nothing
  turned it on, it is on by default).
- HTTP NTLM, proxy requests and persistent Remember acceptance remain untested.
  Certificate questions pass native airOS lifetime checks and two complete
  installed stock WebPositive UI runs; an earlier helper shutdown failure
  remains unexplained. See [certificate acceptance](legacy-certificates-2026-10-22.md).
  The legacy listener
  protocol cannot dismiss a host-owned prompt when a request is withdrawn;
  its late response is ignored.
- The cookie window shows the engine's cookies as they were up to 2 s
  before it was opened; WebPositive rebuilds its list only when the window
  is shown. The jar has no host-only flag of its own: an engine
  `example.com` (host-only) and `.example.com` cookie of one name and path
  are one row, and deleting it deletes both. Unnamed cookies, cookies of
  single-label hosts (`localhost`) and partitioned cookies' partitions are
  not shown (a partitioned cookie is not deleted from the engine). Only
  deletions go back to the engine: a cookie a program adds to or changes in
  the jar is replaced by the engine's at the next sync. Session cookies end
  with the engine, as before.
- Live GitHub/Google sign-in acceptance through a popup. The opener mechanics
  now pass the controlled tests above.
- Why the X399 froze after both uninstalls (30 October) is not known; the
  package itself does not do it (see above). The `package_daemon` boot race
  is an OS bug of its own.

