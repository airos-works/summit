# Extension sidebars (#106)

View › Sidebar lists the loaded extensions that have a sidebar — Firefox's
`sidebar_action` (`default_panel`, `default_title`) or Chrome's
`side_panel.default_path` — and shows the chosen one in a pane on the left
of the window, beside the tabs; choosing it again, the pane's ×, or Close
Sidebar closes it. Each window has its own; private windows and pop-ups
have none. Full screen hides it with the rest of the browser.

The page is an extension page with the extension's privileges, as in
Firefox: it uses the tabs, storage and other APIs it has permission for,
and what it opens with `window.open` opens as from the shown tab.

## How

The engine reports `sidebar_url` and `sidebar_title` (localised) with a
loaded extension (`LoadPreparedExtension`'s LOADED); the application keeps
them (`ExtensionController::Entry`) and publishes the list
(`SharedProfile::ExtensionSidebars`). A window asks the application
(`kOpenSidebarOnApp`), which makes the page's view on WebKit's application
thread with `BWebKitContext::CreateExtensionView` and hands it to the
window (`BrowserWindow::ShowSidebar`), as it does for extension tabs.

## sidebarAction

Firefox's `sidebarAction` is there for an extension with a
`sidebar_action`: `open()`, `close()`, `toggle()` and `isOpen()` act on its
sidebar in the window named (`windowId`, or a `tabId`'s window), else the
front window; `setPanel`/`getPanel` and `setTitle`/`getTitle` change and
read the page and title for that window (null goes back to the manifest's;
a panel must be a page of the extension), the open sidebar following at
once; `setIcon` is accepted (the pane shows no icon). Each call is a
`SIDEBAR` browser command (`BrowserWindow::SidebarCommand`). `open()` needs
no user gesture here, unlike Firefox.

Not yet: remembering the open sidebar across restarts.

## sidePanel

Chrome's `sidePanel` is there for an extension with the `sidePanel`
permission (now one Summit grants). It shows its panel in the same sidebar:
the namespace object is a second `WebExtensionAPISidebarActionHaiku` whose
calls go to the UI process as `sidePanel.METHOD`, which
`WebExtensionContext::sidebarActionHaiku` turns into the same `SIDEBAR`
commands, so the application needs nothing new.

- `open({windowId, tabId})` and `close(...)` act on that window's sidebar
  (or the tab's window; the front window without either). Chrome needs one
  of them; Summit does not, and needs no user gesture.
- `setOptions({tabId, path, enabled})`: `path` (a page of the extension,
  else rejected) becomes the panel of the tab's window, or of the front
  window without a `tabId` (Summit's sidebar belongs to a window, not a
  tab, so per-tab panels follow the window); `enabled: false` closes it.
- `getOptions({tabId})` answers `{enabled, path}` for that window: the
  panel's path relative to the extension, `enabled` while there is one
  (`enabled: false` is not remembered).
- `setPanelBehavior({openPanelOnActionClick})` / `getPanelBehavior()`: kept
  in the extension's state (`SidePanelOpensOnActionClick` in its
  `State.json`), so it lasts across launches as in Chrome. While it is on,
  the extension's toolbar action opens or closes its panel in the tab's
  window instead of its popup or `onClicked`
  (`WebExtensionContext::performAction`).
- The object also has `sidebarAction`'s `toggle()` and `isOpen()`.
  Firefox's `sidebarAction` is still offered only to an extension with a
  `sidebar_action`.

Checked on the X399 (27 Oct) with `tests/fixtures/extensions/side-panel`
(`side_panel.default_path` `panel.html`): `chrome.sidePanel` was there and
`browser.sidebarAction` was not; `getOptions()` gave `{enabled: true,
path: "panel.html"}`; `open()` showed the panel (it reported itself), as
did `open({tabId})`; `setOptions({path: "second.html?x=1"})` loaded that
page in the open panel and `getOptions()` answered it; `setOptions({enabled:
false})` and `setOptions({tabId, path, enabled: false})` closed it; an
outside `path`, a non-boolean `openPanelOnActionClick` and an unknown
`windowId` were rejected; `setPanelBehavior({openPanelOnActionClick:
true})` was still set after a restart. The sidebar fixture's
`sidebarAction` (`open`, `toggle`, `close`, `isOpen`) behaved as before,
without a `sidePanel`. Not checked: the toolbar click with
the behaviour on (the X399's display was down, and the MCP `press_key`
goes to the page, not the window's shortcuts).

## Fixture

`tests/fixtures/extensions/sidebar/`: a sidebar that lists the window's
tabs, kept up to date (`#count` gives how many). Its background page has
`checks` for the MCP `evaluate` tool (`{"extension": ID, ...}`): `open`,
`close`, `toggle`, `isOpen`, `panel`, `title`, `rename(title)`,
`retarget()` (the panel with `?second`) and `shown()`, which asks the open
panel what it shows (`#count`, address and size).

Verified on the X399 (25 Oct): `open()` showed the panel (278×503 beside
the pages) listing the window's 3 tabs, 4 once a tab was opened;
`isOpen()` followed `open`, `close` and `toggle`; `setPanel` moved the open
panel to `sidebar.html?second` and `getPanel` answered it; `setTitle`/
`getTitle` round-tripped; View › Sidebar's item (`sbtg` with the
extension's identifier) and Close Sidebar (`sbcl`) opened and closed it.
An extension installed while Summit runs had no sidebar until a restart:
the installer did not pass the sidebar on (fixed with this change).
