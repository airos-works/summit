# Native navigation API and URL filters

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The process-side `webNavigation` API now uses C++ bindings for its five events,
`getFrame` and `getAllFrames`. The namespace getter is available on Haiku with
its existing permission and content-world restrictions. The two frame-query
IPC messages retain their privileged-message/permission validator. Native
UI-process frame-query handling and the browser tab bridge remain unfinished;
this change does not establish a working extension API in Summit.

Navigation events serialize URL, tab/frame/parent IDs, optional document ID and
floored millisecond timestamps. Frame queries serialize the error flag and
redacted URLs separately, retaining the main-frame/absent-parent sentinels and
omitting absent optional fields. Missing event frame or URL parameters stop
delivery. Each listener receives a fresh JSON-derived JavaScript object, and
dispatch holds a copy of the listener list across callbacks.

The URL filter owns a parsed snapshot of all 20 criteria. Criteria within one
group must all match; any matching group is sufficient. As in upstream WebKit,
an empty filter array matches every valid URL. Host containment adds a leading
dot. URL matching removes the fragment, and origin/path matching also removes
the query. Explicit and default ports use the same numeric comparison. These
rules follow the [Mozilla URL-filter reference](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/events/UrlFilter).

The common implementation uses JavaScriptCore's regular-expression engine and
WTF's canonical URL components. This differs from the former Cocoa filter's
NSRegularExpression and NSURL components: paths/queries retain percent escapes,
default ports are resolved, unknown criteria produce a validation error, and
fractional ports are rejected instead of truncated. Equal range endpoints remain
invalid as in upstream WebKit. No claim of complete Safari/Chrome/Firefox filter
parity is made. Cocoa compilation and runtime have not been tested.

Native evidence:

- `.vm/extension-navigation-filter-tests.aifbGKge/result.json`: **136 checks
  pass**, normal exit 0, no new crash-log events, unchanged staged sources,
  baseline configuration/source manifest and frozen libraries. This executes the
  actual URL filter, navigation parameter helpers and frame-identifier source
  against native WTF/JavaScriptCore; it links neither WebCore nor WebKit.
- `.vm/extension-navigation-filter-tests.ZkMIgrg9/result.json`: the earlier
  filter-only suite passes all 98 checks.
- `.vm/extension-lifecycle-inputs.GtVwPEEm/result.json` and
  `.vm/extension-lifecycle-inputs.Rp26elkF/result.json`: **11 native units compile**
  with both extension features enabled, regenerated IPC and fresh C++ bindings.
  This covers both helpers, the namespace, navigation API/listeners, validator,
  three generated bindings and two generated receivers. Final staged source
  hashes are audited in `.vm/extension-navigation-api-validation.json`.
- `.vm/extension-navigation-api-extraction.json`: nine moved Cocoa method bodies
  and both IPC signatures/validators are unchanged.

The earlier `.KMrIJMdK` probe failed compilation due to Ref access and character
pointer types. The `.5eM3SQaJ` probe compiled but entered Haiku's debugger at the
regex allocator assertion and timed out; it is a failed run. The harness had
initialized WTF but had not initialized JavaScriptCore's options. Creating a
real JavaScriptCore context before regex matching fixed that harness setup.

These probes do not instantiate a navigation API namespace or listener, deliver
IPC, execute a callback in a DOM context, inspect a live browser frame tree or
run an extension. The full feature-enabled WebKit library still has unresolved
native host and event bridges.

Promoted patch: `5358cf199868ababff67cf59212eb911603ae30f75b7980e3fae366f1b993945`.
Probe baseline: `ba1ee173c7963284ecede95c47830e7109b13fad531aca9c2c4495f2212f221f`.
