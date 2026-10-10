#pragma once

#include "Assets/Packages/PackageCodeModules.h"

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{

/// Inputs of the AOT project a ship-optimized build publishes (one assembly for
/// the project scripts and every runtime package C# module).
struct NativeAotProjectDesc
{
    std::filesystem::path ScriptSources;               ///< project scripts root, globbed recursively
    std::vector<PackageCodeModule> PackageModules;     ///< only runtime C# modules are compiled in
    std::vector<std::string> PackageDefines;           ///< union of enabled-package defines
    std::vector<std::filesystem::path> ReferenceDlls;  ///< staged SDK assemblies, referenced by HintPath
    std::filesystem::path GeneratorDll;                ///< source generator; empty when not staged
};

/// The text of <kNativeAotProjectName>.csproj for `dotnet publish /p:NativeLib=Shared`.
std::string GenerateNativeAotCsprojXml(const NativeAotProjectDesc& desc);

} // namespace GameEngine
