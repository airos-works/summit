# Native WebExtensions integration investigation, 2026-09-14

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

This records the native integration route for upstream WebKit
`00991b6cdc59937da6076c69a22ae6a9460a4445` and the implemented extension
resource adapters. Verification comprises **7 production translation units
compiled with extensions enabled, 27 native resource-path checks, 111
native archive checks, and 33 native state-file checks**. Controller/context
and resource-handler adapters also have individual compile passes; their
[exact scope and remaining dependencies](webextensions-native-state-and-resources.md)
are recorded separately. The archive helper and updated constructor also
have separate native compile evidence. Reports and exact scope are in the
[manifest/resource notes](../tools/test-engine-extension-manifest-core-notes.md)
and [ZIP/XPI notes](../tools/test-engine-extension-archives-notes.md).

Subsequent native helper checks add [22 page-port ownership assertions](webextensions-native-ports.md),
[51 JavaScript reply assertions](webextensions-native-message-replies.md), and
[46 UTF-16 bridge assertions](webextensions-native-strings.md). Their related
common API, wrapper and IPC units have isolated compile passes. These remain
separate from a linked extension context and browser runtime.

The verified browser build keeps `ENABLE_WK_WEB_EXTENSIONS=OFF`. A separate
full feature-enabled build is also being ported; its first attempt compiled
JavaScriptCore/PAL but stalled during WebCore compilation in the VM's filesystem,
with a separate compiler crash. It has not linked a feature-enabled WebKit.
The path and archive tests run
production helpers, not a manifest/controller/browser runtime. **No extension
is running in Summit.** The acceptance criteria in
[REQUIREMENTS.md](REQUIREMENTS.md) remain intact, including Safari, Chrome and
Firefox compatibility and adversarial isolation tests.

The useful starting point is WebKit's existing `WebExtension`,
`WebExtensionContext`, `WebExtensionController` and their WebProcess proxies.
The native embedding can attach these directly to `API::PageConfiguration`.
However, enabling `ENABLE_WK_WEB_EXTENSIONS` alone cannot produce a working
Haiku runtime. The pinned GTK/WPE implementation is incomplete, and many
apparently portable classes still require Cocoa implementations at link time.
Port the existing implementation across its platform boundaries; retain its
manifest parser, match patterns, content worlds, IPC and JS binding generator.

The older `WebProcess/InjectedBundle/API/glib/WebKitWebExtension` API is the
native WebProcess plug-in mechanism. It is distinct from the newer browser
WebExtensions classes under `UIProcess/API/glib`, despite the overlapping
name. Loading an injected bundle would not supply the requested extension
manifest, permissions or browser APIs.

All source paths below are relative to `.cache/WebKit`. Line references in
the implementation-route tables identify the pinned upstream baseline;
subsequent native edits may shift those line numbers.

