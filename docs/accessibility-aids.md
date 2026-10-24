# Accessibility aids (#118)

## Caret browsing

View › Caret Browsing, or F7 (as in Firefox and Chrome), puts a movable caret
in page text: the arrow keys move it, Shift-arrows select, and links and form
fields under it can be activated. The setting is shared by every window and
lasts for the session; it is not saved.

Verified on the X399 (23 Oct, cext build): on example.com, Shift-Right four
times after a click selected nothing with caret browsing off, and "Tamp" after
F7.

The browser sends `caretBrowsing` to each tab through
`BWebKitView::SetPageSettings()`, which sets WebKit's `CaretBrowsingEnabled`
preference for that page. New tabs get the current state when they are made.

## Minimum font size (engine only so far)

`SetPageSettings()` also accepts `minimumFontSize` (CSS pixels, 0 to 72; 0 is
none), which sets WebKit's `MinimumFontSize`. Preferences does not offer it
yet: it needs a profile setting, and `src/core/Profile.*` has uncommitted
download-history work (#110) in the main checkout.

## Reduce Motion and Increase Contrast

View › Reduce Motion makes pages match `prefers-reduced-motion: reduce`, and
View › Increase Contrast `prefers-contrast: more`, so sites that offer calmer
or higher-contrast styles use them. Like caret browsing they are shared by
every window, last for the session and reach new tabs when they are made;
saving them waits for the same profile work as the minimum font size.
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

## Still to do

Forced colours (`forced-colors: active`, which WebKit does not render), and
an accessibility tree turned on only when asked for (developer tools, MCP).
