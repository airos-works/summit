# Legacy certificate questions — 22 October 2026

The compatibility library forwards certificate failures through the existing
`SSL_CERT_ERROR` message. `BWebWindow` displays its native Continue/Stop alert
with the failed URL, host, subject, issuer, valid names, UTC validity dates,
SHA-256 fingerprint and session scope. Stop is the default button and Escape
chooses Stop. Certificate questions replace the generic main-document error
alert for that failure. Legacy public headers and class layouts are unchanged.

The engine application thread never waits for the host dialog. A separate
reply handler accepts only a reply message containing `continue: true`;
missing or false approval does not add an exception. A question is
sent once per failed navigation. Replies, navigation withdrawal, process
failure and page shutdown remove pending handlers.

`BWebKitView::ContinueAfterCertificateError(expectedGeneration)` checks the
engine's current failed navigation when it applies the decision. Validation,
trusting the exact rejected host/leaf certificate in that view's context, and
retrying the actual failed HTTPS URL happen on the engine thread. It takes no
caller-supplied host, certificate or retry URL. A changed navigation, Stop,
closed view, invalid generation or already handled failure makes the decision
ineffective. The exception lasts for the context session and is not persisted
by the compatibility library.

## Native acceptance

Acceptance uses an isolated x86_64 airOS engine and shim on the workstation,
private contexts or an owned temporary profile, and a loopback TLS fixture.
The fixture generates two temporary self-signed certificates, separately
records successful HTTP requests and TLS handshakes, rotates its SSL context
when changing certificates, and closes HTTP connections between requests.
No installed engine, regular browser profile or trust store is changed.

`tests/CertificateDecisionTests.cpp` exercises the engine API directly:

- Invalid generations, navigation and Stop cannot approve an earlier failure.
- Reload returns to the last committed document and invalidates the failed
  provisional navigation's approval. A fresh TLS navigation still fails.
- Current approval loads the page and permits reuse in the same context.
- Trust for `127.0.0.1` does not trust `localhost`, even with the same leaf.
- A replacement leaf on an already approved host requires another approval.
- Closing a view before its queued approval runs does not add an exception.
- A fresh context does not inherit exceptions from the previous context.

`tests/LegacyCertificateTests.cpp` uses a real `BWebView` and inherits the
actual `BWebWindow` certificate handler. It checks the native dialog's text
and default button, Stop followed by a new question, Continue and reuse,
application responsiveness while the dialog waits, handler scripting, held
listener replies, navigation withdrawal, stale approval while a new question
waits, and a late reply after view destruction. The server independently
checks that rejected, abandoned and destroyed requests never reach HTTP.

Final acceptance on airOS hrev60207+1733:

| Run | Native assertions | Runner assertions | Input/runtime hashes |
| --- | ---: | ---: | ---: |
| `certificate-engine-run-4` | 50 | 6 | 77 |
| `certificate-legacy-run-2` | 35 | 6 | 78 |
| `certificate-auth-regression-2` | 28 | 6 | 78 |

All three exit zero, drain their owned process groups without forced cleanup,
retain unchanged hashes and have complete native crash intervals with no new
events. The authentication regression uses the existing held-reply and
view-lifetime probe on the final shim. It does not repeat installed
WebPositive's Basic/Digest UI acceptance from the previous change.

The engine is an unprofiled Release airOS build from upstream
`fb054d09146b113aeeabee5f67f4c8b6d9379809`, with patch SHA-256
`5a4ea0655d3a6a401a895f117ba983a6561dd9f2d9eae050ca7b887168d44b30`.
All 1,483 patched source files match the owned build. The deployed engine
library digest is `33df37b5a205c711f6ea36e0adcf33177ce70b3dd093be52dbdbdd69305c96a5`;
the final shim digest is
`925ce9760c8dae2d9adde877aa626a3c35229eb948a4373f609bb350437713bc`.
The shim and probes were cross-compiled with the airOS SDK and then executed
natively. Earlier complete certificate runs are retained separately from
these final results.

Run the native harness with a bundle built for the target OS:

```sh
python3 tools/test-certificate-decisions.py \
  --probe PATH/CertificateDecisionTests --bundle PATH/engine \
  --images-helper PATH/NativeLoadedImages --output NEW_RESULT_DIRECTORY

python3 tools/test-certificate-decisions.py \
  --probe PATH/LegacyCertificateTests --legacy-library PATH/libWebKitLegacy.so.1 \
  --bundle PATH/engine --images-helper PATH/NativeLoadedImages \
  --output ANOTHER_NEW_RESULT_DIRECTORY
```