| Area | Evidence and implication |
| --- | --- |
| Main feature gate | `Source/cmake/WebKitFeatures.cmake:325` defines `ENABLE_WK_WEB_EXTENSIONS`, private and off. `OptionsHaiku.cmake` does not override it. `Source/WTF/wtf/PlatformEnable.h:660` also defaults it off. Use a separate native build configuration with `-DENABLE_WEBKIT=ON -DENABLE_WK_WEB_EXTENSIONS=ON` when porting; the existing preview must not advertise extension support from this flag. |
| GTK/WPE status | `OptionsGTK.cmake:143` and `OptionsWPE.cmake:97` enable it with experimental features. GTK's old API explicitly disables it; `WebExtensionContextGLib.cpp` requires `ENABLE_2022_GLIB_API`. This is evidence of a developing port, not a complete runtime to wrap. |
| Native dependency choice | Do not enable `USE_GLIB` or `ENABLE_2022_GLIB_API` to make Haiku compile. Those branches introduce `GFile`, `GKeyFile`, GObject, GTK/WPE views and their settings/delegates. Native adapters can use WTF file/JSON helpers, Interface Kit objects and `WebPageProxy` instead. |
| Common sources | `Source/WebKit/Sources.txt:317`, `:609` and `:712` already list common extension storage, manifest, context/controller and proxy sources. `SourcesGTK.txt` and `SourcesWPE.txt` add the GLib resource/background adapters. New native adapters belong in `PlatformHaiku.cmake`; common extractions should remain in `Sources.txt`. |
| JS generation | `Source/WebKit/CMakeLists.txt:144` and `:178` split Objective-C and C++ IDLs; `GENERATE_IDL_BINDINGS` produces `JSWebExtensionAPIUnified.mm` and `.cpp`. Of 37 extension IDLs, eight now have `UseCPPAPI`: alarms, event, namespace, port, runtime, test, web-page namespace and web-page runtime. `CodeGeneratorExtensions.pm:304` selects `.cpp` using that attribute. Most APIs still need conversion to its existing C++ path. |
| Namespace exposure | `WebExtensionAPINamespace.idl:36` onward marks almost every useful API, including `runtime`, `storage`, `tabs`, `scripting` and `webRequest`, `Platform=COCOA`. Matching methods in `WebExtensionAPINamespace.cpp` are also guarded. Moving an API to C++ requires changing the IDL, implementation and matching IPC gates together. |
| Context IPC | `WebExtensionController.cpp` now registers both normal and privileged context receivers on Cocoa and Haiku. `WebExtensionContext.messages.in` now exposes alarms, script errors and listener registration outside its Cocoa gates, with most API messages still inside. Preserve both identifiers and each `[Validator=...]` when making these paths native. |
| Request filtering | `ENABLE_CONTENT_EXTENSIONS` is a separate CMake option, off by default and enabled by GTK/WPE. Haiku needs it for the existing content-rule engine. The declarative-net-request translator remains `_WKWebExtensionDeclarativeNetRequestTranslator.mm`; existing common SQLite/rule bookkeeping is not the translator or blocking WebRequest implementation. |
| Service workers | `Source/WTF/Scripts/Preferences/UnifiedWebPreferences.yaml:5451` defaults `ServiceWorkersEnabled` to true for modern WebKit, false otherwise. This is a runtime preference in this checkout, not a missing `ENABLE_SERVICE_WORKER` CMake flag. Extension-scheme registration, worker JS bindings and lifecycle still need native integration. |
| Additional APIs | `WK_WEB_EXTENSIONS_ICON_VARIANTS`, `WK_WEB_EXTENSIONS_OFFSCREEN`, `WK_WEB_EXTENSIONS_SIDEBAR`, `WK_WEB_EXTENSIONS_BOOKMARKS`, `INSPECTOR_EXTENSIONS` and `DNR_ON_RULE_MATCHED_DEBUG` occur in source feature guards. They are not all independent CMake options here. Add deliberate port definitions and implementation as each becomes available; do not assume passing similarly named `-D` values exposes an API. |

The native embedding seam is already present in modern WebKit.
`UIProcess/API/APIPageConfiguration.h:161` exposes
`setWebExtensionController(RefPtr<WebExtensionController>&&)` and
`setWeakWebExtensionController(WebExtensionController*)`.
`WebPageProxy.cpp:968` retains them, `:1030` adds the page to the controller,
and `:14819` includes controller parameters in page creation. The WebProcess now consumes them on Cocoa and Haiku, and the native page
accessor returns its retained proxy. Both proxies and the page/frame hooks
have native compile passes; see the [process/event evidence](webextensions-native-process-bindings.md).
Attach the controller before creating each Summit page, including new windows;
a JavaScript injection API in the application is not needed for this route.

Use one browser-owned controller configuration per actual profile, with its
chosen `WebsiteDataStore`, extension storage directory and durable extension
identifiers. `WebExtensionControllerConfiguration` already exposes
`setStorageDirectory`, `setDefaultWebsiteDataStore`, persistent/nonpersistent
factories, and a UUID constructor. The native adapter now implements path
creation and `copy` in
`UIProcess/Extensions/haiku/WebExtensionControllerConfigurationHaiku.cpp:37`.
It computes default paths through `StoragePathsHaiku`, creates real temporary
directories, and preserves the selected directory and data store when copied.
Its default path follows the application signature; the browser still needs
to supply each actual profile's directory and data store explicitly. Persistent
extension storage also requires a durable custom extension identifier:
`WebExtensionController.cpp:449` returns no directory without one.
Ordinary pages can retain the profile controller; controller-owned
background/popup pages should use the weak configuration reference to avoid
ownership cycles. Private
access remains a separate permission (`setHasAccessToPrivateData`) and data
store decision, with controller sets for private and nonprivate content
controllers. A second storage folder alone does not prove private isolation.

