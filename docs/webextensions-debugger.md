# chrome.debugger, and what Claude in Chrome needed

Claude in Chrome (`fcoeoabgfenejglbffodgkkbkcdhcgfn`, 1.0.109) installed from
the Chrome Web Store did nothing in Summit: its toolbar button opened nothing.
Its service worker (an ES module) failed while loading, so it never registered
`action.onClicked`:

    Unhandled Promise Rejection: TypeError: undefined is not an object (near '...var Am=class e{static insta...')

A class in `ShortcutExecutionService-*.js` has
`static SESSION_GROUP_COLORS=[chrome.tabGroups.Color.BLUE, ...]`, evaluated
when the module loads, and Summit had no `chrome.tabGroups.Color`. Behind that
came the rest of what the extension uses and Summit lacked:

| API | Used for | Now |
| --- | --- | --- |
| `tabGroups.Color` | its tab group's colour, at module load | enum object |
| `declarativeNetRequest.RuleActionType`, `HeaderOperation`, `ResourceType` (and `RequestMethod`, `DomainType`, `UnsupportedRegexReason`) | header rules for its API and the side panel's WebSocket | enum objects |
| `runtime.getContexts` | finding its side panel (toggle, Ctrl+E) | implemented |
| `debugger` (permission and namespace) | every browser tool: screenshots, clicks, typing, JavaScript, console and network reading | implemented (below) |
| `dom.openOrClosedShadowRoot` | reading pages with closed shadow roots | implemented for content scripts |
| `chrome` on claude.ai | claude.ai hands the extension its sign-in with `chrome.runtime.sendMessage(id, {type: "oauth_redirect"})` | `chrome` = `browser` on pages an extension lists in `externally_connectable` |
| `identity.launchWebAuthFlow` ending at `chrome-extension://ID/...` | its silent re-sign-in redirects to `chrome-extension://ID/oauth_callback.html` | accepted as Chrome accepts it, beside `https://ID.chromiumapp.org/` |
| `favicon` permission (`/_favicon/?pageUrl=`) | icons in its tab list | the icon the browser shows (FAVICON browser command) |
| `Page.handleJavaScriptDialog` | answering alert/confirm/prompt/beforeunload while it drives a page | implemented (below) |

Its tool list (`navigate`, `computer` with click/type/key/scroll/screenshot,
`read_page`, `find`, `form_input`, `javascript_tool`, `read_console_messages`,
`read_network_requests`, `get_page_text`, tab and tab-group tools) needs nothing
else from the browser.

## chrome.debugger

`chrome.debugger` speaks the Chrome DevTools Protocol (CDP) to a tab. WebKit's
Web Inspector protocol is close to CDP in its Page, Runtime and Network
domains and has no Input domain. A session
(`UIProcess/Extensions/haiku/WebExtensionContextAPIDebuggerHaiku.cpp`) is a Web
Inspector frontend connected to the tab's page, as Summit's developer tools and
MCP server are; it translates the extension's commands, carries out the ones
WebKit has no equivalent for natively, and sends the page's events back as
`debugger.onEvent`.

- `attach({tabId} | {targetId: "tab-N"}, "1.x")`: pages the extension's host
  permissions cover, `about:blank` and its own pages. One session a tab, as in
  Chrome ("Another debugger is already attached to the tab with id: N.").
