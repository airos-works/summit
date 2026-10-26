# Form controls in pages (#114)

## What the user sees

Buttons, check boxes, radio buttons, text fields (inputs, text areas and
search fields) and `<select>` menus that a page leaves unstyled look like
the same controls in a Haiku application: drawn by `BControlLook` in the
system's colours, in its dark variant for a page that asks for dark colours
(`color-scheme: dark`) on a light system and the other way round, at 100 %
and 200 %. Before, under Skia, they were drawn in GNOME's Adwaita style.
Range sliders (`BSlider`'s bar, filled up to the thumb, and its thumb) and
determinate progress bars (`BStatusBar`'s) are Haiku's too. An indeterminate
progress bar keeps Adwaita's animation; `<meter>` and the number field's
arrows are still Adwaita's.

Scroll bars, of the page and of scrolling boxes in it, are Haiku's classic
ones, as `BScrollBar` draws them: a bar as wide as Haiku's vertical scroll bar
(14 pixels at the default font size), which takes its room as in Haiku's
applications, with an arrow button at each end and the thumb between them,
and the panel colour in the corner between two bars. Before, they were
Adwaita's overlay bars, drawn over the content. A page that sets
`scrollbar-color` keeps Adwaita's drawing in those colours (in the same
room).

## How it works

- Under Skia the engine uses WebKit's Adwaita theme (`USE_THEME_ADWAITA`,
  `Source/cmake/OptionsHaiku.cmake`): its form controls are the Adwaita
  `PlatformControl`s (`platform/graphics/adwaita`). `BControlLook` needs a
  `BView` to draw into, which a Skia `GraphicsContext` does not have.
- `platform/graphics/haiku/ControlLookHaiku.cpp` draws a control with
  `BControlLook` (`DrawButtonFrame` and `DrawButtonBackground`,
  `DrawCheckBox`, `DrawRadioButton`, `DrawTextControlBorder` with the
  document colour inside as `BTextControl` has, `DrawMenuFieldFrame` and
  `DrawMenuFieldBackground` with the pop-up arrow, `DrawSliderBar` in a
  6-pixel bar across the middle of the slider's box with the
  `B_CONTROL_HIGHLIGHT_COLOR` fill up to the thumb, `DrawSliderThumb`,
  `DrawStatusBar` with `B_STATUS_BAR_COLOR`) into an offscreen
  `BBitmap` that accepts views (the web process has a `BApplication`), and
  the result is drawn into the page as a Skia image. `ToggleButtonAdwaita`,
  `ButtonControlAdwaita`, `MenuListAdwaita` and `TextFieldAdwaita` call it
  first and draw Adwaita's only when it cannot.
- Each control is drawn twice, over black and over white: the difference
  gives each pixel's coverage, whatever app_server blends at the control's
  rounded and anti-aliased edges, and the colour over black is the
  premultiplied colour Skia wants.
- Drawn in device pixels, as Haiku draws its controls larger for a larger
  font: frames stay one pixel wide at 200 %. (Scaling the view instead broke
  `BControlLook`'s check marks.) A check box or radio button is square, in
  the middle of its box.
- States: disabled (and read-only text fields), hovered, focused (the page's
  focused control, as Adwaita), pressed buttons and menus, checked and
  indeterminate (`B_PARTIALLY_ACTIVATED`) check boxes and radio buttons, the
  default button.
- The images are cached by part, state flags, size, scale, light or dark,
  the value (a slider's position or the progress, to a thousandth) and the
  system colours they used (a change in Appearance draws new ones); at
  512 looks the cache starts again. Controls larger than 4096 × 1024 device
  pixels are left to Adwaita.
- Scroll bars: `ScrollbarThemeAdwaita` stays the theme (the scrolling code
  takes it to be Adwaita's), with Haiku's geometry when
  `usesHaikuScrollbars()`: not overlay, `haikuScrollbarThickness()` wide
  (`BControlLook::GetScrollBarWidth`), arrow buttons as long as the bar is
  wide (half the bar when too short for two), the track between them. The
  page's own scroll bars are drawn on the scrolling thread
  (`USE(COORDINATED_GRAPHICS_ASYNC_SCROLLBAR)`, `ScrollerCoordinated`), which
  now computes the thumb in that track too; both it and the main thread draw
  through `AdwaitaScrollbarPainter::paint`, which draws Haiku's
  (`paintHaikuScrollbar`): `DrawScrollBarBorder`, two `DrawScrollBarButton`s
  (pressed when held) and `DrawScrollBarBackground` as one image per bar
  size, and `DrawScrollBarThumb` as another, drawn where the thumb is, so
  scrolling only moves an image. The painter's cache and offscreen bitmap
  are behind a lock for the scrolling thread.
- `SUMMIT_HAIKU_CONTROLS=0` (read when a web process starts) draws
  Adwaita's again, scroll bars included; `SUMMIT_HAIKU_SCROLLBARS=0` brings
  back only Adwaita's overlay scroll bars.

## Checked

On the X399 at 200 % with `controls.html` (unstyled buttons, a disabled
one, submit and reset; check boxes unchecked, checked, indeterminate and
disabled; radio buttons; text, placeholder, read-only and search fields; two
selects; a text area; two range sliders, one disabled; a determinate and an
indeterminate progress bar): all drawn as Haiku's controls, in the light colours
and, with `color-scheme: dark`, in the dark ones; a click checks a check box,
which is drawn checked. A focused text field showed no focus border on the
X399 because its window was not active there, with Adwaita's controls too
(WebKit counts a control as focused only in an active window).

Scroll bars, on the X399 at 200 % with `scrollbars.html` (a scrolling box
with both bars, one with only a horizontal bar, a tall page): all drawn as
Haiku's, 14 CSS pixels wide (the page's content 14 pixels narrower); a click
on the box's down arrow scrolled it by 40 pixels; wheel scrolling the page to
500 moved its thumb; Wikipedia's article page drew its scroll bar the same.

## Not done

- `<meter>`, an indeterminate or vertical progress bar, the number field's
  arrows.
- The `<select>` arrow is small at 200 %: `BControlLook` sizes the pop-up
  indicator by the font.
