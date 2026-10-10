#pragma once

// P2 package build staging — a packaged game ships exactly the runtime half of
// every enabled package (package plan §7.6):
//   assets   -> Packages/<alias>/Assets/** (+ per-package .assetmanifest): what
//               the built scenes reference, the package's Shaders/ folder and the
//               runtimeAssets listed for the components those scenes use
//   C# code  -> Managed/Packages/<Assembly>.dll (managed builds; AOT compiles
//               package sources into the single AOT csproj instead)
//   native   -> Packages/<alias>/NativeScripts/ (relative record + engine_abi
//               marker, the per-module generalization of NativeScriptStaging)
//   index    -> Packages/packages.index (the Player's mount list)
// plus the loud build-time validation pass. Pure filesystem/data logic —
// callable against fake staged dirs in tests; BuildPipeline orchestrates.

#include "Assets/Packages/PackageCodeModules.h"
#include "Assets/Packages/PackageResolver.h"
#include "Engine/Build/AssetCollector.h"
#include "Engine/Build/NativeScriptStaging.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace GameEngine
{

// Build-time package resolution — the ONE snapshot every build consumer reads
// (validation, package asset staging, defines collection, native ABI
// digests, packages.index). Resolves fresh from <projectRoot>/Packages/
// manifest.json with the same resolver the editor uses at project open, then
// logs one line per package whose enabled/disabled state drifted from the
// session's live mounts (mid-session manifest edits apply to mounts only on
// the next project open) — so the user can see what the build is about to do.
// `sessionMountedPackageAliases` is the session's registered package sources
// (empty when no session mounted packages, e.g. headless).
PackageResolution ResolveBuildTimePackages(
    const std::filesystem::path& projectRoot,
    const std::vector<std::string>& sessionMountedPackageAliases);

// Build-time validation (never silent). Appends errors/warnings; returns false
// when any error was appended. Checks:
//  - ERROR: a package is DISABLED in the build-time resolution but its mount is
//    still registered in this session (mid-session disable): the session still
//    resolves its GUIDs and compiled its defines while the build would drop it
//    — its assets would vanish with nothing referencing them, and
//    the native ABI gate would fail later with a far less actionable message;
//  - ERROR: a collected scene/asset references a GUID owned by a DISABLED
//    package (attributed by deriving GUIDs over the disabled package's tree);
//  - ERROR: unresolved referenced GUIDs while package resolution reported
//    failures (the failed package's tree cannot be enumerated, so the error
//    carries the resolver's messages);
//  - ERROR: a runtime C# module depends on an Editor-kind assembly;
//  - ERROR: a native runtime module has neither sources nor prebuilt binaries;
//  - WARNING: an asset-carrying enabled package staged nothing and its source is
//    not mounted this session (the package collection read no registry). A
//    mounted package the build reaches nothing of (editor-only content, or
//    assets no built scene uses) is logged, not warned: nothing is missing.
// `runtimeModules` is the Player-context module set (Editor-kind already
// excluded by CollectPackageCodeModules).
bool ValidatePackagedBuild(const AssetManifest& manifest,
                           const PackageResolution& resolution,
                           const std::vector<PackageCodeModule>& runtimeModules,
                           const std::vector<std::string>& sessionMountedPackageAliases,
                           std::vector<std::string>& errors,
                           std::vector<std::string>& warnings);

// Stage each package's built native runtime modules from
// <pkgRoot>/.Cache/NativeScripts/build into <contentRoot>/Packages/<alias>/
// NativeScripts (DLL + optional PDB + RELATIVE record + engine_abi marker — the exact
// layout LoadPrebuiltUserModule(<gameRoot>/Packages/<alias>, module) reads).
// `engineAbiDigestsForModule` yields the shipping engine's ABI digest set
// computed with THAT module's compile defines — the editor stamped each
// module's build record per-config, so a single project-wide digest would
// falsely fail the staleness gate for defines-bearing packages. A declared
// runtime Cpp module with no editor build is a build ERROR: a "successful"
// package without its native gameplay is worse than a failed one.
bool StagePackageNativeModules(
    const std::filesystem::path& contentRoot,
    const PackageResolution& resolution,
    const std::vector<PackageCodeModule>& runtimeModules,
    const std::function<EngineAbiDigestSet(const PackageCodeModule&)>& engineAbiDigestsForModule,
    std::vector<std::string>& errors,
    bool includeDebugSymbols);

// Stage compiled package runtime C# assemblies (<assembliesPackagesDir>/
// <Assembly>.dll, the editor's <assemblies>/Packages/ output) into
// <managedOutDir>/Packages/ — next to GameEngine.Scripts.dll, where the script
// domain loader already picks them up. Editor-kind assemblies never ship
// (runtimeModules excludes them; the Packages/Editor/ subdir is never copied).
// A declared runtime module whose assembly was never compiled is an ERROR.
bool StagePackageManagedAssemblies(const std::filesystem::path& assembliesPackagesDir,
                                   const std::filesystem::path& managedOutDir,
                                   const std::vector<PackageCodeModule>& runtimeModules,
                                   std::vector<std::string>& errors,
                                   bool includeDebugSymbols);

// Write <contentRoot>/Packages/packages.index for every enabled package that
// staged anything (assets or native modules) — the packaged Player's mount
// list (see PackagesIndex.h for why an index replaces re-resolution).
bool StagePackagesIndex(const std::filesystem::path& contentRoot,
                        const AssetManifest& manifest,
                        const PackageResolution& resolution,
                        const std::vector<PackageCodeModule>& runtimeModules,
                        std::vector<std::string>& errors);

} // namespace GameEngine
