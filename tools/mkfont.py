#!/usr/bin/env python3
"""mkfont.py - build PicOS .pfn bitmap fonts.

  mkfont.py SRC OUT.pfn [--size N] [--cell WxH] [--range FIRST-LAST]
                        [--proportional] [--spacing N] [--dump]

SRC may be:
  *.bdf       - parsed directly; DWIDTH gives per-glyph advances
  *.ttf/*.otf - rasterised with Pillow at --size px (default 12)
  *.png       - fixed grid sheet, --cell WxH required, glyphs left to right,
                top to bottom, starting at the range's first code

.pfn layout (little-endian):
  "PFNT" ver=1 flags first last height max_width stride reserved
  widths[count] (u8)  bitmaps[count * height * stride] (row-major, MSB left)
"""
import argparse
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

MAX_DIM = 64


@dataclass
class Glyph:
    rows: list          # one int per row, bit (0x80 >> col) set = pixel on
    advance: int
    name: str = field(default="")


def parse_range(text):
    lo, hi = text.split("-")
    lo, hi = int(lo, 0), int(hi, 0)
    if not (0 <= lo <= hi <= 255):
        raise ValueError(f"bad range {text}")
    return lo, hi


def blank(height):
    return [0] * height


# ── BDF ─────────────────────────────────────────────────────────────────────

def load_bdf(lines, rng):
    """Return (glyphs, first, height, max_width) for codes rng[0]..rng[1]."""
    first, last = rng
    fbb = None
    chars = {}
    it = iter(lines)
    for line in it:
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "FONTBOUNDINGBOX":
            fbb = tuple(int(v) for v in parts[1:5])  # w h xoff yoff
        elif parts[0] == "STARTCHAR":
            name = parts[1] if len(parts) > 1 else ""
            enc = None; dw = None; bbx = None; bitmap = []
            for l in it:
                p = l.split()
                if not p:
                    continue
                if p[0] == "ENCODING":
                    enc = int(p[1])
                elif p[0] == "DWIDTH":
                    dw = int(p[1])
                elif p[0] == "BBX":
                    bbx = tuple(int(v) for v in p[1:5])
                elif p[0] == "BITMAP":
                    for b in it:
                        if b.strip() == "ENDCHAR":
                            break
                        bitmap.append(int(b.strip(), 16))
                    break
                elif p[0] == "ENDCHAR":
                    break
            if enc is not None and first <= enc <= last:
                chars[enc] = (name, dw, bbx, bitmap)
    if fbb is None:
        raise ValueError("BDF has no FONTBOUNDINGBOX")
    fw, fh, fxo, fyo = fbb
    height = fh
    glyphs = []
    max_w = 1
    for code in range(first, last + 1):
        if code not in chars:
            glyphs.append(Glyph(blank(height), 0, ""))
            continue
        name, dw, (bw, bh, bxo, byo), bitmap = chars[code]
        adv = dw if dw is not None else bw
        rows = blank(height)
        # BDF bitmap rows are top-down; place the BBX inside the font box.
        top = (fh + fyo) - (bh + byo)
        hexw = (bw + 7) // 8 * 8
        for i, val in enumerate(bitmap[:bh]):
            r = top + i
            if 0 <= r < height:
                # shift the glyph right by bxo - fxo columns
                v = val << (32 - hexw)          # MSB-align in 32 bits
                v >>= max(0, bxo - fxo)
                rows[r] = (v >> 24) & 0xFF if height else 0
        glyphs.append(Glyph(rows, max(adv, 1), name))
        max_w = max(max_w, adv, bw)
    for g in glyphs:
        if g.advance == 0:
            g.advance = max_w
    return glyphs, first, height, max_w


# ── TTF via Pillow ──────────────────────────────────────────────────────────

def load_ttf(path, size, rng):
    from PIL import Image, ImageDraw, ImageFont
    font = ImageFont.truetype(str(path), size)
    first, last = rng
    ascent, descent = font.getmetrics()
    height = ascent + descent
    glyphs = []
    max_w = 1
    for code in range(first, last + 1):
        ch = chr(code)
        adv = max(1, int(round(font.getlength(ch))))
        img = Image.new("L", (max(adv, 1), height), 0)
        ImageDraw.Draw(img).text((0, 0), ch, font=font, fill=255)
        rows = []
        for y in range(height):
            v = 0
            for x in range(min(adv, MAX_DIM)):
                if img.getpixel((x, y)) >= 128:
                    v |= 0x80 >> x if x < 8 else 0
            rows.append(v)
        glyphs.append(Glyph(rows, adv, ch))
        max_w = max(max_w, adv)
    return glyphs, first, height, max_w


