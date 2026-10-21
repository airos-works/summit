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
| Context menu | The engine leaves the menu to its host; the old engine drew its own. `BWebView` shows one: open link or image in a new tab, copy its address, download it, cut/copy/paste, back/forward/reload. |
| `BWebDownload` | The engine reports downloads to a handler of the application looper, which makes `BWebDownload` objects and sends `B_DOWNLOAD_ADDED` to the download listener. The engine starts saving at once (to the Desktop); `Start(folder)` moves the file into the application's folder when it is complete. `B_DOWNLOAD_STARTED`, `B_DOWNLOAD_PROGRESS` and a synchronous `B_DOWNLOAD_REMOVED` follow as before; the file gets its `META:url` and MIME type. |
| `BWebSettings` | `SetPersistentStoragePath()` chooses where the engine keeps the application's website data: `<path>/WebKit` (WebPositive: `~/config/settings/WebPositive/WebKit`). `SendIconForURL()` answers from the site icons the engine reported. Fonts, script and proxy settings are kept but not applied: the engine has no per-application settings for them. |
| `BWebFrame` | The main frame: address, title, editing commands, zoom, find. What needs the document itself (its text, its source, editing it) is not available, because the page runs in another process. |
| `SendPageSource()` | The source as the server sent it (from the cache), or the page's markup. |
| `GetContentsAsMHTML()` | A MIME archive of the document's markup as it stands, with a `<base>` so that its links and pictures still lead to the site. Waits for the engine; it cannot be called on the application thread. |
| `WebKitInfo` | "Summit" and the engine's versions. |
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
  requires `haikuwebkit`) and replaces and conflicts with both.

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

## Not done

- A per-application user agent, fonts and JavaScript switch (no engine API).
- `BWebFrame`'s document access (`InnerText()`, `AsMarkup()`,
  `FrameSource()`) and the frame tree.
- Web Inspector for BWebView programs (`SetInspectorView()` is kept, unused).
- HTTP authentication and certificate questions: `AUTHENTICATION_CHALLENGE`
  and `SSL_CERT_ERROR` are never sent. Since 20 October the engine can ask its
  host for a name and password (`B_WEBKIT_AUTHENTICATION_REQUESTED`,
  [certificate-warnings.md](certificate-warnings.md)); this library does not
  turn that on yet.
- WebPositive's cookie window still reads its own (now unused) cookie jar;
  the pages' cookies are the engine's.
- Live GitHub/Google sign-in acceptance through a popup. The opener mechanics
  now pass the controlled tests above.

