#!/usr/bin/env python3
"""Exercise the real WGSL cook and check its packaged binding metadata."""
import argparse
import importlib.util
import os
import re
from pathlib import Path
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shaderpkg-dir", type=Path, required=True)
    parser.add_argument("--shader-reflect", type=Path)
    parser.add_argument("--cook-script", type=Path,
                        default=Path(__file__).with_name("builtin_shader_cook.py"))
    args = parser.parse_args()
    if args.shader_reflect:
        os.environ["GE_SHADERREFLECT_EXECUTABLE"] = str(args.shader_reflect)
    spec = importlib.util.spec_from_file_location("builtin_cook", args.cook_script)
    cook = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cook)
    packages = {name: cook.kPackages.get(name, cook.kEditorPackages.get(name)) for name in (
        "volumetric_clouds_march", "ffx_dof_tile", "ffx_dof_dilate", "ffx_dof_composite",
        "ddgi_glossy_resolve_blur", "screen_space_shadow_prepare",
        "screen_space_shadow", "screen_space_shadow_resolve", "screen_space_shadow_match",
        # Cook only: atomics on values a loop carries out are a shape naga 29.x rejects.
        "depth_reduce", "depth_reduce_ms",
        # Cook only, through glslc and naga 29.0.4 (Tools/ShaderCook/toolchain/manifest.json):
        # ocean programs that read a name the compat profile compiles out.
        "ocean_spray_sim", "ocean_underwater", "ocean_underwater_shadowed",
        # Cook only: constructs naga 29.0.4 rejects, a size query of an arrayed storage
        # image (typed without its layer count) and a write-only storage buffer.
        "ocean_shadow_sim", "ocean_spray_cull")}
    assert all(packages.values()), packages
    try:
        cook.FindToolchain()
        if hasattr(cook, "FindShaderReflect"):
            cook.FindShaderReflect(args.shaderpkg_dir, args.shader_reflect)
    except RuntimeError as error:
        print(f"SKIP: {error}")
        return 125
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory)
        failures = cook.CookBuiltinPackages(args.shaderpkg_dir,
            cook.kRepoRoot / "Engine/Modules/Rendering/Shaders", output, packages=packages)
        if failures:
            for package, message in failures:
                print(f"FAIL {package}: {message}")
            return 1
        reflector = cook.FindShaderReflect(args.shaderpkg_dir, args.shader_reflect)

        def read(name):
            package = output / (name + ".shaderpkg")
            meta = cook.ReadShaderPkgMeta(reflector, package)
            wgsl = "\n".join(cook.ReadShaderPkgChunk(reflector, package, chunk["name"]).decode()
                             for chunk in cook.ListShaderPkgChunks(reflector, package)
                             if chunk["name"].endswith("-wgsl"))
            bindings = {b["name"]: b for group in meta["sets"] if group["set"] == 0
                        for b in group["bindings"]}
            return meta, bindings, wgsl
        errors = []
        for package in ("screen_space_shadow_prepare", "screen_space_shadow",
                        "screen_space_shadow_resolve", "screen_space_shadow_match"):
            if package not in cook.kPackages:
                errors.append(f"{package}: missing from Player's common cook set")
            _, bindings, wgsl = read(package)
            if bindings["uDepth"].get("image", {}).get("filtered", False):
                errors.append(f"{package}: depth guide requires optional float filtering")
            if package in ("screen_space_shadow_prepare", "screen_space_shadow"):
                if bindings["Visibility"].get("readOnly", False):
                    errors.append(f"{package}: visibility buffer must be writable")
            elif not bindings["Visibility"].get("readOnly", False):
                errors.append(f"{package}: visibility buffer must be read-only")
        _, bindings, wgsl = read("screen_space_shadow_match")
        if not bindings["uResolved"]["image"].get("unsignedInteger", False):
            errors.append("contact match: packed mask lost its unsigned texture type")
        if not re.search(r"texture_2d\s*<\s*u32\s*>", wgsl):
            errors.append("contact match: WGSL must point-read an unsigned mask")
        if not re.search(r"texture_storage_2d\s*<\s*r32uint\s*,\s*write\s*>", wgsl):
            errors.append("contact match: WGSL must write R32_UINT")
        for package, outputName in (("ffx_dof_tile", "uTileCoc"),
                                    ("ffx_dof_dilate", "uDilatedCoc")):
            _, bindings, wgsl = read(package)
            # Shader reflection must agree with the exact storage declaration
            # consumed by WebGPU, rather than the native package's RG16F.
            if not re.search(r"texture_storage_2d\s*<\s*rgba16float\s*,\s*write\s*>", wgsl):
                errors.append(f"{package}: WGSL does not declare RGBA16F storage")
            composite = read("ffx_dof_composite")[1]["uOutput"]["image"]["format"]
            if bindings[outputName]["image"].get("format") != composite:
                errors.append(f"{package}: metadata disagrees with its RGBA16F output")
        meta, bindings, wgsl = read("volumetric_clouds_march")
        for name in ("ShapeNoiseMinMax", "DetailNoiseMinMax"):
            if not any(name in binding for binding in bindings):
                errors.append(f"cloud march: metadata lacks {name}")
            binding = bindings.get(name, {}).get("binding")
            if binding is None or not re.search(
                    rf"@group\(0\)\s*@binding\({binding}\)\s*var\s*<\s*storage\s*(?:,\s*read\s*)?>", wgsl):
                errors.append(f"cloud march: WGSL lacks read-only storage for {name}")
        if not meta["pushConstants"]:
            errors.append("cloud march: metadata lost the engine push-constant declaration")
        _, bindings, wgsl = read("ddgi_glossy_resolve_blur")
        for name in ("uRoughIn", "uGlossyIn", "uIrradianceIn", "uHistoryIrr"):
            if not bindings[name].get("image", {}).get("filtered", False):
                errors.append(f"DDGI blur: {name} lost linear filtering")
        if bindings["uViewDepth"].get("image", {}).get("filtered", False):
            errors.append("DDGI blur: R32F depth guide requires optional float filtering")
        if "textureSampleLevel" not in wgsl or "textureLoad" not in wgsl:
            errors.append("DDGI blur: WGSL must filter colour and point-read depth")
        # Unoptimized SPIR-V retains opaque function parameters. Reflection
        # must follow every call site through nested/reordered sampler arguments,
        # without marking a texture used only by a fetch helper as filtered.
        helper_source = output / "sampler-helper.comp"
        helper_source.write_text("""#version 450
layout(local_size_x=1) in;
layout(set=0,binding=0) uniform sampler2D ColourA;
layout(set=0,binding=1) uniform sampler2D ColourB;
layout(set=0,binding=2) uniform sampler2D Depth;
layout(set=0,binding=3,rgba16f) uniform writeonly image2D Output;
vec4 filtered(sampler2D tex) { return textureLod(tex, vec2(0.5), 0.0); }
vec4 nested(sampler2D a, sampler2D b) { return filtered(b) + filtered(a); }
vec4 fetched(sampler2D tex) { return texelFetch(tex, ivec2(0), 0); }
void main() {
    imageStore(Output, ivec2(0), nested(ColourB, ColourA) + fetched(Depth));
}
""", encoding="utf-8")
        helper_spv = output / "sampler-helper.spv"
        glslc = cook.FindToolchain()[0]
        compiled = cook.shadercook.Run([glslc, "--target-env=vulkan1.1",
            "-fshader-stage=compute", helper_source, "-o", helper_spv])
        if compiled.returncode:
            errors.append("helper compile: " + compiled.stderr)
        else:
            reflected = cook.shadercook.Run([reflector, "--cs", helper_spv,
                "--out", output / "sampler-helper.shaderpkg"])
            if reflected.returncode:
                errors.append("helper reflection: " + reflected.stderr)
            else:
                _, helper_bindings, _ = read("sampler-helper")
                for name, expected in (("ColourA", True), ("ColourB", True), ("Depth", False)):
                    actual = helper_bindings[name].get("image", {}).get("filtered", False)
                    if actual != expected:
                        errors.append(f"sampler helper: {name} filtered={actual}, expected {expected}")
        for error in errors:
            print("FAIL:", error)
        print(f"WebBuiltinShaderMetadata: {len(packages)} packages + nested sampler helper cooked; {len(errors)} binding errors")
        return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
