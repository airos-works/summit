# Sites with certificates Summit cannot verify

Added 30 September 2026 for [issue #1](https://github.com/jmgasper/summit/issues/1):
a Ubiquiti router on the local network redirects to `https://192.168.1.1` with a
self-signed certificate, and Summit only said "Could not load
https://192.168.1.1: Peer certificate or SSH remote key was not OK".

## What the user sees

- A page whose server certificate fails verification (self-signed, expired,
  issued for another name or by an unknown authority) no longer just fails in
  the status line. When the page was the one asked for (the main frame, before
  it replaced the old page), a warning opens over the window: **This
  connection to 192.168.1.1 is not private**, why the certificate was refused
  (OpenSSL's reason, for example "self-signed certificate"), who it is issued
  to and by, what names it is valid for, its validity dates and its SHA-256
  fingerprint.
- **Go Back** (the default, and Escape) leaves the tab on the page it showed
  before. **Continue to 192.168.1.1** trusts that certificate for that host
  and loads the page.
- The choice is remembered. Summit keeps the host and the certificate's
  SHA-256 in the profile (`trustedCertificates` in `profile.json`), so the
  site opens directly from then on, also after a restart and in private
  windows. Only that certificate is trusted: if the site's certificate
  changes, the warning comes back. A choice made in a private window lasts
  only for the private session.
- A background tab that fails this way asks when it is selected. A request of
  a page that has already loaded (an image, a script, a WebSocket to another
  host) is refused without a question; the status line names the reason.
- **Preferences › History and Data** shows how many such certificates are
  trusted and has **Forget Trusted Certificates…**, after which the warning
  shows again.

## How it works

The Haiku network process uses libcurl. When a TLS handshake fails
verification, `NetworkDataTaskCurl` (and `WebSocketTask`) asks the UI process
through a server-trust authentication challenge before giving up; the Haiku
port answered none of them, so every such load failed.

- `WebView.cpp`'s navigation client now answers server-trust challenges. It
  computes the leaf certificate's SHA-256 from the challenge's certificate
  chain; if the page's `BWebKitContext` trusts that certificate for the host,
  it answers with a credential, and the curl task repeats the request without
  verification. Otherwise it refuses, and remembers the certificate (subject,
  issuer, names, validity, OpenSSL's verification error) for the view.
- When the main frame's load then fails with curl's certificate error, the
  load error sent with `B_WEBKIT_STATE_CHANGED` carries
  `loadErrorCertificate` and `loadErrorCertificateHost`, `…SHA256`,
  `…Subject`, `…Issuer`, `…Problem`, `…Names`, `…ValidFrom` and
  `…ValidUntil` (see [modern-navigation-errors.md](modern-navigation-errors.md)).
- `BWebKitContext::AllowServerCertificate(host, sha256)` and
  `ForgetServerCertificates(host)` keep the trusted certificates of a context.
  The engine does not store them; Summit gives its saved ones to each context
  it creates.

## Viewing a loaded page's certificate

For [issue #41](https://github.com/jmgasper/summit/issues/41), a lock beside the
address opens **Connection security** after an HTTPS page finishes loading.
The selectable native view includes the page URL, subject, issuer, alternative
names, validity dates in UTC, and SHA-256 fingerprint. It uses the certificate
from the committed main-resource connection, without making a second request.

A verified connection uses the supplied lock artwork. An accepted certificate
exception or mixed content adds an amber warning badge; the viewer explains
the warning and never describes a saved exception as verified. Navigating,
switching tabs, changing connection security, or closing the browser dismisses
the viewer. HTTP pages, failed loads and pages still loading have no lock.

`B_WEBKIT_STATE_CHANGED` carries a `connectionCertificate` message for the
committed HTTPS page. Summit binds it to the URL and load generation, preventing
certificate details from a previous navigation appearing beside a new address.

The curl backend also retains the peer's leaf certificate when a TLS session
resumes without a chain. [OpenSSL documents this distinction](https://docs.openssl.org/3.5/man3/SSL_get_peer_cert_chain/).
The native regression covers a verified public site, explicit exception
consent, exact certificate fingerprint, HTTP navigation, tab switches, a
HTTPS-to-HTTP redirect, and loading the exception again through a resumed
session. `tools/bench/certificate-fixture.py --host HOST_IP --directory TEMP_DIR`
provides the HTTP/TLS URLs and fingerprint. Compile
`tests/ModernCertificateInfoTests.cpp` on Haiku with `-std=c++23 -Isrc -Ivendor -lbe`
and run it against an owned browser launched at `https://example.com/`:

```sh
test-certificates TEAM EXECUTABLE HTTPS_BASE HTTP_BASE SHA256 SCREENSHOT_PPM
```

Use a disposable profile. The test approves its temporary fixture certificate
and quits that browser after exercising the real native controls.

X399 validation on 7 October 2026: bundle `bundle-ef1a8im3`, engine patch
`702bb3849f1c36de00ce783c4f1a81563f550a10ad61a59a9dfbc5111c9be8ad`,
72/72 checks pass. Owned team 53859 and its helpers exited cleanly with no new
crash reports or debugger events. The native viewer screenshot was inspected;
all five portable CTest suites pass. Local evidence is
`.vm/issue41-native-results.json` and `.vm/issue41-viewer.png`.

## Sites that ask for a name and password

Added 20 October 2026 for [issue #87](https://github.com/jmgasper/summit/issues/87).
Routers, NAS boxes, printers, intranet and staging servers protect pages with
HTTP authentication: a `401` answer with `WWW-Authenticate: Basic` (or
`Digest`, or `NTLM`). Summit showed the server's 401 page and offered no way
to sign in.

### What the user sees

- A sheet opens over the window: **192.168.1.1 asks you to sign in**, the
  server's own words for the account (*It says: "Router admin"*), **Name**,
  **Password**, **Remember this password**, **Cancel** and **Sign In** (the
  default). On plain `http` it warns in red: with Basic, *your password is
  sent as you type it, and others on the network can read it*; with Digest or
  NTLM, *others on the network can see the page and your name*.
- A wrong name or password asks again, saying *That name or password was not
  accepted*, with the name filled in.
- **Cancel** (and Escape) shows the server's own page for the refusal, as
  other browsers do. Starting another navigation in the tab, closing it, or
  its page crashing closes the sheet.
- **Remember** (on by default when Preferences › Browsing saves passwords)
  keeps the account in the Haiku key store with the other saved passwords
  (#67, keyring "Summit"): it shows in Preferences and in the password export.
  The next time the site asks, one saved account for the site signs in by
  itself, without the sheet; with several, the sheet opens with the first one
  filled in, and typing another saved name fills in its password. A saved
  password the server refuses is asked about, never retried.
- Within a session the engine remembers what was given (WebKit's credential
  storage): a Basic site gets the name and password with its later requests
  without asking, a Digest site answers the server's next challenge itself.
- Private windows ask, but neither fill in nor save.
- A proxy that asks (`407`) gets the same sheet (**The proxy … asks you to
  sign in**), without Remember.
- The sheet belongs to its tab: one waiting in a background tab opens when
  the tab is selected, and several requests for the same server and realm
  (a page and its images) are answered together.

### How it works

- The curl network task already turned a `401`/`407` into an authentication
  challenge for the UI process, which answered every one with the default
  handling. `didReceiveAuthenticationChallenge` in `UIProcess/haiku/WebView.cpp`
  now sends Basic, Digest and NTLM challenges to the host as
  `B_WEBKIT_AUTHENTICATION_REQUESTED` (`"identifier"`, `"url"`, `"host"`,
  `"port"`, `"realm"`, `"scheme"`, `"secure"`, `"proxy"`,
  `"previous_failures"`, `"user"`) when the view asked for them
  (`BWebKitView::SetAuthenticationRequests(true)`; off by default, so other
  programs on the engine keep the 401 page). The challenge waits, on WebKit's
  main thread, until `BWebKitView::AnswerAuthentication(identifier, name,
  password, persistence)` or `CancelAuthentication(identifier)`. A null name
  continues without signing in (the server's page). `B_WEBKIT_CREDENTIAL_FOR_SESSION`
  stores the credential in the session's credential storage, as the curl
  task does for `ForSession` credentials.
- When the view's page starts another main-frame navigation, closes or loses
  its web process, its waiting challenges are cancelled and the host gets
  the notice again with `"withdrawn"`, to close its sheet.
- In Summit, `src/ui/BrowserWindowAuthentication.cpp` keeps the window's
  waiting requests and `src/ui/AuthenticationPrompt.cpp` is the sheet (a
  `B_MODAL_SUBSET_WINDOW_FEEL` window over the browser window). The key store
  is read and written on threads of their own: keystore_server may ask the
  user to allow Summit, and that blocks the caller.

Checked on the X399 on 20 October with a test server on the build host
(Basic and Digest, `user`/`passwd`) and with
`https://httpbin.org/basic-auth/user/passwd` and
`https://httpbin.org/digest-auth/auth/user/passwd`: each asked, a wrong
password asked again saying so, the right one loaded the page; Cancel showed
the 401 page; Remember saved the account, and after a restart both test sites
signed in from the key store without the sheet; navigating away or closing
the tab closed a waiting sheet.

## Remaining work

- `BWebView` programs (WebPositive on Summit's engine, [legacy-webview.md](legacy-webview.md))
  still get no `AUTHENTICATION_CHALLENGE`: the compatibility library does not
  yet ask for `B_WEBKIT_AUTHENTICATION_REQUESTED`.
- A saved account is not tied to the realm: a site's form password and its
  HTTP password share the site's entry in the key store.
