#pragma once

/// Where the build put the managed assemblies the scripting-ABI suites load.
///
/// Two configuration names are in play, and they name different things:
///   GE_BUILD_CONFIG           this binary's own configuration ($<CONFIG>), which
///                             also names the staged native tree, bin/<config>/.
///   GE_MANAGED_FIXTURE_CONFIG the configuration the managed test fixtures
///                             (DomainRoutingTest, GameEngine.Scripts) were built
///                             into. Managed fixtures carry no native ABI
///                             coupling, so the build pins one value across every
///                             engine configuration instead of tracking $<CONFIG>.
///
/// Both are defined by Tests/CMakeLists.txt, which builds and stages the fixtures
/// from the same two values. Deriving every fixture path from them here is what
/// keeps these suites runnable in a configuration other than Debug.
///
/// Paths anchor to StagedRoot() — the build root derived from the executable's
/// own location — never to the current working directory: EngineCore::Initialize
/// moves the process working directory to the executable directory
/// (Engine/Source/Core/PathUtils.cpp), so a CWD-relative fixture path resolves
/// differently before and after the first test that initializes the engine.

#include "StagedTestPaths.h"

#include <filesystem>

#if !defined(GE_BUILD_CONFIG) || !defined(GE_MANAGED_FIXTURE_CONFIG)
#error "Tests/CMakeLists.txt must define GE_BUILD_CONFIG and GE_MANAGED_FIXTURE_CONFIG for this target"
#endif

namespace GameEngine::Tests
{

/// The staged native tree, <build>/bin/<config>.
inline std::filesystem::path StagedBinDir()
{
    return TestPaths::StagedRoot() / "bin" / GE_BUILD_CONFIG;
}

/// The DomainRoutingTest fixture's dotnet output directory, as
/// CopyDomainRoutingTest mirrors it under the build root.
inline std::filesystem::path DomainRoutingTestDir()
{
    return TestPaths::StagedRoot() / "Tests" / "ManagedTestAssemblies" / "DomainRoutingTest" / "bin"
           / GE_MANAGED_FIXTURE_CONFIG / "net10.0";
}

/// The DomainRoutingTest fixture assembly.
inline std::filesystem::path DomainRoutingTestDll()
{
    return DomainRoutingTestDir() / "DomainRoutingTest.dll";
}

/// ScriptAssemblies/<config>/net10.0, the layout BuildAndStageScriptsAssembly.cmake
/// and StageCoreBridgeAndHotReload stage managed assemblies into.
inline std::filesystem::path ScriptAssembliesDir()
{
    return StagedBinDir() / "ScriptAssemblies" / GE_MANAGED_FIXTURE_CONFIG / "net10.0";
}

/// Why a suite skips when FindScriptsAssemblyDll() comes back empty. The engine
/// emits GameEngine.Scripts.csproj into the staged ScriptAssemblies tree on its
/// first run, so BuildScriptsAssembly has nothing to compile in a worktree that
/// has never run the editor or player, and says so at build time. Suites that
/// need this assembly skip rather than fail; where it exists they still assert.
inline constexpr const char* kScriptsAssemblyMissingReason =
    "GameEngine.Scripts.dll not built: the engine emits its project on first run, so a "
    "worktree that has never run the editor or player has no scripts assembly to load. "
    "Run the editor once, then rebuild, to exercise this test.";

/// GameEngine.Scripts.dll, or an empty path when the assembly was never built.
/// The engine emits its project on first run, so BuildScriptsAssembly stages it
/// into the staged ScriptAssemblies tree; the exe-adjacent layout is what an
/// editor or player run rooted at a project directory produces instead.
inline std::filesystem::path FindScriptsAssemblyDll()
{
    const std::filesystem::path candidates[] = {
        ScriptAssembliesDir() / "GameEngine.Scripts.dll",
        TestPaths::ExecutableDirectory() / "ScriptAssemblies" / GE_MANAGED_FIXTURE_CONFIG / "net10.0"
            / "GameEngine.Scripts.dll",
        TestPaths::ExecutableDirectory() / "ScriptAssemblies" / "GameEngine.Scripts.dll",
    };
    for (const auto& candidate : candidates)
    {
        if (std::filesystem::exists(candidate)) return candidate;
    }
    return {};
}

}  // namespace GameEngine::Tests
