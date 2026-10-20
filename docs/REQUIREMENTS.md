# Original objective and acceptance evidence

Create a full web browser for KunanyiOS in this workspace, based on the latest
WebKit codebase, with a KunanyiOS/Haiku native UI resembling Safari, and full
Safari, Chrome and Firefox extension support comparable in intent to Orion.
QEMU is the initial native test environment. The preview is an intermediate
deliverable; the original objective remains active.

| Requirement | Current evidence | Still needed for acceptance |
| --- | --- | --- |
| New application in this folder | `summit/`, native executable, source and build tools | Maintain a reproducible distributable application. |
| Full web browser | Real HTTP/HTTPS pages, native tabs, navigation, search, bookmarks/history, session restore, basic downloads, find/zoom. Native fixture tests. | Since done (20 October 2026): multiple windows and private windows, site permissions, certificate warnings, the downloads panel, Preferences, developer tools (Elements, Console, Network, Storage), printing, spell checking, saved passwords and the update checker. Left: HTTP authentication prompts (#87), pausing and resuming downloads (#107), SameSite and partitioned cookies (#92), accessibility (#118), and performance and sustained use (#116, #117). |
| Latest WebKit version/codebase | Upstream `00991b6cdc59937da6076c69a22ae6a9460a4445`, native port patch, full JavaScriptCore/WebCore/WebKitLegacy build. Summit reports 626.1.6 and the commit; native image paths verify private current libraries. 37 native browser checks and 11/12 advanced platform probes pass. Source and binary hashes accompany the bundle. | Complete relevant upstream conformance/regression testing (Test262 last run 7 October, #120), close platform gaps, and repeat package/restart tests against the current engine. The update process exists since 19 October (#72, docs/updates.md). BroadcastChannel is unavailable in the legacy embedding. |
| KunanyiOS/Haiku UI resembling Safari | Interface Kit window/toolbar, central address field, tabs with favicons, Home button, bookmarks bar, History and Bookmarks tabs, preferences, start page (see docs/browser-ui.md). | Refine interactions and visual consistency, menus, accessibility and native integration; test KunanyiOS itself. |
| Chrome extensions | Verified CRX3 import with signer identity, native consent, retained-package verification and restart tests; Chrome manifest key derives the Chromium ID, verified through `chrome.runtime.id`, saved grants/storage and browser restarts. Real toolbar actions, popup HTML/JS/input/storage, tab-specific state and popup shutdown pass native tests. The unmodified Chromium MV2 Dark Reader 4.9.131 package passes installation, live page theming and popup On/Off checks. | Store publisher authentication, remaining mixed-browser API semantics, updates, Manifest V2/V3, isolated content scripts, background pages/service workers, `chrome.*` API breadth, permissions, remaining actions/options, request filtering, storage, messaging and extension compatibility corpus. |
| Firefox extensions | Native ZIP/XPI/folder picker, consent and install/enable/disable/remove UI; declared Gecko identity and `browser.*` storage, cookie and permission fixtures. Approved startup persists across browser restarts. Native actions and real popup documents run for a Gecko-ID fixture. No Firefox add-on corpus verified. | Package authentication and updates, remaining Gecko manifest fields, complete `browser.*` Promise APIs and Firefox semantics, WebRequest and representative real add-on verification. |
| Safari extensions | Shared WebExtension runtime foundation; no Safari bundle or representative Safari extension verified. | Safari WebExtension bundle/resource import and API behavior, representative Safari WebExtension verification. Native macOS Safari App Extensions are a separate binary/platform problem, not demonstrated by WebExtension support. |
| QEMU test workflow | Separate running x86_64 beta6 guest, SSH/QMP tools, native latest-engine tests, HTTP/platform fixtures and visual HTTPS verification. | Extension isolation and compatibility tests, OS integration and current-engine package regression checks. |

The [published extension corpus](../compatibility/README.md) now pins six
unmodified uBlock Origin and Dark Reader release packages, including Chromium
CRX/ZIP, Firefox XPI and an MV3 service-worker package. Download hashes and
archive inspection are verified. The unmodified Chromium MV2 Dark Reader
4.9.131 package now passes native installation, theming, enable/disable and
popup On/Off checks, including theming an already-open tab without reloading.
The [script-injection suite](webextensions-tab-script.md) verifies 129 cases
using installed fixtures, including arbitrary thenables and cancellation on
navigation. [Result compatibility](webextensions-script-results.md) records
the remaining distinction between Firefox and Chrome MV2 behavior. The remaining published corpus and complete
cross-browser behavior still need verification.

Extension acceptance includes adversarial checks: a page must not obtain
privileged extension APIs, one extension must not access another's data,
content scripts must respect origin grants, revoked permissions must take
effect, and archives must not escape their installation directory. Parsing a
manifest or injecting a script alone does not prove extension compatibility.

Orion's own documentation describes its WebExtensions support as incomplete
and specifically identifies Safari **web** extensions. Its architecture is
inspiration, not evidence of Summit's implementation:
https://help.kagi.com/orion/browser-extensions/macos-extensions.html
