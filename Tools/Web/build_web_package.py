#!/usr/bin/env python3
"""Assemble the web library's package: what a page imports, in one directory.

    python Tools/Web/build_web_package.py \\
        --st-dir build/wasm-release-singlethread/bin --mt-dir build/wasm-release/bin \\
        --component-dts build/wasm-release/generated/components.d.ts \\
        --engine-pack <opengine-core.gepak> --out <package dir>

    python Tools/Web/build_web_package.py --check --out <package dir>

The package, flat so the facade finds the engine module beside itself (its default
`coreUrl` is the folder `opengine.mjs` is served from):

    opengine.mjs                    the facade (Apps/WebLibrary/ts), bundled, version stamped
    opengine.d.ts, components.d.ts  the facade's declarations
    types/components.d.ts           every reflected component (ComponentScanner --emit-dts)
    opengine-core-binding.js        the binding the facade imports
    opengine-core.st.js/.wasm       the single-threaded module
    opengine-core.mt.js/.wasm       the threaded module
    opengine-core.gepak             the engine pack ge_create fetches (Tools/Web/engine_pack.py
                                    defines its contents: the cooked shaders, the engine GLSL)
    coi-serviceworker.js, .LICENSE the isolation worker for hosts without headers: its scope is this
                                    folder, which must hold the engine module (the threaded
                                    build's workers start from opengine-core.mt.js)
    THIRD_PARTY_NOTICES.md          the notices of the third-party code in the module and the
                                    third-party content in the pack
    README.md, llms.txt, LICENSE, package.json
    examples/                       the example pages

`--site <dir>` writes the same package to <dir>/package and the gallery to <dir>/index.html,
the layout of the demo site, with the engine pack split into parts under 100 MB that the
binding joins (opengine-core.gepak.parts.json lists them); `--site-models <dir>` adds the
.glb files the site hosts itself under <dir>/models, and `--site-without-particles` drops the
particle programs from the site's pack. `--check` verifies the file list and prints the raw,
gzip and brotli size of each file and of what a page downloads at each build.

The version is the engine's, from the repository root's VERSION file: `<year>.<month>.<patch>`,
with an optional prerelease suffix `-alpha.<n>` or `-beta.<n>`. A packaged commit carrying a
release tag (`v` plus the version, `v2026.10.0-alpha.4`) is that version, and a tag that differs
from VERSION is refused; a commit without one is `<VERSION>-dev.<commit>`. `--version` names the
version outright and must equal VERSION. A prerelease's package.json
names its npm dist-tag (`alpha`, `beta`) in publishConfig, so a plain `npm publish` never moves
`latest` to it.
"""

from __future__ import annotations

import argparse
import gzip
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gepak  # noqa: E402
from export_web_player import ExportError  # noqa: E402

from engine_pack import EnginePackError, build_engine_pack, kSeedProject  # noqa: E402

kRepoRoot = Path(__file__).resolve().parents[2]
kLibraryDir = kRepoRoot / "Apps" / "WebLibrary"
kFacadeDir = kLibraryDir / "ts"

