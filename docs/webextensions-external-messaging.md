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

`runtime.connect(extensionId)` to another extension is not implemented yet.
A web page's `connect` to an extension runs entirely in the receiving
extension's context (both port ends are counted there, and the page's web
process posts its port messages to that context). Between two extensions each
end's web process posts to its own extension's context, so the controller
would have to relay port messages, disconnects and port counts between the
two contexts by channel identifier.

## Fixture

`tests/fixtures/extensions/external-messaging/`: `receiver` (key fixes its ID,
`lbemamlibomhdpkllpeodnmpommkhbdl`), `closed` (admits one other ID; key fixes
`dbjglklidijppgkmdpjggbmfnbmnghnb`) and `sender`, which three seconds after it
starts messages both and a missing ID. Install the receivers first.

Verified on the X399 (24 Oct, xmsg106 build): the receiver's
`onMessageExternal` got `{"hello":"receiver"}` from the sender's ID and its
reply reached the sender; the closed receiver and the missing ID rejected with
"Receiving end does not exist". The first build delivered the message to
`onMessage`, because the web process treated only web pages as external
senders; a sender with another extension ID now counts too. The userScripts
fixture's messages to its own extension still reach `onMessage`.
