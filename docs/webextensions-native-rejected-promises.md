# Native rejected-promise construction

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

Generated extension bindings use `toJSRejectedPromise` for some invalid API
calls. Its implementation previously required Cocoa's JSValue wrapper and
was missing from the native link. It now lives in common C++, using
JavaScriptCore's deferred-promise API and the existing error formatter.
The promise and rejection function remain protected while constructing the
error. The obsolete Cocoa implementation is removed.

All 20 native checks pass in
`.vm/extension-rejected-promise-tests.UFwaz6TP/result.json`. Coverage includes
promise identity, rejection-only settlement, repeated observers sharing the
same Error, API/argument/plain error formatting, UTF-16 text with NUL and an
unpaired surrogate, overwritten global constructors, collection and a batch
of 64 independent rejections. The run exits normally with unchanged
input/source/library hashes and no fresh debugger event. It compiles the
actual utilities, wrapper and string bridge, linking frozen JavaScriptCore/WTF
and private ICU without WebCore or WebKit.

The utilities unit also passes the isolated full-feature compile probe in
`.vm/extension-lifecycle-inputs.uHidluff/result.json`; its source matches the
runtime-tested candidate. That earlier compile used baseline `1b839dea...`.
The first runtime probe, `2Z4t3seN`, fails compilation because its isolated
configuration enabled WebExtensions without content extensions. The runner
now enables both, matching the full build's required configuration.

`tools/test-engine-extension-rejected-promises.py` reproduces the checks.
They exercise the helper, not generated binding calls, extension contexts
or IPC. Cocoa compilation and complete browser extension execution remain
unverified; the preview continues to use its extension-disabled bundle.

Promoted patch: `6338f83f047dcaa3b58be944341804d64cb421d092305cbd7b0cffeb083d6069`.
Runtime baseline: `55777c374a27e62895e7404aaea3efe33042b78fe1c7d4d459fe4bc057faf861`.
