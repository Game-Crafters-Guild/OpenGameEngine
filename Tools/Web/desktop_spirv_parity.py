#!/usr/bin/env python3
"""Review native SPIR-V changes against a pinned control revision.

Compatibility workarounds normally belong behind GE_COMPAT_PROFILE. A native
byte difference is a review signal, not proof of a regression: shared fixes and
measured improvements belong in both paths. Record each accepted difference in
kKnownDrift with its reason and validation, and remove stale entries when the
control contains the change.

CTest enables this review only when GE_DESKTOP_SHADER_PARITY_BASELINE is set.
The standalone script defaults to the branch's merge base with origin/main.
Module and package stages, moved files, and native UI/scatter/ocean variants are
compiled with the build's own flags, which Rendering states once in
ge_compile_shader. The new test-only CBT heap32 variant has no native
control counterpart; its separate terrain decode tests verify correctness.

Exit 0: only reviewed differences; 1: unreviewed drift, deletion, or compile
failure; 125: toolchain/control unavailable (CTest reports SKIPPED).
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

kStageSuffixes = (".vert", ".frag", ".comp")
kCannotRun = 125
kRenderingShaders = "Engine/Modules/Rendering/Shaders"

# Shader directories are found by convention — every module's `Shaders/` and every
# package's `Assets/Shaders/` — so a new module cannot add kernels the gate never sees.
kModuleParent = "Engine/Modules"
kModuleLeaf = "Shaders"
kPackageParent = "Packages"
kPackageLeaf = os.path.join("Assets", "Shaders")

# Per-file flags, mirroring Engine/Modules/Rendering/CMakeLists.txt and
# Engine/Modules/CBTTerrain/CMakeLists.txt. Rendering's ge_compile_shader gives
# every shader it builds the same include path, so only the target environment
# is per-file there; CBT owns its own rule and its own directory.
kVulkan13 = ("--target-env=vulkan1.3", "--target-spv=spv1.6")
kFlagsByName = {
    # Subgroup arithmetic / ray query.
    "draw_command_scatter.comp": (kVulkan13, kRenderingShaders),
    "rt_shadow_mask.comp": (kVulkan13, kRenderingShaders),
    "ddgi_trace_hw.comp": (kVulkan13, kRenderingShaders),
    "ddgi_classify.comp": (kVulkan13, kRenderingShaders),
    "cbt_kernels.comp": (kVulkan13, "Engine/Modules/CBTTerrain/Shaders"),
}

# Accepted common changes. These are deliberately retained improvements,
# not compatibility changes that must eventually be reverted for parity.
# A stale entry fails so the exception list stays tied to the pinned control.
kKnownDrift = {}


def ShaderDirs(root):
    """Every shader directory the desktop build compiles, by convention."""
    found = []
    for parent, leaf in ((kModuleParent, kModuleLeaf), (kPackageParent, kPackageLeaf)):
        parentPath = os.path.join(root, parent)
        if not os.path.isdir(parentPath):
            continue
        for name in sorted(os.listdir(parentPath)):
            path = os.path.join(parentPath, name, leaf)
            if os.path.isdir(path):
                found.append(os.path.relpath(path, root).replace(os.sep, "/"))
    return found


def StageFiles(root, dirs):
    found = set()
    for shaderDir in dirs:
        for dirPath, _, fileNames in os.walk(os.path.join(root, shaderDir)):
            for name in fileNames:
                if name.endswith(kStageSuffixes):
                    rel = os.path.relpath(os.path.join(dirPath, name), root)
                    found.add(rel.replace(os.sep, "/"))
    return found


def Compile(glslc, root, rel, outDir, defines=()):
    extraFlags, includeDir = kFlagsByName.get(os.path.basename(rel),
                                              ((), kRenderingShaders))
    args = [glslc]
    if includeDir is not None:
        args.append("-I" + os.path.join(root, includeDir))
    args.extend(extraFlags)
    args.extend("-D" + define for define in defines)
    if os.path.basename(rel) in ("grass_placement.comp", "terrain_grass_place.comp"):
        # Runtime-compiled by TerrainGrass with its own and CBT's include roots.
        args.extend(kVulkan13)
        for path in ("Engine/Modules/TerrainGrass/Shaders",
                     "Engine/Modules/CBTTerrain/Shaders",
                     "Engine/Modules/CBTTerrain/Shaders/CBT"):
            args.append("-I" + os.path.join(root, path))
    out = os.path.join(outDir, rel.replace("/", "_") + "_" + "_".join(defines) + ".spv")
    args += [os.path.join(root, rel), "-o", out]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        lines = result.stderr.strip().splitlines()
        return None, lines[0] if lines else "compile failed"
    with open(out, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest(), ""


def ResolveBaseline(repo, explicit):
    if explicit:
        probe = subprocess.run(["git", "-C", repo, "rev-parse", "--verify", explicit],
                               capture_output=True, text=True)
        return (explicit, "") if probe.returncode == 0 else (None, explicit)
    mergeBase = subprocess.run(["git", "-C", repo, "merge-base", "HEAD", "origin/main"],
                               capture_output=True, text=True)
    if mergeBase.returncode != 0 or not mergeBase.stdout.strip():
        return None, "merge-base HEAD origin/main"
    return mergeBase.stdout.strip(), ""


def Archive(repo, rev, dirs, dest):
    # Discover control directories independently, including modules removed or
    # renamed on the branch. Candidate-only discovery misses deleted kernels.
    listing = subprocess.run(["git", "-C", repo, "ls-tree", "-r", "--name-only", rev],
                             capture_output=True, text=True, check=True)
    dirs = set()
    for path in listing.stdout.splitlines():
        parts = path.split("/")
        if len(parts) > 4 and parts[:2] == ["Engine", "Modules"] and parts[3] == "Shaders":
            dirs.add("/".join(parts[:4]))
        elif len(parts) > 4 and parts[0] == "Packages" and parts[2:4] == ["Assets", "Shaders"]:
            dirs.add("/".join(parts[:4]))
    os.makedirs(dest, exist_ok=True)
    for shaderDir in dirs:
        archive = subprocess.run(["git", "-C", repo, "archive", rev, shaderDir],
                                 capture_output=True)
        if archive.returncode != 0:
            continue  # directory absent at that revision
        extract = subprocess.run(["tar", "-x", "-C", dest], input=archive.stdout,
                                 capture_output=True)
        if extract.returncode != 0:
            return False
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True, help="repository root")
    parser.add_argument("--glslc", default=None, help="glslc path (default: PATH)")
    parser.add_argument("--baseline", default=None,
                        help="control revision (default: the merge base of HEAD and "
                             "origin/main)")
    args = parser.parse_args()

    glslc = args.glslc or shutil.which("glslc")
    if not glslc or not os.path.exists(glslc):
        print("SKIP: glslc not found (Vulkan SDK or --glslc)")
        return kCannotRun

    baselineRev, missing = ResolveBaseline(args.repo, args.baseline)
    if baselineRev is None:
        print(f"SKIP: cannot resolve the control revision ({missing}) in this clone")
        return kCannotRun

    candidateDirs = ShaderDirs(args.repo)
    if not candidateDirs:
        print(f"SKIP: no shader directories under {args.repo}")
        return kCannotRun

    with tempfile.TemporaryDirectory() as tmp:
        baselineRoot = os.path.join(tmp, "baseline")
        if not Archive(args.repo, baselineRev, candidateDirs, baselineRoot):
            print("SKIP: could not extract the control shader tree (tar unavailable?)")
            return kCannotRun

        outDirs = {"baseline": os.path.join(tmp, "spv-base"),
                   "candidate": os.path.join(tmp, "spv-cand")}
        for path in outDirs.values():
            os.makedirs(path)

        baselineFiles = StageFiles(baselineRoot, ShaderDirs(baselineRoot))
        candidateFiles = StageFiles(args.repo, candidateDirs)
        # A moved file must stay covered: pair the leftovers by basename so a rename
        # cannot quietly drop a kernel out of the comparison.
        pairs = [(rel, rel) for rel in sorted(baselineFiles & candidateFiles)]
        def UniqueNames(files):
            names = {}
            for rel in files:
                name = os.path.basename(rel)
                if name in names:
                    raise RuntimeError(f"ambiguous moved shader: {names[name]} and {rel}")
                names[name] = rel
            return names
        movedBase = UniqueNames(baselineFiles - candidateFiles)
        movedCand = UniqueNames(candidateFiles - baselineFiles)
        moved = sorted(set(movedBase) & set(movedCand))
        pairs += [(movedBase[name], movedCand[name]) for name in moved]
        unpairedBase = sorted(r for n, r in movedBase.items() if n not in movedCand)
        unpairedCand = sorted(r for n, r in movedCand.items() if n not in movedBase)
        if not pairs:
            print("SKIP: no stage shaders found in both trees")
            return kCannotRun
        common = []
        for baseRel, rel in pairs:
            variants = [()]
            name = os.path.basename(rel)
            if name in ("ui_sdf.vert", "ui_sdf.frag"):
                variants += [("UI_SUBPIXEL_DUAL_SRC",), ("UI_BLEND_SPACE_ENCODED",),
                             ("UI_SUBPIXEL_DUAL_SRC", "UI_BLEND_SPACE_ENCODED")]
            elif name == "draw_command_scatter.comp":
                variants += [("SCATTER_COMPACT",), ("SCATTER_STATS",),
                             ("SCATTER_COMPACT", "SCATTER_STATS")]
            elif name == "ocean_underwater.frag":
                variants += [("OCEAN_UNDERWATER_SHADOWS",)]
            common += [(baseRel, rel, defines) for defines in variants]

        drift, statusChanged, notStandalone = [], [], []
        perDir = {}
        # Bound concurrency; each variant writes a distinct output filename.
        with ThreadPoolExecutor(max_workers=8) as executor:
            pending = [(baseRel, rel, defines,
                        executor.submit(Compile, glslc, baselineRoot, baseRel, outDirs["baseline"], defines),
                        executor.submit(Compile, glslc, args.repo, rel, outDirs["candidate"], defines))
                       for baseRel, rel, defines in common]
        for baseRel, sourceRel, defines, baseResult, candResult in pending:
            rel = sourceRel + (" [" + ",".join(defines) + "]" if defines else "")
            baseHash, baseErr = baseResult.result()
            candHash, candErr = candResult.result()
            counts = perDir.setdefault(os.path.dirname(rel), [0, 0])
            if (baseHash is None) != (candHash is None):
                statusChanged.append((rel, baseErr or candErr))
            elif baseHash is None:
                notStandalone.append((rel, baseErr))
            elif baseHash == candHash:
                counts[0] += 1
            else:
                drift.append(rel)
                counts[1] += 1

    known = [rel for rel in drift if rel.split(" [")[0] in kKnownDrift]
    unexpected = [rel for rel in drift if rel.split(" [")[0] not in kKnownDrift]
    stale = sorted(set(kKnownDrift) - {rel.split(" [")[0] for rel in drift})
    identical = len(common) - len(drift) - len(statusChanged) - len(notStandalone)

    print(f"control={baselineRev}  directories={len(candidateDirs)}  "
          f"stage shaders={len(common)}  identical={identical}  "
          f"drift={len(drift)} (known {len(known)}, unexpected {len(unexpected)})  "
          f"status-changed={len(statusChanged)}  "
          f"not-standalone-either-side={len(notStandalone)}")
    for shaderDir in sorted(perDir):
        same, differ = perDir[shaderDir]
        print(f"  {shaderDir}: identical={same} drift={differ}")
    for name in moved:
        print(f"moved, paired by name: {movedBase[name]} -> {movedCand[name]}")
    for rel in unpairedBase:
        print(f"DELETED CONTROL SHADER: {rel}")
    for rel in unpairedCand:
        print(f"only in the working tree (not compared): {rel}")
    for rel, message in statusChanged:
        print(f"COMPILE STATUS CHANGED {rel}: {message}")
    for rel, message in notStandalone:
        print(f"not standalone on either side (not compared): {rel}: {message}")
    for rel in known:
        print(f"KNOWN DRIFT {rel}: {kKnownDrift[rel.split(' [')[0]]}")
    for rel in unexpected:
        print(f"DESKTOP DRIFT {rel}: differs from {baselineRev}. Review correctness, quality "
              f"and performance; gate a compatibility workaround or document a verified common improvement.")
    for rel in stale:
        print(f"STALE EXEMPTION {rel}: listed in kKnownDrift but its desktop arm now "
              f"matches {baselineRev}. Delete the entry.")
    return 1 if (statusChanged or unexpected or stale or unpairedBase or notStandalone) else 0


if __name__ == "__main__":
    sys.exit(main())
