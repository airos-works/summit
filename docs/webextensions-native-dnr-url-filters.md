# Native DNR URL-filter translation

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The Haiku port has a native C++ foundation for translating the URL fields of
declarative request rules into WebCore content-rule filters. This helper is not
yet connected to the DNR ruleset loader. It does not apply actions, register rule
lists, read the rules database, or enforce rules in a browser.

`WebExtensionDeclarativeNetRequestURLFilter::parse` accepts a condition object and
validates `urlFilter`, `regexFilter`, and `isUrlFilterCaseSensitive`. Missing URL
filters match all URLs. Explicit values must have the correct type; filters must
be non-empty ASCII strings without NUL, and the two filter forms are mutually
exclusive. Other condition fields remain the rule translator's responsibility.

URL patterns support literal text, wildcards, left/right anchors, domain anchors,
and separators. The separator token can also match the end of a URL. WebCore's
filter parser rejects regex disjunctions, so the native helper emits alternative
filters for separator suffixes that can finish without consuming a character.
The resulting patterns form a union; the eventual caller must preserve the
same rule action and other conditions across those alternatives, and avoid
applying an action twice when more than one alternative matches.

Domain anchors start at the actual authority or a subdomain boundary. Conversion
tracks whether the pattern is still within the authority or has entered the
path/query/fragment. This prevents the regex from treating text in credentials
as a hostname, while allowing a wildcard to continue into a path containing an
`@` character. Matching uses the URL serialization expected by WebCore, including
punycode hosts and percent-encoded non-ASCII path text.

These URL-pattern semantics follow the [Chrome DNR condition
documentation](https://developer.chrome.com/docs/extensions/reference/api/declarativeNetRequest#type-RuleCondition).
The native implementation validates every resulting regular expression with
WebCore's actual `URLFilterParser`; JavaScript or RE2 validity alone is insufficient.
Unsupported raw-regex constructs return errors. No unsupported expression is
replaced with a match-all expression. WebCore's narrower regex language rejects
disjunctions, lookarounds, built-in classes such as `\d`, and fixed-count repetition.
Its legacy parser treats backreference syntax as octal or literal text, so the
native helper explicitly rejects numeric/backreference escapes and unsupported
alphabetic escapes. Two-digit ASCII hex escapes are accepted, excluding NUL.

The shared WebCore parser also has a correctness fix from these tests. Previously,
it classified an expression with no mandatory consuming character as matching
every URL, even when both start and end were anchored. It now keeps expressions
such as `^$`, `^a*$`, and `^a?$` in the automaton. Empty matches still work, and
expressions such as `^.*$` retain their unconditional-match optimization. This
changes the shared filter compiler, not just DNR translation.

Default native translation limits are 64 KiB of input, 2,048 output patterns and
1 MiB of total generated pattern text. These are resource bounds for this port,
not a claim of Chrome's compiled-regex quota compatibility. Adjacent wildcards
are coalesced. An impossible authority pattern can produce an empty union, which
means no URL matches; it must not become a match-all rule in the caller.

The QEMU harness compiles the production helper together with WebCore's real
URL parser, NFA builder, DFA conversion/minimization, bytecode compiler and
bytecode interpreter. It links frozen JavaScriptCore/WTF and ICU, without linking
the full WebCore or WebKit libraries. Its core objects use `-O1`; a separate native
integration compile uses the configured engine flags. The harness records source,
object, executable, configuration and library hashes and checks for native crashes.

Fixed cases cover URL syntax, serialized internationalized URLs, authority
boundaries, case sensitivity, rejected inputs, and translation limits. Independent
token matching and parsed-host-position oracles check the actual generated
bytecode. These tests do not exercise action priority, redirects, header changes,
request/initiator-domain intersections, content-rule persistence, extension IPC,
or network enforcement. The full extension-enabled WebKit has not linked and no
extension has run in Summit.

Validation: **120 native checks pass**, including **4,032 token-oracle and 2,576
host-position-oracle comparisons with zero disagreements**. Eighteen additional
cases check nullable expressions directly, including empty input and positive
matches. Evidence: `.vm/extension-dnr-url-filter-tests.9b5gCGfT/result.json`.
Both final integration units compile: the native helper and the changed WebCore
parser, with sibling/namespaced WebCore headers staged and hashed.
Evidence: `.vm/extension-lifecycle-inputs.xZnF6Xyr/result.json`.
The source/hash audit is `.vm/extension-dnr-url-filter-validation.json`.

The earlier native run `.vm/extension-dnr-url-filter-tests.PfbH2vw1/result.json`
exposed 168 token-oracle mismatches with the original WebCore classification.
It also caught legacy backreference reinterpretation and an incorrect test
expectation about percent-encoded braces. Those failures are preserved; the final
run exercises the corrected source and expectations.

Patch: `4777dc25ab9f24d84579c57fb0a19a258a4c6e536725eb70d9f8fa1f899fb90e`.
Probe baseline: `341d7bd1bb1d8d5601a45cd83b1dc5af3eeb3b2f4039f7468d1b212ee32d6ef5`.

The full extension build subsequently compiled the changed parser, rebuilt the
WebCore archive, and compiled the native helper. WebKit's shared-library link
failed on the same **17 missing symbols**, with none newly introduced. Evidence:
`.vm/modern-extensions-dnr-url-filter-build-result.json`. This confirms the full
build path for the change; it does not establish ruleset loading or enforcement.
