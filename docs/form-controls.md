# Form controls in pages (#114)

## What the user sees

Buttons, check boxes, radio buttons, text fields (inputs, text areas and
search fields) and `<select>` menus that a page leaves unstyled look like
the same controls in a Haiku application: drawn by `BControlLook` in the
system's colours, in its dark variant for a page that asks for dark colours
(`color-scheme: dark`) on a light system and the other way round, at 100 %
and 200 %. Before, under Skia, they were drawn in GNOME's Adwaita style.
Sliders, progress bars, the number field's arrows and page scroll bars are
still Adwaita's.

## How it works

- Under Skia the engine uses WebKit's Adwaita theme (`USE_THEME_ADWAITA`,
  `Source/cmake/OptionsHaiku.cmake`): its form controls are the Adwaita
  `PlatformControl`s (`platform/graphics/adwaita`). `BControlLook` needs a
  `BView` to draw into, which a Skia `GraphicsContext` does not have.
- `platform/graphics/haiku/ControlLookHaiku.cpp` draws a control with
  `BControlLook` (`DrawButtonFrame` and `DrawButtonBackground`,
  `DrawCheckBox`, `DrawRadioButton`, `DrawTextControlBorder` with the
  document colour inside as `BTextControl` has, `DrawMenuFieldFrame` and
  `DrawMenuFieldBackground` with the pop-up arrow) into an offscreen
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
- The images are cached by part, state flags, size, scale, light or dark and
  the system colours they used (a change in Appearance draws new ones); at
  512 looks the cache starts again. Controls larger than 4096 × 1024 device
  pixels are left to Adwaita.
- `SUMMIT_HAIKU_CONTROLS=0` (read when a web process starts) draws
  Adwaita's again.

## Checked

On the X399 at 200 % with `controls.html` (unstyled buttons, a disabled
one, submit and reset; check boxes unchecked, checked, indeterminate and
disabled; radio buttons; text, placeholder, read-only and search fields; two
selects; a text area): all drawn as Haiku's controls, in the light colours
and, with `color-scheme: dark`, in the dark ones; a click checks a check box,
which is drawn checked. A focused text field showed no focus border on the
X399 because its window was not active there, with Adwaita's controls too
(WebKit counts a control as focused only in an active window).

## Not done

- Sliders, progress bars, the number field's arrows and scroll bars.
- The `<select>` arrow is small at 200 %: `BControlLook` sizes the pop-up
  indicator by the font.
