# Page translation (#124)

Page translation is under development. Summit does not yet offer a translation
prompt. [Issue #124](https://github.com/jmgasper/summit/issues/124) remains open
until detection, the native prompt, model management and in-place translation
are connected and tested together.

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
13 host CTest targets also pass. CMake and Make share
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
service. The three model files occupy 22,972,674 bytes. Production model
downloads, cache verification, cancellation and UI are still outstanding.

The package passes its options object to the worker through structured clone;
passing an `onerror` function in that object fails with `DataCloneError`.
The successful probes pass only serializable options. Integration must handle
worker failure without leaving requests pending.

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
