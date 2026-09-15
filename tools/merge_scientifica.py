#!/usr/bin/env python3
"""One-off: rewrite src/fonts/font_scientifica.c so each face is one
contiguous [128][12] table covering 0x20..0x9F. 0x7F is blank; the shared
extended glyphs 0x80..0x9F are copied into both faces. Keeps the per-glyph
comments from the source.

Usage: python3 tools/merge_scientifica.py src/fonts/font_scientifica.c
"""
import re
import sys
from pathlib import Path


def parse(src, name, count):
    m = re.search(rf"const uint8_t {name}\[{count}\]\[12\] = \{{(.*?)\n\}};", src, re.S)
    if not m:
        sys.exit(f"{name} not found")
    rows = []
    for line in m.group(1).splitlines():
        code = line.split("//")[0]
        nums = re.findall(r"0x[0-9A-Fa-f]+", code)
        if len(nums) == 12:
            comment = line.split("//", 1)[1].strip() if "//" in line else ""
            rows.append(([int(n, 16) for n in nums], comment))
    if len(rows) != count:
        sys.exit(f"{name}: expected {count} rows, got {len(rows)}")
    return rows


def main():
    path = Path(sys.argv[1])
    src = path.read_text()
    reg = parse(src, "font_scientifica", 95)
    bold = parse(src, "font_scientifica_bold", 95)
    ext = parse(src, "font_scientifica_extended", 32)
    blank = ([0] * 12, "0x7F (blank)")

    def table(name, base):
        out = [f"const uint8_t {name}[128][12] = {{\n"]
        for bits, comment in base + [blank] + ext:
            out.append("    {" + ",".join(f"0x{b:02X}" for b in bits) + "}, // " + comment + "\n")
        out.append("};\n")
        return "".join(out)

    accessors = '''
const uint8_t* font_scientifica_glyph(char c) {
    uint8_t uc = (uint8_t)c;
    if (uc < FONT_SCI_FIRST || uc > FONT_SCI_LAST) return font_scientifica[0];
    return font_scientifica[uc - FONT_SCI_FIRST];
}

const uint8_t* font_scientifica_bold_glyph(char c) {
    uint8_t uc = (uint8_t)c;
    if (uc < FONT_SCI_FIRST || uc > FONT_SCI_LAST) return font_scientifica_bold[0];
    return font_scientifica_bold[uc - FONT_SCI_FIRST];
}
'''
    path.write_text(
        '#include "font_scientifica.h"\n'
        "// Merged by tools/merge_scientifica.py: 0x20..0x9F contiguous per face.\n\n"
        + table("font_scientifica", reg) + "\n"
        + table("font_scientifica_bold", bold) + accessors
    )
    print("rewrote", path)


if __name__ == "__main__":
    main()
