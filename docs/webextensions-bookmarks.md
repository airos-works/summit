# chrome.bookmarks (#106)

Extensions with the `bookmarks` permission use Summit's own bookmarks (the
bookmarks bar, Other Bookmarks and the Bookmarks tab). The tree is Chrome's:
the root `"0"` holds the bookmarks bar (`"1"`, `folderType: "bookmarks-bar"`)
and Other Bookmarks (`"2"`, `folderType: "other"`). Ids are decimal strings,
stable while Summit runs (numbered again at each start, as they are not
saved). Nodes also carry Firefox's `type` (`bookmark`, `folder`,
`separator`), so separators keep the indexes Summit's folders have.

| Call | Answer |
| --- | --- |
| `get(id \| [ids])`, `getChildren(id)`, `getSubTree(id)`, `getTree()` | nodes (children for `getSubTree`/`getTree`) |
| `getRecent(n)` | the newest `n` bookmarks by date added |
| `search(query \| {query?, url?, title?})` | bookmarks and folders with every word of `query` in title or address; `url`, `title` match exactly |
| `create({parentId?, index?, title?, url?})` | a bookmark (with `url`) or folder; default parent Other Bookmarks |
| `move(id, {parentId?, index?})`, `update(id, {title?, url?})` | the changed node |
| `remove(id)`, `removeTree(id)` | `remove` refuses a non-empty folder |
| `onCreated`, `onRemoved`, `onChanged`, `onMoved` | every change, the user's own and imports included |

The bar and Other Bookmarks cannot be renamed, moved or removed; addresses
Summit does not keep as bookmarks (`data:` and the like) are refused.

## How

- Engine: `chrome.bookmarks` (`WebExtensionAPIBookmarksHaiku`; upstream's
  `WebExtensionAPIBookmarks` stays disabled) validates arguments and sends one
  `BookmarksQueryHaiku(method, argumentsJSON)` message, which becomes a
  `B_WEBKIT_BROWSER_BOOKMARKS` browser command answered with
  `RespondToBrowserCommandWithData`.
- Events: `BWebKitContext::NotifyBookmarksChanged(event, id, json)`; the
  controller dispatches `DispatchBookmarksEventHaiku` with Chrome's
  `(id, info)` arguments.
- Browser: `BrowserWindow::BookmarksCommand` and
  `BrowserWindow::ReportBookmarkChanges` (`BrowserWindowFeatures.cpp`). Every
  normal window compares the bookmarks with the last state reported whenever
  they change (one shared snapshot under a lock, so each change is reported
  once): removed nodes (the top of a removed folder only), moves to another
  folder or out of order with the siblings that stayed, new nodes, and title
  or address changes.

## Fixture

`tests/fixtures/extensions/bookmarks/`: install it and read the
`[bookmarks]` lines; then edit bookmarks in Summit to see the events.
