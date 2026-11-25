"""The menu's font: an atlas of Latin-1's printable characters (32..255), 16 to a row, made with Pillow's own default font.

  python tools/font.py        writes runtime/font.bin (and build/font.png, to look at)

font.bin: u16 width, height, cell width, cell height; 224 bytes, each character's advance; the atlas, a byte of coverage a pixel.
"""
import os
import struct

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZE, CW, CH, COLS = 26, 24, 36, 16
font = ImageFont.load_default(size=SIZE)
rows = (224 + COLS - 1) // COLS
img = Image.new('L', (CW * COLS, CH * rows), 0)
draw = ImageDraw.Draw(img)
adv = bytearray()
for i in range(224):
    ch = chr(32 + i)
    if 127 <= 32 + i < 160:
        adv.append(0)
        continue
    draw.text((i % COLS * CW + 1, i // COLS * CH + 2), ch, font=font, fill=255)
    adv.append(min(CW, int(round(font.getlength(ch)))))
os.makedirs(f'{ROOT}/build', exist_ok=True)
img.save(f'{ROOT}/build/font.png')
open(f'{ROOT}/runtime/font.bin', 'wb').write(struct.pack('<4H', img.width, img.height, CW, CH) + bytes(adv) + img.tobytes())
print(img.size, 'advances', list(adv[1:6]), list(adv[33:38]))
