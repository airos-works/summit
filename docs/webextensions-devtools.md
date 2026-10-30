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
  through `inspectedWindow.eval` (the page then has Vue's devtools kit), but
  the panel stays on its loading logo: its connection through the
  background (a service worker that injects `proxy.js` with
  `scripting.executeScript`, which itself works) never completes. Not
  investigated further. The AMO (Manifest V2) package does not get as far:
  its content script adds `<script src>` elements for extension files that
  are not web-accessible, which Firefox allows for content scripts and
  Summit refuses ("The extension resource is not accessible").
