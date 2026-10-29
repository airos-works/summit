# 1Password in Summit

Target package: the unmodified **Firefox** build of 1Password, `8.12.32.33`
(Manifest V2, Gecko ID `{d634138d-c276-4fc8-924b-40a0ea21d284}`), installed from
1password.com through Summit's extension manager. SHA-256 of the XPI:
`b952fb617027f78b5649ffdd88f58bc07da92d21dc73cc6bb6e28879e5e2e30b`.

There is no 1Password desktop app for KunanyiOS/Haiku, so the extension has to
run in its standalone mode: sign in to a 1Password account in a tab, unlock in
the toolbar popup, fill from the inline menu.

## Status (September 20, 2026)

| Stage | State |
| --- | --- |
| Install, consent, restart | Worked before this work |
| Background page start-up | **Fixed.** Reaches `👍 Finished initializing 1Password`: WebAssembly core, IndexedDB upgrade, feature-flag download, desktop-app probe failing gracefully |
| Previously installed copy picks up new permissions | **Fixed.** No reinstall needed (see *Permission upgrade*) |
| Toolbar click while signed out | **Works.** The background opens its welcome/permission-setup page through `tabs.create`; the page renders as designed ([screenshot](screenshots/1password-welcome.png)) |
| Welcome page rendering | **Fixed.** Its Figma-exported SVG used `fill-opacity:0.01` placeholder rects and `<pattern>` image fills; Haiku drew the first opaque black and the second as black (see *Rendering fixes*) |
| "Sign in" from the welcome page | **Fixed.** The extension tab navigating to `start.1password.com` is replaced by a web tab showing the sign-in form; the Safari 26 user agent removes the outdated-browser banner |
| Account sign-in | **Works.** The owner signed in to a real account |
| Inline field icon on login pages | **Works.** The icon appears in the username field and offers to unlock ([screenshot](screenshots/1password-inline-icon.png)). All eight manifest content scripts are registered and matched, including the `world: MAIN` entry |
| Unlock popup, inline menu suggestions, autofill, save login | Not yet verified. Needs the owner's master password |

## The popup that kept spinning (1 October 2026)

With `bundle-5wfht_91` the toolbar popup showed only a spinner on Proton
Mail's sign-in page, while on other pages it offered to unlock. The popup asks
the background for the page's details, and the background asks the page's
content scripts, which never answered: the Proton tab's main thread was
deadlocked. Proton's crypto worker compiles WebAssembly; the compiler thread
that finishes it arms the worker's JavaScriptCore timer under JavaScriptCore's
one timer lock, and in the Haiku port arming a timer waited for the looper of
the timer's thread. The worker held its looper while firing a timer of its own
and waited for that lock, and the main thread waited on the same lock the next
time it scheduled a garbage collection (for a new frame). The page still looked
normal, but nothing ran in it any more, not even Developer Tools.

The worker run-loop fix for issue #4 made this possible: before it, worker
threads never cycled their looper. Timers in `RunLoopHaiku.cpp` now keep their
state under a lock of their own, so no thread waits for another thread's looper
to arm or stop a timer. `tools/bench/pages/worker-timers.html` reproduces the
pattern (four workers compiling WebAssembly while the page creates frames).

The frozen tab also explains a browser that could not be closed: the port
cancelled a close when a page did not approve it within 30 seconds, so a
deadlocked tab kept its window and the application open. See
[STATUS.md](STATUS.md) for how hung pages close now.

## How the gaps were found

`tools/make-traced-extension.py` unpacks a package into a *diagnostic copy* and
injects `tools/extension-gap-tracer/summit-gap-shim.js` into every extension
page. The shim reports missing namespaces and members, replaces them with
logging stubs so start-up continues past the first failure, and reports
unhandled rejections. Run the copy with

```sh
SUMMIT_EXTENSION_DEVELOPER_MODE=1 SUMMIT_TRACE_EXTENSION_CONSOLE=1 ./run-browser.sh --profile …
```

Developer mode lets an edited unpacked package keep its approval for that
session. The engine additionally compares `LastSeenResourceFingerprint` in the
extension's `State.json`; a diagnostic profile has to carry the new value.
Results from a traced copy never count as verification of the published
package. `SUMMIT_TRACE_PAGE_CONSOLE=1` does the same for ordinary tabs, which
is where content-script errors appear.

## What was missing, in the order 1Password hit it

1. `chrome.notifications` — namespace absent (`onClicked` of undefined).
   Added `create/update/clear/getAll/getPermissionLevel` and the four events,
   shown through `BNotification`. Clicks reach the extension since #102
   (the notification starts Summit again with its identity).
2. `chrome.privacy` — added by the previous agent but never linked: two
   sources included `WebExtensionPrivacyControllerHaiku.h` without its `haiku/`
   directory. Added the `services` category (`passwordSavingEnabled`,
   `autofill*`): always `false` / `not_controllable`, because Summit has no
   built-in password manager for an extension to take over.
3. `webRequest.*.addListener(listener, filter, ["blocking"])` — two generator
   bugs. The binding required a dictionary for `extraInfoSpec`, which is an
   array; and with two arguments the right-to-left matching of optional
   parameters assigned the *filter* to `extraInfoSpec`. `JSONValue=Array` now
   types the parameter. `blocking`, `asyncBlocking` and `extraHeaders` are
   accepted and observed, as in Safari; listener return values are not applied
   to the request yet. `webRequestBlocking` is a recognized permission.
