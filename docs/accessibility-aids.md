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

## Still to do

Forced colours / high contrast, `prefers-reduced-motion` from a setting, and an
accessibility tree turned on only when asked for (developer tools, MCP).
