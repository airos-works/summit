# Legacy BWebView HTTP authentication — 22 October 2026

The legacy shim opts its engine views into HTTP authentication and forwards
challenges as the existing `AUTHENTICATION_CHALLENGE` message. The installed
WebPositive now shows its own authentication panel for Basic and Digest login.
The engine application thread continues running while that panel waits.

Each challenge gets a separate native reply handler. Legacy replies have no
request identifier, so that handler binds the reply to the original engine
request. Only reply messages can answer a challenge; ordinary native handler
queries retain their normal behavior. Answer, withdrawal and page shutdown
remove the handler. A missing
username/password cancels the request, matching the old legacy protocol; two
empty strings continue without credentials to the server's 401 page. Accepted
credentials are kept for the engine session. WebPositive continues to manage
its own persistent Remember storage; this change does not introduce another
persistent credential store.

The prompt text includes scheme, host, port, realm and authentication scheme.
WebPositive uses that text as its credential-storage key, so including the
origin's scheme and port prevents collisions between distinct origins. Proxy
requests have a distinct prefix. No credentials are logged by the bridge.
Public headers and public class layouts are unchanged.

## Acceptance

The primary run uses the installed, unmodified WebPositive in the owned stock
Haiku R1/beta6 VM, hrev59866+79, and the complete published stock engine from
`56f49a8`. An isolated candidate shim is selected through `LIBRARY_PATH`; no
installed package, engine file or preload is substituted.

`tools/test-legacy-authentication.py` hosts a loopback-only HTTP fixture and
uses native scripting to fill WebPositive's actual controls and send its OK
button message. It verifies origin/realm/scheme text, an incorrect Basic
password followed by a successful retry, Digest login, Cancel, an empty
credential response, and same-session reuse without another panel. Successful
pages report their load back to the server. Each realm has a different public
test password, so a credential sent preemptively from an earlier case cannot
accidentally satisfy a later challenge.

The final UI run, `/Data/issue113/auth-candidate-6`, passes **19 assertions**,
exits zero, drains its process group without forced cleanup, and has a clean
native crash interval. Its 104 input/runtime hashes are unchanged. Images are
hashed when first observed and rechecked after exit; the report distinguishes
the initial and later loaded-image snapshots. The old shim baseline loads the
401 page without a prompt and exits cleanly.

`tests/LegacyAuthenticationTests.cpp` is a separate native listener probe for
held replies, navigation withdrawal, a stale reply while another challenge
waits, and closing a view with an outstanding request. Run 4 passes 25 native
assertions with legacy tracing enabled. Run 5 adds an explicit engine
initialization check and passes **27 native assertions**, without that tracing,
plus six runner assertions. Both exit normally with clean crash intervals and
unchanged inputs. The server verifies that abandoned and destroyed requests
never authenticate, while a fresh request accepts its own reply.

Final review added a guard so ordinary native handler messages cannot be
interpreted as credential replies. The final binary passes **28 native
assertions** in `auth-lifecycle-6`, including a scripting query to the pending
reply handler, plus six runner assertions. The final installed WebPositive
run above and this lifecycle run both use the final digests below. The existing
three-case popup suite also passes 34 assertions in `auth-popup-regression-2`
with that same shim, clean exit, unchanged hashes and no fresh crash events.

Both the stock SDK and native airOS builds succeed. All 16 airOS shim source
and header files match the repository. Build digests:

| Shim | SHA-256 |
| --- | --- |
| Stock x86_64 | `6994e4195e7f5cd9c385ae2ffb42bb4820408094e9c6b490c90efc3eaab6e6c9` |
| Native airOS x86_64 | `135bd496d0bb78b1eddc7d3fe7e025a8d4e500c0e0765d3a87286aa918da4480` |

Native runtime acceptance here is on stock Haiku. The airOS result for this
change is compilation and source equivalence, not a runtime acceptance claim.

## Retained failures and limits

The [machine-readable record](legacy-authentication-2026-10-22.json) retains the
baseline, successful reports and failed attempts:

- UI run 1 addressed `Text` instead of the native text control's `Value`
  property. Runs 2 and 3 used an incorrectly encoded navigation message, so no
  Digest request reached the server. Correcting it to `B_ARGV_RECEIVED`
  (`_ARG`) with an integer `argc` allowed Digest to pass.
- UI run 4 passed Basic retry and Digest. Its cancellation fixture reused the
  same password, accepting the engine's preemptive credential before it could
  challenge. Distinct per-realm passwords made the cases independent; run 5
  then completed the whole sequence.
- Lifecycle runs 1 and 2 requested a 50 ms application pulse. Haiku rounds that
  down to zero, disabling the probe's handler-count observations and cleanup.
  The app remained responsive to native scripting, but these runs failed and
  required timeout cleanup. The corrected probe requests the supported 100 ms
  interval. These failed runs are not counted as acceptance.
- Lifecycle run 3 timed out before its first request reached the server, with
  no fresh crash event. Its cause was not established. Run 4 with diagnostic
  tracing completed; run 5 then completed without tracing and checked engine
  initialization explicitly. The startup failure remains in the record.

The legacy listener protocol has no withdrawal notification to dismiss a
host-owned authentication panel. Withdrawing a request invalidates its reply
handler; a host's existing panel may remain until the user dismisses it.
NTLM, proxy authentication and persistent Remember behavior are not exercised
by these fixtures. Certificate-error forwarding is still absent. Live OAuth
popup sign-in and the other [#113](https://github.com/jmgasper/summit/issues/113)
items remain open.

## Reproduction

In an owned stock Haiku VM with the matching full engine package installed,
no WebPositive running, the helper from `tests/LegacyWebPositiveUITests.cpp`
and `NativeLoadedImages` available, and the repository-relative test sources
beside the runner:

```sh
python3 tools/test-legacy-authentication.py \
  --library /absolute/path/libWebKitLegacy.so.1 \
  --ui-helper /absolute/path/LegacyWebPositiveUITests \
  --images-helper /absolute/path/NativeLoadedImages \
  --output /absolute/path/new-result-directory
```

Use `--expect-unavailable` with the old shim for the baseline. For lifecycle
coverage, build `tests/LegacyAuthenticationTests.cpp` against the same target
SDK with the legacy/public engine headers, `-Ivendor`, `-lWebKitLegacy`,
`-lWebKit`, `-lbe` and `-lnetwork`, then add
`--lifecycle-probe /absolute/path/LegacyAuthenticationTests`. Its runtime path
should include the installed engine directory only; `LIBRARY_PATH` selects the
isolated shim. The probe waits at an inventory gate before continuing, so its
runtime files can be hashed before the lifecycle checks.