kPackageName = "@openengine/web"
kBuilds = ("st", "mt")
kFacadeName = "opengine.mjs"
kBindingName = "opengine-core-binding.js"
kEnginePackName = "opengine-core.gepak"
# On the site the pack is split: GitHub refuses a file over 100 MB, and a release asset, the
# other place a large file can live, answers without CORS headers, so a page cannot read it.
kPackPartsIndex = "opengine-core.gepak.parts.json"
kPackPartBytes = 95 * 1024 * 1024
# The material variant cache in the engine pack: one program.shaderpkg per cooked variant.
kVariantCachePrefix = ".workspace/.Cache/Shaders/"
# Every particle program carries the particle varyings the adapters declare for the particle
# path (Adapters/adapter_vertex.glsl, adapter_forward.glsl); no mesh program does.
kParticleProgramMarker = b"vParticle"
kComponentTypes = "types/components.d.ts"
kDeclarations = ("opengine.d.ts", "components.d.ts")
kDocuments = ("README.md", "llms.txt")
# The isolation worker and its license sit in the package root, beside the engine module.
kServiceWorker = ("coi-serviceworker.js", "coi-serviceworker.LICENSE")
kNoticesName = "THIRD_PARTY_NOTICES.md"
# The vcpkg ports of the wasm build whose license the notices carry: every port with a copyright
# file except the test frameworks and the header-only API registries the module links nothing of.
kNoticeExcludedPorts = ("benchmark", "gtest", "egl-registry", "opengl-registry", "vulkan-headers")
# Third-party runtime code/content and the toolchain used to build the package.
# Toolchain books identify host-only and conditional entries separately. metal-cpp
# (Apple only) and the Sponza test scene are not part of the web build.
kRepoNotices = (
    ("Roboto (the UI fallback font in the pack)", "Apps/Editor/Assets/Fonts/LICENSE.txt"),
    ("ufbx (the FBX importer)", "Engine/Source/ThirdParty/ufbx/LICENSE"),
    ("GT7 tone mapper (Includes/tonemap_gt7.glsl in the pack)", "Engine/Modules/Rendering/Shaders/Includes/tonemap_gt7.glsl"),
    ("OpenColorIO (the ACES 2 tone mapper tables)", "ThirdParty/OpenColorIO/LICENSE.txt"),
    ("BakingLab ACES", "ThirdParty/BakingLabAces/LICENSE.txt"),
    ("Bend Screen Space Shadows", "ThirdParty/BendScreenSpaceShadows/LICENSE.txt"),
    ("AMD FidelityFX Denoiser", "ThirdParty/FidelityFX-Denoiser/LICENSE.txt"),
    ("Khronos PBR Neutral", "ThirdParty/KhronosPbrNeutral/LICENSE.txt"),
    ("KinoBloom", "ThirdParty/KinoBloom/LICENSE.md"),
    ("Minimal AgX", "ThirdParty/MinimalAgX/LICENSE.txt"),
    ("OCASM Depth Veil", "ThirdParty/OCASMDepthVeil/LICENSE"),
    ("Punikonta VHS", "ThirdParty/PunikontaVhs/LICENSE.md"),
    ("Runevision Erosion Filter", "ThirdParty/RunevisionErosionFilter/LICENSE-MPL-2.0.txt"),
    ("SE Natural Bloom Dirty Lens", "ThirdParty/SENaturalBloomDirtyLens/LICENSE.txt"),
    ("Speedball GI", "ThirdParty/SpeedballGi/LICENSE.md"),
    ("coi-serviceworker", "Apps/WebLibrary/coi-serviceworker.LICENSE"),
    ("Web build toolchain (pinned host tools and bundled libraries)", "Tools/Web/licenses/toolchain-notices.md"),
    ("Rust compiler dependencies (pinned source inventories)", "Tools/Web/licenses/rust-dependency-notices.md"),
    ("Example asset sources and credits", "Apps/WebLibrary/examples/ASSET_PROVENANCE.md"),
    ("Example assets: Creative Commons Attribution 4.0 (verbatim from KhronosGroup/glTF-Sample-Assets LICENSES/CC-BY-4.0.txt at 2bac6f8c57bf471df0d2a1e8a8ec023c7801dddf)",
     "Tools/Web/licenses/CC-BY-4.0.txt"),
    ("Example assets: CC0 1.0 Universal (verbatim from KhronosGroup/glTF-Sample-Assets LICENSES/CC0-1.0.txt at 2bac6f8c57bf471df0d2a1e8a8ec023c7801dddf)",
     "Tools/Web/licenses/CC0-1.0.txt"),
)
kWasmMagic = b"\0asm"
kReleaseTag = re.compile(r"^v(\d{4}\.\d{1,2}\.\d+)(?:-(alpha|beta)\.\d+)?$")

