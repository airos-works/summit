# Dark mode

Websites see the system's appearance: with air/OS's **Dark mode** on
(Screen preferences, or `screenmode --dark-mode on`), Summit reports
`prefers-color-scheme: dark`, so sites that have a dark version serve it, and
form controls, scroll bars and default page colors are drawn dark. Switching
it takes effect at once in open tabs, without reloading.

## How it works

- Engine, UI process (`Source/WebKit/UIProcess/haiku/WebView.cpp`):
  `WebView::effectiveAppearanceIsDark()` (the `PageClient` hook WebKit asks
  for every page) is true when air/OS's `ui_dark_mode()` is. The function is
  looked up at run time (`dlsym`), so the same engine runs on stock Haiku,
  which has no dark mode switch; there, and with the switch off, dark document
  colors chosen in Appearance count, as in the legacy `BWebView`.
- `BWebKitView` forwards `B_COLORS_UPDATED` (which app_server sends when the
  appearance is switched) to the page as `viewAppearanceHaiku`, and the page
  calls `WebPageProxy::effectiveAppearanceDidChange()`, which tells every web
  process (`WebPage::SetUseColorAppearance`).
- WebCore's `RenderThemeHaiku` already picks the dark system colors for pages
  in the dark appearance.

## Testing

On air/OS: open a page with `@media (prefers-color-scheme: dark)` rules (for
example `matchMedia('(prefers-color-scheme: dark)').matches` in the Web
Inspector console), then switch Dark mode in Screen: the page follows.
