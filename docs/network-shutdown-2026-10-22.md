# Native network shutdown recheck, 22 October 2026

**Acceptance is incomplete; #120 remains open.** Five stock Haiku cycles
passed, but a subsequent run caught a compositor-thread abort during shutdown.
The airOS workstation completed one cycle, then stalled before the next
shutdown. Neither failed run is counted as a pass.

The test uses published packages with the current engine pin `fb054d0`:
stock R1/beta6 `cd9de3e`, and airOS `008ccfc`. Both contain the same engine
patch (`df10f964784bb1ada936435c86d1170e0a085600bb92d872ee673d00e8a73fc4`).
These are release CI builds, without PGO profile use. Package, executable,
loaded-library and fixture hashes, per-cycle results, and the native crash
stack are in [the machine-readable report](network-shutdown-2026-10-22.json).

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
NetworkProcess exit reports. Its cause has not been established. Earlier
airOS attempts also stalled during worker startup; the captured network
diagnostics do not yet distinguish a browser defect from a fixture/server
interaction.

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
