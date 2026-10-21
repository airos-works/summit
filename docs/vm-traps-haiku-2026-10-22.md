# Worker termination on older Haiku kernels — 22 October 2026

Stopping a worker running optimized JavaScript could crash the stock R1/beta6
WebProcess with a general-protection fault. The CPU/memory acceptance fixture
reproduced this twice on the published `9d1ebd0` packages, after measuring two
busy workers and calling `Worker.terminate()`. Both worker threads appear in
the saved native debugger report, executing generated code from timer callbacks.

JSC interrupts optimized code by installing halt instructions at registered VM
trap locations. On older x86 Haiku kernels, that instruction raises
`SIGILL`/`ILL_PRVOPC`. Haiku commit `1676c2d77c` changed general-protection faults
to `SIGBUS`/`BUS_OBJERR` on 29 September. JSC's access-fault handler covers
`SIGSEGV` and `SIGBUS`, so the newer airOS kernel handled the trap while the
stock kernel sent an unhandled signal.

The Haiku x86_64 port now registers and activates the same VM trap handler for
illegal-instruction signals as well. It retains the existing checks for a JIT
address and an exact registered VM trap location. Unrelated illegal instructions
return `NotHandled`; this does not suppress arbitrary faults or disable JIT.
Other platforms keep their existing signal registration.

## Native verification

The unchanged worker fixture and corrected retained-memory runner from
`70b6789` run against isolated candidates on both native targets:

| Target | Checks | Idle → two-worker CPU | Retained JavaScript growth |
| --- | ---: | ---: | ---: |
| Stock R1/beta6, hrev59866+79 | 212 | 0.140% → 179.554% | 33,548,681 bytes |
| airOS, hrev60207+1733 | 208 | 0.184% → 179.609% | 33,548,682 bytes |

These runs use normal trap settings without supplying the diagnostic polling
override. Worker termination, CPU reduction, allocation/GC accounting,
recording restart and overlap, observer cleanup, and normal browser shutdown
all pass. Each browser exits zero and its owned process group drains without
forced cleanup. Native crash intervals are complete and contain no new events.
Inputs and recorded runtime files remain unchanged: 101 on stock and 102 on
airOS. The loaded JSC digest matches each candidate manifest.

Each candidate copies its target's complete published `9d1ebd0` bundle, with
the browser's RPATH already relocated for that private directory. Only JSC is
replaced with the library built by the matching SDK. All 230 stock package
files and 231 airOS files match the candidate manifests. No installed browser,
kernel or system library is changed. These are native candidate results, not
acceptance of a newly published full engine package.

Builds use unprofiled Release settings and upstream
`fb054d09146b113aeeabee5f67f4c8b6d9379809`. All 1,484 patched source files match
both owned source trees. The patch SHA-256 is
`2ceaf3215f0f75e7406ddb9bcddcd244324651736d944b0141eb4f7f5a4ae12b`.
The deployed JSC library digests are:

- Stock: `9ea93cd6a9df15cf0debd475a83e207c100ecbe8652081856955e901439a0bb2`.
- airOS: `3963885ea62e5d793b46868e0cb51e5c8231fea30a9646276855a4d055109bf0`.

The [machine-readable evidence](vm-traps-haiku-2026-10-22.json) includes both
native reports, candidate manifests, the shared source audit and the kernel
change establishing the signal difference. The original failures, debugger
report and polling diagnostic remain in
[resource-usage evidence](resource-usage-2026-10-22.json). The polling diagnostic
passed 212 checks but was not accepted as a fix. An initial cross-compile
rejected lvalue lambda conversion to `WTF::Function`; the handler registration
now moves separate closure copies, and both target builds complete.

The full published `8224d7a` packages now pass **212 stock checks and 210 airOS
checks**, including worker termination, with normal trap settings. Both runs
exit cleanly, retain input/runtime hashes and have no new crash events. All
four loaded engine binaries match their published packages; no candidate
library is substituted. See the [release acceptance record](resource-usage-2026-10-22.md#published-release-after-the-worker-trap-fix)
and [complete release evidence](resource-usage-release-2026-10-22.json).
ARM behavior and unrelated illegal-instruction cases are not established by
these x86_64 worker tests.
