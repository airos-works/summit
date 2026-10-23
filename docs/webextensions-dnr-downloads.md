# WebExtensions: declarativeNetRequest and downloads

Engine patch: `.cache/dnr-downloads.patch` (applies to the port tree in
`.cache/WebKit`). No Summit application changes are needed.

## declarativeNetRequest

`chrome.declarativeNetRequest` / `browser.declarativeNetRequest` now exist
on Haiku. The WebProcess side is a C++ port of the Cocoa code, generated
from the `UseCPPAPI` IDL. Extensions get the namespace if they have
`declarativeNetRequest` or `declarativeNetRequestWithHostAccess`.

What works:

- `updateDynamicRules` / `getDynamicRules` and `updateSessionRules` /
  `getSessionRules`. Dynamic rules are stored in SQLite; session rules are kept
  in memory. Every update recompiles all rules through the existing native
  content-rule pipeline, so the rules take effect on page loads.
- `updateEnabledRulesets` / `getEnabledRulesets`. The enabled state is saved in
  the extension's JSON state.
- `getAvailableStaticRuleCount` returns `GUARANTEED_MINIMUM_STATIC_RULES` minus
  the static rules that are currently installed.
- `isRegexSupported` uses the same parser as the rule loader.
  `requireCapturing: true` reports the expression as unsupported, because
  redirect substitution is not implemented.
- Constants: `MAX_NUMBER_OF_STATIC_RULESETS`,
  `MAX_NUMBER_OF_ENABLED_STATIC_RULESETS`,
  `MAX_NUMBER_OF_DYNAMIC_AND_SESSION_RULES`, `MAX_NUMBER_OF_DYNAMIC_RULES`,
  `MAX_NUMBER_OF_SESSION_RULES`, `MAX_NUMBER_OF_REGEX_RULES`,
  `GUARANTEED_MINIMUM_STATIC_RULES`, `DYNAMIC_RULESET_ID`,
  `SESSION_RULESET_ID`.

How errors are reported:

- Wrong argument types (for example, `addRules` is not an array) throw
  synchronously, as in Chrome.
- Before storing anything, each added rule is run through the native translator
  on its own. A rule the translator cannot enforce rejects the promise with that
  rule's reason, for example
  `Rule at index 0 is invalid: Unsupported action property: requestHeaders`.
  Duplicate IDs and the shared dynamic+session limit also reject the promise.
- If an update was stored but compiling it failed, the update is rolled back
  and the promise rejects. The shared code previously resolved in this case.

Limits:

- The native translator enforces only `allow` and `block` actions. It accepts
  only these conditions: `urlFilter`, `regexFilter`,
  `isUrlFilterCaseSensitive` and `resourceTypes`.
- `modifyHeaders`, `redirect`, `upgradeScheme`, `allowAllRequests`,
  `requestDomains` and initiator/method conditions reject the promise.
- WebCore content rule lists do parse `modify-headers`. However, WebCore
  applies only the request-header part, and only for loads whose
  `DocumentLoader` has active-action patterns for the list. Haiku sets no such
  patterns: Cocoa sets them in `updateWebsitePoliciesForNavigation`, and Haiku
  has no equivalent. Response headers are never applied on any platform. So
  `modifyHeaders` is rejected instead of being accepted and silently ignored.
- For extensions with only `declarativeNetRequestWithHostAccess`, `update*`
  calls reject with an explicit message unless the extension may access every
  URL. The native loader has no per-request host-permission check, and with
  access to every URL there is nothing to check. (Until 23 Oct their accepted
  rules were also never attached to pages; see `webextensions-userscripts.md`.)
- 1Password is one of these extensions (its Firefox build requests only
  `declarativeNetRequestWithHostAccess`). Its WebAuthn related-origins session
  rule (`modifyHeaders` with request and response headers, plus
  `requestDomains`) therefore rejects. 1Password catches the rejection, logs
  "proceeding without it" and continues.
- `getMatchedRules` and `setExtensionActionOptions` reject the promise with
  "not supported on this platform". The native path does not report which
  rules matched.