Extension documents also need a dedicated page-configuration path, separate
from ordinary tabs. `WebExtensionContextCocoa.mm:2580` applies MV2/MV3 CSP
mode, permission-derived CORS patterns, the required extension base URL,
process display name, cookie policy and appropriate background preferences.
Map these through the corresponding `API::PageConfiguration` and preferences
properties. Preserve the extension-origin restriction when implementing any
access relaxation. Conversely, ordinary pages need extension URL masking:
`APIPageConfiguration.cpp:228` currently defaults this only on Cocoa. Test
both sides of that boundary rather than applying extension-page privileges
to every page sharing a controller.

A thin native public facade should own these common C++ objects and dispatch
all runtime operations to the WebKit main loop. Interface Kit windows can
receive UI requests and report results through owned messages, following the
new `BWebKitView` bridge. The adapter needs actual tab/window identities and
events, permission prompts, actions/popups, menus, notifications, downloads,
bookmarks and native messaging. `WebExtensionTab` and `WebExtensionWindow`
currently use the Cocoa delegate protocols, and their implementations live in
`UIProcess/Extensions/Cocoa/WebExtensionTabCocoa.mm` and
`WebExtensionWindowCocoa.mm`. Introduce native delegate interfaces or extract
the shared logic behind C++ client interfaces; retain the existing extension
API semantics and identifiers.

The first manifest/resource compile gate is implemented by
`tools/test-engine-extension-manifest-core-in-vm.sh`. It reuses the actual
Haiku Modern compiler flags and generated headers, enabling extensions only
in a copied configuration. Its recorded seven-unit production pass covers
`UIProcess/Extensions/WebExtension.cpp`,
`UIProcess/Extensions/WebExtensionMatchPattern.cpp`,
`Shared/Extensions/WebExtensionLocalization.cpp`,
`Shared/Extensions/WebExtensionPermission.cpp`,
`Shared/Extensions/WebExtensionUtilities.cpp`,
`UIProcess/Extensions/WebExtensionResources.cpp` and
`UIProcess/Extensions/haiku/WebExtensionHaiku.cpp`. There is no dedicated
`SummitWebExtensionCoreCompile` CMake target.

`WebExtension.h` now limits the GFile constructor to GLib and provides a
native resource-path constructor for directories and ZIP/XPI files. The
existing `WebExtension(const JSON::Value&, Resources&&)` and
`WebExtension(Resources&&)` constructors retain the upstream in-memory
manifest path. Scheme registration moved unchanged into
`WebExtensionMatchPatternProcessPool.cpp`, keeping process-pool dependencies
out of the reusable matcher; that full-runtime unit now compiles in the lifecycle preflight described
below; linking still requires the genuine context/process implementations.

`WebExtensionResources.cpp` shares the unchanged GLib `resourceDataForPath`,
`recordError` and `bestIcon` bodies across non-Cocoa platforms. The native
adapter supplies real bitmap/SVG decoding through WebCore, native Icon
creation and screen-scale selection. The parser and matcher were retained;
no success stubs or duplicate implementations were introduced. Object
compilation does not establish manifest execution or a working extension.
The next gates are upstream manifest/match-pattern assertions linked against
compatible Modern dependencies, then the actual WebKit library and process
executables with the feature enabled and the remaining native runtime paths.
Keep those feature-on probes isolated from the full-engine build.


The lifecycle compile preflight is `tools/test-engine-extension-lifecycle-compile.py`.
It snapshots the five common controller/context/process-pool/proxy units and
extension headers, records hashes, and enables both extension features in a
copied native configuration without using the feature-disabled PCH. Its first
run failed all five units on the missing native view types. The adapted header
passes all five. The subsequent process-proxy extraction also compiles without
warnings. The Cocoa file relinquishes the moved definitions; the exhaustive
content-world switch now traps an invalid enum rather than falling through.
No objects in this probe are linked or executed.

Evidence (2026-09-14):

- Failed baseline: `.vm/extension-lifecycle-inputs.fscU4gSa/result.json`.
- Five-unit native type pass: `.vm/extension-lifecycle-inputs.mVSQdL15/result.json`.
- Final proxy extraction pass: `.vm/extension-lifecycle-inputs.jpZLPkp7/result.json`.
- Undefined-symbol inventory: `.vm/extension-lifecycle-native-types-undefined.json`.

`tools/build-webkit-in-vm.sh --modern-extensions all` now configures a separate
source/build tree at `/boot/home/summit-webkit-extensions`, with both
`ENABLE_WK_WEB_EXTENSIONS` and `ENABLE_CONTENT_EXTENSIONS` enabled. It uses the
same locked upstream and Haiku patch as the normal build, but shares no engine
objects or generated headers with it. This build path is an integration gate;
its existence does not establish a successful feature-enabled link or runtime.

