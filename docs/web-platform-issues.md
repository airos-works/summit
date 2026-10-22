# Web platform issues, October 2026

Issues #25–30 are implemented. The verified build `bundle-1gm22c31` was installed
on the X399 on October 6, 2026, using private Mesa `prefix-20261002`. Both
`/boot/home/Desktop/Summit-current.sh` and `/boot/home/summit/Summit-installed`
point to it. The previous launcher is backed up. An existing browser instance
keeps its original libraries until restarted.

The bundled engine patch SHA-256 is
`2cfb95568767c2cf33da4659514d55cca851a06ffd761344474341b74b6d9c72`.
The engine, WebProcess, NetworkProcess and native browser all build successfully
on the workstation. Bundle creation verifies source hashes, the completed
engine and the private dependency closure.

## Results

| Issue | Result | Verification |
| --- | --- | --- |
| #25 Fullscreen | DOM requests enter and leave native fullscreen | 9/9 browser checks; Escape restores DOM, native state and original frame; tab switching/opening/closing, navigation and renderer termination restore the window |
| #26 Image formats | JPEG XL and HEIC decoding; JPEG XR decoding and encoding | 20/20 integrated pixel, alpha, ImageBitmap and encoding checks; 256 native JPEG XR codec checks |
| #27 OffscreenCanvas | Main-thread and worker 2D, WebGL and WebGL2 | 28/28 checks, including concurrent GPU rendering, placeholder presentation and transferred ImageBitmap pixels |
| #28 Custom schemes | Recommended native apps, application selection and website handlers | 18/18 API validation checks; approved website round trip 19/19; native argument delivery, remembering, cancellation, stale replies, removal and private isolation |
| #29 WebUSB | Window bindings, native chooser, origin grants and native transfer backend | 54/54 browser checks; 550 policy checks; native protected-device and closed-session guards; real chooser click returns NotFoundError with no eligible devices |
| #122 WebHID | `navigator.hid` with a native chooser, origin grants, input/output/feature reports and connection events over the WebUSB broker | 48 descriptor parser checks (`tests/HIDDescriptorTests.cpp`, devices from the Framework laptop); VIA configures the Keychron Q6 on the Framework laptop (checked by the reporter, 23 October) |
| #30 Toolbar icons | Native scalable HVIF toolbar resources | All normal/disabled icon variants and complete native toolbar visually checked |

Five consecutive OffscreenCanvas and image-codec runs pass with no new crash
reports or debugger events. The final installed desktop launcher was also
started with an isolated profile: OffscreenCanvas 28/28 and image codecs 20/20
pass, and the browser and helpers close cleanly. All four portable CTest suites
pass.

**Physical USB transfer validation is deferred to a future goal at the user's
request.** No eligible test device is attached. The keyboard, Bluetooth adapter
and root hubs were not opened for transfers.

## Implementation notes

Fullscreen uses an acknowledged WebFullScreenManagerProxy transition on the
native window looper, with a five-second timeout. The window saves its normal
frame, look, flags and visible controls. A temporary notice identifies the URL
and Escape key. Its maximum width is unrestricted, so it fills the X399's
3840×1080 display. Session persistence retains the normal window frame. No
additional air/OS change was necessary.

OffscreenCanvas build switches now explicitly migrate retained CMake caches,
and Haiku enables the separate AllowWebGLInWorkers runtime preference. Worker
ANGLE contexts use distinct virtualization groups and do not share the
compositor's native context. Display readback samples the presented texture,
fixing transparent-black output after buffer swaps with antialiasing disabled.
Worker image handoffs use CPU-backed intermediate buffers because Skia GPU
images belong to their creating GrDirectContext. WebGL rendering itself stays
accelerated; the handoff correction also eliminates the observed worker
teardown crash in repeated testing.

