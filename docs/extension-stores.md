# Install from the extension stores

Window → Extensions has links to the Chrome Web Store and Firefox Add-ons,
plus a field for an extension's store link. On an extension detail page,
the page gets a card, centred at the top of the page, with the Summit icon
and **Install Extension** (see
[issue-fixes-2026-10-19.md](issue-fixes-2026-10-19.md); the toolbar button
that did the same was removed for issue #63). These entry points download
the package and open the existing native permission review. The extension
starts only after the user chooses Install. Store pages' own
browser-specific install buttons are not required. When the page's extension
is installed, the card says Manage Extension instead and opens the
Extensions window with it selected.

The installer accepts only HTTPS detail-page URLs on the official store
origins. Chrome packages come from Google's CRX update service; their CRX3
signature and signer ID must match the chosen item. Firefox packages are
resolved through Mozilla's v5 add-on API; their size and SHA-256 must match
Mozilla's metadata, and their manifest identity must match its GUID. HTTPS
redirects, download limits and cancellation apply to the entire operation.
Temporary downloads are removed after staging, failure or cancellation.

Extensions installed this way, or from a saved package signed for its store
identity, update themselves from the same services, with the same checks
(#104, [extension-fixes-2026-10-20.md](extension-fixes-2026-10-20.md)).

This does not add API compatibility for arbitrary extensions. The review
continues to distinguish CRX signature verification from store provenance;
Firefox's XPI signature is verified as well, for store downloads and saved
packages alike (since 27 Oct, `extension-crx.md`). Package downloads and
permissions do not bypass the existing consent, fingerprint or restart checks.

Protocol references: [Mozilla's add-on API](https://mozilla.github.io/addons-server/topics/api/addons)
and [Chrome's extension update service](https://developer.chrome.com/docs/extensions/how-to/distribute/install-extensions).

## Native verification — 7 October 2026

X399 bundle `/boot/home/summit/build-modern-browser/bundle-uofxmys5` passes:

- All five portable CTest suites, including 22 store URL cases.
- Real Dark Reader 4.9.133 downloads from both stores. The Firefox XPI is
  868,274 bytes; the Chrome CRX is 851,022 bytes.
- 52 native checks per store for URL validation, permission review, cancellation,
  no execution before consent, install, duplicate rejection, correct catalog
  identity, toolbar action and temporary-file cleanup.
- 11 checks per store for restoration and execution after browser restart.
- 29 checks for both native store links and the detail-page install button.

All five final native browser runs exit normally, leave no process-group
members, and add no crash reports or native debugger events. The restart test
waits for initial page navigation before requesting shutdown: an earlier run
requested quit while that navigation was still invalidating close approval.
The failure remains recorded in the earlier local test log; the corrected
precondition passes for both stores.

The native harness is `tests/ModernExtensionStoreTests.cpp`. Compile it on
Haiku with `g++ -std=c++23 -O1 -Wno-multichar -Isrc -Ivendor
 tests/ModernExtensionStoreTests.cpp -lbe -o test-stores`, then run
`test-stores TEAM EXECUTABLE PROFILE STORE_URL IDENTITY PHASE` against an
isolated browser profile, where PHASE is `install`, `restart` or `browse`.
The browser must have `SUMMIT_ENABLE_INPUT_SYNTHESIS=1` for native control input.
The test verifies the executable belongs to TEAM and closes only that browser.

Local evidence: `.vm/issue40-native-results.json`, `.vm/issue40-native.log`,
`.vm/issue40-tests/{firefox,chrome}-{install,restart}.log`,
`.vm/issue40-browse.log`, and `.vm/issue40-browser-build.log`.