**Feature-enabled controller runtime tests cannot use the feature-disabled
WebKit library.** `ENABLE_WK_WEB_EXTENSIONS` changes object layouts and the
generated IPC contract:

| Current source | Feature-enabled difference |
| --- | --- |
| `UIProcess/API/APIPageConfiguration.h:535` | Adds the required extension URL and strong/weak controller references to configuration data. |
| `UIProcess/WebPageProxy.h:3876` | Adds strong/weak controller references to each page proxy. |
| `Shared/WebPageCreationParameters.h:284` | Adds controller parameters to the page-creation structure and its serialized payload. |
| `Shared/API/APIObject.h:190` | Inserts extension API type IDs before existing types, changing subsequent values including `WebsiteDataStore` and `WebsitePolicies`. |

A successful link does not establish compatibility. Controller and
configuration runtime tests need a matching feature-enabled WebKit dependency
closure, including the generated headers/serializers and any process
executables they use. Keep these probes compile-only until that closure is
available; linking selected feature-enabled objects to the baseline
feature-disabled `libWebKit.so` is unsafe. The standalone native path/archive/state-file
helper checks do not cross this boundary and do not demonstrate controller
execution.

The next proposed native slice is profile-owned controller/context lifecycle
and resource serving. `UIProcess/haiku/WebViewContextHaiku` is the existing
owner of the profile's process pool and data store; it should also own the
controller and attach it in `configure(API::PageConfiguration&)`. Preserve
the common controller load/unload implementation at
`WebExtensionController.cpp:147`, including duplicate-context/base-URL
rejection, rollback and process notifications. Native implementation status
under `UIProcess/Extensions/haiku` is:

| File or proposed file | Implemented and remaining scope |
| --- | --- |
| `WebExtensionControllerHaiku.cpp` (not implemented) | Native platform/client initialization and seven generated test callbacks remain. The five-second freshly-created expiry now uses the common RunLoop, and both receiver IDs register on Haiku. |
| `WebExtensionContextHaiku.cpp` | Native construction, JSON state/errors, storage access levels and origin migration compile. Common load/unload/reload and storage invalidation were extracted from Cocoa. Native background/page identities, install metadata, script/rule state and UI delegates remain. |
| `WebExtensionStateHaiku.cpp` | Atomic JSON state-file helper passes 33 native checks. It contains no context/controller object ABI. |
| `WebExtensionURLSchemeHandlerHaiku.cpp` | Native queued task cancellation, permission checks, resource reading/localization and response headers compile. Full scheme-serving runtime tests require the feature-enabled engine closure. |

The header now declares the native extension constructor and maps native
view/navigation types to `WebPageProxy`, `API::PageConfiguration` and
`API::NavigationAction`. Native state uses a JSON object and `State.json`;
background ownership uses `RefPtr<WebPageProxy>`. GLib state declarations
are limited to their feature guard. Native view configuration is nullable,
matching unloaded-context semantics. State persistence and common lifecycle
sequencing are implemented, but native background construction and several
load/unload dependencies remain. Native scheme tasks use a pending-task set;
Cocoa retains its NSBlockOperation map. Native menu/delegate work remains.
Adding only an empty controller platform hook cannot supply this runtime.
The first lifecycle fixture against matching dependencies can use a manifest
without scripts or background content to verify registration, duplicate rejection, rollback,
unload/reset and profile persistence. That establishes lifecycle behavior;
content-script/background execution still requires the proxy, binding and
page integration gates below.

