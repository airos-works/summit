# Native menu implementation candidate

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The 25-file candidate connects menu property validation, typed identifiers,
JavaScript bindings, UI IPC, menu model operations and callback dispatch.
Both `menus` and `contextMenus` use the same native API object and require
menu permission in the extension's main world. **Menu/click-info snapshot: `ee896626...`.**
Current patch `ccacaaa8...` adds [temporary tab access and command routing](webextensions-native-active-tab.md).
The verification below records the menu snapshot; the linked document tracks
the follow-up and its current build.
No native menu has been displayed, and no extension has called these methods.
The preceding `e4ad8a2c...` full build compiled all sources and resolved the
menu-click dispatcher. Linking exposed three missing native functions (seven
references): the DNR loader, command dispatch and user-gesture handling. The
incremental full-target build compiles the corrected helper without warnings
and reaches the same three missing functions.

Native internal keys distinguish string IDs from safe integer IDs. Explicit
strings such as `"001"` remain strings, and numeric `1` cannot alias string
`"1"`. Public values decode back to their original type. Omitted IDs generate
UUID strings. Numeric create IDs retain the pinned Cocoa implementation's
acceptance within safe integer bounds; Chrome and Firefox document explicit
create IDs as strings. [Chrome reference](https://developer.chrome.com/docs/extensions/reference/api/contextMenus?hl=en),
[Mozilla reference](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/menus/create).

The parser checks item types, titles, boolean flags, parent IDs, context names,
URL match patterns and icon resource paths. Icon paths resolve relative to the
extension page; external origins are rejected. Null icon clearing and empty
pattern lists remain distinct from omitted properties. The native parameter
validator separately checks the values a UI receiver would consume. Unknown
properties and unsupported contexts are rejected. Data/symbol icons, icon
variants and additional Firefox menu contexts remain incomplete.

Raw JavaScript conversion reads property values directly and retains `onclick`
as a protected function. An explicit `onclick: null` clears the existing handler;
an omitted or undefined handler leaves it unchanged. It preserves optional
undefined values without calling
JSON serialization hooks or coercing boxed values. JSC's intrinsic array check
and exception scopes handle ordinary/revoked proxies and property enumeration
failures. A failed enumeration cannot become a successful empty update.

Click-info conversion produces fresh objects with typed IDs, before/after
checkbox state, editable status, frame/URL data and link/media/selection fields.
It rejects invalid identifiers, unknown or separator item types, and unsafe
frame numbers. Native JSON construction and JSC's native parser create own data
properties without invoking inherited JavaScript setters or replaceable global
JSON functions. Regression checks reproduce two failures in the earlier setter
implementation and pass with this correction, including Unicode, lone
surrogates, embedded NULs and safe-integer limits.
These values have run in JavaScriptCore. The new renderer dispatcher supplies
fresh protected values to each per-item and general listener, but has not run
inside an extension.

The UI implementation validates updates before changing keys or parent links,
checks for duplicate names and cycles, and removes subtrees iteratively while
detaching their child links. The cycle helper permits moving an item higher in
its existing tree and rejects moving it below a descendant. The native model
supplies construction, mutation, minimal event parameters and icon lookup.
The click producer checks permission, owned item identity, enabled/visible
ancestors and tab access, then rechecks the current load and item after
background wake. These model/UI operations compile but have not executed.

Per-item callbacks use unique UUID tokens independently of public menu IDs.
A pending callback is registered before its request is sent. A successful reply
never re-registers it, so removal before a delayed reply cannot restore it.
Renaming an item preserves its token; replacement, explicit clearing, subtree
removal and remove-all broadcast the discarded tokens to every extension process.
A failed request removes only its pending token. Each document's callback store
balances one listener registration while it contains handlers, clears on document
stop/destruction and keeps listener removals tied to the original extension load.
The registry does not depend on mutable `browser` or `chrome` global properties.
The general `onClicked` path still uses WebKit's existing namespace enumeration.

Extensions with event-page or service-worker backgrounds must supply an explicit
create ID and use `onClicked` instead of a per-item callback. This is enforced
again using the UI process's extension state. [Chromium create validation](https://github.com/chromium/chromium/blob/main/chrome/browser/extensions/api/context_menus/context_menus_api.cc),
[Chromium callback restriction](https://github.com/chromium/chromium/blob/main/chrome/browser/extensions/context_menu_helpers.cc).
Cocoa adapters retain the existing Objective-C implementation behind the shared
C++ binding signatures.

## Verification

- **181 native checks** pass using nine source units, frozen JavaScriptCore/WTF
  and actual WebKit match-pattern code. Inputs and libraries remain unchanged;
  the native crash log is empty and the helper compiles without warnings.
  API object initialization is adapted to real JSC/WTF initialization. The
  executable links neither WebCore nor WebKit and creates no extension context.
- **501 generated-binding checks** pass with Haiku and Cocoa preprocessing.
  The 37 interfaces generate 22 C++ and 15 Objective-C implementations. Menu
  checks cover both aliases, main-world/dynamic guards, raw argument adapters,
  synchronous create IDs and optional update/remove promises. This does not
  compile Cocoa or execute bindings.
- **13 connected native translation units compile**, including the API,
  callback store, namespace, bindings, both generated IPC receivers, context
  load/unload code and menu model. Source/configuration snapshots remain unchanged.
  The unchanged common `WebExtensionMenuItem.cpp` emits eight existing `#import`
  warnings; the new units compile cleanly.
- The current source audit covers all 25 integrated files, combining the original
  menu audit with the one-file conversion correction. The platform build-list changes
  and Cocoa adapters are reviewed separately from native compilation. The full
  native CMake configure succeeds. The original menu build completed all scheduled compilation
  before failing to link on the three missing functions listed above. The corrected
  conversion helper also passes an isolated compile with Extensions enabled.
- These checks do not execute menu-model mutation, callback lifecycle hooks,
  cross-process delivery or an extension. No native extension menu has appeared.

Evidence:

- `.vm/extension-menu-current-validation.json` — current patch and build
- `.vm/extension-menu-items-connected-validation.json` — original menu patch
- `.vm/audit-menu-items-connected.py`
- `.vm/extension-menus-api-promotion.json`
- `.vm/extension-menu-item-tests.9D5SmTi1/result.json` — 169 checks
- `.vm/extension-menu-own-properties-validation.json` — correction and regression audit
- `.vm/extension-menu-item-tests.FtXYnmZj/result.json` — 172 checks, two reproduced failures
- `.vm/extension-menu-item-tests.4yn6V1IJ/result.json` — 181 passing checks with the correction
- `.vm/extension-lifecycle-inputs.YBwQUohJ/result.json` — corrected helper compiles
- `.vm/extension-binding-platforms-1p664xiv/result.json` — 501 checks
- `.vm/extension-lifecycle-inputs.gDI9osNb/result.json` — nine API/binding/receiver units
- `.vm/extension-lifecycle-inputs.SLyRdxdH/result.json` — three model/conversion units
- `.vm/extension-lifecycle-inputs.CCB0FSlX/result.json` — load/unload context unit
- `.vm/modern-extensions-menu-delivery-build-result.json` — completed build; three missing functions
- `.vm/modern-extensions-menu-own-properties-build-result.json` — corrected helper compiles; same link failure

## Remaining connections

The connected API and callback-lifetime code require an actual extension
runtime test. Native menu presentation, trusted click routing,
temporary `activeTab` access, asynchronous lifecycle validation and real
extension listener delivery still need implementation or runtime verification.
Background menu persistence and the wider Safari/Chrome/Firefox API surface
remain part of the full browser goal.
