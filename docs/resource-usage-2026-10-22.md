# Native CPU and memory recordings, 22 October 2026

The unprofiled x86_64 airOS candidate at `eefce05` passes **218 native checks**
for the Inspector CPUProfiler and Memory domains and the new MCP
`resource_usage` tool. The published `9d1ebd0` airOS release passes 208 checks.
Stock-Haiku release acceptance failed when stopping a busy JavaScript worker.
The [signal compatibility fix](vm-traps-haiku-2026-10-22.md) passes native
candidate tests on both targets; a new full release still needs acceptance. Both published
`56f49a8` baselines return “domain not found” for these domains, confirming
the original #120 limitation.

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
clean shutdown and complete crash-log coverage. Their initial image snapshots
cover 73 stock files and 97 airOS files; later translator loads bring the totals
to 101 and 106. Later-loaded files are hashed when first observed and checked
at shutdown. The candidate has all 106 runtime files in its initial snapshot.
The airOS candidate is not substituted into the stock installation.

## Published packages

Release CI [39665615342](https://github.com/jmgasper/summit/actions/runs/39665615342)
successfully published the full x86_64 browser and engine packages for
`9d1ebd0` on both targets. Acceptance extracts each target's complete bundle
into an owned directory. Only the browser RPATH is relocated; its Deskbar
symlink resolves to that same binary. An external EGL vendor file selects
the packaged Mesa library. All other package files retain their published
bytes. No engine library is substituted and no preload is used.

The airOS release passes **208 checks**: median CPU rises from 0.175% idle to
180.002% with two workers, and the retained 32 MiB array grows the JavaScript
category by 33,548,681 bytes. Recording, detach, GC and shutdown checks pass;
the browser exits zero with no owned processes left, no fresh crash events,
and unchanged inputs and 102 recorded runtime files.

The first airOS release attempt failed its allocation comparison: three of
six samples preceded an automatic GC and did not yet count the typed array's
backing store; the remaining samples included it. JSC accounts for oversize
typed-array backing stores when visiting them during GC. The runner now
collects before each allocation comparison to compare settled counters. The
failed attempt is retained with its forced cleanup.

Stock R1/beta6 reproduces a general-protection fault when terminating an
optimized JavaScript worker after the CPU measurement. The initial run reaches
about 180.10% worker CPU before failing. A diagnostic repeat reaches 179.90%
and saves a native report showing both worker threads faulting in generated
code, called by `ScheduledAction` through their timer loops. Both runs fail
their native crash checks and require cleanup; neither is release acceptance.
The repeat deliberately holds its failed process for 45 seconds so the native
debugger can save a report. The complete reports, package manifests and saved
debugger report are retained in the machine-readable evidence.

A separate stock diagnostic with `JSC_usePollingTraps=1` passes 212 checks,
exits normally, keeps its hashes and has no fresh crash events. This changes
the trap strategy and is not normal-release acceptance. The kernel source
shows the compatibility difference: stock reports a general-protection fault
as `SIGILL`/`ILL_PRVOPC`; Haiku commit `1676c2d77c` changed it to
`SIGBUS`/`BUS_OBJERR` on 29 September. JSC's signal-based VM trap handler
currently registers for access faults (`SIGSEGV` and `SIGBUS`), so the older
signal needs separate handling for its known installed trap locations.
The [implemented compatibility change and candidate acceptance](vm-traps-haiku-2026-10-22.md)
now cover that older signal without selecting polling traps.

## Behavior and limits

The collector samples about every 500 ms while observed. Native team counters
include work from threads that exited between samples; individual thread
counters identify main, WebKit and worker threads. CPU baselines reset when
collection resumes. Memory counters are read on the owning main thread with
the VM lock, and memory remainder arithmetic saturates instead of wrapping.

These are WebProcess measurements. The JavaScript category covers the page VM,
not separate worker heaps, and some backing-store counters reflect the most
recent GC rather than every allocation immediately; JIT is process-wide. Images and Layers use existing
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
remain unfinished. Stock release and ARM runtime acceptance remain open.
