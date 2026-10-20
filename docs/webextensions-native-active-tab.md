# Native temporary tab access and command routing

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

Engine patch `ccacaaa8748dde7ba4432cd53707f5731332951c7b8d727f82d0d45599ff3b5d`
connects native user gestures to temporary tab permissions and command dispatch.
The helper passes 61 checks in Haiku; nine connected engine translation units
compile. The first full build found a missing `haiku/` include path in the shared
tab header. With that include corrected, the full build reached the linker
without compiler errors. At that patch it still needed the native action entry
point and request-rule loader. **No extension has executed these paths, and the engine has not linked
successfully.**

A grant belongs to one extension's tab and uses its committed security origin,
including the scheme, host and port. It never uses a provisional address-bar URL.
Opaque documents and extension origins cannot acquire this grant; local files
also require the browser's file-access setting. The descriptor explicitly avoids
WebKit's default subdomain and HTTP/HTTPS wildcard expansion.

URL permission decisions require the same live, open and accessible tab, an
active `activeTab` permission, the current committed origin and the requested
origin. Current persistent denials override a prior invocation. Match-pattern
queries cannot consume an origin grant because patterns do not express its port
restriction. Global permission enumeration and persisted host grants are unchanged.
Tab metadata reads can use temporary access to that tab.

Same-origin navigation retains the grant. Cross-origin navigation, including a
port change or an opaque destination, revokes it. The native commit notification
now follows the UI page-commit message on the same connection, so revocation can
read the new committed origin. Close, stale tab removal, extension unload,
permission revocation and disabling applicable private/file access clear the
state. Removing a denial or restoring access settings does not restore the old
grant. These lifecycle connections compile; they have not run in an extension.

Temporary grants update only the affected tab's metadata. They do not pass through
global host-permission notifications, which would install content scripts in
other matching tabs. Automatic content-script injection following invocation and
extension-page fetch/CORS updates still require their own integration.

Native command dispatch validates the current command object and tab. A menu
command retains the clicked tab instead of resolving a different tab after a
focus change. Ordinary shortcuts resolve the actual frontmost window before
checking access, so an inaccessible private window cannot redirect the command
to another browser window. Trusted invocations call the new user-gesture path;
programmatic calls do not manufacture a gesture. Ordinary commands use the
existing background-wake/event dispatcher. Reserved action commands call
`performAction`; the [native popup bridge](webextensions-native-action-popup.md)
now compiles, but has not executed in an extension.

The intended temporary-access behavior follows [Chrome's activeTab reference](https://developer.chrome.com/docs/extensions/develop/concepts/activeTab).
Command/action separation follows [Chrome's commands reference](https://developer.chrome.com/docs/extensions/reference/api/commands?hl=en).
This work does not establish complete Chrome, Firefox or Safari compatibility.

## Evidence and limits

- `python3 tools/test-engine-extension-active-tab.py` runs the actual grant,
  URL and WebKit match-pattern code against frozen JavaScriptCore/WTF in Haiku.
  Its 61 checks cover origin boundaries, default ports, credentials, file gating,
  unsupported URLs, IPv4/IPv6/internationalized hosts, revocation and independent
  tab grant objects. Six source units compile without warnings; inputs and
  libraries remain unchanged and the native crash log is empty. The harness
  substitutes API object initialization with real JSC/WTF initialization and
  links neither WebCore nor WebKit. It creates no extension context or browser tab.
- Nine engine translation units compile in isolated stages with Extensions
  enabled: the grant helper, tab implementation, context, permission dispatch,
  tab registry, navigation event receiver, frame loader, command dispatch and
  menu caller. Their staged include paths did not expose the full build's missing
  header prefix. These checks do not prove runtime permission, lifecycle or IPC behavior.
- `.vm/extension-active-tab-tests.3ij0oXQS/result.json` records the helper run.
- `.vm/extension-lifecycle-inputs.4gUfS3Pd/result.json` records seven native units.
- `.vm/extension-lifecycle-inputs.xFF5jmhE/result.json` records the command/menu units.
- `.vm/extension-active-tab-current-validation.json` records current sources and configuration.
- `.vm/extension-active-tab-commands-validation.json` records the original 13-file audit.
- `.vm/extension-active-tab-header-promotion.json` records the one-line include correction.
- `.vm/modern-extensions-active-tab-build-result.json` records the failed full build:
  99 occurrences of the same missing-header error; linking was not reached.
- `.vm/modern-extensions-active-tab-header-build-result.json` records the corrected
  full build: no compiler errors, two missing functions with seven linker references.
  The log is `.vm/modern-extensions-active-tab-header-build.log`.

Native menu/toolbar/shortcut input, popup presentation, asynchronous lifecycle
checks in a running extension, background persistence and request-rule loading
remain part of the full browser goal.
