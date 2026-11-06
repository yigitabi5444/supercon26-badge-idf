#!/usr/bin/env python3
"""Convert the baked-in images to raw panel planes: main/defaults.bin.

usage: python3 tools/mkimages.py ["First" "Last" ...]

Each image becomes 2 planes of 960x672 MONO_HLSB (MSB = leftmost pixel, 1 = ink):
the black plane, then the red plane (161280 bytes per image). Name lines, if given,
are written on the nametag in black, like the stock nametag app does.
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W, H = 960, 672
ROOT = Path(__file__).resolve().parent.parent
IMAGES = ["hello_i_am_blank.png", "001.png", "006.png", "015.png", "026.png", "032.png", "037.png"]
NAME_TOP, NAME_BOTTOM = 221, 557  # white band of hello_i_am_blank.png


def planes(im):
    """RGB image -> black plane + red plane, each pixel snapped to the nearest of black/white/red."""
    k, r = bytearray(W * H // 8), bytearray(W * H // 8)
    for i, (R, G, B) in enumerate(im.getdata()):
        d_black = R * R + G * G + B * B
        d_white = (255 - R) ** 2 + (255 - G) ** 2 + (255 - B) ** 2
        d_red = (255 - R) ** 2 + G * G + B * B
        m = min(d_black, d_white, d_red)
        if m == d_black:
            k[i >> 3] |= 0x80 >> (i & 7)
        elif m == d_red:
            r[i >> 3] |= 0x80 >> (i & 7)
    return bytes(k + r)


def font(size):
    try:
        return ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Bold.ttf", size)
    except OSError:
        return ImageFont.truetype("DejaVuSans-Bold.ttf", size)


def draw_name(im, lines):
    size = 140
    while True:  # largest size where every line fits the band
        fnt = font(size)
        boxes = [fnt.getbbox(t) for t in lines]
        lh = max(b[3] - b[1] for b in boxes) * 1.15
        if max(b[2] - b[0] for b in boxes) <= W - 80 and lh * len(lines) <= NAME_BOTTOM - NAME_TOP - 40:
            break
        size -= 4
    d = ImageDraw.Draw(im)
    y = (NAME_TOP + NAME_BOTTOM - lh * len(lines)) / 2
    for t in lines:
        d.text((W / 2, y + lh / 2), t, font=fnt, fill="black", anchor="mm")
        y += lh


def selftest():
    im = Image.new("RGB", (W, H), "white")
    im.putpixel((0, 0), (0, 0, 0))
    im.putpixel((9, 0), (250, 10, 10))
    im.putpixel((W - 1, H - 1), (30, 30, 30))
    p = planes(im)
    P = W * H // 8
    assert p[0] == 0x80 and p[P + 1] == 0x40 and p[P - 1] == 0x01, "bit packing"
    assert sum(p) == 0x80 + 0x40 + 0x01, "stray ink"


def main():
    selftest()
    out = bytearray()
    for n, name in enumerate(IMAGES):
        src = Image.open(ROOT / "images" / name).convert("RGB")
        im = Image.new("RGB", (W, H), "white")
        im.paste(src, ((W - src.width) // 2, (H - src.height) // 2))  # gallery art is 960x670
        if n == 0 and sys.argv[1:]:
            draw_name(im, sys.argv[1:])
        out += planes(im)
    (ROOT / "main" / "defaults.bin").write_bytes(out)
    print(f"main/defaults.bin: {len(IMAGES)} images, {len(out)} bytes")


if __name__ == "__main__":
    main()
