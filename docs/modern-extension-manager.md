# Native extension installation and management

In the modern extension-enabled browser, **Window → Extensions…** opens the
native manager. **Add extension…** opens Haiku's file picker for a CRX3/ZIP/XPI
archive or unpacked extension folder. Package staging runs on a worker, then
the public WebKit SDK validates and prepares an immutable snapshot.

Before code executes, the manager displays the localized name/version and
requested API and website permissions. It includes unknown manifest permission
names and separately lists optional permissions, which installation does not
grant. The review distinguishes verified CRX3 signatures from unverified ZIP/XPI/folder
packages, and explains that store provenance and compatibility are unverified. File access defaults to off; private browsing access is not offered
and remains off. Approval applies only to the current preparation generation,
token and resource fingerprint.

**Install** loads that exact snapshot with the explicitly approved required
grants. The app checks its receipt identity/fingerprint, commits the catalog,
and adopts the runtime into its startup/shutdown controller. Cancelling or
closing the manager abandons the current import. If catalog persistence fails
after a load, the installer unloads the temporary runtime. If that unload also
fails, it retains a visible session-only entry so shutdown still owns it.
These failure branches have code review coverage; persistence failure during
live installation still needs dedicated native fault-injection coverage.

**Disable** unloads before persisting the disabled state and keeps all of
the extension's data. **Enable** prepares the stored package again and
restores its existing approval only when the fingerprint matches. **Remove**
asks first, naming the extension, then unloads it, forgets the startup
record and deletes its package, unpacked copy, storage and website data
(`BWebKitContext::RemoveExtensionData`, #104); reinstalling it starts from
scratch. Shutdown waits for installer work and manager/file-picker window
destruction before releasing the shared WebKit context.

Extensions from the Chrome Web Store and Firefox Add-ons update themselves
(a minute after launch, daily, and with **Check for Updates**), keep their
settings and data, and wait for approval (**Update**) when a new version
asks for more access; each has an **Update automatically** switch and shows
when it was updated. **Keyboard Shortcuts…** opens the extensions' command
keys (#101). See [extension-fixes-2026-10-20.md](extension-fixes-2026-10-20.md).

For [verified CRX3 packages](extension-crx.md), the signer supplies the stable
identity. For other packages, declared `browser_specific_settings.gecko.id` or
`applications.gecko.id` is used as the stable identity. Otherwise a Chrome manifest `key` now derives
the matching Chromium ID from its decoded bytes. Packages without either
receive a local identity that remains in their installation record. Duplicate
declared identities are rejected before activation. See
[identity behavior and tests](extension-identity.md), including mixed-manifest
precedence. Safari bundle import, XPI signatures/store authentication,
remaining action APIs and broad extension API compatibility remain
unfinished. A lost SDK reply has no operation deadline yet; cancellation after
submitting a load also needs further native integration coverage.

Live [toolbar actions and real extension popups](modern-extension-actions.md)
are now connected to installed extensions. Their integrated tests also exercise
this manager's actual picker, consent, disable/enable and removal flows.

## Verification

Run against a frozen full-browser bundle, serially with other native tests:

```sh
python3 tools/test-modern-extension-startup.py --manager --bundle /absolute/guest/bundle
```

The native harness opens the actual browser menu, operates the real picker
through its native scripting interface, selects entries in its file list and
presses its controls. It then exercises consent, stale approval rejection,
cancel/reopen, closing during import, installation, duplicate rejection,
disable/enable, cancellation and confirmation of removal across three distinct
Summit processes. The last
process quits with both the manager and picker open. A generated fixture checks
real background execution, persisted storage and restored grants through HTTP
reports; optional `tabs` access must stay ungranted. The runner checks actual
kernel process-group membership, normal exits, new debugger events, and frozen
bundle/staged/host source hashes.

The current published stock R1/beta6 packages also pass the manager and action
fixture: [22 October acceptance](haiku-build.md#extension-acceptance-22-october-2026),
including clean shutdown after removal and with an open extension popup.

On `bundle-tz1tfyzh` with engine patch
`2972cc15e98b23f6dd04186631cf3f4370fe3a7d3a573175c6fd126a3fbdd3ca`,
the picker run in
`.vm/modern-extension-manager-a54470ada1af41861723f8a4/result.json`
passes **123 checks** (98 installation, 13 removal, 12 after removal). It
reviews XPI packages and installs a folder. Browser teams 32457, 32573 and
32634 exit normally; all owned groups drain without forced cleanup, the native
crash-log interval is clean, and input hashes remain unchanged. The earlier
71-check run supplied the picker's result directly and remains separately
recorded in `.vm/modern-extension-manager-b1765931eb029509c5d87bac/result.json`.

The subsequent XPI installation run in
`.vm/modern-extension-manager-bc0ba598f726fdca21ad40d9/result.json` also passes
all 123 checks. It installs the archive through the actual picker, restores it
in another process and rejects the same declared identity from a folder.
Browser teams 32713, 32829 and 32890 exit normally with drained groups, no
forced cleanup, a clean crash-log interval and unchanged input hashes. Both
portable CTest suites also pass after the app integration.

Preserved failed harness runs:

- `fc134763e26fddb590dfcfad` read `BTextView.Text` as a direct property. Haiku
  requires a byte-range specifier; the runner now reads its bounded text range.
- `df46f47866aa045948d42b11` queried the unsupported `history` permission and
  the fixture saved the resulting error. The optional-grant check now uses
  the supported `tabs` permission. This does not add history API support.

Those results are under `.vm/modern-extension-manager-<run>/result.json` and
remain failures. Passing fixtures establish these installation and lifecycle
behaviors, not general compatibility with browser extension stores or their
extension catalogs.
