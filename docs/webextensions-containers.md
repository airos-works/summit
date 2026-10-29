# Containers and contextualIdentities (#106)

Tabs can belong to a container, as in Firefox: each container keeps cookies
and site storage (local storage, IndexedDB, service workers, caches) of its
own, apart from the profile's and from the other containers'. A new profile
has Firefox's four (Personal, Work, Banking, Shopping); extensions such as
Firefox Multi-Account Containers make, change and remove them.

## In the browser

- File › New Container Tab › *container* opens a tab in it; a page's link menu
  has Open Link in New Container Tab.
- A container's tab has a line of its colour along the bottom of the tab.
- Links, `target=_blank` and `window.open()` from a container's page open in
  the same container (also pop-up windows); Duplicate Tab keeps the
  container, and so does a tab dragged out of its window or moved with Move
  Tab to New Window (both move the live page, its history with it).
- Clearing data clears every container's too, as Firefox does: Preferences ›
  Privacy (cookies and site data, cache, a time range or chosen sites) and
  extensions' `browsingData` without a `cookieStoreId`. The Site data list
  shows each site once, with its data in all containers added up.
- The session keeps each tab's container: restored tabs open in it again
  (a tab of a container that is gone opens without one).
- When a container is removed (by an extension), its tabs close and its data
  is erased; its directory goes at the next start.
- Private windows have no containers.
- `SUMMIT_CONTAINERS=0` starts the profile without containers (the engine
  then answers `contextualIdentities` with Firefox's "Contextual identities
  are currently disabled").

## For extensions

Store identifiers are Firefox's where Firefox has them: the profile's own
store is `firefox-default`, a container's is `firefox-container-<id>`
(private stores keep `ephemeral-<n>`; the older `persistent-<n>` of the
profile's store is still accepted).

| API | Summit |
| --- | --- |
| `contextualIdentities.query({name?})`, `get(id)` | the containers, in order: `{name, icon, iconUrl, color, colorCode, cookieStoreId}` |
| `create({name, color, icon})`, `update(id, {name?, color?, icon?})`, `remove(id)` | as Firefox (Firefox's colours and icons; unknown ones are refused); `remove` closes the container's tabs and erases its data |
| `move(ids, position)` | reorders; fires `onUpdated` for each moved container |
| `getSupportedColors()`, `getSupportedIcons()` | the palette (`{color, colorCode}`) and icons (`iconUrl` empty: extensions draw their own) |
| `onCreated`, `onUpdated`, `onRemoved` | `{contextualIdentity}`, for changes by any extension |
| `Tab.cookieStoreId` | `firefox-default`, `firefox-container-<id>` or `ephemeral-<n>` |
| `tabs.create({cookieStoreId})` | opens the tab in that container (needs `cookies`, as in Firefox); a private window refuses a non-private store |
| `tabs.query({cookieStoreId})` | one store or a list |
| `cookies.*({storeId})` | a container's cookies, also while none of its tabs is open |
| `cookies.getAllCookieStores()` | the profile's store, every container's, and private stores with open tabs, each with its tabs |
| `browsingData.remove*({cookieStoreId})` | a container's data (cookies, local storage, IndexedDB, caches, service workers; `since` and `hostnames`/`origins` honoured); `firefox-default` is the browser's usual removal |

Two fixes Multi-Account Containers needed besides: `permissions.contains()`
answers `false` for a permission Summit does not have (it asked for
`proxy` when its panel opened and stopped at the error), and
`tabs.query({windowId: -1})` (`WINDOW_ID_NONE`, from
`windows.onFocusChanged`) answers no tabs instead of an error.

Not done: Firefox's `firstPartyDomain` (first-party isolation) is not
supported; cookies with it are neither filtered nor reported. A container is
the profile's in everything else: history, bookmarks, permissions,
certificate exceptions and downloads are shared, as in Firefox.

Multi-Account Containers logs "menus.create(): identifier is already used"
now and then: it recomputes its page menu on `tabs.onActivated` and on the
page's `webRequest.onCompleted`, removing its items and creating them again
after an `await`; a page that loads within that time (a new tab of a fast
site: the two events came 36 ms apart) runs the two out of order. Summit
fires each event once and starts an extension with no menu items (checked by
restarting one); Firefox reports the same failure for the same race.

## How

- Engine: `ContextualIdentitiesHaiku` (UIProcess/haiku) keeps the list in
  `<profile>/WebKit/Containers/containers.json` and a persistent
  `WebsiteDataStore` per container under `Containers/<id>/` (Cache, Data),
  made on first use. A container's store has a session identifier of its own
  range (2^48 + id), so both processes turn it into `firefox-container-<id>`
  and back without a table (`WebExtensionCookieStoreIdentifier`).
  `firefox-default` is `PAL::SessionID::defaultSessionID()`, which the Haiku
  port never gives a store; the UI process reports the profile's store so.
- `WebViewContextHaiku::createContainer` gives a container's pages that store
  and everything else of the profile's context (one process pool, the
  extension controller, downloads, site permissions, certificate
  exceptions). `BWebKitContext(profile, container)` is its public face;
  views of the profile and of its containers are tabs of one window
  (`SetBrowserWindowTabs` accepts them together).
- Host API: `BWebKitContext::GetContainers`, `SetContainerListener`
  (`B_WEBKIT_CONTAINERS_CHANGED`), OPEN_TAB's `container`, and
  `B_WEBKIT_NEW_PAGE_REQUESTED`'s `container`.
- Summit: `summit::Containers` (src/ui/Containers.cpp) keeps a context per
  container, made on the application thread, for windows to open tabs in;
  `BrowserWindow::Tab::container`, the session's `containers` list.
- The extension API: `WebExtensionAPIContextualIdentities` →
  `ContextualIdentitiesQueryHaiku` (UIProcess/Extensions/haiku/
  WebExtensionContextAPIContextualIdentitiesHaiku.cpp); events through
  `DispatchContextualIdentitiesEventHaiku`.

## Verified (laptop, 30 Oct 2026, engine + app containers4)

- Isolation: tabs of the profile, Personal and Work logged in to one page as
  three users (a cookie and `localStorage` each); other tabs of each store
  saw only their own user, Banking saw neither.
- Persistence: after quitting and starting again the tabs came back in their
  containers and each still saw its own cookie and local storage.
- Multi-Account Containers 8.3.8 (AMO): starts without errors from the API;
  its panel lists the four containers; clicking one opens a tab in it;
  "Always open this site in Banking" shows its confirmation page for a new
  tab of the site and *Open in Banking container* reopens it in Banking;
  deleting a container closes its tabs and removes it.
- API: create/update/move/remove/query/get with events; bad colours and
  unknown containers refused; `tabs.create`/`query` with `cookieStoreId`;
  `cookies.getAll`/`set` on container stores (none leaking into
  `firefox-default`); `getAllCookieStores()` with all containers;
  `browsingData.removeCookies`/`removeLocalStorage({cookieStoreId})` clears
  that container only.

Not checked by eye: the File menu's New Container Tab submenu and the link
menu's Open Link in New Container Tab (the same code path as the MCP
`open_tab` tool's `container`, which was used). The tab colour line was.

Test kit: `tests` are by hand: an echo page that sets a cookie and
`localStorage` from `?login=NAME`, a profile with the extension unpacked
(catalog entry, `SUMMIT_EXTENSION_DEVELOPER_MODE=1`), and the MCP
`open_tab` tool's `container` argument.
