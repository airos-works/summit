# Native JavaScript message replies

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The runtime's message-listener dispatcher now uses C++ callback ownership in
place of Objective-C blocks. Its native compile passes, and the reply helper
passes 51 assertions executed by JavaScriptCore inside the Haiku VM. This is
not an extension-context, IPC or browser runtime result.

`JSWebExtensionMessageReply` uses the existing `EagerCallbackAggregator` to
accept only the first reply and the engine's `JSNativeStdFunction` to retain
the callback while JavaScript owns it. Serialization occurs inside the
aggregator's first invocation. Later replies therefore cannot execute a
second `toJSON`, and a reentrant reply during serialization cannot replace
the first response. The function has normal JavaScript function behavior,
including `call`, `apply`, `bind` and promise fulfillment.

A null completion string means no listener replied. Explicit `undefined`,
an unserializable value, or a reply exceeding the upstream limit produces a
non-null empty string. The limit is 64 × 1024² serialized UTF-16 code units,
matching the former NSString length check. The tests exercise its exact
ASCII boundary, JSON fragments, Unicode, embedded NUL, cycles, BigInt and
throwing `toJSON` methods.

The thenable adapter retains the receiver and method across allocation,
creates separate fulfillment/rejection functions, and returns exceptions
from property access or invocation to the dispatcher. Rejection does not
produce a message reply. A callback retained by a thenable remains usable
even if that thenable subsequently throws. Forced garbage collection of a
retained bound callback and destruction of an unanswered context are tested.

`WebExtensionAPIRuntime.cpp` now contains
`internalDispatchRuntimeMessageEvent`. It preserves world selection,
sender-page exclusion, target frame/document filtering, external-message
listener selection, listener snapshots and user-gesture scope. Its
thenable exceptions pass through the existing WebCore exception reporter.
The Cocoa runtime file retains the sender conversion; that conversion and
its tab metadata dependency still need a native implementation.

Evidence:

- `.vm/extension-reply-tests.VSNgkQoD/result.json`: 51 JavaScript assertions,
  exit 0, no fresh debugger event, unchanged source/configuration/library
  hashes. The executable links frozen JavaScriptCore/WTF and private ICU,
  with no WebCore or WebKit library. The helper instantiates JavaScriptCore
  functions; it does not instantiate WebKit context or browser objects.
- `.vm/extension-lifecycle-inputs.vd8FjJjD/result.json`: the helper and actual
  runtime dispatcher compile with both extension features enabled in the
  isolated configuration and matching generated IPC headers.
- `.vm/extension-reply-tests.j1RPuled/result.json`: the first attempt failed
  to compile on an rvalue-string argument and a `char8_t` conversion. Those
  failures are retained; the revised sources passed the later checks.
- `tools/test-engine-extension-replies.py` reproduces the native helper
  checks with frozen dependencies. It must use a native source manifest
  matching the host engine lock.

These tests use JavaScriptCore's private C++ function API from the same
locked upstream revision as the frozen library. The two extension feature gates
do not change the inspected JavaScriptCore function/global/VM layouts.
This does not authorize mixing differently configured WebCore or WebKit
objects with that frozen library.

The full feature-enabled build is still running on its separately frozen
source tree. Native sender conversion, outgoing UI message routing,
background content and browser controller attachment remain required before
an extension message can traverse Summit's actual process boundaries.

Promoted engine patch: `fb28750f4eb0d93caf2c9190b2c09e152af29c914fd2f475bb94180e5a9dd90c`.
The probes used baseline
`91b1db7aefd1bc6212d452e2eeb3871e99ecb23febee1d8ea6472eca89d5b02f`
and isolated candidate sources. Cocoa/Xcode compilation has not been run;
the retained Cocoa definition and project header references were checked.
Aggregate: `.vm/extension-reply-native-results.json`.

Subsequent work moved sender conversion into common C++ and validated its
tab helper separately; see [message metadata](webextensions-native-message-parameters.md).
