# Native programmatic action popups

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The Haiku `openPopup` implementation passes 342 browser checks across 41
background commands, repeated on three fresh profiles in QEMU.
Patch `591548f4b18259ad3de2ac4eeac855d882176d083f6718b15f41f5b2fd424c34`
adds the API, IPC receiver, and native presentation completion. The tested native
bundle is `/SummitExtensions/summit/build-modern-browser/bundle-en85evku`.
The preceding distribution, `bundle-ptx4moys` with patch `0cc5c59e...`, does not
expose this API. The new bundle also passes the icon, toolbar-action, and overflow
regressions. Its verified host copy is
`artifacts/modern-browser/bundle-en85evku`; see [copy provenance](modern-bundle-copy.md#programmatic-action-popups).
Full browser and Safari/Chrome/Firefox extension compatibility remain unfinished.

Actual QEMU preview: [programmatic popup](screenshots/native-programmatic-popup.png).
The popup was opened by the background API over the fixture's loopback control
channel. Its response and popup script both reported redacted page metadata and
zero action clicks.

## Behavior

The API accepts an optional window or tab target. The target must be an
accessible, live, focused browser window and its active tab. Omitting the target
selects the focused window. A disabled action, absent popup, invalid target, or
another pending or presented extension popup causes a rejection.

Programmatic presentation uses the extension popup host directly. It does not
grant `activeTab`, call the trusted toolbar action path, or dispatch `onClicked`.
The promise resolves with `undefined`, or the callback runs without arguments,
only after the native window reports presentation. A failed or cancelled load
rejects the promise or reports the callback error through `runtime.lastError`.

The popup remains hidden while its document loads. The browser checks owner
focus when the document becomes ready, and the native window checks it again
immediately before presentation. A busy owner looper causes a later retry;
an inactive, hidden, or minimized owner cancels presentation. Navigation, tab
closure, extension unload, and the 30-second load deadline cancel pending work.
Closing clears the popup's page and context identities before replying, so a
late callback cannot revive the old popup.

The existing trusted toolbar path still grants the user gesture and supports
either a popup or `onClicked`, according to the configured action.

## Verification

- The old native host, adapted only to accept and ignore the new focus-policy
  argument, fails eight assertions in the focus tests. The changed host passes
  all 129 native window and lifetime checks, including focus loss after a show
  request but before the native presentation handler runs. These checks do not
  run WebKit documents or the extension API.
- The API, generated bindings, popup implementation, and regenerated IPC compile
  in the native preflight. The popup unit was compiled again after adding the
  native focus gate. Generated binding/platform verification passes 512 checks.
- The full 326-step engine rebuild succeeds. The browser harness compiles and
  runs against the resulting frozen bundle. Its JavaScript command channel was
  exercised against the old bundle: native installation succeeds, the target's
  URL and title remain redacted, and `openPopup` is absent as expected.
- The browser suite verifies promise and callback calls through both
  `browser.*` and `chrome.*`, actual popup DOM/API execution, permission checks,
  invalid targets, pending loads, focus changes, navigation, tab closure,
  timeout, unload/re-enable, and trusted toolbar positive controls. Each of the
  three runs passes 342 checks and 41 commands. Browser and harness exit with
  status zero, their process groups drain without forced cleanup, and the
  native crash monitor records no events. Frozen bundle, staged harness,
  installed fixture, and host source hashes remain unchanged.
- The same bundle passes 455 icon checks (49 API cases and 55 pixel
  observations), 238 toolbar-action/popup checks, 29 extension-identity checks,
  and 169 overflow checks. Their processes also exit cleanly, with complete,
  empty native crash-monitor results and unchanged bundle and staged inputs.

Run the browser suite against an unused, frozen modern-extensions bundle:

```sh
python3 tools/test-modern-extension-open-popup.py --watch --bundle /path/to/bundle
```

The harness installs through the native file picker and consent dialog. A local
HTTP control channel invokes the background API without clicking the toolbar.
The target page uses `localhost`, while the extension only has a `127.0.0.1`
host permission, allowing the tests to observe `activeTab` isolation. Both absent
and blank URL/title values count as redacted; trusted clicks must reveal the
exact target URL. A successful open response must already have a visible native
popup when observed by the harness.

The fixture waits for a positive native load-success sequence and a successful
load outcome. A new tab already contains its requested URL while still idle;
the earlier URL/not-loading predicate could start popup tests before navigation.
The passing runs record and wait through that state. During tab closure, the
popup rejection can precede replacement-tab activation. The test checks that
all returned metadata remains redacted during this transition, then requires
the original tab identity after the close commits. Trusted-click controls
verify that same-origin navigation retains `activeTab`, while a second localhost
port changes the origin and revokes it.

Evidence retained in `.vm/`:

- `native-extension-popup.EnePNfze/result.json` — old focus behavior fails.
- `native-extension-popup.VQbvqIx5/result.json` — changed native host passes.
- `extension-lifecycle-inputs.ypzDZwVf/result.json` — initial four-unit compile.
- `extension-lifecycle-inputs.ybmgnvIx/result.json` — popup unit with focus gate.
- `action-open-popup-promoted-binding-check.json` — binding verification.
- `open-popup-harness-compile.json` — latest native harness compilation.
- `modern-extension-open-popup-4bfdc7e19798d8d6f115d7e5/result.json` — old browser
  control-channel and missing-API observation.
- `modern-extension-open-popup-b54d7dc23b558531d34a9ba5/result.json` — complete
  browser suite, including load readiness and distinct-origin permission checks.
- `modern-extension-open-popup-1ab88f8d01eb8011100d180f/result.json` and
  `modern-extension-open-popup-0e4fe68be6dee4cd92661dd6/result.json` — independent
  fresh-profile repeats of the complete suite.
- `modern-extension-icons-193a531f5e675ff62ed2cac8/result.json`,
  `modern-extension-actions-0238ae61feb6ab17de857eb5/result.json`, and
  `modern-extension-overflow-810fdf3a779655aaba4edc8f/result.json` — regressions
  against the same frozen bundle.

Earlier failed browser runs are retained in
`modern-extension-open-popup-34da39069a25531074df7b4b/result.json` (a temporary
empty active-tab snapshot during close),
`modern-extension-open-popup-f86455bf7871d6dbfeb955f9/result.json` (popup visibility
after the old new-tab readiness predicate), and
`modern-extension-open-popup-deb00e01e4a4b1e478de66cd/result.json` (the old fixture
incorrectly expected same-origin navigation to revoke access). The visibility
failure did not capture an engine close reason; the successful-load prerequisite
addresses the observed premature readiness condition, and all three corrected
runs pass without relaxing the presentation assertion.

Private browsing and multiple browser window cases still require broader
application support and runtime coverage. Cocoa adapter compilation and runtime
have not been verified by the Haiku checks.