# ── PNG grid sheet ──────────────────────────────────────────────────────────

def load_png(path, cell, rng):
    from PIL import Image
    cw, ch = cell
    first, last = rng
    img = Image.open(path).convert("L")
    per_row = img.width // cw
    if per_row == 0:
        raise ValueError("cell wider than image")
    glyphs = []
    for i, code in enumerate(range(first, last + 1)):
        gx, gy = (i % per_row) * cw, (i // per_row) * ch
        rows = []
        for y in range(ch):
            v = 0
            for x in range(cw):
                px, py = gx + x, gy + y
                if px < img.width and py < img.height and img.getpixel((px, py)) >= 128:
                    v |= 0x80 >> x
            rows.append(v)
        glyphs.append(Glyph(rows, cw, chr(code)))
    return glyphs, first, ch, cw


# ── Transforms and output ───────────────────────────────────────────────────

def trim_proportional(glyphs, max_width, spacing, first):
    out = []
    for i, g in enumerate(glyphs):
        ink = 0
        for r in g.rows:
            for col in range(max_width):
                if r & (0x80 >> col):
                    ink = max(ink, col + 1)
        if ink == 0:
            adv = max(max_width // 2, 2)
        else:
            adv = min(max_width, ink + spacing)
        out.append(Glyph(list(g.rows), adv, g.name))
    return out


def build_pfn(glyphs, first, height, max_width, proportional):
    count = len(glyphs)
    last = first + count - 1
    if not (1 <= height <= MAX_DIM and 1 <= max_width <= MAX_DIM):
        raise ValueError("height/width must be 1..64")
    if not (0 <= first <= last <= 255):
        raise ValueError("code range must fit in 0..255")
    stride = (max_width + 7) // 8
    if stride != 1:
        raise ValueError("this tool emits 1 byte per row; max_width must be <= 8 for now")
    hdr = struct.pack("<4sBBBBBBBB", b"PFNT", 1, 1 if proportional else 0,
                      first, last, height, max_width, stride, 0)
    widths = bytes(min(max(g.advance, 1), max_width) for g in glyphs)
    bitmaps = bytearray()
    for g in glyphs:
        if len(g.rows) != height:
            raise ValueError(f"glyph {g.name!r} has {len(g.rows)} rows, expected {height}")
        bitmaps.extend(r & 0xFF for r in g.rows)
    return hdr + widths + bytes(bitmaps)


def dump(glyphs, first, max_width):
    for i, g in enumerate(glyphs):
        print(f"--- 0x{first + i:02X} {g.name!r} advance={g.advance}")
        for r in g.rows:
            print("".join("#" if r & (0x80 >> c) else "." for c in range(max_width)))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src"); ap.add_argument("out")
    ap.add_argument("--size", type=int, default=12)
    ap.add_argument("--cell", help="WxH for PNG sheets")
    ap.add_argument("--range", default="0x20-0x7E")
    ap.add_argument("--proportional", action="store_true")
    ap.add_argument("--spacing", type=int, default=1)
    ap.add_argument("--dump", action="store_true")
    a = ap.parse_args(argv)
    rng = parse_range(a.range)
    src = Path(a.src)
    ext = src.suffix.lower()
    if ext == ".bdf":
        glyphs, first, height, max_w = load_bdf(src.read_text().splitlines(), rng)
        proportional = a.proportional or len({g.advance for g in glyphs}) > 1
    elif ext in (".ttf", ".otf"):
        glyphs, first, height, max_w = load_ttf(src, a.size, rng)
        proportional = True
    elif ext == ".png":
        if not a.cell:
            sys.exit("--cell WxH is required for PNG sheets")
        cw, ch = (int(v) for v in a.cell.lower().split("x"))
        glyphs, first, height, max_w = load_png(src, (cw, ch), rng)
        proportional = a.proportional
    else:
        sys.exit(f"unsupported source type {ext}")
    if a.proportional and ext != ".ttf":
        glyphs = trim_proportional(glyphs, max_w, a.spacing, first)
        proportional = True
    data = build_pfn(glyphs, first, height, max_w, proportional)
    Path(a.out).write_bytes(data)
    if a.dump:
        dump(glyphs, first, max_w)
    print(f"wrote {a.out}: {len(glyphs)} glyphs, {max_w}x{height}, "
          f"{'proportional' if proportional else 'monospace'}, {len(data)} bytes")


if __name__ == "__main__":
    main()
