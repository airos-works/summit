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
- For extensions with only `declarativeNetRequestWithHostAccess`, the rules
  apply to requests to the hosts the extension may act on: each rule's
  triggers get one more URL-condition group (`if-request-url`) of the hosts
  its host permissions grant (`hostAccessRequestPatterns`: scheme and host,
  `*.` including the domain itself; the pattern's path is not kept), so
  they apply nowhere without any; with access to every URL nothing is added.
  Redirect and header actions are limited to those hosts by WebCore as well
  (`updateWebsitePoliciesForNavigation`). The rules are compiled again when
  host access changes. Before 27 Oct `update*` calls rejected unless the
  extension could access every URL. Checked on the X399 with `dnr-host.zip`
  (hosts `https://example.com/*` and `https://*.example.org/*`): its block
  rule matched images on example.com, www.example.org and example.org, not
  on other.test nor http://example.com (`testMatchOutcome`).
- 1Password is one of these extensions (its Firefox build requests only
  `declarativeNetRequestWithHostAccess`). Its WebAuthn related-origins session
  rule (`modifyHeaders` with request and response headers, plus
  `requestDomains`) rejects, for the response headers no platform applies. 1Password catches the rejection, logs
  "proceeding without it" and continues.
- `getMatchedRules({tabId, minTimeStamp})` answers Chrome's
  `{rulesMatchedInfo: [{rule: {ruleId, rulesetId}, tabId, timeStamp}]}` for
  the last five minutes (at most 10000 matches an extension), static,
  dynamic (`_dynamic`) and session (`_session`) rules alike, allow rules
  included. With `declarativeNetRequestFeedback` it answers for every tab;
  with only `activeTab`, for the tabs the user granted it. Matches are
  recorded from the extension's first `getMatchedRules` call on (that call
  answers none): the rules are then compiled again carrying their ids
  (`_identifier`, `_rulesetIdentifier`), which keeps WebCore from merging
  their actions, a cost only extensions that ask pay. WebCore reports each match
  (`ContentRuleListMatchedRule`, through `WebPageProxy` to the extension);
  WebCore's parser puts the report of an allow rule (a stop action) before
  it. Loads that are not a tab's (a background page's fetches) are not
  recorded. A permission granted later takes effect with the next rule update
  or start. Fixture: `tests/fixtures/extensions/dnr-matched/` (open
  `http://127.0.0.1:8798/dnr.html` from the downloads-control `serve.py`;
  call `checks.all()` once first).
