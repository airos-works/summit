# Native resource notifications for extensions

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

Haiku now marks loader requests when their page's actual extension controller has
loaded contexts. This activates WebKit's existing network resource notifications.
The page proxy forwards request, redirect, challenge, response, completion and
content-rule blocking notifications to the controller and extension contexts.
Controllers iterate a snapshot and skip contexts that have unloaded or moved to
another controller.

Contexts check current tab identity, private access, the webRequest permission,
the tab's host permission for subresources, and the original and target URLs.
Every queued background wake-up callback repeats those checks before sending
resource metadata to the extension process. A retained tab or context cannot
preserve an old permission grant. The existing event groups, redirect sequence,
blocked-load error and request-body handling remain in place.

The six controller and seven context methods now live in common C++ sources.
Cocoa's automatic permission prompt after extension Fetch/XHR CORS failures is
preserved under its platform guard. Haiku still needs native permission UI for
that behavior. No native prompt or substitute success callback is added here.

**Four native units compile** with both extension features enabled and matching
bindings/IPC: controller forwarding, context forwarding, WebPageProxy and
WebLoaderStrategy. `.vm/extension-lifecycle-inputs.OrKe8PEZ/result.json` records
the compiles and unchanged native inputs.
`.vm/extension-web-request-ui-validation.json` matches final source/header hashes
and records the six queued permission checks. The extraction record is
`.vm/extension-web-request-ui-extraction.json`.

These checks do not execute a network request, background wake-up, permission
revocation, private-tab transition, resource event or IPC. End-to-end extension
runtime remains unverified because the feature-enabled engine has not linked.
Cocoa compilation/runtime was not tested. The nonblocking-only behavior and
other [request-event compatibility limits](webextensions-native-web-request.md)
remain. Later [metadata fixes](webextensions-native-web-request-metadata.md)
improve status-line and parent-frame mapping.

Promoted patch: `e9fa1eff70595ba5e03552f0d8fd811084b56861e630853866ccce3e2361f756`.
Probe baseline: `c9587d431175525e047288be50e42c7b97fc365ad93831a1cf9e3c6a78073a71`.

A later source audit found the two content-rule-blocking definitions still in
the Cocoa files: the extraction had expected a blank line before their closing
platform guards. Those duplicate definitions are now removed. The source audit
confirms exactly one definition for all 13 moved methods and no stale Cocoa
resource IPC sends. Cocoa compilation remains unverified. Evidence:
`.vm/extension-web-request-cocoa-cleanup-validation.json`.
Cleanup patch: `a2a94c217a4b41946fcaf638ff43a297e4a23723b6ab92c148fa88d0f20ae82b`.
