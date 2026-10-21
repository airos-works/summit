# Native locale startup race, 22 October 2026

Summit's Haiku engine now initializes libroot's locale backend during main-thread
startup, before starting its workers. This works around a native initialization
race that aborted the WebProcess compositor while libepoxy parsed a valid
`GL_VERSION` string. It does not change the selected locale or time zone.
The underlying OS defect remains tracked in [#119](https://github.com/jmgasper/summit/issues/119).

## Diagnosis

The [network shutdown workload](network-shutdown-2026-10-22.md) caught the abort
before the browser's quit request. A diagnostic `sscanf` wrapper on stock
R1/beta6 recorded the unchanged string `3.1 Mesa 25.3.6 (git-d7d67ef2c5)` and a
return value of zero, with errno zero. Repeating the scan, copying the input,
using a literal input, and changing `%i.%i` to `%d.%d` all still returned zero.
`%c.%c` read `3` and `1` correctly, and `strtol()` returned 3 and consumed one
character. The wrapper returned the original failure; it did not suppress the
subsequent abort.

The decisive difference was character classification: the thread's cached POSIX
ctype table recognized `3`, while `isdigit_l('3', __current_locale_t())` returned
zero. The global table pointer had changed to heap storage. By the time the
diagnostic printed that table's contents, its digit entries were valid again.
The libc entry points all resolved to the native `libroot.so`.

Exact stock libroot SHA-256:
`35be61950d0749f461ebbd12590714d05bf4674570df3cd247b8958dfd18d0d4`.
This matches the R1/beta6 SDK at Haiku commit
`874f0cea9d07c6091817acf8e2cef61f2b2c453f`. Its source explains the transition:

- `localtime_r()` calls `tzset()`, which lazily loads the ICU locale backend.
- `ICUCtypeData::Initialize()` publishes pointers to `fClassInfo`, `fToLowerMap`
  and `fToUpperMap` before they contain their initial POSIX data.
- `ICULocaleBackend::Initialize()` performs other initialization before calling
  `_SetPosixLocale(LC_ALL)`, which fills those tables.
- `vfscanf` reads the global locale's ctype table through `isdigit_l()` during
  that interval, even on a thread whose ordinary `isdigit()` still uses the
  original POSIX table.

The standalone native reproducer starts three number-scanning threads and then
makes the process's first `localtime_r()` call. With the committed reproducer,
four of five cold stock-Haiku processes and all five cold airOS processes report
invalid scans. All five processes on each platform pass when `tzset()` finishes
before the reader threads start. Earlier scan-only and EGL-only probes did not
trigger the first time conversion and therefore missed this race.

## Workaround and validation

`WTF::initializeMainThreadPlatform()` calls `tzset()` on Haiku. The surrounding
main-thread initialization runs once, before the main run loop and WebKit worker
threads. No global `setlocale()` call, parser fallback, delay, or retry is added.
This is a startup workaround, not general synchronization for later application
changes to the process-wide locale.

Engine patch:
`23c96c0dc3d2f4a6b17618931d312ab3319cfdc89f2adfeb1aca95be962de558`.
All four engine targets build; all 1,473 modified source files match the owned
build tree. The existing PGO-profile build emits profile-mismatch warnings for
the changed function; no profile retraining is claimed.

The isolated airOS candidate passes **20 consecutive** TLS/page/worker/service-
worker/WebSocket cycles without a preload. Cookie, localStorage and Cache API
values advance from 1 through 20; ten service-worker unregistrations pass.
Every browser exits zero, every owned group drains, every held connection closes,
and the native syslog interval has no new debugger events. Loaded-runtime and
fixture hashes stay unchanged.

Stock Haiku also passes **20 consecutive cycles** with the published `e732dd0`
browser, WebKit library, helper executables and Mesa, substituting only the
candidate JavaScriptCore library that contains the startup change. Actual loaded
paths and hashes verify that substitution in every process. All the cleanup,
persistence, native-crash and unchanged-input checks above pass. This checks the
workaround on stock libroot; it is not a full release build of the new patch.
The separate full release check below now also passes.

An initial attempt to run the full airOS-built candidate on stock Haiku failed
at relocation because its WebKit library requires airOS's `StrokeFrameLineArray`
symbol. That attempt never opened the fixture and is not an acceptance result.
The published stock `e732dd0` release independently passes five network cycles;
it includes the curl fix but predates the locale workaround.

The [machine-readable evidence](locale-startup-2026-10-22.json) retains diagnostic
results, runtime hashes, the cold/warm comparisons and browser acceptance.
Repeated loaded-image inventories are indexed once per distinct inventory;
each process retains its inventory index.
#120 remains open for its other work.

### Full release packages

CI [39649186372](https://github.com/jmgasper/summit/actions/runs/39649186372)
succeeded for revision `56f49a8`. Its complete published stock-Haiku and airOS
x86_64 packages each pass five TLS/worker/WebSocket shutdown and persistence
cycles without a preload or substituted library. Every owned process exits
normally, all held connections close, both native crash intervals are clean,
and loaded-runtime and fixture hashes remain unchanged. The stock packages were
installed normally; only paths were relocated in the isolated workstation copy.
These are ordinary CI builds without profile use. Exact package/runtime hashes
and per-cycle results are in [the release report](network-release-2026-10-22.json).

## Reproduction

On native Haiku:

```sh
g++ -std=c++17 -O2 tests/NativeLocaleInitialization.cpp -o NativeLocaleInitialization
./NativeLocaleInitialization
./NativeLocaleInitialization --warm
```

Run each invocation as a fresh process. The default deliberately exposes the
OS race and returns nonzero if any scan fails; a passing cold process does not
disprove the race. `--warm` is the control that completes initialization first.
The browser workload is `tools/test-modern-network-shutdown.py`; its report
includes actual loaded libraries, process cleanup, persistence and native
debugger monitoring, rather than checking only the browser's exit code.
