# Tab crash reports

Added 11 November 2026, on top of the crashed-tab page (#71).

## What the user sees

When a tab's web process crashes, the crash page says so as before, and now
also asks: "Summit saved a report of this crash. Send it to Summit's
developers?"

- **Send Report…** opens GitHub's new-issue form for airos-works/summit in a
  new tab, with the title, the crash, the versions, the site's origin and the
  crashed thread's stack filled in. Nothing is filed until the user submits
  that form, signed in to GitHub. The body says where the full report is and
  asks for it to be dragged in as an attachment.
- **Don't Send** forgets the question. The report stays in the folder.
- **Show Report** opens the full report in the text editor.
- **Reload** works as before. Reloading also passes over the question.

A tab that crashed in the background used to reload quietly when it was
selected. With a report waiting, it shows the crash page and asks instead.
Every tab of the crashed process shows the question, and an answer in one of
them clears it in all of them. A crash of a process that showed no tab (the
spare, prewarmed process) is saved but not asked about.

Reports are kept in `~/config/var/log/Summit/crash-DATE-TIME-WebProcess-TEAM.txt`
(the newest 30). `SUMMIT_CRASH_REPORTS=0` turns this off, and crashes go to
debug_server's alert as before.

## What a report holds

- The crash: signal and fault address, or the `debugger()` text (WebKit's
  `CRASH()` passes its function), and the crashed thread's name.
- The packages of Summit and its engine (the `SYS:PACKAGE` attribute), the
  WebKit and port versions, and the Haiku revision, architecture, CPU count
  and memory.
- The crashed thread's stack, walked from the registers at the crash, with
  the code addresses found on its stack. Every other thread's stack follows,
  then the loaded images with their text ranges.
- No page address. The issue body gets the site's origin only, because the
  issue is public.

Every frame reads `#N image+offset symbol`. Packages are stripped
(`--strip-unneeded`), so the symbol is only the nearest exported one ("unexported
code after …"). Return addresses are named by `address - 1`, so the frame
of a call that never returns (`abort()`) names its caller, not the next
function.

The image list gives each image's GNU build ID where it has one (the engine,
Summit and libWebKitLegacy are linked with `--build-id` since 11 November
2026).

## Naming the frames

air/OS CI keeps the symbol tables of every build's binaries before stripping
them, and their line tables: the engine, Summit and libWebKitLegacy are
built with `-g1` (files, lines and inlined calls, no variables; the init
cache's `SUMMIT_DEBUG_LINES`, which CI turns on). They are stored by build ID,
and by the package version for the engine's third-party libraries, which have
none (airos-ci `apps/store-symbols.py`, served at
`http://192.168.1.200/symbols/`). Symbol tables are kept; line tables are
removed 180 days after the last build that has them.

    tools/symbolize-crash-report.py crash-20261111-101500-WebProcess-1234.txt

This prints the report with each frame named by the function that contains
it, local functions included. Where the build has line tables it adds the
file and line and the calls inlined there, innermost first (read with
`llvm-symbolizer`, which must be installed):

    #0 libWebKit.so.1+0x2ab63a0 WTF::CanMakeCheckedPtrBase<…>::decrementCheckedPtrCount() const
         at WTF/Headers/wtf/CheckedRef.h:388
         inlined into WTF::CheckedPtr<WebCore::RenderBox, …>::~CheckedPtr()
         at WTF/Headers/wtf/CheckedPtr.h:97
         inlined into WebCore::RenderBox::nextSiblingBox() const
         at WebCore/PrivateHeaders/WebCore/RenderBox.h:812
         inlined into WebCore::RenderBlockFlow::layoutBlockChildren(…)+0x200
         at Source/WebCore/rendering/RenderBlockFlow.cpp:1229

`--store DIR` reads a copy of the store instead, and `--sdk` picks the target
when a version was built for more than one and the report has no build ID to
tell them apart. Objects are cached decompressed in `~/.cache/summit-symbols`
(about 1.2 GB per engine build with line tables). Reports from builds before
the store existed (before 11 November 2026) cannot be named this way, and
line tables start with the first engine built with them.

## How it works

Engine side (`WebProcess/EntryPoint/haiku/WebProcessMain.cpp`,
`WebKitDiagnostics.h`):

- Summit creates a port and sets `WEBKIT_CRASH_REPORT_PORT` before any web
  process starts. A web process that finds it installs handlers for SIGSEGV,
  SIGBUS, SIGILL, SIGFPE and SIGABRT before anything else runs. For the main
  thread's stack overflowing, they run on an alternate stack. Haiku's kernel
  delivers a fault to a handler when one is installed, instead of to
  debug_server.
- The executable defines `debugger()`. The runtime loader looks symbols up in
  the executable first, so `CRASH()`/`RELEASE_ASSERT` (`debugger()` then
  `abort()`) and every other `debugger()` call of the process come here. The
  linker exports it (`--export-dynamic-symbol=debugger`, and libWebKit refers
  to it anyway).
- JavaScriptCore installs its SIGSEGV/SIGBUS handler later, for WebAssembly.
  It passes faults it does not own to the handler it replaced, which is this
  one. A page with 1,000 out-of-bounds WebAssembly loads still traps
  normally.
- The crashing thread writes a `BWebKitCrashReport` (team, thread, signal,
  fault address, instruction/frame/stack pointers, message) to the port and
  waits. A second crashing thread waits forever. A crash inside the handler
  ends the team. With no port, or when the write fails, the handler puts the
  previous action back and the crash goes to debug_server as before.

Summit side (`src/ui/CrashReporter.cpp`):

- A listener thread takes each report and runs `Summit --mcp-debug-helper
  crash TEAM THREAD IP FP SP DEPTH OUTPUT` (McpDebugHelper.cpp) with
  `load_image()`. The helper attaches, stops the threads, walks the crashed
  one from the given registers and the others from their own, and writes JSON.
  Summit then `kill_team()`s the process. The engine sees an ordinary crash,
  and the tab gets its crash page.
- The report is written to the folder, and `kCrashReportReady` goes to the
  application, which passes it to every window. Tabs whose `processID` is
  that team keep it, and their crash page asks.
- `CrashIssueURL()` builds the issues/new address. GitHub refuses one over
  about 7 KB for a signed-out visitor (who is redirected to sign in with the
  address inside the redirect), so it stays under 6,000 characters: as many
  frames as fit, then shorter lines.

## Not covered

- `int3`/`brk` breakpoint traps (`WTFBreakpointTrap`, JIT code's abort
  paths) are breakpoint debug events, not signals, and still go to
  debug_server.
- The network process and the UI process have no handler: neither is a tab
  crash.
- A thread that overflows its stack, other than the main thread, has no
  alternate stack, and the kernel ends the team.
- Line tables do not cover the engine's third-party libraries (ICU, curl and
  the others from summit/build-deps.sh): those frames name functions only.

## Tests (X399, 11 November 2026, CI engine tree 70a3f16 + this change)

A preloaded test library made each web process crash 5 s after starting, with
a real null write, `abort()` or `debugger()` (what `CRASH()` does):

- All three were reported with the right crash site (`SIGSEGV`/`abort()`/
  `debugger()` in `CrashTestLeaf`). There was no debug_server alert, and the
  process ended.
- The crash page asked, Send opened the GitHub form with the expected body,
  Don't Send cleared the question, and Show Report opened StyledEdit.
- A tab that crashed in the background asked when selected.
- `kill -SEGV` of a web process was reported. A sent signal lands on any
  thread, so the stack is that thread's.
- With `SUMMIT_CRASH_REPORTS=0`, crashes went to debug_server as before.
- WebAssembly out-of-bounds traps were unaffected.
