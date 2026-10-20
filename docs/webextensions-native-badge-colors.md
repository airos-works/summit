# Native extension badge colors

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

Summit's native toolbar displays extension-supplied badge background and text
colors. The Haiku action bindings implement `getBadgeBackgroundColor`,
`setBadgeBackgroundColor`, `getBadgeTextColor`, and `setBadgeTextColor` through
the real extension IPC and action model. The verified native browser bundle is
`/boot/home/summit/build-modern-browser/bundle-lzht66nf`.

## Behavior

Colors accept absolute CSS color strings or arrays of four integer RGBA bytes
from 0 through 255. The WebCore parser handles CSS names, hexadecimal, RGB and
HSL forms. Invalid CSS rejects the returned promise without changing state.
Malformed arrays, inaccessible targets and conflicting tab/window scopes also
fail without modifying the existing colors. Context-dependent CSS values such
as `currentColor`, system colors and variables are unavailable because these
settings have no document context.

Tab overrides inherit from window overrides, which inherit from the global
action. A null color restores that inheritance. The default background is
`[217, 0, 0, 255]`. Without an explicit inherited text color, the model chooses
black or white for contrast against the effective background, composited over
the native panel color. Thus a tab's background override gets its own automatic
contrast result. An omitted text color also restores automatic selection.
Committed navigation clears tab overrides through the existing action lifecycle.
These inheritance and contrast choices follow the documented
[Firefox background-color behavior](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/browserAction/setBadgeBackgroundColor).
Fully transparent text is rejected, following the
[Chrome action text-color contract](https://developer.chrome.com/docs/extensions/reference/api/action#method-setBadgeTextColor).

CSS parsing and target lookup run in the privileged UI-process receiver. The
SDK carries effective colors as `badge_background_rgba` and `badge_text_rgba`,
both unsigned 32-bit values packed as `0xRRGGBBAA`. The existing property-change
observer refreshes the toolbar. Native drawing uses alpha compositing and
attenuates disabled badges while retaining their configured colors.

## Verification

Engine patch
`23a657589a6f9ff50f14517951d8bfc880645945263a9d994118b4dabf0ea986`
builds and links the complete extension-enabled WebKit, WebProcess and
NetworkProcess. The initial build's color-component accessor error was corrected
with WebCore's public `resolved()` accessor. The successful incremental build is
recorded in `.vm/modern-extensions-badge-colors-fixed-build.log`.

```sh
python3 tools/test-modern-extension-startup.py --manager --actions --chrome-key --watch \
  --bundle /boot/home/summit/build-modern-browser/bundle-lzht66nf
python3 tools/test-modern-extension-overflow.py \
  --bundle /boot/home/summit/build-modern-browser/bundle-lzht66nf
```

The action suite passes **238 native UI checks and 29 identity checks**. Actual
MV2 extension JavaScript completes **58 color assertions on each of four loads**,
covering CSS/RGBA values, alpha round trips, promise rejection, Chrome callbacks,
global/window/tab inheritance, null resets and invalid-input atomicity. Native
tests verify tab switching and navigation resets, popup-driven color changes,
and actual screen pixels for a blue badge with yellow text. The runner wakes
the screen saver before the pixel test and validates capture bounds.

The six-extension overflow regression passes **169 native checks**. All five
browser processes across these suites exit zero, their process groups drain
without forced cleanup, and the crash-log intervals contain no new events.
Frozen bundles, staged inputs and host test sources remain unchanged.

Evidence:

- `.vm/modern-extension-actions-1124e8a8d646a8a6a006cf09/result.json`
- `.vm/modern-extension-overflow-f997d5dfb9270db7ffa51215/result.json`
- `.vm/modern-browser-badge-colors-bundle-result.json`
- `.vm/badge-color-binding-validation.json`: **509 generation/preprocessing
  checks**, with input hashes matching the promoted patch. Background-color
  methods remain shared; the new text-color methods are Haiku-specific. This
  check does not compile or run Cocoa.

## Remaining compatibility work

The runtime fixture exercises MV2 `browserAction`. It does not establish MV3
service-worker support or arbitrary Safari, Chrome or Firefox extension
compatibility. Rendering across theme changes, partially transparent and
disabled badges needs more pixel coverage. Dynamic icon APIs, `openPopup`,
multi-window/private contexts and the broader extension corpus remain work.

## Copied artifact and VNC preview

The verified artifact is `artifacts/modern-browser/bundle-lzht66nf/`. Copy the
whole directory to Haiku R1/beta6 x86_64 and run `./run-browser.sh`. It contains
the private runtime, frozen application inputs and matching patched WebKit source.
The copy records source commit `466f4b1` and these SHA-256 digests:

- Copy provenance: `4892ae8ab988b1f8bc57c24256a5b293f1cfbc4df27ac2545821410b5b812b23`.
- WebKit source archive: `f92213370e7b2ab90373c317b9c2cfe2b8ef3525f5348de7ee69f16e402ea1dc`.

All 399 artifact entries match the recorded inventory. Native and host reports,
source fingerprints and build-support fingerprints match. The complete copy log
is `.vm/modern-browser-badge-colors-bundle-copy.log`; the independent inventory
check is `.vm/badge-color-artifact-verification.json`.

The VNC desktop at `127.0.0.1:5905` uses a separate clone of the test's preview
profile. Its original 19 files remained unchanged. The reopened extension
completed all 58 color assertions and loaded its saved popup state. The screenshot
shows actual popup JavaScript changing the badge to purple with white text.

![Native extension popup with a purple badge and white text](screenshots/extension-badge-colors.png)
