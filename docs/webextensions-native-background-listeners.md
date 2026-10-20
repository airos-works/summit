# Native background listener state

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The Haiku background adapter now loads and saves its event listener cache
in the context's JSON state, using the existing atomic state-file writer.
The cache stores full event names and unsigned registration counts. It
retains independent events that share an API-facing name, such as different
`onMessage` events, and avoids numeric enum IDs that can shift with build
features. Unknown names, unsupported versions, wrong types and invalid
counts discard the cache so the background can discover its listeners.

The native format version is 4, following the existing Cocoa/GLib event
state version. Additions to the event enum must bump that version too.
Saving unchanged canonical state avoids another file write, and cache
updates preserve other context-state fields. Native state remains separate
from Cocoa's archived property-list format.

When a new native background starts, its live counts replace the saved
cache, and listener discovery is marked pending until loading finishes.
This prevents registrations accumulating across background restarts and
keeps messages arriving during initial discovery eligible for queuing.
These context transitions compile; their browser runtime verification is
still pending.

Evidence:

- `.vm/extension-background-listener-tests.aTYSrhvE/result.json`: all 44
  native assertions pass with the optional DNR event disabled.
- `.vm/extension-background-listener-tests.ior5ikyt/result.json`: the same
  44 assertions pass with that event enabled. The compiled numeric ID of
  `RuntimeOnMessage` changes from 28 to 29; independently supplied saved
  event names retain their meaning and counts in both layouts. This tests
  enum-layout compatibility, not DNR execution.
- Both runs exit normally with unchanged input, isolated configuration,
  source and dependency hashes and no fresh debugger event. They link
  frozen JavaScriptCore/WTF and private ICU, without WebCore or WebKit.
- `.vm/extension-background-listener-tests.ys4bjFLW/result.json`: an isolated
  mutation that saves every count as one fails five assertions, exits 1
  and creates no fresh debugger event. It is an expected negative result.
- `.vm/extension-lifecycle-inputs.XAk5GbLQ/result.json`: the actual native
  background adapter compiles with both extension features enabled,
  regenerated IPC headers and the final persistence helper.
- `tools/test-engine-extension-background-listeners.py` reproduces the
  helper checks; `--enable-dnr-event` selects the alternate enum layout.

Coverage includes empty caches, independently supplied reordered input,
exact multiplicity and removals, full unsigned counts, JSON round trips,
version/type/range failures, nonfinite values, unknown event identities,
whole-cache rejection and preservation of unrelated state.

Native install/update metadata, tab/window delegates, controller setup and
browser attachment still require work. Subsequent cancellation and post-load
changes are recorded in [deferred replies](webextensions-native-deferred-replies.md).
No browser extension or background document was executed by these checks.

Promoted patch: `a3972b1c21db2ef8fef493e3883a3e588064909cd30365febaaeb0317ec0f89a`.
Probe baseline: `208a07045b9623b53979da1eab879dd1c16db9a88c39e59bc0b22c4181602426`.
Aggregate: `.vm/extension-background-listeners-native-results.json`.
