# Native message-port lifecycle

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The Haiku engine now has a C++ message-port bridge with host callbacks, validated
JSON messages, explicit disconnect errors and cleanup when the extension unloads.
It implements the remaining `WebExtensionMessagePort::reportDisconnection`
method needed by common port handling. Native executable-host discovery,
`runtime.connectNative()` host integration and the browser SDK are unfinished;
no native host has connected to an extension in Summit.

The host callback receives JSON text, including valid fragments such as `null`.
Pending messages wait for a handler and retain their order. A batch enters the
queue before callbacks run, so messages added during a callback follow the
entire earlier batch. Removing a handler pauses delivery; replacing it affects
the next queued message. Closing the queue discards pending messages. A retained
SharedTask protects a callback that removes itself, and the queue retains itself
during delivery. All calls belong on the WebKit main thread. A future executable
transport must dispatch back to that thread without transferring ordinary
WebKit object references to worker threads.

The callback queue permits at most 1,024 pending messages and 64 Mi characters
of pending JSON, using the engine's existing 64 Mi-character per-message limit.
Invalid JSON and queue overflow report a disconnect error at the native port.
Validation rejects an invalid batch before delivering any part of it. These
limits apply to the native callback queue; the common queue used before a native
connection is established is a separate, older mechanism. Queued native messages
are taken from that common queue and delivered toward the host, correcting the
direction used by the upstream Cocoa implementation. The Cocoa implementation
itself remains unchanged.

A port records the context's per-load identifier. A stale port cannot send into
a later load, and map removal checks the actual port object before removing a
channel. Duplicate registration cannot inflate endpoint counts. Disconnect
marks the port detached before releasing context ownership or calling the host.
An explicit host disconnect reports to the extension; it does not invoke that
host's own disconnect callback. Peer disconnect invokes the host callback once.
Context unload detaches ports before closing background pages, then schedules
host notifications after the unload stack unwinds. The destructor does not
retain an object whose reference count has already reached zero.

Disconnect IPC now carries optional error text. The process stores that text
for lazy creation of `port.error`, and exposes `runtime.lastError` during each
disconnect callback. Reading `port.error` marks that error handled as well.
This follows the different error access conventions documented by
[MDN's Port reference](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/runtime/Port).
Disconnect delivery selects the pages that own the channel and includes all
their web-content processes. Ports need their disconnected state updated even
without a listener.
The ownership map returns each page once and handles the Main/Inspector alias
when Inspector extensions are enabled.

**57 native queue checks and 31 native page-ownership checks pass** using the
actual production helpers and frozen JavaScriptCore/WTF in QEMU. The queue tests
cover JSON fragments and invalid input, both queue limits, atomic batch rejection,
callback replacement, pausing, reentrant enqueue, closing during delivery, and
dropping the host's last queue reference during a callback. The ownership tests
cover source world, channel, duplicate page entries, page removal and snapshots.
Inspector extensions were disabled in that native helper configuration.

Evidence:

- `.vm/extension-native-port-queue-tests.IG3tvGvS/result.json`
- `.vm/extension-page-port-tests.VZL5DoEG/result.json`

These helper executions do not instantiate a native message port or an extension
context. They do not execute unload notifications, JavaScript disconnect/error
callbacks, IPC, page-process selection or executable-host communication. The
feature-enabled WebKit library has not yet linked and no extension has run.

**Seven final native integration units compile** with extension and content-rule
features enabled: native message port, common context-port handler, common
context, process port API, process runtime API, generated context-proxy receiver
and generated port binding. Final sources and headers match the successful
probe. Evidence:

- `.vm/extension-lifecycle-inputs.6Ql7S9yP/result.json`
- `.vm/extension-native-port-validation.json`

The first drafts failed on the queue include path, SharedTask API usage and a
raw-pointer lookup. The final compile includes those fixes and the later
ownership-based disconnect routing. Cocoa was not compiled. The helper tests
link frozen JavaScriptCore/WTF; they do not link WebCore or WebKit.

Promoted patch: `425e5c88a7cb5b221858163374430798efa40a166384bcde3f085be904e3df50`.
Probe baseline: `dc4675d8cdc40b87b0e5134c6d09fb58c5f2dfc2880d968d3bc98f8dd2282475`.

The combined full build of this patch and the preceding action work reached the
WebKit shared-library link. `reportDisconnection` and both action symbols resolved.
The link failed with 18 remaining symbols, down from 21, and no new missing
symbols. Evidence: `.vm/modern-extensions-native-port-build-result.json`.
