#!/usr/bin/env python3
"""Export a project as a self-contained web build.

One command produces everything a static host needs. Nothing is baked into the
wasm Player: the same binary is copied verbatim into every dist and discovers
its content at runtime from player-manifest.json, so a template built once
serves any project.

    python3 Tools/Web/export_web_player.py \\
        --project Tools/Web/smoke-project \\
        --out /tmp/websmoke-dist \\
        --template build/wasm-debug/bin \\
        --shaderpkg-dir build/macos-arm64-ninja/Shaders \\
        --material-cook build/macos-arm64-ninja/bin/DebugFast/Tools/MaterialVariantCook

    python3 Tools/Web/serve.py --root /tmp/websmoke-dist   # then open /WebPlayer.html

What it does, in order:

  1. Stages the project's game.config and Assets/ (skipping dot-directories,
     which hold editor state and derived caches). An editor project directory
     has no game.config of its own; --game-config supplies the one the editor
     derived from its build settings.
  2. Overlays the engine shader sources the material pipeline composes against,
     and the bundled UI fallback font (the browser sandbox exposes no system
     fonts).
  3. Cooks the built-in .shaderpkg set to WGSL under the compat profile
     (Tools/Web/builtin_shader_cook.py).
  4. Cooks the project's material variant set with MaterialVariantCook --web,
     into the shader cache the runtime probes. --verify is always on: a variant
     that only resolves with a compiler present is not a variant this runtime
     can draw.
  5. Packs the staged tree into content.gepak + shaders.gepak, emits
     player-manifest.json, and copies the player template and serve.py.

The dist is the unit of deployment; the staging tree is an implementation
detail and is written under --out/.staging so a failed export is inspectable.

=== player-manifest.json, schema version 1 ==============================

  {
    "schemaVersion": 1,
    "project":       "WebSmoke",            display name, from game.config
    "entryScene":    "Scenes/WebSmoke.scene",  asset-relative, from game.config
    "packs": [
      { "role": "content", "file": "content.gepak",
        "bytes": 4321, "entries": 4, "fnv1a64": "0x...." },
      { "role": "shaders", "file": "shaders.gepak",
        "bytes": 4100000, "entries": 76, "fnv1a64": "0x...." }
    ],
    "managedAssemblies": []
  }

`project` and `entryScene` are copied from the game.config that ships inside
the content pack; game.config stays the runtime's source of truth for engine
configuration, and these two exist so a launcher or a CI check can read the
dist's identity without unpacking it. Packs unpack relative to the MEMFS root
and are listed in load order. `fnv1a64` is FNV-1a 64 over the whole pack file —
a check against a truncated or stale download, not a security boundary.

`managedAssemblies` is always empty today: C# on wasm is Phase 5 of the web
platform plan and is toolchain-blocked. The slot is declared so the runtime's
manifest parse and this schema do not change when it is populated.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import builtin_shader_cook  # noqa: E402
import gepak  # noqa: E402

kRepoRoot = Path(__file__).resolve().parents[2]

kManifestName = "player-manifest.json"
kManifestSchemaVersion = 1
kContentPackName = "content.gepak"
kShaderPackName = "shaders.gepak"

# Engine shader sources the material pipeline composes against at runtime: it
# still resolves and hashes them to address the cooked cache, so a dist without
# them addresses nothing even though every variant was cooked.
kEngineShaderDirs = ("Adapters", "Includes", "Particles", "Surfaces")

# Authored surfaces that live outside Rendering/Shaders. Matches the WebEditor
# material-cook --package-shaders list: the web cook emits SSSR rows for CBT
# and grass, and Surfaces/gpu_fog_particles.glsl forwards into GPUFogParticles.
kPackageShaderRoots = (
    kRepoRoot / "Engine/Modules/GPUFogParticles/Shaders",
    kRepoRoot / "Engine/Modules/CBTTerrain/Shaders",
    kRepoRoot / "Engine/Modules/TerrainGrass/Shaders",
    # Tree Generator authors its surface and wind modifier out of the eztree
    # package. Without this root the eztree-leaves rows hard-fail and the
    # bark/trellis wind include silently composes EMPTY (shaderc substitutes
    # missing includes), so the whole cook exits 1.
    kRepoRoot / "Packages/eztree/Assets/Shaders",
)
# Staged at Assets/Shaders/<name> to match the editor's preload layout
# (the WebEditor --preload-file list in Apps/Editor/CMakeLists.txt).
kPackageShaderMounts = (
    (kRepoRoot / "Engine/Modules/GPUFogParticles/Shaders/Surfaces", "GPUFogParticles/Surfaces"),
    (kRepoRoot / "Engine/Modules/CBTTerrain/Shaders/CBT", "CBT"),
    (kRepoRoot / "Engine/Modules/TerrainGrass/Shaders/TerrainGrass", "TerrainGrass"),
    # Whole tree under EZTree/ for AppendStagedModuleShaderDirs, plus the two
    # files at adapter-relative paths: intern's include-closure scan resolves
    # `#include "Surfaces/ez_tree_leaves.glsl"` against AdapterShaderDir, and a
    # miss there hashes a different key than the cook wrote.
    (kRepoRoot / "Packages/eztree/Assets/Shaders", "EZTree"),
    (kRepoRoot / "Packages/eztree/Assets/Shaders/Surfaces", "Surfaces"),
    (kRepoRoot / "Packages/eztree/Assets/Shaders/VertexModifiers", "VertexModifiers"),
)

# No system fonts exist in the browser sandbox, so the UI's fallback face has to
# ship with the content.
kFallbackFont = Path("Apps/Editor/Assets/Fonts/Roboto-Regular.ttf")

# MEMFS layout. The Player's "executable directory" is the FS root, so these
# mirror the desktop Player's next-to-exe layout.
kShaderAssetDir = "Assets/Shaders"
kWorkspaceDir = ".workspace"
kVariantCacheDir = f"{kWorkspaceDir}/.Cache/Shaders"

# The template is the wasm Player exactly as built. WebPlayer.html already has
# Tools/Web/shell.html expanded into it by the link step, so the shell is a
# build input and never a dist file.
kTemplateFiles = ("WebPlayer.html", "WebPlayer.js", "WebPlayer.wasm")


class ExportError(Exception):
    """An export that cannot produce a loadable dist."""


def StagedRelativePaths(root: Path) -> list[str]:
    return sorted(str(path.relative_to(root).as_posix())
                  for path in root.rglob("*") if path.is_file())


def CopyProjectContent(project: Path, staging: Path, gameConfig: Path | None = None) -> None:
    """Stage game.config and Assets/, skipping dot-directories.

    Dot-directories are editor state (.Editor), derived caches (.Cache) and
    version control — never runtime content, and .Cache in particular would
    ship a desktop shader cache the browser cannot read.

    `gameConfig` overrides the project's own file. An editor project directory
    holds no game.config — the editor derives one from its build settings
    (startup scene, render pipeline, game name) and hands it over here, the
    same values BuildPipeline writes for a desktop package.
    """
    configSource = gameConfig if gameConfig is not None else project / "game.config"
    if not configSource.is_file():
        if gameConfig is not None:
            raise ExportError(f"--game-config is not a file: {configSource}")
        raise ExportError(f"{project} is not a project: no game.config")
    shutil.copy2(configSource, staging / "game.config")

    assets = project / "Assets"
    if not assets.is_dir():
        raise ExportError(f"{project} has no Assets directory")
    for source in sorted(assets.rglob("*")):
        relative = source.relative_to(project)
        if any(part.startswith(".") for part in relative.parts):
            continue
        if source.is_file():
            destination = staging / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)


def CopyEngineRuntimeAssets(engineShaders: Path, font: Path, staging: Path) -> None:
    for directory in kEngineShaderDirs:
        source = engineShaders / directory
        if not source.is_dir():
            raise ExportError(f"engine shader source directory missing: {source}")
        shutil.copytree(source, staging / kShaderAssetDir / directory, dirs_exist_ok=True)
    for source, mountName in kPackageShaderMounts:
        if not source.is_dir():
            raise ExportError(f"package shader source directory missing: {source}")
        shutil.copytree(source, staging / kShaderAssetDir / mountName, dirs_exist_ok=True)
    if not font.is_file():
        raise ExportError(f"fallback font missing: {font}")
    destination = staging / "Assets" / "Fonts" / font.name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(font, destination)


def CookMaterialVariants(materialCook: Path, project: Path, engineShaders: Path,
                         shaderCook: Path, staging: Path) -> None:
    command = [str(materialCook),
               "--project", str(project.resolve()),
               "--engine-shaders", str(engineShaders.resolve()),
               "--cache", str((staging / kVariantCacheDir).resolve()),
               "--web", "--shadercook", str(shaderCook.resolve()),
               "--verify"]
    for root in kPackageShaderRoots:
        command.extend(["--package-shaders", str(root.resolve())])
    print(f"export: {' '.join(command)}")
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        tail = "\n".join((result.stdout + result.stderr).splitlines()[-25:])
        raise ExportError(f"MaterialVariantCook failed (exit {result.returncode}):\n{tail}")


def SplitStagedFiles(staging: Path) -> tuple[list[str], list[str]]:
    """Partition the staged tree into the content pack and the shader pack.

    Shader artifacts are separated from game content because they are the part
    an export re-cooks: the split keeps a content-only change from invalidating
    a client's cached shader pack, and makes a missing cook obvious in the
    manifest instead of hidden inside one opaque blob.
    """
    content: list[str] = []
    shaders: list[str] = []
    for path in StagedRelativePaths(staging):
        isShader = path.startswith(f"{kShaderAssetDir}/") or path.startswith(f"{kWorkspaceDir}/")
        (shaders if isShader else content).append(path)
    return content, shaders


def WritePack(staging: Path, outDir: Path, name: str, role: str,
              relativePaths: list[str]) -> dict:
    image = gepak.PackDirectory(staging, relativePaths)

    # Round-trip every pack before it ships: a writer/reader disagreement, a
    # path that escapes the root, or a truncated entry is caught here rather
    # than as an empty frame in a browser.
    roundTripped = gepak.Unpack(image)
    if [entry.Path for entry in roundTripped] != sorted(relativePaths):
        raise ExportError(f"{name}: round-trip lost or reordered entries")
    for entry in roundTripped:
        if entry.Data != (staging / entry.Path).read_bytes():
            raise ExportError(f"{name}: round-trip corrupted {entry.Path}")

    (outDir / name).write_bytes(image)
    return {"role": role, "file": name, "bytes": len(image),
            "entries": len(relativePaths), "fnv1a64": f"0x{gepak.Fnv1a64(image):016x}"}


def CopyTemplate(template: Path, outDir: Path) -> None:
    for name in kTemplateFiles:
        source = template / name
        if not source.is_file():
            raise ExportError(
                f"player template incomplete: {source} missing. Build the WebPlayer target "
                f"(cmake --build build/wasm-debug --target WebPlayer) and point --template at "
                f"its bin directory.")
        shutil.copy2(source, outDir / name)
    shutil.copy2(kRepoRoot / "Tools" / "Web" / "serve.py", outDir / "serve.py")


def Export(args: argparse.Namespace) -> None:
    outDir: Path = args.out
    staging = outDir / ".staging"
    if outDir.exists():
        shutil.rmtree(outDir)
    staging.mkdir(parents=True)

    CopyProjectContent(args.project, staging, args.game_config)
    CopyEngineRuntimeAssets(args.engine_shaders, args.font, staging)

    print("export: cooking built-in shader packages to WGSL")
    failures = builtin_shader_cook.CookBuiltinPackages(
        args.shaderpkg_dir, args.engine_shaders, staging / kShaderAssetDir)
    for packageName, error in failures:
        print(f"  FAILED {packageName}: {error.splitlines()[0] if error else ''}", file=sys.stderr)
    if failures:
        raise ExportError(f"{len(failures)} built-in shader package(s) failed to cook")

    CookMaterialVariants(args.material_cook, args.project, args.engine_shaders,
                         args.shadercook, staging)
    if not (staging / kVariantCacheDir).is_dir():
        raise ExportError("material variant cook produced no shader cache")

    contentFiles, shaderFiles = SplitStagedFiles(staging)
    manifestPacks = [
        WritePack(staging, outDir, kContentPackName, "content", contentFiles),
        WritePack(staging, outDir, kShaderPackName, "shaders", shaderFiles),
    ]

    gameConfig = json.loads((staging / "game.config").read_text())
    manifest = {
        "schemaVersion": kManifestSchemaVersion,
        "project": gameConfig.get("gameName", args.project.name),
        "entryScene": gameConfig.get("startupScene", ""),
        "packs": manifestPacks,
        "managedAssemblies": [],
    }
    (outDir / kManifestName).write_text(json.dumps(manifest, indent=2) + "\n")

    CopyTemplate(args.template, outDir)
    shutil.rmtree(staging)

    total = sum(pack["bytes"] for pack in manifestPacks)
    print(f"export: {manifest['project']} -> {outDir}")
    for pack in manifestPacks:
        print(f"  {pack['file']}: {pack['entries']} entries, {pack['bytes']} bytes, "
              f"{pack['fnv1a64']}")
    print(f"  {total} bytes of content; serve with "
          f"python3 Tools/Web/serve.py --root {outDir}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--project", type=Path, required=True,
                        help="project directory (game.config + Assets/)")
    parser.add_argument("--out", type=Path, required=True, help="dist directory to create")
    parser.add_argument("--template", type=Path, required=True,
                        help="directory holding the built WebPlayer.{html,js,wasm}")
    parser.add_argument("--shaderpkg-dir", type=Path, required=True,
                        help="desktop build's Shaders/ dir (SPIR-V .shaderpkg + reflection)")
    parser.add_argument("--material-cook", type=Path, required=True,
                        help="path to the built MaterialVariantCook")
    parser.add_argument("--game-config", type=Path,
                        help="game.config to ship instead of the project's own; the editor "
                             "supplies one derived from its build settings")
    parser.add_argument("--engine-shaders", type=Path,
                        default=kRepoRoot / "Engine" / "Modules" / "Rendering" / "Shaders",
                        help="engine GLSL shader source root")
    parser.add_argument("--shadercook", type=Path,
                        default=kRepoRoot / "Tools" / "ShaderCook" / "shadercook.py")
    parser.add_argument("--font", type=Path, default=kRepoRoot / kFallbackFont,
                        help="UI fallback font to bundle")
    args = parser.parse_args()

    try:
        Export(args)
    except (ExportError, RuntimeError) as error:
        print(f"export: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
