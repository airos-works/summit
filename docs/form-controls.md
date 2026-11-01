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
progress bar keeps Adwaita's animation; `<meter>`, the number field's arrows
and page scroll bars are still Adwaita's.

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
- `SUMMIT_HAIKU_CONTROLS=0` (read when a web process starts) draws
  Adwaita's again.

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

## Not done

- Page scroll bars. With Adwaita, the engine draws a page's scroll bars on
  the scrolling thread (`USE(COORDINATED_GRAPHICS_ASYNC_SCROLLBAR)`,
  `ScrollerImpAdwaita`), and the scrolling code takes the scroll bar theme to
  be Adwaita's: Haiku's look there needs that painter to draw Haiku's parts
  (from a thread-safe cache), not just another theme.
- `<meter>`, an indeterminate or vertical progress bar, the number field's
  arrows.
- The `<select>` arrow is small at 200 %: `BControlLook` sizes the pop-up
  indicator by the font.

## AutoFill: contact info, addresses and payment cards (#149)

### What the user sees

- Going into a field Summit recognises (a click, Tab, or Down in a field
  that already has the focus) opens a list under it with the saved entries
  that have something for that form: addresses as the focused field's value
  with the address or name below, cards as "Visa •••• 4242" with the name and
  expiry. The last row opens Preferences › AutoFill. A click, or Down/Up and
  Return, fills the whole form; Escape closes the list, and typing in the
  field closes it too. The list takes no focus: the field keeps it.
- After a form with a new address or card is sent, the notice bar offers to
  save it ("Save this address for AutoFill?", "Save the card Visa •••• 4242
  for AutoFill?", or Update for a known card with a new expiry or name).
  Private windows fill from saved entries but never offer to save.
- Preferences › AutoFill: turn AutoFill off (new tabs follow), and list,
  add, edit and remove addresses and cards. Card numbers are shown masked;
  editing a card leaves its number as it was unless a new one is typed.
- Cards are offered only on secure pages (https, the computer itself, files);
  elsewhere the status line says so. The security code is never stored or
  filled.

### How it works

- `kAutoFillScript` (PageScripts.cpp), in every frame, recognises fields by
  their `autocomplete` token first (given-name, family-name, name,
  organization, street-address, address-line1/2/3, address-level1/2,
  postal-code, country, country-name, email, tel, cc-name, cc-number, cc-exp,
  cc-exp-month, cc-exp-year; cc-csc and sign-in tokens are left alone), then
  by the field's name, id, label, `aria-label` and placeholder in English and
  a few common languages. A form needs two kinds of recognised address field,
  or a card number, unless the focused field has a token; forms with a
  password field are the password manager's.
- The page only reports the kind of field and where it is (as the passkey
  field script does, in the page's coordinates through its frames). The
  browser window (`BrowserWindowAutoFill.cpp`) shows the native list and
  answers the script message's promise only when the user chooses: the page
  gets that one entry's values, never the list. A click that lands within
  half a second of the list appearing is ignored, so a page cannot open the
  list under a click it expects. The page then fills the form's visible
  fields that are empty (or that AutoFill filled) through the elements'
  own value setters, with `input` and `change` events, and picks list options
  by value or text (countries also by their codes through `Intl.DisplayNames`).
- Addresses (`AutoFillAddress`) are in the profile, `autoFillAddresses` in
  profile.json, as Firefox keeps its addresses: the list reads them on the
  window's thread without the key store's prompts. Cards are in the Haiku key
  store, keyring "Summit cards" (separate from the passwords' "Summit"): a
  generic key per card with the name, number and expiry in a flattened
  BMessage (`AutoFillStore.cpp`). Every key store call runs on a thread of
  its own, never a window's: the first ones ask the user to allow Summit.
- `SUMMIT_AUTOFILL_INSECURE_CARDS=1` offers and saves cards on plain http
  pages too, for tests.
- Tests can answer the save bar with `hey TEAM 'afba' of Window 0 with
  which=int32(0)` (1 is Not Now); the MCP `tabs` tool shows the bar's text as
  `noticeBar`.

### Checked

On the X399 (1 November 2026), with a page whose forms have `autocomplete`
tokens and one with only names, labels and placeholders (a textarea for the
street, state and country lists, month and year lists for the card, a CVV
field and a search field outside the forms): sent forms offered to save the
address and two cards, Save stored them (the key store asked once each to
enumerate, add and remove), and an MCP click in a field opened the list.
Down and Return (MCP `press_key`) or a real click on the row filled all 11
address fields and the card name, number and expiry (an `MM/YY` field, or
the two lists), with one `input` event per field and no key reaching the
page; CVV and the search field stayed empty. A sent address that was already
saved offered nothing; a private window filled but offered nothing. On http
without the switch the card list did not open. Preferences listed both kinds
(numbers masked), and editing and removing a card and an address worked.

### Not done

- The list is not filtered by what is typed (typing closes it).
- No previously typed values for other fields (Chrome's and Firefox's
  per-field history).
- Payment forms split over several frames (each card field in its own
  frame) are filled one frame at a time.
