# Zoom browser joining and media permissions (#127)

[Issue #127](https://github.com/jmgasper/summit/issues/127) reports no camera or
microphone permission prompt while trying to join Zoom. The reporter's entry
path is not yet established. The workstation investigation on 22 October 2026
reproduced an earlier obstacle in the public `https://zoom.us/test` flow.

## Launcher diagnosis

With the published `8224d7a` engine (`20261021.2246`), the default user agent
identifies Safari 26 on Haiku. The test page loads, but its Join action reaches
a launcher that rejects the operating system and offers no browser join option.
No media request was observed on that launcher.

Selecting the existing **Safari — macOS** preference in the isolated test
profile makes the launcher offer **Join from browser**. The resulting web-app
shell initially reports a blocked popup; its **Try again** action opens the
actual prejoin page. That page presents a native Summit microphone permission
question. Restoring the default user agent and reloading the actual prejoin
page also presents the microphone question.

This distinguishes the launcher's OS check from the permission mechanism on
the actual prejoin page. It does not establish that the reporter followed the
same route. Zoom documents the expected browser permissions in its
[web-app permissions guide](https://support.zoom.com/hc/en/article?id=zm_kb&sysparm_article=KB0078476).

## Compatibility behavior

The Haiku engine now uses the desktop macOS OS token for HTTP(S) pages on
`zoom.us` and its subdomains when using the default user agent. It retains its
Safari 26/WebKit identity. Unrelated sites and similarly named hosts retain
the ordinary Haiku user agent. Disabling site-specific quirks disables this
rule.

Explicit custom user agents take precedence, including a custom string that
is identical to the engine's default. The UI process applies the rule to
initial navigation requests; the web process applies it to DOM queries and
page requests. Page creation carries the custom-agent flag, and changes to
that flag invalidate the cached DOM value even when the underlying string
does not change. Clearing the custom setting restores the site rule.

## Native regression test

`tests/ModernUserAgentTests.cpp` is a native host using the public WebKit view
and Inspector APIs. `tools/test-modern-user-agent.py` supplies an HTTP proxy
bound to loopback. Every request is answered locally; no traffic is forwarded
to Zoom or the lookalike domains, and no DNS or hosts settings are changed.

The 24 scenarios check initial navigation headers, `navigator.userAgent`,
subresource request headers, exact and nested Zoom hosts, lookalike hosts,
cross-site navigation, initial custom values, live changes, explicit values
equal to the default, clearing overrides, and independent pages. Both HTTP
observations and DOM assertions must pass. The runner records loaded native
images, checks the selected engine binaries, checks input hashes again after
exit, waits for helper teardown, and checks the complete native crash-log
interval.

The published baseline passes 105 native checks with the original Haiku value
on Zoom. The candidate passes the same 105 checks with the site rule enabled.
Each run covers 24 scenarios, verifies 76 input/runtime hashes, exits normally
with no remaining helpers, and has a complete crash-log interval with no
debugger events. Results and source provenance are in
[`zoom-camera-2026-10-22.json`](zoom-camera-2026-10-22.json).

The candidate was also exercised against the real HTTPS test flow using a
fresh Summit profile and the default preference. The launcher offers **Join
from browser**; the prejoin page identifies Safari on the recognized desktop
OS and presents **“app.zoom.us wants to use your microphone.”** The question
was answered **Not Now**. All four loaded engine images matched the selected
candidate, all 359 recorded file hashes were unchanged after exit, and the
browser and helpers exited normally without a fresh debugger event.

The first browser-link attempt reached the web-app home without a visible
retry banner. A second test launch used DOM activation for the launcher links,
then native input on **Try again**, reaching prejoin. This establishes the
launcher and permission observations; it is not an uninterrupted manual-click
acceptance of the entire Zoom flow.

The workstation's BFS volume rejected copying the large candidate library
with `EFBIG`, including an attempt that preallocated its size. The accepted
candidate therefore ran from an owned temporary RAM filesystem after all 242
baseline package files and the replacement engine files were verified. This
mount was removed after testing. The published baseline ran from BFS; these
are compatibility checks, not storage or performance comparisons.

To run on native Haiku with a matching built probe and isolated engine:

```sh
python3 tools/test-modern-user-agent.py \
  --probe /path/to/ModernUserAgentTests \
  --bundle /path/to/lib/summit-webkit \
  --images-helper /path/to/NativeLoadedImages \
  --egl-vendor /path/to/egl-vendor.json \
  --output /new/test-output-directory
```

Add `--baseline` only when testing an engine without the Zoom rule. The probe
must link to the selected engine; the runner verifies the actual loaded images.

## Remaining acceptance

#127 remains open for the Framework camera path and a real Zoom audio/video
retest. The workstation enumerates no camera. Native microphone questions were
answered **Not Now**; no microphone recording or meeting participation was
used for this investigation. The launcher fix does not verify media capture,
audio quality, camera preview, or a completed meeting.

The popup shell's **Try again** step remains part of the observed public flow;
this change does not alter popup handling. The native screen blanker made the
available screenshot black, so page and native-control observations, rather
than that screenshot, support the baseline findings.
