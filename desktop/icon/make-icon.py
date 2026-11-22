"""Turn G-hanko-source.png (red seal on a cream square) into the app icon:
cream outside the seal -> transparent, seal scaled to fill the icon.
Usage: python make-icon.py  (needs Pillow; writes rdp-launcher-1024.png)."""
from PIL import Image, ImageDraw, ImageFilter
import os

here = os.path.dirname(os.path.abspath(__file__))
src = Image.open(os.path.join(here, "G-hanko-source.png")).convert("RGB")
W, H = src.size
RED = (0xC8, 0x10, 0x2E)
CREAM_G, RED_G = 0xEF, 0x10          # green channel separates cream from red best

# 1. Outside region = everything reachable from the corners without crossing red.
mark = src.copy()
for corner in [(0, 0), (W - 1, 0), (0, H - 1), (W - 1, H - 1)]:
    ImageDraw.floodfill(mark, corner, (255, 0, 255), thresh=40)
outside = Image.eval(mark.split()[1], lambda g: 255 if g == 0 else 0)  # magenta has G=0
outside = outside.point(lambda v: v)  # L mask: 255 = outside

# 2. Edge band (just inside the outside region): blend pixels -> red with alpha.
near = outside.filter(ImageFilter.MaxFilter(5))   # outside grown by 2 px
out = Image.new("RGBA", (W, H))
sp, op, np_ = src.load(), outside.load(), near.load()
dp = out.load()
for y in range(H):
    for x in range(W):
        r, g, b = sp[x, y]
        if op[x, y]:
            dp[x, y] = (0, 0, 0, 0)
        elif np_[x, y]:
            t = max(0.0, min(1.0, (CREAM_G - g) / (CREAM_G - RED_G)))
            dp[x, y] = RED + (int(round(t * 255)),)
        else:
            dp[x, y] = (r, g, b, 255)

# 3. Crop to the seal and scale it to ~90% of a 1024 canvas, centred.
seal = out.crop(out.getbbox())
S, fill = 1024, 0.90
k = fill * S / max(seal.size)
seal = seal.resize((round(seal.width * k), round(seal.height * k)), Image.LANCZOS)
icon = Image.new("RGBA", (S, S), (0, 0, 0, 0))
icon.alpha_composite(seal, ((S - seal.width) // 2, (S - seal.height) // 2))
icon.save(os.path.join(here, "rdp-launcher-1024.png"))
print("wrote rdp-launcher-1024.png; seal box", out.getbbox())
