# Networking: online state, long requests and SameSite cookies

Three fixes of 20 October 2026 in the engine's network layer, for
[#85](https://github.com/jmgasper/summit/issues/85),
[#96](https://github.com/jmgasper/summit/issues/96) and
[#92](https://github.com/jmgasper/summit/issues/92). Sites that ask for a name
and password ([#87](https://github.com/jmgasper/summit/issues/87)) are in
[certificate-warnings.md](certificate-warnings.md).

## `navigator.onLine` and the online and offline events (#85)

### What the user sees

Pages learn when the network goes away and comes back: `navigator.onLine`
turns `false` and `offline` fires on every open page about two seconds after
the cable is pulled or the wireless network is left, and `online` fires when
it is back. Gmail, Google Docs, Outlook, Teams, Slack and installed web apps
switch to their offline mode instead of sending requests that fail.

### How it works

- `NetworkStateNotifierHaiku.cpp` (WebCore) was a stub that always said
  online. The network process owns the state and sends it to every web
  process (`NetworkConnectionToWebProcess::setOnLineState`), which is where
  `navigator.onLine` reads it.
- **Online** means an interface other than loopback is up, has a link
  (`IFF_UP` and `IFF_LINK`: a cable, or a joined wireless network) and an
  address that is not just self-assigned (`0.0.0.0`, `169.254/16` and IPv6
  link-local ones do not count). It is read with `getifaddrs()`; a failure
  reads as online, so a page that might work is not stopped.
- **Changes** come from the network stack: `start_watching_network()` (in
  libbnetapi, which the engine does not link: it is looked up with `dlopen`
  when the network process starts) delivers interface, link and wireless
  notices to a small looper of its own, which asks WebKit's main thread to
  read the state again (`updateStateSoon()`, which waits 2 s for a burst of
  changes to settle). Without notifications it reads the state every 5 s.
- The network process logs `Summit: the network is gone` / `is back`.
- Test hook: `SUMMIT_NETWORK_IGNORE_INTERFACES_FILE=PATH` names a file of
  interfaces (as `ifconfig` shows them, by commas or lines) to leave out,
  read at every check. On a machine reached over its only network, write its
  interfaces to the file and make the stack announce a change with
  `ifconfig loop metric 0`; empty the file and do it again to come back.

Checked on the X399 (wired and wireless): with both interfaces in the file,
`offline` fired 2.5 s after `ifconfig loop metric 0` with
`navigator.onLine == false`; emptied, `online` fired 2.3 s after the next one.

## Long polls and slow streams (#96)

### What the user sees

A request the page set no timeout for may wait as long as its server takes:
a long poll that answers after two minutes completes, and a `fetch()` stream
or XHR that sends a line every 90 s keeps streaming. Before, every such
request failed after 60 s without data. Requests with a timeout of their own
keep it exactly (a synchronous XHR still gives up after 10 s, an
`xhr.timeout` after its time).

When the network goes away, such a request that has had no data since then
fails 15 s later, so a page's reconnect logic starts instead of waiting for
ever.

### How it works

- WebKit's curl backend turns a request's timeout (60 s by default) into an
  idle limit (`CURLOPT_LOW_SPEED_TIME`) for the whole transfer
  (`CurlHandle::setTimeout`). In `CurlRequest::setupTransfer`, a request
  whose timeout is the default now gets 300 s for its response to start, as
  in Firefox, and no idle limit once its response has started
  (`didReceiveHeader`, for a final response other than 401/407). Requests with
  their own timeout interval keep the old behaviour.
- `CURLOPT_TCP_KEEPALIVE` is set for them, but Haiku's TCP sends no
  keepalives (the option is only stored), so a dead connection is never
  noticed by the stack. Instead a progress callback
  (`CurlRequest::progressCallbackHaiku`, about once a second while a
  transfer waits) gives up on one that has been quiet for 15 s since the
  network went away (`networkLostAtHaiku`, set by the notifier above). The
  page gets a network error; the log says `Summit net: gave up on a request
  quiet since the network went away`.
- Not covered: a connection whose server dies silently while the network
  stays up still waits until the 300 s for its response to start, or for
  ever once the response has started; that needs TCP keepalive in Haiku.

Checked on the X399 with a test server on the build host: an XHR and a
`fetch()` answered after 120 s completed (`done after 120.0 s`, read by the
page); a chunked stream with a line every 90 s delivered all three lines at
0, 90 and 180 s, and a two-line one ended normally after 90 s; an XHR with `timeout = 5000` timed out at 5.0 s and a
synchronous one at 10.0 s. With the network made to go away (hook above),
an XHR and a `fetch()` waiting for 300 s and a stream between lines failed
15.6 s after the offline event.

The test server must say `Connection: close` when it closes a connection after
a response: otherwise the page's next request goes out on that connection,
and a `POST` sent on it fails (curl cannot send its body again). That, not the
streams, made an earlier run look as if chunked responses never ended.

## Connections after a network change (#129)

### What the user sees

When a VPN disconnects, or the machine leaves one network for another while
staying online, pages on hosts Summit already had connections to load again
within seconds. Before, every new request to such a host went onto the old
HTTP/2 connection, waited the 300 s a response may take to start and failed,
and the next one did the same, until Summit was restarted.

### How it works

- Haiku's TCP never fails an established connection whose local address was
  removed (jmgasper/haiku#41), and libcurl keeps such a connection in its
  pool. The offline handling above does not apply: another interface is
  still up.
- On every network change the notifier (`NetworkStateNotifierHaiku.cpp`)
  also keeps the local addresses of the interfaces that count for being
  online, and the ones seen before that have gone since (until they come
  back). It logs `Summit net: the local address A went away`.
- Every socket libcurl opens for a connection is registered
  (`CURLOPT_OPENSOCKETFUNCTION` and `CURLOPT_CLOSESOCKETFUNCTION` set in the
  `CurlHandle` constructor, `CurlContext.cpp`). Those whose local address
  went away are ended: each socket is replaced under its descriptor
  (`dup2`) with one end of a socket pair whose other end is closed, which
  reads as ended at once, and the curl workers are woken
  (`curl_multi_wakeup`). libcurl fails what was on them, drops them from its
  pool and later closes the replacement: `Summit net: closed N connections
  whose local address went away`. `shutdown(SHUT_RDWR)` is not enough on
  Haiku: its TCP sends the FIN and then waits for the peer's, which a dead
  connection never sends (seen on the X399: the request stayed in
  `fin-wait2` until the server answered). WebSockets close the same way, and pages
  reconnect them.
- A GET, HEAD or OPTIONS that failed on such a connection before any of its
  response came is sent again once on a fresh connection
  (`CurlRequest::retryOnFreshConnectionHaiku`, which also does the HTTP/2
  error retries).
- A request the page set no timeout for that gets no response in 300 s
  closes its connection (found by its local address and port), so the
  requests after it do not wait on it too, and a GET, HEAD or OPTIONS is
  sent once more on a fresh connection. A request with a timeout of its own
  just fails.
- Not covered: a connection whose route changed while its local address
  stays (a full-tunnel VPN connecting) is left alone; it fails at the 300 s
  limit above and is closed then.

Checked on the X399, which has a wired (192.168.1.247) and a wireless
(192.168.1.176) interface, with the wired one made to go away through the
test hook above while the wireless one kept it online. With a request
pending on a test server on the build host (answering after 25 s, over
HTTP/1.1) and one to `https://httpbin.org/delay/8`, the change closed 3
connections 2 s later; both requests were sent again on fresh connections and
the page got both answers (at 28.1 s and 12.2 s), with no error. A
network change that removed no address left a pending 15 s request alone
(answered at 15.0 s). A request whose server held it past 300 s was sent
again then, on a new connection, and answered at once (300.0 s). With both
interfaces left out, the network was gone and both addresses went away, as
before.

## SameSite cookies (#92)

### What the user sees

Cookies marked `SameSite=Strict` or `SameSite=Lax` are no longer sent with
requests from other sites, so another site cannot make the browser send the
user's session cookie in a form post, an image or a `fetch()` (cross-site
request forgery):

- `Strict` cookies go only with requests from the site itself.
- `Lax` cookies also go with a top-level navigation by a safe method: a link
  or a `GET` form from another site, not a cross-site form `POST`, frame,
  image or `fetch()`.
- Responses to such cross-site requests cannot set `Lax` or `Strict`
  cookies, and a document in a cross-site frame cannot set or read them
  through `document.cookie`.
- Cookies without the attribute behave as `SameSite=None`, as in Safari and
  Firefox: Summit does not use Chrome's Lax-by-default, nor its rule that
  `SameSite=None` needs `Secure`. Sign-ins that post back from another site
  (Microsoft's and Apple's `form_post`, SAML) send what they send in Safari.

### How it works

- `NetworkStorageSessionCurl.cpp` (the network process's cookie store, on
  `CookieJarDB`, which already kept each cookie's SameSite) applies the rules
  of RFC 6265bis with WebCore's `SameSiteInfo` in the lookups for requests
  (`cookieRequestHeaderFieldValue`), for `document.cookie` (`cookiesForDOM`)
  and `getRawCookies`, and drops `Lax`/`Strict` cookies a cross-site document
  sets (`setCookiesFromDOM`). The web process already computes `SameSiteInfo`
  for every request and document.
- `NetworkDataTaskCurl.cpp` drops `Lax`/`Strict` `Set-Cookie` headers of
  responses to requests that are neither same-site nor top-level
  navigations. A request no page made (a download the browser started) is
  same-site unless its first party is another site, as in libsoup; requests
  of an extension's own pages count as same-site, as in Chrome.
- The cookie lists of the developer tools' Storage tab and of extensions'
  `cookies` API come from the whole store and are unchanged: they still show
  every cookie with its SameSite.
- Partitioned cookie storage is described below. The legacy `BWebView`
  loader still lacks the SameSite enforcement described in this section.

Checked on the X399 with two sites on the build host (`http://192.168.1.200:8861`
and `http://airos-build:8861`; an address and a host name are different
sites): with `c_strict`, `c_lax`, `c_none` and `c_plain` set on the first,
the second's image, `fetch()` (GET and POST, with credentials) and frame
got only `c_none` and `c_plain`; a link and a `GET` form also sent `c_lax`;
a cross-site `POST` form did not; the frame's `document.cookie` showed
neither, and of the `Lax`, `Strict` and `None` cookies it and a cross-site
frame response set, only the `None` ones were stored. A typed address on the
first site sent all of them, and the developer tools' Storage tab listed them
with Strict, Lax and None.

## Partitioned cookies (CHIPS, #121)

The Curl cookie store now recognizes `Partitioned` in HTTP response headers
and `document.cookie`. These cookies require `Secure` and an HTTPS setter;
their key includes the top-level site's scheme and registrable domain. Paths,
ports and subdomains of that top-level site share a partition, while different
schemes or sites do not. Ordinary cookies retain their existing behavior.
This follows the [CHIPS top-level site model](https://privacysandbox.google.com/cookies/chips).

`CookieJarDB` schema version 3 includes `partitionkey` in the cookie's unique
identity. Migration rebuilds the table in a transaction, retaining existing
values, creation times, SameSite policy and update times. Existing cookies
remain unpartitioned: old versions discarded the attribute, so their rows
cannot be retrospectively classified. Session cookies are removed at startup
as before. A failed migration rolls back without deleting the old database.

Request headers and `document.cookie` select ordinary cookies plus cookies of
the current top-level site. Overwrite, expiry, HttpOnly checks, exact deletion
and mutation receipts use the complete partitioned identity. Privileged
enumeration retains partition keys, and cookie equality distinguishes them
when the inspector combines results from multiple frames.

The native developer tools' Storage panel shows a **Partition Key** column and
includes it in filtering and row identity. Deleting a partitioned row expires
that exact structured cookie through `Page.setCookie`; the older
`Page.deleteCookie` command cannot express a partition. The inspector and
network process verify that a partitioned edit belongs to the inspected
top-level site and the embedded cookie origin.

The extensions API accepts `partitionKey: {topLevelSite: "https://site.example"}`
for `cookies.get`, `getAll`, `set` and `remove`. Omission selects ordinary
cookies; `getAll({partitionKey: {}})` explicitly selects all partitions.
Returned cookies, removal receipts and `onChanged` events include the key.
Host permissions and private-store restrictions still apply, including to
wildcard queries. See the [Chrome cookies API](https://developer.chrome.com/docs/extensions/reference/api/cookies).

The key here is the top-level site. Chrome's additional `hasCrossSiteAncestor`
dimension and `cookies.getPartitionKey` are not implemented; unsupported
partition fields fail explicitly instead of silently broadening a query.
The legacy loader uses the same CHIPS database for opt-in cookies but still
rejects separate tracking-prevention requests for automatic partitioning.

### Validation

On 21 October 2026, the original browser sent a third-party frame's server and
JavaScript cookies under both unrelated top-level sites; writing under the
second site also changed the first site's values. The reproducer is
`tools/bench/chips-server.py`: an HTTPS server using the machine's LAN IP and
127.0.0.1 as top-level sites, with an iframe on localhost. Use an isolated
profile trusting a test certificate for those three hosts.

Native component validation passes **237 database/parser/change
checks** and **145 query/JavaScript conversion checks**. This covers real
SQLite, migrations from versions 0, 1 and 2, rollback, partition isolation,
expiry, event identity, wildcard queries and structured API results. Build
the executables from the configured engine using
`tools/test-engine-cookie-components.py --build BUILD --output OUTPUT
--runtime-lib-dir HAIKU_LIBRARY_DIRECTORY`, within its Haiku SDK environment;
then run both executables on Haiku with the matching libraries.

The native workstation browser with engine commit `d4fed42` passed **nine
HTTPS scenarios**: seed under A, visit unseeded B, seed under B, return to A,
restart and revisit both, delete A's cookies, revisit B, and reseed A. Both
the server's echoed request header and the frame's `document.cookie` retained
the expected partition; HttpOnly cookies stayed out of the DOM. Ordinary
cookies remained shared.

The extension fixture, `tests/fixtures/extensions/chips`, passed **17 checks**
from `runCookiePartitionTest()` and **two restart checks** from
`resumeCookiePartitionTest()`. These use the actual extension API, including
write/removal receipts, event delivery, wildcard queries, path filtering,
denied host permissions, exact expiry and persistent partitioned cookies.

The native Storage panel showed distinct ordinary and partitioned rows with
the same name, filtered by partition key, and deleted the selected partitioned
row. Extension enumeration then confirmed that the ordinary same-name cookie
and the other site's cookie remained. The host DevTools suite also passed
**380 checks**.

Finally, a copy of the original browser's schema-2 profile migrated to schema
3 in the new browser. All three persistent fixture rows retained every old
column and their row IDs, acquired empty partition keys, and remained usable
by the page. The original profile was unchanged. The test bundle was installed
alongside the existing browser, and all acceptance checks used isolated profiles.
