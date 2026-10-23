# chrome.sessions (#106)

Extensions with the `sessions` permission see the tabs and windows closed
while Summit runs (25 at most, private windows' left out), as Chrome's
`sessions` API does, and keep Firefox's per-tab and per-window values.

| Call | Answer |
| --- | --- |
| `getRecentlyClosed({maxResults?})` | Session objects, newest first: `{lastModified, tab}` or `{lastModified, window: {tabs}}` (a window closed with one tab counts as a tab) |
| `restore(sessionId?)` | reopens that session (default: the newest) in the front window, or a new window for a window; answers with the Session, whose tab has no `id` yet |
| `forgetClosedTab(windowId, sessionId)`, `forgetClosedWindow(sessionId)` | Firefox's: drops it |
| `getDevices()` | `[]`: Summit does not sync |
| `setTabValue`/`getTabValue`/`removeTabValue`, `setWindowValue`/... | Firefox's; kept in the UI process while Summit runs (not across restarts yet) |
| `onChanged` | a tab or window closed, restored or forgotten (Reopen Closed Tab too) |

Without the `tabs` permission or access to the site, closed tabs come
without `url`, `title` and `favIconUrl`, as in Chrome.

## How

`WebExtensionAPISessions` sends `SessionsQueryHaiku` (forwarded as a
`B_WEBKIT_BROWSER_SESSIONS` browser command; the UI process strips what the
extension may not see) and `SessionsValueHaiku` (answered in the UI process
from `WebExtensionContext::m_sessionValuesHaiku`). Summit records closes in
`BrowserWindow::RecordClosedSession` (one list for all windows) and reports
changes with `BWebKitContext::NotifySessionsChanged`.

## Fixture

`tests/fixtures/extensions/sessions/`: install it and read the `[sessions]`
lines.
