# Issues #51 to #55 (15 October 2026)

Five issues filed on 15 October, worked on the X399 (air/OS x86_64, two
displays at 200%) and the laptop (air/OS x86_64, the user's machine). The
fixes are in c23f184 and the CI build of it (`summit 0.1.0~git20261015.0702`,
`summit_webkit 1.10.1~git20261015.0702`) is installed on the X399 as
`summit/build-modern-browser/bundle-ci0702` (the Desktop launcher
`Summit-current.sh` and `summit/Summit-installed` point at it; the previous
launcher is in `summit/launcher-backups/`; assembled by
`claude-issues/x399-install-ci.sh` as on 13 October) and on the laptop as
packages (`pkgman install`, the previous state backed up by the package
daemon). On the X399 YouTube's full-screen button fills the left display
exactly (window frame 0,0-1919,1079, `document.fullscreenElement` set) and
Escape restores the window; on the laptop the caret blinks and the generic
families resolve with the installed build.

## #55 YouTube: "Your browser doesn't support full screen"

The browser has handled page fullscreen since issue #43 (7 October), but
only in engines built with `ENABLE_FULLSCREEN_API`. The CI engines are
configured from `engine/arm64/rpi4-gl/init-cache-rtc.cmake`, which had the
option off, so no CI build had `Element.requestFullscreen` at all and
YouTube's player reported the browser as unable. The option is on now
(1ad0f01); every CI build since carries the API.

A full-screen page fills the display the window is on, like airTime's
full-screen video (`get_display_frame(frame, false, …)`), whatever the
desktop's zoom setting says about spanning displays; the earlier code
followed the zoom setting (`forZoom` true), which on a desktop set to zoom
across displays stretched a video over both monitors.

## #54 The start page

`resources/start.html` carries the air/OS mark (the site's `favicon.svg`,
inline) and wordmark, the site's palette with a dark scheme, links to
airos.works and its Downloads, New hardware, Screenshots, Source code and
Changelog pages, Summit on GitHub and Haiku, and four favorites. It is one
file with no requests, as before.

## #52 Missing fonts

What the user saw on the laptop (GitHub's comment box in a serif font, and
form controls elsewhere) had two causes, one in the OS image and one in the
engine.

**The laptop's fontconfig has no aliases.** The fontconfig 2.18.2-2 package
on the laptop's image links `/boot/system/settings/fonts/conf.d/*.conf` to
`../../../../../.self/data/fontconfig/conf.avail/…`, a path that resolves
only inside the package's own directory, not through `/boot/system/settings`.
`fc-conflist` shows every `conf.avail` file as not loaded, and
`fc-match monospace` answers Noto Sans. Without `60-latin.conf`'s preference
lists fontconfig cannot resolve the generic families, and Skia's fontconfig
font manager (which accepts only strong family matches) returns nothing for
`monospace`, `serif` and `sans-serif`. GitHub's comment box names
`ui-monospace, SFMono-Regular, SF Mono, Menlo, Consolas, Liberation Mono,
monospace`; every family failed, and WebKit fell to the last-resort font,
which was `serif`: Noto Serif. The X399 (fontconfig 2.16, real files in
`conf.d`) was fine. This is an image/package bug for the OS side; it is
reported in the issue.

**The engine resolves the families itself now** (d0ed805), so a broken or
missing fontconfig configuration costs nothing visible:

- The generic families (`serif`, `sans-serif`, `monospace`, `cursive`,
  `fantasy`, `math`, `emoji`) are asked of fontconfig first; when it has no
  answer, the engine tries the system's plain or fixed font (`be_plain_font`,
  `be_fixed_font`), then the Noto, DejaVu and Liberation families in
  fontconfig's own order of preference.
- The CSS system families (`system-ui`, `ui-sans-serif`, `ui-rounded`,
  `ui-monospace`, `ui-serif`) name the system's fonts directly; fontconfig
  has only weak aliases for them, which Skia ignores, so they used to fail
  on every installation. `ui-monospace` is the system's fixed font.
- The last-resort font on Haiku is `sans-serif`, not `serif`.
- The system font shorthands (`caption`, `menu`, `-webkit-small-control`…)
  were not implemented and answered with the standard family, Times, so
  every form control without a font of its own (the user agent stylesheet
  sets `-webkit-small-control`) was drawn in a serif font. They name the
  system's plain font at its size now (`SystemFontDatabaseHaiku.cpp`).

