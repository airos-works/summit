# Extension localization work

Native browser bundle `bundle-omqykez8` uses engine patch
`97c027b989cd0ac2f0707fe777c4e504b33827666a7eaa650ded22f28b4c912c`.
It exposes five localization methods on Haiku and collects injected author styles
during initial render-tree creation, including pages without their own stylesheets.
The installed-extension integration run passes 329 checks: 79 JavaScript assertions
in each of the background, popup and content-script worlds, 13 assertions for an
extension without a translation catalog, and native installation/UI/lifecycle
checks. The programmatic popup regression passes 342 checks across 41 commands;
the icon regression passes 455 checks across 49 cases and 55 pixel observations.
All three runs exited normally, drained their owned processes, preserved all recorded
source/package/bundle hashes, and recorded complete crash-log coverage without
debugger events. This does not establish published-extension compatibility.

The matching host artifact is `artifacts/modern-browser/bundle-omqykez8/`.
The copy records host revision `886704cfab7dcbbbe0c220f8c01ccc41de43523c`;
source and build-support fingerprints match before and after archiving, and
all 399 artifact entries match the final inventory. Its provenance digest is
`2367a3fb5d5bb8b2c9f3df90b3de8daf9ae66c635072a1a0fb4911fe0ba13ef2`;
the matching WebKit source archive digest is
`4c4d3311e203bb514517d6c833b29739f4e05634f43fd24e517a5b9e03b2a5be`.
Validation is recorded in `.vm/i18n-css-fixed-artifact-validation.json`.
The [VNC preview on port 5905](VM.md) advances to later verified bundles;
`.vm/vnc-follow-along-browser.json` records its current bundle and owned process.

The implementation exposes the five localization methods already defined by the
pinned WebKit interface through C++ bindings on Haiku. It also repairs shared
message processing: replacement text is appended without scanning it again,
escaped dollar occurrences remain literal beside unescaped occurrences, and
`escapeLt` applies before caller substitutions. ICU supplies text direction on
ports without HarfBuzz; the previous fallback caused English to report RTL.

`tests/EngineExtensionLocalizationTests.cpp` exercises the actual shared
localizer and ICU locale source. The runner links those implementations as
executable-local definitions, with real dependencies from the matching frozen
bundle. It checks catalog precedence, case folding, predefined messages, English
and Arabic direction, recursive JSON localization, named and positional
substitutions, dollar escaping, Unicode, and nonrecursive manifest token insertion.
It creates no mock extension context and does not exercise JavaScript API bindings.

The first native run completed 38 assertions with 9 failures, including wrong
English direction and corruption of caller-supplied dollar text. The adjacent
placeholder fixture was then corrected to use valid adjacent named placeholders.
After the implementation fixes, the expanded suite passed 44 assertions with no
debugger events. Source, configured native inputs, and frozen dependency hashes
were unchanged throughout that run. Evidence:

- `.vm/extension-localization-tests.xCgQ2AKH/runtime.json`: initial failures.
- `.vm/extension-localization-tests.7jawQXs5/runtime.json`: 44 passing assertions.
- `.vm/i18n-next.json`: current work and remaining compatibility coverage.

To compile the candidate helper:

```sh
python3 tools/test-engine-extension-localization.py \
  --overlay .vm/i18n-candidate \
  --bundle-report MATCHING_FROZEN_BUNDLE_REPORT
```

The report must match the configured native engine patch; the old popup bundle
no longer matches once the i18n engine build reconfigures the native tree.
The command prints a native stage. After other browser tests and the owned VNC
preview have closed, execute that stage once:

```sh
python3 tools/test-engine-extension-localization.py --run-stage NATIVE_STAGE
```

