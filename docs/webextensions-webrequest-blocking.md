# Blocking webRequest

Extensions with the `webRequestBlocking` permission can now change requests
from `webRequest` listeners registered with `blocking` (or `asyncBlocking` for
`onAuthRequired`), with Chrome/Firefox MV2 semantics:

| Event | Honoured reply fields |
| --- | --- |
| `onBeforeRequest` | `cancel`, `redirectUrl` (main-frame and subresource loads, also for every redirect hop) |
| `onBeforeSendHeaders` | `cancel`, `requestHeaders` (full replacement) |
| `onHeadersReceived` | `cancel`, `responseHeaders` (full replacement) |
| `onAuthRequired` | `cancel`, `authCredentials` (`blocking` return value, Promise, or `asyncBlocking` callback) |

Status: **syntax-checked only** (every changed unit, the regenerated IPC
receivers and serializers; see "Verification"). Nothing has been linked into a
browser or run. The engine delta is `.cache/webrequest-blocking.patch`.

## Design

```
WebProcess (page)          NetworkProcess                      UIProcess                         WebProcess (extension)
  load ───────────────▶ NetworkResourceLoader
                         phase bit set in NetworkProcess? ──no──▶ load continues (no extra IPC)
                            │ yes
                            ▼
                         ResourceLoadWebRequestDecisionHaiku ─▶ NetworkProcessProxy (validate page/frame/store)
                           (async reply)                          WebExtensionController::decideWebRequestHaiku
                                                                   eligible contexts (webRequestBlocking + host
                                                                   permission + live blocking listener)
                                                                     └─ WebExtensionContext::dispatch… ──▶ DispatchBlockingWebRequestHaiku
                                                                                                            blocking listeners only; value,
                                                                                                            Promise or asyncBlocking callback
                                                                   BlockingTransaction: decode, re-authorize, ◀── Vector<String> JSON replies
                                                                   merge, 20 s timeout
                         apply result ◀──────────────────────── WebExtensionWebRequestGateResultHaiku
```

* **Fast path.** The UI process keeps, per controller, the set of phases that
  have a live blocking listener in an extension holding `webRequestBlocking`,
  and pushes the process-wide union to every network process
  (`NetworkProcess::SetWebRequestBlockingPhasesHaiku`, also sent to network
  processes launched later). `NetworkResourceLoader` only asks the UI process
  when the page has loaded extensions (`pageHasLoadedWebExtensions`) and the
  phase bit is set. Without a blocking listener there is no extra IPC; with one
  (uBlock Origin) each request costs one Network→UI→extension round trip per
  consulted phase. The mask is only a hint: when the UI process finds no live
  blocking listener for a phase it recomputes and republishes the mask.
* **Gate points** (`NetworkResourceLoader.cpp`, `PLATFORM(HAIKU)` only):
  `onBeforeRequest` then `onBeforeSendHeaders` run before the disk/prefetch
  cache lookup or network load in `startRequest`, before cached/synthetic
  redirects restart the load, before a server redirect hop is followed, and
  before `restartNetworkLoad`. `onHeadersReceived` runs at the top of
  `didReceiveResponse` (before the response is observed or delivered).
  `onAuthRequired` runs from `NetworkLoad::didReceiveChallenge` through the new
  `NetworkLoadClient::decideAuthenticationChallengeHaiku` hook, before the
  embedder is asked.
* **Cancel** fails the load with the content-blocker error (as a DNR block
  does). A main-frame cancel therefore shows the blocked-load error page.
* **Redirect** is an internal `307 Internal Redirect` (method and body kept,
  `Non-Authoritative-Reason: WebRequest API`, `Access-Control-Allow-Origin`
  echoed for CORS requests, `no-store`). It goes through the ordinary
  `willSendRedirectedRequest` path, so the WebProcess, navigation policy,
  CORS/mixed-content checks and the redirect limit (20) all apply, and
  `onBeforeRequest` runs again for the new URL. Synthetic redirects are never
  cached.
* **Header replacement** replaces the whole header list (Chrome semantics).
  Replacing `Content-Type` in `onHeadersReceived` also updates the derived MIME
  type and charset.
* **Auth**: `cancel` exposes the 401/407 response (Chrome behaviour);
  credentials are applied with session persistence. At most 3 extension
  credential attempts per load, then the ordinary authentication path takes
  over (prevents loops with wrong credentials).

## IPC (all `ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)`)