# The examples import the library by this specifier; in the source tree the import map points
# it at the facade's tsc output, in the package at the bundled facade two folders up.
kSourceImportTarget = '"@openengine/web": "../../ts/dist/src/index.js"'
kPackageImportTarget = '"@openengine/web": "../../opengine.mjs"'
# The gallery links the examples beside it in the source tree; on the site they sit in package/.
kGalleryExamplesLink = 'href="../examples/'
kSiteExamplesLink = 'href="package/examples/'

# The facade's build stamps (ts/src/build-stamp.ts) as esbuild emits them.
kVersionStamp = 'var kPackageVersion = "0.0.0";'
kWasmBytesStamp = "var kCoreWasmBytes = null;"


class PackageError(Exception):
    """A package that would not load in a page."""


def ModuleFiles(build: str) -> tuple[str, str]:
    return f"opengine-core.{build}.js", f"opengine-core.{build}.wasm"


def PackageFiles() -> list[str]:
    """Every file of the package outside examples/, relative to its root."""
    files = [kFacadeName, *kDeclarations, kComponentTypes, kBindingName, kEnginePackName,
             *kDocuments, *kServiceWorker, kNoticesName, "LICENSE", "package.json"]
    for build in kBuilds:
        files.extend(ModuleFiles(build))
    return sorted(files)


def SourceVersion(versionFile: Path) -> str:
    """The engine version the VERSION file holds."""
    version = versionFile.read_text(encoding="utf-8").strip()
    if not kReleaseTag.match(f"v{version}"):
        raise PackageError(f"{versionFile} holds '{version}', which is not <year>.<month>.<patch>, "
                           "optionally followed by -alpha.<n> or -beta.<n>")
    return version


def PackageVersion(releaseTag: str | None, commit: str, sourceVersion: str) -> str:
    if releaseTag is None:
        return f"{sourceVersion}-dev.{commit}"
    if not kReleaseTag.match(releaseTag):
        raise PackageError(f"release tag '{releaseTag}' is not v<year>.<month>.<patch>, optionally "
                           "followed by -alpha.<n> or -beta.<n>")
    if releaseTag[1:] != sourceVersion:
        raise PackageError(f"release tag '{releaseTag}' does not match VERSION, which holds "
                           f"'{sourceVersion}': tag the commit v{sourceVersion}, or change VERSION "
                           "in the commit the tag names")
    return releaseTag[1:]


def PrereleaseChannel(version: str) -> str | None:
    """The npm dist-tag a prerelease version publishes under (`alpha`, `beta`); None for a release."""
    match = kReleaseTag.match(f"v{version}")
    return match.group(2) if match else None


def CommitVersion(repo: Path) -> str:
    """The package version of the commit `repo` has checked out."""
    commit = subprocess.run(["git", "-C", str(repo), "rev-parse", "--short=10", "HEAD"],
                            capture_output=True, text=True, check=True).stdout.strip()
    tags = subprocess.run(["git", "-C", str(repo), "tag", "--points-at", "HEAD"],
                          capture_output=True, text=True, check=True).stdout.split()
    releases = sorted(tag for tag in tags if kReleaseTag.match(tag))
    if len(releases) > 1:
        raise PackageError(f"HEAD carries more than one release tag: {', '.join(releases)}")
    return PackageVersion(releases[0] if releases else None, commit, SourceVersion(repo / "VERSION"))


def ModuleProblems(directory: Path, build: str, allowStub: bool) -> list[str]:
    """What keeps `directory` from holding a loadable `build` module; empty when it does."""
    problems = []
    for name in (*ModuleFiles(build), kBindingName):
        path = directory / name
        if not path.is_file():
            problems.append(f"{path} is missing")
        elif path.stat().st_size == 0:
            if not allowStub:
                problems.append(f"{path} is empty (a stub module; pass --allow-stub to package it)")
        elif name.endswith(".wasm") and path.read_bytes()[:4] != kWasmMagic:
            problems.append(f"{path} is not a WebAssembly binary")
    return problems


