# Native extension state and resource adapters

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The 2026-09-14 port adds native context state and resource-serving implementations
and extracts more of WebKit's lifecycle from Cocoa. **No extension runs in Summit
yet.** The ordinary browser keeps `ENABLE_WK_WEB_EXTENSIONS=OFF`; the separate
feature-enabled engine build has not linked successfully.

The common `WebExtensionContext.cpp` now owns load/unload/reload, storage
invalidation, storage access-level broadcasting and script-error formatting.
The lifecycle extraction preserves controller/world attachment, asynchronous
unloaded-context checks, storage migration, script/rule/background hooks and
the order of `dispatchDidLoad` before `addInjectedContent`. Cocoa-specific
metadata access and tab-delegate-map cleanup remain in small Cocoa helpers.

`WebExtensionContextHaiku.cpp` implements construction, error deduplication,
JSON state, stored base URL/display name, storage access levels and local
storage/IndexedDB origin migration through the profile's real WebsiteDataStore.
Construction initializes in-memory state before public storage-level setters
can use it. Native page configuration uses nullable `RefPtr<API::PageConfiguration>`.
The controller uses the common RunLoop for its existing five-second startup
window and registers both normal and privileged receivers on Haiku.

The native scheme handler queues work on the main loop and retains a pending
task set so stopped tasks cannot begin resource reads. Its port preserves the
requesting-frame fallback, same-origin and `web_accessible_resources` checks,
extension-page configuration restriction, tab-page registration, CSS localization,
CSP, MIME type and content length. Missing/disabled extensions and permission
denials return identical errors. Responses, data and completion use the existing
WebURLSchemeTask API. These behaviors have compile evidence, not scheme/IPC runtime
evidence. The required controller, tab and page implementations remain unfinished.

The state-file helper has a separate runtime gate. It only uses WTF JSON/file
APIs, with no WebExtension/context/controller objects and no WebCore or WebKit
library dependency. It reads an absent file as an empty object and reports
invalid paths, read errors and invalid JSON separately. Writes use a private
temporary file, flush its contents and rename it in the same directory. A
failed replacement removes the temporary file and preserves the destination.
The native context uses this tested helper directly.

The **33 native checks** cover missing/empty/truncated/non-object/invalid-UTF-8
state, Unicode paths/data, nested storage levels, mode 0600, replacement errors,
temporary-file cleanup, symlink destination handling, absolute/NUL path checks,
and concurrent replacements/readers. The passing run made 100 replacements
while the reader observed 3,455 complete objects. It exited normally, preserved
all staged inputs and frozen libraries, and recorded complete native crash-log
coverage with no new debugger event. These checks do not establish power-loss
recovery or profile installation behavior.

Reproduction, after configuring a matching native baseline:

```sh
python3 tools/test-engine-extension-lifecycle-compile.py \
  --unit WebExtensionContext.cpp --unit WebExtensionController.cpp \
  --unit haiku/WebExtensionContextHaiku.cpp \
  --unit haiku/WebExtensionURLSchemeHandlerHaiku.cpp
python3 tools/test-engine-extension-state.py
```

Both tools support candidate source overlays, record exact source hashes, and
use unique native staging directories. The lifecycle tool can also select the
separate configured engine with `--engine-root`; its generated headers must
already exist. It always remains compile-only.

Evidence:

- Native resource-handler compile: `.vm/extension-lifecycle-inputs.dm10CfkI/result.json`.
- Common context/controller compile passes: the corresponding units in
  `.vm/extension-lifecycle-inputs.IeVv2B5g/result.json`. That overall probe remains
  failed because its earlier native state adapter lacked a complete FileHandle
  include; the adapter was corrected and tested separately.
- Final native context adapter: `.vm/extension-lifecycle-inputs.0iK4YLHa/result.json`.
- State-file helper: `.vm/extension-state-tests.5bpLw7jh/result.json`.
- Collected per-unit results: `.vm/extension-native-adapters-results.json`.

Earlier failures remain recorded. Resource-handler preflights against the new
full build first lacked generated WebCore forwarding headers (`kHSjMzww`) and
then WebKit IPC headers (`fboNRK7H`). The existing configured baseline exposed
an rvalue-reference response-constructor mismatch (`pWPCgUVW`), fixed before the
passing run. State-helper preflights exposed the current char8_t path/byte API
requirements (`qG5BEzlQ`, `OZDxwY7h`). The first runtime run (`E0dEIKxt`) passed
concurrent replacement checks but failed empty-file error classification; the
helper now classifies an existing empty file as invalid JSON.

Native controller/client initialization, page/tab/window delegates, background
pages/workers, install/update metadata, listener persistence, registered-script
and declarative-rule state, permission events, JavaScript bindings and browser
attachment still need implementation and integration tests. The extracted
lifecycle calls those real upstream hooks; missing implementations have not
been replaced with successful no-ops. A matching feature-enabled WebKit/WebCore/
generated-IPC dependency closure is required before any context runtime test.

## Damaged state after a power cut (issue #17, 2 October 2026)

uBlock Origin on the X399 showed "Enabled, but not running" with only
"Operation not allowed". Its `State.json` held 702 bytes of another file's
old data (a list of two-factor help pages): it had been written on 1 October
at 15:16:41 through the temporary file, `fsync` and rename above, and the
machine was later cut off. BFS kept the new size and block run but not the
data, so the file system lost data that had been synced; that is reported to
the OS work. Its 20 MB `LocalStorage.db` passed SQLite's integrity check.

A browser-startup restore refused any state that did not name the package's
fingerprint, so the extension could never start again. Now
(`ExtensionPackageRegistryHaiku::load`):

- A state that names a *different* package version is still refused, with a
  reason the Extensions window shows.
- A state that is missing or unreadable is moved aside to
  `State.damaged.json`. If `State.backup.json` belongs to the package it is
  put back ("restored from the copy taken when it last started": grants,
  base URL and privacy settings survive). Otherwise the extension starts
  with the access its installation granted (the requested permissions and
  hosts, `file:` hosts only with local-file access), as the catalog approved
  exactly this package.
- After each start the state is copied to `State.backup.json` on the
  registry's work queue, only when it differs, so the copy is usually long on
  the disk when a cut comes.

Every refusal now carries a description, and a recovery is shown in the
Extensions window and logged as `Summit extension <id>: ...`. Verified on the
X399 with a copy of the owner's damaged profile: uBlock started, blocked
`adsbygoogle.js` (which the X399 itself fetches with curl), a clean restart
logged nothing, and a deliberately damaged state came back from the backup.
