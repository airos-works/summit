# Accessibility aids (#118)

## Caret browsing

View › Caret Browsing, or F7 (as in Firefox and Chrome), puts a movable caret
in page text: the arrow keys move it, Shift-arrows select, and links and form
fields under it can be activated. The setting is shared by every window and
saved in the profile (`caretBrowsing`), and Preferences › Browsing ›
Accessibility has it too.

Verified on the X399 (23 Oct, cext build): on example.com, Shift-Right four
times after a click selected nothing with caret browsing off, and "Tamp" after
F7.

The browser sends `caretBrowsing` to each tab through
`BWebKitView::SetPageSettings()`, which sets WebKit's `CaretBrowsingEnabled`
preference for that page. New tabs get the current state when they are made.

## Minimum font size

Preferences › Browsing › Accessibility › Smallest text size (None, or 9 to
24 pixels) keeps page text from being drawn smaller; it is saved in the
profile (`minimumFontSize`) and applies to every tab at once.
`SetPageSettings()` takes it as `minimumFontSize` (CSS pixels, 0 to 72; 0 is
none), which sets WebKit's `MinimumFontSize`.

Saved settings verified on the X399 (25 Oct, pf118 build): a smallest text
size of 14 sent as Preferences sends it raised a 6 px paragraph to 14 px at
once; View › Reduce Motion matched `prefers-reduced-motion: reduce`; after
quitting, `profile.json` held both, and on restart the page had them again.

## Reduce Motion and Increase Contrast

View › Reduce Motion makes pages match `prefers-reduced-motion: reduce`, and
View › Increase Contrast `prefers-contrast: more`, so sites that offer calmer
or higher-contrast styles use them. Like caret browsing they are shared by
every window, are saved in the profile (`reduceMotion`, `increaseContrast`)
and reach new tabs when they are made; Preferences › Browsing ›
Accessibility has them too, kept in step with the View menu.
Haiku has no system setting for either, so with them off pages see
`no-preference`.

`SetPageSettings()` takes `reducedMotion` and `increasedContrast`, which set
WebKit's `ForcedPrefersReducedMotionAccessibilityValue` and
`ForcedPrefersContrastAccessibilityValue` to On (off: System). Those were
WebCore-only settings (Internals and the inspector's overrides); Summit
moved them to `UnifiedWebPreferences.yaml` so a page's preferences carry
them, with `SettingsBase::accessibilitySettingsChanged` re-evaluating the
media queries when they change. The inspector's user-preference overrides
last until the page's preferences change again.

Test page: `tests/fixtures/prefers-media.html` shows what the media queries
match.

Verified on the X399 (25 Oct, fx106 build), toggled with the menu items'
messages: Reduce Motion turned the page's `prefers-reduced-motion` to
`reduce` live (its animation stopped) and back; Increase Contrast turned
`prefers-contrast` to `more` (black background) and back, and a tab opened
meanwhile matched `more` too.

## The accessibility tree, on demand

WebKit builds its accessibility tree only once something asks for it, and
then keeps it for every page of the web process, at a cost on each change.
On Haiku nothing asks except the developer tools: Elements › Accessibility
shows the selected element's role, name (label) and states
(`DOM.getAccessibilityPropertiesForNode`), asked for only while that tab is
shown, and the MCP `inspector` tool can send the same command. When the
inspector session that turned accessibility on ends (the tools close), the
engine turns it off again in that process (`InspectorDOMAgent`,
`PLATFORM(HAIKU)`): there is no assistive technology to keep it on for.

Verified on the X399 (25 Oct, ax118b build) with a test page: the tab
showed a button as role `button`, label "Mute", pressed; a checkbox as
`checkbox`, "Remember me", checked; an `h2` as `heading` level 2; a `<nav
aria-label>` as `navigation` "Primary". After the tools closed, renaming
the heading and removing and adding elements (what crashed the web
process before 19 Oct's fix) left it running.

## Override Page Colours

View › Override Page Colours (also in Preferences › Browsing ›
Accessibility, saved as `overrideColors`) draws every page in the
browser's high-contrast colours, as Firefox's "Override the colours
specified by the page" does: light text on black, links yellow and visited
ones lilac, a visible focus ring, no background pictures; images and video
are left alone. It is a user style sheet of `!important` rules, set with
the engine's new `BWebKitView::SetUserStyleSheet()` (one sheet for the
views sharing a configuration, replaced or removed as a whole).

## Still to do

The `forced-colors: active` media query: WebKit has no forced-colours
mode (it always answers `none`), so pages cannot tell that their colours
are overridden.
