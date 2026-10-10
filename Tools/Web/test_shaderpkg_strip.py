#!/usr/bin/env python3
"""Check ShaderReflect --strip-spirv-with-wgsl through the web cook's wrapper.

A package cooked for the web loses the SPIR-V of every stage that carries WGSL; a package
with WGSL for some stages keeps the SPIR-V of the others; a package with no WGSL (a desktop
one) is left byte for byte as it was.

    python3 Tools/Web/test_shaderpkg_strip.py --shaderpkg-dir build/vs2026-x64-local/Shaders

Exit 0 = pass, 1 = fail, 125 = the WGSL toolchain or ShaderReflect is missing.
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import builtin_shader_cook  # noqa: E402

kPackage = "tonemap"


def ChunkNames(reflector: Path, path: Path) -> list[str]:
    return [chunk["name"] for chunk in builtin_shader_cook.ListShaderPkgChunks(reflector, path)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--shaderpkg-dir", type=Path, required=True, help="a desktop build's Shaders/ directory")
    parser.add_argument("--shader-reflect", type=Path, help="the ShaderReflect to test (default: beside the build)")
    args = parser.parse_args()
    try:
        builtin_shader_cook.FindToolchain()
        reflector = builtin_shader_cook.FindShaderReflect(args.shaderpkg_dir, args.shader_reflect)
    except RuntimeError as error:
        print(f"SKIP: {error}")
        return 125

    with tempfile.TemporaryDirectory() as directory:
        out = Path(directory)
        failures = builtin_shader_cook.CookBuiltinPackages(
            args.shaderpkg_dir, builtin_shader_cook.kRepoRoot / "Engine/Modules/Rendering/Shaders", out / "web",
            packages={kPackage: builtin_shader_cook.kPackages[kPackage]})
        if failures:
            print(f"FAIL: the {kPackage} cook failed: {failures}")
            return 1
        web = out / "web" / f"{kPackage}.shaderpkg"
        desktop = out / f"{kPackage}.desktop.shaderpkg"
        shutil.copy2(args.shaderpkg_dir / f"{kPackage}.shaderpkg", desktop)
        desktopBytes = desktop.read_bytes()
        # WGSL for the vertex stage only: the fragment stage's SPIR-V must stay.
        partial = out / f"{kPackage}.partial.shaderpkg"
        shutil.copy2(desktop, partial)
        vertexWgsl = out / "vs.wgsl"
        vertexWgsl.write_bytes(builtin_shader_cook.ReadShaderPkgChunk(reflector, web, "vs-wgsl"))
        builtin_shader_cook.AppendShaderPkgChunks(reflector, partial, [("vs-wgsl", vertexWgsl)])
        before = ChunkNames(reflector, web)

        builtin_shader_cook.StripShaderPkgSpirv(reflector, [web, desktop, partial])

        after = ChunkNames(reflector, web)
        expected = [name for name in before if not (name.endswith("-spv") and name[:-4] + "-wgsl" in before)]
        problems = []
        if not any(name.endswith("-spv") for name in before) or after != expected:
            problems.append(f"web package chunks {before} became {after}, expected {expected}")
        partialChunks = ChunkNames(reflector, partial)
        if "vs-spv" in partialChunks or "fs-spv" not in partialChunks or "vs-wgsl" not in partialChunks:
            problems.append(f"a package with WGSL for its vertex stage only kept {partialChunks}")
        if desktop.read_bytes() != desktopBytes:
            problems.append("a package with no WGSL was rewritten")
        builtin_shader_cook.ReadShaderPkgMeta(reflector, web)
    for problem in problems:
        print(f"FAIL: {problem}")
    print(f"test_shaderpkg_strip: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
