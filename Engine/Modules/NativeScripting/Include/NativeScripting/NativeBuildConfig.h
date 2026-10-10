#pragma once

// NativeBuildConfig / NativeBuildResult — inputs and outputs of the C10
// generate→compile→load→register pipeline.
//
// The config describes HOW to build a user module against this engine: where the
// user sources live, where to build, and the engine's build interface (include
// dirs + import lib + compile defs). In C10 the caller supplies the engine
// interface (the integration test reads it from CMake generator expressions; the
// editor-runtime path will read a staged SDK manifest in a later slice).

#include "Types/Types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{
namespace NativeScripting
{

struct NativeBuildConfig
{
    // Directory holding the user's .cpp/.h sources (the generated CMakeLists.txt
    // globs all *.cpp under it into the user DLL).
    std::filesystem::path SourceDir;

    // The package a module belongs to; empty for a project's own scripts. Its
    // top-level Tests folder never compiles into the module, even when SourceDir
    // is the package root (AssetIgnoreRules::IgnorePackageTestsFolder).
    std::filesystem::path PackageRootDir;

    // Directory that receives the generated CMakeLists.txt and is passed to
    // `cmake -S`. Empty keeps the legacy SourceDir behavior for user projects.
    // Package sources can be installed inside a signed/read-only app bundle, so
    // their generated project lives under the package's writable derived cache.
    std::filesystem::path ProjectDir;

    // The user project's OWN build directory — kept separate from the engine's
    // build dir so user-DLL builds never contend with the engine build.
    std::filesystem::path BuildDir;

    // Where freshly-built user DLLs are shadow-copied to (versioned) before load,
    // so a rebuild never fights a loaded/locked DLL. Defaults to BuildDir/active.
    std::filesystem::path ActiveDir;

    std::filesystem::path CMakeExe;   // path to the cmake executable
    std::string           Config = "Debug"; // build config; MUST match the host engine's

    // The engine's build interface — exactly what a consumer of the Engine import
    // lib needs. Mirrors Engine's INTERFACE_INCLUDE_DIRECTORIES / link lib / defs.
    std::vector<std::filesystem::path> IncludeDirs;
    std::filesystem::path              EngineImportLib;
    std::vector<std::string>           CompileDefinitions;

    // EditorSDK interface (native Editor-kind package modules only): the
    // EditorSDK import lib + the staged editor headers. Both empty for
    // Runtime modules and in Player layouts (the editor stages the EditorSDK;
    // nothing else does). When set, the generated project links the lib, the
    // include dirs join the compile, and the identity folds into the ABI
    // digest so an editor-surface relink invalidates cached module DLLs.
    std::filesystem::path              EditorImportLib;
    std::vector<std::filesystem::path> EditorIncludeDirs;

    // Root of the staged engine SDK the interface above lives under. When set, the
    // generated CMakeLists references SDK paths through the GAMEENGINE_SDK_DIR cmake
    // variable (passed live on every engine-driven configure) instead of baking this
    // machine's absolute paths; the baked default in the file is only a last-known-good
    // for IDE / manual configures. Empty → paths are emitted absolute (out-of-SDK
    // layouts, e.g. the integration test's engine-build-tree interface).
    std::filesystem::path SdkRoot;

    // SDK source compiled into every user DLL: provides the four ABI exports so the
    // user never writes them. Added to the generated target's sources.
    std::filesystem::path SdkEntrySource;

    // Component scanner (optional). When both DotnetExe and ComponentScannerDll are
    // set, the pipeline runs the scanner over SourceDir before building and compiles
    // the generated registration into the user DLL — so `struct X : ECS::ComponentBase`
    // is auto-registered with no macro. Leave empty to use explicit macros only.
    std::filesystem::path DotnetExe;
    std::filesystem::path ComponentScannerDll;
    std::string           DetectBase = "ComponentBase"; // inheritors of this base are detected

    // Fast-build toolchain (filled by the manager from the detected MsvcToolchain). When
    // NinjaExe is set, the build uses the Ninja generator — far less per-build overhead
    // than MSBuild, so warm rebuilds are quick and "cold == warm" with no resident server.
    // MsvcEnvBatch (Windows) is a .bat that sets the cached MSVC environment, prepended to
    // the configure/build commands so cl.exe finds its headers/libs without re-running
    // vcvars. Both empty → fall back to the platform default generator (Visual Studio).
    std::filesystem::path NinjaExe;
    std::filesystem::path MsvcEnvBatch;

    // Logical name for the generated user-DLL target / output (e.g. "UserScripts").
    std::string ModuleName = "UserScripts";

    // P3 packages: absolute root of the package's shipped prebuilt binaries
    // (manifest `prebuilt` dir). When set, the manager tries the
    // host-fingerprint-matching DLL under it BEFORE building from source
    // (PrebuiltModuleBinaries.h); absent/mismatched prebuilts fall back to the
    // source build with a loud log. Empty = source-only (the project module).
    std::filesystem::path PrebuiltDir;
};

inline std::filesystem::path EffectiveProjectDir(const NativeBuildConfig& config)
{
    return config.ProjectDir.empty() ? config.SourceDir : config.ProjectDir;
}

// Appends package-provided compile defines to a user-module build config.
// Compile definitions are engine-ABI digest inputs (HashEngineAbiInputs), so the
// editor's live module builds and the package pipeline's digest recomputation
// must append the same set in the same order — both go through this one helper,
// or packaging falsely rejects fresh builds as stale (C2 gate).
inline void AppendPackageDefines(NativeBuildConfig& config, const std::vector<std::string>& defines)
{
    config.CompileDefinitions.insert(config.CompileDefinitions.end(), defines.begin(), defines.end());
}

struct NativeBuildResult
{
    bool        Configured = false; // cmake configure succeeded (or cache present)
    bool        Compiled   = false; // cmake --build succeeded
    bool        Loaded      = false; // DLL loaded + ABI version accepted
    bool        Registered  = false; // Register_v1 returned success
    std::string Output;             // merged build output (for the console)
    std::string Error;              // a one-line failure summary, empty on success
    std::filesystem::path LoadedModulePath; // the shadow-copied DLL actually loaded
    std::string ModuleId;           // logical module whose result is being reported
    // True only when this result mapped a new module image and ran its registrars.
    // Cached callbacks for an image already mapped in this process leave it false.
    bool RegistrationsChanged = false;

    // Error is set on every failure path, so it is the success predicate. The
    // Configured/Compiled/Loaded/Registered flags stay as a "which stage failed"
    // diagnostic for the build console and tests.
    bool Success() const { return Error.empty(); }
};

} // namespace NativeScripting
} // namespace GameEngine
