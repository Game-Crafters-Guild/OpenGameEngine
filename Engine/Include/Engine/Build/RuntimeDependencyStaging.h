#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace GameEngine {

/// Ship-safety C5: one entry of the explicit runtime dependency set a packaged
/// game needs. The Editor's own directory holds far more (test frameworks,
/// editor-only managed assemblies, tooling DLLs, stale applocal leftovers); the
/// curated tables below replace the old ship-everything *.dll glob. An entry
/// lists acceptable file stems separated by ';' — per-config vcpkg names
/// (freetyped/freetype, zd/z, bz2d/bz2, …) are candidates of ONE entry, first
/// match ships. A trailing '*' ships every stem match (ABI-versioned names such
/// as avcodec-62). Stems compare case-insensitively with any "lib" prefix
/// dropped, so entries cover Engine.dll and libEngine.so alike. A required entry
/// with no match fails the BUILD — never the game launch.
struct RuntimeDependency
{
    const char* Name;       ///< diagnostic name for the missing-entry error
    const char* Candidates; ///< ';'-separated acceptable stems
    bool Required;          ///< missing from the source dir => package error
};

/// Engine.dll's import closure (engine + transitive vcpkg runtime deps, derived
/// from the binaries' PE import tables) plus the platform loader set. Needed by
/// BOTH script modes: Engine is a shared library, the game cannot launch without
/// it. When the engine gains/drops a dynamic dependency, update the table — a
/// stale entry fails the package step with the entry's name.
std::span<const RuntimeDependency> EngineRuntimeDependencySet();

/// CoreCLR hosting for the managed (non-AOT) script path — loaded by name at
/// runtime, invisible to import tables.
std::span<const RuntimeDependency> ManagedHostingDependencySet();

/// Copies every satisfied entry into destDir, searching `sourceDirs` in order
/// (first directory containing a candidate wins). Packages pass the runtime of the
/// configuration the player links (BuildPipeline::ResolvePackagedEngineRuntime) first,
/// then the editor's own directory (same CRT flavor) or the vcpkg tree's flavor
/// bin dirs (cross-CRT-flavor, BuildPipeline::FindVcpkgRuntimeBinDirsForFlavor
/// order). Appends to `errors`
/// and returns false when a Required entry has no match or a copy fails.
bool StageCuratedDependencySet(const std::vector<std::filesystem::path>& sourceDirs,
                               const std::filesystem::path& destDir,
                               std::span<const RuntimeDependency> entries,
                               std::vector<std::string>& errors);

/// Stages the managed engine assemblies a packaged CoreCLR game loads, from the
/// editor's engine managed directory (sourceDir) flat into destDir, each with its
/// sidecars (.deps.json / .runtimeconfig.json / .xml). The set is derived, not
/// listed: the assemblies native code loads by name
/// (ScriptingPaths::kHostLoadedEngineAssemblyFileNames), every engine ABI surface
/// scripts compile against (GameEngine.*.ABI.dll, never GameEngine.Editor.*), and
/// the transitive runtime closure their deps.json files declare. False + an error
/// naming each absent assembly and what asked for it, or a malformed deps.json.
bool StageManagedRuntimeAssemblies(const std::filesystem::path& sourceDir,
                                   const std::filesystem::path& destDir,
                                   std::vector<std::string>& errors);

} // namespace GameEngine
