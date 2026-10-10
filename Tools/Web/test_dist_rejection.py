#!/usr/bin/env python3
"""Assert the web Player refuses malformed export dists.

WebDistLoader and WebPackReader parse bytes fetched over HTTP, so their
rejection paths are the part worth testing: a happy-path export exercises none
of them, and a hole here writes attacker-chosen files into the Player's MEMFS
or boots it on content that is not what the manifest describes.

Each case builds a dist whose player template is symlinked (the wasm is ~575 MB
and nothing here executes past the manifest), serves it, and requires the
Player to name the fault and refuse to boot.

    python3 Tools/Web/test_dist_rejection.py --template build/wasm-debug/bin

Needs the same headless Chrome browser_gate.py drives.
"""

from __future__ import annotations

import argparse
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gepak  # noqa: E402

kRepoRoot = Path(__file__).resolve().parents[2]
kTemplateFiles = ("WebPlayer.html", "WebPlayer.js", "WebPlayer.wasm")


def WriteDist(root: Path, template: Path, packImage: bytes, manifestPack: dict) -> None:
    for name in kTemplateFiles:
        (root / name).symlink_to((template / name).resolve())
    (root / "content.gepak").write_bytes(packImage)
    (root / "player-manifest.json").write_text(json.dumps({
        "schemaVersion": 1,
        "project": "RejectionCase",
        "entryScene": "x",
        "packs": [{"role": "content", "file": "content.gepak", **manifestPack}],
        "managedAssemblies": [],
    }, indent=2))


def EscapingPackImage() -> bytes:
    """A pack whose entry path walks out of the unpack root.

    Hand-built: gepak.Pack refuses to write this, which is exactly why the
    reader cannot be trusted to only ever see well-formed input.
    """
    path = b"../escaped.txt"
    payload = b"owned"
    dataOffset = gepak.kHeaderSize + 4 + len(path) + 16
    index = struct.pack("<I", len(path)) + path + struct.pack("<QQ", dataOffset, len(payload))
    return struct.pack(gepak.kHeaderFormat, gepak.kMagic, 1, 1, len(index)) + index + payload


def NotAPackImage() -> bytes:
    """Header-sized bytes carrying the wrong magic."""
    return b"NOTGEPAK" + bytes(gepak.kHeaderSize - 8)


def Cases() -> list[tuple[str, bytes, dict, str]]:
    """(name, pack image, manifest pack fields, expected substring)."""
    escaping = EscapingPackImage()
    valid = gepak.Pack([gepak.PackEntry("game.config", b'{"gameName":"Case"}')])
    validFields = {"bytes": len(valid), "entries": 1,
                   "fnv1a64": f"0x{gepak.Fnv1a64(valid):016x}"}
    return [
        ("path escapes the unpack root", escaping,
         {"bytes": len(escaping), "entries": 1, "fnv1a64": f"0x{gepak.Fnv1a64(escaping):016x}"},
         "escapes the unpack root"),
        ("content hash does not match the manifest", valid,
         {**validFields, "fnv1a64": "0xdeadbeefdeadbeef"},
         "does not match the manifest"),
        ("byte count does not match the manifest", valid,
         {**validFields, "bytes": len(valid) + 1},
         "arrived"),
        ("entry count does not match the manifest", valid,
         {**validFields, "entries": 7},
         "manifest declares"),
        # Long enough to reach the magic check — a shorter blob is refused one
        # step earlier, by the header-length bound.
        ("pack is not a .gepak", NotAPackImage(),
         {"bytes": len(NotAPackImage()), "entries": 1,
          "fnv1a64": f"0x{gepak.Fnv1a64(NotAPackImage()):016x}"},
         "bad magic"),
        ("pack is shorter than a header", b"tiny",
         {"bytes": 4, "entries": 1, "fnv1a64": f"0x{gepak.Fnv1a64(b'tiny'):016x}"},
         "shorter than its header"),
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--template", type=Path, required=True,
                        help="directory holding the built WebPlayer.{html,js,wasm}")
    parser.add_argument("--port", type=int, default=8131)
    parser.add_argument("--seconds", type=float, default=20.0)
    args = parser.parse_args()

    gate = kRepoRoot / "Tools" / "Web" / "browser_gate.py"
    failures = []
    for index, (name, image, fields, expected) in enumerate(Cases()):
        with tempfile.TemporaryDirectory(prefix="ge_dist_rejection_") as tempDir:
            root = Path(tempDir) / "dist"
            root.mkdir()
            WriteDist(root, args.template, image, fields)
            result = subprocess.run(
                [sys.executable, str(gate), "--root", str(root), "--page", "WebPlayer.html",
                 "--seconds", str(args.seconds), "--port", str(args.port + index)],
                capture_output=True, text=True)
            transcript = result.stdout + result.stderr
            if expected in transcript and "no loadable export dist" in transcript:
                print(f"  ok   refused: {name}")
            else:
                failures.append(name)
                print(f"  FAIL not refused as expected: {name}", file=sys.stderr)
                print(f"       wanted '{expected}' in the console transcript", file=sys.stderr)

    print(f"dist rejection: {len(Cases()) - len(failures)}/{len(Cases())} cases refused")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
