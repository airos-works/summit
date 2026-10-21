# User-agent compatibility settings (#123)

Preferences › Compatibility › User agent chooses how Summit identifies itself
to websites. Chrome, Firefox and Edge have Windows and macOS presets; Safari
has macOS and a clearly labelled legacy Windows preset. Summit (default)
restores WebKit's usual site-aware identity. Reload an open page after changing
the choice. The choice applies to all windows, private windows, new tabs and
restored sessions, and survives a restart.

The presets change the HTTP User-Agent header and `navigator.userAgent` through
`BWebKitView::SetCustomUserAgent`. They do not replace the WebKit engine or add
another browser's APIs. A site can still detect other browser features. Returning
to Summit (default) is useful when a preset makes a site work less well.

`profile.json` stores a preset identifier in `userAgent`, never a raw header.
Unknown identifiers and older profiles use the default. Fixed compatibility
versions are kept in `src/core/UserAgent.cpp`; they are not advertised as the
latest versions. Formats follow [Chromium's reduced desktop identity](https://www.chromium.org/updates/ua-reduction/),
[Firefox's desktop format](https://developer.mozilla.org/en-US/docs/Web/HTTP/Reference/Headers/User-Agent/Firefox),
and [Edge's Edg token](https://learn.microsoft.com/en-us/microsoft-edge/web-platform/user-agent-guidance).
The Windows Safari preset is 5.1.7, [Apple's last Windows release](https://support.apple.com/en-us/102665).

Host tests cover profile round trips for every preset, old profiles and invalid
identifiers. `tools/test-user-agent.py` runs against a native browser: it compares
the document and fetch request headers with `navigator.userAgent` for every
preset, then checks new tabs, regular and private windows, a restart, invalid
input and restoring the default. Compile `tools/bench/summitctl.cpp` with
`g++ -std=c++17 -Isrc tools/bench/summitctl.cpp -lbe -o summitctl`, then run:

```sh
python3 tools/test-user-agent.py --browser /path/to/Summit --control /path/to/summitctl
```

The default target is the test VM; `--shell tools/ws.sh --host HOST_ADDRESS`
selects the workstation and the address on which it can reach the test host.
The harness uses a separate profile and addresses only its own browser team.
