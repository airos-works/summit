# Native permission-change handling

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

Haiku now implements both permission-change callbacks used by the common
extension context. Named permission changes broadcast the current grants to
extension processes, update clipboard-write preferences on extension pages,
and queue added/removed events for grants and revocations. Host-pattern changes
clear cached permission decisions, refresh CORS exemptions, update injected
content and schedule URL/title updates for matching tabs.

Native background pages already participate in the controller's page set:
WebPageProxy registration resolves both strong and weak extension controllers.
Their required extension base URL selects them for the existing page updates.

Permission events capture the context's per-load privileged identifier and
check it again after a background wake. Tab changes coalesce for 25 ms, clear
the accumulated change flags before further event work, and verify the retained
tab's identity, open state and private access at both delayed stages. Tab URL
and title payloads are read when the event is sent, using current permissions.
Empty change notifications default to the tab's supported metadata rather than
retaining a previous URL/title snapshot.

`setNativePermissionChangeHandler` installs an optional main-thread host
observer. Notifications own copies of their permission and pattern sets and
run asynchronously. Replacing or clearing the observer invalidates its queued
notifications, including when the same callback object is later installed
again. A retained SharedTask keeps a callback alive if it replaces itself.
This is a C++ engine interface; the public SDK and extension UI do not yet
register an observer. It does not implement permission prompts or persist the
browser host's grant policy.

**Four native translation units compile** with both extension features enabled:
the new permission unit, common context, native background page code and the
generated context receiver. Final source/header hashes and the unchanged native
configuration are recorded in:

- `.vm/extension-lifecycle-inputs.WWVOawf7/result.json`
- `.vm/extension-native-permissions-validation.json`

No permission transition, observer callback, tab event, background wake, IPC or
browser runtime was executed. These are compilation checks, not proof of runtime
revocation behavior. The JavaScript Permissions namespace, native request prompt
and complete tab-event API still need their ports. Cocoa implementations remain
in place and were not compiled by these checks. No extension has run in Summit.

Promoted patch: `61626a3e1a2fcb8cf9b87008c6543921fa3b338666873e55df51254c11dbeaf5`.
Probe baseline: `a2a94c217a4b41946fcaf638ff43a297e4a23723b6ab92c148fa88d0f20ae82b`.

The full feature-enabled build of this patch reached the WebKit shared-library
link. Both permission-change callbacks resolved; the link failed with 21 missing
symbols, down from 23, and none newly introduced. Evidence:
`.vm/modern-extensions-native-permissions-build-result.json` and its recorded
build-log hash. This build predates the later native action-state patch.