def CopyModules(directories: dict[str, Path], out: Path, allowStub: bool) -> None:
    problems = []
    for build, directory in directories.items():
        problems += ModuleProblems(directory, build, allowStub)
    bindings = {build: directory / kBindingName for build, directory in directories.items()}
    if not problems and bindings["st"].read_bytes() != bindings["mt"].read_bytes():
        problems.append(f"{bindings['st']} and {bindings['mt']} differ: the two trees were built "
                        "from different sources")
    if problems:
        raise PackageError("the engine module is not ready:\n  " + "\n  ".join(problems) +
                           "\nBuild the WebLibrary target in both wasm trees "
                           "(wasm-release-singlethread and wasm-release).")
    for build, directory in directories.items():
        for name in ModuleFiles(build):
            shutil.copy2(directory / name, out / name)
    shutil.copy2(bindings["st"], out / kBindingName)


def BundleFacade(out: Path, version: str) -> None:
    esbuild = kFacadeDir / "node_modules" / "esbuild" / "bin" / "esbuild"
    if not esbuild.is_file():
        raise PackageError(f"{esbuild} is missing: run `npm ci` in {kFacadeDir}")
    target = (out / kFacadeName).resolve()
    result = subprocess.run(
        ["node", str(esbuild), "src/index.ts", "--bundle", "--format=esm", "--target=es2022",
         "--log-level=warning", f"--outfile={target}"],
        cwd=kFacadeDir, capture_output=True, text=True)
    if result.returncode != 0:
        raise PackageError(f"bundling the facade failed (exit {result.returncode}):\n"
                           f"{result.stdout}{result.stderr}")
    text = target.read_text(encoding="utf-8")
    # The module's download progress scales its compressed bytes by the wasm files' sizes.
    wasmBytes = ", ".join(f"{build}: {(out / ModuleFiles(build)[1]).stat().st_size}" for build in kBuilds)
    for stamp, value in ((kVersionStamp, f'var kPackageVersion = "{version}";'),
                         (kWasmBytesStamp, f"var kCoreWasmBytes = {{ {wasmBytes} }};")):
        if text.count(stamp) != 1:
            raise PackageError(f"{target} does not hold the build stamp `{stamp}` once; "
                               "update the stamps here to match ts/src/build-stamp.ts")
        text = text.replace(stamp, value)
    target.write_text(text, encoding="utf-8", newline="\n")


