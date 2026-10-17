"""Draws the Explicitly Desktop icon: a music note whose speech bubble swears
("#@$%!"), with the bubble crossed out by a cancel sign.

Writes Resources/icon.png (1024 px) and Resources/icon.ico (16-256 px).
Run: python Resources/make_icon.py
"""
import math
import os
from PIL import Image, ImageDraw, ImageFont

S = 4                      # Supersampling factor
N = 1024 * S
here = os.path.dirname(os.path.abspath(__file__))

NOTE = (94, 53, 177, 255)        # Purple
BUBBLE = (255, 255, 255, 255)
INK = (33, 33, 33, 255)
RED = (229, 57, 53, 255)

img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
p = lambda v: int(v * S)

# --- Music note (eighth note), lower left -------------------------------------
def bezier(pts, steps=40):
    (x0, y0), (x1, y1), (x2, y2), (x3, y3) = pts
    out = []
    for k in range(steps + 1):
        t = k / steps
        mt = 1 - t
        out.append((mt**3 * x0 + 3 * mt**2 * t * x1 + 3 * mt * t**2 * x2 + t**3 * x3,
                    mt**3 * y0 + 3 * mt**2 * t * y1 + 3 * mt * t**2 * y2 + t**3 * y3))
    return out

# Stem
d.rounded_rectangle([p(262), p(420), p(316), p(860)], radius=p(18), fill=NOTE)
# Flag: outer curve down and out, inner curve back to the stem
outer = bezier([(316, 420), (330, 520), (450, 540), (420, 720)])
inner = bezier([(410, 680), (420, 590), (350, 580), (316, 560)])
d.polygon([(p(x), p(y)) for x, y in outer + inner], fill=NOTE)
# Head: tilted ellipse whose right edge meets the stem's foot
hcx, hcy, ha, hb, ang = 182, 858, 132, 92, math.radians(-25)
head = []
for k in range(120):
    t = 2 * math.pi * k / 120
    x, y = ha * math.cos(t), hb * math.sin(t)
    head.append((p(hcx + x * math.cos(ang) - y * math.sin(ang)), p(hcy + x * math.sin(ang) + y * math.cos(ang))))
d.polygon(head, fill=NOTE)

# --- Speech bubble with grawlix, upper right ------------------------------------
cx, cy, r = 680, 350, 310     # Cancel sign circle
bx0, by0, bx1, by1 = 470, 240, 890, 460
d.polygon([(p(530), p(440)), (p(630), p(450)), (p(400), p(560))], fill=INK)                 # Tail outline (points at the note)
d.rounded_rectangle([p(bx0), p(by0), p(bx1), p(by1)], radius=p(70), fill=INK)
d.polygon([(p(545), p(430)), (p(615), p(438)), (p(430), p(530))], fill=BUBBLE)              # Tail fill
d.rounded_rectangle([p(bx0 + 20), p(by0 + 20), p(bx1 - 20), p(by1 - 20)], radius=p(52), fill=BUBBLE)

font = ImageFont.truetype(r"C:\Windows\Fonts\impact.ttf", p(140))
text = "#@$%!"
tb = d.textbbox((0, 0), text, font=font)
tw, th = tb[2] - tb[0], tb[3] - tb[1]
d.text((p((bx0 + bx1) / 2) - tw / 2 - tb[0], p((by0 + by1) / 2) - th / 2 - tb[1]), text, font=font, fill=INK)

# --- Cancel sign over the bubble ------------------------------------------------
ring = p(64)
d.ellipse([p(cx - r), p(cy - r), p(cx + r), p(cy + r)], outline=RED, width=ring)
a = math.radians(45)
dx, dy = math.cos(a) * (r - 40), math.sin(a) * (r - 40)
d.line([(p(cx - dx), p(cy - dy)), (p(cx + dx), p(cy + dy))], fill=RED, width=ring)

out = img.resize((1024, 1024), Image.LANCZOS)
out.save(os.path.join(here, "icon.png"))
out.save(os.path.join(here, "icon.ico"), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
out.resize((256, 256), Image.LANCZOS).save(os.path.join(here, "icon-256.png"))
print("wrote icon.png, icon-256.png, icon.ico")
