#!/usr/bin/env python3
"""Assemble the animated water arc strip for the sprinkler enemy.

Takes tools/raw/water_frame_0..4.png (each 64x32 RGBA) and writes a
320x32 horizontal strip (frame order 0..4 left-to-right) to
sprites/water_arc.png, flattening alpha onto the magenta transparency
key so the firmware image loader treats it as transparent.

Usage:
  build_water_strip.py            # default in/out paths
"""
import os
import sys
from PIL import Image

KEY = (255, 0, 255)
FRAME_W, FRAME_H = 64, 32
FRAMES = 5

HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.path.join(HERE, "raw")
OUT = os.path.normpath(os.path.join(HERE, "..", "sprites", "water_arc.png"))


def main():
    strip = Image.new("RGB", (FRAME_W * FRAMES, FRAME_H), KEY)
    for i in range(FRAMES):
        src = os.path.join(RAW, f"water_frame_{i}.png")
        img = Image.open(src)
        if img.size != (FRAME_W, FRAME_H):
            sys.exit(f"{src}: size {img.size} != {(FRAME_W, FRAME_H)}")
        if img.mode in ("RGBA", "LA", "P"):
            img = img.convert("RGBA")
            cell = Image.new("RGB", img.size, KEY)
            cell.paste(img, (0, 0), img)   # alpha-composite over magenta
        else:
            cell = img.convert("RGB")
        strip.paste(cell, (i * FRAME_W, 0))
    strip.save(OUT)
    print(f"water_arc: {OUT} ({strip.width}x{strip.height}, {FRAMES} frames)")


if __name__ == "__main__":
    main()
