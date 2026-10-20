# Native extension test-message delivery

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

Haiku's extension context accepts JSON values for test messages and test-started
and test-finished notifications. It retains arguments while queued, then routes
the existing IPC messages to web-page and content-script listeners and wakes
background content for main-world delivery. Loaded/testing-mode checks remain
in place. The browser's native test-host API has not yet been connected.

Haiku queries the actual weak frame registry to decide whether a test listener
exists. The previous separate totals could stay positive after duplicate
registrations were removed or frames disappeared. Haiku no longer maintains
those totals. Cocoa retains its existing counter implementation; its query body
moves to the common source behind the platform branch.

Each queue drain checks availability before taking an item and sends at most
the number of messages queued at entry. This preserves pending messages when
delivery becomes unavailable and prevents a requeueing sender from keeping a
drain running indefinitely. Queues and JSON argument references remain separate
for the three event types.

The helper passes 70 native checks in
`.vm/extension-test-message-queue-tests.SId95oaf/result.json`. Tests use WTF's
actual weak counted sets with reference-counted test frame objects. They cover
all three event types and routed worlds, duplicate registrations, dead weak
frames, removal/replacement of the last listener while draining, FIFO ordering,
requeueing, independent queues, JSON payload identity/lifetime and an iterative
10,000-message drain. The process exits normally with no new debugger events;
sources, native inputs and frozen JavaScriptCore/WTF libraries remain unchanged.

The earlier 16-check queue-only pass is retained in
`.vm/extension-test-message-queue-tests.CjvL9R42/result.json`. Expanded fixture
compile failures are also retained: modern WTF requires weak-pointer targets
to provide supported reference ownership; the final fixture uses RefCounted.

These are helper checks, with test frame objects. No WebFrameProxy, extension
context, listener callback, background wake-up or test-message IPC was executed.
Cocoa was not built or tested.

All three final integration units compile with both extension features enabled
and regenerated IPC in `.vm/extension-lifecycle-inputs.Hv1HggSu/result.json`:
the native adapter, common context and event registration/removal source.
`.vm/extension-test-delivery-validation.json` matches their final source/header
hashes to the candidate and the successful helper probe. Initial staging-path
and fixture compile failures remain recorded as failures.

Promoted patch: `5732f4073311cf11bed8a00cb90e5aa3d87ae0bea6c0199dbf15bdf3dfb9464e`.
Probe baseline: `8ba00bb96388053d59053815dba4bea5c8d7e2ee28fb67c553a8a2c1949e3c57`.