- `testMatchOutcome` and `onRuleMatchedDebug` are absent.
- `MAX_NUMBER_OF_REGEX_RULES` is reported but not enforced. Dynamic and session
  rules share one limit of 30000.

## downloads

The `downloads` permission is now supported on Haiku. The namespace is available
to an extension only if it has that permission.

- `download({url, filename, conflictAction, saveAs, method, headers, body})`
  resolves with the download ID after the transfer is registered.
  - The UI process starts the transfer on behalf of the extension page that
    called it (`NeedsPageIdentifier`). That page supplies the session, the
    first party and the top origin, so the network process can resolve blob
    URLs created by that page. This covers 1Password's
    `downloads.download({url: URL.createObjectURL(blob), filename})` followed
    by `onChanged` until the state is no longer `in_progress`.
  - Allowed schemes: http, https, data, and blob URLs belonging to the calling
    extension. Relative URLs resolve against the extension.
  - Forbidden request headers are refused.
- `search(query)` supports: `id`, `url`, `finalUrl`, `filename`, `mime`, `state`,
  `error`, the `*Regex` variants, `query` terms, `exists`, `paused`, `danger`,
  `incognito`, the byte-count filters, start and end time filters (ISO string
  or milliseconds), `limit` and `orderBy`. Unknown keys reject the promise.
- `cancel(id)` cancels an active download. Cancelling a download that has
  already finished is not an error.
- `erase(query)` removes entries from the history only, fires `onErased` and
  returns the erased IDs.
- `show(id)` opens the file's folder in Tracker. `showDefaultFolder()` opens the
  Downloads folder.
- Events: `onCreated` (a full DownloadItem) and `onChanged` (a Chrome-style
  delta for `state`, `filename`, `endTime`, `error`, `totalBytes`, `fileSize`,
  `exists`, `mime` and `finalUrl`) are fired for every download in the browser
  context, including ones the user starts, as in Chrome. `onErased` is also
  fired.
- Events wake a non-persistent background page. The background-listener state
  version was bumped to 6.

Implementation:

- `DownloadRegistryHaiku` has a value-only observer, an optional requested
  filename per download, and it records the MIME type.
- `WebExtensionDownloadsHaiku` is owned by the controller. It is attached in
  `WebViewContextHaiku`, keeps the history, maps registry events to API
  events, and handles queries.
- A private browsing context has its own registry and history. Its items report
  `incognito: true`, and it dispatches only to extensions allowed in private
  browsing.

Limits:

- The history lasts only for the session and is not persisted.
- `filename` subdirectories are flattened to the last path component.
- Existing files are never overwritten. Every `conflictAction` behaves like
  `uniquify`.
- `saveAs` is accepted but no dialog is shown.
- `open()` rejects: it would need the `downloads.open` permission and a
  user-gesture policy.
- `pause`, `resume`, `getFileIcon`, `removeFile`, `acceptDanger` and
  `setUiOptions` are absent.
- `bytesReceived` updates are visible through `search`, not through
  `onChanged`, as in Chrome.
- `download()` rejects until a browser window has set the download directory.

## Manual test

Fixture: `tests/fixtures/extensions/dnr-downloads/`. It is an MV2 extension
with a persistent background page, the `declarativeNetRequest`, `downloads` and
`<all_urls>` permissions, and a disabled static ruleset `static_block`.

1. Serve the test site:
   `python3 -m http.server 8765 -d tests/fixtures/extensions/dnr-downloads/site`
   (on the guest, or forward the port).
2. Start Summit with `SUMMIT_TRACE_EXTENSION_CONSOLE=1` and install the fixture
   folder from the Extensions window.
3. When the background page loads, it runs every check. Look for
   `[dnr-downloads] PASS|FAIL ...` and `DONE n/m` on the console, or click the
   toolbar action to see the results popup, which has a "Run again" button.
4. Check the following:
   - `~/Downloads/summit-dnr-downloads-fixture*.txt` exists and contains the
     timestamp line.
   - The status bar reported the download.
   - The "unsupported modifyHeaders rule rejects" line shows the translator's
     reason.
5. Open `http://127.0.0.1:8765/`. The session, dynamic and static probes must
   show BLOCKED, and the control probe must show LOADED.
