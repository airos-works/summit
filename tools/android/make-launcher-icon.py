#!/usr/bin/env python3
"""Generates the Android launcher icon from docs/summit-icon.png.

Writes an adaptive icon (white background, the mountain as the foreground,
and a monochrome layer for Android 13's themed icons) into
android/app/src/main/res. Run it after the artwork changes; the output is
checked in.
"""
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parents[2]
RES = REPO / "android" / "app" / "src" / "main" / "res"
DENSITIES = {"mdpi": 1, "hdpi": 1.5, "xhdpi": 2, "xxhdpi": 3, "xxxhdpi": 4}

art = Image.open(REPO / "docs" / "summit-icon.png").convert("RGBA")
art = art.crop(art.getbbox())

for name, scale in DENSITIES.items():
    canvas = round(108 * scale)
    # Keep the drawing inside the 66 dp circle every launcher mask leaves whole.
    box = round(62 * scale)
    w, h = art.size
    factor = box / max(w, h)
    size = (max(1, round(w * factor)), max(1, round(h * factor)))
    fg = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    fg.alpha_composite(art.resize(size, Image.LANCZOS), ((canvas - size[0]) // 2, (canvas - size[1]) // 2))
    out = RES / f"mipmap-{name}"
    out.mkdir(parents=True, exist_ok=True)
    fg.save(out / "ic_launcher_foreground.png", optimize=True)
    mono = Image.new("RGBA", fg.size, (0, 0, 0, 0))
    mono.putalpha(fg.getchannel("A"))
    mono.save(out / "ic_launcher_monochrome.png", optimize=True)

(RES / "mipmap-anydpi-v26").mkdir(parents=True, exist_ok=True)
(RES / "mipmap-anydpi-v26" / "ic_launcher.xml").write_text("""<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
  <background android:drawable="@android:color/white" />
  <foreground android:drawable="@mipmap/ic_launcher_foreground" />
  <monochrome android:drawable="@mipmap/ic_launcher_monochrome" />
</adaptive-icon>
""")
print("wrote", RES)
