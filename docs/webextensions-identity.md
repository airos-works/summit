# chrome.identity (#106)

Extensions with the `identity` permission can sign in to web services
("Sign in with …" in Stylus sync, Raindrop and others) with
`launchWebAuthFlow`, as in Chrome.

| Call | Answer |
| --- | --- |
| `getRedirectURL(path?)` | `https://<extension id>.chromiumapp.org/<path>`, Chrome's form |
| `launchWebAuthFlow({url, interactive})` | opens `url` in a pop-up window; when it navigates (by a link, a script or a server redirect) to an address on the extension's `chromiumapp.org` host, the navigation is stopped, the window closes and the call answers with that address |

Closing the window first rejects with "The user did not approve access.".
`interactive: false` rejects with "User interaction required.": Summit does
not load pages unseen, so even a flow that would need no sign-in is refused.
`getAuthToken` (Google account tokens) is not provided.

## How

`WebExtensionAPIIdentity` sends `IdentityLaunchWebAuthFlowHaiku(url,
interactive)`. `WebExtensionContext::identityLaunchWebAuthFlowHaiku` opens a
pop-up window with an `OPEN_WINDOW` browser command and remembers the page;
the Haiku navigation policy (`WebView.cpp`) asks
`WebExtensionController::interceptAuthFlowNavigationHaiku` about every
main-frame navigation to a `chromiumapp.org` host, and the tab's removal
(`fireTabsRemovedEventIfNeeded`) fails a flow still waiting.

## Fixture

`tests/fixtures/extensions/identity/`: its `AUTH_PAGE` is a page that sends
the browser to its `?redirect=` with `?code=42` (one line of script, for
example `location.href = new URLSearchParams(location.search).get('redirect')
+ '?code=42'`); the server-redirect step uses httpbin.org.

Verified on the X399 (24 Oct, id106 build): the script redirect answered
`…chromiumapp.org/cb?code=42`, the httpbin.org server redirect `?code=7`,
`interactive: false` and a window closed by the extension were refused, and
the sign-in windows closed.
