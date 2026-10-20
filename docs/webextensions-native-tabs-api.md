# Native tabs reads and events

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The Haiku port implements `tabs.get`, `tabs.getCurrent`, `tabs.query`,
`TAB_ID_NONE`, and all nine upstream tab lifecycle events to the native C++
bindings. It uses the existing native browser registry and permission-filtered
tab parameters. This implementation has passed isolated native checks; its
full engine build compiled and reached linking, leaving five missing symbols. **No extension has run these APIs.**

## Read behavior

The three read methods use IPC requiring a loaded privileged context. `get`
rejects invalid identifiers and reports an error for unavailable tabs.
`getCurrent` returns undefined when the calling page is not an associated
browser tab, including background pages. Neither method prompts for additional
access.

`query` parses the pinned upstream filter fields and validates match-pattern
syntax and supported schemes again in the UI process. It reads window order,
selection, tab index, loading state, audio and mute state from the registered
browser objects. Explicit false filters remain distinct from omitted filters.
The current-window identifier and `currentWindow` boolean remain separate
constraints, and `highlighted` takes precedence over `selected` independently
of JSON property order.

URL/title metadata and filtering require either the named `tabs` permission
or access to that tab's URL. The named permission affects metadata only; the
existing host-access check remains separate for execution and network access.
Every lookup checks tab/window validity and private-data access. No URL or
title is fetched by the JavaScript converter itself.

Registered native browser windows currently have normal type and tabs have no
pinned state. Popup-type and pinned-only queries therefore match no registered
native browser objects. Index filters require nonnegative safe integers;
unsupported query fields produce an error. This is stricter than the pinned
Cocoa implementation's index clamping. Popup/sidebar host associations and the
remaining native tab mutation, navigation, capture, script and messaging APIs
remain unfinished; those methods are not exposed as native implementations.

## Event payloads

The port implements dispatch for created, updated, activated, attached,
detached, highlighted, moved, removed and replaced tabs. Attached/detached
events update the existing web-process page/window association. Payloads
preserve the API's argument order, numeric identifiers and nested info objects.

Created/updated events and read results reuse the existing
`JSWebExtensionTabParameters` converter. This preserves optional properties,
empty redacted metadata and the converter's NaN representation for an unknown
index. A new argument factory protects each value before constructing the
next one, and listener snapshots retain callback order during mutation. Each
listener receives newly created objects. JSON arguments used by the other
events are likewise parsed and protected separately for each listener.

The web-process dispatchers still need native host lifecycle notifications
connected through the UI process before browser actions can produce these
events in an extension.

## Verification

**136 native checks pass** for the new structural query parser and the actual
existing JavaScriptCore tab/string converters. They cover invalid flags and
identifiers, explicit false values, filter precedence, malformed and unsupported
fields, URL/title encoding, optional metadata, redaction preservation, public
identifier sentinels, unknown indices, object mutation and garbage collection.
The harness verifies that it ran from `BApplication::ReadyToRun` before reporting
success. Frozen libraries and source inputs remain unchanged, and native
crash-log coverage records no events.

**Nine native integration translation units compile**, covering the parser,
UI API, metadata provider, web-process API, event helper, namespace, generated
tabs/namespace bindings and regenerated IPC. **252 generated-binding checks
pass**, including 132 checks of the tabs methods/events and namespace on Haiku
and Cocoa. Seventeen of 37 interfaces now generate C++ bindings.

The native helper links frozen JavaScriptCore/WTF and ICU, not WebKit or
WebCore. It uses a real JavaScriptCore context but does not load an extension
context, inspect browser tabs, execute API IPC, make permission decisions, or
dispatch browser events. Those runtime behaviors still need the complete
extension-enabled browser. Cocoa implementation wrappers preserve the existing
methods but have not been compiled against a Cocoa SDK.

The full feature-enabled build resolved all nine tab dispatchers, reducing
missing symbols from 14 to **5** (9 references). It reported no new missing
symbols or compiler errors. At this tabs checkpoint, the remaining symbols are the declarative request-rule
loader and windows, menus, action and cookies dispatchers. This does not yet
produce a runnable extension-enabled browser.

Evidence:

- `.vm/modern-extensions-tab-api-build-result.json`
- `.vm/extension-tab-api-validation.json`
- `.vm/extension-tab-json-tests.8lM4jSwH/result.json`
- `.vm/extension-binding-platforms-h8g8u7lm/result.json`

The audit combines passing, hash-matched translation units from independent
compile probes. `getCurrent` explicitly excludes extension views; it does not
use the native helper's default fallback to the active tab for a background
page.

The promoted patch is
`f0952d6df18433d24eff8f132fc8bc1cd5f66c860936e00ff74aa3c644515db6`.
