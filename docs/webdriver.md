# WebDriver (summitdriver)

Summit can be driven by WebDriver clients (Selenium, WebdriverIO, plain HTTP)
through WebKit's own WebDriver service, as Safari is through `safaridriver`
(issue #159). The service is the engine's `WebKitWebDriver` executable; Summit
ships it as `summitdriver`.

## Running it

```sh
summitdriver --port=4444            # or WebKitWebDriver -p 4444
```

`--host=ADDRESS` limits where it listens (default: every interface).
A new session starts Summit itself: the installed `/boot/system/apps/Summit`,
or the binary named by the session's capabilities or by
`SUMMIT_WEBDRIVER_BROWSER`, with `--automation 127.0.0.1:PORT` (a free port).
Deleting the session closes its windows and asks that Summit to quit.

```json
{"capabilities": {"alwaysMatch": {
    "summit:browserOptions": {
        "binary": "/boot/home/Summit/Summit",
        "args": ["--profile", "/boot/home/webdriver-profile"]
    }
}}}
```

`socket:targetAddr` / `socket:targetPort` (or the driver's `--target=ADDRESS:PORT`)
connect to a Summit that is already running with `--automation ADDRESS:PORT`
instead of starting one.

A start of the same Summit binary while it already runs is handed to the
running copy (Summit is single-launch, and a profile has one owner): that copy
then opens the automation socket, its session's windows open beside the user's,
and the driver does not quit it at the end of the session. Use
`--profile` with another folder and another copy of Summit to keep test runs
apart from your own browsing.

## What a session looks like

Each browsing context is a window (or a tab of the session's window when the
client asks for a tab) that Summit opens for the session. Its address field is
orange, as in Safari, and its title ends in "(Automated)".

Plain HTTP works without Selenium:

```sh
S=$(curl -s -XPOST localhost:4444/session -d '{"capabilities":{}}' | jq -r .value.sessionId)
curl -s -XPOST localhost:4444/session/$S/url -d '{"url":"https://example.com"}'
E=$(curl -s -XPOST localhost:4444/session/$S/element -d '{"using":"css selector","value":"a"}' \
    | jq -r '.value["element-6066-11e4-a52e-4f735466cecf"]')
curl -s localhost:4444/session/$S/element/$E/text
curl -s -XPOST localhost:4444/session/$S/element/$E/click -d '{}'
curl -s localhost:4444/session/$S/screenshot | jq -r .value | base64 -d > shot.png
curl -s -XDELETE localhost:4444/session/$S
```

## How it works

- `WebKitWebDriver` (Source/WebDriver, `PlatformHaiku.cmake`) is WebKit's
  service with the socket session host of the Windows and PlayStation ports
  (`USE_INSPECTOR_SOCKET_SERVER`); `haiku/WebDriverServiceHaiku.cpp` has the
  capabilities, `socket/SessionHostSocket.cpp` starts and quits the browser.
- `Summit --automation ADDRESS:PORT` calls
  `BWebKitContext::EnableRemoteAutomation`, which opens the remote inspector
  socket for the profile's context and installs `AutomationClientHaiku`
  (UIProcess/haiku). A driver connected there starts a `WebAutomationSession`.
- The session asks for browsing contexts with the browser commands extensions
  use (`B_WEBKIT_BROWSER_COMMAND` OPEN_WINDOW / OPEN_TAB, plus ACTIVATE_TAB and
  SET_WINDOW_STATE), with `automation` true; Summit opens them like
  `windows.create()` and marks the window (`BrowserWindow::MarkAutomated`).
- Clicks and keys are the BMessages a `BWebKitView` would pass on, turned into
  `NativeWebMouseEvent` / `NativeWebKeyboardEvent` as real input is
  (UIProcess/Automation/haiku). Screenshots are taken in the web process and
  encoded with Skia.
- The engine needs `ENABLE_WEBDRIVER=ON`, which also turns on WebDriver's
  mouse and keyboard interactions (Source/cmake/OptionsHaiku.cmake).

## Limits

- No WebDriver BiDi, no touch or wheel actions, no proxy capabilities.
- JavaScript dialogs (alert, confirm, prompt) are Summit's own windows: the
  driver cannot read, accept or dismiss them yet.
- One automation socket per Summit instance, one session at a time.
