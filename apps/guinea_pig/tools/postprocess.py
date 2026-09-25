#!/usr/bin/env python3
"""Post-process PixelLab PNGs for PicOS: flatten alpha to the magenta
transparency key, verify tile opacity, slice grid sheets.

Usage:
  postprocess.py flatten <in.png> <out.png>
  postprocess.py opaque  <file.png>           # exit 1 if any magenta pixel
  postprocess.py slice   <sheet.png> <cw> <ch> <col> <row> <out.png>
"""
import sys
from PIL import Image

KEY = (255, 0, 255)

def flatten(src, dst):
    img = Image.open(src)
    if img.mode in ("RGBA", "LA", "P"):
        img = img.convert("RGBA")
        out = Image.new("RGB", img.size, KEY)
        out.paste(img, (0, 0), img)          # alpha-composite over magenta
    else:
        out = img.convert("RGB")
    out.save(dst)
    print(f"flatten: {dst} ({out.width}x{out.height})")

def opaque(path):
    img = Image.open(path).convert("RGB")
    n = sum(1 for p in img.getdata() if p == KEY)
    if n:
        print(f"opaque: FAIL {path}: {n} transparent pixels")
        sys.exit(1)
    print(f"opaque: OK {path}")

def slice_cell(sheet, cw, ch, col, row, out):
    img = Image.open(sheet)
    cell = img.crop((col * cw, row * ch, (col + 1) * cw, (row + 1) * ch))
    cell.save(out)
    print(f"slice: {out} ({cw}x{ch} from {col},{row})")

if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "flatten":
        flatten(sys.argv[2], sys.argv[3])
    elif cmd == "opaque":
        opaque(sys.argv[2])
    elif cmd == "slice":
        slice_cell(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]),
                   int(sys.argv[5]), int(sys.argv[6]), sys.argv[7])
    else:
        sys.exit(f"unknown command {cmd}")
