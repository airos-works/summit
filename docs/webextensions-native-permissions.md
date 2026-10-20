# Native permissions API

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The Haiku port implements the upstream `browser.permissions` methods `getAll`,
`contains`, `request` and `remove`, and dispatches `onAdded` and `onRemoved`.
The generated bindings use C++ JSON values, with the existing Cocoa methods
retained behind conversion wrappers. This is implementation and compile
progress: **no extension has executed these APIs in an extension-enabled
browser yet**.

## Consent and validation

Every native permission IPC requires a loaded privileged extension context.
The web process requires a user gesture for `request`; the UI process checks
the gesture flag, supported permissions, match-pattern syntax and manifest
declarations again. A declaration alone never grants access. Origins are
canonicalized to all paths because permission checks ignore paths. Explicit
file access requires the browser's separate file-access setting.

Missing permissions and hosts form one all-or-none request to the controller's
[native consent broker](webextensions-native-permission-prompts.md). Already
granted capabilities are omitted from the prompt. Allow makes only the
displayed capabilities permanent; it does not extend existing temporary
grants. Deny and closing the dialog return false. Queue, host, timeout,
cancellation and persistence failures produce API errors.

When consent completes, the context rechecks its privileged load identifier,
manifest and revocation generation. Revocation or denial invalidates pending
prompts. A response from an earlier load cannot change a reloaded context.
Requests also recheck the resulting grants after commit, so expiration during
a disk write cannot silently be reported as retained access.

`remove` rejects required permissions and host patterns overlapping required
origins. It removes optional grants covered by the requested patterns. An
empty or already absent optional set succeeds. A narrower removal that leaves
access through a broader grant returns false. This differs from Chromium's
unconditional success response after a valid removal; it follows the result's
meaning of whether the requested access remains granted.

## Saved grants and events

The existing extension state file contains a versioned `PermissionGrants`
object. Permission names and canonical origins map to expiration timestamps;
JSON null means permanent. Unknown schema versions, invalid identifiers and
malformed expiration values are rejected. Expired grants are discarded.

A permission API transaction copies unrelated state, prepares the next grant
snapshot, and writes it with the native atomic state-file writer before
changing live grants. A write error retains the previous live state and
returns an API error. Nonpersistent contexts keep state in memory. Ordinary
host setters retain their existing void contract; normal state writes and
unload snapshot their current grants, with write errors logged.

On first load, saved grants are filtered against the current manifest,
supported capabilities, expiration and host denials. Explicit host replacement
setters and revocations made before load override saved grants. Reloading the
same context preserves its current host/API grants and filters them again.
Unreadable or corrupt state stops loading before a controller is attached or
metadata can overwrite that file.

Transactions update both permission categories before sending combined
`onAdded`/`onRemoved` events. The existing CORS, content injection, clipboard and
native observer effects still run. Event payloads use the actual installed
maps, including grants that expire during a write. Delivery checks the load
identifier after waking background content. Each JavaScript listener receives
a fresh rooted JSON object.

## Verification

- **90 native checks pass** for the actual permission-details parser, saved
  grant codec and atomic state-file writer. Tests cover malformed input,
  duplicate entries, NUL rejection, schema versions, expiration, independent
  state copies, unrelated settings, disk reload, revocation, private file mode,
  failed writes and corrupt files. Source/library hashes are unchanged and
  native crash-log coverage records no events.
- **Nine additional native translation units compile**, covering the UI API,
  context lifecycle, permission notifications, web-process API and namespace,
  generated permission and namespace bindings, and regenerated privileged IPC.
  The helper run also compiles the final saved-state implementation.
- **120 generated-binding checks pass**, including actual C++ preprocessing
  of the permission methods, events and namespace exposure for Haiku and
  Cocoa. Sixteen of 37 extension interfaces now generate C++ bindings.

Evidence is recorded in `.vm/extension-permissions-validation.json`, with
native helper results in
`.vm/extension-permission-state-tests.9aZEY2v3/result.json` and generated-binding
checks in `.vm/extension-binding-platforms-fg4tnlmj/result.json`.

The runtime helper links frozen JavaScriptCore/WTF and ICU. It does not link
WebKit or WebCore and does not instantiate a context, call the SDK, show the
consent dialog, execute permission IPC or run an extension. The separately
verified broker/dialog has 165 native checks. Those separate checks do not
establish end-to-end permission behavior. Cocoa SDK compilation is untested.

The latest Chrome host-access-request methods and Firefox data-collection
permission fields are outside the pinned upstream interface. Unsupported
details fields are rejected rather than treated as an empty successful
request. Packaging, installation/update consent, a native extension manager,
full API coverage and actual extension compatibility remain unfinished.

## Full engine build

The full native build of patch
`cf7f9b4e1f7360a5fc273bbc97724e33c56fe7494bea227e3287a4a5474a3314`
reaches the WebKit library link with **14 missing symbols (18 references), down
from 15**. The permission event dispatcher is resolved, with no new missing
symbols and no compiler errors. The DNR loader and 13 other event dispatchers
still prevent an extension-enabled browser from linking.

Both native source manifests match the patch. The feature configuration has
WebExtensions and content extensions enabled; the separate probe baseline
remains disabled. The terminal build result and exact unresolved symbols are
recorded in `.vm/modern-extensions-permissions-build-result.json`.
