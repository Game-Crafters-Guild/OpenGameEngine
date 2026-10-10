#!/usr/bin/env python3
"""Build opengine-core.gepak: the engine's own content the web library's module fetches.

ge_create (Apps/WebLibrary/Source/AbiLifecycle.cpp) fetches opengine-core.gepak from beside
the module's glue and unpacks it at the module's file system root "/", where the module runs
with its asset root at /Assets and its workspace at /.workspace. This docstring is the one
definition of what the pack holds; build_engine_pack writes exactly this, and
test_engine_pack.py checks a built pack against the list below (one pattern per line, `*`
within a path segment, `**` across segments, `!` marking a pattern at least one file must
match):

Layout:
    Assets/RenderPipelines/Web.rendergraph !   the render pipeline ge_create loads, the
                                               library's own (Apps/WebLibrary/Assets)
    Assets/Shaders/Adapters/** !               the engine GLSL the material pipeline composes
    Assets/Shaders/Includes/** !               against and hashes to address the cooked
    Assets/Shaders/Particles/** !              variant cache (export_web_player.kEngineShaderDirs)
    Assets/Shaders/Surfaces/** !
    Assets/Shaders/GPUFogParticles/Surfaces/** ! package shader mounts
    Assets/Shaders/CBT/** !                     (export_web_player.kPackageShaderMounts)
    Assets/Shaders/TerrainGrass/** !
    Assets/Shaders/EZTree/** !
    Assets/Shaders/VertexModifiers/** !
    Assets/Shaders/*.shaderpkg !               the built-in shader packages, each with its WGSL
    Assets/Shaders/*.wgsl !                    chunks, and the bare-stage WGSL files the
    Assets/Shaders/**/*.wgsl                   pipeline's sky lighting (IBL) loads by name
                                               (builtin_shader_cook.py)
    Assets/Shaders/**/*.shaderpkg
    Assets/Fonts/Roboto-Regular.ttf !          the UI fallback font: a page has no system fonts
    Assets/SkeletonProfiles/*.profile.json !   the engine's skeleton profiles (Assets/SkeletonProfiles):
                                               every model load resolves the humanoid one
    Assets/Textures/Sky/MoonPhases/moon_full.png ! the moon the sky pass draws (SkyRenderer.h)
    .workspace/.Cache/Shaders/** !             the material variant cache, cooked by
                                               MaterialVariantCook --web --verify over the seed
                                               project (the engine's default PBR and the
                                               synthetic documents included)

Every .shaderpkg in the pack carries its stages as WGSL and keeps SPIR-V only for a stage
the cook gave no WGSL, because a browser reads nothing else.

The seed project is Tools/Web/smoke-project, the web Player's smoke project: its materials
are cooked beside the engine's own documents. The pipeline is the library's: the web
export's compatibility-profile graph (the seed project's Web.rendergraph) with the sky pass
and the sky's image-based lighting added, because a page's default world has a sky and no
other light on the model's shaded side, and with the desktop default pipeline's
(ForwardPlus.rendergraph) sun glare, bloom and auto exposure, whose
packages (builtin_shader_cook.kLibraryPackages) the pack cooks on top of the web export's.

    python3 Tools/Web/engine_pack.py --out opengine-core.gepak --build-tree build/vs2026-x64-local
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import builtin_shader_cook  # noqa: E402
import export_web_player  # noqa: E402
import gepak  # noqa: E402

kRepoRoot = Path(__file__).resolve().parents[2]
kSeedProject = kRepoRoot / "Tools" / "Web" / "smoke-project"
kPipeline = Path("Assets") / "RenderPipelines" / "Web.rendergraph"
kPipelineSource = kRepoRoot / "Apps" / "WebLibrary" / kPipeline
# The stage shaders the IBLGen pass loads by bare name: the BRDF LUT and the sky capture chain,
# whose cube faces draw with the full-screen vertex stage.
kIblStageShaders = ("brdf_lut.comp", "fullscreen_noinput.vert", "sky_capture_cube.frag",
                    "sky_diffuse_convolve.comp", "sky_env_mip_downsample.comp", "sky_specular_prefilter.comp")
kEngineShaders = kRepoRoot / "Engine" / "Modules" / "Rendering" / "Shaders"
kShaderCook = kRepoRoot / "Tools" / "ShaderCook" / "shadercook.py"
kFont = kRepoRoot / export_web_player.kFallbackFont
kSkeletonProfiles = Path("Assets") / "SkeletonProfiles"
kMoonTexture = Path("Assets") / "Textures" / "Sky" / "MoonPhases" / "moon_full.png"
kToolConfigs = ("DebugFast", "Release", "RelWithDebInfo", "Debug")


class EnginePackError(Exception):
    """An engine pack that cannot be built as the layout defines it."""


def Layout() -> list[tuple[str, bool]]:
    """The docstring's layout: (pattern, required) per line."""
    lines = __doc__.split("Layout:\n", 1)[1].split("\n\n", 1)[0].splitlines()
    layout = []
    for line in lines:
        fields = line.split()
        if not fields or "/" not in fields[0] and not fields[0].startswith("."):
            continue
        layout.append((fields[0], len(fields) > 1 and fields[1] == "!"))
    return layout


