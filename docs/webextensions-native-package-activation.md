# Native extension package activation

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

`BWebKitContext::LoadPreparedExtension` loads the exact snapshot retained by the
public preparation API. The host supplies its opaque token and
`BWebKitExtensionLoadOptions`, including a stable extension identity and the
resource fingerprint associated with the host's approval. Installation consent
and the persisted installed-package registry remain browser responsibilities.

The options distinguish three load purposes:

- `UserApproved` replaces both saved API permission grants and origin grants
  with the explicitly supplied lists. Empty lists remove prior grants.
- `BrowserStartup` restores saved grants only when the previously recorded
  resource fingerprint matches the prepared package. The supplied grant lists
  must be empty, and the browser context must be persistent.
- `BrowserUpdate` uses the same fingerprint gate and grant restoration, with
  the browser-update load purpose passed to the extension context.

An identity is limited to 255 ASCII letters, digits, and `.`, `-`, `_`, `@`, `{`,
or `}`; `.` and `..` are rejected. The identity is also a component in the
extension's storage path. A loaded identity cannot be replaced by a second load.
Requested grants must be covered by the package's declared required, optional,
or content-script permissions. Input is bounded before match-pattern parsing.
An unknown or unrequested grant causes rejection; it is not silently discarded.

`allowFileURLs` and `allowPrivateBrowsing` default to false. Private contexts
require explicit private access and `UserApproved` grants. Startup restoration
is not permitted in a private context. These flags are independent of the
permission and origin lists.

The method accepts a live local reply messenger and a request ID from 1 through
`UINT64_MAX - 1`. Its immediate `B_OK` means the operation was submitted. The
`B_WEBKIT_EXTENSION_LOADED` reply contains `identifier`, `error`, and the validated
`extension_identifier`; success also contains `fingerprint` and `base_url`.
Invalid options can fail before an identity is included. Success means the
controller loaded the extension context. Background execution follows
asynchronously. The prepared token is consumed after delivery of the success
receipt. A rejected load leaves it available for retry or discard.

`UnloadExtension(identity, reply, requestID)` disables an owned runtime and
reports `B_WEBKIT_EXTENSION_UNLOADED`, including its identity and status. A stale
identity returns `B_NAME_NOT_FOUND`. Unloading preserves stored extension data;
it does not uninstall a package. Context destruction unloads the runtimes owned
by its registry and releases their snapshots.

## Dedicated extension views

`BWebKitContext::CreateExtensionView(frame, name, identity, target, error)`
requires the application thread, a loaded identity owned by the context, and a
live local notification target. It returns a caller-owned native view configured
with the extension's required origin, CSP, controller, and privileged context
identity. The usual public view state and load-error notifications apply. The
view rejects top-level navigation outside its extension. Ordinary browser views
keep their ordinary configuration and cannot load private extension pages.

## Native verification

The complete extension-enabled engine builds and links at patch
`2972cc15e98b23f6dd04186631cf3f4370fe3a7d3a573175c6fd126a3fbdd3ca`.
`EngineExtensionPackageActivationTests.cpp` passes 47 native checks and 17
JavaScript assertions in three native extension-page rounds. The fixture uses
only the public context/view SDK and matches nonce-tagged title notifications.

It verifies initial approval, background execution, storage and cookies,
unload/startup restoration in the same browser process, and replacement of
saved grants by a fresh empty approval. It also checks invalid identity/grants,
snapshot isolation from source edits, token consumption, duplicate identity,
refusal to inherit approval for changed resources, view context ownership,
rejection of views for unloaded extensions, and the top-level navigation bound.

The same build passes preparation (63 native checks), storage (14 native and
10 JavaScript assertions), and cookies (14 native and 28 JavaScript assertions).
All four runs preserve input/library hashes, report clean crash-log intervals,
and drain their process groups without forced cleanup. Evidence:

- `.vm/extension-page-view-tests-result.json`
- `.vm/content-rule-pipeline.tKpYFHZ9/result.json` (activation)
- `.vm/content-rule-pipeline.9pbUaJrF/result.json` (preparation)
- `.vm/content-rule-pipeline.IiFIAalz/result.json` (storage)
- `.vm/content-rule-pipeline.w7pm6uEX/result.json` (cookies)

The preceding ordinary-view failure is retained at
`.vm/content-rule-pipeline.ND4AbW3h/result.json`; its diagnostic follow-up at
`.vm/content-rule-pipeline.8nZdZT6J/result.json` identifies the extension-resource
configuration check. The fix provides the required configuration without
relaxing that resource-access check.

```sh
python3 tools/test-engine-content-rule-pipeline.py --extension-package-activation
```

These tests do not yet prove browser restart persistence, private activation,
installation/update UI, signed package authentication, or complete Safari,
Chrome, and Firefox extension compatibility. Those remain part of the browser's
unfinished extension work.
