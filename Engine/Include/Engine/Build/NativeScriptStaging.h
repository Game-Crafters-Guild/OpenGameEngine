#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine {

/// Outcome of staging the project's editor-built native C++ user-script module
/// into a packaged game.
enum class NativeScriptStageOutcome
{
    NoScripts, ///< project has no native scripts — nothing staged, nothing logged loudly
    Staged,    ///< module (+PDB when available) staged with a relative record + engine_abi marker
    Failed     ///< the project HAS a native module but it could not be packaged — fatal to the build
};

/// Engine ABI digest variants for the C2 staleness gate. Package compile
/// defines are digest inputs, so a changed enabled-package set changes the
/// digest without any engine change — the gate compares the record against
/// the variants to attribute a mismatch honestly instead of reporting every
/// package-set change as "native user scripts are stale".
struct EngineAbiDigestSet
{
    /// Shipping engine + the current effective package defines (the digest the
    /// staged record must match).
    std::string Current;
    /// Shipping engine with NO package defines. Matches records built before
    /// the current defines existed (a package was enabled since the build).
    std::string WithoutPackageDefines;
    /// Shipping engine + the defines of enabled AND disabled packages. Matches
    /// records built while a now-disabled package was still enabled. Empty when
    /// the caller has no disabled-set information.
    std::string WithAllPackageDefines;
};

/// Platform-neutral ship-safety step (C1/C2): reads the native build record
/// (<projectRoot>/.Cache/NativeScripts/build/last_build.txt, or
/// <cacheDir>/NativeScripts/build when a package module's writable cache dir
/// is passed instead of a project root), refuses to
/// package a module that is stale relative to `engineAbiDigests.Current` (the
/// identity of the engine this package ships), and stages into
/// <contentRoot>/NativeScripts:
///   - the user module DLL/.so (renamed back to its plain module name),
///   - its PDB when requested and found beside the build (Windows),
///   - a RELATIVE last_build.txt the Player resolves against its own root,
///   - the engine_abi marker LoadPrebuiltUserModule validates the record against.
/// A "successful" build that silently omits the user's C++ gameplay is worse than
/// a failed one, so every problem past "the project has no native scripts" appends
/// to `errors` and returns Failed. A macOS bundle stages here too, into its
/// Contents/Resources; MacBundleAssembler::MoveNativeModulesIntoFrameworks then
/// moves the image into Contents/Frameworks and rewrites the record.
NativeScriptStageOutcome StageNativeUserScriptsForPackage(const std::filesystem::path& projectRoot,
                                                          const std::filesystem::path& contentRoot,
                                                          const EngineAbiDigestSet& engineAbiDigests,
                                                          std::vector<std::string>& errors,
                                                          bool includeDebugSymbols);

} // namespace GameEngine
