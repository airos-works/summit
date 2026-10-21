# Legacy BWebView popup acceptance — 22 October 2026

The `legacy/` shim now accepts the engine's opened page when the host attaches
its new `BWebView` to a window. The former default declined that page and loaded
its URL independently, so `window.open()` returned `null`, the child had no
opener, and an empty-URL popup was discarded. The opt-in
`SUMMIT_LEGACY_OPENED_PAGES` switch and independent-load fallback are removed.
Public headers and class layouts are unchanged.

This completes the controlled popup mechanics in [#113](https://github.com/jmgasper/summit/issues/113).
The issue remains open: live GitHub/Google sign-in, authentication prompts,
settings, frame access, Inspector, cookie UI and image packaging still need
work. These tests do not establish when the historical blank-page problem was
fixed in the engine.

## Native coverage

The primary test launches the **installed, unmodified WebPositive** in an owned
stock Haiku R1/beta6 VM, hrev59866+79, with the candidate shim selected through
`LIBRARY_PATH`. The installed engine is the complete published stock x86_64
package from `56f49a8`; no airOS engine files or preloads are substituted. No
package installation changes were needed.

`tests/LegacyWebPositiveUITests.cpp` sends native mouse messages to the real
engine view and inspects WebPositive's windows. The fixture reports actual
page events to a loopback server. For each case the harness requires:

- A non-null `window.open()` result and a child with `window.opener`.
- A child-ready message and a ping/pong exchange, checking origin and source.
- Visible page pixels with the fixture's exact RGB `(66, 140, 222)`.
- The child responding to a message by calling `window.close()`, the opener
  observing `child.closed`, and WebPositive removing its tab or window.

The three cases are a sized popup, an ordinary new tab, and an initially blank
popup filled with `document.write()`. All pass without the old opt-in variable.
The process exits zero, its helper group drains without forced cleanup, and the
system-wide native crash monitor reports no new debugger or crash events.
The harness hashes its inputs and loaded runtime files before use or when first
observed, then verifies those hashes again after exit. This is first-observation
coverage, not a claim that every image was mapped at the initial snapshot.

The old shim's default path is also tested: its child renders, but the opener
is absent and the return value is null. That expected-failure baseline passes
10 assertions. The old opt-in path passes a separate 15-assertion real
WebPositive probe. The candidate's expanded three-case run passes 37 assertions
(`webpositive-candidate-4`). The final helper/runner sources are exercised again
in `webpositive-candidate-5`, which passes 40 assertions with the same clean
exit, hash and crash-monitor results. Both full reports are retained.

A separate native airOS probe on hrev60207+1733 passes 25 assertions with the
candidate shim and the private CPU/memory engine candidate described in
[the resource-usage record](resource-usage-2026-10-22.md). This probe creates
`BWebView` directly, uses Inspector only for observation, verifies presented
pixels, exchanges messages and closes the popup. It is supplementary evidence;
the three-case installed WebPositive acceptance is on stock Haiku.

## Provenance and failed attempts

The [machine-readable record](legacy-popups-2026-10-22.json) contains full stock
run reports, the airOS candidate probe report and log, source digests, and the
stale-dialog evidence. The candidate libraries are:

| Build | SHA-256 |
| --- | --- |
| Stock SDK, installed WebPositive test | `977a97bbfbec2f1eab7fdcf758a2382eaef7cdcd41e19dbe7446c874bdb44ebd` |
| Native airOS, BWebView probe | `e69a649d8a7892c524c4108c6a907c7a944613b6a5772eb3b0b66da5fa87fe40` |

Both shims build successfully. All 16 native airOS shim source/header files
match the repository; the stock build uses those repository files directly and
is up to date. Runtime image enumeration verifies that each passing run loaded
its intended shim. The primary stock reports also record and recheck its
resolved library hash.

Failures are retained rather than counted as acceptance:

- The first airOS probe attached Inspector before navigation committed and
  timed out before the click. Moving observation after navigation fixed the
  probe sequence. A later candidate deployment still loaded the old shim
  through its executable's RPATH; relinking the probe with the candidate RPATH
  corrected that deployment, after which the default candidate passed.
- Stock candidate run 1 timed out waiting for popup closure. Its crash interval
  was clean. The exact cause was not established; later expanded runs exercise
  closure again. This one failure is not attributed to the rendering issue.
- Stock candidate runs 2 and 3 passed popup closure and tab messaging but failed
  the tab's screen-pixel assertion. Run 3's screenshot shows the blue child page
  behind an old floating `debug_server` dialog. There were no new crash records
  in those runs. Three stale dialogs named `/boot/system/lib/summit-webkit/WebProcess`;
  native process enumeration showed no WebProcess or WebPositive teams left.
  Their syslog debugger records predated these tests. The evidence was saved,
  and only those dialogs in the owned VM were dismissed. The unchanged candidate
  library and frozen inputs then passed all three cases in run 4.

## Reproduction

Build the UI helper against the target Haiku SDK with C++20, `-Ivendor` and
`-lbe`. Build the shim with `legacy/Makefile`, setting `ENGINE_HEADERS` and
`ENGINE_LIB` to the matching target engine. The engine runpath for the stock
package is `/boot/system/lib/summit-webkit/lib`.

In an owned native VM with the matching full engine package installed, no
WebPositive already running, and the test desktop unobstructed:

```sh
python3 tools/test-legacy-popups.py \
  --library /absolute/path/libWebKitLegacy.so.1 \
  --ui-helper /absolute/path/LegacyWebPositiveUITests \
  --images-helper /absolute/path/NativeLoadedImages \
  --output /absolute/path/new-result-directory
```

The helper `NativeLoadedImages` is built from `tests/NativeLoadedImages.cpp`.
Preserve the repository-relative fixture/helper source paths alongside the
runner. It uses WebPositive's normal settings in that owned VM and refuses to
reuse an existing WebPositive process. `--expect-fallback` exercises only the
old default's null/opener baseline. On failure the runner saves a full-screen
PPM capture and cleans up only its own process group.
