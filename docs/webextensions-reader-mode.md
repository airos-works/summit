# Reader view for extensions (#106)

`tabs.toggleReaderMode(tabId?)` switches Summit's Reader view on or off for
the shown tab (Firefox's API), and tabs report Firefox's `isArticle`
(Summit found an article: the Reader button shows) and `isInReaderMode`;
`tabs.onUpdated` fires when either changes.

Reader view is switched only for the shown tab, as Summit's own button does;
a background tab, or a page without an article, is refused.
`tabs.create({openInReaderMode: true})` (Firefox) opens the tab and shows it
in Reader view when Summit's article check on its first page succeeds and the
tab is shown by then (`OPEN_TAB`'s `reader`; `BrowserWindow`'s
`Tab::openInReader`). A page without an article, or a background tab, just
loads.

## How

Summit reports each tab's state with `BWebKitContext::SetBrowserTabReader`
from `BrowserWindow::RefreshChrome` (only when it changed); the tab registry
keeps it for `WebExtensionTab::isReaderModeAvailable`/`isReaderModeActive`.
`setReaderModeActive` sends a `B_WEBKIT_BROWSER_SET_READER` browser command,
which `BrowserWindow::ToggleReader` answers.

## Fixture

`tests/fixtures/extensions/reader-mode/`: install it and read the `[reader]`
lines.

Verified on the X399 (24 Oct, rm106 build): a Wikipedia article reported
`isArticle: true`; `toggleReaderMode` switched Reader view on
(`isInReaderMode: true`) and off again, `tabs.onUpdated` reporting each
change; a background tab was refused; an unloaded tab reported `status:
"unloaded"`. Summit's article test also accepts example.com. (`isArticle`
stayed true in Reader view only after a fix that was not re-run on the
device.)
