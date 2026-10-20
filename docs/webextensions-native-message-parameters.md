# Extension message metadata

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

Runtime sender conversion now lives in common C++ and uses a new
`JSWebExtensionTabParameters` helper. The helper creates the JavaScript tab
object directly, preserving absent optional fields and explicit false
values. It maps tab/window identifier sentinels, the unknown-index NaN,
URLs, titles, active/selection/pinning/audio state, load status, private
browsing and reader state. String fields use the length-based UTF-16 bridge.

The common sender conversion preserves the upstream rule that a frame ID
is present only with tab metadata. Valid URLs retain origins computed by
WebCore's `SecurityOrigin`; extension and document IDs remain optional.
Cocoa's dictionary-based tab converter remains available to other Cocoa
APIs. The common source list includes the new helper and the Cocoa project
references its header. Cocoa/Xcode compilation has not been run.

Tab dimensions remain Cocoa-only, matching the existing shared parameters
and IPC serializer. Native tab dimensions require those fields, their
serializer and the native tab delegate to be ported together. This change
does not alter the native tab-parameter IPC layout.

Evidence:

- `.vm/extension-tab-parameter-tests.C4XlwXFB/result.json`: 27 native
  assertions pass, normal exit 0, unchanged sources/configuration/libraries
  and no fresh debugger event. The checks cover plain JavaScript objects,
  missing and false properties, numeric/sentinel identifiers, zero and NaN
  indices, null-present strings, URL query/fragment retention and titles
  containing NULs and unpaired UTF-16 surrogates.
- `.vm/extension-lifecycle-inputs.LJmoknzV/result.json`: the new helper,
  common runtime sender conversion and port unit compile with both extension
  features enabled and regenerated IPC headers. Native configuration and
  input snapshots remain unchanged.
- `tools/test-engine-extension-tab-parameters.py` runs the helper against
  frozen JavaScriptCore/WTF and private ICU, without linking WebCore or
  WebKit. It does not run the sender converter or create a browser tab.

This establishes tab-conversion behavior and sender/port compilation. It
does not establish metadata delivery through IPC or extension execution.
Native UI routing, background lifecycle, browser attachment and the full
feature-enabled engine closure remain necessary.

Promoted engine patch: `23064dc4cd0304e24b9c5924d1aa7238c727d76439502b37887affdc1162516c`.
The probes used baseline `afeb9db214b575cddb072d8f65e890cb1cb10d0508b6a55bae32a00089e1074e`
with isolated candidate sources. Aggregate: `.vm/extension-parameters-native-results.json`.

Subsequent UI routing extraction and compile evidence are recorded in
[message routing](webextensions-native-message-routing.md).
