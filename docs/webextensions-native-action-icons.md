# Native dynamic action icons

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

The native icon suite passes against `bundle-ptx4moys`: **49 API cases, 55 SDK
and desktop pixel observations, and 455 native checks**. The owned browser and
helpers exit cleanly, the crash monitor passes, and the frozen bundle and staged
inputs remain unchanged. The same bundle also passes 238 action/popup checks
and 169 overflow checks with clean native exits and no crash events.
The full browser and arbitrary extension compatibility remain unfinished.

The current native bundle is
`/SummitExtensions/summit/build-modern-browser/bundle-ptx4moys`. Its selected
report is `.vm/modern-browser-action-icons-malformed-svg-bundle-result.json`.
The larger volume avoids the boot filesystem allocation failure encountered
while copying the new library.

The matching host copy is `artifacts/modern-browser/bundle-ptx4moys`, from
source revision `a807a22` after the SVG fix in `5375aea`. Its provenance digest is
`800941a35e53700d2b83c77721bcd9d4d9d645f6c89a195b1c3f33ef705af6d6`;
the WebKit source archive digest is
`beba5136830221cd40d24865288ab2f29fb59e97fc5d49f896c93da497a96b54`.
All 399 artifact entries match their recorded hashes, with matching source and
support fingerprints before and after copying.

The previous verified bundle is preserved at
`artifacts/modern-browser/bundle-_inqiw0r`. Copy provenance is
`1474b16a3a8c78464c5067e44d7c4769d1fed85af7959c5566b0cdf5b19a4dbb`;
the matching WebKit source archive digest is
`55292ce7df17670c59740adb716631477e3975e3ce0f8be52678bf2f50f6a49f`.
The copy records host revision `6cc95e9`, after native implementation commit
`e94637d`, and verifies 399 artifact entries. Source and support fingerprints
match before and after the copy.

QEMU's VNC display `127.0.0.1:5905` runs `bundle-ptx4moys`. Its owned preview
uses a fresh copy of the passing icon-test profile; the 22 source-profile files
remain unchanged. `.vm/vnc-follow-along-browser.json` records its live ownership
and profile. Close that owned preview before another exclusive native suite.

## Behavior

`setIcon` accepts paths, size-to-path dictionaries, genuine `ImageData`, and
size-to-`ImageData` dictionaries. The binding retains raw JavaScript values and
uses native wrapper checks, including when constructors, dimensions, or typed
array properties are shadowed. It copies image buffers before encoding PNG for
IPC, so subsequent JavaScript mutations cannot change the installed icon.

Relative paths resolve against the calling extension frame. Rooted paths and
same-extension absolute URLs use the package resource loader. External URLs are
rejected. Native decoders handle bitmap and SVG data URLs, including SVG media
types without a semicolon.

SVG rendering creates WebCore's isolated image document. The UI process now
provides the required platform loader, which accepts only inline data resources
in SVG image frames and completes their decoding before rasterization. Scripting
and media stay disabled. SVG files, scripted SVG isolation, embedded PNGs, and
malformed embedded images are covered by the native pixel suite.

A completed SVG image load does not itself prove that XML parsing succeeded.
The decoder now requires a well-formed document with an SVG root before an
icon can replace its predecessor. Tests reject plain text, a wrong SVG
namespace, XHTML, mismatched tags and trailing content, both directly and as
one entry in a representation dictionary. Valid empty SVGs remain transparent
icons, and viewBox-only SVGs render without explicit width/height attributes.
The new rejection case failed against the previous bundle before this fix.

The tests also exposed a Haiku image-buffer mismatch: B_RGBA32 stores straight
alpha, while the common pixel helpers assumed premultiplied storage. The Haiku
backend now uses the native storage format for reads and writes and synchronizes
its BView before accessing bitmap memory. Coverage includes translucent icons,
canvas painting, exact native canvas readback with clipping, SVG alpha, and PNG
alpha inside SVG. The readback fixture exceeds WebKit's 60x60 `putImageData`
cache so it exercises the native backend directly.

The UI receiver independently validates the serialized dictionary and target
access, decodes every representation before mutation, and preserves the old
icon on failure. Successful changes use the existing inheritance, invalidation,
observer, SDK export and toolbar drawing paths.

