# Native extension package identity and install state

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

Haiku extension construction now creates an owned resource snapshot and records
its SHA-256 fingerprint. Directory loads copy their resources into a private
`WebKitPackage-*` directory. ZIP/XPI loads first use the existing extractor, copy
and fingerprint that extracted package, then discard the intermediate extraction.
Manifest parsing and subsequent resource reads use the accepted snapshot. Edits
to the original directory affect a newly constructed extension, while an existing
instance retains its copied resources. Its destructor removes that snapshot.

The snapshot walks entries in deterministic UTF-8 byte order, hashes directory
and file paths with explicit type and length fields, and hashes every file's
bytes. The format includes a versioned domain. Absolute source paths, timestamps
and permissions are excluded from the fingerprint. Empty directories participate
because their presence can affect resource discovery. Contained file and directory
symlinks are materialized as ordinary copied resources. Escapes, dangling links,
cycles and unsupported file types are rejected.

Source access uses descriptor-relative opens. Canonical symlink targets are opened
component by component beneath the retained source root with `O_NOFOLLOW`.
Reads check file sizes and metadata before and after copying, writes handle short
writes and flush errors, and a second source traversal verifies the content hash.
Observed source changes fail the operation and remove partial output. This is not
an atomic snapshot of a concurrently modified filesystem. Once accepted, the
browser reads its owned copy; changes to the original source no longer affect it.
The copy is owned by the browser process, not protected against other programs
running as the same operating-system user.

The snapshot uses the archive adapter's existing bounds: 10,000 entries, 64 MiB
per file, 256 MiB total copied file data, 1,024 path bytes and 32 path components.
Aliases count toward expansion limits. Copying and verification are synchronous;
the eventual installer should perform this work outside the UI event loop.

Native construction from in-memory resources copies caller-owned API::Data bytes
before computing a fingerprint. It retains the supplied string resource bytes and
clones the initial manifest JSON. The fingerprint is computed before lazy resource
caches are populated, and includes the original data/string resource distinction.
Memory resources have a separate versioned hash format from filesystem packages.
These hashes identify resource contents for update detection; they are not package
signatures or proof of publisher identity.

Native install decisions read the previous version and a versioned resource
fingerprint. A first installation schedules `install`; a changed version or known
changed fingerprint schedules `update`, including changes at the same version.
An unchanged package loaded again by the user schedules neither event. Legacy or
malformed fingerprint state clears cached listeners and rule-set state and requests deletion of
registered scripts, but does not manufacture a content-change event when no valid previous
fingerprint exists. The actual previous version is preserved when known; the
JavaScript event omits that field if the saved value is missing.

The browser host can call `setNativeLoadPurpose` before loading a context. Its
purpose is consumed once: `UserInitiated` is the default, `BrowserStartup` enables
startup for an existing installation, `BrowserUpdate` also selects the browser
update reason, and `PrivateBrowsing` suppresses installation/startup events.
An ephemeral controller datastore forces private-context behavior. A first
installation takes precedence over startup, and an extension update takes
precedence over a simultaneous browser update. This replaces the native use of a
five-second controller-age heuristic. The public SDK has not yet connected the
browser startup/update paths to this C++ interface.

The intended event distinctions follow the
[onInstalled](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/runtime/onInstalled)
and [onStartup](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/runtime/onStartup)
references. Context load rejects an unparsed manifest, missing version or missing resource
fingerprint before publishing its loaded state. Cocoa code continues to use its existing
bundle identity and lifecycle implementation; Cocoa was not compiled here.

**64 native helper checks pass** using the actual snapshot implementation, actual
PAL/OpenSSL SHA-256, WTF and the install-decision helper. The tests exercise copied
bytes, Unicode and binary names/data, creation-order independence, content changes,
retained old resources, symlink handling, FIFO rejection, ownership release and
cleanup, expansion limits, two independently computed SHA-256 vectors, and install
state transitions including private browsing and legacy/corrupt state.

Evidence: `.vm/extension-package-snapshot-tests.CTdRS8kL/result.json`.

The tests do not construct a WebExtension, launch a browser, execute a startup or
installed event, modify the real registered-script database, or exercise concurrent
source mutation. They do not prove snapshot construction can run in the eventual
installer's threading model. These helper checks preceded the full engine link.
The engine has since linked and the isolated native package/controller fixture
executes extension JavaScript on direct startup; see
[current runtime evidence](webextensions-native-runtime.md). Public SDK loading,
installer threading and actual installed/startup event behavior remain pending.

**Six final native integration units compile** with extension and content-rule
features enabled: the snapshot implementation, native and common WebExtension
constructors, native and common context, and the runtime API. Five final units
are in `.vm/extension-lifecycle-inputs.mxL4lgMB/result.json`; the snapshot unit
was recompiled after its filename-depth correction in
`.vm/extension-lifecycle-inputs.8xny7y3c/result.json`. The final hash audit is
`.vm/extension-package-snapshot-validation.json`.

The compile runner now includes staged native headers when it flattens native
source files. Earlier drafts failed on character/span types, the native include
path and a nonexistent controller accessor. An early runtime fixture also
truncated its NUL-bearing name with an ASCII literal; the final fixture uses an
explicit-length string. These failures are retained in the validation history.

Promoted patch: `341d7bd1bb1d8d5601a45cd83b1dc5af3eeb3b2f4039f7468d1b212ee32d6ef5`.
Probe baseline: `425e5c88a7cb5b221858163374430798efa40a166384bcde3f085be904e3df50`.

The subsequent full build of patch `341d7bd1...` reached shared-library linking
and failed with **17 missing symbols, down from 18**. The native
`determineInstallReasonDuringLoad()` symbol resolves and no new missing symbol
was introduced. This confirms full-build compilation, not extension runtime.
Evidence: `.vm/modern-extensions-package-snapshot-build-result.json`.
