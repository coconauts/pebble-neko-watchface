"""Convert the oneko XBM bitmaps to PNGs suitable for Pebble.

The XBMs are 32x32 1-bit images. We scale them up 3x (96x96) with
nearest-neighbor sampling to keep the pixel-art look, and draw the cat
as opaque black over a transparent background.

We auto-detect which pixel value represents the cat (the minority color).
"""

import os
from PIL import Image

SRC_DIR = "/home/marbu/dev/pebble/neko-watchface/bitmaps"
DST_DIR = "/home/marbu/dev/pebble/neko-watchface/neko-watchface/resources/images"
SCALE = 2  # 32*2 = 64 pixels

os.makedirs(DST_DIR, exist_ok=True)

# Only regenerate PNGs that already exist in DST_DIR — this preserves any
# deletions the user made to cull animations they don't want on the watch.
existing = {f for f in os.listdir(DST_DIR) if f.endswith(".png")}

for fname in sorted(os.listdir(SRC_DIR)):
    if not fname.endswith(".xbm"):
        continue
    out_name = fname.replace(".xbm", ".png")
    if out_name not in existing:
        continue
    src = os.path.join(SRC_DIR, fname)
    dst = os.path.join(DST_DIR, out_name)

    img = Image.open(src).convert("L")
    w, h = img.size

    data = list(img.getdata())
    black = sum(1 for p in data if p < 128)
    white = len(data) - black
    # The cat is the minority colour.
    cat_is_black = black <= white

    rgba = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    px = img.load()
    out = rgba.load()
    for y in range(h):
        for x in range(w):
            is_cat = (px[x, y] < 128) if cat_is_black else (px[x, y] >= 128)
            if is_cat:
                out[x, y] = (0, 0, 0, 255)

    rgba = rgba.resize((w * SCALE, h * SCALE), Image.NEAREST)
    rgba.save(dst, optimize=True)
    print(f"  {fname} -> {os.path.basename(dst)} ({w*SCALE}x{h*SCALE})")

print("Done.")
