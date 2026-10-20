# Common extension queued-test execution

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The implementation moves `addTest` and the sequential test executor from Cocoa to
common C++. Named functions are queued with their completion functions and
source locations. Each run emits TestAdded, TestStarted and TestFinished through
the existing IPC path, then resolves or rejects its result promise. A following
test is scheduled through the main work queue rather than recursive execution.

The adapter rejects noncallable input and anonymous functions before queueing.
Property/name-conversion errors reject with their original reasons. Regular
validation failures retain the upstream string rejection format. A thrown test
or failed assertion reports failure and rejects its result; synchronous values
and thenable results pass through the intrinsic promise helper. An unavailable
script execution context explicitly reports failure. No JS completion can be
executed in a stopped context.

Queued test functions and completions retain the existing Protected references.
Once a test starts, its completion functions are traced through the JavaScript
promise graph. The shared promise helper consumes its native completion callback
on settlement, releasing captured native state without waiting for collection.
Assertion state is cleared between tests. Full queued-API/namespace lifetime
remains unverified; the isolated helper's lifetime checks do not establish it.

The helper passes 41 native checks in
`.vm/extension-promise-tests.Oo4f8mJP/result.json`, including the existing promise,
exception, capture and context-release cases plus immediate release of settled
native callback state. The process exits normally with no new debugger events;
sources, native inputs and frozen JavaScriptCore/WTF libraries remain unchanged.

The API executor itself has only compile evidence. No WebExtensionAPITest object,
queued test, DOM execution context, report IPC or extension was run. Cocoa's
queued-test suite remains unexecuted. Those integration checks are required
before claiming a working extension test runner.

Both final integration units compile with both extension flags and regenerated
IPC: the helper in `.vm/extension-lifecycle-inputs.trYsqVV0/result.json`, and
the final API source in `.vm/extension-lifecycle-inputs.uzc5sbSn/result.json`.
The latter includes the unavailable-context failure path and avoids protecting
null C API results after exceptions. `.vm/extension-test-executor-validation.json`
matches final source hashes to these reports and the 41-check helper probe.

Promoted patch: `ba1ee173c7963284ecede95c47830e7109b13fad531aca9c2c4495f2212f221f`.
Probe baseline: `35dcd668ac188e1ef81ca581f5be6303a44db6f16a5614f5e0948d9f06838643`.
