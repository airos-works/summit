# Native action API state and dispatch

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The Haiku port adds `getTitle`, `setTitle`, `getBadgeText`, `setBadgeText`,
`getPopup`, `setPopup`, `enable`, `disable`, `isEnabled`, and `onClicked` to
the native action bindings. The namespace retains the pinned manifest checks
for `action`, `browserAction`, and `pageAction`. Real extensions now execute
the action state and click/popup paths through Summit's native toolbar; see
[current integrated evidence](modern-extension-actions.md). The earlier
compile-only stage below used patch
`71a2c16ef1345fb2225b97bd8017adc654ab4aa923ae759785d67eb8f074dad5`
and reached linking with three missing symbols: the DNR loader, menu-click
dispatcher and cookie-change dispatcher. Those historical link failures no
longer describe the current complete engine build.

The preceding Action build exposed a missing inline definition. The follow-up
replaces `LocalFrame.h` with `LocalFrameInlines.h`, which contains
the definition used by click-event dispatch. The isolated native source compiles
with regenerated IPC and no warnings, and its undefined-symbol list no longer
contains `LocalFrame::document()`. The full build confirms that fix.

Reads and writes cross the actual context IPC boundary. Receivers require a
loaded privileged context whose manifest declares an action. They independently
validate target identifiers and check that the requested tab/window is live and
accessible. Missing or inaccessible targets return an error. Getters use the
existing inherited action without creating an override; setters use the native
action model and its coalesced property-change observer.

Details accept either a tab or window scope, never both. IDs must be safe
positive integers, with the current-window sentinel accepted for window scope.
Setters require the corresponding `title`, `text`, or `popup` field. Null
resets inheritance and an empty string is an explicit override. Unknown fields,
invalid value types and embedded null characters are rejected. This parser is
stricter than the pinned Cocoa implementation for unknown fields. The title
getter preserves explicit empty labels. Popup state stores the pinned WebKit
path value; it does not normalize it into a qualified URL or display a popup.

`enable` and `disable` use raw JavaScript values because the binding generator's
JSON conversion accepts dictionaries, not scalar numbers. Their converter
accepts omitted/undefined values or valid numeric tab IDs without coercing
strings and booleans. The inherited optional-callback overload treats a single
null argument as an omitted callback/default target; an explicit null tab
argument supplied before a callback reaches the converter and is rejected.
The helper tests the converter directly, not that overload dispatcher.

The native click-event producer checks the current load, tab access and enabled
state, and suppresses events when a popup is configured. It wakes background
content and checks those conditions again before sending permission-filtered
tab parameters. The web-process dispatcher creates fresh protected values for
each listener within a WebCore user-gesture scope. The public SDK and Summit
toolbar now call this path, with real native action/popup integration tests.
The [programmatic popup suite](webextensions-native-open-popup.md) now verifies
that programmatic calls preserve redaction, real toolbar clicks grant access,
same-origin navigation retains it, and a different origin revokes it. See also
[current SDK and toolbar evidence](modern-extension-actions.md).

Dynamic `setIcon` now has a [native implementation](webextensions-native-action-icons.md)
with a passing 49-case browser suite and 55 SDK/desktop pixel observations,
including SVG and ImageData transparency. The same bundle passes 238 existing
action/popup checks and 169 overflow checks. That verified distribution does not
expose `openPopup`; its [new native implementation](webextensions-native-open-popup.md)
passes 342 browser checks on three fresh profiles.
Badge background and text colors now have a
[verified native implementation](webextensions-native-badge-colors.md).
The SDK does display decoded manifest action icons.
The Cocoa bindings retain those methods and adapt ordinary details to their
existing Objective-C implementations. Icon details reach Cocoa as raw
JavaScript values so its existing `ImageData` conversion is preserved. The
Xcode project registers the new argument helper in its bindings group and
header build phase. Cocoa compilation and runtime behavior have not been tested.

## Earlier isolated verification

The isolated native helper passes **125 checks**. It runs the actual structural
parser, the raw JavaScript tab-ID converter and the existing WebKit dictionary
converter against frozen JavaScriptCore/WTF and ICU. Coverage includes malformed
scopes, required fields, safe integer limits, non-finite numbers, null versus
empty values, Unicode and garbage collection. All four source units compile,
the executable runs from `BApplication::ReadyToRun`, source/library hashes stay
unchanged, and the native crash log records no events. WebCore and WebKit are
not linked into this helper.

**Seven native integration units compile**, including the API, privileged
receiver, generated bindings and regenerated IPC. **393 binding generation and
preprocessing checks pass** across the Cocoa/Haiku surfaces, including raw
numeric argument handling and the Cocoa icon call signature. Twenty of the
37 interfaces generate C++ bindings. An audit matches the current candidate
sources, headers and generated outputs to the tested inputs.

The first helper attempt compiled but failed to link because Haiku retained
unrelated exported WebCore-dependent functions. The final isolated helper uses
hidden symbol visibility and section collection, as existing helpers do; it
does not stub those functions or link a mismatched engine library.

These earlier isolated results do not execute action API IPC, model inheritance, observer
delivery, permission decisions, real click events, popup presentation or an
extension in the browser. Current integrated results are linked above.

Evidence:

- `.vm/extension-action-api-validation.json`
- `.vm/extension-action-details-tests.amSqInFE/result.json`
- `.vm/extension-lifecycle-inputs.yawnB3OU/result.json`
- `.vm/extension-bindings-generated-37zlp8gp/binding-generation.json`
- `.vm/extension-binding-platforms-gtlumi_b/result.json`
- `.vm/modern-extensions-action-api-build-result.json`
- `.vm/extension-action-inline-validation.json`
- `.vm/modern-extensions-action-inline-build-result.json`