Compile the probes with the target SDK, this engine's public headers, and
`-std=c++20 -Ivendor -lWebKit -lbe -lnetwork`. The legacy probe also needs the
legacy headers, private netservices headers, and `-lWebKitLegacy`. Give the
probe and shim an RPATH to the selected isolated engine: on Haiku an existing
RPATH can take precedence over `LIBRARY_PATH`. The harness verifies the loaded
engine and shim before releasing its first-question gate, records input and
initial runtime hashes, rechecks them after exit, and monitors the native crash
log interval. It records process-group cleanup separately from assertion results.

The [machine-readable evidence](legacy-certificates-2026-10-22.json) includes
the exact builds, complete native reports and logs, and retained failures.

## Installed stock WebPositive

The unchanged `/boot/system/apps/WebPositive` on stock R1/beta6
`hrev59866+79` also passes the certificate UI checks. It loads the complete
published stock engine from `958ec59` (`20261021.2134`), with the matching
shim cross-compiled from that commit using the stock SDK. The engine is
extracted into an isolated directory; installed packages are unchanged.
Loaded-image hashes confirm that WebPositive uses the selected engine and shim.

`tools/test-legacy-webpositive-certificates.py` drives the actual application's
native dialog and checks its URL, subject and certificate fingerprint. Stop
prevents the HTTP request; Continue loads the requested document, executes
JavaScript and paints the expected blue pixels in the real browser view.
Session reuse needs no new prompt. A replacement certificate and a different
hostname each require separate approval before any HTTP request is sent.

| Run | Passed checks | Input/runtime hashes | Saved tabs |
| --- | ---: | ---: | --- |
| `ui-run-2` | 21 | 76 | Temporarily set aside and restored byte for byte |
| `ui-run-3` | 21 | 105 | Normal WebPositive session restoration |

Both runs exit zero, drain all owned helpers without forced cleanup, retain
unchanged input/runtime hashes and have complete native crash intervals with
no new events. These use the owned VM's normal profile. The optional
`--fresh-session` mode preserves and restores only its `Session` file;
WebPositive still writes its ordinary history and settings. The subsequent
normal-session run checks shutdown with restored tabs too.

The first run passed all 20 dialog/rendering checks but failed the final
shutdown check: WebPositive exited zero while one owned process remained
after 15 seconds and required cleanup. Its identity was not captured, and
the initial saved-session state was not preserved. The runner now records
remaining processes before cleanup. The two successful runs do not establish
the cause or a fix for that earlier failure; it remains in the evidence.

To repeat in an owned stock VM, build the shim and helpers with the matching
stock SDK and run:

```sh
python3 tools/test-legacy-webpositive-certificates.py \
  --library PATH/libWebKitLegacy.so.1 --bundle PATH/lib/summit-webkit \
  --ui-helper PATH/LegacyWebPositiveUITests \
  --images-helper PATH/NativeLoadedImages \
  --egl-vendor PATH/egl-vendor.json --output NEW_RESULT_DIRECTORY
```

The EGL vendor JSON must select the isolated bundle's Mesa library. The
runner refuses to start if WebPositive is already running. For these tests,
the shim's RPATH selects the isolated engine; no preload is used. The stock
build, package/source audits, complete reports, logs and first failure are in
the `installed_stock_webpositive` section of the machine-readable evidence.

## Retained failures and limits

- Engine attempt 1 selected the installed older engine through the probe's
  RPATH and failed to resolve the new API before initialization. The deployment
  was corrected to select the isolated candidate explicitly.
- Engine attempt 2 assumed Reload would retry the failed provisional HTTPS
  URL. The server showed it reloaded the previously committed HTTP page.
  The corrected test checks that behavior and then verifies a fresh HTTPS
  request remains untrusted. There was no native crash or forced cleanup.
- The first authentication regression invocation stopped before launching a
  native process because its harness unnecessarily required an installed
  WebPositive executable. The harness now requires it only for the actual
  WebPositive UI cases; the lifecycle probe uses its selected isolated bundle.
- The airOS lifetime checks use the inherited `BWebWindow` dialog in a test
  host; the stock checks above use installed WebPositive. These are separate
  matching builds. airOS engine libraries must not be substituted into a
  stock Haiku installation.
- The old listener protocol has no withdrawal message that can close an
  already displayed host-owned dialog. Its late response is ignored. Trust
  is session-only; this change does not add persistent legacy exceptions.
- The fixture uses self-signed certificates. Expired, name-mismatched, proxy
  and public-site certificate failures have not separately been exercised.
- The modern Summit browser still uses its existing persistent certificate
  workflow. This change adds the engine operation and connects the legacy host.