Checked on the X399 with `tests` pages: `ui-monospace` resolves to Noto Sans
Mono, `system-ui` to Noto Sans, an `<input>` without a font is Noto Sans.
GitHub's home page itself was never affected: its Mona Sans web fonts load
(`document.fonts` reports them loaded).

**What the images ship.** The laptop's image has the Noto Sans, Serif and
Mono families and nothing for emoji or CJK; the X399 has Noto Emoji, Noto
Sans Symbols and the CJK fonts too. Summit packages no fonts: the engine
takes the system's fonts through fontconfig, and a glyph no installed font
has stays a box. That is the image's choice (the `noto` packages).

## #53 The caret in GitHub's issue editor

Reproduced on the laptop, and it is the same fault as #52. With the
laptop's fontconfig unable to resolve any generic family, a field whose
font stack ends in `monospace` or `sans-serif` (GitHub's editor, and
anything without a font of its own) gets WebKit's last-resort font. On
Haiku that last resort was `serif`, which failed too, and Skia's fontconfig
manager answers a lookup without a family name with nothing (its pattern
has no family to match), so the font became `SkTypeface::MakeEmpty()`: a
face with no glyphs and zero metrics. The text still appeared, drawn by the
per-character fallback (which found Noto Serif), but the caret's height
comes from the primary font's metrics, and a zero-height caret is nothing
to see. `tools/bench/caretcap.cpp` (reads a screen region repeatedly and
counts caret-colored pixels) showed it: in a field set to Noto Sans Mono or
Noto Serif by name the caret blinked at 500 ms; in the same field set to
`monospace` it never appeared, before or after typing.

With the font resolution of d0ed805 the generic families resolve to real
faces on the laptop and the caret blinks in a textarea, a contenteditable
and an input. The same engine on the X399, where fontconfig works, blinked
in every case tried before the fix too: textareas, inputs and
contenteditables, inside `will-change` and `translateZ` layers, in an
`overflow: auto` scroller, in a scrolled document, after a mouse click
(over VNC) into an active window, an inactive window, and when the address
field had the focus before; GitHub's login page fields as well.

One trap found while testing: the Developer Tools open as a separate
window placed beside the browser window, and when that lands on top of the
page (the X399's right half, the laptop), clicks and screen reads meant for
the page hit the Developer Tools window.

## #51 1Password passkeys

The passkey path works as far as it can be driven without the vault's
password. On github.com/login with the user's 1Password profile copy:

- 1Password's hooks are installed in the page (`navigator.credentials.get`
  is its function, `isConditionalMediationAvailable()` answers `true` in
  about a millisecond after the first call).
- The page's conditional-mediation request at load opens 1Password's
  "Sign in with a passkey: select the 1Password icon in your browser's
  toolbar to unlock" prompt.
- "Continue with passkey" is intercepted by 1Password: GitHub shows
  "Waiting for input from browser interaction…" while 1Password waits for
  the unlock.

With the installed 0702 build the toolbar click opens the popup with the
unlock screen 40 ms after the click (`SUMMIT_EXTENSION_TIMING=1` traces it).
Clicking into the username field shows 1Password's inline icon with its
"press the 1Password icon in your browser's toolbar to unlock" hint: the
field route reaches the unlock step as well.

A failure that leaves no trace in Summit: the laptop profile installed
1Password without access to private windows (uBlock Origin and SponsorBlock
have it), so in a private window GitHub gets Summit's own answer, no
authenticator, and reports the passkey as failed.

A test-profile trap: a profile whose `profile.json` was zero-filled by a
hard power cycle makes Summit show "The saved profile could not be read"
(a modal alert that blocks every click into the browser window until it is
dismissed), and after that the extension toolbar buttons do nothing.

1Password's own design has a race worth knowing: its page script asks its
content script whether to handle a request over `window.postMessage` and
gives up after one second, falling back to the browser's own answer (no
authenticator, so `NotAllowedError`, which GitHub reports as a failed
passkey). A page that blocks its main thread for more than a second at that
moment loses the race in any browser.
