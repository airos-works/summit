# Native extension action popup

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The native WebKit action popup now runs real extension HTML and JavaScript
through Summit's toolbar and public SDK. The complete engine builds and the
browser tests cover actual popup input, storage, tab changes, unload and
shutdown. See [current implementation and native evidence](modern-extension-actions.md).
The earlier host-only stage used engine patch
`4bd2ca81e2ab0c0e2d5a10f9e776e0bf41cd04e392427c57961edf6c6775ba4b`;
its narrower evidence is retained below.

The host creates a floating subset window for its browser owner, keeps it hidden
until requested, clamps its size and position to the screen, and dismisses on
Escape, focus loss or owner closure. Native workers acquire the owner with a
bounded wait without blocking the WebKit main loop. Window completion follows
child-view destruction, and application drain accounting includes both workers
and windows. A callback allows an active JavaScript dialog to postpone dismissal.

The bridge creates a page with `webViewConfiguration(Popup)`, preserving the
extension's required origin, CSP, process and storage configuration. An internal
`BWebKitView` constructor adopts that page's shared native state. Page references,
action flags and callbacks remain on the WebKit main loop; window workers only
handle native state. Page content-size notifications drive native resizing.
The current anchor is the top-right corner of the browser content below its
toolbar. A toolbar-button-specific anchor still needs the native SDK/UI route.

A popup belongs to its extension load, action, tab and browser window. The
existing popup-page map supplies its originating tab to `tabs.getCurrent` and
extension-view lookup. A new owner navigation, tab selection/move, private-access
revocation, action disable/path change, unload, page failure or window dismissal
closes the page and clears its association. A later action invocation also
supersedes pending popups from other extensions in the same browser profile.
Initial document loading has a
30-second deadline. Top-level navigation must remain in the extension's origin;
child-frame loads retain the normal extension CSP checks.

Trusted `performAction` validates the current action/tab before granting the
existing user gesture. An action without a popup uses the action-click event
producer. An action with a popup creates the page and window; its presented flag
is set only after native presentation. Programmatic invocation does not grant
`activeTab` access.

## Earlier host-only evidence

`python3 tools/test-native-extension-popup.py --overlay .vm/extension-action-popup-candidate`
passed all 88 checks in `.vm/native-extension-popup.GpRFd4dG/result.json`.
Compilation emitted no warnings. The runtime returned zero, input hashes stayed
unchanged, the native crash-log interval contained no events, and all windows
and workers drained. This executable links libbe, not WebKit or WebCore.

The checks exercise hidden creation, delayed show, resizing, Escape, real native
focus transfer to the owner, owner destruction, the modal-child callback gate,
cancellation before start, a failing content factory and an invalid owner.
The modal-child gate uses a callback fixture; it does not show a JavaScript alert.
Earlier failed runs are preserved: the Escape fixture initially sent an ordinary
window message instead of preferred-target keyboard input, and the focus fixture
used `Activate(false)` (which asks Haiku to reorder a floating window) instead of
selecting another window. Both corrected fixtures pass without changing the
native host implementation.

Ten affected engine units compile with Extensions and Content Extensions enabled
in isolated configuration headers. The native source/configuration inputs stayed
unchanged. Successful units are recorded separately from failed units in earlier
stages; the full engine build remains the test of their combined integration.

- `.vm/extension-lifecycle-inputs.RBwdMTqB/result.json`: `WebView.cpp` and `WebKitView.cpp`.
- `.vm/extension-lifecycle-inputs.2flINGev/result.json`: context, native context and action-property observer.
- `.vm/extension-lifecycle-inputs.f7CGbu4O/result.json`: page proxy, window registry observer and navigation-event receiver.
- `.vm/extension-lifecycle-inputs.VaVbCdcS/result.json`: popup controller and native `performAction`.
- `.vm/extension-lifecycle-inputs.fLiNB1mT/result.json`: popup-to-tab association.
- `.vm/extension-action-popup-current-validation.json`: hashes covering all 19 promoted source files, the selected successful units and the native host test.
- `.vm/modern-extensions-action-popup-build-result.json`: completed full build,
  one undefined function (`loadDeclarativeNetRequestRules`) with five references.
  Its log is `.vm/modern-extensions-action-popup-build.log`.

These checks do not execute a WebKit page, extension context, privileged IPC,
JavaScript dialog, action input or permission lifecycle. Failed compile attempts
and native test attempts remain preserved in `.vm`.

## Remaining work

Extend integrated coverage to navigation/CSP rejection, permission revocation,
process failure and modal dialogs. Individual-button anchoring, overflow and
context-menu input and remaining programmatic action APIs also need work.
The current end-to-end tests and their limits are recorded in
[native toolbar actions](modern-extension-actions.md). This work does not
establish complete Chrome, Firefox or Safari extension compatibility.
