# Native cookie backend port

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The cookie API port exposed missing behavior in the Curl backend. This port
repairs storage metadata, expiry and exact cookie deletion, and implements the
native network session's URL lookup. It is integrated as engine patch
`64e36654...`; its full engine compilation reaches linking, which still lacks
the DNR loader and menu-click dispatcher. Tests use isolated
copies of the candidate with recorded source and library hashes.
Cookie API receivers and bindings are connected in the
[API port](webextensions-native-cookie-api.md). The typed event path is connected
and compile-checked; delivery to an extension remains untested, and no extension
has run.

The real backend baseline ran 17 checks and failed seven. SameSite and creation
time were lost during enumeration and URL lookup; persistent metadata was lost
on reopen; the HTTP parser ignored SameSite; and an expired replacement was
compared against monotonic time instead of wall-clock time. Basic cookie text
persistence across close/reopen passed. The failures were reproduced against
the actual WebCore archive, with no native crashes or input/library changes.

The candidate adds `created` and `samesite` columns through a transaction that
migrates the previous or unversioned schema. Existing cookie values survive.
Previously unrecorded metadata remains unknown. Unsupported future schemas and
malformed current schemas reject access without deleting their rows. A failed
second migration step rolls back the first. Startup session-cookie cleanup
happens only after the database has passed schema and statement validation.

Writes preserve the original creation time when replacing a live known cookie while
updating its value, flags and SameSite policy. An expired predecessor is removed
and a replacement receives a new creation time. Expired replacements remove the
stored row using wall-clock time, and enumeration excludes expired cookies.
URL-query ordering uses path length and creation time. The HTTP parser retains
SameSite values. This does not implement SameSite enforcement on network
requests. Partitioned writes remain unsupported and are rejected by the
structured database writer.

An exact-key deletion method preserves the stored name, domain and path instead
of converting the cookie into a URL. Its runtime tests cover leading-dot domains
and IPv6 identity. The native network-session method now calls that operation;
its URL getter calls the database's real URL search and excludes secure cookies
for HTTP URLs. These two network-session methods have compiled but have not run
through NetworkProcess IPC.

## Verification

The combined backend/write-parser candidate passes **227 native checks**. The
harness compiles `CookieJarDB.cpp`, `CookieUtil.cpp` and the write parser, then
links them before the
existing, up-to-date WebCore archive. It uses real SQLite databases in memory
and temporary files. Coverage includes metadata, overwrite behavior, physical
row deletion, reopen, old-schema migration, rollback, future-version rejection,
malformed-schema rejection, exact cookie-key deletion and mutation results.
Change tests cover ordered overwrite batches, actual old/new values, expiry
causes, observer removal and destruction, nested writes, and rollback after
partial SQL mutation or failed receipt validation. Native run-loop tests
exercise automatic expiry and cancellation. Production IPC round trips verify
every serialized cookie field, ordered mutation batches, all implemented causes,
truncated-packet rejection and invalid cause/session rejection. The IPC codec
test runs in one process and does not execute NetworkProcess/UI receivers.
The expanded checks also cover API input validation and actual IPv6 lookup.
The IP-address helper now handles the brackets retained by WebKit's URL host,
so IPv6 HTTP cookies retain host-only identity.
Real SQLite triggers reject writes and deletes; those failures return no
invented cookie and preserve the original value. All runtime inputs and
libraries remain unchanged, and the native crash log records no events.

A new database method returns the stored row after a successful write, including
preserved creation time and the expiry actually stored by SQLite. Successful
expired replacements return no cookie. A candidate UI-to-network IPC path
returns this result and propagates delete failures, unavailable sessions and
network-process failure as failure. Writes use the existing website-data-store
cookie-version barrier so dependent resource loads retain their ordering.
The database result is tested. The **24 compiled native integration units**
include the generated IPC receiver, API cookie store, website data store,
network cookie manager and network storage session. This does not execute the
IPC path, resource-load ordering or an extension API call.

Separately, **136 native checks** cover the
WebExtension query parser, full-width store identifiers and actual
JavaScriptCore cookie/store/removal conversion. These helpers are now listed
in the candidate engine build and called by its API handlers.

The database now delivers populated change batches after committing SQL and
schedules persistent-cookie expiry on its owning Haiku run loop while observed.
The Curl network adapter and extension dispatcher now carry typed change
batches. UI delivery checks the sending network process, store identity and
extension access; observer setup is repeated after network-process recreation.
Those UI/lifecycle paths have compiled but have not run. These tests do not
establish delivery to extensions. See the
[event port](webextensions-native-cookie-changes.md) for the remaining delivery
checks. The API port connects permission checks and database outcomes, but
asynchronous revocation, private-store access
and actual extension calls still need runtime testing.

Evidence:

- `.vm/extension-cookie-delivery-validation.json` — current 48-file aggregate audit
- `.vm/extension-cookie-events-validation.json` — historical 35-file backend audit
- `.vm/extension-cookie-api-validation.json` — historical 33-file API audit
- `.vm/extension-cookie-backend-validation.json` — earlier 17-file backend audit
- `.vm/cookie-store.ZYYKxjtR/result.json` — baseline, 17 checks / 7 failures
- `.vm/cookie-store.2Cm8xADb/result.json` — earlier backend, 74 checks pass
- `.vm/cookie-store.3pBlh6zo/result.json` — mutation results, 88 checks pass
- `.vm/cookie-store.1Kr8HtTc/result.json` — earlier backend/write parser, 138 checks
- `.vm/cookie-store.ogftUPTK/result.json` — earlier backend/parser/events/timer, 209 checks
- `.vm/cookie-store.PqEyQzwL/result.json` — current backend/parser/events/IPC, 227 checks
- `.vm/extension-lifecycle-inputs.qLKpQ9wr/result.json` — network-session compile
- `.vm/extension-lifecycle-inputs.w9Etatc7/result.json` — earlier five store integration units
- `.vm/extension-lifecycle-inputs.dbnWncMF/result.json` — earlier two Curl database callers
- `.vm/extension-lifecycle-inputs.fPIOJ700/result.json` — current Curl database callers
- `.vm/extension-cookie-parameters-tests.S3O8ggPP/result.json` — current 136 helper checks
