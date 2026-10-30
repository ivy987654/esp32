"""Wrap a TTF for the board's font partition: "FONT" + u32 length (LE) + ttf bytes.

python tools/mkfontbin.py font/font.ttf font/font.bin
"""
import struct
import sys

ttf = open(sys.argv[1], "rb").read()
with open(sys.argv[2], "wb") as f:
    f.write(b"FONT" + struct.pack("<I", len(ttf)) + ttf)
print(f"{sys.argv[2]}: {len(ttf) + 8} bytes")
