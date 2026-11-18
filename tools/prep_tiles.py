#!/usr/bin/env python3
"""Prepare preloaded map tiles for sondechase.

Input : an MBTiles file (raster PNG/JPEG tiles) or a z/x/y tile directory that you already
        have the right to use offline (self-rendered, or from a provider that permits it).
        This tool does not download anything.
Output: <out>/<z>/<x>/<y>.png, 256x256, limited to a bounding box and zoom range,
        recoloured to the dark palette of the UI.

Example:
  prep_tiles.py --mbtiles vietnam.mbtiles --bbox 105.3,20.5,106.6,21.5 --zoom 8-14 --out tiles
"""
import argparse, io, math, os, sqlite3, sys

try:
    from PIL import Image
except ImportError:
    Image = None

GROUND, LINE, HI, WATER = (10, 6, 20), (58, 36, 102), (167, 155, 209), (31, 47, 122)


def tile_of(lat, lon, z):
    n = 1 << z
    x = (lon + 180.0) / 360.0 * n
    y = (1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2.0 * n
    return int(x), int(y)


def tile_range(bbox, z):
    w, s, e, n = bbox
    x0, y0 = tile_of(n, w, z)
    x1, y1 = tile_of(s, e, z)
    lim = (1 << z) - 1
    return max(0, x0), max(0, y0), min(lim, x1), min(lim, y1)


def recolor(img):
    """Light street map -> dark UI palette.
    Brightness of the output follows the colour distance from the dominant (land) colour, so
    roads, labels and boundaries all turn into light lines on a dark ground; blue-dominant
    pixels (water) get their own tint."""
    img = img.convert("RGB")
    colors = img.getcolors(maxcolors=256 * 256)
    land = max(colors, key=lambda c: c[0])[1] if colors else (242, 239, 233)
    px = img.load()
    for yy in range(img.height):
        for xx in range(img.width):
            r, g, b = px[xx, yy]
            if b > r + 18 and b > g + 6:
                px[xx, yy] = WATER
                continue
            if min(r, g, b) >= 250 and land != (r, g, b):  # white = road casing on most light styles
                px[xx, yy] = tuple(int(LINE[i] + (HI[i] - LINE[i]) * 0.6) for i in range(3))
                continue
            d = min(1.0, (abs(r - land[0]) + abs(g - land[1]) + abs(b - land[2])) / 150.0)
            if d < 0.5:
                t, a, c = d / 0.5, GROUND, LINE
            else:
                t, a, c = (d - 0.5) / 0.5, LINE, HI
            px[xx, yy] = tuple(int(a[i] + (c[i] - a[i]) * t) for i in range(3))
    return img


def write_tile(data, path, do_recolor):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if Image is None:
        if do_recolor:
            sys.exit("Pillow is required for recolouring: sudo apt install python3-pil (or use --recolor none)")
        with open(path, "wb") as f:
            f.write(data)
        return
    img = Image.open(io.BytesIO(data))
    if img.size != (256, 256):
        img = img.resize((256, 256))
    img = recolor(img) if do_recolor else img.convert("RGB")
    img.save(path, "PNG", optimize=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--mbtiles")
    src.add_argument("--dir", help="z/x/y.png tile directory")
    ap.add_argument("--bbox", required=True, help="west,south,east,north in degrees")
    ap.add_argument("--zoom", default="8-14", help="min-max")
    ap.add_argument("--out", required=True)
    ap.add_argument("--recolor", choices=["dark", "none"], default="dark")
    a = ap.parse_args()
    bbox = [float(v) for v in a.bbox.split(",")]
    zmin, zmax = [int(v) for v in a.zoom.split("-")]
    db = sqlite3.connect(a.mbtiles) if a.mbtiles else None
    n = miss = 0
    for z in range(zmin, zmax + 1):
        x0, y0, x1, y1 = tile_range(bbox, z)
        for x in range(x0, x1 + 1):
            for y in range(y0, y1 + 1):
                if db:  # MBTiles rows are TMS: y axis flipped
                    row = db.execute("SELECT tile_data FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=?",
                                     (z, x, (1 << z) - 1 - y)).fetchone()
                    data = row[0] if row else None
                else:
                    p = os.path.join(a.dir, str(z), str(x), f"{y}.png")
                    data = open(p, "rb").read() if os.path.exists(p) else None
                if data is None:
                    miss += 1
                    continue
                write_tile(data, os.path.join(a.out, str(z), str(x), f"{y}.png"), a.recolor == "dark")
                n += 1
    print(f"{n} tiles written to {a.out}, {miss} not present in the source")


if __name__ == "__main__":
    main()
