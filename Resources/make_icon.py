"""Draws the Explicitly Desktop icon: beamed eighth notes whose speech bubble
swears ("#@$%!"), with the bubble crossed out by a cancel sign in front.

Writes Resources/icon.png (1024 px), icon-256.png and icon.ico (16-256 px).
Run: python Resources/make_icon.py
"""
import math
import os
from PIL import Image, ImageDraw, ImageFont

S = 4                      # Supersampling factor
N = 1024 * S
here = os.path.dirname(os.path.abspath(__file__))

NOTE = (251, 140, 0, 255)        # Orange
NOTE_EDGE = (120, 60, 0, 255)    # Dark rim so the notes read on light backgrounds
BUBBLE = (255, 255, 255, 255)
INK = (33, 33, 33, 255)
RED = (229, 57, 53, 255)

img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
p = lambda v: int(round(v * S))


def ellipse_points(cx, cy, a, b, angle_deg, steps=160):
    ang = math.radians(angle_deg)
    pts = []
    for k in range(steps):
        t = 2 * math.pi * k / steps
        x, y = a * math.cos(t), b * math.sin(t)
        pts.append((p(cx + x * math.cos(ang) - y * math.sin(ang)), p(cy + x * math.sin(ang) + y * math.cos(ang))))
    return pts


def beamed_notes(grow, colour):
    """Two note heads, two stems and a slanted beam (grow > 0 draws the rim)."""
    stem_w = 70
    heads = [(205, 870), (625, 800)]          # Head centres
    head_a, head_b = 150, 104
    tops = [560, 490]                          # Stem tops (the beam slants up to the right)
    for (hx, hy), top in zip(heads, tops):
        d.polygon(ellipse_points(hx, hy, head_a + grow, head_b + grow, -22), fill=colour)
        sx = hx + head_a * math.cos(math.radians(22)) - stem_w + 8   # Stem on the head's right side
        d.rectangle([p(sx - grow), p(top - grow), p(sx + stem_w + grow), p(hy - 20)], fill=colour)
    # Beam joining the stem tops
    x0 = heads[0][0] + head_a * math.cos(math.radians(22)) - stem_w + 8
    x1 = heads[1][0] + head_a * math.cos(math.radians(22)) + 8
    beam = 120
    d.polygon([(p(x0 - grow), p(tops[0] - grow)), (p(x1 + grow), p(tops[1] - grow)),
               (p(x1 + grow), p(tops[1] + beam + grow)), (p(x0 - grow), p(tops[0] + beam + grow))], fill=colour)


# --- Notes (background) -------------------------------------------------------
beamed_notes(14, NOTE_EDGE)
beamed_notes(0, NOTE)

# --- Speech bubble with grawlix (foreground, top of the icon) ------------------
cx, cy, r = 512, 360, 345            # Cancel sign circle
bx0, by0, bx1, by1 = 262, 225, 762, 495
d.polygon([(p(330), p(470)), (p(450), p(482)), (p(250), p(610))], fill=INK)          # Tail outline (to the notes)
d.rounded_rectangle([p(bx0), p(by0), p(bx1), p(by1)], radius=p(80), fill=INK)
d.polygon([(p(348), p(462)), (p(432), p(470)), (p(280), p(572))], fill=BUBBLE)       # Tail fill
d.rounded_rectangle([p(bx0 + 22), p(by0 + 22), p(bx1 - 22), p(by1 - 22)], radius=p(60), fill=BUBBLE)

font = ImageFont.truetype(r"C:\Windows\Fonts\impact.ttf", p(170))
text = "#@$%!"
tb = d.textbbox((0, 0), text, font=font)
tw, th = tb[2] - tb[0], tb[3] - tb[1]
d.text((p((bx0 + bx1) / 2) - tw / 2 - tb[0], p((by0 + by1) / 2) - th / 2 - tb[1]), text, font=font, fill=INK)

# --- Cancel sign (front) ------------------------------------------------------
ring = p(74)
d.ellipse([p(cx - r), p(cy - r), p(cx + r), p(cy + r)], outline=RED, width=ring)
a = math.radians(45)
dx, dy = math.cos(a) * (r - 45), math.sin(a) * (r - 45)
d.line([(p(cx - dx), p(cy - dy)), (p(cx + dx), p(cy + dy))], fill=RED, width=ring)

out = img.resize((1024, 1024), Image.LANCZOS)
out.save(os.path.join(here, "icon.png"))
out.save(os.path.join(here, "icon.ico"), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
out.resize((256, 256), Image.LANCZOS).save(os.path.join(here, "icon-256.png"))
print("wrote icon.png, icon-256.png, icon.ico")
