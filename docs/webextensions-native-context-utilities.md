# Native context permission and rule-path utilities

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

`hasActiveUserGesture` and `clearUserGesture` move from the Cocoa context to
common C++ without changing their bodies. Clearing a gesture still removes
the tab's temporary match pattern and forwards the permission-removal
notification when a pattern existed. The existing loaded-context checks
remain.

Haiku now implements `declarativeNetRequestContentRuleListFilePath`. It caches
the rule-file path under persistent extension storage or a private temporary
directory for nonpersistent storage. A failed directory request returns a null
path, so callers cannot accidentally use a relative rule filename. Cocoa keeps
its original path method and named-directory API.

`.vm/extension-lifecycle-inputs.OMvJ6ZT5/result.json` records both final context
units compiling natively with both extension flags enabled and regenerated
IPC. Native configuration and inputs remain unchanged, and the source hashes
match the promoted files. `.vm/extension-context-utilities-extraction.json`
records the unchanged gesture method bodies.

The first path attempt used an unavailable prefix argument on Haiku and failed
compilation (`ZTGQZjxV`). The intermediate common zero-argument path compiled
on Haiku (`kzlnNjpB`), then moved to the Haiku file to preserve Cocoa's distinct
return type and existing implementation. Only the final two-unit result above
validates the promoted source.

These are compile checks. No live tab gesture, permission notification or
compiled network-rule lookup was exercised. Native permission notifications,
tab/action delegates, network-rule translation and loading remain incomplete.

Promoted patch: `55798afee3c77e27b9dfc8ab1d46e7833d64568b5b6581a91fab79dfa6f3d5da`.
Probe baseline: `b330520c856f6a9a46eb9e787cba7acdd119772e062f68f96c06aed7c1d350a6`.
