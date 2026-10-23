# Extension tab and window commands

Extensions can now ask the native browser to open, close, select, navigate and
reload tabs and to open, close, focus and minimize windows. The engine never
changes a host tab by itself: every change is a *browser command* sent through
the public SDK to the application, which performs it in its own tab model and
answers. This enables `tabs.create/update/remove/reload/goBack/goForward`,
`windows.create/update/remove`, and adds `tabs.insertCSS/removeCSS`,
`tabs.connect`, the `scripting` namespace implementation, tab lifecycle events
and a clean failure for native messaging.

Status: **syntax-checked only**. Nothing here has been linked or run; see
"Verification" below. The engine delta is `.cache/tabs-commands.patch` and the
Summit delta is `.cache/tabs-commands-app.patch`.

## SDK protocol (`WebKit/WebKitContext.h`)

```cpp
status_t BWebKitContext::SetBrowserCommandListener(const BMessenger&);   // application thread, local target
void BWebKitContext::RespondToBrowserCommand(uint64 identifier, status_t status,
    const std::vector<BWebKitView*>& createdViews = { }, const char* error = nullptr);  // any looper
```

The listener receives `B_WEBKIT_BROWSER_COMMAND` (`'wbcm'`) with:

| Field | Type | Meaning |
| --- | --- | --- |
| `identifier` | uint64 | Pass back to `RespondToBrowserCommand`. |
| `command` | uint32 | One of the `B_WEBKIT_BROWSER_*` values below. |
| `deadline` | int64 | `system_time()` microseconds; 30 s after the request. |
| `extension_identifier`, `extension_name` | string | Attribution only. |
| `view` + `page_identifier` | messenger + uint64 | A tab: the messenger of its `BWebKitView`. Repeated for several tabs. |
| `window` + `window_identifier` | messenger + uint64 | A window: the messenger given to `SetBrowserWindowTabs`. Parallel to `view` when repeated. |

| Command | Extra fields | Expected result |
| --- | --- | --- |
| `B_WEBKIT_BROWSER_OPEN_TAB` | `url` (absent: new-tab page), `active`, `index` (int32, -1 appends), optional `window`, optional `opener`/`opener_page_identifier` | Exactly one created view. |
| `B_WEBKIT_BROWSER_CLOSE_TABS` | repeated `view`/`window` | `B_OK` only when every tab closed; `B_CANCELED` when the user kept one. |
| `B_WEBKIT_BROWSER_ACTIVATE_TAB` | `view`, `window` | Select the tab; do not raise the window. |
| `B_WEBKIT_BROWSER_NAVIGATE_TAB` | `view`, `url` | Start the load. |
| `B_WEBKIT_BROWSER_RELOAD_TAB` | `view`, `bypass_cache` | Start the reload. |
| `B_WEBKIT_BROWSER_GO_BACK` / `GO_FORWARD` | `view` | Start the history navigation. |
| `B_WEBKIT_BROWSER_OPEN_WINDOW` | repeated `url`, repeated `view` (tabs to adopt), `focused`, `type` (`normal`/`popup`), `state`, `private` | One or more created views, all in the new window. |
| `B_WEBKIT_BROWSER_CLOSE_WINDOW` / `FOCUS_WINDOW` | `window` | |
| `B_WEBKIT_BROWSER_SET_WINDOW_STATE` | `window`, `state` (`normal`, `minimized`, `maximized`, `fullscreen`) | |

Rules for hosts:

- Answer every command exactly once. Unknown commands get `B_NOT_SUPPORTED`.
- Make the change visible with `SetBrowserWindowTabs` **before** answering.
  Both calls queue to the WebKit main thread in order, so the engine can resolve
  the created views as tabs, `tabs.query` already reflects the change and the
  returned `tabs.Tab` has its final window and index.
- Pass created views from the looper that owns them, while they are alive.
- `B_WEBKIT_BROWSER_COMMAND_CANCELLED` (`'wbcc'`, `identifier` only) means the
  engine stopped waiting (deadline, extension unload, listener change). A late
  answer is ignored; no answer is needed.
- An optional `error` string is shown to the extension (trimmed to 512 chars).

Failure behaviour seen by extensions (promise rejection or `runtime.lastError`,
never a hang): no listener → "it is not implemented"; no answer within 30 s →
"the browser did not respond in time"; extension unloaded or listener replaced →
"the request was cancelled"; at most 256 commands may be pending per profile.

## Engine semantics

- `tabs.create`, `tabs.update({url})` and `windows.create({url})` accept only
  `http(s):`, `about:blank` and the calling extension's own pages. `javascript:`,
  `data:`, `file:`, other extensions' and internal URLs are rejected before the
  host is asked (this matches Firefox and is stricter than upstream WebKit, which
  leaves URL policy to the embedder).
