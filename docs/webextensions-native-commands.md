# Native extension commands

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

Haiku implements the command model, `commands.getAll()`, Firefox-style
`commands.update()` and `commands.reset()`, and both command event dispatchers.
The namespace and generated JavaScript bindings now expose these native
handlers. This brings the C++ binding set to **15 of 37 extension interfaces**.
The API also permits an empty `getAll()` query from a privileged extension.
Update/reset are guarded to Haiku; the Cocoa implementation keeps its existing
API surface.

## Shortcuts and saved settings

The native parser maps `Ctrl` to Control. It accepts the default manifest
shortcut as a string or an object with a `default` string. Haiku uses that
platform-neutral default, rather than selecting a Mac or Linux override.
Manifest function keys span F1–F12; updates also accept F13–F19. Function keys
can stand alone, and media keys require no modifiers. Duplicate modifiers,
missing key components, embedded NUL, invalid named keys, inappropriate Mac
modifiers and unsupported combinations are rejected. Formatting normalizes
modifier order and letter case. Incomplete combinations made through the
individual native setters appear unassigned.

The shared update parser distinguishes omitted fields from empty strings.
A description-only update preserves a saved shortcut; `shortcut: ""` stores an
explicit clear. State preparation deep-copies the existing JSON object and
preserves unrelated settings. The context validates the shortcut, saves the
prepared state with the existing atomic native state writer, and then changes
the live command. A write failure reports an API error without applying the
new command state in memory. Reset removes only that command's overrides and
restores its manifest values. Overrides for removed commands cannot create
new commands. MV3 action settings inherit `_execute_browser_action` unless
`_execute_action` has its own settings; updating/resetting the latter removes
the older key to prevent it resurfacing.

The intended API contracts follow [MDN's command manifest documentation](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/manifest.json/commands),
[`update()`](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/commands/update),
and [`reset()`](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/commands/reset).
Persistence of overrides and migration of action settings were checked against
[Mozilla's ExtensionShortcuts implementation](https://github.com/mozilla/gecko-dev/blob/master/toolkit/components/extensions/ExtensionShortcuts.sys.mjs).
Compatibility is still incomplete: whitespace-normalized shortcut spellings,
shortcut conflict management, global shortcuts and shortcut settings UI need
further work.

## Events

Native setter changes coalesce until the next main-loop dispatch. Returning
to the previous shortcut produces no change event. Deferred deliveries capture
the per-load context identifier and recheck command object identity, so unload,
reload or command-cache reset cancels stale notifications. The background wake
callback captures each committed shortcut transition rather than reading a
later value after another update.

The ordinary command event rejects action/sidebar command identifiers. Action
shortcuts must invoke their associated UI action. Tab identity, liveness and
private access are checked before and after waking background content; tab
parameters are produced after those checks. The process-side dispatcher
supplies the existing user-gesture scope and native tab conversion. It snapshots
listeners, creates a separate tab object for each listener, roots JavaScript
arguments across allocations, and passes `undefined` when no tab is supplied.

**Summit's keyboard handling does not yet call this engine command path.**
Action popup/click routing and the extension host SDK still need integration.
No physical shortcut, background command listener, promise completion or
extension-context state write has been exercised in a running extension.

## Verification

The native helper runs the actual shortcut and state preparation sources with
the frozen JavaScriptCore/WTF and ICU libraries. **161 checks pass** in QEMU,
including explicit clears, JSON reload, partial updates, reset isolation,
malformed settings and action migration. Its input/library integrity checks
and crash-log audit pass. It does not link WebCore or WebKit and does not call
the context or disk writer.

- `.vm/extension-command-settings-tests.S5jEkeKF/result.json`
- `python3 tools/test-engine-extension-command-settings.py --overlay .vm/extension-commands-candidate`

The extension IDL generator now handles platform guards on operations in
function bodies, static function tables and dynamic property getters, alongside
its existing declaration/name guards. Conjunctions preserve feature-expression
precedence. **92 checks pass** using actual Perl generation and C++
preprocessing across Haiku/Cocoa platform selections and both feature flags.
This checks binding exposure, not a Cocoa engine build.

- `.vm/extension-binding-platforms-vc560da7/result.json`
- `.vm/extension-bindings-generated-8yqkhopq/binding-generation.json`
- `python3 tools/test-extension-binding-platforms.py --generated-bindings .vm/extension-bindings-generated-8yqkhopq`

The generation tool can stage candidate generator/attribute files, so these
changes are checked before the engine patch is promoted.

Twelve distinct native integration units compile against the final sources:
the shortcut/settings helpers, common and native command models, context
commands/state loading, manifest parser, process API and namespace, both
JavaScript bindings, and the regenerated UI message receiver. The audit matches
individual compiled source hashes to the candidate and checks the generated
receiver in the guest:

- `.vm/extension-commands-validation.json`
- `.vm/extension-lifecycle-inputs.wiC1XvxT/result.json`
- `.vm/extension-lifecycle-inputs.y1uaGCbd/result.json`
- `.vm/extension-lifecycle-inputs.OFsxV8RX/result.json` (the settings unit passed;
  this earlier probe's process API failed because it used old IPC headers)
- `.vm/extension-lifecycle-inputs.FSc6bpUh/result.json` (process API and regenerated receiver pass)
- `.vm/extension-lifecycle-inputs.vO5WCxrd/result.json` (final namespace and context handlers pass)

The promoted patch is
`4665ca569eb1404566c3f87c095450f66ac374f3226af0fa38c1746b364cd855`.
Patch application and source-delta whitespace checks pass.

The full native build compiled the command sources, regenerated bindings and
IPC, and reached the WebKit shared-library link. It failed with **15 unresolved
symbols, down from 17**. Both command event dispatchers are resolved, and no
new missing symbols were introduced. The remaining work is the DNR loader and
14 other event dispatchers. This is not a successful extension-enabled engine
build or an extension runtime test.

- `.vm/modern-extensions-commands-build-result.json`
- `.vm/modern-extensions-commands-build.log`

Both native source trees were verified at the promoted patch. The isolated
baseline retains extensions disabled; the full build used extensions enabled.
