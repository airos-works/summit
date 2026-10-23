# chrome.history (#106)

Extensions with the `history` permission use Summit's own history (the
History tab and address-field suggestions). Private windows leave none.

| Call | Answer |
| --- | --- |
| `search({text, startTime?, endTime?, maxResults?})` | pages last visited in the range (default: the last 24 hours, 100 at most) whose address or title contains `text` (empty: all) |
| `getVisits({url})` | one VisitItem per retained visit time (`transition` is always `link`) |
| `addUrl({url, title?, visitTime?})` | records a visit |
| `deleteUrl({url})` | removes the page |
| `deleteRange({startTime, endTime})` | removes pages last visited in the range, with their earlier visits |
| `deleteAll()` | clears history, as History › Clear History |
| `onVisited` | each visit Summit records outside private windows, and `addUrl` |
| `onVisitRemoved` | `deleteUrl`, `deleteRange`, `deleteAll` (Summit's own Clear History and Preferences do not report yet) |

Item `id`s are a hash of the address; `typedCount` is 0. Times are
milliseconds since the epoch.

## How

- Engine: `chrome.history` (`WebExtensionAPIHistory`) validates arguments and
  sends one `HistoryQueryHaiku(method, argumentsJSON)` message. The UI process
  turns it into a `B_WEBKIT_BROWSER_HISTORY` browser command (`method`,
  `arguments`); the browser answers with
  `BWebKitContext::RespondToBrowserCommandWithData` (JSON).
- Events: the browser calls `BWebKitContext::NotifyHistoryChanged("visited" |
  "removed", json)`; the controller dispatches `history.onVisited` /
  `onVisitRemoved` (new listener types at the end of the Haiku block) to
  extensions with the permission.
- Browser: `BrowserWindow::RecordVisit` (every visit) and
  `BrowserWindow::HistoryCommand` (`BrowserWindowFeatures.cpp`).

## Fixture

`tests/fixtures/extensions/history/`: install it, open
`https://example.com/?history`, and read the `[history]` lines.
