#!/usr/bin/env python3
"""Convert BDF bitmap font files to C arrays for PicOS.

Usage:
    python3 bdf2c.py <regular.bdf> <bold.bdf> <output_dir>

Generates font_scientifica.h and font_scientifica.c with row-major uint8_t[12]
arrays for ASCII 0x20-0x7E, positioned within a 6x12 cell.
"""

import sys
import os
import re


def parse_bdf(path):
    """Parse a BDF file and return dict of {codepoint: (bbx, bitmap_rows)}."""
    glyphs = {}
    font_ascent = 0

    with open(path) as f:
        lines = f.readlines()

    i = 0
    while i < len(lines):
        line = lines[i].strip()

        if line.startswith("FONT_ASCENT "):
            font_ascent = int(line.split()[1])

        if line.startswith("STARTCHAR"):
            encoding = -1
            bbw = bbh = bbox = bboy = 0
            bitmap = []

            i += 1
            while i < len(lines):
                line = lines[i].strip()
                if line.startswith("ENCODING "):
                    encoding = int(line.split()[1])
                elif line.startswith("BBX "):
                    parts = line.split()
                    bbw, bbh, bbox, bboy = int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4])
                elif line == "BITMAP":
                    i += 1
                    while i < len(lines) and lines[i].strip() != "ENDCHAR":
                        bitmap.append(int(lines[i].strip(), 16))
                        i += 1
                    break
                i += 1

            if 0x20 <= encoding <= 0x7E:
                glyphs[encoding] = {
                    "bbw": bbw, "bbh": bbh, "bbox": bbox, "bboy": bboy,
                    "bitmap": bitmap
                }

        i += 1

    return font_ascent, glyphs


def glyph_to_cell(glyph, font_ascent, cell_w=6, cell_h=12):
    """Position a glyph within a cell_w x cell_h cell, return cell_h row bytes."""
    cell = [0] * cell_h

    # BDF coordinate: baseline is at y=0, ascent goes up (positive bboy),
    # descent goes down (negative bboy).
    # In our cell, the baseline is at row (font_ascent - 1) from top (0-indexed).
    # The glyph's top pixel is at baseline - bboy - bbh + 1
    baseline_row = font_ascent  # row index in cell (0-indexed from top)
    top_row = baseline_row - glyph["bboy"] - glyph["bbh"]

    # x offset within cell
    x_off = glyph["bbox"]

    for r, rowdata in enumerate(glyph["bitmap"]):
        cell_row = top_row + r
        if cell_row < 0 or cell_row >= cell_h:
            continue

        # BDF bitmap: MSB = leftmost pixel, 1 byte per row
        # We need to shift glyph pixels into the cell, MSB = leftmost
        for bit in range(glyph["bbw"]):
            if rowdata & (0x80 >> bit):
                px = x_off + bit
                if 0 <= px < cell_w:
                    cell[cell_row] |= (0x80 >> px)

    return cell


def generate_c(regular_glyphs, regular_ascent, bold_glyphs, bold_ascent, output_dir):
    """Generate font_scientifica.h and font_scientifica.c."""

    def format_array(name, glyphs, ascent):
        lines = []
        lines.append(f"const uint8_t {name}[95][12] = {{")
        for cp in range(0x20, 0x7F):
            if cp in glyphs:
                cell = glyph_to_cell(glyphs[cp], ascent)
            else:
                cell = [0] * 12
            hex_str = ",".join(f"0x{b:02X}" for b in cell)
            ch = chr(cp) if 0x20 < cp < 0x7F else (" " if cp == 0x20 else "")
            comment = f" // '{ch}'" if ch else " // ' '"
            lines.append(f"    {{{hex_str}}},{comment}")
        lines.append("};")
        return "\n".join(lines)

    # Header
    header = """\
#pragma once

#include <stdint.h>

#define FONT_SCI_WIDTH  6
#define FONT_SCI_HEIGHT 12
#define FONT_SCI_COUNT  95

extern const uint8_t font_scientifica[95][12];
extern const uint8_t font_scientifica_bold[95][12];

const uint8_t* font_scientifica_glyph(char c);
const uint8_t* font_scientifica_bold_glyph(char c);
"""

    # Source
    regular_array = format_array("font_scientifica", regular_glyphs, regular_ascent)
    bold_array = format_array("font_scientifica_bold", bold_glyphs, bold_ascent)

    source = f"""\
#include "font_scientifica.h"

{regular_array}

{bold_array}

const uint8_t* font_scientifica_glyph(char c) {{
    if (c < 0x20 || c > 0x7E) return font_scientifica[0];
    return font_scientifica[c - 0x20];
}}

const uint8_t* font_scientifica_bold_glyph(char c) {{
    if (c < 0x20 || c > 0x7E) return font_scientifica_bold[0];
    return font_scientifica_bold[c - 0x20];
}}
"""

    os.makedirs(output_dir, exist_ok=True)
    with open(os.path.join(output_dir, "font_scientifica.h"), "w") as f:
        f.write(header)
    with open(os.path.join(output_dir, "font_scientifica.c"), "w") as f:
        f.write(source)

    print(f"Generated {output_dir}/font_scientifica.h")
    print(f"Generated {output_dir}/font_scientifica.c")


def main():
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <regular.bdf> <bold.bdf> <output_dir>")
        sys.exit(1)

    regular_path, bold_path, output_dir = sys.argv[1], sys.argv[2], sys.argv[3]

    regular_ascent, regular_glyphs = parse_bdf(regular_path)
    bold_ascent, bold_glyphs = parse_bdf(bold_path)

    print(f"Regular: ascent={regular_ascent}, {len(regular_glyphs)} ASCII glyphs")
    print(f"Bold: ascent={bold_ascent}, {len(bold_glyphs)} ASCII glyphs")

    generate_c(regular_glyphs, regular_ascent, bold_glyphs, bold_ascent, output_dir)


if __name__ == "__main__":
    main()