The revised options parser validates own string properties, including
nonenumerable properties, and rejects unknown keys, nonboolean `escapeLt`, and
throwing getters or enumeration. Inherited properties and symbols are ignored;
optional null/undefined values are omitted. Validation runs before substitution
conversion, consistent with Chromium's
[argument parser](https://github.com/chromium/chromium/blob/main/extensions/renderer/bindings/argument_spec.cc)
and [i18n dispatch](https://github.com/chromium/chromium/blob/main/extensions/renderer/api/i18n_hooks_delegate.cc).
Generated bindings pass 560 platform checks; five native API/namespace/shared
units compile after this change. Neither check runs the API in a browser.

`tests/fixtures/extensions/i18n` contains 79 independently specified expected
assertions for each of the background, popup and content-script contexts.
The `i18n-unlocalized` fixture adds 13 background checks for an extension with no
catalog or default locale, including identity, empty predefined locale messages,
and argument validation. The runner installs it as a second independent package.
`tools/test-modern-extension-localization.py` installs it through the native
picker and permission UI, verifies reports against `expected.json`, and checks
the visible popup, native action title, localized manifest/CSS and HTTP language
header. Its native harness compiled successfully. As a negative control, the
old popup bundle failed when the background called its missing `i18n.getMessage`;
installation and report transport worked, the browser exited normally, its
process group drained, and no debugger event occurred. Evidence:

- `.vm/extension-lifecycle-inputs.Ntj016ve/result.json`: native API compile.
- `.vm/extension-binding-platforms-ih9viaai/result.json`: binding checks.
- `.vm/modern-extension-localization-47cd9d6706aff3d24e34bdda/result.json`: harness compile only.
- `.vm/modern-extension-localization-f055950b86681dafc63d5acf/result.json`: expected missing-API failure.
- `.vm/modern-extension-localization-4d983361ad8336d9c361820f/result.json`: expanded harness with unlocalized fixture, compile only.

To run the integration suite against the verified native bundle:

```sh
python3 tools/test-modern-extension-localization.py \
  --bundle /SummitExtensions/summit/build-modern-browser/bundle-omqykez8
```

Passing browser evidence:

- `.vm/modern-extension-localization-57598881eeb660ed2507a526/result.json`: 329 checks, including initial localized CSS on a page with no stylesheets.
- `.vm/modern-extension-open-popup-8aa6ee513861c2bc36bc041b/result.json`: 342 popup regression checks.
- `.vm/modern-extension-icons-3ea51ae05d58bef7188acc6c/result.json`: 455 icon regression checks, including SVG utility documents affected by initial style collection.
- `.vm/modern-browser-i18n-css-fixed-bundle-result.json`: frozen native build inputs.

Remaining work includes additional locale-selection/fallback runs and unmodified
published extensions.
The shared dollar behavior also needs a cross-browser review: consecutive runs
currently use Chrome's one-escape-per-run behavior, while unmatched text retains
WebKit's behavior. Cocoa compilation and runtime remain unverified.

`i18n.detectLanguage(text)` (absent from the pinned WebKit API) is answered by
the browser: a `DETECT_LANGUAGE` browser command, which Summit answers with the
CLD2 detector page translation uses (`summit::DetectTextLanguages`, the first
64 KiB of the text). It gives Firefox's `{isReliable, languages: [{language,
percentage}]}`: up to three languages, most of the text first, with CLD2's
codes (`zh-Hant` for traditional Chinese), and no languages for text without
one. Content scripts may call it too. Fixture:
`tests/fixtures/extensions/detect-language`.

The first browser result is
`.vm/modern-extension-localization-67e8e2b23940ec3768e781ff/result.json`.
A diagnostic follow-up,
`.vm/modern-extension-localization-8a8fbff5544b8adb7fd48953/result.json`, adds
plain color and literal pseudo-element rules. All three rules work in the
background page; none applies to the content-script probe, immediately or
250 ms later. Both runs exited normally, drained their owned browser processes,
preserved source/package/bundle hashes, and recorded complete crash-log coverage
without debugger events. The initial CSS expectations remained unchanged during
diagnosis.

Native traces established that the stylesheet reaches the matching document
and all three rules parse, but a page without its own stylesheet does not collect
the injected author styles initially. In
`.vm/modern-extension-localization-2bb1bfb0706bca978062d8f7/result.json`, inserting
an unrelated page stylesheet makes all three extension rules apply; they remain
active after removing it. The final candidate schedules active stylesheet
collection before the first render-tree style resolution. Temporary engine
traces and the diagnostic page mutation have been removed. The fixture now
checks plain color, literal pseudo-element text, translated pseudo-element text
and direction. It gives the parent the opposite direction so a missing CSS rule
cannot pass by inheriting the page's default direction.

During diagnosis, the test VM's boot ATA device stalled. After reboot, the
incomplete `bundle-y_4lmne4` failed 17 file checks and is excluded from testing.
The previous verified preview and all 14 recorded engine/build inputs matched
their saved hashes. A new diagnostic bundle was built, synced and verified
before the decisive runtime run. Recovery evidence is in
`.vm/css-resolve-vm-recovery.json`; the invalid bundle has not been repaired or
represented as verified.

Verified on the X399 (25 Oct, lf113 build): `i18n.detectLanguage` gave `de`
(99 %, reliable) for German, `en` and `ja` for English and Japanese text,
from a content script too, and `tabs.detectLanguage` gave `de` for
`tests/fixtures/detect-language.html` (no `lang`). That run showed the
fixture's own non-ASCII strings garbled: extension resources were served
without a charset, so a Manifest V2 generated background page read its
scripts as windows-1252. Extension text (HTML, scripts, CSS, JSON, XML) is
now served as UTF-8, as in Chrome and Firefox
(`WebExtensionURLSchemeHandlerHaiku.cpp`).
