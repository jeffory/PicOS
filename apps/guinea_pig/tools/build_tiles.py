#!/usr/bin/env python3
"""Build the seamless ground tiles for Guinea Pig Run from the PixelLab
sidescroller Wang tileset (tools/raw/sidescroller_tileset.png, 64x64,
4x4 grid of 16px cells).

Deterministic — no generation happens here.

Tileset anatomy (verified empirically, see alpha census):
  * wang_15 (all corners "upper") is a fully EMPTY cell and wang_0 (all
    corners "lower") is solid earth — this set renders the EARTH BODY only;
    "upper"/grass regions are transparent air, and grass fringe is drawn
    on the earth body wherever it borders air.
  * Every surface tile is BOTTOM-aligned in its cell: 7 fully-transparent
    rows on top, blade fringe at row 7, grass band rows 8-11, dark outline
    row 12, earth rows 13-15. (E.g. wang_12: only 122/256 px opaque.)
  * wang_4 (32,0) is an INSIDE CORNER (floor meets left wall: full-height
    earth strip on the left), not an end cap. The true end caps are the
    rounded bottom-aligned blobs wang_14 (16,48, left end) and wang_13
    (0,0, right end).

Because the game wants grass AT the platform top (the old tiles and the
procedural fallback both put grass at sy), each surface tile is shifted
up SHIFT px so its blade fringe lands on output row 0, then alpha-
composited over the fully-opaque earth cell wang_0. Outputs are therefore
100% opaque by construction — no magenta anywhere, which is what fixes
the 1px gap bug (the old tiles had 64 magenta px each).

Cell picks (0-based pixel x,y in the sheet; see tools/raw/sidescroller_tileset.json):
  w12 = (48, 0)  flat grass surface      -> tile_grass_top.png
  w14 = (16,48)  left end cap (rounded)  -> tile_edge_l.png
  w13 = ( 0, 0)  right end cap (rounded) -> tile_edge_r.png (mirror of w14,
                                              per controller decision)
  w0  = (32,16)  solid earth interior    -> tile_earth.png

Outputs (RGB PNGs, 16x16) into ../sprites/.
Run from apps/guinea_pig/:  python3 tools/build_tiles.py
"""
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.normpath(os.path.join(HERE, ".."))
SHEET = os.path.join(HERE, "raw", "sidescroller_tileset.png")
OUT = os.path.join(APP, "sprites")

CELL = 16
GRASS_TOP_XY = (48, 0)   # wang_12 flat surface
CAP_LEFT_XY = (16, 48)   # wang_14 left end blob
EARTH_XY = (32, 16)      # wang_0 solid earth
SHIFT = 7                # lift bottom-aligned graphic so blades hit row 0


def crop_cell(sheet, x, y):
    return sheet.crop((x, y, x + CELL, y + CELL))


def surface_tile(earth, cell):
    """cell shifted up SHIFT px, alpha-composited over the opaque earth
    cell -> 100% opaque RGB tile with the grass fringe at the top."""
    shifted = Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0))
    shifted.paste(cell, (0, -SHIFT))
    return Image.alpha_composite(earth, shifted).convert("RGB")


def main():
    sheet = Image.open(SHEET).convert("RGBA")
    w0 = crop_cell(sheet, *EARTH_XY)
    w12 = crop_cell(sheet, *GRASS_TOP_XY)
    w14 = crop_cell(sheet, *CAP_LEFT_XY)

    grass_top = surface_tile(w0, w12)
    edge_l = surface_tile(w0, w14)
    edge_r = edge_l.transpose(Image.FLIP_LEFT_RIGHT)
    earth = w0.convert("RGB")

    for name, img in (
        ("tile_grass_top.png", grass_top),
        ("tile_edge_l.png", edge_l),
        ("tile_edge_r.png", edge_r),
        ("tile_earth.png", earth),
    ):
        path = os.path.join(OUT, name)
        img.save(path)
        print(f"wrote {path} ({img.width}x{img.height})")


if __name__ == "__main__":
    main()