Null, undefined, omitted and empty dictionaries reset the selected scope. Tab
and window targets cannot be combined. Only known detail properties are read;
inherited properties are accepted and throwing getters/proxies are rejected.
These inputs and resets follow the [Firefox API](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/browserAction/setIcon)
and the [Chrome API/callback contract](https://developer.chrome.com/docs/extensions/reference/api/action#method-setIcon).

Limits are 16 Mi characters of serialized icon JSON, 256 representations and
64 MiB per ImageData buffer. Size keys are canonical positive 32-bit integers.
The upstream icon-variants feature remains disabled on Haiku. Cocoa compilation
and runtime have not been tested.

## Evidence

Engine patch:
`0cc5c59e2d4732fe3b02d38b898ce74b5cec00c477980cad43863d93f1071d70`.

- `.vm/modern-browser-action-icons-malformed-svg-bundle-result.json`: immutable native
  browser bundle built from the completed engine.
- `.vm/modern-extension-icons-217bac07928b10ec344a04a1/result.json`: passing native
  icon suite, clean process exits and crash monitor, unchanged source inputs and
  bundle. Native stage: `/boot/home/summit/extension-icon-inputs.BhS9aEbp`.
- `.vm/modern-extension-actions-acd7aa8b4afd4b451d0737fb/result.json`: 238
  action/popup checks over installation, popup quit, removal and restart phases.
- `.vm/modern-extension-overflow-0d8ea8619623bd7ea575781a/result.json`: 169 native
  overflow checks. Both regression reports preserve bundle/staged input hashes,
  record clean browser/helper exits and have passing crash monitors.
- `.vm/modern-extension-icons-eae529207fe9df60c59584a0/result.json`:
  the old bundle accepts malformed SVG and fails the new rejection case.
- `.vm/action-icon-malformed-svg-engine-build.log`: successful rebuild/link
  after document validation; `.vm/extension-lifecycle-inputs.nWmmUuQY/result.json`
  records its isolated compile.
- `.vm/bundle-copy-diagnostic.json`: boot-volume copy failure at 72,679,424 bytes
  with 310,343,680 bytes still free; the full bundle succeeds on the larger volume.
- `.vm/action-icon-alpha-engine-build.log`: successful WebCore, WebKit,
  NetworkProcess and WebProcess build/link after the alpha correction.
- `.vm/extension-lifecycle-inputs.YNxUk7WV/result.json`: isolated native compile
  of the SVG loader; no warnings.
- `.vm/extension-lifecycle-inputs.SRwK7L7Z/result.json`: six action API, parser,
  receiver and generated-binding integration units compile.
- `.vm/action-icon-promoted-binding-validation.json`: 510 generation/preprocessor
  checks against the earlier `84d3e1b1...` patch. Later changes affect only native
  SVG loading and image-buffer storage, not the bindings.

Tests drive real installation controls and toolbar clicks. Each observation
checks decoded SDK pixels and alpha-composited desktop pixels before advancing.
Coverage includes paths, representation selection, image snapshots, callbacks,
invalid-input atomicity, inherited state, tab switching, navigation resets and
closed-tab rejection, alongside the rendering cases above.

```sh
python3 tools/test-modern-extension-icons.py --watch --bundle /path/to/native/bundle
python3 tools/test-modern-extension-startup.py --manager --actions --chrome-key --watch \
  --bundle /path/to/native/bundle
python3 tools/test-modern-extension-overflow.py --bundle /path/to/native/bundle
```

Run these suites serially after closing the owned VNC preview and draining its
helpers: crash-log observation is global to the VM. Failed bundles remain
preserved. The first run's manifest SVG crash is recorded in
`.vm/action-icon-svg-crash.report`; subsequent SVG/alpha diagnostic runs and the
passing icon run have clean native exits and crash logs.

Further work includes external SVG resource coverage, private/multiwindow scenarios, programmatic `openPopup`, MV3 service
workers, and representative Safari/Chrome/Firefox extensions. Passing this
fixture does not establish arbitrary third-party extension compatibility.