Image dependencies are private and checksum-pinned: libjxl 0.12.0, libheif
1.23.6, libde265 1.1.3 and Debian-patched jxrlib 1.2. Preparation and bundling
retain manifests and licenses without replacing OS packages. HEIC decoding
handles dimensions, crops/rotation, alpha and ICC conversion with bounded
allocations. JPEG XR has bounded in-memory encode/decode helpers and a Skia
canvas export path, including GPU readback. Independent binary fixtures are
in `tests/fixtures/image-codecs`.

Protocol handlers validate schemes, templates, committed frame ownership and
same-origin HTTP(S) targets. Website registration requires a native approval
prompt. External links offer Haiku's MIME-recommended application or a file
chooser; BRoster receives the original URL as one argument, without a shell.
Preferences › Link Handlers removes saved choices and denials. Private changes
stay in memory, and stale/background-tab prompt replies do nothing.

WebUSB exposes the Window interfaces, descriptor/result objects, device
operations and connection events. The UI process independently checks frame
ownership, committed origins, activation, filters, transfer limits and leases.
Grants are per-origin and per-browsing-context/session, with separate private
contexts. Native operations cover configuration, alternate interfaces, control,
bulk, interrupt and isochronous transfers; navigation and closure cancel
outstanding work. Transfers are limited to 16 MiB and have a ten-second native
deadline. The device blocklist and every alternate interface's protected class
are checked. Haiku lacks exclusive kernel interface claims, so a composite
device containing any protected interface is excluded as a whole.

Current WebUSB limits: access is restricted to the top-level origin and
same-origin children allowed by Permissions Policy; WorkerNavigator.usb is not
exposed. Haiku has no raw USB reset command, so reset reports NotSupportedError.
USB's permissions-policy value is included in IPC serialization, and transfer
result constructors preserve their supplied DataView objects through GC-traced
attribute caches.

