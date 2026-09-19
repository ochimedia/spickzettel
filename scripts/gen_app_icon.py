#!/usr/bin/env python3
"""Rasterizes assets/app_icon/spickzettel.svg into assets/app_icon/spickzettel.ico
(a standard multi-resolution Windows icon), which src/app_main/app_icon.rc then
compiles into the .exe as its window/taskbar/tray icon.

Not part of the CMake build - like scripts/gen_icons.py, this is a one-time
dev-machine tool, run by hand whenever assets/app_icon/spickzettel.svg
changes, not a project dependency. Needs cairosvg (`pip install cairosvg`)
and Pillow (`pip install Pillow`).
"""
import os
import struct

import cairosvg

HERE = os.path.dirname(os.path.abspath(__file__))
SVG_PATH = os.path.join(HERE, "..", "assets", "app_icon", "spickzettel.svg")
ICO_PATH = os.path.join(HERE, "..", "assets", "app_icon", "spickzettel.ico")

# Standard Windows icon sizes: 16/32 for taskbar/title-bar/Alt-Tab, 48 for
# desktop/Explorer icons, 256 for jumbo/high-DPI views. Rendered directly
# from the vector source at each size (via cairosvg) rather than downscaled
# from one large raster, so small sizes stay crisp instead of muddy.
SIZES = [16, 32, 48, 256]


def main():
    # Assembled by hand (ICONDIR + one ICONDIRENTRY per size + the raw PNG
    # bytes back to back) rather than through Pillow's ICO writer: Pillow's
    # `sizes=`/`append_images=` combination silently collapses to a single
    # entry (it resizes the base image internally instead of packing the
    # already-rendered per-size PNGs), which defeats the whole point of
    # rendering each size straight from the vector source. The modern ICO
    # format (Vista+) allows each entry's image data to be a plain PNG file
    # rather than the legacy uncompressed BMP+AND-mask pair, which is all
    # this needs.
    pngs = [cairosvg.svg2png(url=SVG_PATH, output_width=size, output_height=size) for size in SIZES]

    header = struct.pack("<HHH", 0, 1, len(SIZES))  # ICONDIR: reserved, type=icon, count
    entries = b""
    data = b""
    offset = len(header) + 16 * len(SIZES)
    for size, png in zip(SIZES, pngs):
        wh = size if size < 256 else 0  # 0 means 256 in the ICONDIRENTRY's 1-byte fields
        entries += struct.pack("<BBBBHHII", wh, wh, 0, 0, 1, 32, len(png), offset)
        data += png
        offset += len(png)

    with open(ICO_PATH, "wb") as f:
        f.write(header + entries + data)

    print(f"Wrote {ICO_PATH} ({', '.join(f'{s}x{s}' for s in SIZES)})")


if __name__ == "__main__":
    main()
