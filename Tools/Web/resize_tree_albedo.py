#!/usr/bin/env python3
"""Resize Tree Generator albedo maps for the web editor pack.

Desktop keeps the authored 1024 maps (and the extra PBR maps) in
Packages/eztree. The wasm pack cannot: Cloudflare's static cap is 25 MiB
gzip, and packing 1024 albedo three times already blows it. This writes 512
copies into a build directory; CMake preloads those at the same MEMFS names
the runtime looks up, so no asset path change.

    python3 Tools/Web/resize_tree_albedo.py --out build/wasm-release/WebEditorTreeAlbedo512
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.stderr.write("resize_tree_albedo.py needs Pillow (pip install pillow)\n")
    sys.exit(1)

kRepoRoot = Path(__file__).resolve().parents[2]
kSourceRoot = kRepoRoot / "Packages" / "eztree" / "Assets" / "Textures" / "EZTree"
kSize = 512
kJpegQuality = 85

kLeafAlbedo = (
    "oak_color.png",
    "ash_color.png",
    "aspen_color.png",
    "pine_color.png",
)
kBarkAlbedo = (
    "oak_color_1k.jpg",
    "birch_color_1k.jpg",
    "pine_color_1k.jpg",
    "willow_color_1k.jpg",
)


def ResizeAlbedo(src: Path, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    with Image.open(src) as image:
        image.load()
        if image.size != (kSize, kSize):
            image = image.resize((kSize, kSize), Image.Resampling.LANCZOS)
        suffix = src.suffix.lower()
        if suffix in (".jpg", ".jpeg"):
            image.convert("RGB").save(dest, "JPEG", quality=kJpegQuality, optimize=True)
        else:
            image.save(dest, "PNG", optimize=True)


def Main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path,
                        help="Build directory to write leaves/ and bark/ into")
    args = parser.parse_args()
    outDir = args.out.resolve()

    jobs = [(kSourceRoot / "leaves" / name, outDir / "leaves" / name) for name in kLeafAlbedo]
    jobs += [(kSourceRoot / "bark" / name, outDir / "bark" / name) for name in kBarkAlbedo]

    missing = [str(src) for src, _ in jobs if not src.is_file()]
    if missing:
        sys.stderr.write("resize_tree_albedo.py: missing source albedo:\n")
        sys.stderr.write("\n".join(missing) + "\n")
        return 1

    for src, dest in jobs:
        ResizeAlbedo(src, dest)
        print(f"  {dest.relative_to(outDir)}  {dest.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(Main())
