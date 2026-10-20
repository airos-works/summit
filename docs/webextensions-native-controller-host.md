# Native extension controller and browsing context

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

Haiku extension controllers now initialize an owned native browser-tab registry.
When extensions are enabled, WebViewContextHaiku creates a controller bound to
its existing website data store and uses that controller's registry for the
native SDK. Every page configuration receives the same controller. The default
build with extensions disabled continues to create its own registry.

Persistent extension storage uses the browsing context's verified profile under
`WebExtensions`. Failure to obtain that directory returns a context creation
error. Private contexts use nonpersistent controller configuration and the same
ephemeral website data store already owned by that context. The temporary global
default path computed by createDefault is replaced before the controller is
constructed; it is not used as a fallback for a failed profile directory.

`.vm/extension-lifecycle-inputs.NnHOTGx8/result.json` records **three successful
native compile checks** with both extension features enabled, regenerated IPC,
matching bindings and unchanged native inputs. The checks cover native platform
initialization, the common controller and browsing-context configuration.
`.vm/extension-controller-host-validation.json` matches final source hashes to
the compile report. No controller, profile-directory failure, private context
or configured page was executed by these checks.

Native extension tab/window delegates and frame-query handlers are still
unfinished. The SDK and browser integration have not run with this controller.
Cocoa's distributed storage-deletion notifications also remain unported: the
native platform initialization provides real browser state but does not replace
that observer behavior. No extension has run in Summit.

Promoted patch: `7d5fe9b8cc4f898dae1bcda278c6468dc4eee5cedd4904d0b146d40f230188ce`.
Probe baseline: `0f24f49c6189020ec84b14732cd10bfa31b4c7f4a555ebaa224c18f9c63d5209`.
