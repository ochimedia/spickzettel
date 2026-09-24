#!/usr/bin/env python3
"""Packs assets/app_icon/spickzettel.png into assets/app_icon/spickzettel.ico
(a standard multi-resolution Windows icon), which src/platform/win32/resources/
app_icon.rc then compiles into the .exe as its window/taskbar/tray icon.

Not part of the CMake build - like scripts/gen_icons.py, this is a one-time
dev-machine tool, run by hand whenever assets/app_icon/spickzettel.png
changes, not a project dependency. Standard library only: the artwork is
already a raster, so there is nothing to rasterize and no reason to make a
contributor install cairosvg and Pillow to regenerate an icon.
"""
import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
PNG_PATH = os.path.join(HERE, "..", "assets", "app_icon", "spickzettel.png")
ICO_PATH = os.path.join(HERE, "..", "assets", "app_icon", "spickzettel.ico")

# 16 for the tray and title bar, 20 and 24 for the same two at 125% and 150%
# display scaling, 32 for Alt-Tab, 40/48/64 for Explorer's icon views, 128 and
# 256 for its jumbo view and high-DPI shell surfaces. Windows picks the entry
# nearest the size it wants and scales from there, so offering the awkward
# in-between sizes outright is what keeps it from squashing 32 into 20.
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def read_png(path):
    """Returns (width, height, RGBA bytes) for an 8-bit non-interlaced RGBA PNG."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != PNG_MAGIC:
        raise ValueError(f"{path} is not a PNG")

    width = height = None
    idat = bytearray()
    pos = 8
    while pos + 8 <= len(data):
        (length,) = struct.unpack(">I", data[pos : pos + 4])
        kind = data[pos + 4 : pos + 8]
        body = data[pos + 8 : pos + 8 + length]
        pos += 12 + length  # length + type + body + CRC
        if kind == b"IHDR":
            width, height, depth, color, _comp, _filt, interlace = struct.unpack(">IIBBBBB", body)
            # Only the one shape the icon artwork is actually saved in. A
            # palette or a 16-bit or interlaced PNG would need a decoder
            # several times this size, so refuse it loudly instead.
            if (depth, color, interlace) != (8, 6, 0):
                raise ValueError(
                    f"{path}: need an 8-bit non-interlaced RGBA PNG, got depth={depth} "
                    f"color type={color} interlace={interlace}"
                )
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break

    if width is None:
        raise ValueError(f"{path}: no IHDR")
    return width, height, _unfilter(zlib.decompress(bytes(idat)), width, height)


def _unfilter(raw, width, height, bpp=4):
    """Reverses the per-scanline PNG filters (RFC 2083 section 6)."""
    stride = width * bpp
    out = bytearray(height * stride)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        kind = raw[pos]
        pos += 1
        line = bytearray(raw[pos : pos + stride])
        pos += stride
        if kind == 1:  # Sub
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif kind == 2:  # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif kind == 3:  # Average
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif kind == 4:  # Paeth
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                up = prev[i]
                upleft = prev[i - bpp] if i >= bpp else 0
                guess = left + up - upleft
                dl, du, dul = abs(guess - left), abs(guess - up), abs(guess - upleft)
                if dl <= du and dl <= dul:
                    line[i] = (line[i] + left) & 0xFF
                elif du <= dul:
                    line[i] = (line[i] + up) & 0xFF
                else:
                    line[i] = (line[i] + upleft) & 0xFF
        elif kind != 0:
            raise ValueError(f"unknown PNG filter {kind} on row {y}")
        out[y * stride : (y + 1) * stride] = line
        prev = line
    return bytes(out)


def downscale(width, height, rgba, size):
    """Box-filters RGBA down to size x size, averaging in premultiplied alpha.

    Averaging the color channels straight would pull the transparent pixels
    just outside the note's outline - black with alpha 0 - into the edge and
    leave a dark fringe all the way round, most visible at 16 and 20 where the
    outline is already only a pixel wide. Weighting each color by its own
    alpha, then dividing the total back out, is what keeps the edge the color
    of the paper.
    """
    out = bytearray(size * size * 4)
    scale_x = width / size
    scale_y = height / size
    for ty in range(size):
        top, bottom = ty * scale_y, (ty + 1) * scale_y
        for tx in range(size):
            left, right = tx * scale_x, (tx + 1) * scale_x
            red = green = blue = alpha = coverage = 0.0
            for y in range(int(top), min(height, math.ceil(bottom))):
                weight_y = min(y + 1, bottom) - max(y, top)
                if weight_y <= 0.0:
                    continue
                row = y * width * 4
                for x in range(int(left), min(width, math.ceil(right))):
                    weight = (min(x + 1, right) - max(x, left)) * weight_y
                    if weight <= 0.0:
                        continue
                    i = row + x * 4
                    a = rgba[i + 3]
                    red += rgba[i] * a * weight
                    green += rgba[i + 1] * a * weight
                    blue += rgba[i + 2] * a * weight
                    alpha += a * weight
                    coverage += weight
            i = (ty * size + tx) * 4
            if alpha > 0.0:
                out[i] = min(255, int(red / alpha + 0.5))
                out[i + 1] = min(255, int(green / alpha + 0.5))
                out[i + 2] = min(255, int(blue / alpha + 0.5))
            out[i + 3] = min(255, int(alpha / coverage + 0.5)) if coverage > 0.0 else 0
    return bytes(out)


def write_png(size, rgba):
    def chunk(kind, body):
        return (
            struct.pack(">I", len(body))
            + kind
            + body
            + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
        )

    stride = size * 4
    raw = bytearray()
    for y in range(size):
        raw.append(0)  # filter None: these are small, and zlib does the work
        raw += rgba[y * stride : (y + 1) * stride]

    return (
        PNG_MAGIC
        + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )


def main():
    width, height, rgba = read_png(PNG_PATH)
    if width != height:
        raise ValueError(f"{PNG_PATH}: need a square icon, got {width}x{height}")
    with open(PNG_PATH, "rb") as f:
        source_bytes = f.read()

    # The entry that matches the artwork's own resolution is the artwork's own
    # file, byte for byte - decoding and re-encoding it could only lose to it.
    pngs = [
        source_bytes if size == width else write_png(size, downscale(width, height, rgba, size))
        for size in SIZES
    ]

    # Assembled by hand (ICONDIR + one ICONDIRENTRY per size + the raw PNG
    # bytes back to back). The modern ICO format (Vista+) allows each entry's
    # image data to be a plain PNG file rather than the legacy uncompressed
    # BMP+AND-mask pair, which is all this needs.
    header = struct.pack("<HHH", 0, 1, len(SIZES))  # ICONDIR: reserved, type=icon, count
    entries = b""
    body = b""
    offset = len(header) + 16 * len(SIZES)
    for size, png in zip(SIZES, pngs):
        wh = size if size < 256 else 0  # 0 means 256 in the ICONDIRENTRY's 1-byte fields
        entries += struct.pack("<BBBBHHII", wh, wh, 0, 0, 1, 32, len(png), offset)
        body += png
        offset += len(png)

    with open(ICO_PATH, "wb") as f:
        f.write(header + entries + body)

    print(f"Wrote {ICO_PATH} ({', '.join(f'{s}x{s}' for s in SIZES)})")


if __name__ == "__main__":
    main()
