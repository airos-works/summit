# Native extension storage bindings

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The storage namespace, local/session/sync area objects, seven storage methods,
quota properties and change-event dispatch now use common C++.
`Storage` and `StorageArea` use C++ generated bindings. Their messages retain
the existing storage permission validator and can reach the existing common
UI handlers on Haiku.

The value adapter serializes each entry independently with JavaScriptCore's
JSON intrinsic. It skips undefined entries, rejects nonserializable entries
and checks the per-item and per-call limits using UTF-8 key and serialized
value sizes. It enumerates own enumerable string keys. Proxy arrays use the
engine's array intrinsic, because the C API's `JSValueIsArray` only recognizes
direct arrays.

Reads retain default JavaScript values across an asynchronous reply without
JSON conversion, preserving object/function identity and undefined values.
Stored values override defaults. The result uses own data properties so a
key named `__proto__` cannot invoke a prototype setter. Invalid stored JSON
rejects the reply. Empty get selections and empty removals complete locally.
The existing byte-count behavior for an empty key array is retained.

Change-event chunks are merged and dispatched to both the namespace and area
listeners in the requested content world. Each listener receives a parsed
changes object followed by the area name. The listener list is copied before
invocation, and the changes object remains protected during argument creation.

Validation:

- `.vm/extension-storage-value-tests.bAX3nBDI/result.json`: 73 native
  JavaScriptCore/WTF checks pass with normal exit, unchanged input/library
  hashes and no new crash log. Coverage includes independent JSON encoding,
  exact UTF-8 quota boundaries, getters and serialization failures, key
  selection, proxies, retained defaults, malformed replies, prototype-looking
  and numeric keys, inherited setters, and collection between native calls.
- `.vm/extension-bindings-generated-z4ohxfpl/binding-generation.json`:
  all 37 interfaces generate with unchanged inputs; ten now use C++ bindings.
  Storage's optional callbacks, promise creation and page/context arguments
  appear in the generated code.
- `.vm/extension-lifecycle-inputs.kwWa64Tr/result.json`: nine units compile
  with both extension features enabled in isolation and matching regenerated
  IPC. These include the two API implementations, namespace, three generated
  bindings, helper, UI receiver and existing UI storage handlers.
- `.vm/extension-lifecycle-inputs.XtOUQfMx/result.json`: the final helper
  compiles again after correcting numeric-key insertion. Its source hash
  matches the final runtime test. The other eight units are unchanged.
- `.vm/extension-storage-validation.json` verifies the current source hashes
  against those reports. `.vm/extension-storage-extraction.json` records
  thirteen unchanged common method bodies and the unchanged storage message
  signatures and permission validators.

The earlier probe `WDF8Vhpr` failed one of 64 checks: array proxies were
misclassified by `JSValueIsArray`. The corrected intrinsic-based probe
`vVxVlmAX` passed 69 checks before the final numeric-key checks were added.
Earlier compile failures for the obsolete exception-scope header, a test
function-pointer declaration and the Cocoa-gated storage messages are retained
in their original logs. They are not counted as passing runs.

The tests do not run a loaded extension, storage IPC, SQLite persistence,
access-level changes or change listeners. No cloud synchronization is tested.
Cocoa compilation and its extension test suite have not been run. Database
work remains: the existing backend mixes SQLite text lengths with WTF string
allocation sizes for quota accounting, appends replacement keys when counting
sync entries, and interpolates escaped keys into SQL predicates. Unicode,
embedded-NUL keys, replacement quotas and concurrent writes need native
database tests. These binding checks do not establish storage compatibility.

Promoted patch: `b330520c856f6a9a46eb9e787cba7acdd119772e062f68f96c06aed7c1d350a6`.
Probe baseline: `75a0dfd1b1212c05e00e58ec22cd613289a90ac45284517fe47803024adf3b70`.
