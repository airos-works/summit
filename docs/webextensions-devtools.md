# Extension developer tools: devtools_page and devtools.* (#106)

An extension with a `devtools_page` (React Developer Tools, Vue.js devtools,
Redux DevTools) gets it loaded, unseen, each time Summit's developer tools
open for a tab, as in Chrome and Firefox: one page per extension per tools
window, gone when the tools close. The panels it creates with
`devtools.panels.create()` are extra tabs of Summit's tools window, after
Performance, each showing the extension's panel page.

The devtools page and its panels' pages are extension pages with the
extension's privileges (`runtime`, `tabs`, `storage`...) and also have
`devtools`. Other extension pages and content scripts do not.

## What is there

| API | In Summit |
| --- | --- |
| `devtools.panels.create(title, iconPath, pagePath, cb)` | a tab titled `title` in the tools window showing `pagePath` (relative to the extension's root); the icon is not shown. The callback gets the panel object |
| `ExtensionPanel.onShown` / `onHidden` | when its tab is selected (once its page has loaded) and left; `onShown` hands the panel page's `window`, which the devtools page can script (React DevTools draws its UI into it) |
| `devtools.panels.themeName`, `onThemeChanged` | `"light"` or `"dark"`, from the system's document colour when the tools open (no change event fires while they are open) |
| `devtools.panels.elements.onSelectionChanged` | the Elements panel's selection changed; `$0` in `inspectedWindow.eval` is that node |
| `devtools.panels.openResource(url, line, cb)` | shows the script in the Sources panel (by its address; the line is not scrolled to); `{isError, code: "E_NOTFOUND"}` when the panel lists no such script |
| `devtools.inspectedWindow.eval(expr, options, cb)` | runs in the inspected page's main world through the tools' inspector session, as the Console would (`$0`, `inspect()`, `$()`... work): the callback gets `(result, exceptionInfo)`, the promise `[result, exceptionInfo]`. The result is the value as JSON (`returnByValue`); a throw gives `{isException: true, value}`, a failure `{isError: true, code, description}`. `frameURL` picks a frame by its origin once its script context is known; `useContentScriptContext` is refused (`E_NOTSUPPORTED`) |
| `devtools.inspectedWindow.reload({ignoreCache})` | reloads the tab (from the origin with `ignoreCache`); `userAgent` and `injectedScript` are accepted and not applied |
| `devtools.inspectedWindow.tabId` | the inspected tab's `tabs` identifier |
| `devtools.inspectedWindow.getResources(cb)` | only the page's document `[{url, type: "document"}]` |
| `devtools.network.onNavigated` | the page's main frame navigated (its new address) |
| `devtools.network.onRequestFinished` | a request finished: its HAR entry (no `getContent()`) |
| `devtools.network.getHAR(cb)` | the HAR log of the requests the Network panel holds (those since the tools opened, with the bodies it has) |

Not offered: sidebar panes (`panels.elements.createSidebarPane`,
`panels.sources`; Summit's panels are native, with no room for an
extension's page), `panels.setOpenResourceHandler`, `ExtensionPanel.onSearch`
and `createStatusBarButton`, `inspectedWindow.onResourceAdded`, the
`devtools.recorder` and `devtools.performance` namespaces. Extensions test
for these before using them (React DevTools does).

## How

Engine (`engine/patches/0001-haiku-port.patch`):

- WebProcess: a `devtools` namespace of its own on Haiku
  (`WebExtensionAPIDevToolsHaiku` and the `…InspectedWindowHaiku`,
  `…NetworkHaiku`, `…PanelsHaiku`, `…ExtensionPanelHaiku` and
  `…ElementsPanelHaiku` interfaces, C++ bindings; upstream's are Cocoa and
  need `INSPECTOR_EXTENSIONS`, which drives WebKit's own Web Inspector).
  `WebExtensionAPINamespace::isPropertyAllowed` offers it to a page that the
  UI process registered as the extension's inspector background page (the
  devtools_page) or inspector page (a panel), and only when the manifest has
  `devtools_page`. Those registrations (`WebExtensionContextProxy`'s inspector
  page maps, upstream's, now built on Haiku too) carry the inspected tab:
  `tabId`, and which pages hear the tab's events.
- Calls go to the UI process as one `DevToolsHaiku(page, method, JSON)`
  message; events come back as `DispatchDevToolsEventHaiku(type, tab, panel,
  JSON, frame)`. `onShown` gets the panel page's window through
  `toWindowObject` on its frame: the panel's page and the devtools_page run
  in the extension's process (both are related to its background page).
- UIProcess (`WebExtensionContextAPIDevToolsHaiku.cpp`): the registrations,
  `inspectorPageIdentifiers()` for new processes, `reload` and
  `getResources` itself, the rest as a `DEVTOOLS` browser command for the
  tools window.
- Host API (`WebKitContext.h`): `CreateExtensionDevToolsPages` (a hidden
  view of each loaded extension's devtools_page, registered before it
  loads, for tools identified by a number), `CreateExtensionDevToolsPanel`
  (the view of a panel `panels.create` asked for) and
  `DevToolsExtensionEvent` (panel shown/hidden, navigated,
  request-finished, theme, selection-changed); the `B_WEBKIT_BROWSER_DEVTOOLS`
  command (`panels.create`, `inspectedWindow.eval`, `network.getHAR`,
  `panels.openResource`).

Summit (`src/ui/DevToolsWindow.cpp`): when the tools open,
`BrowserWindow::ShowDeveloperTools` asks the application thread
(`kOpenDevToolsExtensionsOnApp`) to make the pages, which the tools window
keeps in a hidden view. A `DEVTOOLS` command reaches the tab's browser
window and is passed to its tools: `panels.create` makes the panel's view
on the application thread (`kCreateDevToolsPanelOnApp`) in a new tab;
`inspectedWindow.eval` is `devtools::Session::EvaluateForExtension`
(`Runtime.evaluate` with `includeCommandLineAPI` and `returnByValue`, no
console entry); `getHAR` and the request entries use the Network panel's HAR
export. The window reports its panel changes, the session's navigations,
finished requests and the Elements selection with `DevToolsExtensionEvent`.
`devtools-do panel TITLE` selects an extension's panel; the tools' state
(`devtools-do state`, MCP) lists `extensionPages` and `extensionPanels`.

## Fixture

`tests/fixtures/extensions/devtools/`: a devtools page that reports to its
background page what `devtools.*` gave it (`checks.results()`, `last(kind)`,
`count(kind)` through the MCP `evaluate` tool with `{"extension": ID}`) and
runs calls on request (`checks.ask({call: "eval", expression})`, `getHAR`,
`reload`, `openResource`, `resources`). Its panel "Fixture" shows what the
devtools page wrote into it from `onShown`.

## Verified (laptop, 30 Oct 2026, engine + app devtools106c)

React Developer Tools 8.0.0, Vue.js devtools 7.7.7 and Redux DevTools
3.2.12 (AMO; Vue also as its Chrome package) with the fixture, in one
profile:

- The tools opened on react.dev made four hidden devtools pages (fixture,
  React, Vue, Redux) and the panels Fixture, Redux, Components, Profiler and
  Suspense (Vue adds "Vue" only on a page with a development build of Vue).
- Fixture: `tabId` 28, the same as `tabs.query` gives; `themeName` "light";
  `panels.elements.onSelectionChanged` and `network.getHAR` present; the
  panel created; `eval("document.title")` → "React", a throw →
  `{isException: true, value: "Error: fixture failure"}`, an object by
  value, the promise form `["https://react.dev/", undefined]`; `$0` →
  "BODY", and after selecting another row in Elements `onSelectionChanged`
  fired and `$0` → "HTML"; an unknown `frameURL` → `E_NOTFOUND`.
  `onShown` handed the panel's window (its `marker` read back) and the
  devtools page wrote into the panel's document (screenshot). After a
  reload: `onNavigated` with the URL, 78 `onRequestFinished` entries and a
  78-entry `getHAR` log (creator "Summit"); `getResources` the document;
  `inspectedWindow.reload({ignoreCache: true})` navigated again;
  `openResource` of a page script → `{}` and the Sources panel showed it
  (the first attempt, before Sources had listed scripts, failed: hence the
  retry for 3 s), an unknown address → `E_NOTFOUND`.
- React DevTools: Components showed react.dev's component tree (screenshot),
  also in tools opened a second time. The first page of a new profile had no
  renderer in React's hook: React's background registers its MAIN-world
  hook script at startup, after that tab had loaded; a reload fixed it.
- Redux DevTools: on a page using `__REDUX_DEVTOOLS_EXTENSION__.connect()`
  the panel listed `@@INIT` and five `INCREMENT` actions with the state diff
  (screenshot); on other pages "No store found".
- Vue.js devtools (Chrome package): the "Vue" panel appears for a page with
  Vue's development build and its page injects `user-app.js` into the page
  through `inspectedWindow.eval`, but the panel stayed on its loading logo,
  and the AMO (Manifest V2) package did not get as far: its content script
  adds `<script src>` elements for extension files that are not
  web-accessible, which Summit refused. Both fixed since (below).

## Vue.js devtools: listeners that are not functions, content-script resources

Two engine changes make both Vue.js devtools 7.7.7 packages work:

- **A listener that is not a function is ignored.** Vue's panel connects
  with `chrome.runtime.connect()` and adds its port's `onMessage` listener
  before the handler exists (`port.onMessage.addListener(undefined)`), inside
  a `try` whose `catch` marks the port disconnected for good. Chrome ignores
  such a listener (`EventEmitter::AddListener` returns when the argument is
  not a function); Summit threw "not a function", so the panel never used
  its port. `addListener`, `removeListener` and `hasListener` of every
  extension event (`WebExtensionAPIEvent.idl`, now `any` arguments) ignore a
  non-function (`hasListener` gives `false`). A call with no argument still
  throws.
- **An extension's content script can load the extension's own files as
  page scripts**, as in Firefox: Vue's AMO package (`injection.js`) and its
  `devtools-overlay.js` add `<script src="moz-extension://…/dist/prepare.js">`
  (and `detector.js`) to the page, files its manifest does not list as
  `web_accessible_resources`. Firefox records who set the element's `src`;
  Summit looks at the realm of the script that is running when the script
  element starts its load (it is inserted, or its `src` set):
  `CachedScriptFetcher` (WebCore, Haiku only) takes the lexical global object
  of the current JavaScript frame, and when its world is an extension's
  content-script world (`WebExtension-<id>`) it adds a
  `Sec-Summit-Content-Script-World` header naming that world to the request
  and keeps the response out of the memory cache.
  `WebExtensionURLSchemeHandlerHaiku.cpp` then serves a file that is not
  web-accessible when the header names the content-script world of the
  extension the file belongs to; any other extension's world, or none, is
  refused as before. Page script cannot send the header (`Sec-` names are
  forbidden in `fetch()` and XHR), and the page cannot reuse the response from
  the cache. Only `no-cors` script loads (no `crossorigin` attribute) are
  marked. `SUMMIT_CONTENT_SCRIPT_RESOURCES=0` turns this off.
  A page's CSP still applies to such a script (Firefox exempts it); not
  changed.

### Verified (laptop, 30 Oct 2026, engine + app devtools106e)

- Fixture extension `csres` (content script inserts a script that is not
  web-accessible, and again from an event the page dispatches; the page then
  tries the same URLs): the content script's script loaded and ran, also
  when inserted inside the page's `dispatchEvent` (`secretRuns` 2); the
  page's own `<script src>` for that file, with and without a query, failed,
  as did `fetch()` with a forged `Sec-Summit-Content-Script-World` header and
  a `no-cors` fetch; a web-accessible file loaded for the page. With
  `SUMMIT_CONTENT_SCRIPT_RESOURCES=0` the content script's loads failed too.
- Vue.js devtools 7.7.7, unmodified, on a page with Vue 3's development
  build: the AMO package (MV2, persistent background,
  `tabs.executeScript` of `proxy.js`) and the Chrome package (MV3 service
  worker) each showed the "Vue" panel with the page's component tree (Root,
  TodoList, three TodoItems; screenshots). With
  `SUMMIT_CONTENT_SCRIPT_RESOURCES=0` the AMO package's page hook was never
  installed and no "Vue" panel appeared.
- In an extension page, `onMessage.addListener(null | undefined | 5 | "x")`
  no longer throws and `hasListener` of those is `false`; a function is
  added, found and removed as before.
- Redux DevTools and the devtools fixture in the first round's profile:
  the Redux panel listed the test page's actions, the fixture's checks
  passed as before.
- Vue's panel appears only if Vue's page hook was installed before the
  page's Vue app started: on the address Summit opens at startup, the
  content scripts can come too late (as the React hook does, above); a
  reload fixes it.
