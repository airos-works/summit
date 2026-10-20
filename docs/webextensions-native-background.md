# Native extension background pages

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The Haiku extension context now has an adapter for creating and owning a
background page through the existing native `WebView` page client. The
extension-only factory accepts a prepared `API::PageConfiguration` and
creates no native window or browser tab. The context owns both the page
proxy and its page client, closes the client during unload/destruction,
and clears page identity before close can complete a pending worker load.

Configuration uses the controller's data store, manifest-specific CSP,
required extension base URL, granted cross-origin patterns, related-page
process pool when available and background timer/process preferences.
Context-owned pages use a weak controller reference to avoid an ownership
cycle. Native page enumeration and cross-origin-pattern updates operate on
pages whose required base URL belongs to this extension.

The navigation client handles document completion, load errors and process
termination through a weak context reference. Navigation policy confines
the main background document to its extension origin. Worker completion
also checks current page identity before changing state. Background page
IDs are sent to the WebProcess for document backgrounds; worker backgrounds
use the existing `WebPageProxy::loadServiceWorker` entry point. Persistent
backgrounds have a process-restart path, while nonpersistent backgrounds
retain the permission-request, active-port and inspector unload checks.

Six UI-process `WebPageProxy` hooks now include Haiku: controller inclusion,
strong/weak controller initialization, page registration, controller lookup,
page removal and controller parameters in page creation. Page removal's
port cleanup and the existing Cocoa post-load event/task sequence now live
in common C++. The context registry removes entries on destruction in the
common non-GLib destructor. Cocoa/Xcode compilation has not been run.

Evidence (all native probes use both extension features in an isolated
configuration, with unchanged native input/configuration snapshots):

- `.vm/extension-lifecycle-inputs.6wzeu7mT/result.json`: the Haiku view unit
  passes. This first probe fails overall: duplicated staged/native
  `WebPageProxy.h` declarations and a configuration type name were corrected.
- `.vm/extension-lifecycle-inputs.H56E4BMD/result.json`: final common context
  and UI page proxy units pass. The background unit still fails on a missing
  process-pool include and an extra optional wrapper around a page ID.
- `.vm/extension-lifecycle-inputs.IDAa4FTY/result.json`: the corrected adapter
  and native context constructor pass. Subsequent callback-identity checks
  were tightened in the adapter.
- `.vm/extension-lifecycle-inputs.RZPOkQo7/result.json`: the final adapter,
  including cross-origin-pattern updates and callback guards, passes.
- The five selected passing units have source hashes matching the final
  candidate or unchanged baseline, and their two changed headers also match.
  `.vm/extension-background-native-results.json` retains these selected passes
  alongside the failed earlier probe results. No combined native engine was
  linked by these checks.

Promoted patch: `208a07045b9623b53979da1eab879dd1c16db9a88c39e59bc0b22c4181602426`.
Probe baseline: `e3a4cf2b5baf68e779cb6cb3fcd8d3bf38f2e242f7aacd7a96645971efb65ea8`.

These are native compilation results. Background document/worker execution,
failure recovery, unload races, privacy isolation and port continuity still
need tests against a linked feature-enabled engine. Subsequent cancellation
and post-load task draining changes are recorded in
[deferred replies](webextensions-native-deferred-replies.md). Native install
state, tab/window delegates, controller platform/client setup and browser
attachment remain incomplete. No extension has run in the preview, which
continues to use its verified extension-disabled bundle.

Subsequent native listener persistence and enum-layout checks are recorded
in [background listener state](webextensions-native-background-listeners.md).
