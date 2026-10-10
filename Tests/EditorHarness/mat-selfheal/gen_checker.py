#!/usr/bin/env python3
"""Generate Assets/Textures/checker.png — a procedural UV-checker texture for the
material-texture self-heal test. Fully original/first-party (no third-party art,
no license), reproducible: re-run this script to regenerate the texture.

    python3 gen_checker.py

Pure stdlib (zlib) PNG writer — no Pillow/ImageMagick needed.
"""
import os
import struct
import zlib

W = H = 256
CELL = 32  # checker cell size in pixels


def _chunk(ctype: bytes, data: bytes) -> bytes:
    body = ctype + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def build_png() -> bytes:
    raw = bytearray()
    for y in range(H):
        raw.append(0)  # per-scanline filter: none
        for x in range(W):
            checker = ((x // CELL) + (y // CELL)) & 1
            # Colourful, obviously-synthetic pattern: position-driven gradient,
            # inverted on alternating checker cells, so it reads clearly as a
            # texture (never mistakable for the default white) and shows
            # orientation. Black grid lines on cell borders.
            r = (x * 255) // W
            g = (y * 255) // H
            b = 210 if checker else 70
            if checker:
                r, g = 255 - r, 255 - g
            if x % CELL == 0 or y % CELL == 0:
                r = g = b = 20  # grid line
            raw += bytes((r & 0xFF, g & 0xFF, b & 0xFF))
    png = b"\x89PNG\r\n\x1a\n"
    png += _chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))  # 8-bit RGB
    png += _chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += _chunk(b"IEND", b"")
    return png


def main() -> None:
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "Assets", "Textures", "checker.png")
    with open(out, "wb") as f:
        f.write(build_png())
    print(f"wrote {out} ({os.path.getsize(out)} bytes, {W}x{H} RGB)")


if __name__ == "__main__":
    main()
