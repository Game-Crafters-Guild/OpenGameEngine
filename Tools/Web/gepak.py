"""The .gepak container the web Player unpacks into MEMFS.

A dist ships its files in two packs rather than as loose assets so a cold start
costs a fixed number of requests instead of one per file. The format is
deliberately minimal — an index of paths and extents, then the bytes — because
both ends are in this repo and the runtime reader must work with no zlib, no
allocator tricks and no seeking beyond a byte span already in memory.

Layout, little-endian throughout:

    0   char[8]  "GEPAK\\0\\0\\0"
    8   u32      formatVersion (1)
    12  u32      entryCount
    16  u64      indexBytes
    24  index    entryCount records, each:
                     u32 pathBytes
                     u8  path[pathBytes]
                     u64 dataOffset   (absolute, from the start of the file)
                     u64 dataBytes
        payload  the entry blobs, in index order

Paths are relative to the unpack root, '/'-separated, never absolute and never
containing a '..' component — the reader rejects anything else rather than
writing outside the root.

The C++ reader is Apps/Player/Source/WebPackReader.cpp; the two must be changed
together, and kPackFormatVersion there is this file's formatVersion.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

kMagic = b"GEPAK\0\0\0"
kFormatVersion = 1
kHeaderFormat = "<8sIIQ"
kHeaderSize = struct.calcsize(kHeaderFormat)

kFnv1a64OffsetBasis = 14695981039346656037
kFnv1a64Prime = 1099511628211
kUint64Mask = (1 << 64) - 1


def Fnv1a64(data: bytes) -> int:
    """FNV-1a 64, matching Engine/Modules/Types/Include/Types/Fnv1a.h.

    Integrity against a truncated or stale download, not against an adversary.
    """
    hashValue = kFnv1a64OffsetBasis
    for byte in data:
        hashValue = ((hashValue ^ byte) * kFnv1a64Prime) & kUint64Mask
    return hashValue


@dataclass(frozen=True)
class PackEntry:
    Path: str
    Data: bytes


def IsLegalEntryPath(path: str) -> bool:
    if not path or path.startswith("/") or "\\" in path:
        return False
    parts = path.split("/")
    return all(part not in ("", ".", "..") for part in parts)


def Pack(entries: list[PackEntry]) -> bytes:
    """Serialize entries into a pack image. Entry order is preserved."""
    for entry in entries:
        if not IsLegalEntryPath(entry.Path):
            raise ValueError(f"illegal pack entry path: {entry.Path!r}")

    encoded = [(entry.Path.encode("utf-8"), entry.Data) for entry in entries]
    indexBytes = sum(4 + len(path) + 8 + 8 for path, _ in encoded)
    payloadStart = kHeaderSize + indexBytes

    index = bytearray()
    payload = bytearray()
    for path, data in encoded:
        index += struct.pack("<I", len(path))
        index += path
        index += struct.pack("<QQ", payloadStart + len(payload), len(data))
        payload += data

    header = struct.pack(kHeaderFormat, kMagic, kFormatVersion, len(encoded), indexBytes)
    return bytes(header + index + payload)


def Unpack(image: bytes) -> list[PackEntry]:
    """Parse a pack image. Raises ValueError on anything malformed."""
    if len(image) < kHeaderSize:
        raise ValueError("pack is shorter than its header")
    magic, version, entryCount, indexBytes = struct.unpack_from(kHeaderFormat, image, 0)
    if magic != kMagic:
        raise ValueError("bad pack magic")
    if version != kFormatVersion:
        raise ValueError(f"unsupported pack format version {version}")

    entries: list[PackEntry] = []
    cursor = kHeaderSize
    indexEnd = kHeaderSize + indexBytes
    for _ in range(entryCount):
        (pathBytes,) = struct.unpack_from("<I", image, cursor)
        cursor += 4
        path = image[cursor:cursor + pathBytes].decode("utf-8")
        cursor += pathBytes
        dataOffset, dataBytes = struct.unpack_from("<QQ", image, cursor)
        cursor += 16
        if cursor > indexEnd:
            raise ValueError("pack index overruns its declared size")
        if not IsLegalEntryPath(path):
            raise ValueError(f"illegal pack entry path: {path!r}")
        if dataOffset + dataBytes > len(image):
            raise ValueError(f"pack entry {path!r} extends past end of file")
        entries.append(PackEntry(path, image[dataOffset:dataOffset + dataBytes]))
    return entries


def PackDirectory(root: Path, relativePaths: list[str]) -> bytes:
    """Pack the named files, read from `root`, keyed by their relative paths."""
    return Pack([PackEntry(path, (root / path).read_bytes())
                 for path in sorted(relativePaths)])
