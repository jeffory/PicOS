"""Unit tests for tools/mkfont.py. Run: python3 -m pytest tests/unit/test_mkfont.py -v"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import mkfont  # noqa: E402

TINY_BDF = """STARTFONT 2.1
FONT tiny
SIZE 4 75 75
FONTBOUNDINGBOX 3 4 0 0
CHARS 2
STARTCHAR A
ENCODING 65
DWIDTH 3 0
BBX 3 4 0 0
BITMAP
E0
A0
E0
00
ENDCHAR
STARTCHAR B
ENCODING 66
DWIDTH 2 0
BBX 2 4 0 0
BITMAP
C0
80
C0
00
ENDCHAR
ENDFONT
"""


def parse_header(data: bytes):
    magic, ver, flags, first, last, h, w, stride, res = struct.unpack("<4sBBBBBBBB", data[:12])
    return dict(magic=magic, ver=ver, flags=flags, first=first, last=last,
                height=h, max_width=w, stride=stride, reserved=res)


def test_bdf_roundtrip_is_proportional():
    glyphs, first, height, max_w = mkfont.load_bdf(TINY_BDF.splitlines(), (65, 66))
    assert (first, height, max_w) == (65, 4, 3)
    data = mkfont.build_pfn(glyphs, first, height, max_w, proportional=True)
    hdr = parse_header(data)
    assert hdr["magic"] == b"PFNT" and hdr["ver"] == 1 and hdr["reserved"] == 0
    assert hdr["flags"] & 1
    assert (hdr["first"], hdr["last"], hdr["stride"]) == (65, 66, 1)
    count = 2
    assert len(data) == 12 + count + count * 4 * 1
    assert list(data[12:14]) == [3, 2]                       # advances from DWIDTH
    assert list(data[14:18]) == [0xE0, 0xA0, 0xE0, 0x00]     # 'A' rows


def test_range_fills_missing_glyphs_blank():
    glyphs, first, height, max_w = mkfont.load_bdf(TINY_BDF.splitlines(), (64, 67))
    assert first == 64 and len(glyphs) == 4
    assert glyphs[0].rows == [0, 0, 0, 0] and glyphs[0].advance == max_w
    assert glyphs[3].rows == [0, 0, 0, 0]


def test_proportional_trim_from_mono_grid():
    # 4x2 cells: 'A' has ink in cols 2..3 (centred, blank on the left), 'B' is blank (space-like)
    a = mkfont.Glyph(rows=[0b00110000, 0b00100000], advance=4)
    b = mkfont.Glyph(rows=[0, 0], advance=4)
    out = mkfont.trim_proportional([a, b], max_width=4, spacing=1, first=65)
    assert out[0].rows == [0b11000000, 0b10000000]  # shifted left to drop blank cols
    assert out[0].advance == 3          # ink width 2 + spacing 1
    assert out[1].advance == 2          # blank keeps max(cell/2, 2)

    # 8x2 cell: ink only in column 7 (far right)
    c = mkfont.Glyph(rows=[0b00000001, 0], advance=8)
    out2 = mkfont.trim_proportional([c], max_width=8, spacing=1, first=65)
    assert out2[0].rows == [0b10000000, 0]
    assert out2[0].advance == 2         # ink width 1 + spacing 1


def test_png_grid_input(tmp_path):
    from PIL import Image
    # 2 cells of 3x2, first='A': A = all white, B = left column only
    img = Image.new("L", (6, 2), 0)
    for x in range(3):
        img.putpixel((x, 0), 255); img.putpixel((x, 1), 255)
    img.putpixel((3, 0), 255); img.putpixel((3, 1), 255)
    p = tmp_path / "sheet.png"
    img.save(p)
    glyphs, first, height, max_w = mkfont.load_png(p, cell=(3, 2), rng=(65, 66))
    assert (first, height, max_w) == (65, 2, 3)
    assert glyphs[0].rows == [0xE0, 0xE0]
    assert glyphs[1].rows == [0x80, 0x80]


def test_build_rejects_oversize():
    import pytest
    g = mkfont.Glyph(rows=[0] * 65, advance=1)
    with pytest.raises(ValueError):
        mkfont.build_pfn([g], 65, 65, 1, proportional=False)