- `onRuleMatchedDebug` (29 Oct, #106) fires Chrome's `MatchedRuleInfoDebug`
  (`{request: {frameId, parentFrameId, method, requestId, tabId, type, url,
  initiator?, documentId?, frameType?}, rule: {ruleId, rulesetId}}`) for each
  match WebCore reports, from the same place `getMatchedRules` records it
  (`WebExtensionController::declarativeNetRequestRuleMatchedHaiku`). The
  property exists only with `declarativeNetRequestFeedback` (Chrome further
  limits it to unpacked extensions; Summit does not). Adding a listener turns
  on match reporting as a `getMatchedRules` call does, and a non-persistent
  background's saved listener does at startup. The event is a Haiku enum value
  (`DeclarativeNetRequestOnRuleMatchedDebugHaiku`, at the end of the list)
  rather than Cocoa's `DNR_ON_RULE_MATCHED_DEBUG`, whose value sits mid-enum;
  the saved-listener version went to 8 so backgrounds learn it. Differences
  from Chrome: `method` is that of the document making the load, lowercased
  (WebCore reports the document's, not the subresource's); `initiator` is the
  frame's origin, and for `sub_frame` loads absent (WebCore reports none);
  `parentDocumentId` and `documentLifecycle` are absent; `requestId` is a UUID
  WebCore makes per load, unrelated to `webRequest` ids. Frame ids are the
  ones `webNavigation` uses (0 for the main frame, a subframe's parent 0 when
  it is the main frame). Checked on the laptop (29 Oct, engine ext106a) with
  the ext106 fixture: a blocked image, an allowed fetch (allow rules report
  too), a blocked iframe (`sub_frame`, `parentFrameId` 0, `documentId`,
  `frameType`) and a blocked image inside an iframe each fired once per load
  with the right rule, tab and frame ids; `getMatchedRules` answered the same
  8 matches.
- `setExtensionActionOptions` rejects the promise with "not supported on this
  platform".
- `testMatchOutcome({url, type, method, initiator, tabId})` answers
  `{matchedRules: [{ruleId, rulesetId}]}`: the UI process runs the
  extension's compiled rules (`m_nativeDNRRules`) on the request with
  WebCore's `ContentExtensionsBackend::actionsForResourceLoad`, as for a
  load (the request's own address as the page's for `main_frame`, else the
  initiator's), and returns the rules whose report actions matched; an
  allow rule of higher priority leaves out the rules below it, as in
  Chrome. The first call has the rules compiled again to report their ids
  (as `getMatchedRules` does). It needs `declarativeNetRequestFeedback`
  (Chrome offers it to unpacked extensions only; Summit to every extension
  with the permission). Checked on the X399 with `dnr-matched.zip`: a
  matching image gave rule 1, the allowed one only rule 2, a script of that
  name and another image nothing, and an invalid address an error.
- Session rules may name tabs, as in Chrome: `tabIds` (only those tabs;
  -1 for requests that are not a tab's) or `excludedTabIds` (every tab but
  those). Dynamic rules with either are rejected (`tabIds and excludedTabIds
  are only allowed in session rules`), as are an empty `tabIds` and both
  together; static rules with them are skipped. WebCore's rules have no tab
  condition, so each tab a rule names gets its own copy of the rule list
  (`translateDeclarativeNetRequestRulesHaiku(..., tab)`: the rules as they
  apply in that tab), compiled after the shared list; tabs no rule names
  share the list without the `tabIds` rules. The lists keep the extension's
  identifier, so the right one replaces the shared one in the tab's page
  (`attachNativeDeclarativeNetRequestRules`: each page has its own user
  content controller; a controller that pages of several tabs share gets the
  shared list), again when tabs change. Priorities across all the
  extension's rules hold, as each page has one list. A tab named only by
  rules gets an empty list (no rules). `testMatchOutcome` uses the list of
  its `tabId` (-1 when not given). Checked on the X399 with `dnr-tab.zip`
  and `dnrtab.html` (two images marked `summit-dnr-tab` and
  `summit-dnr-excl`): with rule 1 `tabIds: [A]` and rule 2
  `excludedTabIds: [A]`, tab A lost the first image and tab B the second; a
  tab opened afterwards behaved as B; naming the new tab in both rules moved
  them to it; only `tabIds` rules left other tabs untouched; clearing the
  rules brought every image back; `testMatchOutcome` gave rule 1 for A and
  rule 2 for B and for -1.
- `onRuleMatchedDebug` is absent (Chrome offers it to unpacked extensions
  only).
- `MAX_NUMBER_OF_REGEX_RULES` (1000) is enforced for the dynamic rules and
  for the session rules, each, as in Chrome (static rulesets are not
  checked). Dynamic and session rules share one limit of 30000, counted once
  (upstream WebKit counted a kind's own rules twice, so more than half the
  limit in dynamic rules blocked any addition; fixed 26 Oct).

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
  already finished is not an error; cancelling a paused one removes its
  partial file.
- `pause(id)` stops an HTTP(S) download and keeps its partial file: it stays
  `in_progress` with `paused: true` and `canResume: true` (at once, as in
  Chrome). The browser's own Pause, and quitting, look the same.
  `resume(id)` continues it under the same ID: from where it stopped, with
  `Range`/`If-Range`, or afresh if the server cannot. Resuming while the
  pause is still stopping the transfer resumes once it has. `onChanged`
  reports `paused` and `canResume`, and `search({paused})` filters on them.
  The downloads panel keeps one row (the new transfer names the one it
  continues: `continues` in the engine's notifications). A download the
  browser resumes from its panel stays the extension's same item when its
  partial file was kept.
- `open(id)` opens a complete download's file in its preferred application,
  as Tracker would. It needs the `downloads.open` permission, as in Chrome
  and Firefox; no user gesture is required.
- `removeFile(id)` deletes a complete download's file and reports
  `exists: false`.
- `getFileIcon(id, {size})` answers the file's Tracker icon (16 or 32
  pixels, 32 by default) as a PNG `data:` URL.
- `acceptDanger(id)` always rejects: Summit marks no download dangerous.
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

- The history is kept across launches (as in Chrome and Firefox), in
  `DownloadsHistory.json` of the extension storage directory (written beside
  it and moved over it after each change extensions hear of; the newest 5000
  items). A private context's is not saved. Items have their own identifiers,
  continuing from the last launch's, as the engine's download identifiers
  start again each launch. A download still transferring when Summit quit
  (quitting pauses it and keeps its partial file) is paused and resumable
  on the next launch; one whose file is gone, or not over HTTP, is
  interrupted with `USER_SHUTDOWN`. Before 27 Oct the history lasted for the
  session only. Checked on the X399 with `downloads-control.zip`: a complete,
  a paused and a running download kept their identifiers after a restart,
  the running one resumed from its partial file (8 MiB, the bytes intact), a
  new download got the next identifier, and `erase` was saved.
- A `filename` with folders (`reports/2026/a.csv`) is saved in those folders
  of the download directory, made as needed (each folder name made safe).
- `saveAs: true` asks where to save in a save panel (the engine's
  `B_WEBKIT_DOWNLOAD_DESTINATION`, answered with
  `BWebKitContext::SetDownloadDestination`; the panel's folder and name start
  at the download folder and the requested name). Cancelling the panel
  cancels the download (`USER_CANCELED`). `conflictAction: "overwrite"`
  writes over a file of that name in the download folder (never one another
  download is writing), `"prompt"` asks with the panel only when the name is
  taken, and `"uniquify"` (the default) picks a free name. A file saved
  outside the download folder cannot be resumed from where it stopped.
- `setUiOptions({enabled})` (with the `downloads.ui` permission, as in
  Chrome) turns the browser's own download UI off and on: while any loaded
  extension has it off, a new download neither opens the downloads panel
  nor shows its status text. An extension that unloads lets it show again.
- A download the browser resumes after its partial file was dropped (a
  server without byte ranges) starts a new item.
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

## Pause, resume and the file actions

Fixture: `tests/fixtures/extensions/downloads-control/` (an MV2 extension
with `downloads` and `downloads.open`) and its `serve.py`, which sends
`/slow.bin` (8 MiB at about 1 MiB a second, byte ranges, an ETag) from
`127.0.0.1:8798`. With Summit's MCP server, evaluate in the extension's
background page (`{"extension": ID, ...}`): `checks.pauseAndResume()` starts
the download, pauses it, checks that it stops growing, resumes it and waits
for the end; `checks.icon(id, 16)`, `checks.open(id)`,
`checks.removeFile(id)` and `checks.acceptDanger(id)` act on the result, and
`events` holds every `onChanged` delta.
