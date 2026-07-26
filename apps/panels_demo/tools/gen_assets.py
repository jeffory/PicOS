#!/usr/bin/env python3
"""Generate placeholder art and audio for the panels_demo comic.

Deliberately dependency-light: Pillow for PNGs, stdlib wave/math for WAVs.
Re-run from apps/panels_demo/: python3 tools/gen_assets.py
"""

import math
import os
import struct
import wave

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
IMG = os.path.normpath(os.path.join(HERE, "..", "images"))
AUD = os.path.normpath(os.path.join(HERE, "..", "audio"))
os.makedirs(IMG, exist_ok=True)
os.makedirs(AUD, exist_ok=True)

W = 320  # screen width; layer art may exceed panel height for parallax

# Magenta rgb(255, 0, 254) is the SDK's transparent-key convention
# (pure 255,0,255 maps to RGB565 0xF81F which is fine too; we follow the
# nonogram note that transparent_color==0 means "disabled").
KEY = (255, 0, 254)


def save(img, name):
    path = os.path.join(IMG, name)
    img.save(path)
    print(f"wrote {path} ({img.width}x{img.height})")


def vertical_gradient(size, top, bottom):
    img = Image.new("RGB", size)
    px = img.load()
    w, h = size
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(int(a + (b - a) * t) for a, b in zip(top, bottom))
        for x in range(w):
            px[x, y] = c
    return img


def seq1_parallax():
    """Three layers for the scrolling parallax sequence. Backgrounds are taller
    than the 480px panel so slow layers always have art under the fast ones."""
    # Far: night sky, tall
    sky = vertical_gradient((W, 700), (8, 8, 40), (40, 12, 60))
    d = ImageDraw.Draw(sky)
    import random
    rng = random.Random(7)
    for _ in range(140):
        x, y = rng.randrange(W), rng.randrange(700)
        d.point((x, y), fill=(220, 220, 255))
    save(sky, "s1_sky.png")

    # Mid: skyline silhouette on transparent key
    mid = Image.new("RGB", (W, 620), KEY)
    d = ImageDraw.Draw(mid)
    rng = random.Random(21)
    x = 0
    while x < W:
        bw = rng.randrange(28, 64)
        bh = rng.randrange(160, 380)
        d.rectangle([x, 620 - bh, x + bw, 620], fill=(24, 28, 46))
        for wy in range(620 - bh + 10, 610, 18):
            for wx in range(x + 6, x + bw - 6, 14):
                if rng.random() < 0.35:
                    d.rectangle([wx, wy, wx + 5, wy + 8], fill=(255, 214, 120))
        x += bw + rng.randrange(2, 10)
    save(mid, "s1_city.png")

    # Near: foreground railing, fast parallax
    near = Image.new("RGB", (W, 560), KEY)
    d = ImageDraw.Draw(near)
    for y in range(80, 560, 120):
        d.rectangle([0, y, W, y + 16], fill=(60, 70, 90))
        for x in range(10, W, 40):
            d.rectangle([x, y - 40, x + 10, y], fill=(50, 58, 76))
    save(near, "s1_rail.png")


def seq2_advance():
    """Panel art plus a small 'hero' that the animate keyframe slides in."""
    bg = vertical_gradient((W, 320), (30, 12, 12), (70, 24, 20))
    d = ImageDraw.Draw(bg)
    d.polygon([(0, 320), (110, 140), (200, 320)], fill=(50, 20, 24))
    d.polygon([(140, 320), (250, 100), (330, 320)], fill=(42, 16, 20))
    save(bg, "s2_bg.png")

    hero = Image.new("RGB", (64, 96), KEY)
    d = ImageDraw.Draw(hero)
    d.ellipse([16, 4, 48, 36], fill=(240, 200, 160))            # head
    d.rectangle([20, 36, 44, 78], fill=(60, 90, 160))           # body
    d.rectangle([20, 78, 30, 96], fill=(40, 40, 60))            # legs
    d.rectangle([34, 78, 44, 96], fill=(40, 40, 60))
    save(hero, "s2_hero.png")

    alert = Image.new("RGB", (96, 48), KEY)
    d = ImageDraw.Draw(alert)
    d.polygon([(8, 40), (48, 4), (88, 40)], fill=(255, 210, 60))
    d.rectangle([44, 14, 52, 30], fill=(30, 20, 0))
    d.rectangle([44, 33, 52, 38], fill=(30, 20, 0))
    save(alert, "s2_alert.png")


def seq3_auto():
    """Frame array for the auto-advance sequence: a sunrise in 3 states."""
    for i, (top, bottom, sun_y) in enumerate([
        ((10, 10, 46), (60, 30, 70), 300),
        ((40, 24, 80), (200, 90, 60), 220),
        ((90, 130, 200), (255, 190, 120), 130),
    ], start=1):
        img = vertical_gradient((W, 360), top, bottom)
        d = ImageDraw.Draw(img)
        d.ellipse([120, sun_y, 200, sun_y + 80], fill=(255, 230, 150))
        d.rectangle([0, 320, W, 360], fill=(20, 30, 20))
        save(img, f"s3_dawn{i}.png")


def seq5_credits():
    stars = Image.new("RGB", (W, 360), (4, 4, 12))
    d = ImageDraw.Draw(stars)
    import random
    rng = random.Random(99)
    for _ in range(120):
        x, y = rng.randrange(W), rng.randrange(360)
        d.point((x, y), fill=(200, 200, 230))
    save(stars, "s5_stars.png")


def tone_wav(name, freq, ms, rate=22050, vol=0.5, sweep=None):
    """Small mono 16-bit WAV; sweep=(f0,f1) makes a chirp."""
    n = int(rate * ms / 1000)
    frames = bytearray()
    for i in range(n):
        t = i / rate
        f = freq
        if sweep:
            f = sweep[0] + (sweep[1] - sweep[0]) * (i / n)
        env = min(1.0, (n - i) / (n * 0.3), i / (rate * 0.005) if i else 0)
        s = int(32767 * vol * env * math.sin(2 * math.pi * f * t))
        frames += struct.pack("<h", s)
    path = os.path.join(AUD, name)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames))
    kb = os.path.getsize(path) / 1024
    print(f"wrote {path} ({kb:.1f} KB)")
    assert kb < 64, f"{name} exceeds the 64KB sampleplayer cap"


def bgm_wav(name, ms=8000, rate=11025, vol=0.28):
    """Looping pad for the fileplayer (streams from SD, no 64KB cap)."""
    chord = [110.0, 164.81, 220.0, 329.63]
    n = int(rate * ms / 1000)
    frames = bytearray()
    for i in range(n):
        t = i / rate
        s = sum(math.sin(2 * math.pi * f * t + k) for k, f in enumerate(chord))
        lfo = 0.75 + 0.25 * math.sin(2 * math.pi * t / 4.0)
        frames += struct.pack("<h", int(32767 * vol / len(chord) * lfo * s))
    path = os.path.join(AUD, name)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames))
    print(f"wrote {path} ({os.path.getsize(path)/1024:.1f} KB)")


if __name__ == "__main__":
    seq1_parallax()
    seq2_advance()
    seq3_auto()
    seq5_credits()
    tone_wav("chime.wav", 880, 350, sweep=(660, 1320))
    bgm_wav("bgm.wav")
    print("done")
