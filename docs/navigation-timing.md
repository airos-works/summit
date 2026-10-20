# From Enter to the page (#81)

jmgasper/summit#81 asked why the time from Enter in the address field to the
page starting to load seemed long on the Framework 13, and for a clear path
from the key to the first frame of the page. The measurements are in
[performance.md](performance.md) ("20 October 2026: from Enter to the page");
this page says what changed and how to trace a navigation.

## What the user sees

- A navigation to another site no longer waits for a web process to start.
  Every site runs in a web process of its own, and when none was ready the
  new one took 0.15-0.3 s to start on the laptop before the page could even
  be asked for. That happened whenever an address was entered within a few
  seconds of the last one, or while a page was still loading: the spare
  process used to be started only a second after a page had finished
  loading, and many pages take seconds to finish. Now a spare is started
  while the user types in the address field, and a second after each page
  first paints. In the measured case (a search entered while the last page
  was still loading) the request now leaves 41-81 ms after Enter instead of
  186-312 ms, and the new page's first frame comes 670-700 ms after Enter
  instead of 730-1030 ms.
- Nothing else changes: the address field, the tab's loading dot and the
  progress line react within 1-4 ms of the key, as before.

## How it works

- **Typing.** `BrowserWindow::AddressModified()` calls the engine's new
  `BWebKitView::PrewarmProcess()` when the user types in the field (at most
  once a second). The engine runs `WebProcessPool::prewarmProcess()` on its
  main loop, which does nothing when a spare process exists or the process
  count limit is near; under memory pressure nothing is started either.
  Private windows get the same: a spare process has no data store until a
  page takes it.
- **First paint.** The engine's navigation client
  (`UIProcess/haiku/WebView.cpp`, `renderingProgressDidChange`) starts a spare
  a second after a page's first visually non-empty layout, as it already did
  a second after a load finished. The second's wait keeps the spare's start
  (0.6 s of a core on a Raspberry Pi 4) away from the first frame of the page
  that just started. `SUMMIT_PREWARM_PROCESS=0` still turns all of it off.
- WebKit swaps processes on cross-site navigations in this port; the old
  comment in `WebView.cpp` that said it did not was wrong and is corrected.

## Tracing a navigation

`SUMMIT_NAV_TRACE=1` prints `Summit nav` lines on standard error, from the
browser and from the engine, stamped with `system_time()` in seconds (the
clock of `SUMMIT_STARTUP_TRACE` and `SUMMIT_NET_TRACE`, so one log orders
every process's lines):

- browser (`src/ui/NavigationTrace.h`): the Enter key (input_server's time
  stamp, with the wall clock to line it up with the page's
  `performance.timeOrigin`), the window handling it, the call into the
  engine, and every change of the tab's loading state, progress, outcome and
  address, as milliseconds since the key;
- engine (`Shared/haiku/StartupTraceHaiku.h`): the load reaching the
  engine's main loop, the navigation policy asked by the web process and
  decided, the provisional load and the process it runs in (a swap names the
  new process), the response policy, the commit, the first layout, first
  visually non-empty layout and first meaningful paint, the first frame after
  the commit (presented directly or received by the view) and the end of the
  load.

With `SUMMIT_NET_TRACE=1` the network process's lines carry `at=` times too:
the disk cache's answer, each request's start, the preconnect's connection,
and the response's first byte (`first-byte-at=`) and end. Add
`SUMMIT_STARTUP_TRACE=1` to see web processes start. For the first frame in
detail, `SUMMIT_COMPOSITOR_TIMING_TRACE=1`.

`tools/bench/navigation-trace.py LOG` turns such a log into one row per
navigation, with the milliseconds from the key to each stage. The #81
measurements typed addresses with `summitctl --team ID typeaddress TEXT
select` and `keyaddress enter` (the browser needs
`SUMMIT_ENABLE_INPUT_SYNTHESIS=1`).
