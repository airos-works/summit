# webNavigation: same-document navigations and new tabs (#106)

Summit used to register `webNavigation.onHistoryStateUpdated`,
`onReferenceFragmentUpdated` and `onCreatedNavigationTarget` without ever firing
them. 1Password listens to `onCreatedNavigationTarget`. Single-page sites
change their address with `history.pushState()`, and extensions that follow
those sites (SponsorBlock, Vimium, many content filters) listen to
`onHistoryStateUpdated`.

| Event | Fired when | Details |
| --- | --- | --- |
| `onReferenceFragmentUpdated` | a frame moves to another fragment of its document: a `#anchor` link, `location.hash`, back/forward between fragments | as `onCommitted`: `tabId`, `frameId`, `parentFrameId`, `url`, `timeStamp` |
| `onHistoryStateUpdated` | `history.pushState()` / `replaceState()`, and back/forward between their entries | the same |
| `onCreatedNavigationTarget` | a page opens a tab or window: `window.open()`, `target="_blank"`, with or without an opener | `sourceTabId`, `sourceFrameId`, `sourceProcessId` (always -1), `tabId`, `url`, `timeStamp` |

`onTabReplaced` is still never fired: Summit does not swap one tab's page for another
(prerendering, instant pages).

## How

- **Same-document navigations** are taken from the UI process's
  `WebPageProxy::didSameDocumentNavigationForFrame()` (fragment) and
  `didSameDocumentNavigationForFrameViaJS()` (push, replace, pop), before the
  frame takes its new URL. WebCore reports a pop for every same-document load,
  fragment changes included; a pop that only changes the fragment is followed
  by a fragment report, so it is skipped. The events go through
  `WebExtensionContext::didSameDocumentNavigationForFrameHaiku()`, with the same
  checks as the other webNavigation events (`webNavigation` permission and
  access to the URL).
- **New tabs.** `WebPageProxy::createNewPage()` gives the controller the
  source page, frame and URL of each page it creates. Every context reports
  it just after `tabs.onCreated`, when the browser's tab list first shows the
  page. That is later than in Chrome, where `onCreatedNavigationTarget` comes
  before the new tab's `onBeforeNavigate`. The extension needs access to the
  new tab's URL. The source tab only has to be one the extension can see (not
  a private tab it is kept out of).

## Test recipe

`tests/fixtures/extensions/webnavigation/` (MV3, `webNavigation`, all URLs).
Serve `site/` over HTTP (`python3 -m http.server 8790` in that folder),
install the extension, open `http://HOST:8790/page.html`, wait 6 seconds,
click both "target" controls, and then open the extension's `log.html`.

| Step | Expected entries |
| --- | --- |
| load | `onBeforeNavigate` … `onCompleted` for `page.html` (frame 0) and `frame.html` (parent 0) |
| the frame's `pushState` | `onHistoryStateUpdated frame.html?pushed`, `parentFrameId` 0 |
| fragment link | `onReferenceFragmentUpdated page.html#one` |
| `pushState` / `replaceState` | `onHistoryStateUpdated page.html?pushed`, then `?replaced` |
| `history.back()` / `forward()` | `onHistoryStateUpdated page.html#one`, then `?replaced` |
| `target="_blank"` link, `window.open()` button | `onCreatedNavigationTarget target.html` (and `target.html?opened`) with the page's tab as `sourceTabId` and `sourceFrameId` 0, then the new tab's load events |
