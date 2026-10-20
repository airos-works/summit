# Native extension consent

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

Summit now has a native consent broker, a public SDK listener/reply path, and a
scrollable Haiku permission dialog. Extension controllers own the broker;
their profile context exposes it to the SDK. An extension unload cancels
requests belonging to its old privileged load identifier after invalidating
that identifier. Profile destruction shuts down the broker.

The [native permissions API](webextensions-native-permissions.md) now connects
`browser.permissions.request()` to this broker and implements manifest
validation, applying and persisting grants, and permission events. Actual
extension execution remains unfinished. The broker returns a consent decision;
the caller rechecks its load, manifest and current permission state before
applying that decision.

## Broker contract

All broker operations and completion callbacks run on WebKit's main RunLoop.
Requests have random UUIDs and an internal owner identifying one extension
load. The owner is never included in the host message. Only the displayed
request can accept a reply; unknown, queued, duplicate and cancelled IDs cannot
approve anything. Completions are always asynchronous and are detached from
the queue before invocation, including validation and cancellation errors.

There is one displayed request per broker, with at most 32 outstanding
requests. Each request is limited to 256 permission/site entries, 8,192 UTF-16
code units per string and 16,384 in total. Empty requests, missing identity,
zero owners and embedded NUL are rejected. A request expires two minutes
after enqueueing, including time spent waiting in the queue. The test helper
can use a shorter deadline. Native sends never block the application loop;
initial delivery has a short, bounded retry on queue congestion. A missing
or failed host returns an error instead of a consent decision.

`SetExtensionPermissionListener()` accepts local native messengers on the
application thread and returns `B_BUSY` while requests remain pending.
Feature-disabled builds return `B_NOT_SUPPORTED`. SDK replies and cancellation
may originate on any looper and dispatch to WebKit main while retaining the
profile context. The public protocol is in `WebKitExtensionPermission.h`.

Cancellation invalidates the request before notifying the UI. Every request
also carries its absolute monotonic deadline, so the dialog expires even if a
cancellation message encounters a full queue. A newer request dismisses any
previous dialog. An expired reply still fails the broker's deadline check.

## Summit UI

The application owns the permission handler. Its windows own no WebKit
objects. Each window lists the extension identity, every requested permission
and site, and the private profile label when applicable. Common permission
names include short descriptions; canonical names remain visible. Content
scrolls and uses native document colors. Control characters and bidirectional
formatting characters in extension-supplied text cannot create extra lines or
reorder the browser's consent labels.

The default button is **Deny**. Allow and Deny submit one decision for the
entire request. Closing the window denies; expiry cannot grant access. On
application shutdown, Summit cancels broker requests, closes permission
windows and removes the handler before releasing the SDK context.

## Verification scope

`tools/test-engine-extension-permission-prompts.py` compiles the production
broker and Summit dialog sources, links the frozen JavaScriptCore/WTF and ICU
libraries, and runs real BApplication/BMessenger/BWindow interactions in QEMU.
It invokes native buttons, closes windows, exercises FIFO decisions, rejects
queued/unknown/stale replies, cancels owners, expires unanswered prompts, checks
queue bounds and callback reentrancy, and verifies private labeling and display
sanitization. It also captures the actual dialog.

The helper deliberately initializes WTF from `ReadyToRun`, matching Summit.
The frozen helper library predates the separately tested pre-Run attachment
fix. No replacement RunLoop, WebKit stubs, mocked SDK or automatic extension
permission grants are used. The test's callback connects the actual dialog
directly to the actual broker. **It does not run the SDK wrapper, an extension
context, permission bindings, persisted grants or an extension-enabled browser.**

Native integration compilation and the full engine link are recorded
separately from this runtime scope. The application build includes the dialog
in both CMake and the modern browser staging tool; legacy builds compile its
guarded source without using the modern SDK.

**165 native checks pass**, with unchanged input/library hashes and no crash-log
events. Four additional native integration sources compile against the
extension-enabled probe configuration. All six modern application sources
compile against the updated public headers. The two changed application
sources also compile in the legacy configuration, and both SDK/profile sources
compile against the actual WebExtensions-disabled engine configuration.

- Runtime and screenshot: `.vm/extension-permission-prompt-tests.Nj4Sp7eY/`
- Source-hash audit: `.vm/extension-permission-prompts-validation.json`
- Modern app: `.vm/extension-permission-prompts-app-compile.json`
- Legacy app: `.vm/extension-permission-prompts-legacy-compile.json`
- SDK with extensions disabled: `.vm/extension-permission-prompts-disabled-compile.json`

The final native helper used the previous configured source baseline plus the
complete candidate overlay. The consent checkpoint's promoted engine patch was
`7bdd8bfd20284706a60fc1abf37b82341f6c843500de9c99607af82f138384fe`.

That checkpoint's full extension-enabled engine build compiled the new
implementation and reached the WebKit library link. It failed with the same **15 missing symbols
(19 references)** as the preceding command port, with no new missing symbols
and no compiler errors. The unresolved DNR loader and extension event
dispatchers still prevent an extension-enabled browser from linking. The
build result is `.vm/modern-extensions-permission-prompts-build-result.json`.
Both native source manifests match the promoted patch; the feature build
configuration remains enabled and the separate baseline remains disabled.

The test VM encountered a Haiku filesystem kernel panic before the first
compile probe started. Evidence is preserved in
`.vm/permission-prompts-vm-recovery/`. After a reset and remount, all 76,113
idle engine source files compared unchanged, the damaged manifest was rebuilt,
and the frozen JavaScriptCore hash matched its recorded value. No operating
system source was changed. Ninja also reported recovering a truncated build
log during the subsequent full build; that warning is retained in its evidence.
