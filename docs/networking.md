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
- Not done: partitioned cookies (CHIPS, `Partitioned`) are still kept in the
  one jar, not per top-level site (they work, without the isolation); doing
  it needs a partition column or table in `CookieJarDB` and partition-aware
  lookups, deletion and expiry. The legacy `BWebView` loader
  (`CookieStorageSessionHaikuCurl.cpp`) keeps the old behaviour.

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