def FindMaterialCook(buildTree: Path) -> Path:
    for config in kToolConfigs:
        for name in ("MaterialVariantCook.exe", "MaterialVariantCook"):
            candidate = buildTree / "bin" / config / "Tools" / name
            if candidate.is_file():
                return candidate
    raise EnginePackError(f"{buildTree} has no MaterialVariantCook: build that target in it "
                          "(configure with -DBUILD_TOOLS=ON).")


def build_engine_pack(out_path: Path, build_tree: Path, seed_project: Path = kSeedProject) -> list[str]:
    """Write the engine pack the module docstring defines to `out_path`.

    `build_tree` is a desktop build of this checkout with CompileShaderPkgs and
    MaterialVariantCook built: its Shaders/ directory holds the packages the WGSL cook
    transcodes, and its MaterialVariantCook cooks the variant cache. `seed_project` provides
    the materials cooked beside the engine's own. Any cook failure raises. Returns the pack's
    paths, sorted.
    """
    out_path = Path(out_path)
    build_tree = Path(build_tree)
    seed_project = Path(seed_project)
    shaderpkgDir = build_tree / "Shaders"
    if not any(shaderpkgDir.glob("*.shaderpkg")):
        raise EnginePackError(f"{shaderpkgDir} holds no .shaderpkg: build CompileShaderPkgs in {build_tree}.")
    materialCook = FindMaterialCook(build_tree)

    staging = Path(tempfile.mkdtemp(prefix="engine_pack_"))
    try:
        export_web_player.CopyEngineRuntimeAssets(kEngineShaders, kFont, staging)
        (staging / kPipeline).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(kPipelineSource, staging / kPipeline)
        shutil.copytree(kRepoRoot / kSkeletonProfiles, staging / kSkeletonProfiles)
        (staging / kMoonTexture).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(kRepoRoot / kMoonTexture, staging / kMoonTexture)

        failures = builtin_shader_cook.CookBuiltinPackages(
            shaderpkgDir, kEngineShaders, staging / export_web_player.kShaderAssetDir,
            packages=builtin_shader_cook.kPackages | builtin_shader_cook.kLibraryPackages)
        for packageName, error in failures:
            print(f"engine_pack: FAILED {packageName}: {error.splitlines()[0] if error else ''}", file=sys.stderr)
        failures += builtin_shader_cook.CookStageShaders(
            kEngineShaders, staging / export_web_player.kShaderAssetDir,
            stages={name: builtin_shader_cook.kEditorStageShaders[name] for name in kIblStageShaders})
        if failures:
            failed = sorted({name for name, _ in failures})
            raise EnginePackError(f"built-in shader package(s) failed to cook: {', '.join(failed)}")

        export_web_player.CookMaterialVariants(materialCook, seed_project, kEngineShaders, kShaderCook, staging)
        reflector = builtin_shader_cook.FindShaderReflect(shaderpkgDir)
        builtin_shader_cook.StripShaderPkgSpirv(reflector, sorted(staging.rglob("*.shaderpkg")))
        paths = export_web_player.StagedRelativePaths(staging)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(gepak.PackDirectory(staging, paths))
        return paths
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, required=True, help="the .gepak file to write")
    parser.add_argument("--build-tree", type=Path, required=True,
                        help="a desktop build of this checkout with CompileShaderPkgs and MaterialVariantCook built")
    parser.add_argument("--seed-project", type=Path, default=kSeedProject)
    args = parser.parse_args()
    try:
        paths = build_engine_pack(args.out, args.build_tree, args.seed_project)
    except (EnginePackError, export_web_player.ExportError) as error:
        print(f"engine_pack: {error}", file=sys.stderr)
        return 1
    print(f"engine_pack: {args.out}: {len(paths)} files, {args.out.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
