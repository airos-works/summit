# Native extension port bindings

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

This work ports the connection API and ownership bookkeeping to C++. It does
not establish a linked extension engine or working browser extensions.

`WebExtensionAPIPort.cpp` now contains the former Cocoa port implementation:
channel lookup, registration/removal, sender/error access, message sending,
disconnect handling, listener creation and process-side event dispatch. The
IDL uses `UseCPPAPI` and `RaisesStringException`; CMake, the Cocoa makefile
and Xcode generated-file reference agree on `JSWebExtensionAPIPort.cpp`.
Eight of the 37 extension interfaces now use the C++ generator route.

Messages are deserialized through the existing C++ JavaScript wrapper in
each listener's context. Listener vectors are copied before invocation, and
the per-listener user-gesture indicator is retained. Finalization marks a
port disconnected before removal, preserving the prohibition on calling
JavaScript during garbage collection. Removal also checks that this port
still belongs to the channel before sending `PortRemoved`, so finalization
after an explicit disconnect cannot decrement another listener's count.
That finalization path still needs a real JavaScript runtime regression test.

The namespace's runtime getter and the delayed disconnect of quarantined
web-page ports no longer depend on the Cocoa platform gate. Other runtime
IPC operations still need their native implementations.

Fourteen UI-context methods now live in `WebExtensionContextAPIPort.cpp`:
connection/page counting, queue bookkeeping, incoming messages/removals and
disconnect-event routing. Incoming messages preserve the upstream rule that
web pages and content scripts cannot propagate user gestures to extension
pages. `PortPostMessage` and `PortRemoved` keep their `isLoaded` validators.
Native-application message delivery and outgoing message routing remain in
the Cocoa implementation and still require a native port.

The extracted `WebExtensionPagePortMap` corrects two ownership errors in the
previous code. Registration now records the full listener count for a page,
and removing the last listener on one channel retains other channels owned
by that page. Taking the page's map supplies the full remaining counts to
the existing disconnect loop. Global channel counts and queued messages
remain managed by the context.

The native ownership harness runs the production helper with the frozen
WTF/JavaScriptCore dependency. It does not link WebCore or WebKit or create
an extension context. All 22 assertions pass, including multiple batches,
independent pages/worlds/channels, repeated teardown and registration after
unload. An isolated mutation restoring the two previous behaviors fails
nine assertions and exits normally with status 1, confirming that these
regression cases distinguish the fixes. Both runs retain unchanged source,
configuration and library hashes and report no fresh debugger event.

Evidence:

- `.vm/extension-page-port-tests.09yDtO60/result.json`: 22 native checks pass.
- `.vm/extension-page-port-tests.OujsNiIx/result.json`: expected negative
  mutation, 9 failed assertions; this is not a production pass.
- `.vm/extension-bindings-generated-blaaus11/binding-generation.json`:
  all 37 interfaces generate through the locked upstream Perl generator.
- `.vm/extension-lifecycle-inputs.igHt8Oh8/result.json`: the initial port API,
  UI bookkeeping, generated JavaScript port binding and both IPC receivers
  compile. This run precedes the ownership and finalization fixes.
- `.vm/extension-lifecycle-inputs.qzcFTS9P/result.json`: six final port,
  namespace, runtime, context and receiver units compile after the fixes.
- `.vm/extension-lifecycle-inputs.ei6PBBNT/result.json`: the final UI unit
  also compiles after moving disconnect-event routing from Cocoa.
- `tools/test-engine-extension-page-ports.py` runs the ownership assertions;
  `tools/test-engine-extension-lifecycle-compile.py` stages the actual engine
  units and regenerates isolated IPC inputs.

The remaining dependency closure includes the sender/tab conversion,
runtime reply callbacks, native-application messaging, UI message routing,
background pages, and browser controller attachment. Successful compilation
does not demonstrate extension installation, connection establishment,
cross-process message delivery or Chrome/Firefox/Safari compatibility.

Promoted engine patch: `91b1db7aefd1bc6212d452e2eeb3871e99ecb23febee1d8ea6472eca89d5b02f`. Native compile probes used the configured
`1daf0679f17993a8988c64dcd33fa4e04e8ea4cb12e29a79f4a9866849a1d49f`
baseline with isolated candidate sources and regenerated IPC headers. The
ongoing full build remains frozen on that baseline. Cocoa/Xcode compilation
has not been run; its project references and retained definitions were
checked during the extraction. Aggregate: `.vm/extension-port-native-results.json`.

The subsequent [message reply port](webextensions-native-message-replies.md)
implements the runtime reply callback helper and common dispatcher listed
above. Its 51 native JavaScript assertions are separate from these ownership
checks; the remaining sender/UI/background dependencies are still open.

Subsequent UI routing extraction and compile evidence are recorded in
[message routing](webextensions-native-message-routing.md).
