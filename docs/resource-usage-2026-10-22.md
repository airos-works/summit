# Native CPU and memory recordings, 22 October 2026

The unprofiled x86_64 airOS candidate at `eefce05` passes **218 native checks**
for the Inspector CPUProfiler and Memory domains and the new MCP
`resource_usage` tool. Published-package acceptance and a stock-Haiku candidate
run are pending. Both published `56f49a8` baselines return “domain not found”
for these domains, confirming the original #120 limitation.

The [machine-readable evidence](resource-usage-2026-10-22.json) contains the
baseline and candidate results, raw samples, loaded-image hashes, crash-log
intervals, build manifests and the audit of all 1,483 port source files.
The native browser's 301 source inputs match the checkout. All four engine
targets build, and Ninja reports no work after the final collector change.
Host MCP and engine-option checks pass. The configuration enables resource
usage and JIT, keeps JavaScript stack sampling disabled, and has no PGO flags.

## Native acceptance

The workstation runs airOS `hrev60207+1733`, x86_64. The browser uses a new
profile and a loopback fixture, with a private MCP token and no preload.
The complete published dependency bundle is copied to an isolated directory;
the candidate replaces the browser, both engine libraries and both helpers.
The installed browser and its profile are unchanged.

- Median process CPU rises from **0.08% idle to 179.72%** with two busy workers.
  Both workers have Inspector target IDs. Thread percentages remain within
  0–100%; the process can exceed 100% because 100% represents one core.
- Retaining and touching a 32 MiB array increases the JavaScript category by
  **33,522,207 bytes**. After release and GC, it falls from 35,470,761 to
  1,921,216 bytes. Committed JIT memory is nonzero.
- CPU-only, memory-only and combined recordings return ordered samples.
  Restarted recordings contain no old events. CPU falls after workers stop.
- Detach and reattach work. Two simultaneous first requests permit exactly one
  recording; rejecting the other does not interrupt it.
- Detaching raw CPU and Memory observers, including Memory tracking started
  without `Memory.enable`, leaves one collector thread waiting with zero CPU.
- The browser exits zero, its owned process group drains, and the native crash
  interval has no new events. Inputs and all 106 recorded runtime files remain
  unchanged. The five loaded candidate binaries match the deployment manifest.

The first attempt failed because the private package clone contained regular
`libWebKit.so.1` and `libJavaScriptCore.so.18` files. Adding new versioned files
left those old runtime files active. The retained failed report records the
baseline library hashes and forced cleanup. Correcting the runtime links made
the unchanged candidate pass; this was a deployment error, not an accepted run.

The stock R1/beta6 and airOS baseline runs each pass their three checks with
clean shutdown and complete crash-log coverage. They hash loaded images before
the workload and again after it (101 stock files, 106 airOS files). The airOS
candidate is not substituted into the stock installation.

## Behavior and limits

The collector samples about every 500 ms while observed. Native team counters
include work from threads that exited between samples; individual thread
counters identify main, WebKit and worker threads. CPU baselines reset when
collection resumes. Memory counters are read on the owning main thread with
the VM lock, and memory remainder arithmetic saturates instead of wrapping.

These are WebProcess measurements. The JavaScript category covers the page VM,
not separate worker heaps; JIT is process-wide. Images and Layers use existing
cache/tile counters. The Page allocator category remains zero. Resident area
and allocation estimates are not exact private physical memory, and shared
mappings can be counted more than once. See [MCP usage and limits](mcp.md#cpu-and-memory-recordings).

The fixture is `tests/fixtures/resource-usage.html`; the native runner is
`tools/test-modern-resource-usage.py`. Supply `--browser`, `--close-helper`,
`--images-helper` and a new `--output` directory. `--expect-unavailable` records
the published baseline's missing domains. Helpers are the existing native
`ModernCloseTests` and `NativeLoadedImages` executables. Isolated bundles need
the same helper/library/Mesa environment as their normal launch.

This implements the CPU/Memory backend portion of #120. JIT symbolization,
extension background-page attachment, and the native Timelines UI in #108
remain unfinished. It does not establish ARM runtime or release-package acceptance.
