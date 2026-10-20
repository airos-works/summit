# Extension string preservation

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The common extension string bridge now copies UTF-16 code units directly
between WTF strings and JavaScriptCore strings. Its previous UTF-8 C-string
conversion truncated embedded NULs and replaced unpaired surrogates. The
length-based bridge preserves both, including data used by extension API
arguments, results and metadata.

`JSWebExtensionString.h/.cpp` contains the two conversions formerly defined
in `JSWebExtensionWrapper.h/.cpp`. A null native string still maps to an
empty JavaScript string. A null JavaScript string reference still maps to
a null native string, while a present empty JavaScript string maps to a
non-null empty native string. The new source is in the common source list,
and its header has a Cocoa project reference.

Native checks exercise both directions independently, inspect the resulting
JavaScript string characters, and evaluate a converted payload in
JavaScriptCore. The suite covers ASCII, Latin-1, other Unicode characters,
embedded NULs, paired and unpaired surrogates, empty/absent values, and a
sequence containing all 65,536 UTF-16 code-unit values.

Evidence:

- `.vm/extension-string-tests.uqozs3OI/result.json`: all 46 assertions pass,
  exit 0, unchanged source/configuration/dependency hashes and no fresh
  debugger event. The executable links frozen JavaScriptCore/WTF and private
  ICU, with no WebCore or WebKit library.
- `.vm/extension-string-tests.mQTWsGTG/result.json`: restoring the previous
  conversion in an isolated candidate fails 21 assertions and exits with
  status 1. This expected negative test is not a production pass.
- `.vm/extension-lifecycle-inputs.XKn7qxSg/result.json`: the helper, common
  wrapper and runtime unit compile with both extension features enabled.
- `.vm/extension-string-tests.8VoiTAUI/result.json`: the initial harness did
  not compile because its unqualified `toString(nullptr)` selected WTF's
  unrelated formatting template. The corrected harness qualifies the call.
- `tools/test-engine-extension-strings.py` runs the native string checks
  against the configured source manifest and frozen dependencies.

This is a string-conversion runtime result and an extension compilation
result. It does not demonstrate browser extension execution, metadata
delivery through IPC, or a linked feature-enabled WebKit build. Cocoa/Xcode
compilation has not been run.

Promoted engine patch: `afeb9db214b575cddb072d8f65e890cb1cb10d0508b6a55bae32a00089e1074e`.
The native probes used baseline
`fb28750f4eb0d93caf2c9190b2c09e152af29c914fd2f475bb94180e5a9dd90c`
with isolated candidate sources. Aggregate: `.vm/extension-string-native-results.json`.
