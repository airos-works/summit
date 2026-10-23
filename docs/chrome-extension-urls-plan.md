# `chrome-extension://` page addresses (#106)

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

## Change (cext, 23 Oct)

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

## As implemented

- `WebExtensionController::restoreNativeBaseURL()`: no recorded base URL and
  a 32-letter `a`–`p` identifier → `chrome-extension://<id>/`.
- `chrome-extension` is in `extensionSchemes()`, `validSchemes()` and
  `supportedSchemes()` (Haiku only); the curl cookie rule treats it as
  same-site like `webkit-extension`; the website-data filters in
  `WebKitContext.cpp`, the external-link test in `WebView.cpp` and the
  web-process priority test accept it.
- Browser: `summit::IsExtensionPageURL()` (`src/core/Protocol.*`) is used by
  the tab, navigation, load-error and installer checks; `ResolveAddress` and
  `IsExternalScheme` know the scheme. `main.cpp` still holds back only
  `webkit-extension:` tabs of a restored session until extensions load
  (`main.cpp` has uncommitted #110 work).

## Verified (X399, cext build 4c6edf0, fresh profile)

- Tampermonkey 5.5.1 from its Chrome Web Store CRX loads at
  `chrome-extension://dhdgffkkebhmkfjojejmpbldmpobfkfo/`; an unsigned zip
  (local `summit-pkg-…` ID) stays at `webkit-extension://<uuid>/`.
- Opening a `.user.js` link shows Tampermonkey's installer with the script's
  details; Install adds it and closes the tab. On `https://example.com/` the
  script ran and its `GM_setValue` counter went 1, 2.
- After quitting and restarting Summit both extensions kept their addresses,
  and the script ran again with its stored count (4).
