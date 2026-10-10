#!/usr/bin/env python3
"""Golden snapshot tests for the shader cook.

Cooks one representative shader per remediation class and compares the
WGSL against the committed snapshot in golden/, ignoring trailing whitespace. A diff means the toolchain
moved (naga/tint/glslc version bump) or a shader/include changed — both are
events a human should look at, not absorb silently. Regenerate deliberately
with --update after verifying the cause.
"""

from __future__ import annotations

import argparse
import difflib
import subprocess
import sys
import tempfile
from pathlib import Path

import shadercook

kRoot = Path(__file__).resolve().parent
kRepo = kRoot.parent.parent
kShaders = kRepo / "Engine/Modules/Rendering/Shaders"
kGolden = kRoot / "golden"
# CTest reports this exit code as SKIPPED (SKIP_RETURN_CODE).
kCannotRun = 125

# One per remediation class: plain compute, combined-sampler split (post fx),
# derivative hoist + uniformity (core lighting), lod-0 LUT taps (sky),
# constant-bound barrier walk + spec-const pin (skinning).
kCases = [
    "auto_exposure_histogram.comp",
    "bloom_blur_h.frag",
    "forwardplus_lighting.frag",
    "sky_render.frag",
    "animation_skinning.comp",
]


def NormalizeSnapshot(text: str) -> str:
    # Naga emits trailing spaces on some binding-attribute lines.
    return "\n".join(line.rstrip() for line in text.splitlines()) + "\n"


def Cook(source: Path, out_dir: Path) -> Path:
    result = subprocess.run(
        [sys.executable, str(kRoot / "shadercook.py"),
         "-D", "GE_COMPAT_PROFILE",
         "-I", str(kShaders / "Includes"),
         "-I", str(kShaders),
         "--out-dir", str(out_dir),
         str(source)],
        capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"cook failed for {source.name}:\n{result.stderr}")
    return out_dir / (source.stem + ".wgsl")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--update", action="store_true",
                        help="regenerate the snapshots from the current toolchain")
    args = parser.parse_args()

    # The cook needs naga; a clone that has not run toolchain/setup.py has none.
    if shadercook.FindTool("naga", None) is None:
        print("SKIP: naga not found - run Tools/ShaderCook/toolchain/setup.py to fetch it")
        return kCannotRun

    kGolden.mkdir(exist_ok=True)
    failures = []
    with tempfile.TemporaryDirectory() as tempdir:
        out_dir = Path(tempdir)
        for case in kCases:
            cooked = Cook(kShaders / case, out_dir)
            snapshot = kGolden / cooked.name
            if args.update:
                snapshot.write_text(NormalizeSnapshot(cooked.read_text()))
                print(f"updated {snapshot.relative_to(kRoot)}")
                continue
            if not snapshot.exists():
                failures.append(f"{case}: no snapshot (run --update once)")
                continue
            got = NormalizeSnapshot(cooked.read_text())
            want = NormalizeSnapshot(snapshot.read_text())
            if got != want:
                diff = "".join(difflib.unified_diff(
                    want.splitlines(keepends=True), got.splitlines(keepends=True),
                    fromfile=str(snapshot.name) + " (golden)",
                    tofile=str(cooked.name) + " (cooked)", n=2))
                failures.append(f"{case}: WGSL drifted\n{diff[:2000]}")
    if failures:
        print("GOLDEN FAILURES:\n" + "\n".join(failures), file=sys.stderr)
        return 1
    if not args.update:
        print(f"golden OK ({len(kCases)} snapshots match)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