**WebHID (#122).** VIA (`https://www.usevia.app/`) configures QMK keyboards
through WebHID, not WebUSB: it filters devices for a collection with usage page
`0xff60` and usage `0x61`, sends 32-byte output reports and waits for
`inputreport` events ([its shim](https://github.com/the-via/app/blob/935106a990af39bdd1ade881c02366eac486240d/src/shims/node-hid.ts)).
On 21 October 2026 `navigator.hid` was undefined and VIA showed "USB Detection
Error". Summit now implements WebHID on the WebUSB broker:

- `navigator.hid` (`getDevices`, `requestDevice`, `connect`/`disconnect`) and
  `HIDDevice` (`open`, `close`, `forget`, `sendReport`, `sendFeatureReport`,
  `receiveFeatureReport`, `inputreport`, `collections`) in
  `Source/WebCore/Modules/webhid`. Requests use the WebUSB request channel
  with `"api": "hid"`, so the same document, secure-context, same-origin and
  user-activation checks apply in both processes.
- The browser process (`UIProcess/haiku/usb/WebHIDBrokerHaiku.cpp`) lists the
  HID interfaces of USB devices, reads and parses each report descriptor once
  (`HIDReportDescriptorHaiku.h`, tested by `tests/HIDDescriptorTests.cpp`),
  asks with a native chooser ("Connect HID Device"; a choice grants each
  matching interface of that device for the browsing session) and owns the
  open devices. Paths and identities never reach the web process.
- One HID interface is one WebHID device. An interface is offered only when
  none of its top-level collections is on the generic desktop, keyboard,
  consumer, digitizer or FIDO usage pages: WebHID protects keyboards, pointers
  and security keys, and Haiku's `usb_hid` driver reads every interface it has
  a protocol handler for. The raw USB driver cannot reserve an interface, so
  the two would split input reports. QMK's raw HID interface, other vendor
  interfaces and feature-report devices qualify.
- `HIDDeviceHaiku.cpp`: a device thread keeps one interrupt-in transfer of the
  largest input report pending. `usb_raw` holds a device for the whole of a
  transfer and only killing the waiting thread interrupts it, so the thread
  kills that transfer (as WebUSB's deadline does) to send an output report
  (interrupt-out endpoint, else SET_REPORT) or a feature report (SET/GET_REPORT)
  and then queues the next. A device that is not polled keeps its report, so
  replies are not lost. Short reports are padded to the descriptor's length;
  numbered feature reports keep their ID as the first byte, as in other
  browsers. This works with stock Haiku's `usb_raw` too.
- Input reaches the page through a waiting request: the browser answers it as
  soon as reports arrive (up to 64 at a time) or after two seconds. A page that
  stops reading loses its oldest reports beyond 1024 (or 1 MiB).

Limits: HID over Bluetooth, worker exposure and `HIDDevice` in dedicated
workers are not implemented; gamepads and other generic desktop collections
are not offered (they belong to `usb_hid`).

Toolbar icons are native HVIF resources rendered at the display scale, with
BControl's pressed and disabled variants. Back, Forward, Stop and Home come
from Haiku WebPositive; the remaining toolbar vectors are Summit resources.

## Reproducing validation

Serve the repository root on the workstation's loopback address or HTTPS and
open the pages under `tools/bench/pages`:

- `fullscreen.html`: activation, promises/events, viewport dimensions, exit and
  iframe policy. Also compare native frames during Escape, tab changes,
  navigation and renderer termination.
- `offscreen-canvas.html`: 2D pixels, PNG, bitmaprenderer, clearing/resizing,
  worker-first GL, WebGL/WebGL2 GPU readback, concurrent contexts, transferred
  placeholder canvases and returned ImageBitmaps.
- `image-codecs.html`: independent fixtures, exact/toleranced pixels and alpha,
  ImageBitmap, JPEG XR MIME/signature and canvas/toBlob/Offscreen encoding.
- `webusb.html`: interfaces, constructors, filters, activation, empty grants,
  DataView identity, subclasses, frozen packet arrays and iframe policy.
  The manual chooser/forget actions perform no transfers.
- `protocol-handlers.html`: validation plus manual registration, approval,
  round-trip URLs and unregister. `tools/bench/protocol-target.cpp` provides a
  native argument recorder; remove its test MIME registration after use.

Automatic pages expose `window.testResults` and `window.testDone`. Fullscreen
entry can be requested with the native Developer Tools; genuine pointer input
is needed to verify the UI-process activation check for the USB chooser.

The html5test.co OffscreenCanvas 2D probe checks
`context instanceof CanvasRenderingContext2D`. The standard uses the separate
`OffscreenCanvasRenderingContext2D` interface; the drawing and readback probes
above verify actual functionality without changing that interface hierarchy.
Its JPEG XR check tests canvas encoding with `image/vnd.ms-photo`, so decoding
alone is insufficient.

Build settings:

```sh
export SUMMIT_REMOTE_SHELL=tools/ws.sh SUMMIT_REMOTE_TAG=ws
export SUMMIT_ENGINE_BUILD_NAME=SkiaCGMiPGO SUMMIT_WEBKIT_JOBS=24
export SUMMIT_ENGINE_CXX_FLAGS='-ftrack-macro-expansion=0 -fprofile-use -fprofile-correction -fprofile-partial-training -Wno-error=coverage-mismatch -Wno-coverage-mismatch'
export SUMMIT_ENGINE_CMAKE_EXTRA='-DUSE_SKIA=ON -DUSE_HAIKU_GL_COMPOSITING=ON -DENABLE_WEBGL=ON -DENABLE_FULLSCREEN_API=ON -DENABLE_OFFSCREEN_CANVAS=ON -DENABLE_OFFSCREEN_CANVAS_IN_WORKERS=ON'
bash tools/build-webkit-in-vm.sh --modern-extensions all
bash tools/build-modern-browser-in-vm.sh --browser --bundle --modern-extensions
```

Do not change native engine sources or start another engine build while its
ninja is live. Test a frozen bundle with a separate profile, and address only
its returned team/process group. Local task evidence includes
`.vm/issues-final-auto-results.json`, `.vm/issues-fullscreen-integrated-results.json`,
`.vm/issues-worker-stability-results.json`, `.vm/issues-protocol-private-results.json`
and `.vm/issues-installed-smoke.json`.
