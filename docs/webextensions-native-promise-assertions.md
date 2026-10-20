# Native extension promise assertions

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The implementation moves `browser.test.assertRejects` and `assertResolves` from Cocoa
to common C++. A shared helper resolves input through a fresh intrinsic promise
and transforms its settlement through native callbacks. Callback return values
resolve the output promise; callback exceptions reject it. The helper uses
JavaScriptCore's traced callback captures rather than retaining a global context.

The assertion adapter preserves rejection matching by any error, regular
expression or loose equality, and reports through the existing assertion path.
A mismatch records an assertion; an unexpectedly fulfilled `assertRejects`
rejects with undefined. `assertResolves` retains the upstream nonthenable-input
check and its fulfilled-value/failed-assertion behavior. Errors while reading
`message`, testing a regular expression or comparing values propagate as output
promise rejections. Global RegExp lookup and the initial thenable check remain
upstream behaviors; the intrinsic helper's tampering tests do not establish
those API-level behaviors.

The helper passes 40 native checks in
`.vm/extension-promise-tests.XqUPCMYi/result.json`: primitive values, identity,
fulfillment/rejection including undefined, repeated thenable settlements,
throwing then getters/calls, completion return/exception handling, intrinsic
hooks under replaced Promise globals/then/species, captures across collection,
128 competing settlements, context-release lifetime, and rejection-message
access including getters and proxies. The process exits normally with no new
debugger events; sources, native inputs and frozen JavaScriptCore/WTF libraries
remain unchanged. The earlier 32-check helper-only pass is retained separately.

The helper links frozen JavaScriptCore/WTF, without WebCore or WebKit. No
WebExtensionAPITest object, assertion report, extension or IPC was executed.
Full API/namespace lifetime and compatibility remain unverified. The queued-test
executor is still Cocoa-only, and Cocoa's API test suite has not been run.

Both final integration units compile with both extension features enabled and
regenerated IPC. The helper passes in
`.vm/extension-lifecycle-inputs.DC0eD6OH/result.json`; the final assertion source
passes in `.vm/extension-lifecycle-inputs.p1CXYIyZ/result.json`.
`.vm/extension-test-promises-validation.json` matches final source hashes to
those reports and the 40-check runtime probe. An initial compile warned about
missing Strong inline definitions; the header was corrected before runtime
validation. Cocoa project entries are updated, but Cocoa was not built.

Promoted patch: `35dcd668ac188e1ef81ca581f5be6303a44db6f16a5614f5e0948d9f06838643`.
Probe baseline: `5732f4073311cf11bed8a00cb90e5aa3d87ae0bea6c0199dbf15bdf3dfb9464e`.
