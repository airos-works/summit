# Modern navigation errors

Summit retains the current document when a replacement request fails before
commit. The selected tab's status bar shows the failed URL and the engine's
description; its tooltip contains the full message. A failure after commit
reports that loading was interrupted. Enter in the focused address field
submits the address even when its text is unchanged, allowing a retry.
Changing focus still preserves an unsubmitted address draft.

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
for a name and password (HTTP authentication, same page). An error page with
recovery controls and wider network-failure coverage remain separate work.
