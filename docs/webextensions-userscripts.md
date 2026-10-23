# userScripts (#105)

`chrome.userScripts` lets userscript managers (Tampermonkey, Violentmonkey)
run the scripts their users install. Before this, Tampermonkey's MV3 build
installed but ran nothing: it found no `chrome.userScripts`.

## What exists

| API | Behaviour |
| --- | --- |
| `userScripts.register(scripts)` | `id`, `matches` (required), `excludeMatches`, `includeGlobs`, `excludeGlobs`, `js` (`{file}` or `{code}` sources, in order), `runAt`, `allFrames`, `world` (`USER_SCRIPT`, the default, or `MAIN`) |
| `userScripts.update(scripts)` | merges the given fields into existing scripts |
| `userScripts.unregister(filter?)` / `getScripts(filter?)` | by `ids`, or all user scripts; content scripts of `scripting` are unaffected |
| `userScripts.configureWorld()` / `resetWorldConfiguration()` / `getWorldConfigurations()` | kept and reported; see the world below |
| `runtime.onUserScriptMessage` / `onUserScriptConnect` | present, never fired; see the world below |

The namespace needs the `userScripts` permission, which Summit now accepts at
install ("Run user scripts you add on permitted sites"). Chrome additionally
asks the user to switch user scripts on per extension; Summit does not.

## How

User scripts are registered scripts (the machinery of
`scripting.registerContentScripts()`) whose identifiers start with `_us:`.
`scripting` IDs may not start with `_`, so the two never collide; `scripting`
listings and an unfiltered `unregisterContentScripts()` skip `_us:` scripts,
and `userScripts` sees only them.

- `WebExtensionRegisteredScriptParameters` gains `userScriptSources`
  (`(file, code)` pairs), `includeGlobs` and `excludeGlobs`.
  `parseWebExtensionUserScripts()` / `webExtensionUserScriptJSON()`
  (`Shared/Extensions/WebExtensionRegisteredScriptParser.cpp`) convert
  RegisteredUserScript objects for the API and for the store.
- The registered script store keeps user scripts, inline code included, so
  they survive a restart (loaded with the `userScripts` permission).
- Inline code becomes `WebCore::UserScript` text with a
  `webkit-extension://…/__userscripts__/<id>/<n>.js` URL.
- **Globs.** User content patterns cannot express globs (`*` any run of
  characters, `?` one, over the whole URL), so a script with `includeGlobs` or
  `excludeGlobs` is wrapped in a function that checks `location.href` first.
  Tampermonkey's `@include` rules arrive as globs.
- **The `USER_SCRIPT` world is the extension's content script world.** Chrome
  gives user scripts a world of their own, whose `runtime.sendMessage` reaches
  `runtime.onUserScriptMessage`. Here their messages reach `runtime.onMessage`
  and `onConnect`. Tampermonkey registers the same listener on both events, so
  this is enough for its `GM_*` calls; an extension that listens only to
  `onUserScriptMessage` would not hear its scripts. `configureWorld({csp})`
  is not applied.
- **Service worker startup.** Summit runs extension service workers on the
  main thread, and `importScripts()` cycles the main run loop while the
  worker's script is still running. That cycle used to run the worker's
  event loop, microtasks included, so a promise callback ran before the
  script reached its next line. Tampermonkey sets a "still starting" flag
  that its first microtask clears, and registers `tabs.onUpdated` after
  `importScripts()`: it failed with "listener can only be registered during
  startup". `WorkerEventLoop::scheduleToRun()` now posts the run again while
  JavaScript is on the stack.
- The extension bindings generator now renames static callbacks whose
  JavaScript names are C++ keywords (`register` → `registerCallback`).

## Test recipe

`tests/fixtures/extensions/userscripts/` (MV3, `userScripts`, host permission
for `https://example.com/*`). Install it (Extensions › Add extension…, or
`summitctl --team ID installext FOLDER`), then:

| Check | Expected |
| --- | --- |
| background console | `registered heading:USER_SCRIPT:1 glob-miss:USER_SCRIPT:1 glob-hit:USER_SCRIPT:1 main-world:MAIN:1` |
| `https://example.com/` first heading or paragraph | `Changed by a user script`; the worker logs `message {"ran":"heading",…}` |
| `document.documentElement.dataset` on `https://example.com/` | `heading: ran`, `globHit: ran`, no `globMiss` |
| on `https://example.com/?skip` | no `globHit` |
| `window.summitUserScriptMainWorld` (page console) | `page` |
| after quitting and restarting Summit | the same, without reinstalling |

Then install Tampermonkey (Chrome Web Store build), add a script with
`// @match https://example.com/*` that changes the heading and calls
`GM_setValue`/`GM_getValue`, and check it runs and keeps its value.
