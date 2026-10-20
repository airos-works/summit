# Native process bindings and events, 2026-09-14

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The engine patch now shares the existing WebProcess world-binding and event
implementation with Haiku. The native translation units and generated
interfaces described below compile. **No extension has executed in Summit.**
The verified browser artifact remains `bundle-yqhmejfw`, with extensions off.

`WebExtensionControllerProxy.cpp` now contains the C++ implementation formerly
in its Cocoa file: normal/isolated-world namespace selection, `browser` and
`chrome` installation, service-worker hooks, frame tracking, script-error
callbacks and navigation-event sends. The method bodies preserve upstream
origin/world selection and callback ownership. `WebExtensionContextProxy.cpp`
also contains the script-error send, with its message and sender-inline
headers included explicitly. The vacated Cocoa files retain their licenses
and remain valid empty translation units for existing project references.

Haiku's `WebPage` now retains the controller proxy from page-creation
parameters and returns it through its accessor. The frame-loader hooks for
provisional/committed/finished/failed navigation, global-object availability,
service workers and pre-user-script injection include Haiku. These callbacks
still require a controller attached by the browser; this patch does not attach
one to ordinary Summit pages or expose an installation UI.

Portable event invocation and listener management moved from
`WebExtensionAPIEventCocoa.mm` to `API/WebExtensionAPIEvent.cpp`. Listener
invocation still copies the listener vector before callbacks can mutate it.
The Cocoa `id` argument overloads remain in the Cocoa implementation. UI-side
frame/listener counts and background listener bookkeeping moved to
`UIProcess/Extensions/API/WebExtensionContextAPIEvent.cpp`; only log-string
conversion needed a native adaptation. Their background-page and queued-test
dependencies still need a functioning native context runtime.

`DidEncounterScriptError`, `AddListener` and `RemoveListener` are available
outside the Cocoa message gate, each retaining `[Validator=isLoaded]`.
The existing controller frame-event validators are unchanged. The event IDL
now uses the upstream `UseCPPAPI` generator route, bringing the C++ list to
seven of 37 extension IDLs. CMake, `DerivedSources.make` and the Xcode generated
file reference agree on the event's `.cpp` output. A Cocoa/Xcode build has not
been run here. The native source list also includes `JSWebExtensionWrapper.cpp`;
its Objective-C `nil` became `nullptr`, and an exhaustive enum conversion now
traps an invalid enum instead of falling through a non-void function.

Verification used configured native compiler flags with both extension gates
enabled in an isolated configuration, without the feature-disabled PCH.
The configured production tree was patch
`fd009b76e291c55a5ba28f2400c9071c4e620667ccd46b1fadcb860c19f8c192`, with recorded
candidate overlays. No feature-enabled object was linked to the preview
WebKit library.

| Native report under `.vm/` | Evidence |
| --- | --- |
| `extension-lifecycle-inputs.eOJPK0cV/result.json` | Five-unit pass: both process proxies, generated context receiver, `WebPage.cpp` and `WebLocalFrameLoaderClient.cpp`. Configuration and watched input hashes unchanged. |
| `extension-lifecycle-inputs.vUrCRgHt/result.json` | Four-unit pass: process event API, UI listener bookkeeping, generated C++ event binding and generated context receiver with listener messages. Configuration and watched input hashes unchanged. |
| `extension-lifecycle-inputs.GzhyJISm/result.json` | Corrected native wrapper compiles without warnings. |
| `extension-lifecycle-inputs.4nuJe3hO/result.json` | Namespace and runtime API units compile individually. Overall baseline fails because the wrapper used `nil`; this report remains failed. |
| `extension-lifecycle-inputs.h7hbuqs9/result.json` | Earlier page-hook run remained incomplete during the VM filesystem stall and is recorded as failed. The unchanged page-hook candidate subsequently passed in `eOJPK0cV`. |

The initial process-only IPC probe (`Ny4lCngt`) compiled, but warned about a
missing inline sender definition. The final process/page pass includes the
sender-inline header and has no such warning. Aggregate evidence is
`.vm/extension-native-bindings-results.json`. The API undefined-symbol inventory
is `.vm/extension-bindings-api-undefined.txt`; it is not a link pass.

The lifecycle probe's `--regenerate-ipc` option snapshots the actual configured
list of 186 receivers and runs WebKit's own generator with the configured
Python interpreter, producing 187 message/name headers in the private stage.
It records receiver, generator and output hashes and preserves validators.
`tools/generate-extension-bindings-candidate.py` uses WebKit's Perl generator,
the CMake IDL lists and optional candidate files. Its verified event run
generated all 74 files for 37 IDLs with 54 watched inputs unchanged.
`--generated-bindings` on the lifecycle probe validates this manifest and
stages the generated headers and requested C++ unit for native compilation.

Runtime/port methods and bindings, namespace API gates, storage, background
documents/workers, installation metadata, controller initialization, native
tab/window/UI delegates and permission transitions remain unfinished.
Listener delivery, callback mutation, context teardown and isolation still
need runtime tests against a matching feature-enabled engine and processes.

The promoted engine patch is
`1daf0679f17993a8988c64dcd33fa4e04e8ea4cb12e29a79f4a9866849a1d49f`.
A full attempt using this patch and three workers is recorded in
`.vm/modern-extensions-process-events-build.log`; it is still an integration
gate, not a completed build or runtime result. The earlier eight-worker
attempt's filesystem waits, compiler crash and saved VM checkpoint are
documented in [VM.md](VM.md).
