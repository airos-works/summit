# Messages between extensions (#106)

`runtime.sendMessage(extensionId, message)` to another installed extension
reaches that extension's `runtime.onMessageExternal`, as in Chrome:

- The receiver must be loaded. It admits the sender if it has no
  `externally_connectable` key, or if the sender's ID (or `"*"`) is in
  `externally_connectable.ids`. Otherwise, and for an ID that is not
  installed, the call rejects with "Could not establish connection. Receiving
  end does not exist."
- `sender.id` is the sending extension's ID; `sender.url` its page; `sender.tab`
  is described with the receiver's own tab identifiers, and is left out when
  the receiver cannot see that tab (a private window it is not allowed in).
- The receiver's reply (`sendResponse`, or a returned promise) resolves the
  sender's call. Gestures do not carry over.

`WebExtension` now keeps `externally_connectable.ids`
(`acceptsExternalMessagesFrom`); `WebExtensionContext::runtimeSendMessage`
hands the message to the receiver's
`receiveExternalRuntimeMessageHaiku`, which wakes its background and
dispatches `RuntimeOnMessageExternal`.

## Ports between extensions

`runtime.connect(extensionId)` to another extension opens a port to its
`runtime.onConnectExternal`, under the same `externally_connectable` rules (a
refused connection disconnects with "Receiving end does not exist").

A web page's `connect` to an extension runs entirely in the receiving
extension's context. Between two extensions each end's web process posts to
its own extension's context, so the UI process bridges the two through their
Native worlds (`connectToExtensionHaiku`, `acceptBridgedConnectHaiku`):

- The sender's port aims at the Native world. Its context counts the bridge
  as the open far end, and hands each message to the other context, which
  delivers it with `portPostMessage` (queued there until the receiving port
  exists).
- The receiver's ports are created with the Native world as their partner and
  post to the bridge the same way.
- When one side's ports are all gone, the bridge is taken down and the other
  side's ports get `onDisconnect`.

Verified on the X399 (24 Oct, xconn106 build): `onConnectExternal` got the
port with the sender's ID; "ping 1" and "ping 2", posted right after
`connect`, were answered; after "bye" the receiver disconnected and the
sender's `onDisconnect` fired. A port to the closed receiver disconnected at
once, but without setting `runtime.lastError` in `onDisconnect` (Chrome sets
"Could not establish connection. Receiving end does not exist."; the error is
reported to the console instead).

## Fixture

`tests/fixtures/extensions/external-messaging/`: `receiver` (key fixes its ID,
`lbemamlibomhdpkllpeodnmpommkhbdl`), `closed` (admits one other ID; key fixes
`dbjglklidijppgkmdpjggbmfnbmnghnb`) and `sender`, which three seconds after it
starts messages both and a missing ID, then opens a port to the receiver (two
pings, then "bye", after which the receiver disconnects) and one to the closed
receiver. Install the receivers first.

Verified on the X399 (24 Oct, xmsg106 build): the receiver's
`onMessageExternal` got `{"hello":"receiver"}` from the sender's ID and its
reply reached the sender; the closed receiver and the missing ID rejected with
"Receiving end does not exist". The first build delivered the message to
`onMessage`, because the web process treated only web pages as external
senders; a sender with another extension ID now counts too. The userScripts
fixture's messages to its own extension still reach `onMessage`.

## chrome on externally connectable pages (27 Nov 2026)

Web pages had Safari's `browser.runtime` only, and sites written for Chrome
call `chrome.runtime.sendMessage(id, ...)`: claude.ai gives Claude in Chrome
its sign-in that way. A page that a loaded extension lists in
`externally_connectable.matches` now also gets `chrome`, the same object
(`WebExtensionControllerProxy::addBindingsToWebPageFrameIfNecessary`). Other
pages still have no `window.chrome`, which sites take for Chrome itself.
Checked on the laptop: claude.ai's page messaged Claude in Chrome and got its
answer; example.com has no `chrome`.

## A page's message nobody takes (28 Nov 2026)

WebKit answered `runtime.sendMessage(id, ...)` from a web page with nothing
(after a random delay) when no extension with that ID was loaded, so a page
could not tell installed extensions from missing ones. Chrome reports
"Could not establish connection. Receiving end does not exist.", and sites
rely on it: claude.ai hands Claude in Chrome its sign-in by trying each of the
extension's IDs (development builds first) until one accepts, and took the
empty answer from the first missing one as success, so the sign-in page
spun forever. Summit now answers as Chrome does, still after the random
delay, also when the browser could not deliver the message.
