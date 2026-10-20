# Native sorted JSON for extension test reporting

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The native JS wrapper now implements `toSortedJSONString`, used by extension
test reports. JavaScriptCore serializes the input once, preserving JSON's
handling of getters, `toJSON`, omitted properties and invalid root values.
The result is parsed with WTF JSON and written with object keys sorted at
every level. Array order is retained. The existing Cocoa implementation is
unchanged; exact Foundation formatting parity is not claimed.

Evidence:

- `.vm/extension-sorted-json-tests.PVd151KU/result.json`: all 28 native checks
  pass, with normal exit, unchanged input/source/library hashes and no fresh
  debugger event. Coverage includes recursive ordering, numeric-looking keys,
  JSON fragments, precision, Unicode/NUL/surrogates, `__proto__` data, getters,
  `toJSON`, cycles, exceptions, replaced `JSON.stringify`, and collection.
- `.vm/extension-lifecycle-inputs.z0A8mJCH/result.json`: the actual wrapper
  unit compiles with both extension features enabled and regenerated IPC.
  Native inputs and configuration remain unchanged.
- The final wrapper source hash matches both reports:
  `c2d863a4d9217f341f5a71d7c592f758b0357a772363a67f609a0e5918274c09`.
- `tools/test-engine-extension-sorted-json.py` links the actual wrapper and
  string bridge against frozen JavaScriptCore, without WebCore or WebKit
  libraries. Unused wrapper functions are removed by the linker.

Serialization failures return a null string. WTF JSON's existing nesting
limit also applies. These checks do not run extension test APIs, controller
callbacks, generated bindings or IPC. No extension has run in the browser.

Promoted patch: `7f17d7a6cc8b598ce1774299459c0e953e5002f938c75db14201bced7aff9029`.
Probe baseline: `7be92cbcfb08a29914cf9f58eed2197bd4dd20f7f8755f10f88f5cb3b877a284`.
