# Native extension action state

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

Haiku now implements action objects for default, window and tab scopes. Labels,
badge text, enabled state, popup paths and custom icons follow their scope's
fallback action. Icon selection uses the existing native resource loader and
cache, at the port's current backing scale. The icon-variants feature remains
disabled on Haiku. The Cocoa action implementation remains in place.

Clearing customizations also resets unread-badge state. An empty badge does not
retain an unread indicator. Label fallback preserves the caller's choice about
empty labels across inherited actions. Blocked-resource counts clamp at zero
and saturate at the signed counter limit, avoiding signed overflow. The context
reads/writes the existing `DisplayBlockedResourceCountAsBadgeText` state key and
increments a tab's action only when that setting is enabled and the tab is live
and accessible.

Property changes coalesce for 25 ms and capture the context's per-load identity.
A native C++ host observer receives valid current actions after that delay.
Default changes also notify valid window/tab overrides that can inherit from
them; window changes notify the window action and its tab overrides. The target
list retains its objects before callbacks begin. Each delivery rechecks the
load, observer registration, scope identity and private access, so callbacks
can replace the observer, close tabs or unload the context during delivery.
The observer active when coalescing finishes is used; replacing it during a
delivery batch cancels the remaining callbacks in that batch.

This supplies engine state and an observer interface. The browser SDK and
toolbar do not yet register that observer. Native popup presentation and the
toolbar click route remain unfinished. The subsequent
[native action API port](webextensions-native-action-api.md) implements state
methods and event dispatch, with separate compile/helper evidence.
Popup-path metadata alone does not display a popup.

**Four final native integration units compile** with both extension features
enabled: action state, context action management, the common context and the
common declarative-rule API unit. Source and header hashes match the candidate:

- `.vm/extension-lifecycle-inputs.ioDHFx3o/result.json`: three passing units;
  its context-action unit failed reference-type deduction and is superseded.
- `.vm/extension-lifecycle-inputs.adyylV3F/result.json`: the corrected final
  context-action unit passes.
- `.vm/extension-native-action-validation.json`: the four final units and
  matching source/header hashes.
- `.vm/extension-native-action-extraction.json`: 26 upstream methods recorded
  before native adaptations.

No getter/setter, badge counter, inherited-state transition, observer callback,
icon decode, popup or extension runtime was executed. The compile results do not
prove those runtime behaviors. No extension has run in Summit.

Promoted patch: `dc4675d8cdc40b87b0e5134c6d09fb58c5f2dfc2880d968d3bc98f8dd2282475`.
Probe baseline: `61626a3e1a2fcb8cf9b87008c6543921fa3b338666873e55df51254c11dbeaf5`.
