# Native cookie API port

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The port connects `cookies.get`, `getAll`, `set`, `remove` and
`getAllCookieStores` to Haiku's real website data stores and Curl cookie database.
It includes generated C++ bindings, privileged context IPC and populated
`cookies.onChanged` delivery. The 48-file implementation is integrated as patch
`64e36654...`. **No extension has executed these APIs.** Full engine compilation
reaches linking with two missing symbols (six references): the DNR loader and
menu-click dispatcher. The cookie dispatcher is defined and resolves, with no
new missing symbols or compiler errors. The engine has not linked successfully.

Receivers require a loaded privileged context and the `cookies` API permission.
Host checks use existing grants, ignore match-pattern paths, honor expiration
and denial, and exclude tab metadata and temporary tab grants. `get` and
`remove` authorize the requested URL. `getAll` additionally filters each cookie
by its own origin. Explicit-host versus wildcard-host precedence follows the
context's existing policy. Store lookup rejects inaccessible private stores;
the extension's own default store retains the pinned WebKit exception.

Asynchronous reads recheck the current privileged load identifier, API
permission, store identity, private access and requested host before returning
data. Removal repeats the checks before deleting the selected cookie and when
processing the result. Writes validate their URL and domain in both processes,
then repeat access checks on completion. A write already dispatched to the
network process may have committed before permissions change; the callback
checks do not undo that mutation.

The write IPC carries the original details JSON rather than trusting a cookie
constructed by the renderer. The parser validates domain boundaries and public
suffixes, cookie flags, secure prefixes, finite expiry and full-width store IDs.
It normalizes international domains with WebKit's URL parser and keeps IPv6
identity consistent with Curl. Zero and negative expiry values delete expired
cookies rather than creating session cookies. The database returns the actual
stored row or a failure, and writes retain WebKit's cookie-version barrier for
dependent resource loads. Removal returns only `url`, `name` and `storeId`.
Missing cookies return null, following the pinned implementation.

This remains an incomplete cookie implementation. Native `onChanged` is now
exposed and connected through typed IPC, database observation and context
permission checks. Database events, expiry and the production IPC codecs have
run in native tests. Network-to-UI delivery and extension listeners have not.
Partition keys and Firefox first-party isolation (`firstPartyDomain`) remain
unfinished; container stores (`firefox-container-<id>`, the profile's store as
`firefox-default`) work since 30 October 2026, see
[webextensions-containers.md](webextensions-containers.md). WebCore still folds an omitted SameSite
attribute into `None`; explicit `unspecified` is rejected. SameSite metadata
storage does not establish SameSite enforcement on network requests (added
separately on 20 October 2026, [networking.md](networking.md)). Unknown
query/write dimensions are rejected instead of silently broadening scope.
Cocoa retains adapters to its existing dictionary implementations; no Cocoa SDK
compile or runtime has been performed.

## Verification

- **227 native parser/database/change/IPC checks** pass using real SQLite and the existing
  WebCore archive, including domain and public-suffix rejection, IPv6 URL
  lookup, HTTP response cookies, expiry, persistence, SQL failure injection,
  committed change batches, rollback, real Haiku expiry-timer delivery, complete
  production IPC round trips and malformed-packet rejection. These codec tests
  run in one process; they do not invoke NetworkProcess or extension receivers.
- **136 native JavaScript conversion/query checks** pass against frozen
  JavaScriptCore/WTF, including removal result shape, session expiry omission,
  exact store identifiers, populated changeInfo, cause spellings, independent
  nested result objects and garbage collection.
- **40 native host-access checks** pass with the actual WebExtension match
  pattern code, including path and port handling, host boundaries, IPv4/IPv6,
  grant removal, deadlines and denial precedence. This isolated helper adapts
  API-object initialization to real JSC/WTF initialization; it does not create
  an extension context or test private stores, API permission or lifecycle.
- **431 binding generation/preprocessing checks** pass for the Haiku and Cocoa
  surfaces. Twenty-one of the 37 interfaces now generate C++ bindings in the
  candidate.

The connected API/event path and helpers have **24 unique native integration
units** compiled, including both event receivers and the full generated network
serializer. The latest network-process ownership and observer-restart changes
also compile. The source audit covers all 48 files. Full-build CMake
configuration, WebCore and WebKit compilation have run; the full link remains
incomplete on the two dependencies above.
The Cocoa adapter has only been source-reviewed. No API IPC, asynchronous permission change,
private-store access, process restart or extension/browser runtime was tested.

Evidence:

- `.vm/cookie-store.PqEyQzwL/result.json` — current 227 database/parser/change/IPC checks
- `.vm/extension-cookie-parameters-tests.S3O8ggPP/result.json`
- `.vm/extension-cookie-host-access-tests.HSWexkFR/result.json`
- `.vm/extension-bindings-generated-eu3ohp6a/binding-generation.json`
- `.vm/extension-binding-platforms-_2tzrsnq/result.json`
- `.vm/extension-lifecycle-inputs.1Le3QTKc/result.json` — initial API compilation
- `.vm/extension-lifecycle-inputs.z80NxwRY/result.json` — corrected/current units
- `.vm/extension-lifecycle-inputs.w9Etatc7/result.json` — final store-to-network units
- `.vm/extension-lifecycle-inputs.fPIOJ700/result.json` — first event compilation
- `.vm/extension-lifecycle-inputs.vEzg0JG6/result.json` — corrected path and receivers
- `.vm/extension-lifecycle-inputs.mmqQpxpj/result.json` — current ownership/restart path
- `.vm/extension-cookie-delivery-validation.json` — current aggregate audit
- `.vm/extension-cookie-api-promotion.json` — integrated patch and source hashes
- `.vm/modern-extensions-cookie-delivery-build-result.json` — completed full build attempt
- `.vm/extension-cookie-api-validation.json` — historical 33-file API audit

The [backend port](webextensions-native-cookie-backend.md) records the
database migration, metadata and mutation-result implementation.

The [cookie event port](webextensions-native-cookie-changes.md) records
committed mutation events, automatic expiry and the remaining delivery checks.
