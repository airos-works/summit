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
  Return, fills the whole form; Escape closes the list. Typing in the field
  filters the list to the entries whose value for that field starts with
  what is typed (a card number without its spaces); it hides while nothing
  fits and comes back as the typing is deleted. The list takes no focus: the
  field keeps it.
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

- Payment forms split over several frames (each card field in its own
  frame) are filled one frame at a time.

## Form history (#149)

Safari's "Other forms", Chrome's and Firefox's remembered entries.

### What the user sees

- Going into a text field (text, search, e-mail, telephone, URL or number)
  that is not part of an address or card form lists, under it, what was
  typed into fields of the same name in forms sent before: up to ten values,
  those last used on this site first, then the most recently used. Typing
  filters them (values that start with what is typed; one the field already
  holds is left out), and the list hides while nothing fits. A click, or
  Down/Up and Return, fills that field only. Shift+Delete on the highlighted
  row forgets that value. An address field with no saved address that fits
  offers its form history instead.
- Never remembered: password fields and every field of a form with one (and
  formless fields on a page with a formless password field), fields or forms
  with `autocomplete=off`, card fields and `cc-*`, `one-time-code` and
  `*-password` tokens, fields whose name, label or placeholder speaks of a
  security code, PIN, password, passcode or similar, values longer than 200
  characters or of several lines, anything that looks like a card number
  (12-19 digits passing the Luhn check), values the user did not type (the
  page's own, or unchanged defaults), and anything in private windows
  (which still offer what was remembered, as they do saved addresses).
- Preferences › AutoFill › Form history: "Remember what I type in forms"
  (on by default; off stops both remembering and offering), how many values
  are remembered, and Clear Form History…. Clear History… (History menu, or
  Preferences › History and Data with a time range) also clears the form
  history (of that range), and so does Clear Cookies and Site Data… (of the
  range; removing chosen sites in the Site data list forgets the values last
  used on them). An extension's `browsingData.remove` with `formData`
  clears it too.

### How it works

- Keyed by field name across sites, as Chrome and Firefox do (the field's
  `name`, else its `id`, trimmed and in lower case): a search box called `q`
  or a field called `email` means the same on most sites, and the list is the
  browser's own window, which a page cannot read; only the value chosen
  reaches the page. The host each value was last used on is kept, to rank
  that site's values first and to forget a site's values with its data.
- `kAutoFillScript` (the same page script as AutoFill) marks the fields the
  user typed in (trusted `input` events) and, when a form is sent (submit, a
  click on a button, Return in a field), reports the eligible fields'
  changed values with the address or card. The browser window
  (`BrowserWindowAutoFill.cpp`) gives them, outside private windows, to
  `FormHistory` (`src/core/FormHistory.cpp`), which checks the values again.
- Focusing a field sends `kind: "history"` with the field's name (or the
  address kind with it); typing sends `filter` messages with the field's
  value and the window filters and updates the list in place (`afrw` to the
  list window). The script's choice answer is `{"historyValue": ...}`, which
  it sets into that field with `input` and `change` events.
- Stored in the profile directory as `form-history.json`
  (`{"version":1,"entries":[{field, value, host, first, last, uses}]}`), not
  in profile.json. At most 50 values per field and 5000 in all; the least
  recently used go first. Read on first use, written atomically on a thread
  of its own half a second after a change, and when Summit quits.
- The MCP `tabs` tool shows the open list as `autoFill` on the window:
  `kind` (address, card or history), the `rows` shown, `selected`, what was
  `typed`, and whether it `shown`.

### Checked

On the X399 (1 November 2026, a test page with a search form, a
comment form with a name, a lone city and an `autocomplete=off` note, a form
with `autocomplete=off`, a sign-in form, a payment-like form and an address
form), driven by MCP click, `type_text` and `press_key`: sent forms
remembered `q`, `author`, `city` and the gift message, but not the
`autocomplete=off` field, the field of the `autocomplete=off` form, the
sign-in form's user name or the card-number-like reference. After a reload
(and after a restart) focusing the search field listed both remembered
values; typing `k`, `itc`, `z` and Backspace showed 2, 1, none (hidden) and 1
again; Down and Return filled only that field (one untrusted `input` event).
Shift+Delete removed the highlighted value from the list and the file. The
lone city field offered its value; the note, nick and user fields offered
nothing. A private window offered the remembered values but sent forms
there added nothing. With two saved addresses, typing `Ja`, then `x`, then
Backspace in the first-name field showed Jane only, nothing, and Jane again,
and Return filled the address form. Clear Cookies and Site Data with a time
range removed only the values used in it, Clear History removed all (the
Preferences count followed), the checkbox off stopped remembering, and
Clear Form History… (after its alert) emptied the file.

### Not done

- Values are matched from their start only (Firefox also matches word
  starts inside values).
- Textareas are not remembered (neither browser does).
