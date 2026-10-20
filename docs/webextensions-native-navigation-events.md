# Common navigation and cookie event forwarding

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

Four context navigation handlers now live in common C++: provisional start,
commit, completion and failure. The two cookie notification methods move
with them. All six bodies are unchanged from Cocoa. They retain permission
checks, background wake-up, event routing, and the commit-time clearing of
tab action customizations, temporary permissions and page-specific styles.

The cookie-store observer also forwards to the common controller on Haiku.
Upstream still omits cookie `changeInfo`; this change does not add it.

`.vm/extension-lifecycle-inputs.kbr4VPPy/result.json` records native compilation
of the final event and controller units, with both extension flags enabled,
regenerated IPC, and unchanged native inputs and configuration. Final source
and controller-header hashes match the report. The earlier
`.vm/extension-lifecycle-inputs.XFEqV8Fg/result.json` also passed, before the
observer guard was removed. Extraction body hashes are recorded in
`.vm/extension-navigation-events-extraction.json`.

These are compile checks. Native tab/action delegates, process-side event
dispatch and public bindings remain incomplete; no notification or navigation
event has been delivered to a running extension. Cocoa compilation has not
been run.

Promoted patch: `a3f30d1b01848597f29ffa1009bc53cee272ecb4dcc48fa09631d8e7fac2e469`.
Probe baseline: `7f17d7a6cc8b598ce1774299459c0e953e5002f938c75db14201bced7aff9029`.
