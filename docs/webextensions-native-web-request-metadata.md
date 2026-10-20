# Request status and frame metadata

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

Request events now serialize the HTTP protocol, status code and server reason
text together, for example `HTTP/1.1 404 Not Found`. A response without reason
text retains its protocol and code without a fabricated phrase. Missing protocol
or status metadata produces an empty status line. The Curl backend supplies
HTTP/1.0, HTTP/1.1, HTTP/2 and HTTP/3 versions. Its unspecified version still
cannot identify an HTTP/0.9 response reliably. The intended API shape is described
in [Mozilla's response-event reference](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/webRequest/onHeadersReceived).

The UI process captures the actual main-frame identifier before queuing a
background wake. It prefers a live parent known to be a main frame belonging to
the same page, which can retain the correct identity during provisional frame
changes. Otherwise it uses the page's current main frame. Six resource IPC
messages carry that snapshot. The process maps a known main-frame parent to
`0`, retains nested parent identifiers and represents absent identifiers as `-1`.
ResourceLoadInfo keeps its actual Core identifiers throughout. These sentinel
values follow [Mozilla's request-event reference](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/webRequest/onBeforeRequest).

If the parent has already disappeared and the current root has a different
identifier, the old numeric parent remains unnormalized; this path has no
historical frame tree. Permission and live-tab checks still run before and after
each background wake.

**147 native helper checks pass**, including immediate and nested frames,
document and subresource mapping, absent identities, unchanged Core metadata,
HTTP versions and incomplete responses. The harness retains the explicitly
isolated JSC/WTF initialization adapter documented in the
[request-event evidence](webextensions-native-web-request.md); it does not
initialize or link a full WebCore/WebKit runtime.

**Five native integration units compile** with both extension features enabled:
the helper, process API, UI forwarding, generated process receiver and context
proxy. IPC was regenerated from the candidate messages. Final source and header
hashes match the reports:

- `.vm/extension-web-request-tests.9LfooUff/result.json`
- `.vm/extension-lifecycle-inputs.6FDijonq/result.json`
- `.vm/extension-web-request-metadata-validation.json`

No live frame lookup, navigation race, network response, extension event, IPC or
browser runtime was executed. Cocoa compilation was not tested. The complete
feature-enabled engine still has not linked or run an extension.

Promoted patch: `d577d67362ab609beaa6d87d67938b3f65eb3476190953d9c2af0313aff0882d`.
Probe baseline: `e9fa1eff70595ba5e03552f0d8fd811084b56861e630853866ccce3e2361f756`.

The full native build of the promoted patch regenerated resource IPC and
completed compilation. Its WebKit shared-library link failed with the same 23
missing symbols as the preceding build; none were added by this change.
Evidence: `.vm/modern-extensions-web-request-metadata-build-result.json`.
