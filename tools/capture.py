#!/usr/bin/env python3
"""Capture a real screenshot from the Spotify CYD over WiFi.

Usage: python3 capture.py [host] [outfile]
Needs Pillow. Saves a 2x-scaled PNG (default docs/screen.png).
"""
import sys
import urllib.request
from pathlib import Path

from PIL import Image

HOST = sys.argv[1] if len(sys.argv) > 1 else "spotify-cyd.local"
OUT = Path(sys.argv[2]) if len(sys.argv) > 2 else \
    Path(__file__).resolve().parent.parent / "docs" / "screen.png"
W, H = 320, 240

raw = urllib.request.urlopen(f"http://{HOST}/screen", timeout=60).read()
assert len(raw) == W * H * 2, f"got {len(raw)} bytes"
img = Image.new("RGB", (W, H))
pix = img.load()
for i in range(W * H):
    v = (raw[i * 2] << 8) | raw[i * 2 + 1]
    r = ((v >> 11) & 0x1F) << 3
    g = ((v >> 5) & 0x3F) << 2
    b = (v & 0x1F) << 3
    pix[i % W, i // W] = (r, g, b)
img = img.resize((W * 2, H * 2), Image.NEAREST)
OUT.parent.mkdir(exist_ok=True)
img.save(OUT)
print(f"saved {OUT}")
