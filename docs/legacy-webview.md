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
| Context menu | The engine leaves the menu to its host; the old engine drew its own. `BWebView` shows one: open link or image in a new tab, copy its address, download it, cut/copy/paste, back/forward/reload. |
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

## In the Haiku fork

Commit 3c6c1ce347 on the fork's `x399-workstation` branch: when
`HAIKU_SUMMIT_PACKAGES_DIR` (UserBuildConfig, or `jam -s`) names a directory
with the two packages, the `webkit` build feature takes its headers and
library from `summit_webkitlegacy` instead of HaikuWebKit, and WebPositive's
link finds the engine's libraries through `-rpath-link`. Unset, the build is
as before. `jam -q WebPositive` was built both ways on the x86_64 build; the
Summit-built binary needs `libWebKitLegacy.so.1` like the other.

Putting the packages into the fork's images (a local package repository for
the image's dependency resolution) is not done; on a running system they are
installed with `pkgman install` (below).

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

Two failures seen in these runs also happen with the shim before this
change (same engine): closing the last browser window while it has several
tabs crashes WebPositive in a WebKit run-loop thread
(`WTF::RunLoop::dispatch` from `~Ref<IPC::Connection>`), and quitting while
the view's context menu is open hangs the application. Quitting with the
application's Quit does not crash.

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