| Required port work after the manifest-core compile | Existing implementation to preserve |
| --- | --- |
| Context construction, load/unload and state | The load/unload/reload sequence now lives in common `WebExtensionContext.cpp`, with platform metadata and Cocoa tab-map helpers. Native JSON persistence, storage access levels and origin migration compile. The background adapter and saved-script restoration now compile; see [registered scripts](webextensions-native-registered-scripts.md). Install/update metadata, tab population, rules and runtime verification remain. |
| Required load ordering | The extracted common `load` path calls `controller->dispatchDidLoad(*this)` before `addInjectedContent()`, so the WebProcess knows the extension world before a content script runs. Preserve this order and unloaded-context checks in asynchronous callbacks. |
| Resource scheme | `WebExtensionURLSchemeHandlerHaiku.cpp` now uses cancellable main-loop tasks and WebCore responses/errors. Its native compile pass covers controller lookup, same-origin/`web_accessible_resources` checks, required extension base URL, CSP, MIME type, localization and identical permission failures for absent/inaccessible extensions. Prove those behaviors through the real task/IPC path once the full engine links. |
| Scheme registration | `WebExtensionMatchPattern.cpp` defaults to `webkit-extension`; the unchanged `registerCustomURLScheme` body now lives in `WebExtensionMatchPatternProcessPool.cpp` and updates match-pattern sets, service-worker-client registration and existing WebProcesses. Use the existing base URL mechanism. Chrome/Firefox URL identity expectations require explicit compatibility tests before adopting additional schemes. |
| Background documents/workers | GLib's `loadBackgroundWebView` configures MV2/MV3 preferences, foreground process activity, load/failure/process-exit handlers and either URL loading or worker loading. The Haiku adapter now creates hidden pages through the native `WebView` page client, with matching configuration, identity-checked callbacks, foreground activity and unload logic; see [native background evidence](webextensions-native-background.md). Its page/client, context and UI proxy units compile. Native listener state now passes 44 helper assertions in each of two enum layouts; see [listener-state evidence](webextensions-native-background-listeners.md). Install metadata and runtime verification remain. Cancellation and post-load draining changes are recorded in [deferred replies](webextensions-native-deferred-replies.md). `WebPageProxy::loadServiceWorker`, implemented in common `WebPageProxy.cpp:18701`, is the worker entry point. Retain persistent/event-background behavior, listeners, wake-up tasks and unload timers. |
| WebProcess context lifecycle | Constructors, `getOrCreate`, state updates, permission expiry, localization parsing and receiver registration now live in common `WebExtensionContextProxy.cpp`. The extraction preserves privileged identifier distribution and frame/page tracking. Script-error formatting and recording now exist in common UI context code. The process-side send and its loaded-context-validated message now compile in common code alongside the native binding hooks. |
| JS world binding | The namespace/world installation and navigation-event sends now live in common `WebExtensionControllerProxy.cpp`. Haiku page/frame hooks invoke them, including the service-worker and pre-user-script paths. Both proxies, the page and frame loader compile; actual namespace installation still needs a linked context and browser attachment. |
| Content scripts and permission changes | Common `WebExtensionContext.cpp:1221` onward converts injected content to `API::UserScript`/`API::UserStyleSheet`, computes granted patterns and installs them through `WebUserContentControllerProxy`. `toContentWorld` chooses the extension's isolated world. Permission-change broadcasting and several removal/revocation side effects still live in Cocoa. Preserve those transitions as well as initial grants. |
| Core API messages | The event API, UI listener bookkeeping and generated C++ event binding now compile with their validated IPC messages. The port API and its generated C++ binding now compile too; native page-port ownership passes 22 checks. The runtime reply dispatcher also compiles, and its JavaScript helper passes 51 native assertions; see [message reply evidence](webextensions-native-message-replies.md). Sender and tab conversion now compile in common C++, with 27 native tab-conversion assertions; see [message metadata](webextensions-native-message-parameters.md). The UI message handlers, process sends, outgoing port routing and regenerated receiver now compile; see [routing evidence](webextensions-native-message-routing.md). Native tab/background dependencies, runtime verification and storage bindings remain. See [port scope and evidence](webextensions-native-ports.md). Convert the remaining bindings through `UseCPPAPI`, with `JSONValue`/WTF types and existing callbacks. `JSWebExtensionWrapper.cpp` is now included by `PlatformHaiku.cmake`. Move generated interfaces between CMake lists when their IDL output changes. The namespace exposure, API methods, context messages and message validators must agree. |
| Native UI APIs | Port action, tabs/windows, commands, menus, permissions, notifications and associated events against Summit's real browser objects. Returning success for an unimplemented delegate would conceal an extension compatibility failure. |
| Network APIs | Rule removal and DNR-state unload now live in common C++ and compile natively; actual removal is unverified. Reuse common content-rule and SQLite stores, port the existing DNR translator's semantics, and connect the controller's request event hooks in `WebPageProxy.cpp:12504` onward. The Cocoa controller and API handlers remain necessary source references. Verify redirects, headers, cancellation and private sessions through the Curl NetworkProcess; notification hooks alone do not establish blocking WebRequest. |

