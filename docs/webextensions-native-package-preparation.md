# Native extension package preparation

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The public `BWebKitContext` preparation API separates filesystem work from WebKit
object creation. A serial worker extracts ZIP/XPI archives or snapshots directory
packages. It copies and fingerprints the resources, then transfers an owned
snapshot with isolated string storage to the application RunLoop. The RunLoop
creates the `WebExtension` and retains it under an opaque, context-scoped token.
Preparing a package grants no permissions and loads no extension context.

`PrepareExtension(path, reply, identifier)` accepts an absolute UTF-8 path and a
live local messenger. Identifiers range from 1 through `UINT64_MAX - 1`.
`B_OK` means the request was submitted. The asynchronous
`B_WEBKIT_EXTENSION_PACKAGE_PREPARED` message contains `identifier` and `error`.
Successful replies include:

- `token`, `name`, `version`, `description`, and `fingerprint` strings.
- `manifest_version` as a double.
- Repeated recognized `permission` and `origin` strings. Origins include sites
  requested by content scripts.
- `manifest_json` preserves the complete parsed and
  localized manifest, including requirements outside WebKit's recognized
  permission set. Recognized permissions alone do not establish API support.

The token refers to the exact package that produced the metadata. Later edits
to the original directory do not change that retained package. This provides
the resource identity needed to keep eventual installation consent and loading
consistent.

`CancelExtensionPreparation(identifier)` sends a cancellation reply while the
worker finishes and releases its resources. A separate internal operation
number prevents an older cancelled worker from completing a newer request with
the same public identifier. Duplicate live identifiers are rejected. A context
allows at most eight in-flight preparations and retained snapshots; cancellation
does not release a worker slot until that worker finishes.

`DiscardPreparedExtension(token, reply, identifier)` releases a retained package.
Its optional `B_WEBKIT_EXTENSION_PACKAGE_DISCARDED` reply reports `B_OK` or
`B_NAME_NOT_FOUND`. A token cannot be discarded through another context. Context
shutdown cancels requests and releases retained packages. Before application
shutdown, callers must drain queued looper submissions and keep the context and
RunLoop alive until `HasPendingExtensionPreparations()` returns false.

## Native verification

The complete extension-enabled engine builds and links at patch
`2972cc15e98b23f6dd04186631cf3f4370fe3a7d3a573175c6fd126a3fbdd3ca`.
The public preparation fixture passes 63 native checks in QEMU. The same build
passes the extension storage regression (14 native and 10 JavaScript assertions)
and cookie regression (14 native and 28 JavaScript assertions). All three runs
preserved their source/library inputs, recorded clean crash-log intervals, and
drained their process groups without forced cleanup.

`EngineExtensionPackagePreparationTests.cpp` uses the public SDK and native
messenger replies. It covers directory and ZIP packages, invalid input,
duplicate IDs, capacity limits, cancellation followed by ID reuse, manifest
metadata, source mutation, context-scoped tokens, directory cleanup, and main
looper responsiveness while copying and hashing a 32 MiB resource. An explicit
10 ms `BMessageRunner` drives that check: Haiku rounds application pulse rates
down to 100 ms increments. Native testing also exposed a cleanup defect in
`std::filesystem::remove_all` with a trailing slash. Extension destruction now
removes the resource URL's final slash before deleting the owned package.

Evidence: `.vm/extension-page-view-tests-result.json`,
`.vm/content-rule-pipeline.9pbUaJrF/result.json`,
`.vm/content-rule-pipeline.IiFIAalz/result.json`, and
`.vm/content-rule-pipeline.w7pm6uEX/result.json`. The failed initial run is retained
at `.vm/content-rule-pipeline.cP7abb2i/result.json`; it timed out and required
fixture-group cleanup. The successful runs above use fresh stages.

```sh
python3 tools/test-engine-content-rule-pipeline.py --extension-package-preparation
```

The preparation API supplies the snapshot for [native activation and dedicated
extension views](webextensions-native-package-activation.md). The browser also
has [catalog and package staging storage](extension-catalog.md). Native installer
UI, startup integration, CRX/Safari packages, signature verification, and broader
extension API support remain unfinished.