| Message | Direction | Notes |
| --- | --- | --- |
| `NetworkProcess::SetWebRequestBlockingPhasesHaiku(uint8_t)` | UI→Network | Bit per phase (0 BeforeRequest … 3 AuthRequired). |
| `NetworkProcessProxy::ResourceLoadWebRequestDecisionHaiku(pageID, phase, ResourceLoadInfo, ResourceRequest, body?, ResourceResponse?, AuthenticationChallenge?) -> (WebExtensionWebRequestGateResultHaiku)` | Network→UI | `MESSAGE_CHECK` on the phase. Page must exist, be open and use this network process' data store; the frame must belong to the page. Every failure replies empty (continue). |
| `WebExtensionContext::Add/RemoveBlockingWebRequestListenerHaiku(frameID, type[, count])` | Web→UI | `isLoadedAndPrivilegedMessage`. Tracked as weak frames, so closed pages drop out automatically. |
| `WebExtensionContextProxy::DispatchBlockingWebRequestHaiku(phase, tab, window, ResourceLoadInfo, request, body?, response?, challenge?, mainFrame?) -> (Vector<String>)` | UI→Web | One JSON `BlockingResponse` per blocking listener, empty for no decision. |

`WebExtensionWebRequestGateResultHaiku` (new `.serialization.in`, appended in
`PlatformHaiku.cmake`) carries `cancel`, `redirectURL?`, `headers?` and
`credentials?`.

## Semantics

* Only extensions with `webRequestBlocking` **and** host permission for the
  request (the same live tab/private-browsing/host checks as observation) may
  block. Eligibility is checked when dispatching and again when each reply
  arrives. `addListener(..., ['blocking'])` without the permission throws, as
  in Chrome; `blocking` with `asyncBlocking`, `asyncBlocking` outside
  `onAuthRequired`, and `blocking` on non-blocking events also throw.
  `extraHeaders` is accepted and ignored (all headers are always exposed).
* **Conflicts** (`WebExtensionWebRequestMergeHaiku`, unit-tested): any cancel
  wins; otherwise the most recently installed extension (native installation
  order from the package registry) wins conflicting modifications; header edits
  are deltas against the headers the listeners saw, so non-conflicting edits of
  different extensions combine, a conflicting delta is dropped whole.
  Extensions without native installation metadata (not installed through
  `ExtensionPackageRegistryHaiku`) only observe.
* **Invalid replies are ignored** (`WebExtensionWebRequestDecisionHaiku`,
  unit-tested; Chromium rules, e.g. `cancel` combined with other keys is
  invalid). A thrown exception, a rejected Promise, `undefined` or `null` means
  no decision.
* **Promises** are accepted from every blocking listener (Firefox); the
  `asyncBlocking` callback is accepted for `onAuthRequired` (Chrome).
* **Timeouts fail open.** The WebProcess stops waiting for unsettled listeners
  after 18 s, the UI process completes the decision after 20 s with whatever
  replies arrived (`webExtensionWebRequestListenerTimeoutHaiku`,
  `webExtensionWebRequestBlockingTimeoutHaiku` in
  `WebExtensionWebRequestGateResultHaiku.h`). A closed extension or network
  connection also completes with an empty result. A load is never held
  indefinitely.
* **Observation is not duplicated.** Blocking listeners are invoked by the
  blocking dispatch; the later observation of the same load and phase skips
  them (bounded per-process record keyed by resource-load id and phase) and
  still reaches non-blocking listeners. When no blocking dispatch happened
  (e.g. a cached response for `onHeadersReceived`), blocking listeners get the
  ordinary observation, without effect.
