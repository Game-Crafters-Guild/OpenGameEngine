#pragma once

// P1 package C# modules — the pure/testable halves of the package compile path:
// the compile-server request JSON builder (mirrors CompileServerHost's
// BuildRequest record, including the References/Defines fields and the
// AllowUnsafe/ImplicitUsings/Nullable language settings) and the
// generated-csproj XML for a package module (IDE entry point + dotnet CLI
// fallback; the live compile path is the request, not the csproj).
// Orchestration (compile order, server round-trip, DLL writes) stays in
// ScriptManager.

#include "Assets/Packages/PackageCodeModules.h"
#include "Scripting/ScriptTargetFramework.h"

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{

// One compile-server request. SourceRoots are enumerated recursively for .cs
// files with the same filters the server applies (obj/bin/ScriptAssemblies
// trees, MSBuild-generated sources).
struct CompileServerRequestDesc
{
    std::filesystem::path ProjectRoot; // workspace key on the server (csproj dir)
    std::string AssemblyName;          // empty => server default identity
    std::string Config = "Debug";
    std::string Tfm = kScriptTargetFramework;
    std::filesystem::path EngineBinDir;
    std::vector<std::filesystem::path> SourceRoots;
    std::vector<std::filesystem::path> References; // extra absolute reference paths
    std::vector<std::string> Defines;              // preprocessor symbols
    // Language settings every generated csproj declares, so the live compile and
    // `dotnet build` of the csproj agree. True is the engine's policy for project
    // scripts, editor scripts and package modules alike; each is sent only when true
    // (an absent field means false to the server).
    bool AllowUnsafe = true;    // <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
    bool ImplicitUsings = true; // <ImplicitUsings>enable</ImplicitUsings>
    bool Nullable = true;       // <Nullable>enable</Nullable>
};

std::string BuildCompileServerRequestJson(const CompileServerRequestDesc& desc);

// Generated SDK-style csproj for one package C# module: AssemblyName from the
// deterministic package mapping, sources globbed from the module root
// (csproj-relative when both stay inside the package root), DefineConstants
// from the module's effective defines, engine ABI references enumerated from
// engineBinDir, GameEngine.Editor.dll referenced for Editor-kind modules, and
// dependency package assemblies referenced by absolute HintPath. OutputDir is
// emitted absolute — the file lives in the package's .Cache and regenerates on
// every editor open, so it never outlives a relocation.
std::string GeneratePackageModuleCsprojXml(const PackageCodeModule& module,
                                           const std::filesystem::path& projectDir,
                                           const std::filesystem::path& outputDir,
                                           const std::filesystem::path& engineBinDir,
                                           const std::vector<std::filesystem::path>& dependencyAssemblyPaths);

} // namespace GameEngine
