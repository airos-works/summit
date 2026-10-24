# menus.onShown, onHidden and refresh (#106)

Firefox's `menus.onShown(info, tab)` fires when Summit opens a page's
context menu, for every extension allowed on the page (with or without items
in it), and `menus.onHidden()` when the menu goes. While it is open an
extension can change its items (`menus.update`, `create`, `remove`) and call
`menus.refresh()`: the menu rebuilds its extension items in place.

`info` carries Firefox's fields for what was clicked: `menuIds` (the
extension's items shown), `contexts`, `editable`, `frameId`, `pageUrl`,
`frameUrl`, `linkUrl`/`linkText`, `srcUrl`/`mediaType`, `selectionText` and
`viewType` ("tab"). Only extensions with host permission for the clicked
frame (or activeTab, for the main frame) are told, as they are the ones that
get items in the menu; Firefox also tells the others, without the URLs.

Menus Summit draws itself (the tab strip, the extension action menu) do not
fire the events; neither does Firefox's `menus.overrideContext`, which is
not supported.

## How

`addExtensionMenusHaiku` (WebView.cpp) remembers the shown menu's context and
which items each extension had in it, and fires
`WebExtensionContext::fireMenusShownEventHaiku`. `menus.refresh` sends
`MenusRefreshHaiku` to the UI process, which posts the extension items again
to the view's owner as `B_WEBKIT_CONTEXT_MENU_EXTENSIONS`.
`BrowserWindow::RefreshPageMenuExtensions` forwards them to the open menu
(a `CancellableMenu`, which tells the window its messenger when it opens and
when it closes), and the menu replaces its extension items in its own
thread. When it closes, `BWebKitView::ContextMenuClosed` fires onHidden.

## Fixture

`tests/fixtures/extensions/menus-shown/`: right-click a page. The fixture's
item reads "Shown N times, on …" while the menu is open (set in onShown,
then `menus.refresh()`), and `[menus]` console lines (also
`globalThis.events` in the background page) log onShown, the refresh and
onHidden.
