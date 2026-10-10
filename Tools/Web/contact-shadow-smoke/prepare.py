#!/usr/bin/env python3
"""Cook the production contact-shadow packages into a standalone WebGPU check.

Run with --shaderpkg-dir <native-build>/Shaders --out /tmp/contact-shadow-smoke,
serve that output with Tools/Web/serve.py, then click Run shader tests.
The fixture projects light (-4, 0, 0, -1) into a 257 x 97 viewport; its dispatches
come from BuildScreenSpaceShadowDispatches, also covered by the native GPU tests.
"""
import argparse
import json
from pathlib import Path
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import builtin_shader_cook as cook


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shaderpkg-dir", type=Path, required=True)
    parser.add_argument("--shader-reflect", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    names = ("screen_space_shadow_prepare", "screen_space_shadow",
             "screen_space_shadow_resolve", "screen_space_shadow_match")
    packages = {name: cook.kPackages[name] for name in names}
    args.out.mkdir(parents=True, exist_ok=True)
    failures = cook.CookBuiltinPackages(args.shaderpkg_dir,
        cook.kRepoRoot / "Engine/Modules/Rendering/Shaders", args.out,
        packages=packages, shaderReflect=args.shader_reflect)
    if failures:
        for name, error in failures:
            print(f"FAIL {name}: {error}")
        return 1
    reflector = cook.FindShaderReflect(args.shaderpkg_dir, args.shader_reflect)
    metadata = {}
    for name in names:
        package = args.out / (name + ".shaderpkg")
        metadata[name] = cook.ReadShaderPkgMeta(reflector, package)
        (args.out / (name + ".wgsl")).write_bytes(cook.ReadShaderPkgChunk(reflector, package, "cs-wgsl"))
    (args.out / "metadata.json").write_text(json.dumps(metadata, indent=2))
    for name in ("index.html", "dispatch.json"):
        shutil.copy2(Path(__file__).with_name(name), args.out / name)
    print(f"Contact shadow WebGPU check: {args.out / 'index.html'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
