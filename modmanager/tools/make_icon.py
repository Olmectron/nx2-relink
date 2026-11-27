#!/usr/bin/env python3
"""Draws the program's icon (icon.jpg, 256 x 256): three rows of a list, each with its tick box, two of them ticked.

    python tools/make_icon.py [icon.jpg]          (needs Pillow)
"""
import sys

from PIL import Image, ImageDraw

S = 4  # (drawn four times as large, then made smaller: smooth edges)
N = 256 * S
BG_TOP, BG_BOTTOM = (22, 28, 40), (12, 15, 22)
ROW, ROW_SELECTED = (36, 46, 64), (42, 70, 96)
ACCENT, LIGHT, DIM = (64, 200, 184), (232, 236, 242), (120, 132, 150)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else 'icon.jpg'
    img = Image.new('RGB', (N, N))
    d = ImageDraw.Draw(img)
    for y in range(N):
        t = y / (N - 1)
        d.line([(0, y), (N, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOTTOM)))
    rows = [(True, 0.62), (True, 0.44), (False, 0.54)]  # (ticked, how long its name is)
    top, height, gap, left, right = 0.17, 0.19, 0.045, 0.11, 0.89
    for k, (ticked, length) in enumerate(rows):
        y0 = top + k * (height + gap)
        box = [left * N, y0 * N, right * N, (y0 + height) * N]
        d.rounded_rectangle(box, radius=0.035 * N, fill=ROW_SELECTED if k == 1 else ROW)
        # the tick box
        b0, side = left + 0.045, height - 0.08
        by = y0 + 0.04
        d.rounded_rectangle([b0 * N, by * N, (b0 + side) * N, (by + side) * N], radius=0.018 * N, outline=LIGHT if ticked else DIM, width=round(0.014 * N))
        if ticked:
            m = 0.03
            d.rounded_rectangle([(b0 + m) * N, (by + m) * N, (b0 + side - m) * N, (by + side - m) * N], radius=0.008 * N, fill=ACCENT)
        # the name, as a bar
        x0 = b0 + side + 0.05
        bar = 0.034
        cy = y0 + height / 2
        d.rounded_rectangle([x0 * N, (cy - bar / 2) * N, (x0 + (right - 0.05 - x0) * length) * N, (cy + bar / 2) * N], radius=bar / 2 * N, fill=LIGHT if ticked else DIM)
    # an arrow each way at the right of the middle row: its place in the order can be changed
    cx, cy = right - 0.085, top + (height + gap) + height / 2
    for sign in (-1, 1):
        tip = cy + sign * 0.062
        d.polygon([((cx - 0.034) * N, (cy + sign * 0.02) * N), ((cx + 0.034) * N, (cy + sign * 0.02) * N), (cx * N, tip * N)], fill=ACCENT)
    img.resize((256, 256), Image.LANCZOS).save(out, quality=95)
    print('written:', out)


if __name__ == '__main__':
    main()
