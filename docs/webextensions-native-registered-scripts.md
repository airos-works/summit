# Native restoration of registered scripts

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

Restoring saved registered scripts now uses common C++ and a JSON parser.
The parser preserves ordered file lists, optional fields, explicit false
values, frame choices, injection time, world and CSS origin. Initial
registration requires match patterns and a CSS or JavaScript file, while
partial updates keep omitted values absent. Malformed known fields reject
the whole batch. Unknown fields are ignored; resource names and match
patterns receive their semantic checks in the context's injection builder.

The injection builder and registered-script clearing also move out of Cocoa.
The builder checks duplicate IDs, reads resources and validates supported
match patterns before returning content data. Cocoa API callers retain a
small NSString error adapter. The restore path uses the existing SQLite
store and adds content only after both parsing and building succeed.

Each restore attempt has a generation number. Unload and script-state reset
invalidate pending reads; callbacks also recheck that the context is loaded
and still has scripting permission before using their result.

Evidence:

- `.vm/extension-registered-script-tests.rVTRE5HS/result.json`: all 46 native
  parsing assertions pass with normal exit, unchanged input/source/library
  hashes and no fresh debugger event. Tests include registration/update
  differences, malformed field types and enum values, full-batch rejection,
  ordered paths, Unicode and embedded NUL preservation for later resource
  validation. No WebCore or WebKit library is linked.
- `.vm/extension-lifecycle-inputs.ic1Yi12M/result.json`: the final parser and
  common context compile with both extension features enabled. This probe
  fails overall because the loader was missing its nested-enum type alias.
- `.vm/extension-lifecycle-inputs.eiCCfwfp/result.json`: the corrected common
  scripting unit compiles. All three selected source hashes and the changed
  context/parser headers match the final candidate, with unchanged native
  input and configuration snapshots and regenerated IPC.
- `tools/test-engine-extension-registered-scripts.py` reproduces the parser
  checks; the lifecycle compile tool accepts both new translation units.

These tests do not run SQLite restoration, asynchronous invalidation,
resource validation through this builder, or script injection. Cocoa/Xcode
compilation has not been run. The public scripting API bindings and native
tab injection remain incomplete, as do install metadata, DNR state/rules,
controller setup and browser attachment. No extension has run in the preview.

Promoted patch: `1d075f46ad782a384ae548bd1a2f1b390ace18e0ea342e8ea44ace3322cb0543`.
Probe baseline: `2ea935a16cbd980a09e5bc20229863c257a22e09e2fc48c29afcc8837d7936b6`.
Aggregate: `.vm/extension-scripts-native-results.json`.

## Shared script ownership

The registered-script allocator and its seven ownership/parameter methods
now live in `WebExtensionDynamicScripts.cpp`. Their bodies are unchanged
from Cocoa, including removal from every user-content controller. The
document identifier helper also moves unchanged into common utilities.
The old definitions are removed, and the new source is listed in Sources
and the Xcode project.

`.vm/extension-lifecycle-inputs.F2WTQUFt/result.json` records successful native
compilation of the ownership, utilities, scripting and context units, with
both extension features enabled and regenerated IPC. Native input and
configuration snapshots remain unchanged. This is compilation evidence;
it does not exercise script removal, live frame identification or injection.
Cocoa compilation has not been run.

Extraction: `.vm/extension-script-ownership-extraction.json`.
Promoted patch: `d5c9a8c180f514ddebb4d39392d4df6b08873769d9e3f733c82b615afb0d6aa8`.
Probe baseline: `6338f83f047dcaa3b58be944341804d64cb421d092305cbd7b0cffeb083d6069`.