* Redirect destinations: `http(s)` URLs redirect. A `data:` URL or a resource
  the redirecting extension lists in `web_accessible_resources` (uBlock
  Origin's `redirect=` surrogates: `noop.js`, `google-ima.js`, `1x1.gif`…) is
  served by the network process as a `200` response under the request's own
  URL (#103): the UI process reads the bytes and MIME type
  (`WebExtensionControllerWebRequestBlockingHaiku.cpp`) and returns them in
  the decision; `NetworkResourceLoader::serveWebRequestSurrogateHaiku` answers
  with them (`Access-Control-Allow-Origin` echoed, `no-store`, not offered to
  `onHeadersReceived`). Chrome's internal redirect to the extension URL is not
  observable to scripts either, and a redirect to a `data:` or extension URL
  would fail the WebProcess' redirect checks. Any other destination
  (`about:blank`, a resource that is not web-accessible, another extension's
  URL) is applied as a **cancel**.
* WebSocket connections (#103) pass `onBeforeRequest` with type `websocket`
  before the network process connects
  (`NetworkConnectionToWebProcess::createSocketChannel`): a cancel fails the
  connection as one the server refused (`error`, `close` 1006) and a redirect
  to a `ws:`/`wss:` URL connects there instead. Match patterns treat `ws:` and
  `wss:` URLs as `http:` and `https:` (host permissions and listener filters,
  as in Chrome).

## Limits

* Synchronous XHR is never held (its WebProcess is blocked and could host the
  listener); it is observed only.
* Not intercepted: service-worker handled fetches, speculative/prefetch
  loads, downloads, and responses served from the disk cache for
  `onHeadersReceived` (cache hits do pass `onBeforeRequest`/
  `onBeforeSendHeaders`). `redirectUrl` from `onHeadersReceived` is ignored.
  Pings and `navigator.sendBeacon` are ordinary keep-alive loads of the
  page's `CachedResourceLoader` in this engine (the old `PingLoad` is gone),
  so they pass the same gates as other subresources.
* Request headers seen/modified in `onBeforeSendHeaders` are WebKit's request
  headers before libcurl adds `Cookie`, `Accept-Encoding`, `Host` etc.
* Blocking listeners must be registered in a live extension page (MV2 persistent
  background page, event page while awake, popup, options). An unloaded event
  page is not woken for a blocking decision; the request continues.
* No `handlerBehaviorChanged` cache flush is needed (listeners are consulted
  for every request) and no DNR header constraints are passed to the merge yet.
* Each blocking phase costs one round trip per request while any blocking
  listener exists; Speedometer is unaffected unless a blocking extension is
  installed.

## Decisions for the lead

1. **Non-HTTP(S) redirects are served, not followed** (#103, 23 October
   2026): `data:` URLs and the redirecting extension's web-accessible
   resources become the response itself; anything else is a cancel.
2. **Unregistered contexts do not block** (they lack the immutable
   installation order the conflict rules need).
3. **MV3** manifests may use `webRequestBlocking` (Firefox model; Chrome only
   allows it for policy-installed extensions).
4. **Partial replies on timeout** are applied (e.g. a fast extension's cancel
   still wins over a hung one).

## Test recipe

Fixture: `tests/fixtures/extensions/webrequest-blocking/` (MV2, persistent
background, `webRequest`, `webRequestBlocking`, host permissions for
`http://127.0.0.1/*` and `http://localhost/*`) and its local server.

1. In the guest: `python3 tests/fixtures/extensions/webrequest-blocking/server.py 8765`.
2. Install the fixture folder through the extension manager (**Add
   extension…**, unpacked folder), approving `webRequestBlocking` and the host
   permissions. Installing through the manager gives the context the native
   installation metadata that blocking requires.
3. Open `http://127.0.0.1:8765/`. The page probes each case and fills a table:

| Row | Expected |
| --- | --- |
| plain | `200 plain /plain.txt` |
| cancel (`/wr-block/`) | `blocked` (fetch rejects); the image row also `blocked` |
| redirect (`/wr-redirect/`) | `200 redirected /wr-redirected.html?from=…` |
| promise cancel | `blocked` |
| invalid reply | `200` (the invalid reply is ignored) |
| request header | JSON echo contains `"x-summit-webrequest": "request-header-added"` |
| response header | `response-header-added` |
| auth (`/wr-auth/`) | `200 authorized`, no credential prompt |
| surrogate data: URL (`/wr-surrogate-data/`) | `200 surrogate data` |
| surrogate not web-accessible (`/wr-surrogate-hidden/`) | `blocked` |
| surrogate script (`/wr-surrogate-js/`) | `ran 1 time(s)`: the extension's `noop.js` ran as the page's script |
| websocket allowed (`/ws/echo`) | `open, message hello, close 1000` |
| websocket cancel (`/wr-block/ws`) | `error, close 1006`, and the server never sees the handshake |
| beacons | `ok sent, blocked one stopped` |
| slow (never settles) | `200` after about 20 s (fail open) |

4. Click the main-frame links: `/wr-block/page.html` shows the blocked-load
   error page, `/wr-redirect/page.html` lands on `/wr-redirected.html`,
   `/wr-auth/page.html` loads without a prompt.
5. `http://127.0.0.1:8765/hits` lists the server hits: no `/wr-block/` or
   `/wr-promise-block/` entries may appear (cancelled before the network), and
   `/wr-redirect/` must not appear either (redirected before the network).
6. The extension console (`[wr-fixture]` lines) shows each decision once and a
   `completed`/`error` line per request (non-blocking observers still work).
7. Performance: with the fixture disabled, a Speedometer run must match the
   baseline (no IPC is added); with it enabled, expect one extra round trip per
   request and phase.

## Verification

* `-fsyntax-only` with the real Modern compile flags (scratch overlay first on
  the include path, IPC headers and serializers regenerated into the scratch
  directory) passed for all changed units, the four regenerated message
  receivers, `GeneratedSerializersSharedExtensions.cpp`, and unchanged units
  that include the modified headers (`WebExtensionContext.cpp`,
  `WebExtensionController.cpp`, `WebExtensionControllerWebRequest.cpp`,
  `WebExtensionContextWebRequest.cpp`, `WebExtensionContextAPIEvent.cpp`,
  `WebExtensionContextProxy.cpp`, `NetworkProcess.cpp`).
* The decoder/merge helpers are byte-identical to the previously unit-tested
  candidate (`tests/EngineExtensionWebRequest{Decision,Merge}Tests.cpp`).
* **Not verified:** linking, runtime behaviour, the fixture recipe above,
  performance, and interaction with DNR.
