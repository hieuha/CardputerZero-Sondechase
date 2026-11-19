#!/usr/bin/env python3
"""Draw the SondeChase icon in the UI palette: a compass ring with north, the arrow towards the sonde, the
landing point, and a radiosonde (balloon, line, sonde) in the middle, so it does not read as a plain compass.
Drawn 4x larger and scaled down for smooth edges. Needs Pillow.

Usage: tools/make_icon.py [out_dir]   (writes sondechase_256.png for the store, sondechase.png 100x100 for APPLaunch)
"""
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw

BG = (10, 6, 20)
RING = (60, 37, 106)
TICK = (170, 150, 225)
WHITE = (239, 235, 255)
CYAN = (25, 230, 255)
PINK = (255, 61, 154)
YELLOW = (255, 225, 74)
K = 4  # supersampling
C = 128 * K  # centre


def p(x, y):
    return (x * K, y * K)


def polar(r, deg):  # compass bearing: 0 = up, clockwise
    a = math.radians(deg)
    return (C + r * K * math.sin(a), C - r * K * math.cos(a))


def draw():
    im = Image.new("RGB", (256 * K, 256 * K), BG)
    d = ImageDraw.Draw(im)
    r = 96
    d.ellipse([C - r * K, C - r * K, C + r * K, C + r * K], outline=RING, width=9 * K)
    for deg in range(0, 360, 15):  # ticks on the ring; the four cardinal ones long and white
        major = deg % 90 == 0
        d.line([polar(r + 4, deg), polar(r - (14 if major else 7), deg)], fill=WHITE if major else TICK,
               width=(4 if major else 3) * K)
    d.polygon([p(116, 5), p(140, 5), p(128, 21)], fill=CYAN)  # north
    dx, dy = polar(r - 3, 52)
    s = 11 * K
    d.polygon([(dx, dy - s), (dx + s, dy), (dx, dy + s), (dx - s, dy)], fill=YELLOW)  # landing point

    # arrow towards the sonde: from behind the centre to the north-east
    tail, tip = polar(52, 225), polar(80, 45)
    d.line([tail, polar(56, 45)], fill=PINK, width=11 * K)
    a = math.radians(45)
    ux, uy = math.sin(a), -math.cos(a)
    bx, by = tip[0] - ux * 30 * K, tip[1] - uy * 30 * K
    d.polygon([tip, (bx - uy * 17 * K, by + ux * 17 * K), (bx + uy * 17 * K, by - ux * 17 * K)], fill=PINK)

    # radiosonde in the middle: balloon, knot, line, sonde box; a dark outline keeps it clear of the arrow
    bx0, by0, rx, ry = 128, 108, 19, 23
    d.ellipse([p(bx0 - rx - 5, by0 - ry - 5), p(bx0 + rx + 5, by0 + ry + 5)], fill=BG)
    d.rectangle([p(bx0 - 4, by0 + ry), p(bx0 + 4, 162)], fill=BG)
    d.rectangle([p(bx0 - 12, 147), p(bx0 + 12, 168)], fill=BG)
    d.ellipse([p(bx0 - rx, by0 - ry), p(bx0 + rx, by0 + ry)], fill=CYAN)
    d.ellipse([p(bx0 - 9, by0 - 15), p(bx0 - 2, by0 - 6)], fill=WHITE)  # highlight
    d.polygon([p(bx0 - 5, by0 + ry + 5), p(bx0 + 5, by0 + ry + 5), p(bx0, by0 + ry - 2)], fill=CYAN)  # knot
    d.line([p(bx0, by0 + ry + 4), p(bx0, 151)], fill=WHITE, width=2 * K)
    d.rectangle([p(bx0 - 7, 151), p(bx0 + 7, 164)], fill=WHITE)  # the sonde
    return im


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "assets/images")
    im = draw()
    im.resize((256, 256), Image.LANCZOS).save(out / "sondechase_256.png")
    im.resize((100, 100), Image.LANCZOS).save(out / "sondechase.png")
    print(f"wrote {out}/sondechase_256.png, {out}/sondechase.png")


if __name__ == "__main__":
    main()
