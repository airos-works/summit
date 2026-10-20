# Native cookie event port

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The Curl cookie database now produces typed, populated change batches after
successful SQL commits. A native run-loop timer removes expired persistent
cookies while a database observer is registered. The 48-file implementation
is integrated as engine patch `64e36654...`. That first full build reaches linking
with the cookie dispatcher resolved and two missing dependencies. Subsequent
integration completes the full engine link. The first real NetworkProcess/API
observer test at `96ce44b4...` confirms initial typed event delivery, then exposes
lost observation after rapid unregister/register: **13 checks pass and one fails**.
See [current runtime evidence](webextensions-native-runtime.md). **No extension
JavaScript listener has received these events in a runtime test.**

`CookieChange` carries the stored cookie, a removal flag and the backend cause.
The database handles mutations as follows:

| Mutation | Committed notifications |
| --- | --- |
| Insert | Explicit addition with the actual stored row |
| Overwrite a live cookie | Removal of the old cookie with overwrite cause, then explicit addition |
| Replace a live cookie with an expired cookie | Removal of the old cookie with expired-overwrite cause |
| Remove an already expired cookie | Removal with expired cause |
| Replace an expired predecessor | Expired removal, then explicit addition with a new creation time |
| Explicitly delete live cookies | Explicit removal for each deleted row |
| Missing row or failed mutation | No change notification |

Overwrite pairs travel in one ordered batch. The stored write receipt is
captured before invoking the observer, so a callback that writes another value
cannot change the outer operation's result. SQL statements and transactions
finish before callbacks run. The handler stays alive during delivery even if
it unregisters itself or destroys the database.

Writes validate SQL binds, affected-row counts and the stored result. Deletes
select the actual rows and remove them in one transaction. A partial deletion,
ignored write, failed replacement insertion or invalid stored receipt rolls
back without publishing changes. Exact-key deletion preserves empty paths,
leading-dot domains and IPv6 identity. HTTP and accepted script writes use the
same observer path.

Expiry uses wall-clock time. The timer targets the earliest persistent cookie,
rechecks at least once per minute while persistent rows exist, and retries SQL
failures after one second. Session cookies are excluded. Unregistering the
observer cancels its timer; changing the accept policy reschedules or stops it.
Startup session cleanup does not publish fabricated events. No cookie eviction
policy or eviction cause is implemented.

## Verification

The combined database, write-parser, observer and timer helper passes **227
native checks** in Haiku/QEMU. It compiles 15 production/generated/test units and links
the existing WebCore archive, real SQLite and the native WTF run loop. The
scheduled-expiry test receives an event without another cookie operation,
verifies the retained session cookie, cancels another observer and destroys
the database from its expiry callback. The crash log is empty; inputs and
libraries remain unchanged. There are no compiler warnings in this run.

The native IPC test encodes actual database change batches with the production
encoder and complete generated serializers, then decodes them with the
production decoder. It checks every cookie field explicitly: Curl's
`Cookie::operator==` compares only identity and the secure flag. It also rejects
every truncated packet prefix, an invalid cause and an invalid session. These
are codec tests in one process, without NetworkProcess or extension receivers.

The full path and API helpers have **24 unique native integration units**
compiled. The broader validation includes **136 JSC/query/changeInfo checks**,
**40 host-grant checks** and **431 binding checks**. Both native event receivers
and the full generated network serializer compile. Source/header hashes match
the integrated implementation. The full build completed CMake configuration,
WebCore and WebKit compilation, then failed linking on the two dependencies
above. It introduced no missing symbols or compiler errors. Five existing
`#import` deprecation warnings came from `WebExtensionCommand.cpp`.

Evidence:

- `.vm/cookie-store.gQCSVte6/result.json` — earlier observer version, 195 checks
- `.vm/cookie-store.pHZLpgRS/result.json` — earlier timer version, 200 checks
- `.vm/cookie-store.ogftUPTK/result.json` — earlier database version, 209 checks
- `.vm/cookie-store.PqEyQzwL/result.json` — current database and IPC tests, 227 checks
- `.vm/extension-cookie-parameters-tests.S3O8ggPP/result.json` — 136 JSC checks
- `.vm/extension-lifecycle-inputs.vEzg0JG6/result.json` — corrected path and receivers
- `.vm/extension-lifecycle-inputs.mmqQpxpj/result.json` — current ownership/restart path
- `.vm/extension-cookie-delivery-validation.json` — current 48-file source audit
- `.vm/audit-cookie-delivery.py` — audit reproducer
- `.vm/extension-cookie-api-promotion.json` — integrated patch and source hashes
- `.vm/modern-extensions-cookie-delivery-build-result.json` — completed full build attempt
- `.vm/modern-extensions-cookie-delivery-build.log` — full build log

## Connected delivery path and remaining validation

The Curl observer adapter passes typed batches to WebCookieManager, which sends
them to NetworkProcessProxy and the API cookie-store observers. The proxy
requires the sending process to own the registered data store. Adding a session
after process recreation reinstalls existing observers. Replacing a Curl
cookie database detaches observation from the old database and attaches it to
the replacement. Observer iteration tolerates removal during callbacks.

Each extension context checks its loaded state, API permission, store identity,
private-store access and cookie host grants before scheduling background wake.
It rechecks the load identifier and access before dispatching the ordered
batch. The renderer requires the cookie permission and constructs a fresh
`changeInfo` and nested cookie object for each listener. Native `cookies.onChanged`
is exposed in generated bindings. Generic cookie-store clients still receive
one invalidation per batch; they do not cause duplicate extension events.

These UI, restart, private-store and asynchronous permission paths have compiled
but have **not run in a full engine**. The completed full build attempt on
`64e36654...` confirms that the typed cookie dispatcher resolves, reducing the
missing dependency count from three symbols/seven references to two/six.
Partitioned cookies, Firefox container/first-party isolation, eviction and
network SameSite enforcement remain incomplete.

To reproduce the codec test after the native engine is built, generate an
isolated native stage with the lifecycle compile tool:

```sh
python3 tools/test-engine-extension-lifecycle-compile.py \
  --engine-root /boot/home/summit-webkit-extensions \
  --overlay .cache/WebKit --regenerate-ipc \
  --unit GeneratedSerializersSharedWebCoreArgumentCodersNetwork.cpp
```

Use that run's native stage path when exporting its generated outputs. Pass the
export directory to the cookie harness:

```sh
python3 tools/export-engine-ipc-generation.py --stage <native-stage>
python3 tools/test-engine-cookie-store.py --overlay .cache/WebKit \
  --with-api-parser --ipc-generated <export-directory>
```

The harness checks source, generator-output and library hashes and requires an
up-to-date WebCore archive. These commands regenerate test inputs; they do not
establish full-engine delivery to listeners.
