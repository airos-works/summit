# Native extension tab, window and frame queries

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

Haiku extension contexts now map registered browser pages and windows to their
normal per-extension identifiers. Each tab holds a weak WebPageProxy reference;
each window holds its registry identifier. Lookups require a loaded context,
matching context-map identity, live window messenger, registered page membership,
a nonclosed page and the page's current controller. Stale wrappers are pruned at
query boundaries. Retaining a wrapper through context unload does not preserve
its validity after reload.

The registry's SDK `window_identifier` is a native host identifier. It is separate
from the per-extension window identifier serialized in extension API responses.
The native host identifier must not be passed to `browser.windows` as its ID.

Tab order, active selection, focus and window order come from the shared browser
registry. Titles, URLs and loading state come from the actual PageLoadState.
Private status comes from the actual website data store. Extension access checks
hide private objects when private access is denied; URL/title metadata remains
subject to WebKit's permission checks. Background-page current-tab lookup uses
the frontmost active tab only when extension-view lookup is explicitly allowed.
Popup and sidebar associations still need native host integration.

The two navigation query handlers now request WebPageProxy's actual frame tree.
Traversal preserves document order and actual parent relationships, maps the
main-frame sentinel, includes document UUIDs and error state, and filters each
frame URL through current permissions. Single-frame responses omit `frameId`, as
in the upstream handler. Missing tabs, main frames and requested frames produce
errors. Before returning an asynchronous result, handlers recheck navigation
permission, context-map identity, page validity and private access. Frame trees
and URLs are not synthesized from the browser registry.

This implements host lookups and supported metadata, not the complete tabs or
windows namespaces. Native mutation operations, pinning, reader mode, geometry,
window state and tab/window event delivery are unfinished. Optional unsupported
metadata is omitted. No success callback is supplied for an unimplemented action.

**Eight final units compile** with both extension features enabled, regenerated
IPC and matching bindings. Records:

- `.vm/extension-lifecycle-inputs.RVxKgKI7/result.json`: tab and window adapters,
  common context, runtime handler, navigation permission validator and generated
  context receiver. Its two earlier context adapter compiles are superseded.
- `.vm/extension-lifecycle-inputs.SFhEdEgb/result.json`: final context lookup and
  frame-query adapters, including map-pruning and reply-permission checks.
- `.vm/extension-native-tabs-validation.json`: final source/header hash audit,
  enabled-feature configuration and unchanged native input checks.

These are compile checks only: no context, tab wrapper, frame-tree request, permission
revocation race, IPC or extension runtime has been executed for this change.
The browser SDK integration has also not yet run in the feature-enabled engine.

Promoted patch: `e9e643ec3a02d6b3f73f386db7b4a5e05eda99c45832c506dc1a34ce6db43502`.
Probe baseline: `7d5fe9b8cc4f898dae1bcda278c6468dc4eee5cedd4904d0b146d40f230188ce`.
