# chrome.search (#106)

Extensions with the `search` permission search with Summit's engines
(DuckDuckGo, Google, Bing; the default is the one chosen in Summit).

| Call | Answer |
| --- | --- |
| `chrome.search.query({text, disposition?, tabId?})` | searches with the default engine in the tab (`CURRENT_TAB`, the default: `tabId` or the active tab), a new tab (`NEW_TAB`) or a new window (`NEW_WINDOW`) |
| `search.search({query, engine?, disposition?, tabId?})` | Firefox's: the same, with an engine named as `search.get()` names it |
| `search.get()` | Firefox's: `[{name, isDefault, favIconUrl}]` |

`tabId` only goes with `CURRENT_TAB`; Firefox's lower-case dispositions are
accepted.

## How

`WebExtensionAPISearch` sends `SearchQueryHaiku(method, argumentsJSON, tab)`;
the UI process adds the tab's view and forwards a `B_WEBKIT_BROWSER_SEARCH`
browser command, which the application routes to the tab's window (else the
front window). `BrowserWindow::SearchCommand` (`BrowserWindowFeatures.cpp`)
builds the address from the engine's prefix and answers with
`RespondToBrowserCommandWithData`.

## Fixture

`tests/fixtures/extensions/search/`: install it and read the `[search]` lines.

Verified on the X399 (24 Oct, bm106 build): `get`, `query` in a new tab,
`search` with Bing in a named tab, and the refusals (unknown engine, bad
disposition, missing tab, empty text).