def RewriteOnce(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if text.count(old) != 1:
        raise PackageError(f"{path} does not contain `{old}` exactly once")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")


def WriteNotices(out: Path, vcpkgShare: Path) -> None:
    """THIRD_PARTY_NOTICES.md: each third-party component's license text, from the wasm build's
    vcpkg ports and the repository's third-party folders."""
    sections = []
    ports = sorted(path.parent for path in vcpkgShare.glob("*/copyright")
                   if path.parent.name not in kNoticeExcludedPorts)
    if not ports:
        raise PackageError(f"{vcpkgShare} holds no vcpkg port copyright files: pass the wasm build "
                           "tree's vcpkg_installed/<triplet>/share")
    for port in ports:
        sections.append((f"{port.name} (vcpkg port, a dependency of the engine's wasm build)", port / "copyright"))
    sections += [(name, kRepoRoot / path) for name, path in kRepoNotices]
    text = ["# Third-party notices", "",
            "The engine module (opengine-core.st/mt.wasm), the engine pack and the examples carry the "
            "third-party code and content below. Build-tool notices are also included and identify "
            "their own scope; host tools are not embedded in the WebAssembly module. "
            "License texts are reproduced from the stated sources.", ""]
    for name, path in sections:
        if not path.is_file():
            raise PackageError(f"the notice for {name} is missing: {path}")
        notice = path.read_text(encoding="utf-8")
        # A source notice may itself contain Markdown fences. Preserve it as one literal block.
        fence = "`" * max(3, max((len(run) + 1 for run in re.findall(r"`+", notice)), default=0))
        text += [f"## {name}", "", fence, notice, fence, ""]
    (out / kNoticesName).write_text("\n".join(text), encoding="utf-8", newline="\n")


def CopyExamples(out: Path) -> None:
    shutil.copytree(kLibraryDir / "examples", out / "examples")
    for page in sorted((out / "examples").glob("*/index.html")):
        RewriteOnce(page, kSourceImportTarget, kPackageImportTarget)


def WritePackageJson(out: Path, version: str) -> None:
    facade = json.loads((kFacadeDir / "package.json").read_text(encoding="utf-8"))
    manifest = {
        "name": kPackageName,
        "version": version,
        "description": facade["description"],
        "license": "MIT",
        "type": "module",
        "types": "./opengine.d.ts",
        "exports": {
            ".": {"types": "./opengine.d.ts", "default": f"./{kFacadeName}"},
            "./package.json": "./package.json",
        },
        "files": [name for name in PackageFiles() if name != "package.json"] + ["examples/"],
        "sideEffects": False,
    }
    channel = PrereleaseChannel(version)
    if channel:
        manifest["publishConfig"] = {"tag": channel}
    (out / "package.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8",
                                      newline="\n")


def Build(args: argparse.Namespace, out: Path) -> str:
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    sourceVersion = SourceVersion(kRepoRoot / "VERSION")
    if args.version and args.version != sourceVersion:
        raise PackageError(f"--version {args.version} does not match VERSION, which holds "
                           f"'{sourceVersion}': pass --version {sourceVersion}, or change VERSION")
    version = args.version or CommitVersion(kRepoRoot)

    CopyModules({"st": args.st_dir, "mt": args.mt_dir}, out, args.allow_stub)
    if args.engine_pack:
        shutil.copy2(args.engine_pack, out / kEnginePackName)
    else:
        if not args.pack_build_tree:
            raise PackageError("the engine pack needs --engine-pack, or --pack-build-tree to build it")
        build_engine_pack(out / kEnginePackName, args.pack_build_tree, args.seed_project)

    BundleFacade(out, version)
    for name in kDeclarations:
        shutil.copy2(kFacadeDir / name, out / name)
    if not args.component_dts.is_file():
        raise PackageError(f"{args.component_dts} is missing: it is written by the Engine target's "
                           "component scan (ComponentScanner --emit-dts) in the build tree")
    (out / "types").mkdir()
    shutil.copy2(args.component_dts, out / kComponentTypes)
    for name in (*kDocuments, *kServiceWorker):
        shutil.copy2(kLibraryDir / name, out / name)
    WriteNotices(out, args.vcpkg_share)
    shutil.copy2(kRepoRoot / "LICENSE", out / "LICENSE")
    CopyExamples(out)
    WritePackageJson(out, version)
    return version


def WriteSite(site: Path, models: Path | None) -> Path:
    """The demo site's layout: the gallery at the root, the package under package/, and the
    models the site hosts itself (the .glb files in `models`) under models/."""
    if site.exists():
        for child in site.iterdir():
            if child.name in ("package", "index.html", "models"):
                shutil.rmtree(child) if child.is_dir() else child.unlink()
    site.mkdir(parents=True, exist_ok=True)
    if models is not None:
        glbs = sorted(models.glob("*.glb"))
        if not glbs:
            raise PackageError(f"--site-models {models} holds no .glb")
        (site / "models").mkdir()
        for glb in glbs:
            shutil.copy2(glb, site / "models" / glb.name)
    shutil.copy2(kLibraryDir / "gallery" / "index.html", site / "index.html")
    text = (site / "index.html").read_text(encoding="utf-8")
    if kGalleryExamplesLink not in text:
        raise PackageError(f"the gallery links no example through `{kGalleryExamplesLink}`")
    (site / "index.html").write_text(text.replace(kGalleryExamplesLink, kSiteExamplesLink),
                                     encoding="utf-8", newline="\n")
    return site / "package"


def StripParticlePrograms(out: Path) -> tuple[int, int]:
    """Drops the particle programs from the engine pack in `out`, for a site whose pages draw
    no particles; returns how many programs it dropped and how many it kept."""
    pack = out / kEnginePackName
    kept, dropped = [], 0
    for entry in gepak.Unpack(pack.read_bytes()):
        isProgram = entry.Path.startswith(kVariantCachePrefix)
        if isProgram and kParticleProgramMarker in entry.Data:
            dropped += 1
            continue
        kept.append(entry)
    pack.write_bytes(gepak.Pack(kept))
    return dropped, sum(entry.Path.startswith(kVariantCachePrefix) for entry in kept)


def SplitPack(out: Path, partBytes: int = kPackPartBytes) -> list[str]:
    """Replaces the engine pack in `out` with parts of at most `partBytes` and their index."""
    pack = out / kEnginePackName
    image = pack.read_bytes()
    parts = []
    for index, offset in enumerate(range(0, len(image), partBytes)):
        name = f"{kEnginePackName}.part{index}"
        (out / name).write_bytes(image[offset:offset + partBytes])
        parts.append({"file": name, "bytes": min(partBytes, len(image) - offset)})
    (out / kPackPartsIndex).write_text(json.dumps({
        "file": kEnginePackName, "bytes": len(image),
        "fnv1a64": f"0x{gepak.Fnv1a64(image):016x}", "parts": parts}, indent=2) + "\n",
        encoding="utf-8", newline="\n")
    pack.unlink()
    return [part["file"] for part in parts]


def PackFiles(out: Path) -> list[str]:
    """The files that carry the engine pack in `out`: the pack, or its index and parts."""
    index = out / kPackPartsIndex
    if not index.is_file():
        return [kEnginePackName]
    return [kPackPartsIndex, *(part["file"] for part in
                               json.loads(index.read_text(encoding="utf-8"))["parts"])]


def JoinPack(out: Path) -> bytes:
    """The engine pack's bytes, joined from its parts when it is split; checked against the index."""
    files = PackFiles(out)
    if files == [kEnginePackName]:
        return (out / kEnginePackName).read_bytes()
    index = json.loads((out / kPackPartsIndex).read_text(encoding="utf-8"))
    image = b"".join((out / name).read_bytes() for name in files[1:])
    if len(image) != index["bytes"] or f"0x{gepak.Fnv1a64(image):016x}" != index["fnv1a64"]:
        raise PackageError(f"the parts {kPackPartsIndex} lists do not join into the pack it describes")
    return image


def CompressedSizes(paths: list[Path]) -> dict[Path, tuple[int, int, int]]:
    """Raw, gzip (level 9) and brotli (quality 11) bytes of each file."""
    script = ("const fs = require('fs'), zlib = require('zlib');"
              "const q = { params: { [zlib.constants.BROTLI_PARAM_QUALITY]: 11 } };"
              "console.log(JSON.stringify(process.argv.slice(1).map("
              "p => zlib.brotliCompressSync(fs.readFileSync(p), q).length)));")
    result = subprocess.run(["node", "-e", script, *map(str, paths)], capture_output=True,
                            text=True, check=True)
    brotli = json.loads(result.stdout)
    return {path: (path.stat().st_size, len(gzip.compress(path.read_bytes(), 9)), size)
            for path, size in zip(paths, brotli)}


def Check(out: Path, allowStub: bool) -> list[str]:
    """Problems with the package at `out`; prints its sizes when it is complete."""
    packFiles = PackFiles(out)
    expected = [name for name in PackageFiles() if name != kEnginePackName] + packFiles
    problems = [f"{name} is missing" for name in expected if not (out / name).is_file()]
    for page in ("model-viewer",):
        if not (out / "examples" / page / "index.html").is_file():
            problems.append(f"examples/{page}/index.html is missing")
    if problems:
        return problems
    for build in kBuilds:
        problems += ModuleProblems(out, build, allowStub)
    image = JoinPack(out)
    if image or not allowStub:
        try:
            gepak.Unpack(image)
        except Exception as error:  # the reader's own refusal, whatever its type
            problems.append(f"{kEnginePackName} does not unpack: {error}")
    if problems:
        return problems

    shared = [kFacadeName, kBindingName, *packFiles]
    names = sorted({*shared, *(name for build in kBuilds for name in ModuleFiles(build))})
    sizes = CompressedSizes([out / name for name in names])
    print(f"{'file':32} {'raw':>12} {'gzip':>12} {'brotli':>12}")
    for name in names:
        raw, gz, br = sizes[out / name]
        print(f"{name:32} {raw:12,} {gz:12,} {br:12,}")
    for build in kBuilds:
        files = [out / name for name in (*shared, *ModuleFiles(build))]
        totals = [sum(sizes[path][column] for path in files) for column in range(3)]
        print(f"{'page download at ' + build:32} {totals[0]:12,} {totals[1]:12,} {totals[2]:12,}")
    manifest = json.loads((out / "package.json").read_text(encoding="utf-8"))
    print(f"package {kPackageName}@{manifest['version']}: {len(expected)} files and examples/ complete")
    return []


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, help="the package directory to write or check")
    parser.add_argument("--site", type=Path,
                        help="write the demo site instead: the gallery and <site>/package")
    parser.add_argument("--site-models", type=Path,
                        help="with --site: a folder of .glb files the site hosts under models/")
    parser.add_argument("--site-without-particles", action="store_true",
                        help="with --site: drop the particle programs from the site's engine pack "
                             "(its pages draw no particles; the package keeps the full pack)")
    parser.add_argument("--check", action="store_true",
                        help="verify the package and print its sizes; builds nothing")
    parser.add_argument("--st-dir", type=Path, help="the single-threaded tree's bin directory")
    parser.add_argument("--mt-dir", type=Path, help="the threaded tree's bin directory")
    parser.add_argument("--component-dts", type=Path,
                        help="components.d.ts the Engine target's component scan wrote")
    parser.add_argument("--engine-pack", type=Path, help="a built opengine-core.gepak")
    parser.add_argument("--vcpkg-share", type=Path,
                        help="the wasm build's vcpkg_installed/<triplet>/share, whose port copyright "
                             "files THIRD_PARTY_NOTICES.md carries")
    parser.add_argument("--pack-build-tree", type=Path,
                        help="the build tree engine_pack.py cooks from, without --engine-pack")
    parser.add_argument("--seed-project", type=Path, default=kSeedProject,
                        help="the project engine_pack.py cooks variants for, without --engine-pack")
    parser.add_argument("--version", help="the package version, instead of the commit's; must equal VERSION")
    parser.add_argument("--allow-stub", action="store_true",
                        help="accept zero-length module files (a package for tests)")
    args = parser.parse_args()
    if (args.out is None) == (args.site is None):
        parser.error("give one of --out and --site")

    try:
        out = args.out
        if args.check:
            out = out or args.site / "package"
        else:
            if not (args.st_dir and args.mt_dir and args.component_dts and args.vcpkg_share):
                parser.error("building needs --st-dir, --mt-dir, --component-dts and --vcpkg-share")
            if args.site:
                out = WriteSite(args.site, args.site_models)
            version = Build(args, out)
            if args.site:
                if args.site_without_particles:
                    dropped, kept = StripParticlePrograms(out)
                    print(f"build_web_package: the site pack keeps {kept} variant programs and drops "
                          f"{dropped} particle programs")
                SplitPack(out)
            print(f"build_web_package: {kPackageName}@{version} -> {out}")
        problems = Check(out, args.allow_stub)
    except (PackageError, EnginePackError, ExportError, subprocess.CalledProcessError) as error:
        print(f"build_web_package: {error}", file=sys.stderr)
        return 1
    for problem in problems:
        print(f"build_web_package: {problem}", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
