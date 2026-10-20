# Native extension test reporting

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

Haiku's controller handles the seven existing test-report messages: assertion,
equality, log, message, added, started and finished. Each produces a
`WebExtension test: ` log line followed by a JSON object. Reports retain source
URL and line number, result flags, test names, messages and equality operands as
applicable. The message argument is retained as serialized JSON in `argumentJSON`.
Empty-message defaults match Cocoa's fallback reporting.

JSON escaping keeps embedded newlines and control characters inside one report.
`WTFLogAlways` keeps test reports available when release-log channels are disabled.
The existing seven `inTestingMode` IPC validators are unchanged. Cocoa's delegate
implementation is unchanged; native host callbacks and replies remain future work.

The final native source compiles with both extension features enabled and
regenerated IPC in `.vm/extension-lifecycle-inputs.uLPqVdtI/result.json`.
Its source hash matches the candidate, and native inputs/configuration remain
unchanged. `.vm/extension-test-reporting-extraction.json` records the unchanged
method signatures and message validators. No test-report IPC or extension test
was executed; this is compile evidence only.

Promoted patch: `8ba00bb96388053d59053815dba4bea5c8d7e2ee28fb67c553a8a2c1949e3c57`.
Probe baseline: `adb8b391c6e42031b5ccd084b4898216e9634bfc929214306c7a58f63ec23ac6`.
