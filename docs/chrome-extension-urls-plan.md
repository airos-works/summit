# `chrome-extension://` page addresses (plan, #106)

Chrome builds of some extensions compare addresses with
`chrome-extension://<runtime.id>/`. Tampermonkey's Chrome build trusts a
message only from such a page (`T.INTERNAL_PAGE_PROTOCOLS`), so in Summit its
installer page (`webkit-extension://<uuid>/ask.html`) gets no answer and stays
blank (#105). Comparing `sender.url` with `runtime.getURL("")` works in Summit
today; hard-coding the scheme does not.

## Facts that shape the change

- `runtime.id` is the context's unique identifier: the Chrome ID (32 letters
  `a`–`p`) for Chrome Web Store / keyed packages, a Gecko ID, or a local
  `summit-pkg-XXXXXX` ID for unsigned zips.
- An extension's pages are at `webkit-extension://<uuid>/`. The base URL is
  saved in the extension's state (`LastSeenBaseURL`) and restored at load
  (`WebExtensionController` → `setBaseURL`). That address is the origin that
  its localStorage, IndexedDB, cookies and service-worker registration belong
  to.
- Changing an installed extension's address would orphan that data
  (Tampermonkey keeps its scripts there).

## Plan

1. **New installs only.** When a context is loaded for the first time (no
   `LastSeenBaseURL`) and its `runtime.id` is a Chrome ID, give it the base URL
   `chrome-extension://<id>/`. Everything else keeps `webkit-extension://`.
   Existing installs keep their recorded address, so no data moves. Reinstall
   to switch. A local `summit-pkg-…` ID is not lower-case, and a host would be,
   so zips keep `webkit-extension://`.
2. **Engine.** Add `chrome-extension` to `WebExtensionMatchPattern::extensionSchemes()`.
   That registers it as secure, CSP-bypassing and without domain relaxation, it
   gets the extension URL-scheme handler, and `isURLForAnyExtension()`
   accepts it. Review the literal `"webkit-extension"` checks in
   `UIProcess/haiku/WebView.cpp`, `UIProcess/API/haiku/WebKitContext.cpp` (three
   origin checks) and `WebProcess/WebProcess.cpp`.
3. **Browser.** Accept both schemes wherever `src/` tests for
   `webkit-extension` (address bar, `Protocol.cpp`, `Address.cpp`, load errors,
   the installer, MCP tools).
4. **Tests.** A keyed fixture: `runtime.getURL()`, `location.origin`,
   `sender.url`/`origin` in `runtime.onMessage`, `tabs.query` URL, web-accessible
   resources from a page, storage surviving a restart, and an unkeyed
   fixture still on `webkit-extension://`. Then Tampermonkey from the Chrome
   Web Store: install a `.user.js` and run it with `GM_setValue`.

## Not covered

Moving an installed extension to the new address (it would need copying its
website data from one origin to the other), and Firefox's
`moz-extension://` (Firefox builds compare with `runtime.getURL`).
