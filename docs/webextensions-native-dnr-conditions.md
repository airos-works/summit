# Compound URL conditions for native declarative request rules

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

WebCore's content-rule compiler now has a representation for independent request,
top-page and frame URL conditions. This supplies the matching foundation for the
native DNR translator. The extension ruleset loader is still unfinished, the full
extension-enabled WebKit has not linked, and no extension has run in Summit.

A trigger can contain `url-conditions`, an array of one to six groups. Each group
has a `type`, a non-empty `urls` array of regular expressions, and an optional
boolean `case-sensitive`. The six types are `if-request-url`,
`unless-request-url`, `if-top-url`, `unless-top-url`, `if-frame-url`, and
`unless-frame-url`. Types cannot repeat. Unknown properties, incorrectly typed
values, empty patterns, non-ASCII text and embedded NUL are rejected. Regex
support is checked by the real WebCore compiler.

Patterns within one group form a union. All groups must accept the request:
inclusion groups require a match and exclusion groups require no match. An
exclusion removes only its own action. It does not introduce an
`ignore-following-rules` action that could suppress unrelated rules.

The existing `url-filter` is still required. An optional non-empty
`url-filter-alternatives` array supplies additional base patterns. All base
patterns share the trigger's case flag, request flags, conditions and serialized
action location. This is needed for the URL translator's separator/end-of-URL
alternatives, including actions such as appending a header that cannot safely
run twice.

The backend removes repeated serialized action locations when merging unflagged
universal and URL-specific matches. This prevents an overlapping alternative
from repeating an action. Distinct conditional rules retain separate locations
and remain independently effective. The real pipeline test below exposed and
verified this fix; the earlier helper tests combined matches in a set and missed
that backend boundary.

Legacy top/frame URL conditions remain supported. A trigger cannot combine
legacy conditions with the new groups. Each new group's case sensitivity is
independent. Trigger hashing, equality and cross-thread copies include the new
fields.

The compiler stores six condition selectors in bits 25–30 of its action flags.
Bit 31 remains unused so encoded action tags cannot collide with the hash
table's deleted-value sentinels. The low 32 bits retain the serialized action
offset. Request condition tags share the existing request bytecode stream;
top-page and frame tags use their existing streams. The backend collects request
tags before removing any, evaluates the conjunction, and removes all auxiliary
tags before action deserialization. Collection order cannot change the result.

The content-rule cache version advances from 21 to 22 because the meaning of
compiled flags has changed. The file's three bytecode sections remain in place.
Existing compiled rules require recompilation through the store's version check.

Two related fixes preserve condition evaluation. Interpreting top/frame filters
with `AllResourceFlags` now treats the request method as unrestricted; previously,
a legacy condition carrying a specific method could never match. Real requests
still require an exact method match. Top/frame caches now distinguish an
uninitialized cache from a cached null URL, so the first empty-URL lookup cannot
skip evaluation.

The native harness compiles production condition parsing, trigger compilation,
condition compilation and backend filtering with WebCore's actual NFA/DFA
compiler and bytecode interpreter. It links frozen JavaScriptCore/WTF and ICU
without linking the full WebCore or WebKit libraries. It checks all 4,032
combinations of non-empty condition requirements and six match results, plus
actual-bytecode cases for exclusions, overlapping alternatives, method/resource
flags, case sensitivity, universal patterns, empty input, malformed groups and
cross-thread ownership. It records input and library hashes and native crashes.

These tests do not execute the full rule-list JSON parser, cache object, action
deserializer, extension loader, persistence, IPC or network enforcement. Separate
native integration compilation covers the parser, compiler, cache, backend and
rule-list store. DNR action priority, domain-field translation, redirects, header
changes and loader integration remain separate work.

Validation: **76 native checks and all 4,032 condition comparisons pass**, with
zero disagreements or native crashes. All eight integration units compile with
the configured engine flags. Source, configuration and library snapshots remain
unchanged. The final audit matches 292 source/header hashes.

Evidence: `.vm/extension-dnr-conditions-tests.RVgG6S0U/result.json`,
`.vm/extension-lifecycle-inputs.U8ZXkCnl/result.json`, and
`.vm/extension-dnr-conditions-validation.json`. The probes used baseline patch
`4777dc25ab9f24d84579c57fb0a19a258a4c6e536725eb70d9f8fa1f899fb90e` with
isolated extension-enabled configuration and candidate source snapshots.

Promoted engine patch:
`656209ed1a5461930864b0bc5ee25d29edce7c026f34754e2f4397797a5d16f0`.
The full extension-enabled build rebuilt WebCore and reached WebKit's library
link. It failed on the same 17 unresolved symbols as the previous build, with
none newly introduced. Evidence:
`.vm/modern-extensions-dnr-conditions-build-result.json`.

A stronger pipeline test links the rebuilt WebCore archive and exercises
the real rule-list parser, compiler, URL caches, backend, action deserializer and
a header change on an in-memory `ResourceRequest`. On the initial `656209ed...`
patch it reported **84 passing checks and two failures**, both from unflagged
universal/specific
overlaps returning the same action twice. The other cases cover compound clauses,
distinct conditional actions, legacy methods, first null-URL cache lookups and
cache transitions, parser rejection, and compiler failure without finalization.
There were no native crashes, and input/library hashes stayed unchanged.
Evidence: `.vm/content-rule-pipeline.72m2NrMW/runtime-result.json`.
This test does not link WebKit or run an extension, persistent rule store, IPC,
browser or network request.

The backend deduplication fix is in patch
`dbfa7e97e15983e2e645a9b200e91472f3213239d3b7ae0157463e6ef1f2a7cc`.
The identical pipeline source now passes **all 86 checks**, with no native crashes
and unchanged inputs/libraries. Evidence:
`.vm/content-rule-pipeline.rzdrhT4E/result.json` and
`.vm/extension-dnr-action-union-validation.json`. The incremental full build
recompiled the backend's unity unit and rebuilt WebCore; WebKit's link still
fails on the same 17 unresolved symbols. Evidence:
`.vm/modern-extensions-dnr-action-union-build-result.json`.

Run the pipeline after rebuilding the feature-enabled WebCore archive:

```sh
python3 tools/test-engine-content-rule-pipeline.py
```

The tool also supports compiling the harness with `--compile-only` and later
resuming the printed native staging directory with `--resume`. It verifies that
the archive is up to date, records 850 compiler input dependencies for this test,
and hashes the actual linked archive and explicit library inputs. A failed check
or native crash makes the run fail.
