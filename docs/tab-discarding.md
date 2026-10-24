# Unloading tabs (#116, #106)

A background tab can be unloaded: its page and what it holds (the web
process, if no other page uses it, its GPU contexts, tiles and decoded
images) are freed, and the tab keeps its place, title and icon until it is
selected, when its address loads again. Restored sessions' tabs already
waited this way; unloading puts a loaded tab back in that state.

- **Unload Tab** in a tab's menu.
- `tabs.discard(tabId?)` for extensions (Chrome's form): without a tab,
  the least recently selected background tab. The tab gets a new id, as in
  Chrome; `Tab.discarded` and `tabs.query({discarded})` report unloaded
  tabs, restored-session tabs included.

Not unloaded: the shown tab, tabs playing sound, capturing the camera,
microphone or screen or keeping the screen on, Summit's own pages and
extension pages, and tabs with Developer Tools, a crash or load-error page,
Reader view, Picture in Picture or full screen. Unloading forgets the tab's
back and forward lists and its translation.

## How

`BrowserWindow::DiscardTab` (`BrowserWindow.cpp`) puts a new `BWebKitView`
in the tab's card (set up by `AttachPageView`, as `CreateTab` sets up its
views), deletes the old view and sets `Tab::deferredURL`, which
`SelectTab` loads. `BWebKitContext::SetBrowserTabDiscarded` tells the
engine's tab registry (`BrowserTabRegistryHaiku::setDiscarded`), which
`WebExtensionTab::isDiscarded` reads. `tabs.discard` sends
`TabsDiscardHaiku`, forwarded as a `B_WEBKIT_BROWSER_DISCARD_TAB` browser
command answered with the new view.

Not done yet: unloading by itself under memory pressure or after a tab has
been in the background a long time.

## Fixture

`tests/fixtures/extensions/tab-discard/`: install it, read memory during the
first 20 s (three loaded background tabs) and after the discards, and read
the `[discard]` lines.
