# Native windows reads and event dispatch

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The Haiku port implements `windows.get`, `getCurrent`, `getLastFocused`,
`getAll`, `WINDOW_ID_NONE`, `WINDOW_ID_CURRENT`, and web-process dispatch for
`onCreated`, `onRemoved`, and `onFocusChanged`. It reads the native browser
registry and uses the existing permission-filtered tab parameters. **No extension has run these APIs.**

Engine patch
`6730e1ed2e25f7c9f9a8071634aed7e8dee6e4f374fce052230b745f627b155e`
is promoted. Its full extension-enabled engine build reached the WebKit link
with **four missing symbols (eight references), down from five**. The windows
dispatcher is resolved, with no new missing symbols or compiler errors.
The remaining symbols cover the DNR loader and menu, action and cookie
dispatchers. Both native source/configuration checks match this patch.
Evidence: `.vm/modern-extensions-window-api-build-result.json`.

The four reads require a loaded privileged context. `getCurrent` resolves the
calling page's window, falling back to the frontmost accessible browser window
through the native context helper. `getLastFocused` uses the retained frontmost
window order even when the browser has lost focus. Missing or inaccessible
windows produce an error; `getAll` can return an empty array. No read requests
additional permissions. Populated tabs retain the metadata allowed by the
existing `tabs` or host permissions and private-data checks.

Options accept `populate` as a boolean and `windowTypes` as a non-empty array
containing `normal`, `popup`, or both. Omitted options select both types without
populating tabs. The event filter accepts only `windowTypes`. Unsupported
fields, malformed values and unknown types produce errors; mixed arrays with
unknown types are stricter than the pinned Cocoa parser. Public window inputs
must be valid safe integer identifiers or `WINDOW_ID_CURRENT`.

Registered native browser windows currently have normal type and normal window
feel. Popup-only filters match no registered windows. State and geometry are
not available from the registry and are omitted. Window creation, updates and
removal through extensions remain unfinished and are not exposed as native API
methods.

Window objects use a native JavaScriptCore converter and reuse the existing
tab converter for populated tabs. Optional fields remain optional, explicit
false flags remain booleans, and already-redacted tab metadata stays redacted.
Each event listener receives a fresh protected value. Listener snapshots keep
iteration stable if callbacks add or remove listeners, and type filters select
the matching callbacks. An incoming focus event without a window dispatches the public
`WINDOW_ID_NONE` to listeners for either supported type.

A registry observer connects committed native browser changes to the loaded
extension contexts. It publishes independent before/after snapshots after
validation and suppresses no-op updates. Callback copies permit nested updates
or unregistering during dispatch. The controller uses a weak capture and a
snapshot of its loaded contexts.

Each context keeps the public identifiers of registered windows until their
removal transition, including when a dead native messenger has already caused
its wrapper to be pruned. Initial load seeds these identities without sending
creation events; unload clears them. Creation, removal and focus transitions
update context bookkeeping and wake background content before dispatch. The
completion checks the same load identity and current private-data access.
Captured transition values remain stable if the browser changes again while
the background content wakes.

End-to-end reads, actual permission decisions, listener registration, IPC and
event delivery still require the complete extension-enabled browser and remain
unverified. Native tab lifecycle notifications remain separate unfinished work.

## Verification

The isolated native helper passed **111 checks** of the new options parser and
actual JavaScriptCore window/tab/string converters. It covers malformed options,
defaults, independent filters, nested tab order and values, Unicode, redaction
preservation, mutation and garbage collection. The harness confirms execution
from `BApplication::ReadyToRun`, unchanged source and frozen library inputs,
and no native crash-log events. It links JavaScriptCore/WTF and ICU; it does not
link WebKit/WebCore or instantiate extension contexts.

**318 generated-binding checks pass**, including the Cocoa/Haiku window API
surfaces and checks against Objective-C argument handling in native bindings.
Nineteen of 37 interfaces now generate C++ bindings.
**Seventeen native integration units compile** against the combined candidate,
including the APIs, converters, generated bindings/IPC, registry observer,
controller forwarding, context bookkeeping and native context constructor.
The audit matches current candidate sources and headers to the compiled inputs. Cocoa JSON adapters retain the existing
Objective-C implementations; no Cocoa SDK compile has been performed.

The native registry helper passed **594 checks**, extending the existing
543-check suite with observer transitions, no-op and invalid mutation handling,
focus loss, atomic moves, independent snapshots, nested callbacks and observer
removal. It uses real native messengers, but no WebPage, SDK, extension context
or IPC runtime. The suite confirms it ran from `BApplication::ReadyToRun` and
records unchanged sources/libraries and no native crash-log events.

Evidence:

- `.vm/extension-window-lifecycle-validation.json`
- `.vm/extension-lifecycle-inputs.bjbSMxsr/result.json`
- `.vm/extension-window-json-tests.cdOChCiO/result.json`
- `.vm/extension-bindings-generated-2j8icenl/binding-generation.json`
- `.vm/extension-binding-platforms-w5pg85fn/result.json`
- `.vm/extension-lifecycle-inputs.JAL578rl/result.json`
- `.vm/extension-browser-tab-registry-tests.DC2Vz4JN/result.json`
