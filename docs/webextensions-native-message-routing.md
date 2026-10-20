# Native extension message routing

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The UI runtime handlers for background-page lookup, reload, internal
messages/connections, page-to-extension messages/connections and startup/
install events now live in common `WebExtensionContextAPIRuntime.cpp`.
The corresponding six runtime IPC messages and process-side sends are
available outside Cocoa, retaining their `isLoaded` message validators.

The extraction preserves the existing cross-extension rejection, sender tab
lookup, `externally_connectable` URL matching, host permissions, delayed
replies for absent or inaccessible destinations, listener-world selection,
background wake-up sequence and first-reply aggregation. Content-script
and web-page gestures remain restricted. Connection replies retain port
listener counts and queued-message/disconnection ordering. These policies
were inspected in source; their native execution is not yet tested.

Outgoing port message routing now also lives in common C++. The Cocoa
native-application branch calls a small Cocoa adapter; native-application
messaging and options-page UI still require platform implementations. The
new common runtime source is in `Sources.txt`, and the moved implementations
were removed from their Cocoa files. Cocoa/Xcode compilation has not run.

Evidence:

- `.vm/extension-lifecycle-inputs.40FjtxVO/result.json`: all four actual units
  compile: UI runtime, UI port delivery, process runtime, and the regenerated
  UI context receiver. Both extension features are enabled in the isolated
  configuration. Native input and engine configuration hashes are unchanged.
- `tools/test-engine-extension-lifecycle-compile.py` stages the candidate
  headers and messages, invokes the configured upstream IPC generator and
  compiles the resulting receiver with the handlers.
- `.vm/extension-routing-native-results.json` records unit hashes and exact
  baseline/promoted patch provenance.

This is compile coverage for both ends of the message routes. No messages
were delivered through a running extension. Native background-page ownership,
tab/window delegates, browser attachment and a fully linked feature-enabled
engine remain required for that test. The preview still has extensions off.

Promoted patch: `e3a4cf2b5baf68e779cb6cb3fcd8d3bf38f2e242f7aacd7a96645971efb65ea8`.
Probe baseline: `23064dc4cd0304e24b9c5924d1aa7238c727d76439502b37887affdc1162516c`.

Subsequent native page ownership and UI controller attachment compile
results are recorded in [background evidence](webextensions-native-background.md).
