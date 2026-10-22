# Spelling dictionaries from page and field language

Added 22 October 2026 for [#110](https://github.com/jmgasper/summit/issues/110).
Automatic spelling now uses the language of the text being checked. Previously,
every page used the same dictionary selected at browser startup, even when the
document declared another language.

## Behavior

Preferences › Browsing › Spelling › Language defaults to **Page language
(system fallback)**. A field's inherited HTML language selects an installed
dictionary: its own `lang` can override an ancestor or the document's language,
and a subframe uses its own document. Changing a language attribute affects
subsequent text checks. Tabs and views do not change one another's dictionary.

An explicit dictionary chosen in Preferences takes precedence over page
languages. `SUMMIT_SPELLING_LANGUAGES` also keeps its explicit configuration.
Missing, empty or invalid language tags, unavailable dictionaries, and a
selected dictionary that cannot be loaded use the configured system fallback.
Dictionary names are matched without regard to case, accept hyphens or
underscores, and try progressively less specific language tags. A configured
regional preference wins when several installed dictionaries match a broader
language tag. The existing requirement for UTF-8 Hunspell dictionaries remains.

The same choice applies to typed underlines and context-menu suggestions.
`spellcheck="false"` and password fields remain excluded. Ignored words are
shared across dictionaries in the browser process; learned words are added to
all loaded dictionaries and the profile's word list, and survive restarting.
As the Preferences hint says, a changed preference applies from the next word
checked; it does not immediately recheck every existing word in every page.

## Implementation

`TextCheckerClient::checkSpellingOfStringForNode` carries the checked node
through WebCore's word, paragraph, editor and accessibility paths. Other ports
default to their existing text-checking method. On Haiku, `WebEditorClient`
reads WebCore's effective inherited language and includes it in each checking
or suggestion request to the UI process. There is no active-tab dictionary
switch or language state shared between renderer requests.

`TextCheckerHaiku` keeps the fallback dictionary and at most eight extra page
dictionaries, keyed by installed dictionary names. Arbitrary page tags cannot
become file paths or create an unbounded handle cache. The installed-name
catalog is refreshed when spelling configuration changes or the browser
restarts. Dictionary loading, ignored words and personal-word persistence
retain the existing UI-thread Hunspell implementation.

The four-argument `BWebKitSetSpellChecking` symbol remains available and enables
automatic page-language selection. A fifth `usePageLanguage` argument lets an
embedder keep an explicit dictionary instead. Summit passes false for an
explicit Preferences or environment choice, and true for automatic mode.
`BWebKitSpellCheckingLanguage()` reports the fallback dictionary.

## Validation

On the preceding bookmarklet engine (`8cd77d2`), the isolated native probe
accepts an English control word, then fails when a German document types a word
present in its German dictionary. The baseline exits normally after 35 checks,
with 82 frozen hashes unchanged, no helpers and a clean complete crash-log
interval.

The candidate passes:

- **30 host lookup checks**, covering case, separators, regional and script
  tags, fallback order, missing dictionaries and invalid input.
- **403 native checks** with real key/mouse events and controlled English,
  German and French dictionaries. These cover document, field, ancestor and
  subframe language; independent views; textarea, input and contenteditable;
  changing language attributes; missing/invalid tags; explicit overrides;
  suggestions; disabled/password fields; ignored and learned words.
- **47 checks in a fresh process**, confirming learned words persist across
  English and German dictionaries, while ignored words do not persist.
- **Three runs of 19 native full-browser assertions** invoking the actual
  Language menu item, observing its selected state and saved profile, and
  typing into the page. **15 browser/fixture checks** verify the result and
  runtime lifecycle. Automatic → explicit English → automatic produces
  **0 → 886 → 0 red underline pixels** for the controlled German word.

The native probe also checks the actual presented pixels before opening a
context menu: correct German, French-field and German-subframe words have no
red markers; their typos have **411, 532 and 411** red pixels respectively.
Learned words have none. Each capture waits for a unique painted pixel to
exclude stale frames. WebCore's inspector snapshots omit document markers;
the assertions use `CopyPresentedFrame()` instead. Opening a context menu can
erase the selected word's marker on this port, so the typing capture precedes
that action. The German document and subframe underlines and full-browser
English-override output were visually inspected.

All four candidate engine images were verified as loaded. Each native phase
retains **82 frozen hashes**; the full browser retains **666**. All final runs
exit normally, leave no helpers, and have clean complete crash-log intervals.
The engine build completed 402 steps, with all 1,499 patched source files
matching its frozen input manifest. The complete application builds and links
from 323 frozen input files. These are isolated airOS x86_64 tests on
hrev60207+1733 using software rendering; stock-Haiku, ARM64 and published-package
runtime acceptance were not performed. The Preferences menu action and saved
choice were checked; the dropdown's visual layout was not inspected.
After the runs, a scan of all 42 native teams found no owned test images, and
the temporary RAM filesystem was unmounted and its mount directory removed.

Full results and provenance:
[`page-language-spelling-native-2026-10-22.json`](page-language-spelling-native-2026-10-22.json).

## Repeating the checks

With the patched engine source prepared, compile `tests/SpellingLanguageTests.cpp`
with C++23 and `-I.cache/WebKit/Source/WebKit/UIProcess/haiku`, then run it.

Build `tests/PageSpellingTests.cpp` against the candidate engine's headers and
`libWebKit`, with C++23, `-DSUMMIT_PAGE_SPELLING=1`, `-Ivendor`, and the `be` and
`network` libraries. It includes the helpers from `ModernPDFTests.cpp`; do not
compile that helper source a second time. On Haiku, run:

```sh
python3 tools/test-page-spelling.py \
  --probe /path/to/PageSpellingTests \
  --bundle /path/to/lib/summit-webkit \
  --images-helper /path/to/NativeLoadedImages \
  --output /path/to/new-result-directory
```

The runner creates private dictionaries, serves loopback fixtures and runs two
fresh processes. `tests/fixtures/page-language-spelling.html` is the full-browser
fixture. `NativeSpellingPreferencesTests` takes the owned browser team, exact
executable, Language menu label, word to type, profile JSON path and expected
saved language. It invokes the native menu through Haiku's menu scripting and
delivers native key events; it never edits the profile directly.