Native ZIP/XPI resource loading is implemented in
`UIProcess/Extensions/haiku/WebExtensionArchiveHaiku.cpp` using libzip 1.11.4
in the recorded tests. `PlatformHaiku.cmake` requires `libzip>=1.11` only
when extensions are enabled. The native resource constructor preserves
directory loading, extracts stored/deflated ZIP or ZIP64 into a newly owned
private directory, and transfers cleanup ownership only after upstream
manifest parsing succeeds. The helper's **111 native checks** cover valid
packages, resource preservation, traversal, links/special entries,
corruption/encryption, expansion bounds and temporary-directory cleanup.
Those tests do not instantiate WebExtension or run its manifest parser.

The native loader does not call the upstream CRX-prefix conversion or the
non-Cocoa `FileSystem::extractTemporaryZipArchive` stub. The later
[CRX3 importer](extension-crx.md) verifies signatures before passing only the
authenticated ZIP slice to this extractor. Generic self-extracting prefixes
and other formats remain unsupported. The subsequent native manager adds
consent, durable installation and approved startup; automatic updates, store
publisher authentication and broader extension compatibility remain unfinished.

Native resource containment now uses explicit canonicalization failure,
separator-aware ancestry and correctly escaped file URLs. Its **27 native
checks** cover sibling-prefix and symlink escapes, encoded/raw NUL, traversal,
Unicode and percent filenames. The common non-Haiku method also uses
`FileSystem::isAncestor` in place of the original textual-prefix check.
Archive extraction uses descriptor-relative operations; the resource-path
tests establish static canonical containment, not atomic protection against
concurrent filesystem replacement between checking and opening a resource.

Safari WebExtension resources can enter the same upstream manifest/runtime
after locating their bundle resource directory and preserving their identity
and metadata. Cocoa's `WebExtensionCocoa.mm` additionally uses `NSBundle`,
Security.framework resource validation and macOS-specific bundle metadata;
these need native import/verification equivalents where relevant. Native
macOS Safari App Extensions contain native platform code and use Safari/macOS
APIs. Running their WebExtension resources is not evidence that Summit runs
those native components. That part of the full Safari support objective still
requires a separate platform compatibility implementation or demonstrated
equivalent behavior; it cannot be silently marked solved by this route.

The first runtime acceptance fixture should load an MV2 extension and an MV3
extension through the real controller, load an ordinary HTTP fixture page,
observe an isolated content script, exchange a message with a background
document/worker, round-trip extension storage, revoke a host grant, unload and
restart. It should prove that a page cannot call privileged APIs and that two
extensions cannot share data or privileged identifiers. Subsequent fixtures
cover popups/tabs/windows, navigation, subframes, workers, private windows,
network filtering, crashes and update migration. This progression is a way to
reach the full runtime, not a replacement for the real Safari/Chrome/Firefox
extension compatibility corpus required by the objective.

Existing upstream tests provide expectations to adapt rather than invent:
`Tools/TestWebKitAPI/Tests/WebKit/WKWebView/glib/TestWebKitWebExtension.cpp`,
`TestWebKitWebExtensionContext.cpp` and
`TestWebKitWebExtensionMatchPattern.cpp` cover the developing GLib facade.
The adjacent `WKWebExtension*.mm` tests cover the broader Cocoa runtime and
API families, with helpers under `Helpers/cocoa/WebExtensionUtilities.mm`.
Port their assertions and fixtures to the native embedding while retaining
API behavior; merely reproducing the small GLib test surface would leave most
of the browser's requested extension compatibility unverified.

## Common popup and tab page associations

The context now builds popup/tab page identifier tuples and registers popup
and extension-tab pages in common C++. These four bodies are unchanged from
Cocoa. They preserve destination-process page IDs, optional tab/window IDs,
and notification of every content process belonging to a page.

`.vm/extension-lifecycle-inputs.y4RuJo8n/result.json` records successful native
compilation of the final context unit with both extension features enabled
and regenerated IPC. Native inputs and configuration remain unchanged; the
source hash matches the promoted candidate. Body hashes are recorded in
`.vm/extension-page-associations-extraction.json`.

This does not implement native tab/window delegates or run page-association
IPC. Popup creation, tab lookup and browser attachment remain incomplete.
Cocoa compilation has not been run.

Promoted patch: `b85b0ea4c1b19d2bf6b0c421e1916c5a7d90e6f183f0888f1995bd4320763101`.
Probe baseline: `a3f30d1b01848597f29ffa1009bc53cee272ecb4dcc48fa09631d8e7fac2e469`.
