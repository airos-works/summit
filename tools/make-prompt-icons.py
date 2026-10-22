#!/usr/bin/env python3
"""Generate resources/prompts/microphone.hvif, the microphone question's icon.

The camera question uses CodyCam's webcam from the Haiku tree (see
resources/prompts/README.md). Haiku has no plain microphone icon (Sound
Recorder shows a tape recorder), so this draws one in the same style: shaded
metal, dark outlines, on Haiku's 64x64 icon grid. The HVIF writer is the one
of tools/make-summit-icon.py.

Run `python3 tools/make-prompt-icons.py` after editing; the result is checked
in and resources/Summit.rdef imports it.
"""
import importlib.util
import os

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('summit_icon', os.path.join(HERE, 'make-summit-icon.py'))
hvif = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hvif)

KAPPA = hvif.KAPPA


def rounded_rect(x0, y0, x1, y1, r):
    """A closed rounded rectangle; corners are quarter ellipses of radius r."""
    k = r * KAPPA
    return [
        (x0 + r, y0, x0 + r - k, y0, x0 + r, y0),
        (x1 - r, y0, x1 - r, y0, x1 - r + k, y0),
        (x1, y0 + r, x1, y0 + r - k, x1, y0 + r),
        (x1, y1 - r, x1, y1 - r, x1, y1 - r + k),
        (x1 - r, y1, x1 - r + k, y1, x1 - r, y1),
        (x0 + r, y1, x0 + r, y1, x0 + r - k, y1),
        (x0, y1 - r, x0, y1 - r + k, x0, y1 - r),
        (x0, y0 + r, x0, y0 + r, x0, y0 + r - k),
    ]


def holder(cx, top, bottom, outer, inner):
    """The U-shaped yoke: a thick lower half ellipse open at the top."""
    ry_out, ry_in = bottom - top, bottom - top - (outer - inner)
    ko, ki = KAPPA * outer, KAPPA * inner
    kyo, kyi = KAPPA * ry_out, KAPPA * ry_in
    return [
        # outer edge, left top -> bottom -> right top
        (cx - outer, top, cx - outer, top, cx - outer, top + kyo),
        (cx, top + ry_out, cx - ko, top + ry_out, cx + ko, top + ry_out),
        (cx + outer, top, cx + outer, top + kyo, cx + outer, top),
        # square end, then the inner edge back
        (cx + inner, top, cx + inner, top, cx + inner, top + kyi),
        (cx, top + ry_in, cx + ki, top + ry_in, cx - ki, top + ry_in),
        (cx - inner, top, cx - inner, top + kyi, cx - inner, top),
    ]


def vertical(x0, x1):
    """A gradient across x0..x1 (Haiku's linear gradients span -64..64)."""
    width = (x1 - x0) / 128.0
    return (width, 0.0, 0.0, width, (x0 + x1) / 2.0, 32.0)


def build():
    icon = hvif.Icon()
    outline = icon.style(hvif.Solid(40, 40, 46))
    metal = icon.style(hvif.Gradient(hvif.GRADIENT_LINEAR,
        [(0, (150, 152, 162)), (70, (236, 238, 244)), (150, (196, 199, 210)), (255, (104, 107, 120))],
        vertical(20, 44)))
    grille = icon.style(hvif.Solid(70, 72, 84, 210))
    shine = icon.style(hvif.Solid(255, 255, 255, 150))
    dark_metal = icon.style(hvif.Gradient(hvif.GRADIENT_LINEAR,
        [(0, (118, 120, 132)), (90, (186, 188, 198)), (255, (72, 74, 86))],
        vertical(14, 50)))

    # Base and stem, then the yoke around the capsule's lower half.
    icon.draw(outline, rounded_rect(15, 53, 49, 61, 4))
    icon.draw(dark_metal, rounded_rect(17, 55, 47, 59, 2))
    icon.draw(outline, [(28.5, 43), (35.5, 43), (35.5, 55), (28.5, 55)])
    icon.draw(dark_metal, [(30.5, 43), (33.5, 43), (33.5, 55), (30.5, 55)])
    icon.draw(outline, holder(32, 24, 46, 18, 11))
    icon.draw(dark_metal, holder(32, 25, 44.2, 16.2, 12.8))

    # The capsule, its grille and a highlight.
    icon.draw(outline, rounded_rect(19, 2, 45, 40, 13))
    icon.draw(metal, rounded_rect(21, 4, 43, 38, 11))
    for y in (9, 13.5, 18, 22.5):
        icon.draw(grille, [(23.5, y), (40.5, y), (40.5, y + 1.6), (23.5, y + 1.6)])
    icon.draw(grille, [(21, 27.5), (43, 27.5), (43, 29.5), (21, 29.5)])
    icon.draw(shine, rounded_rect(25, 6, 28.5, 25, 1.75))
    return icon


def main():
    data = build().to_bytes()
    path = os.path.join(HERE, '..', 'resources', 'prompts', 'microphone.hvif')
    with open(path, 'wb') as out:
        out.write(data)
    print('wrote %s (%d bytes)' % (os.path.normpath(path), len(data)))


if __name__ == '__main__':
    main()
