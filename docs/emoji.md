# Bundled emoji

Summit carries Noto Color Emoji in the scalable COLRv1 format and Noto Emoji
for monochrome text presentation. Pages can render emoji without installing a
font package. Faces, skin tones, flags, keycaps and joined sequences use the
same fonts in DOM text and Canvas. The text variation selector (U+FE0E) can
select the monochrome font; U+FE0F requests emoji presentation.

The unmodified fonts and their licenses are embedded in `Summit.rdef`, so both
native packages and standalone builds carry them. Before initializing WebKit,
Summit verifies copies in the profile's `Fonts` directory against its embedded
resources and atomically repairs missing or corrupted files. It writes a local
fontconfig configuration that includes the system or inherited configuration
and adds this font directory. WebKit's helper processes inherit it. System font
files and settings are unchanged, and existing user fontconfig aliases remain
available. Browser-launched web apps retain the original configuration through
`SUMMIT_BASE_FONTCONFIG_FILE` rather than including another profile's generated
file.

Using scalable color outlines also avoids relying on the system's bitmap emoji
font discovery. The X399 workstation had `NotoColorEmoji.ttf` installed, but
fontconfig did not list it or select it for its family name. The bundled COLRv1
font is discovered by the private configuration.

Sources and checksums are pinned in
[`resources/fonts/sources.json`](../resources/fonts/sources.json). Both fonts
use the SIL Open Font License 1.1; their complete licenses also accompany the
package documentation in [emoji-font-licenses.txt](emoji-font-licenses.txt).
Upstream projects: [Noto Emoji](https://github.com/googlefonts/noto-emoji) and
[Google Fonts' monochrome Noto Emoji](https://github.com/google/fonts/tree/main/ofl/notoemoji).
The configuration follows [fontconfig's documented directory, include and alias elements](https://fontconfig.pages.freedesktop.org/fontconfig/fontconfig-user.html).

## Verification

`tools/test-emoji.py` runs the real browser with an isolated profile. Its
`tests/fixtures/emoji.html` measures rendered Canvas pixels and glyph advances,
so a missing glyph box, monochrome fallback for a colored emoji, or a broken
joined sequence cannot pass by merely claiming that a font loaded. Noto's
current family symbol is intentionally grayscale and is checked accordingly.

```sh
python3 tools/test-emoji.py \
  --browser /path/to/Summit --control /path/to/summitctl
```

The control helper is built from `tools/bench/summitctl.cpp`. Use `--shell`,
`--host` and `--directory` for a workstation rather than the default Haiku VM,
with the engine bundle's usual graphics and library environment.

The test checks 12 samples on initial launch, in a private window, after
repairing a deliberately corrupted cached font, and with an inherited
fontconfig configuration. It verifies all four embedded font/license hashes,
XML escaping in profile paths, preservation of custom font aliases, normal
browser shutdown and no fresh native debugger events. It also saves a native
PNG screenshot in the test profile.

On 2026-10-21, these checks pass on stock Haiku R1/beta6 at 100% display scale.
