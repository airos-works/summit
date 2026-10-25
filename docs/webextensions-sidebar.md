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

Not yet: remembering the open sidebar across restarts, and Chrome's
`sidePanel` API (its panel opens from View › Sidebar).

## Fixture

`tests/fixtures/extensions/sidebar/`: a sidebar that lists the window's
tabs, kept up to date (`#count` gives how many).
