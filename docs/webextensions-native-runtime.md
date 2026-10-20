# Native extension engine runtime

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The full Extensions-enabled engine first links at patch
`c8b960d698b09f68d906606c00f4ac52e7a2d14fda74a42b2bb7ac49037fb81b`.
The later store-cleanup patch
`284266a9d79e372f7c617cbf1e6694b6d309035c9b725b850197da34e5a49011`
also completes all engine targets. Both builds enable `WK_WEB_EXTENSIONS` and
`CONTENT_EXTENSIONS` and use the private ICU/libzip dependencies.

The real persistent rule store passes **111 checks**; see
[request-rule loading](webextensions-native-dnr-loader.md). The first successful
extension run executes its background JavaScript, privileged storage operations
and native test-message IPC after an ordinary page has loaded. Patch
`96ce44b447dd95dc56a01cbe6b016043500cbdf8b984fb4f6f77a79b392da1bb`
then fixes direct startup: both contexts execute without page preparation,
passing **14 native checks and ten JavaScript assertions**. A separate real
cookie-observer test exposes lost event delivery after rapid unregister/register.

## Actual extension fixture

`EngineExtensionRuntimeTests.cpp` constructs an isolated manifest-v2 package
directory and loads it through the native package snapshot, extension and
controller classes. The host grants the fixture's declared storage permission
explicitly. Its extension profile is temporary and persistent for the test;
its website data store is a separately owned nonpersistent store. The controller
treats its own store as the normal extension environment.

The intended two rounds queue a native test message before the JavaScript
listener exists, check the `browser` and `chrome` bindings and runtime identity,
write storage through promises and read it through callbacks, then recreate the
extension context to check persistent state. Successful completion requires the
background DOM title, ten assertion reports, two completion reports with a
matching nonce and two success reports from the native test-message IPC route.
Compilation alone does not satisfy any of those JavaScript requirements.

```sh
python3 tools/test-engine-content-rule-pipeline.py --extension-runtime
```

The runner verifies source/header hashes and completed engine targets, links the
actual WebKit library, selects the matching helper executables and records their
hashes. It checks the native crash log and all input/library/helper hashes after
execution. Output goes to a file so orphan helpers cannot keep a pipe open.
The fixture runs in a new process group inherited by the native fork/exec
launcher; forced cleanup makes the test fail. Live Haiku team IDs determine
whether the group has drained, because `killpg(group, 0)` can still succeed
after all its live teams have exited.

Native runner checks verify normal exit, an orphan child and a timeout in
`.vm/extension-runner-process-check-live-teams.json`. They test OS process
cleanup, not WebKit or an extension. During normal fixture teardown, the host
unloads the context and explicitly requests termination of its owned helper
processes. This does not test automatic background-process retirement.

## Observed failure

The first run against the initial linked engine records **six native checks
passed and one deadline failure** in
`.vm/content-rule-pipeline.NCFQATF3/runtime-result.json`. The package snapshot,
context load and storage grant succeed. Both native helper processes start,
but the background title never becomes nonempty and no JavaScript test reports
arrive. The fixture reaches its own deadline, unloads and exits; no forced
runner cleanup or debugger event occurs.

A second run adds page-state diagnostics against the store-cleanup build:
`.vm/content-rule-pipeline.cOE5dEEj/runtime-result.json`. It shows a background
page with a running process, `loading=1`, the requested extension URL as its
active URL, and an empty provisional URL. That state does not advance before
the deadline. Again there are six native passes, one failure, no JavaScript
reports, no debugger events and no remaining owned helper processes. All source,
header, library and helper hashes remain unchanged during both runs.

## First successful extension execution and startup controls

An ordinary offscreen page, using the same native page client, successfully
executes JavaScript before the extension is loaded. After that control page is
closed and its WebProcess is explicitly terminated, both extension rounds
succeed: **15 native checks and ten JavaScript assertions pass**, with two native
completion messages and two success reports. Storage survives destruction and
recreation of the extension context, and both promise and callback API forms
cross privileged IPC. Evidence: `.vm/content-rule-pipeline.ZgViMefk/result.json`.
All input/library/helper hashes remain unchanged, the crash-log interval is
clean and all owned helper processes exit without forced runner cleanup.

The fixture now offers diagnostic startup modes while keeping direct startup
as its default:

```sh
python3 tools/test-engine-content-rule-pipeline.py --extension-runtime --extension-startup page
```

At patch `284266a9d79e372f7c617cbf1e6694b6d309035c9b725b850197da34e5a49011`,
the `cold` and `deferred` modes reproduce the stall without any JavaScript
reports. Results are preserved in `.vm/extension-startup-{cold,deferred}-284-result.json`.
An earlier `network`-labelled attempt also stalls, but its private-store fetch
could complete without launching NetworkProcess. That result does not establish
actual network preparation. The fixture now explicitly launches the process
before fetching data and requires its owned child to be present at completion.
Each earlier run has unchanged inputs and binaries, no debugger event, and a
clean process-group exit. The warm-page success establishes actual extension
execution, but does not resolve the direct-startup defect.

## IPC startup diagnosis

