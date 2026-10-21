# Native network shutdown recheck, 22 October 2026

**The network shutdown recheck passes on both x86_64 release targets.** Published
`56f49a8` packages pass five complete cycles each on stock R1/beta6 and airOS,
including persistence, native crash monitoring and complete process cleanup.
The two defects found by the earlier runs are addressed by the curl connection
policy fix (`b22cb50`) and early locale initialization (`7d0654b`). The earlier
failures remain recorded below. #120 stays open for its other items.

Release CI [39649186372](https://github.com/jmgasper/summit/actions/runs/39649186372)
succeeded. Both targets used the complete published packages from 21 October,
18:02 UTC, without a preload or substituted engine library. Stock packages were
installed with `pkgman`; the workstation packages were extracted into an
isolated directory, relocating only the app's RPATH and Mesa vendor JSON.
Every cycle closed three held HTTPS bodies and one secure WebSocket, preserved
cookie/localStorage/Cache API values, exited zero and left no owned process.
Both native crash intervals are clean; runtime and fixture hashes are unchanged.
Package hashes, build metadata and per-cycle evidence are in
[the release report](network-release-2026-10-22.json). These finite x86_64 checks
do not claim long-duration reliability or ARM runtime acceptance.

The test uses published packages with the current engine pin `fb054d0`:
stock R1/beta6 `cd9de3e`, and airOS `008ccfc`. Both contain the same engine
patch (`df10f964784bb1ada936435c86d1170e0a085600bb92d872ee673d00e8a73fc4`).
These are release CI builds, without PGO profile use. Package, executable,
loaded-library and fixture hashes, per-cycle results, and the native crash
stack are in [the machine-readable report](network-shutdown-2026-10-22.json).

## Follow-up: requests waiting behind an HTTP/1.1 stream

The airOS trace shows sibling requests entering curl's `PIPEWAIT` state while
a TLS connection negotiates its protocol. That connection falls back to
HTTP/1.1 without ALPN and starts a response whose body remains open. The
sibling requests stay pending, even though the server has already sent
HTTP/1.1 headers. The fixture's service-worker and page transfers reproduce
this with curl 8.22.0; a later stock-Haiku run with curl 8.21.0 also stalled.

The Haiku `CurlHandle::enableHttp()` path now uses curl's default connection
policy (`CURLOPT_PIPEWAIT=0`). It keeps ALPN, HTTP/2 and the existing per-host
connection limit. This permits another connection while negotiation is pending;
it may open more connections at startup than the previous policy. See curl's
[PIPEWAIT contract](https://curl.se/libcurl/c/CURLOPT_PIPEWAIT.html).

Native workstation validation of patch
`483862e88fdc5a784c4ae2c6aaa2f6358d637d8ebca4e46ccd129e0d3e55077a`:

- A diagnostic preload changing only this option first made all five cycles
  pass on the published engine. The trace-only control still stalled.
- The rebuilt engine then passed all five cycles **without a preload**:
  three live HTTPS bodies and a secure WebSocket at each quit, two service-worker
  unregistrations, persistent cookie/localStorage/Cache API values, zero browser
  exit codes, empty process groups and no fresh debugger events.
- A separate HTTP/2 server kept one response stream open while twelve other
  responses completed on that same established connection. The browser reported
  `h2`; server session/stream IDs independently confirm multiplexing. Browser
  shutdown was clean. Total TLS-session counts include certificate-challenge
  retries against the fixture's self-signed certificate, so they are not a
  cold-start performance measurement.

All four engine targets built. The isolated source audit matched all 1,472 port
files, and loaded runtime hashes stayed unchanged during the tests. This uses
the existing PGO-profile engine build; release CI builds are checked separately.
Evidence: [candidate and HTTP/2 reports](network-pipewait-2026-10-22.json).

Reading the full stock browser log also refined the graphics failure: libepoxy
printed `Unable to interpret GL_VERSION string: 3.1 Mesa 25.3.6` **before** the
quit trace. Shutdown monitoring detected the trapped helper, but the evidence
does not establish shutdown as its trigger. Standalone native probes passed
800,000 concurrent scans of that string and 400,000 version queries across four
EGL contexts. Another 24 browser startup/animation/quit cycles passed. The
original abort is retained as a failure. The subsequent investigation below
identifies the first-time locale initialization those standalone probes missed.

### Published network fix and locale startup diagnosis

Release CI [39646509855](https://github.com/jmgasper/summit/actions/runs/39646509855)
completed successfully. Stock-Haiku packages `e732dd0` (21 October, 17:44 UTC),
containing `b22cb50`, pass five full TLS/worker/WebSocket shutdown and persistence
cycles without a preload. The loaded runtime and fixture hashes remain unchanged,
all owned groups exit, and the native crash interval is clean. This verifies the
network fix in the release configuration; that release predates the locale
workaround.

Further diagnostics reproduced the graphics abort and identified a separate
libroot initialization race: the first time conversion publishes its ICU ctype
tables before filling them. A concurrent `sscanf()` can then reject the `3` in
the valid Mesa version string. Early locale initialization now works around this
in the Haiku engine. See [the locale investigation](locale-startup-2026-10-22.md)
and [its evidence](locale-startup-2026-10-22.json) for the reproducer and candidate
browser checks. The completed release validation is recorded at the top.

## Workload and results

`tools/test-modern-network-shutdown.py` starts an owned HTTPS server and browser
profile. The certificate is trusted only in that profile. It requires all of:

- A controlling service worker that returns an intercepted response.
- Live HTTPS response bodies consumed by the page, a dedicated worker and the
  service worker, plus a secure WebSocket echo. All observed connections used
  TLS 1.3 with `TLS_AES_256_GCM_SHA384`.
- Cookie, localStorage and Cache API values that survive browser restarts.
- Service-worker unregistration before closing on alternating cycles.
- A native quit request, browser exit status zero, an empty owned process
  group, and all four active network connections closed.
- Complete native syslog coverage without new debugger events, and unchanged
  test inputs and loaded runtime files.

| Platform | Observed result |
| --- | --- |
| Stock R1/beta6, hrev59866+79 | Five consecutive cycles passed. All three persistent values increased from 1 through 5; both unregistrations succeeded. No forced cleanup or new debugger event. |
| Stock R1/beta6, subsequent run | Browser exited zero and connections closed, but WebProcess 1866 remained in the debugger. The monitor caught `abort()` on thread 1899. The test failed and killed its remaining process group. |
| airOS workstation, hrev60207+1733 | One complete cycle passed. On reopening, the service-worker transfer and secure WebSocket worked, but the page transfer and `worker.js` request remained pending until the fixture's 20-second readiness deadline. The run failed and cleaned up its own browser. No new debugger event. |

The stock failure's native stack runs through libepoxy,
`TextureMapper::bindDefaultSurface()`,
`ThreadedCompositor::paintToTextureMapper()` and `renderLayerTree()`.
It is a **WebProcess graphics failure**, distinct from the historical
NetworkProcess exit reports. The follow-ups above establish the locale-startup
cause and the separate curl connection-wait defect; the original failures are
retained in the report.

The existing NetworkProcess fix still calls `exitProcess()` after normal
auxiliary event-loop/session teardown, avoiding libbe static destruction
while worker threads remain active. The older `terminateWorker` stack had
a different trigger: libnetwork's resolver parser called `exit()` from a
resolver thread. That OS problem and the stock-Haiku workaround remain tracked
in [#119](https://github.com/jmgasper/summit/issues/119). This finite test does
not prove that a rare resolver failure cannot recur.

## Repeating the test

On native Haiku, build the two helpers from the repository root:

```sh
g++ -std=c++23 -O2 -Isrc -Ivendor -Wno-multichar tests/ModernCloseTests.cpp -lbe -o /tmp/ModernCloseTests
g++ -std=c++23 -O2 tests/NativeLoadedImages.cpp -o /tmp/NativeLoadedImages
python3 tools/test-modern-network-shutdown.py \
  --browser /boot/system/apps/Summit \
  --close-helper /tmp/ModernCloseTests \
  --images-helper /tmp/NativeLoadedImages \
  --output /boot/home/network-shutdown-new-run --runs 5
```

The output directory must be new. Run serially with other native acceptance
tests: the debugger monitor is system-wide. For cross-built helpers, use the
SDK's `--sysroot` and `-specs="$UNWIND_SPECS"` options; the checked helpers were
built against the stock R1/beta6 SDK.

The airOS packages were extracted into a separate test directory. Only the
browser's absolute library search path and the private Mesa vendor JSON path
were relocated. The actual loaded engine/helper paths were checked and hashed;
the installed browser and the earlier frozen Test262 bundle were unchanged.

Initial harness failures are retained separately: hashing the non-file
`commpage` image, checking connection closure only by writing, and expecting
a session cookie to survive restart. The harness now reads peer EOF, uses an
expiring cookie, and performs TLS handshakes in connection threads so an idle
speculative connection cannot block server shutdown. An early airOS attempt
loaded the installed engine through the package's absolute RPATH and failed
before opening the fixture; this was corrected in the isolated copy.

Raw local evidence is retained under
`.vm/issue120-stock-20261022/network-inputs/` and
`.vm/issue120/network-airos-packages/`. The five-cycle stock pass predates the
server-handshake and failure-diagnostic improvements; the hashes in each
report identify exactly which runner was used.
