#!/usr/bin/env python3
"""Validate guinea_pig sprites/ and sfx/ against the shipping manifest."""
import os, struct, sys, wave

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.normpath(os.path.join(HERE, ".."))

SPRITES = {  # name -> (w, h) or None to skip size check
    "tile_grass_top.png": (16, 16), "tile_earth.png": (16, 16),
    "tile_edge_l.png": (16, 16), "tile_edge_r.png": (16, 16),
    "bg_trees_far_1.png": None, "bg_trees_far_2.png": None,
    "bg_fence_mid_1.png": None, "bg_fence_mid_2.png": None,
    "bg_garden_near_1.png": None, "bg_garden_near_2.png": None,
    "sprinkler_body.png": (32, 32), "water_arc.png": (320, 32),
    "title_logo.png": None, "heart.png": (32, 32),
}
OPAQUE = {"tile_grass_top.png", "tile_earth.png",
          "tile_edge_l.png", "tile_edge_r.png"}

def check_wav(path, max_data=65536):
    with wave.open(path, "rb") as w:
        ok = (w.getframerate() == 11025 and w.getsampwidth() == 2
              and w.getnchannels() == 1)
        frames = w.getnframes()
    size = os.path.getsize(path)
    if not ok:
        return f"{path}: must be 11025Hz 16-bit mono"
    if os.path.basename(path).startswith("bank") and frames * 2 > max_data:
        return f"{path}: bank data {frames*2} > {max_data}"
    return None

def main():
    from PIL import Image
    errors = []
    for name, dims in SPRITES.items():
        p = os.path.join(APP, "sprites", name)
        if not os.path.exists(p):
            errors.append(f"missing sprite {name}"); continue
        img = Image.open(p).convert("RGB")
        if dims and img.size != dims:
            errors.append(f"{name}: size {img.size} != {dims}")
        if name in OPAQUE and any(px == (255, 0, 255) for px in img.getdata()):
            errors.append(f"{name}: has transparent pixels (tile gap bug)")
    for name in ("bank1.wav", "bgm.wav"):
        p = os.path.join(APP, "sfx", name)
        if not os.path.exists(p):
            errors.append(f"missing sfx {name}"); continue
        err = check_wav(p)
        if err: errors.append(err)
    rp = os.path.join(APP, "sfx", "sfx_ranges.lua")
    if not os.path.exists(rp):
        errors.append("missing sfx/sfx_ranges.lua")
    if errors:
        print("\n".join("FAIL " + e for e in errors)); sys.exit(1)
    print("check_assets: all OK")

if __name__ == "__main__":
    main()