4. `runtime.connectNative` / `sendNativeMessage` — never settled, so the
   desktop-app probe would wait forever. They now fail with
   `runtime.lastError`, and 1Password falls back to standalone mode.
5. Extension `.wasm` files were served without `application/wasm`, which
   `WebAssembly.instantiateStreaming` requires. The Haiku MIME table now lists
   wasm, mjs, json, css and font types instead of depending on the system
   database.
6. `menus.create({contexts: [..., "tab", "password"]})` — Firefox's `password`
   context was rejected. It maps to editable fields; other browsers' contexts
   for surfaces Summit lacks (`bookmark`, `tools_menu`, `launcher`) are ignored
   when a supported context accompanies them.
7. `webNavigation.onCreatedNavigationTarget` — fired since #106 (see
   [webextensions-webnavigation.md](webextensions-webnavigation.md)), as are
   `onHistoryStateUpdated` and `onReferenceFragmentUpdated`; `onTabReplaced`
   never fires.
8. `runtime.getBrowserInfo` (Firefox only), `runtime.onSuspend`,
   `onSuspendCanceled`, `onUpdateAvailable`, `requestUpdateCheck`,
   `webRequest.handlerBehaviorChanged`, `idle.*`, `management.getSelf/getAll`.
   `idle` uses the input server's `idle_time()`. `management` only discloses the
   calling extension.
9. `tabs.create/update/remove/reload/insertCSS/connect`, `windows.create/
   update/remove` and the whole `scripting` namespace were compiled for Cocoa
   only. See [tab commands](webextensions-tab-commands.md).

## Rendering fixes found through 1Password

- The Haiku graphics context ignored the context's global alpha for fills. SVG
  `fill-opacity`, `stroke-opacity` and canvas `globalAlpha` reach the context as
  that alpha, so translucent shapes were painted opaque. Fills now multiply the
  color's alpha by it, and changing it no longer replaces a translucent color's alpha.
- Pattern fills (`fill="url(#pattern)"`, canvas `createPattern`) were not
  implemented and drew in the default black. Rects and paths now tile the
  pattern image in pattern space.
- Checked while investigating: canvas `drawImage`, `createImageBitmap` from
  canvases and `ImageData`, and `putImageData` all produce correct pixels.
  `OffscreenCanvas` is absent because the port builds with
  `ENABLE_OFFSCREEN_CANVAS` off; enabling it needs a full rebuild and a review
  of ImageBuffer painting from worker threads.

## Permission upgrade

The install consent always listed every required manifest permission, including
ones the engine could not provide at the time. The engine only granted what it
supported, so an extension installed earlier never received permissions added
later. At browser start-up Summit now names the currently supported required
permissions, and the engine grants those that are neither granted nor denied
yet. Optional permissions and origins are never added this way. 1Password's
existing installation received `idle`, `management`, `privacy` and
`webRequestBlocking` without being reinstalled.

## Launching with extensions

Extension packages are staged beside the profile
(`<profile>/ExtensionStaging/<team>`). Before that, staging used the system
temporary directory; on a machine whose boot volume is small or full that copy
fails or crawls, and the browser comes up with no extensions and nothing in the
log. A desktop entry that still points at an older build without extension
support, or at a different profile, produces the same impression.

## Known limits that affect 1Password

Updated 20 October 2026. Since this was first written, blocking `webRequest`
(cancel, redirect, headers and `onAuthRequired`,
[webextensions-webrequest-blocking.md](webextensions-webrequest-blocking.md)),
the `downloads` API (exporting the Emergency Kit,
[webextensions-dnr-downloads.md](webextensions-dnr-downloads.md)),
`storage.managed` and OffscreenCanvas have landed. What is left:

- No desktop-app integration, biometrics or SSH agent: those need a native
  messaging host (#106).
- Since 29 Oct content scripts have Firefox's `cloneInto` and
  `exportFunction` (`docs/webextensions-content-script-helpers.md`). The
  Firefox build's injected scripts call `cloneInto(detail, window,
  {cloneFunctions: true, wrapReflectors: true})` for the CustomEvents they
  send the page (1password.com sign-in, remote autofill pairing); they now
  get copies made in the page's world. Not yet rechecked with 1Password itself.

## CPU scheduling (9 October 2026)

Extension-owned pages retain normal Haiku thread priorities from creation,
using their extension configuration before the first `webkit-extension:` URL
commits. This covers the initial `about:blank` background host and service
worker host. Ordinary hidden tabs still use `B_LOW_PRIORITY`; loaded extension
background pages already kept normal priority and disabled hidden-page DOM
timer throttling.

`EngineExtensionRuntimeTests.cpp`, with `SUMMIT_EXTENSION_SCHEDULING=1`,
compares a hidden ordinary page with an extension-configured `about:blank`
host, then checks actual extension background timers, asynchronous PBKDF2,
message/storage operations, priority after loading and context reload. This
checks scheduling, not the latency of unlocking a particular 1Password vault.

The old bundle measured priority 5 for both preflight pages and failed the
extension startup check. `bundle-b46l7ic6` measures priority 5 for the ordinary
hidden page and 15 for the extension host; all 18 native checks pass, including
two background load/storage rounds. Both runs cleanly release their helpers.
Evidence: `.vm/fixes-2026-10-09/priority-{baseline,final}.log`.