- `detach`; a closed tab detaches with `onDetach(source, "target_closed")`.
- `getTargets()`: the tabs, as `{id: "tab-N", type: "page", tabId, url, title, attached}`.
- `sendCommand`:
  - `Page.enable`, `Runtime.enable` (also `Console.enable`), `Network.enable`:
    turned on in the page; `*.disable` only stops this session's events,
    because other frontends of the page share its agents.
  - `Runtime.evaluate`: WebKit's, with `awaitPromise` through
    `Runtime.callFunctionOn(awaitPromise)` and `wasThrown` as
    `exceptionDetails`. `replMode` (Claude's `javascript_tool`): code with
    top-level `await` or `return` is a syntax error in a script, so it runs
    again as the body of an async function in which every expression
    statement (in blocks, labels, if/else, loops and try, not in functions)
    assigns to a variable that the body returns: the value of the last one
    run, as in a REPL (a small tokenizer finds the statements). Code the
    rewrite breaks runs once more as written.
  - `Page.captureScreenshot` (`png`/`jpeg`, `quality`, `clip`): the frame the
    tab's view shows, in device pixels, as `tabs.captureVisibleTab` takes it;
    a hidden tab is painted by the page (`Page.snapshotRect`).
  - `Input.dispatchMouseEvent` (pressed/released/moved/wheel, buttons,
    clickCount, modifiers), `Input.dispatchKeyEvent` (keyDown/rawKeyDown/keyUp
    built from key, code, text and Windows key code; `char` and `commands` as
    editing commands), `Input.insertText` (the InsertText editing command):
    native events into the page, answered once the page has handled them.
    They reach background tabs too; CDP coordinates are CSS pixels of the
    viewport (times the page zoom).
  - `Page.getFrameTree` (from `Page.getResourceTree`), `Page.navigate`
    (a load of the address), `Page.bringToFront` (activates the tab),
    `Browser.getVersion`.
  - Accepted as no-ops: `Emulation.setFocusEmulationEnabled`,
    `Network.setBypassServiceWorker`, `Page.startScreencast`/`stopScreencast`,
    `Input.cancelDragging`, `Log.enable`, `Page.setLifecycleEventsEnabled`, ...
  - Anything else goes to the page unchanged, which works for the many
    commands both protocols share (`Page.reload`, `DOM.*`,
    `Runtime.getProperties`...).
  - `Page.handleJavaScriptDialog {accept, promptText}`: answers the oldest
    dialog the page shows; its window closes (the view's dialog host,
    `javaScriptDialogAnswerHaiku`) and the next queued one shows. Every
    dialog (the person's too) is reported as `Page.javascriptDialogOpening`
    `{url, message, type, defaultPrompt, hasBrowserHandler}` and
    `Page.javascriptDialogClosed {result, userInput}` while `Page` is
    enabled; without one showing the answer is "No dialog is showing".
    `WebView`'s UI client calls `debuggerJavaScriptDialogOpeningHaiku` and
    `...ClosedHaiku` (`UIProcess/haiku/WebExtensionDebuggerHaiku.h`).
- Events forwarded: `Page.frameNavigated` (with `type`), `loadEventFired`,
  `domContentEventFired`, `frameStartedLoading`, `frameStoppedLoading`;
  `Network.requestWillBeSent` (`walltime` as `wallTime`), `responseReceived`,
  `dataReceived`, `loadingFinished`, `loadingFailed`; console messages as
  `Runtime.consoleAPICalled` and JavaScript errors as
  `Runtime.exceptionThrown`; `Runtime.executionContextCreated`. A navigation
  into another web process keeps the session: the new (provisional) target
  gets the enabled domains before it commits, and commands still waiting on
  the old one fail with "Inspected target navigated or closed", as in Chrome.
- The frontend router broadcasts answers to every frontend of a page, so a
  session's identifiers start at 2,000,000,000 (above the developer tools'
  and the MCP server's; the inspector reads them as 32-bit integers).
  Console messages need the page's developer extras: sessions turn them on and
  the last one to end turns them off again (shared preferences, counted).

The calls travel as `debugger.METHOD` (and `runtime.getContexts`) in the
`SidebarActionHaiku` message and the events as `DispatchHistoryEventHaiku`,
as `tabGroups` does, so no IPC message was added. Extensions installed before
this get the `debugger` permission at their next start (the browser passes
every manifest permission the engine supports); the install prompt describes
it as "Control pages in tabs: read and change them, click and type in them".

## favicon

`chrome-extension://ID/_favicon/?pageUrl=URL&size=N`, for an extension with
the `favicon` permission (it is not a web-accessible resource, so only the
extension's own pages load it): the scheme handler asks the browser with a
FAVICON browser command (`page_url`, `size`), which answers with the PNG its
tabs show (`FaviconCache::DataURL`); a page without one fails to load, which
extensions answer with their own icon (Claude shows a globe).

## runtime.getContexts

From the UI process's pages of the extension: `BACKGROUND`,
`OFFSCREEN_DOCUMENT`, `POPUP`, `TAB` (with `tabId`/`windowId`), and other
extension views as `SIDE_PANEL` (the sidebar) or `DEVELOPER_TOOLS`; the
filter's lists (`contextTypes`, `tabIds`, `documentUrls`...) and `incognito`
apply. Claude's side panel shows as `sidepanel.html?tabId=N`.

## Checked (27 Nov 2026, Framework laptop, test copy of 1126 + this engine)

`tests/fixtures/extensions/debugger` (unpacked, `SUMMIT_EXTENSION_DEVELOPER_MODE=1`)
and its `page.html` served over HTTP, driven through the MCP `evaluate` tool
(`extension: "debugger@summit.test"`, `checks.*`): attach/second attach
refused/detach; enables; `Runtime.evaluate` by value, awaited, thrown;
REPL mode (`await slow(5)`, multi-line with declarations, `return 7`,
`await fetch(...).then(...)`, a loop with `await`, object literals); clicks
arrive trusted at the coordinates given; `insertText`, Backspace, Enter and
letters reach the field; PNG, JPEG and clipped screenshots of the shown tab and
of a hidden one; Network/Page/console events across a reload; a navigation to
another site and back with the session kept (events and input still work);
`onDetach` `target_closed` when the tab closed; `getTargets`; the content
script read the page's closed shadow root; `checks.signIn(auth.html)` ended at
`oauth_callback.html?code=42` of the fixture and closed its window;
`checks.dialog()` for confirm (accepted and dismissed), prompt (with text) and
alert gave the opening and closed events, the page got the answer and no dialog
window stayed; REPL values from if/else, try, loops and labelled loops;
`checks.favicon()` gave claude.ai's icon (32x32 PNG) and failed for a site
without one. Claude in Chrome: its service worker
starts (no TypeError, offscreen document created), the toolbar button opens its
side panel ("Log in") and puts the tab in an orange "Claude" group, Log in
opens claude.ai's sign-in, claude.ai sees `chrome.runtime.sendMessage` and the
extension answers its `oauth_redirect` message, and `getContexts` lists the side
panel. Not checked: signing in and running the agent (needs the owner's
claude.ai account).
