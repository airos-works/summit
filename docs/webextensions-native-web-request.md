# Web request extension events in common C++

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The WebRequest namespace and event bindings now use C++. Six process-side
resource notification handlers produce the nine existing WebKit request events,
including request headers/body, response metadata, redirects, authentication
challenges, completion and errors. The getter and listener-removal method bodies
are retained from upstream. Listeners are copied before dispatch, and each
callback receives its own JavaScript payload and binary buffers.

Request filters use the actual WebExtensionMatchPattern parser and matcher.
Document requests are classified as main frames or subframes consistently in
both filters and payloads. Filters combine URL alternatives with resource type,
tab and window constraints. Invalid identifiers are rejected; `-1` matches only
unassociated requests. Empty URL/type arrays match nothing. This follows the
filter dimensions described by [Mozilla's RequestFilter reference](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/webRequest/RequestFilter)
and the frame categories in [ResourceType](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/webRequest/ResourceType).

URL-encoded uploads join byte chunks before decoding, retain repeated keys and
handle UTF-8, empty values and embedded nulls. Raw upload bytes use actual
JavaScriptCore ArrayBuffers with independent owned copies. File entries expose
the upload path without reading the file. Unresolved blobs and streams produce
an error object instead of partial body data. JSON-created own properties keep
prototype setters from intercepting body construction.

The current implementation observes requests. It rejects blocking, asynchronous
blocking and unsupported listener options rather than accepting interception
requests it cannot fulfill. [Mozilla's onBeforeRequest documentation](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/webRequest/onBeforeRequest)
describes the additional behavior expected of blocking listeners. Authentication
observations do not supply credentials or cancel challenges.

**125 native helper checks pass**, with normal exit, no captured crash and
unchanged sources, native configuration and frozen libraries. The harness uses
real ResourceLoadInfo, upload elements, API::Object, match-pattern/parser code,
localized formatting, WTF and JavaScriptCore. It substitutes InitializeWebKit2
with JSC/WTF initialization for this isolated test; full WebCore atom/JIT and
browser initialization are not exercised. It does not link WebCore or WebKit
libraries. The report records this adapter explicitly.

Evidence:

- `.vm/extension-web-request-tests.H25yztQl/result.json`: final helper checks.
- `.vm/extension-lifecycle-inputs.dD5Q1pGR/result.json`: helpers, namespace,
  generated bindings and generated process receiver compile with both extension
  features enabled. Its two earlier API compiles are superseded below.
- `.vm/extension-lifecycle-inputs.t7B7Z3hk/result.json`: final API/event adapters.
- `.vm/extension-web-request-validation.json`: nine final units and final source
  hashes matched to the helper and compile reports.
- `.vm/extension-web-request-extraction.json`: twelve upstream bodies retained.

Earlier probes remain recorded: `.DZ4C4oyL` failed a test compilation,
`.GvELOD0q` lacked match-pattern dependencies at link, `.dhZktee6` found two body
adapter compile errors, and `.OYH3d32H` had one failed assertion caused by a
non-ASCII C++ test literal. The final test checks the decoded Unicode code point.

Native UI/controller resource forwarding has since been ported; see the
[forwarding evidence](webextensions-native-web-request-forwarding.md). No event
listener, request IPC, extension, network request or browser was executed by
these checks. The feature-enabled engine still has not linked. Cocoa
compilation/runtime is also unverified after moving its bindings to common C++.

Compatibility work remains. The `incognito` filter is rejected because this IPC
path lacks request privacy metadata. Websocket filters are accepted but the
current ResourceLoadInfo type has no websocket event source. Multipart uploads
remain raw, and complete cookie/header parity is unfinished. The existing network
notification path omits upload bodies larger than 1 MiB. Status-line formatting
and known main-frame parents have since been corrected; see the
[metadata evidence and remaining limits](webextensions-native-web-request-metadata.md).
Full Chrome/Firefox/Safari compatibility remains unfinished.

Promoted patch: `c9587d431175525e047288be50e42c7b97fc365ad93831a1cf9e3c6a78073a71`.
Probe baseline: `e9e643ec3a02d6b3f73f386db7b4a5e05eda99c45832c506dc1a34ce6db43502`.
