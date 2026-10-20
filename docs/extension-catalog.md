# Installed extension catalog

`src/core/ExtensionCatalog` provides the filesystem storage needed by Summit's
native installer and startup loader. It is included in `summit_core` and works
on both the host and Haiku. The modern extension-enabled browser uses it for
automatic startup and its [native installer and manager](modern-extension-manager.md).

An `ExtensionCatalog` owns a dedicated profile directory containing
`catalog.json` and a `packages` directory. Each installation records its stable
identity, display name, version, approved resource fingerprint, owned package
name, enabled state, and explicit file/private-access flags. Since #104 it
also records where updates come from (`source`: `chrome` or `firefox`, and the
manifest's `update_url`), the `auto_update` switch, when the extension was
`updated` and `checked` (seconds since the epoch), and the required
`permissions` and `origins` the user approved. These fields are optional:
older catalogs load with automatic updates on, and the controller fills them
in at the next start. Runtime permission
grants remain in WebKit's saved state; the catalog does not grant permissions.
Fingerprints retain their exact hexadecimal spelling, including the uppercase
digits returned by WebKit's native digest formatter.

`Stage(source, error)` creates a private owned copy of an archive or directory.
The returned `StagedExtensionPackage` removes that copy on destruction unless
`Install` successfully commits it. Archive copies are limited to 128 MiB;
directory imports to 256 MiB total, 64 MiB per file, 10,000 entries, and 32 nested
levels. Directory links are preserved without copying their targets. WebKit's
subsequent preparation step must validate containment and materialize accepted
links. Special filesystem entries and imports into their own storage tree are
rejected.

The application installer sequence is:

1. Stage the selected package on a worker.
2. Pass its owned `Path()` to `BWebKitContext::PrepareExtension`.
3. Present the returned manifest requirements for consent, retaining the
   preparation token and fingerprint.
4. Load the approved snapshot with explicit grants and commit its installation
   record. If catalog persistence fails after loading, unload the runtime.
5. On later startup, prepare each enabled stored package and use the recorded
   fingerprint with `BrowserStartup`. WebKit checks saved resource identity
   before restoring grants.

`Install` refuses duplicate identities and stages belonging to another catalog.
It does not independently validate a manifest, authenticate package signatures,
or verify the caller-supplied fingerprint. These responsibilities remain with
package preparation, approval, and loading. The application must serialize
catalog mutations and coordinate runtime changes with metadata writes.

The catalog validates every record before replacing its file. Writes use a
private temporary file, complete writes, `fsync`, and rename. A failed write
keeps the prior catalog intact. Reads are bounded to 4 MiB even if a file grows
while being read; malformed, duplicate, unsupported-version, or unsafe-path
records cause an error without replacing the caller's current list or rewriting
the file. `SetEnabled` persists an existing installation's enabled state.
`Forget` removes only its record; `RemovePackage` then deletes a package no
record names. `Modify` changes a record's update details and switches (never
its identity, package or order), and `Replace` commits a new version of an
installed extension: the record takes the staged package and the new name,
version and fingerprint, and keeps its order. When the user removes an
extension, the manager unloads it, forgets the record, has the engine erase
its storage and website data (`BWebKitContext::RemoveExtensionData`) and then
deletes the package. See [extension-fixes-2026-10-20.md](extension-fixes-2026-10-20.md).

The host core suite passes, and `ExtensionCatalogTests.cpp` passes 57 checks in
Haiku. The native run has unchanged input hashes and a clean crash-log interval:
`.vm/extension-catalog.UuD5DdlP/result.json`. A separate integration test seeds
approval through the public SDK and launches the actual browser twice. It
verifies automatic activation, saved permissions and extension storage across
process exits; see [startup behavior and evidence](modern-extension-startup.md).

```sh
cmake --build build-host
ctest --test-dir build-host --output-on-failure
python3 tools/test-extension-catalog-in-vm.py
```
