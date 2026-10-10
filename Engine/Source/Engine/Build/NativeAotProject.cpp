#include "Engine/Build/NativeAotProject.h"

#include "Engine/Build/NativeAotPublish.h"
#include "Scripting/ScriptTargetFramework.h"

#include <sstream>

namespace GameEngine
{

namespace
{

namespace fs = std::filesystem;

// Escapes the characters XML reserves in attribute values and text.
std::string XmlEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c; break;
        }
    }
    return out;
}

// obj/bin hold MSBuild outputs — including generated AssemblyInfo/
// attribute .cs files from any csproj that ever built under a root
// (the editor's own script compiles do). Sweeping them into the AOT
// compile duplicates assembly attributes (CS0579). Mirrors the
// generated hot-reload csproj's Exclude (ScriptManager.cpp).
std::string ObjBinExclude(const fs::path& root)
{
    return XmlEscape((root / "**" / "obj" / "**").generic_string()) + ";" +
           XmlEscape((root / "**" / "bin" / "**").generic_string());
}

} // namespace

std::string GenerateNativeAotCsprojXml(const NativeAotProjectDesc& desc)
{
    std::string packageDefines;
    for (const std::string& define : desc.PackageDefines)
        packageDefines += ";" + define;

    // The language settings are the live compile's (CompileServerRequestDesc) and
    // the generated hot-reload csprojs', so code that compiles in the editor
    // compiles in the export.
    std::ostringstream f;
    f << R"(<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>)" << kScriptTargetFramework << R"(</TargetFramework>
    <PublishAot>true</PublishAot>
    <OptimizationPreference>Speed</OptimizationPreference>
    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
    <ImplicitUsings>enable</ImplicitUsings>
    <Nullable>enable</Nullable>)";
    if (!packageDefines.empty())
    {
        f << R"(
    <DefineConstants>$(DefineConstants))" << XmlEscape(packageDefines) << R"(</DefineConstants>)";
    }

    const fs::path scriptGlob = desc.ScriptSources / "**" / "*.cs";
    f << R"(
  </PropertyGroup>
  <ItemGroup>
    <Compile Include=")" << XmlEscape(scriptGlob.generic_string())
      << R"(" Exclude=")" << ObjBinExclude(desc.ScriptSources) << R"(" />)";

    // Package runtime C# modules compile INTO the single AOT assembly: their
    // sources join the project's (cross-package references resolve inside the
    // one compilation) and the union of package defines applies. A module
    // without sources ships its declared prebuilt assembly instead, linked by
    // reference and rooted below.
    std::vector<const PackageCodeModule*> prebuiltModules;
    for (const PackageCodeModule& module : desc.PackageModules)
    {
        if (module.Lang != PackageModuleRecord::ModuleLang::CSharp ||
            module.Kind != PackageModuleRecord::ModuleKind::Runtime)
            continue;
        if (module.RootDir.empty())
        {
            prebuiltModules.push_back(&module);
            f << R"(
    <Reference Include=")" << XmlEscape(module.AssemblyName) << R"(">
      <HintPath>)" << XmlEscape((module.PrebuiltDir / (module.AssemblyName + ".dll")).generic_string())
              << R"(</HintPath>
    </Reference>)";
            continue;
        }
        const fs::path moduleGlob = module.RootDir / "**" / "*.cs";
        f << R"(
    <Compile Include=")" << XmlEscape(moduleGlob.generic_string())
          << R"(" Exclude=")" << ObjBinExclude(module.RootDir) << R"(" />)";
    }

    for (const fs::path& dll : desc.ReferenceDlls)
    {
        f << R"(
    <Reference Include=")" << XmlEscape(dll.stem().generic_string()) << R"(">
      <HintPath>)" << XmlEscape(dll.generic_string()) << R"(</HintPath>
    </Reference>)";
    }

    // The ge_scripts_* UnmanagedCallersOnly exports the Player resolves via
    // GetProcAddress live in GameEngine.Scripting.Runtime (a plain Reference).
    // ILC only emits native exports for the compiled assembly itself unless a
    // referenced assembly is named via --generateunmanagedentrypoints (this
    // item) — without it the published library exports nothing and the
    // packaged game's C# silently never ticks.
    //
    // The user scripts assembly must be rooted as well: its only entry points
    // are the source-generated [ModuleInitializer] registrations, which no
    // export references — unrooted, ILC trims the whole module, the module
    // initializer never runs at runtime startup, and GameSystemRunner
    // initializes with zero systems (CoreCLR avoids this by force-running
    // module constructors on assembly load; see HotReloadManager). This root
    // is also the invariant GameSystemRunner.DiscoverGameSystems' trim-warning
    // suppressions rely on (reflection over the rooted assembly cannot lose
    // types) — keep them in sync. A prebuilt package assembly is rooted for
    // the same reason: its module initializers are its only entry points.
    f << R"(
    <UnmanagedEntryPointsAssembly Include="GameEngine.Scripting.Runtime" />
    <TrimmerRootAssembly Include=")" << kNativeAotProjectName << R"(" />)";
    for (const PackageCodeModule* module : prebuiltModules)
    {
        f << R"(
    <TrimmerRootAssembly Include=")" << XmlEscape(module->AssemblyName) << R"(" />)";
    }

    if (!desc.GeneratorDll.empty())
    {
        f << R"(
    <Analyzer Include=")" << XmlEscape(desc.GeneratorDll.generic_string()) << R"(" />)";
    }

    f << R"(
  </ItemGroup>
</Project>
)";
    return f.str();
}

} // namespace GameEngine
