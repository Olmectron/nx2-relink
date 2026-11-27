#!/usr/bin/env python3
"""Makes a PNG file of every PPM picture in a folder (the preview program writes PPM), into another folder.

    python tools/ppm_to_png.py <folder of .ppm> <folder for .png>      (needs Pillow)
"""
import os
import sys

from PIL import Image


def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    for name in sorted(os.listdir(src)):
        if name.endswith('.ppm'):
            out = os.path.join(dst, name[:-4] + '.png')
            Image.open(os.path.join(src, name)).save(out, optimize=True)
            os.remove(os.path.join(src, name))
            print('written:', out)


if __name__ == '__main__':
    main()