Temporary message tracing at patch
`1c3818e80445d7b70578342c0105d0c27de0cdea34161af6d805731ef6249199`
shows WebProcess waiting for its network connection while NetworkProcess stops
dispatching after receiving `StartObservingCookieChanges`. The result is
`.vm/content-rule-pipeline.AqZZFI1F/runtime-result.json`.

Native session recreation sends that message with
`DispatchMessageEvenWhenWaitingForSyncReply`, but its receiver metadata did not
allow the flag. The invalid-flag branch also called the error dispatcher while
holding the same incoming-message lock that the dispatcher reacquires.

The startup correction gives native cookie observation matching metadata and
flags on both send paths, and releases the incoming-message locks before
scheduling invalid-flag rejection. It removes the temporary tracing. The full
build links WebKit and both helper executables without compiler errors or
undefined symbols. `EngineIPCValidationTests.cpp`
exercises malformed flags, subsequent valid traffic and invalidation from the
error callback through actual WebKit connection endpoints and native sockets;
both endpoints live in the fixture process. **All 23 IPC checks pass** in
`.vm/content-rule-pipeline.XFrD2pQI/runtime-result.json`.

The direct-startup run is
`.vm/content-rule-pipeline.ZJOxn6oV/runtime-result.json`: **14 native checks,
ten JavaScript assertions, two nonce-matched completion messages and two success
reports pass**. It uses the default `cold` mode and no ordinary control page.
Storage survives destruction and recreation of the extension context. Both
runtimes have unchanged inputs/libraries, no debugger events and clean process
groups without forced cleanup. The extension helper hashes also remain unchanged.

`EngineCookieObserverRuntimeTests.cpp` adds a separate full-engine regression
for the observer lifecycle. It creates an owned nonpersistent website data
store, writes through the real API cookie store and verifies both the committed
receipt and typed event. It then unregisters and registers the observer 32
times without waiting, uses a network query as a barrier, and checks that the
next write still produces exactly one populated event. Finally it unregisters
the observer and verifies a confirmed write produces no event. This tests
NetworkProcess delivery and registration ordering, not JavaScript listeners.
Its first runtime has **13 passes and one failure** in
`.vm/content-rule-pipeline.F0ocC1xc/runtime-result.json`. Initial registration
delivers the correct typed event. After the repeated unregister/register calls,
the write receipt and subsequent query confirm the new cookie, but no event
arrives before the deadline. The final unregister-only phase is not reached.
All source/library/helper hashes match, no debugger event occurs and the owned
process exits without forced cleanup.

Registration is dispatched through the priority queue while removal remains
ordinary; later registrations can overtake pending removals and leave observation
disabled. Follow-up patch `b42d85ea...` gives native removal the same dispatch
metadata and sender flag as registration. Its full rebuild links successfully,
and all **21 observer checks pass** in
`.vm/content-rule-pipeline.QSQqviNE/runtime-result.json`, including delivery after
32 unregister/register cycles and silence after final removal. The same engine
also passes all **23 IPC checks** in `.vm/content-rule-pipeline.27UXXcU8/runtime-result.json`
and **14 native plus ten JavaScript extension checks** in
`.vm/content-rule-pipeline.aXshZMyg/runtime-result.json`. All three runs preserve
their input/library hashes, have clean crash-log intervals, and drain their owned
process groups without forced cleanup. The IPC rejection fixture now uses the still-ordinary
`DeleteAllCookies` message name with its own socket clients and payload, because
native observer removal is now intentionally allowed in the priority queue.
This failure does not invalidate the separately verified direct-startup or
IPC-rejection results.

```sh
python3 tools/test-engine-content-rule-pipeline.py --cookie-observers
```

## Cookie APIs in an actual extension

The runtime fixture can load the checked-in cookie package with explicitly
granted `cookies` and `https://summit-cookie.invalid/*` permissions:

```sh
python3 tools/test-engine-content-rule-pipeline.py --extension-runtime --extension-fixture cookies
```

At patch `32b97d3a...`, `.vm/content-rule-pipeline.CmGvDVb2/result.json` records
**14 native checks and 28 JavaScript assertions passed** across two cold-start
extension contexts. The background page uses promise and Chrome callback forms
to set, query, overwrite and remove secure HttpOnly cookies. It verifies the
committed cookie fields, ordered overwrite removal/addition events, deletion
events, silence after listener removal, and a cookie surviving destruction and
recreation of the extension context. Both rounds deliver nonce-matched completion
messages and success reports through privileged test IPC.

The package resources are staged with hashes, checked before execution and
included in the input snapshot checked afterward. Sources, headers, libraries
and helpers remain unchanged, the crash-log interval is clean, and all owned
helper processes exit without forced cleanup. This run does not cover cookies
created by HTTP responses, browser restart persistence, or permission revocation.
The default storage package also passes with the updated fixture and runner:
`.vm/content-rule-pipeline.kvjSYIF4/result.json` records 14 native checks, ten
JavaScript assertions, unchanged inputs/binaries, and clean process teardown.

The fixture does not implement browser installation UI, store distribution, signature
verification, manifest-v3 workers, action UI, DNR enforcement or complete
Safari/Chrome/Firefox compatibility. Those remain part of the browser goal.