- Window and tab lookups keep the existing private-browsing rule: objects of a
  profile the extension may not access are "not found". `windows.create` rejects
  an `incognito` value that differs from the calling profile, because a native
  profile is entirely private or entirely persistent.
- `tabs.insertCSS`, `scripting.insertCSS` and `scripting.executeScript` require
  host or `activeTab` access to the committed document; the `tabs` permission
  never authorizes injection, and `file:` documents also need file access.
  Injected style sheets carry an allowlist of the extension's current grants, so
  they stop applying if the tab later navigates somewhere it cannot access
  (upstream attaches them without a URL limit).
- `scripting.executeScript` reuses the native tab-script executor: isolated
  world only (`world: "MAIN"` is rejected), `files` run in order and the last
  supplies the result, `func`/`args` travel as source text plus JSON, results are
  structured clones with `frameId`/`documentId`, `injectImmediately` maps to
  document-start. `tabs.insertCSS/removeCSS` remain Manifest V2 only.
- `tabs.onCreated`, `onRemoved` (with `isWindowClosing`), `onActivated`,
  `onAttached` and `onDetached` are now derived from committed tab-registry
  transitions, ordered as browsers do (tab removal before window removal, window
  creation before its tabs). Tabs that exist when an extension loads are not
  reported as created.
- `runtime.sendNativeMessage` rejects, and a `runtime.connectNative` port
  disconnects with `runtime.lastError`/`port.error`, both saying
  `No such native application <name>` (Firefox's wording). Previously the first
  never settled and the second never disconnected.

## Summit behaviour

- Summit is single-window. `windows.create` opens the requested pages as tabs in
  the existing window and reports that window; `type`, `state` and geometry are
  ignored. `windows.remove` closes only the tabs Summit opened for that
  extension's `windows.create` calls and otherwise answers `B_NOT_ALLOWED`: an
  extension can never close the browser window. `windows.update` supports
  `focused: true` and `state: normal|minimized`.
- `tabs.remove` goes through the normal asynchronous close handshake
  (`RequestClose` → beforeunload → `CommitClose`), one tab at a time, without
  blocking a looper. The command succeeds when every tab is gone and fails with
  "The tab was kept open." when the user stays on a page or a window close is
  cancelled. A prompt left open past the deadline rejects the promise while the
  close itself may still complete. Tabs in several windows get one command per
  window (a window closes only its own tabs); the call succeeds when all of
  them have, and otherwise reports the first failure.
- Extension pages open in the privileged extension view through the existing
  application-thread path; the command identifier travels with that request.
- Failures of extension-requested tabs are reported to the extension, not shown
  as alerts.

## Zoom (#106)

`tabs.getZoom()` reads the tab's page zoom. `tabs.setZoom()` sends
`B_WEBKIT_BROWSER_SET_ZOOM` (`zoom`: 0.25 to 5, or 0 for the default) to the
browser, which zooms the tab the way its own zoom commands do: the zoom
indicator shows it and the zoom is remembered for the site, so the site's other
tabs follow (`BrowserWindow::SetTabZoom`). Values outside that range throw.
`tabs.onZoomChange` reports every page zoom change of a tab the extension can
see (from `WebPageProxy::setPageZoomFactor`), with `zoomSettings`
`{mode: "automatic", scope: "per-origin"}`, which is how Summit remembers zoom.
Verified on the X399 (24 Oct, zc106 build): `onZoomChange` fired 1 → 1.5 and
1.5 → 1 with `automatic/per-origin`. `getZoomSettings`/`setZoomSettings` are
not implemented. Fixture: `tests/fixtures/extensions/tab-zoom` (open
`https://example.com/?zoom`).

## Pinned tabs (#106)

`Tab.pinned` is the browser's own pinned state: Summit reports each change with
`BWebKitContext::SetBrowserTabPinned()` (Pin Tab, restored sessions), and
`tabs.onUpdated` reports `{pinned}`. `tabs.update({pinned})` sends
`B_WEBKIT_BROWSER_SET_PINNED`, which the browser handles like its tab menu's
Pin Tab (the tab moves to the pinned group). `tabs.create({pinned: true})`
pins the new tab once it exists, and `tabs.query({pinned})` filters by it.
Fixture: `tests/fixtures/extensions/tab-pin` (open `https://example.com/?pin`). Verified
on the X399 (24 Oct, pin106 build ca81f4b): `pinned=false`, then after
`update({pinned: true})` an `onUpdated {pinned: true}` and `get`/`query`
agreeing, then `onUpdated {pinned: false}` after unpinning.

## Duplicate (#106)

`tabs.duplicate()` opens a new tab at the original's address, after it (or at
`index`), active unless `active: false`, with the original as its opener. It
goes through the same `OPEN_TAB` command as `tabs.create()`. Unlike Chrome,
the copy does not get the original's back/forward list.

