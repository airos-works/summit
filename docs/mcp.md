# Built-in MCP server: debugging Summit from the outside

Added 17 October 2026. Summit can run its own [Model Context Protocol](https://modelcontextprotocol.io)
server, so a client such as Claude Code can look inside the running browser
on the machine that shows a problem. It reaches what the system-wide
`airos_mcp` cannot: the browser's tabs and their pages (through the Web
Inspector protocol), the engine's processes from the inside (JavaScript heap,
caches, media players and their decoders, compositors and the GL renderer),
the frames delivered to each view, a log of everything Summit and its engine
processes write to stderr, stack traces and a sampling profiler for any of its
processes. It is off unless it is turned on.

## Turning it on

**Preferences › Developer › Let debugging tools connect to Summit (MCP
server).** The first time, Summit makes a random access token (64 hex
digits) and keeps it in the profile (`profile.json`, `"mcp"`). The tab shows:

- **Port** (default 7790; Enter applies a change) and **Only from this
  computer** (listen on 127.0.0.1 instead of every interface).
- **Status**: the addresses it listens on, the number of tools, requests
  answered and the last client. A port in use is reported here.
- **Copy Client Configuration**: the server's address and token, as an
  `mcpServers` entry and as a `claude mcp add` command.
- **New Access Token…**: clients with the old one can no longer connect.

The environment overrides the profile, for test copies started by a script:
`SUMMIT_MCP=1` (or `0`), `SUMMIT_MCP_PORT`, `SUMMIT_MCP_TOKEN`,
`SUMMIT_MCP_LOCAL=1`. Preferences then say "(set by SUMMIT_MCP in the
environment)".

Connecting from the build host:

```sh
claude mcp add --transport http summit http://192.168.1.103:7790/mcp \
    --header "Authorization: Bearer <token>"
```

Two Summits with the server on need different ports; the second reports the
port as in use.

## Security

Whoever has the token can read every page and act in it (run JavaScript,
type, navigate, kill web processes). The transport is plain HTTP, as for
`airos_mcp`; use **Only from this computer** on networks that are not your
own. Requests without the bearer token are refused (constant-time compare),
and requests that carry a browser `Origin` header are refused too, so a web
page cannot reach the server through DNS rebinding. The token is not echoed
back by `summit_info` or `environment`.

## Tools

Tabs are named `WINDOW.TAB` (`1.2`), as `tabs` lists them; without `tab`
a tool works on the front window's selected tab.

| Group | Tools |
|---|---|
| Browser | `summit_info` (versions and packages, processes and roles, display and accelerant, environment, log state), `tabs` (every window and tab: address, load state and errors, web process, frames in the last second), `open_tab`, `navigate`, `tab_control` (reload, stop, back, forward, select, close, zoom, devtools), `preferences` (`show` opens the window on a tab), `updates` (the build that runs, the last update check, the update window; `do` checks or presses its buttons), `bookmarks` (the tree with ids; `import` or `export` a path as the File menu does), `downloads` (the downloads panel's list with ids and states; `do` starts, pauses, resumes, cancels, retries or removes one as the panel's buttons do, or clears the list), `extensions`, `extension_control` |
| Views and input | `screenshot` (screen, window or page as on screen), `page_frame` (the last frame the engine presented to the view, device pixels), `page_render` (the page as WebCore paints it), `frame_stats` (frames per second and gaps; `scroll=true` measures a wheel burst), `click`, `type_text`, `press_key`, `scroll` |
| Pages (Web Inspector) | `evaluate`, `console`, `network` (with `request=SERIAL` for headers and body), `dom`, `page_metrics` (navigation timing, FCP/LCP, resources, DOM size, animation frame rate), `page_media`, `timeline` (main-thread time by kind, rendering frames, longest tasks), `resource_usage` (CPU and memory samples, including worker CPU), `layers` (compositing layers, memory, paint counts, reasons), `heap` (objects and bytes per class; gc), `inspector` (any protocol command), `page_detach` |
| Engine | `engine_report` (a web or network process's pages, memory, media, graphics/compositor, network topics; or this browser's view of its processes), `media` (players, decoders, frames presented and dropped), `graphics` (view presentation, compositor and GL renderer, frames delivered, screen mode), `memory_action` (gc, release-memory) |
| Processes | `processes` (CPU and memory of every Summit process), `threads` (states, waits, CPU per thread), `memory` (areas by kind), `cpu` (the whole machine), `stacks`, `profile`, `logs`, `syslog`, `crash_reports`, `environment`, `set_environment`, `restart_process` |

Some ways to use them:

- **A tab hangs.** `tabs` (its web process), `engine_report` with that tab
  (is `mainThread.responsive` false?), `threads` (is the main thread running
  or waiting, and on what?), `stacks` with `filter` on the main thread's name.
  `restart_process` ends the process and loads its tabs again.
- **Scrolling or animation stutters.** `frame_stats` with `scroll=true`,
  `timeline` with `scroll=true` (main-thread work), `graphics` (compositor
  render times, direct present), `profile` of the web process while it
  happens, `layers` for oversized or repainting layers.
- **Video plays badly.** `media` (which decoder: a named add-on such as
  amduvd, cedar or rpi_mmal, or libavcodec; frames presented and dropped),
  `page_media` (what the page sees), `frame_stats`, then
  `set_environment SUMMIT_MEDIA_PLAYBACK_TRACE=1` or `SUMMIT_MSE_TRACE=1`,
  `restart_process` and `logs`.
- **Memory grows.** `processes`, `memory` with `all=true`, `engine_report`
  topic memory, `resource_usage` with `cpu=false`, `heap` (and
  `heap action=gc`, then again), `memory_action`.
- **CPU is busy.** `cpu` (which team), `threads` (which thread), `profile`
  (which functions, self and inclusive).
- **An extension misbehaves.** `extensions` (loaded, errors, notices), `logs`
  filtered by its name (its pages' console), `extension_control` to restart
  it, `open_tab` with its `baseURL` pages to inspect them with the page tools.

## CPU and memory recordings

`resource_usage` records the Inspector's `CPUProfiler` and `Memory` events.
It accepts `tab`, `seconds` (1–60, default 3), `cpu` and `memory` (both true
by default). The result contains timestamped `cpu` and `memory` arrays,
the tab ID and whether its process changed during the recording. A failed
stop is reported in `stopErrors`; an unsupported engine returns its protocol
error. Concurrent resource recordings on the same tab are rejected.

The native collector samples about every 500 ms while observed. CPU is a
percentage of one core, so a process using multiple cores can exceed 100%.
Thread samples identify the main thread, WebKit threads and workers, including
workers' Inspector target IDs. Process CPU includes work by threads that
exited between samples and excludes the resource collector's CPU. CPU deltas
reset when collection resumes; baseline samples report zero. Recordings in
pages sharing a process also share its collector.

These measurements describe the inspected **WebProcess**, including other
pages sharing it. JavaScript memory is the page VM's heap and owned allocations;
JIT memory is process-wide committed executable memory. Images covers the
decoded image cache, and Layers covers tracked coordinated tile buffers.
The Page allocator category is unavailable and remains zero. Other is the
nonnegative remainder of resident area estimates after attributed allocations.
The OS reports cache-backed area sizes; shared mappings can be counted more
than once, and allocation counts are not private physical-memory totals.

Memory counters are collected on the page's main thread. Set `memory=false`
to keep CPU collection independent of a busy main thread; delivery of the
Inspector events still waits for that thread to run. This tool does not
provide JavaScript stack samples or add a Timelines pane to the native
Developer Tools window.

Native acceptance and exact build evidence: [22 October 2026 resource-usage
report](resource-usage-2026-10-22.md).

## How it works

- **Server.** `src/core/Mcp.{h,cpp}` is the protocol (JSON-RPC 2.0, MCP
  2025-06-18 back to 2024-11-05, tools only) and `src/core/McpHttp.{h,cpp}`
  the streamable HTTP transport (POST `/mcp`, `GET /health` without a token),
  on plain sockets and threads; `tests/McpTests.cpp` covers both on the host.
  `src/ui/McpService.{h,cpp}` starts and stops the server for the settings.
  Tools run on the connection's thread.
- **Nothing waits for a hung thread.** What belongs to a window or to the
  application is asked of its looper with a timeout (`McpAsk`); a looper that
  does not answer is reported as busy or hung, which is itself a finding.
  The application thread runs WebKit's main loop, so this matters. What the
  operating system knows (teams, threads, semaphores, areas) needs no looper.
- **Pages.** One `BWebKitInspectorSession` per tab, made on first use and kept
  (`src/ui/McpPageTools.cpp`). `src/core/McpInspector.{h,cpp}` routes the
  protocol: console and network go to the developer tools' model
  (`devtools::Session`), the tools' own commands carry identifiers from 10⁹ up
  and their answers are kept until taken; a page that moves to another
  process gets the commands that were waiting. The engine announces a page's
  target only to its first connection, so a session made while the Developer
  Tools are open is told it (`page-` and the tab's page identifier).
  Registration also queues an initial drain: the engine can announce messages
  before the session enters the lookup map, and coalesces further announcements
  until those messages are taken. This avoids a lost first notification leaving
  every page command waiting indefinitely.
- **Engine diagnostics** (engine patch): every web and network process starts
  a thread that waits on a port named "WebKit diagnostics"
  (`Shared/haiku/ProcessDiagnosticsHaiku.{h,cpp}`). Topics that only the main
  thread can read are collected there within a time limit; the answer says
  when the main thread did not get to it. Providers: `WebProcess/haiku/WebProcessDiagnosticsHaiku.cpp`
  (pages, memory, media, graphics) and the network process's curl workers
  (`CurlRequestScheduler::workerStatesHaiku`). The compositor reports its GL
  strings and every frame's time (`ThreadedCompositor.cpp`); the Haiku media
  players describe their decoder and frames in `engineDescription()`. The
  public API is `WebKitDiagnostics.h` (`BWebKitDiagnostics`,
  `BWebKitProcessDiagnostics`) and `BWebKitView::Diagnostics()` and
  `CopyPresentedFrame()`; views' state notifications carry `processID` and
  `pageIdentifier`.
- **Stacks and profiles** come from Summit itself started as
  `Summit --mcp-debug-helper stacks|profile …` (`src/ui/McpDebugHelper.cpp`):
  a short-lived debugger that attaches to the team, stops each thread for an
  instant (stacks) or runs the kernel's sampling profiler (profile), and
  detaches. A separate process can debug the browser's own team too. Stacks
  give the frame-pointer chain (WebKit keeps frame pointers) and the code
  addresses found on the stack; names come from libdebug and the libraries'
  exported symbols. Haiku's `Debugger --save-report` was tried first: on a
  running team it lists no stacks, and the web process it reported on exited.
- **Log.** When the server starts, Summit's stderr becomes a pipe that a
  thread copies to where it went before and into memory (8 MiB, 40000
  lines). Engine processes started afterwards inherit it, so their traces and
  warnings are kept; the server also turns on
  `SUMMIT_TRACE_EXTENSION_CONSOLE` (unless set) so extension pages' console
  messages are there.
- **Frames per view.** A message filter on every tab's view keeps when the
  last 1024 frames arrived (`src/ui/FrameHistory.h`).

## Testing

Host: `tests/McpTests.cpp` (protocol, HTTP parsing and responses, a real
socket round trip, the inspector channel, timeline and heap summaries).

On 21 October 2026, the registration fix was checked against the native engine
on air/OS: Excalidraw page evaluation timed out before the fix, then succeeded
afterward through five detach/reattach cycles, including during navigation.

The same day's scroll-burst fix preserves `x`, `y`, `dx` and `activate` when
`count` is supplied. Previously bursts ignored these arguments, so a targeted
horizontal notch did nothing and a vertical notch could scroll the page behind
the intended container. `tools/test-scroll-snap.py` checks both snap axes,
direction reversal, targeted bursts and invalid coordinates/deltas through the
native browser. It passed at 100% and 200% against engine `31a91fe`. Serve
`tests/fixtures/scroll-snap.html`, set `SUMMIT_TEST_MCP_TOKEN` for an isolated test
browser, and pass `--mcp-url`, `--tab` and `--fixture-url` to the script.

On the laptop (17 October, test copy `/boot/home/claude-test/swmcp`, profile
`pmcp`): every tool was called against Wikipedia, a VP9 video and
example.com. The preference path was checked without environment overrides:
off by default; `prch mcp_enabled=true` (what the checkbox sends) started it
on the chosen port with a new token saved in the profile; a wrong token got
401; **Only from this computer** refused the network and answered 127.0.0.1;
turning it off stopped it. A page made to spin for 8 s showed
`mainThread.responsive: false`, its main thread running at one core, and its
stack in the JavaScript interpreter under `Runtime.evaluate`.

## Limits

- The ScriptProfiler gives no JavaScript stack samples
  (`ENABLE_SAMPLING_PROFILER` remains off). `resource_usage` records CPU and
  memory counters; use `profile` for the existing native sampling tool.
- JIT code has no symbols: such frames show as addresses, and frames in
  functions that are not exported take the nearest exported name.
- A blanked desktop captures black (`screenshot` says when `screen_blanker`
  runs); frames put straight into the screen (direct present) are not kept for
  `page_frame`.
- Extension background pages are not views, so the page tools cannot attach to
  them; their console is in `logs`.
- Most `SUMMIT_*` switches are read once when a process starts:
  `set_environment` applies to processes started afterwards
  (`restart_process`).
