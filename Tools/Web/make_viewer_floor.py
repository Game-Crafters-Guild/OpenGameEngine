#!/usr/bin/env python3
"""Write the model viewer's floor: a disc of radius 1 m facing +Y in two primitives. The middle, out
to kOpaqueRadius, is an opaque disc: what the model stands on and casts its contact shadow on.
Around it a ring fades from opaque to transparent by kClearRadius (a glTF BLEND material whose only
texture is the fade), so the floor has no edge for the camera to see. Neither part feeds a DDGI
volume's bounce yet: a mesh a page loads with scene.load feeds none (#3430).

    python Tools/Web/make_viewer_floor.py Apps/WebLibrary/examples/model-viewer/floor.glb

The output is deterministic: the same script writes the same bytes.
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import zlib
from pathlib import Path

kSegments = 96            # triangles around the disc
kTextureSize = 128        # the fade texture's width and height in pixels
kOpaqueRadius = 0.35      # of the disc's radius: fully opaque inside it
kClearRadius = 0.95       # of the disc's radius: fully transparent outside it
kAlbedo = 0.6             # linear base colour, a mid grey
kRoughness = 0.9


def Png(width: int, height: int, rgba: bytes) -> bytes:
    def Chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    rows = b"".join(b"\x00" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + Chunk(b"IHDR", header) + Chunk(b"IDAT", zlib.compress(rows, 9)) + Chunk(b"IEND", b"")


def FadeTexture() -> bytes:
    """White, with alpha 1 inside kOpaqueRadius falling smoothly to 0 at kClearRadius."""
    pixels = bytearray()
    for y in range(kTextureSize):
        for x in range(kTextureSize):
            r = math.hypot((x + 0.5) / kTextureSize * 2 - 1, (y + 0.5) / kTextureSize * 2 - 1)
            t = min(max((r - kOpaqueRadius) / (kClearRadius - kOpaqueRadius), 0.0), 1.0)
            pixels += bytes((255, 255, 255, round(255 * (1.0 - t * t * (3 - 2 * t)))))
    return Png(kTextureSize, kTextureSize, bytes(pixels))


def Rim(radius: float) -> list[tuple[float, float]]:
    """kSegments points on the circle of `radius`, x and z."""
    return [(radius * math.cos(2 * math.pi * i / kSegments), radius * math.sin(2 * math.pi * i / kSegments))
            for i in range(kSegments)]


def Vertices(points: list[tuple[float, float]]) -> tuple[list[float], list[float], list[float]]:
    """Positions on y = 0, normals (+Y) and texture coordinates (the unit disc onto the texture)."""
    positions, normals, uvs = [], [], []
    for x, z in points:
        positions += [x, 0.0, z]
        normals += [0.0, 1.0, 0.0]
        uvs += [0.5 + 0.5 * x, 0.5 + 0.5 * z]
    return positions, normals, uvs


def Disc() -> tuple[list[float], list[float], list[float], list[int]]:
    """The opaque middle: a fan about the centre out to kOpaqueRadius."""
    positions, normals, uvs = Vertices([(0.0, 0.0)] + Rim(kOpaqueRadius))
    indices = []
    for i in range(kSegments):
        indices += [0, 1 + (i + 1) % kSegments, 1 + i]   # counter-clockwise seen from +Y
    return positions, normals, uvs, indices


def Ring() -> tuple[list[float], list[float], list[float], list[int]]:
    """The fading ring from kOpaqueRadius to the disc's edge: inner vertex i, outer vertex kSegments + i."""
    positions, normals, uvs = Vertices(Rim(kOpaqueRadius) + Rim(1.0))
    indices = []
    for i in range(kSegments):
        inner, nextInner = i, (i + 1) % kSegments
        outer, nextOuter = kSegments + i, kSegments + nextInner
        indices += [inner, nextInner, outer, nextInner, nextOuter, outer]   # counter-clockwise seen from +Y
    return positions, normals, uvs, indices


def Glb() -> bytes:
    floats = lambda values: struct.pack(f"<{len(values)}f", *values)
    blobs, accessors, primitives = [], [], []
    for material, (positions, normals, uvs, indices) in enumerate((Disc(), Ring())):
        first = len(blobs)
        blobs += [floats(positions), floats(normals), floats(uvs), struct.pack(f"<{len(indices)}H", *indices)]
        count = len(positions) // 3
        accessors += [
            {"bufferView": first, "componentType": 5126, "count": count, "type": "VEC3", "min": [-1, 0, -1], "max": [1, 0, 1]},
            {"bufferView": first + 1, "componentType": 5126, "count": count, "type": "VEC3"},
            {"bufferView": first + 2, "componentType": 5126, "count": count, "type": "VEC2"},
            {"bufferView": first + 3, "componentType": 5123, "count": len(indices), "type": "SCALAR"}]
        primitives.append({"attributes": {"POSITION": first, "NORMAL": first + 1, "TEXCOORD_0": first + 2},
                           "indices": first + 3, "material": material})
    blobs.append(FadeTexture())
    binary, views = b"", []
    for blob in blobs:
        binary += b"\x00" * ((-len(binary)) % 4)
        views.append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(blob)})
        binary += blob
    binary += b"\x00" * ((-len(binary)) % 4)
    grey = [kAlbedo, kAlbedo, kAlbedo, 1.0]
    document = {
        "asset": {"version": "2.0", "generator": "Tools/Web/make_viewer_floor.py"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": "Floor"}],
        "meshes": [{"name": "Floor", "primitives": primitives}],
        "materials": [
            {"name": "Floor", "pbrMetallicRoughness": {"baseColorFactor": grey, "metallicFactor": 0.0,
                                                       "roughnessFactor": kRoughness}},
            {"name": "FloorRim", "alphaMode": "BLEND", "pbrMetallicRoughness": {
                "baseColorFactor": grey, "baseColorTexture": {"index": 0}, "metallicFactor": 0.0,
                "roughnessFactor": kRoughness}}],
        "textures": [{"source": 0, "sampler": 0}],
        "samplers": [{"magFilter": 9729, "minFilter": 9729, "wrapS": 33071, "wrapT": 33071}],
        "images": [{"bufferView": len(blobs) - 1, "mimeType": "image/png"}],
        "accessors": accessors, "bufferViews": views, "buffers": [{"byteLength": len(binary)}],
    }
    text = json.dumps(document, separators=(",", ":")).encode()
    text += b" " * ((-len(text)) % 4)
    total = 12 + 8 + len(text) + 8 + len(binary)
    return (struct.pack("<III", 0x46546C67, 2, total) + struct.pack("<II", len(text), 0x4E4F534A) + text
            + struct.pack("<II", len(binary), 0x004E4942) + binary)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("out", type=Path, help="the .glb to write")
    args = parser.parse_args()
    args.out.write_bytes(Glb())
    print(f"make_viewer_floor: {args.out} ({args.out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