`openerTabId` (since opener106): the controller remembers which page opened
which, from `WebPageProxy::createNewPage()` (links with `target`,
`window.open()`) and from `tabs.create()` with `openerTabId` (and so
`tabs.duplicate()`), while both pages are open. A tab reports its opener's
identifier when the extension can see the opener. Verified on the X399 (24 Oct,
opener106 build): a `target="_blank"` tab reported its page's tab as opener,
`tabs.duplicate()`'s copy reported the original, and tabs the browser opened
itself reported none.

Verified on the X399 (23 Oct, zoom106 build 538d837) with
`tests/fixtures/extensions/tab-zoom`: `getZoom` 1, then 1.5 after
`setZoom(1.5)`, 1 after `setZoom(0)`; `duplicate` opened the same address at
index 2 after the original at 1, and removing it worked; `setZoom(9)` threw
the range error.

## Not implemented

`tabs.move`, `tabs.detectLanguage`,
`tabs.toggleReaderMode`, `tabs.captureVisibleTab`, `tabs.getSelected`,
`tabs.onMoved`, `openerTabId` on `tabs.update`, window geometry
(`left/top/width/height` are validated and dropped), `maximized`/`fullscreen`
in Summit, cache-bypassing reload in Summit (`bypassCache` reloads normally:
`BWebKitView` has no such call yet), `scripting` in the main world, and real
native messaging hosts. Navigating an ordinary tab to an extension page (or the
reverse) is refused by Summit because the two use different view types.

The `scripting` namespace is implemented but **not yet exposed**: that needs the
edits listed in the hand-off report to `WebExtensionAPINamespace.idl/.h/.cpp`.

## Verification

Done: every changed or new engine translation unit, the regenerated IPC
receiver, serializers and JS bindings, and Summit's `main.cpp` and
`BrowserWindow.cpp` were compiled with `-fsyntax-only` in the QEMU guest, in a
private mirror under `/SummitExtensions/summit/claude-tabs/`, using the frozen
build's flags without its precompiled header. See the hand-off report for the
exact unit list and results.

Not done: linking, any run of the engine or Summit, any extension exercising
these paths, the Cocoa adapter edits (never compiled here), and native unit tests
for the new parsers and the command broker.

## Manual test recipe

1. Apply both patches, make the namespace edits, rebuild the engine and Summit.
2. Load an MV2 test extension with `tabs`, `<all_urls>` and `nativeMessaging`
   whose background page runs, step by step:
   - `browser.tabs.create({url: "https://example.org", active: false})` → a
     background tab opens; the promise yields a `Tab` with `windowId`, `index`,
     `active: false`; `tabs.onCreated` fired once; `tabs.query({})` lists it.
   - `browser.tabs.update(id, {active: true})` → tab selected, `onActivated`
     carries `previousTabId`; `{url: "https://example.com"}` → navigates;
     `{url: "javascript:1"}` → rejects without touching the tab.
   - `browser.tabs.reload(id)`, `goBack(id)`, `goForward(id)`.
   - `browser.tabs.remove([id])` → resolves after the tab is gone, `onRemoved`
     fires. Repeat on a page with a `beforeunload` handler and choose Stay → the
     promise rejects and the tab remains.
   - `browser.windows.create({url: ["https://a.example", "https://b.example"]})`
     → two tabs in the same window, first selected; `windows.remove(thatId)` →
     only those two close. `windows.remove` without a prior create → rejects.
   - `browser.tabs.insertCSS(id, {code: "body{outline:4px solid red}"})` then
     `removeCSS` with the same details; navigate to a site without permission and
     confirm the rule no longer applies.
   - `const p = browser.runtime.connectNative("com.example.none");` with a
     `p.onDisconnect` listener → it fires promptly and `p.error.message` /
     `runtime.lastError.message` is `No such native application com.example.none`;
     `browser.runtime.sendNativeMessage("com.example.none", {})` rejects.
3. Stop answering commands (comment out the `B_WEBKIT_BROWSER_COMMAND` case in
   `BrowserWindow::MessageReceived`) → `tabs.create` rejects after 30 s with "the
   browser did not respond in time". Clear the listener → it rejects at once with
   "it is not implemented".
4. With the real 1Password build: unlock, use "Open & Fill", and the
   save-login prompt; confirm the unlock popup opens as a tab and is closed again
   by the extension without closing Summit.
