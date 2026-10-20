# Modern navigation errors

Summit retains the current document when a replacement request fails before
commit, and shows an error page in its place (#89, below). The selected tab's
status bar shows the failed URL and the engine's description; its tooltip
contains the full message. A failure after commit reports that loading was
interrupted, in the status bar only. Enter in the focused address field
submits the address even when its text is unchanged, allowing a retry.
Changing focus still preserves an unsubmitted address draft.

## Error pages (#89)

A main-frame load that fails before commit gets a page in the tab, made by
the browser in the tab's card like the crashed-page view
(`src/ui/BrowserWindowLoadErrors.cpp`): a title for the kind of failure, a
sentence or two, the address and **Try Again**. The kinds come from the
engine's domain and code (`src/core/LoadError.cpp`, `ClassifyLoadFailure`):

| Kind | Title | Engine error |
| --- | --- | --- |
| Host not found | Can't find example.com | `CurlErrorDomain` 5, 6 |
| Refused | Can't connect to example.com | 7 |
| Reset | Can't connect to example.com | 8, 16, 18, 52, 55, 56, 92 |
| Too slow | The server took too long | 28 (curl's 60 s idle timeout) |
| Offline | You're offline | any network failure (not to localhost) while no interface other than loopback is up, with a link and an address that is not link-local |
| Secure connection | Secure connection failed | 35 and the other TLS codes; a certificate Summit can trust after asking ([certificate-warnings.md](certificate-warnings.md)) still asks instead |
| Other | Summit can't open this page | the engine's description |

None for `WebKitErrorDomain` 100, 102 (download and policy hand-offs), 203
and 204 (a media document took the load over: that load counts as a success,
with history and the lock), `WebKitNetworkError` 302 (stopped) and an
extension view's own refusal.

- The address that failed stays in the address field, to correct or enter
  again; the tab and the window are titled after the error.
- Try Again, Reload (toolbar, View menu, the tab's menu, MCP `tab_control
  reload`) and Return while the button has the focus (it takes the focus from
  the page when the error appears) load the address again. The button is not
  the window's default button, so Return in the address field or in another
  tab is not taken. While the new attempt runs the page stays, its button
  reads "Trying Again…"; it goes when the new load has data (its progress
  passes the 0.1 every load starts at) or finishes, and a new failure
  replaces its text.
- Back (and MCP `tab_control back`) shows the page that was there before,
  which the failed load never replaced; it is enabled even when that page
  has no history of its own. A tab whose first load failed has nothing
  behind.
- An offline page loads again by itself: while one is shown, the window
  looks at the interfaces every 2 s (`BNetworkRoster`), and retries once the
  network has been up for two looks in a row (an address from DHCP comes a
  moment before its name server answers). The engine has no online signal for
  the browser (#85 gives pages `navigator.onLine`), so the browser looks for
  itself.

Errors belong to individual tabs. Starting another navigation clears that
tab's old error. Stop and download handoffs finish as cancellations. A complete
HTTP error-status document, such as a site's 404 page, is a successful document
load. Failed and stopped requests do not create history visits. A failed reload
preserves the title from the earlier successful visit.

## Native state notifications

`B_WEBKIT_STATE_CHANGED` carries copied values in addition to the existing URL,
title, loading, progress and close-handshake fields:

| Field | Meaning |
| --- | --- |
| `loadGeneration` | Per-view load generation used to reject older error snapshots. |
| `loadOutcome` | `idle`, `loading`, `succeeded`, `cancelled`, `failed`, or `process-exited`. |
| `loadError` | Whether this snapshot contains a load failure. |
| `loadErrorDescription`, `loadErrorDomain`, `loadErrorCode` | Original engine diagnostic, with a fallback description if empty. |
| `loadErrorURL`, `loadErrorProvisional` | Failed request and whether it failed before document commit. |
| `loadSuccessSequence`, `loadSuccessURL`, `loadSuccessTitle` | Last successful visit retained in subsequent snapshots. |
| `loadErrorCertificate` | The load failed because the server certificate failed verification; then also `loadErrorCertificateHost`, `…SHA256` (hex of the DER leaf), `…Subject`, `…Issuer`, `…Problem` (OpenSSL's reason), `…Names` (comma-separated) and `…ValidFrom`/`…ValidUntil` (seconds since the epoch). See [certificate-warnings.md](certificate-warnings.md). |

The engine associates terminal callbacks with a NavigationIdentifier. An
anchor navigation can report the existing DocumentLoader's ID; the adapter
also checks an explicitly requested fragment URL and its unchanged document
identity. Reloads and unrelated newer requests keep their own navigation
checks. Back/forward cache callbacks without an ID use the committed document
identity when there is no newer pending API request.

The success sequence makes history updates independent of a loading indicator
transition. Dynamic titles update an existing visit only while the matching
document has a successful outcome. Close-handshake generation remains separate
from this load bookkeeping.

## Verification

Run against an explicitly supplied frozen full-browser bundle:

```sh
python3 tools/test-modern-load-errors.py --bundle /boot/home/summit/build-modern-browser/BUNDLE
python3 tools/test-modern-close.py --bundle /boot/home/summit/build-modern-browser/BUNDLE --port 0
python3 tools/test-modern-downloads.py --browser --bundle /boot/home/summit/build-modern-browser/BUNDLE
```

These tests use dedicated HTTP fixtures and exact native executable checks.
The load-error fixture drops connections before headers, truncates committed
HTML, serves a valid 404 document, stalls a request for Stop, and overlaps two
requests to the same URL. Native assertions cover retry, independent tab state,
fragment replacement, back/forward navigation and the saved history file.
Close and download checks exercise the existing native dialogs and draft
preservation. The runners reject fresh native debugger events during runtime.
Run them serially because that final log check covers the whole test VM.

See [the verification record](STATUS.md) for exact results and failed runs.
Certificates that fail verification now ask whether to continue
([certificate-warnings.md](certificate-warnings.md)), and so do sites that ask
for a name and password (HTTP authentication, same page). The error pages were
checked on the X399 on 20 October 2026 (see
[issue-fixes-2026-10-20-app.md](issue-fixes-2026-10-20-app.md)).
