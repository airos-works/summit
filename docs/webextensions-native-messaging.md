# Native messaging (#106)

Extensions with the `nativeMessaging` permission talk to programs on the
computer with `runtime.sendNativeMessage` and `runtime.connectNative`, as
in Chrome and Firefox (KeePassXC-Browser and other desktop-app companions).

## Hosts

A host is a JSON manifest named `<name>.json` in `Summit/NativeMessagingHosts`
under one of (the first found wins):

- `~/config/settings` (`B_USER_SETTINGS_DIRECTORY`)
- `~/config/non-packaged/data`, `~/config/data`
- `/boot/system/non-packaged/data`, `/boot/system/data` (for packages)

```json
{
  "name": "com.example.host",
  "description": "…",
  "path": "/boot/home/config/non-packaged/bin/example-host",
  "type": "stdio",
  "allowed_origins": ["chrome-extension://<extension id>/"],
  "allowed_extensions": ["addon@example.com"]
}
```

Names are letters, digits, `_` and dots. `path` may be relative to the
manifest. Chrome extensions are matched by `allowed_origins` (their
`chrome-extension://` origin, or the `webkit-extension://` origin of
extensions installed earlier) and the host gets the origin as its argument;
Firefox add-ons by `allowed_extensions` (their `gecko` ID, or Summit's
identifier) and the host gets the manifest's path and the ID, as Firefox
passes them.

Messages are a 32-bit length in native byte order and that much UTF-8 JSON
on the host's standard input and output; its standard error goes to
Summit's. A message from the host may be at most 1 MB (Chrome's limit).

- `sendNativeMessage`: starts the host, sends the message, answers with its
  first reply and closes the host's input (ending it after 2 seconds if it
  is still running).
- `connectNative`: a port to a host that runs until either side
  disconnects; the host exiting disconnects the port with "Native host has
  exited.".

## How

`UIProcess/Extensions/haiku/WebExtensionContextAPINativeMessagingHaiku.cpp`:
`findNativeHostHaiku` reads the manifests, `NativeMessagingHostHaiku`
spawns the host (`posix_spawn` with two pipes), reads its messages on a
thread and writes on a work queue with `SIGPIPE` blocked. Ports are
`WebExtensionMessagePort`s in the Native world (`setMessageHandler`,
`setDisconnectHandler`, `sendMessage`).

## Fixture

`tests/fixtures/extensions/native-messaging/`: copy `host/summit.echo.json`
and `host/summit_echo.py` to `~/config/settings/Summit/NativeMessagingHosts/`,
install the extension and read the `[native]` lines.

Verified on the X399 (24 Oct, nm106 build): `sendNativeMessage` answered
with the host's reply (arguments: the manifest path and the add-on ID), a
`connectNative` port got two replies and disconnected with "Native host has
exited." when the host quit, a reply over 1 MB, a missing host and a name
with `..` were refused, and no host was left running. A Summit started
without `PATH` cannot run `#!/bin/env python3` hosts (the host inherits
Summit's environment).
