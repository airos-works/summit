# Page translation (#124)

Modern Summit offers local page translation when a page's detected language
differs from the first language in the system Locale preferences. The native
notice offers **Translate** and **Not Now**. View → Translate Page reopens the
offer; **Show Original** restores the original text.

The first translation downloads verified language files, usually 20–37 MB per
direction. Later translations reuse the cache in the profile's
`TranslationModels` directory. Page text stays on the device. The translator
runs in a private hidden WebKit view with a self-only content security policy;
the native downloader fetches only the selected model files. Downloads can be
cancelled, and partial or unverifiable files are never used.

The pinned registry supports Bulgarian, Czech, German, English, Spanish,
Estonian, French, Italian, Portuguese, Russian and Ukrainian. Pairs without
a direct model use English as an intermediate language when both directions
are available. An unsupported Locale language is never silently replaced with
English; the manual command explains that the pair is unavailable.

Translation covers eligible text in the main document body, including new
content and text the website later changes. It preserves elements, links,
listeners and form values. It skips editable content, code, hidden subtrees,
`translate="no"`, and `.notranslate`. Frames, shadow trees, element attributes
and document titles are not translated. Work and retained originals have
limits; the notice identifies a partial translation when a limit is reached.

Implementation and acceptance for
[issue #124](https://github.com/jmgasper/summit/issues/124) are described below.

## Language detection

`src/core/PageLanguage.cpp` uses the pinned CLD2 library in `vendor/cld2` to
detect the dominant language of a bounded plain-text sample. Detection runs
locally. It validates UTF-8, rejects samples over 64 KiB, and requires CLD2's
confidence flag, at least 200 text bytes and an 80% majority before suggesting
translation. A short title or a page split evenly between languages therefore
does not trigger an automatic offer. The page's `lang` attribute does not
override the statistical result.

The target is the first language selected in the system's Locale preferences.
`PreferredPageLanguage` normalizes Haiku locales such as `en_AU` to language
keys without silently choosing a secondary or more widely supported language.
Regional English, French and similar variants compare as the same language;
the simplified and traditional Chinese writing systems remain distinct.

`tests/PageLanguageTests.cpp` passes **90 checks** on Linux and the native
airOS x86_64 workstation (22 October 2026). These cover English, German, French,
Spanish, Russian, Japanese, simplified and traditional Chinese, mixed and
short text, malformed UTF-8, the sample limit and locale normalization. All
14 host CTest targets also pass. CMake and Make share
`vendor/cld2/sources.list`; the standalone modern-browser builder has the same
sources in its literal list, which release CI parses. A small documented
upstream patch preserves one generated table's unsigned initialization under
modern C++. Its license is included in the browser packaging paths and in
`docs/cld2-license.txt` for CI's documentation-based package layout.
The patched C++23 table's native `.rodata` bytes match the original C++98
object exactly; the native detector checks pass under both standards.

## Local translation prototype

The native workstation successfully ran
[`@browsermt/bergamot-translator` 0.4.9](https://www.npmjs.com/package/@browsermt/bergamot-translator/v/0.4.9)
with its German-to-English model. This is an engine feasibility check, not
page-translation acceptance. The npm archive's SHA-512 integrity and all
three model files' SHA-256 hashes were verified before the test.

The input was:

> Guten Morgen! Dies ist ein Test der lokalen Übersetzung. Der Browser
> übersetzt diese Seite auf Ihrem Computer. Die Texte verlassen das Gerät nicht.

The output was:

> Good morning! This is a test of local translation. The browser translates
> this page on your computer. The texts do not leave the device.

The first run used a localhost fixture in Summit on engine `eb9af69`, with
one worker and the portable WebAssembly matrix routines (`useNativeIntGemm:
false`), and completed in **1,205 ms** including initialization and loading
the already-downloaded model. A separate native probe then completed the
same translation in **1,224 ms** in a hidden `BWebKitView`, with a private
context and browser-owned `summit-translate:` resource handler. Its CSP
allowed only its own scripts, worker and model requests. That probe exited
normally after destroying the view and draining native UI teardown.

The prototype reads local files; no page text is sent to a translation
service. The three model files occupy 22,972,674 bytes. The integration below
connects the model downloader and cache to the browser UI.

The package passes its options object to the worker through structured clone;
passing an `onerror` function in that object fails with `DataCloneError`.
The successful probes pass only serializable options. Integration must handle
worker failure without leaving requests pending.

## Native integration acceptance

On the airOS workstation, the browser detects the German acceptance fixture
and offers English from the system Locale preferences. Its first download
verifies all 22,972,674 bytes before starting the worker. The private WebKit
context is created asynchronously on the application thread, as the embedding
API requires; resource handling and UI updates run on their own loopers.

The initial integration passes **20 native checks** using
`tests/fixtures/page-translation.html`. They verify real German-to-English
output, spacing around an inline link, retained element identities, preserved
event listeners, form values and excluded text, new content translated without
another click, and original text restored after the website moves a node.
Restoration also preserves a later website edit. The fixture's strict CSP and
a conflicting website-global controller do not interfere with translation.
The owned browser exits cleanly with its translation view and worker running.

Another **10 native checks** cover cancellation of a real cold download,
removal of partial files, an actionable error when the download host is
unavailable, cached translation with downloads blocked, delayed article
content, fresh state after reload, rejection of late results during navigation,
and clean shutdown. A separate cache-recovery check corrupts one byte of the
5,047,568-byte lexicon without changing its size: Summit rejects it, downloads
a verified replacement, and successfully translates the page. The visible
native offer and completion notices were also inspected on screen.

Moving a translated tab to another window preserves its text and resumes
translation of new content there. Show Original in the receiving window
restores the same document's original text.

`tests/TranslationModelsTests.cpp` passes **69 checks** for direct and intermediate routes,
unsupported languages, all pinned model configurations, and malformed model
URLs, sizes and hashes. The worker is the unmodified pinned upstream package;
see `resources/translation/sources.json`, `docs/bergamot-license.txt` and
`docs/translation-runtime-notices.txt` for integrity, source and licenses.

## Retaining original DOM nodes

The modern API now has a five-argument `EvaluateJavaScriptIsolated` overload.
Setting its final `keepWorld` argument to `true` reuses one browser-owned
content world for the view. Translation can retain references to text nodes
and their originals without adding properties or identifiers to the website's
world. A new document has new globals; another view has an independent world.
The view releases the retained world on the WebKit main thread when closing.
The existing four-argument API still creates a fresh world on every call.

Both forms check the load generation before execution and before accepting
the result, and neither grants nor consumes user activation. The native
`tests/NativeIsolatedWorldTests.cpp` acceptance program passes **18 checks**
on the workstation, including restoring the same node after the website moves
it, page-global and prototype isolation, independent views, fresh calls,
navigation, stale requests that must not run, and normal teardown. The test
uses its own private context and local scheme handler; it needs no network
or existing browser profile.
