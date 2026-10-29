# content-helpers

Firefox content-script helpers (`cloneInto`, `exportFunction`,
`createObjectIn`) and `declarativeNetRequest.onRuleMatchedDebug` (#106,
`docs/webextensions-content-script-helpers.md`).

- `ext/`: a Manifest V2 extension with DNR rules, a background script that
  records `onRuleMatchedDebug` events and a content script that exercises the
  helpers.
- `page.html` and the `ext106-*` files: the test page (frames, a blocked
  picture, an allowed fetch). Serve this directory as `/ext106/` from a web
  server; the content script's match pattern and `setup-profile.sh` name the
  lab's build host (`192.168.1.200:8851`), so edit them for another server.
  Match patterns must not contain a port.
- `setup-profile.sh PROFILE`: makes a profile with the extension installed
  (start Summit with `SUMMIT_EXTENSION_DEVELOPER_MODE=1`); its catalog entry
  lists `"origins": ["<all_urls>"]`, without which content scripts don't run.
