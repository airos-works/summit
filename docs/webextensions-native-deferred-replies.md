# Deferred extension replies and background task draining

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

Queued background operations now retain an eager reply aggregator with an
explicit fallback result. The actual completion handler is constructed when
the queued operation starts. Discarding an operation before it starts thus
completes its reply instead of abandoning a completion handler.

The five runtime wake-up routes use this helper. Internal messages, connects
and background-page lookup report unavailable background content; web-page
messages retain their empty-result behavior. The common background-loading
API supplies its background-load error on cancellation.

Unloading or closing a background moves pending tasks out of the context and
releases them on the next main-run-loop turn, after context state has been
reset. Post-load draining also takes a local task snapshot, marks loading
complete before callbacks, and checks context/page identity before each
task and the final persistence step. This allows a callback to unload or
replace the background without invalidating the pending-task iteration.

Evidence:

- `.vm/extension-deferred-reply-tests.s4HubWvE/result.json`: all 18 native
  helper assertions pass, with normal exit, unchanged input/source/library
  hashes and no fresh debugger event. Coverage includes discarded pending
  tasks, retained references, successful/error replies, duplicate handlers,
  started-task ownership and move-only, void and optional result payloads.
- The helper links frozen JavaScriptCore/WTF and private ICU without WebCore
  or WebKit. `tools/test-engine-extension-deferred-replies.py` reproduces it.
- `.vm/extension-lifecycle-inputs.PpIQaB9Y/result.json`: the actual UI runtime,
  common context and native background units compile with both extension
  features enabled and regenerated IPC. Source/header hashes match the final
  candidate; native input and configuration snapshots remain unchanged.

The context's cancellation timing and reentrant post-load sequence have
compile coverage only. These checks execute no background document, worker,
extension context or IPC route. Their runtime verification, native install
metadata, tab/window delegates, controller setup and browser attachment
remain pending. The verified preview still has extensions disabled.

Promoted patch: `c711e3e700a24caf8c796eee55989f36d12d139e3129f92aeea80fd5d3db26cb`.
Probe baseline: `a3972b1c21db2ef8fef493e3883a3e588064909cd30365febaaeb0317ec0f89a`.
Aggregate: `.vm/extension-deferred-native-results.json`.
