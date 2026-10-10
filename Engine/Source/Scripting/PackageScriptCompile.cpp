#include "Scripting/PackageScriptCompile.h"
#include "Scripting/ScriptTargetFramework.h"

#include "Logger/Logger.h"

#include <sstream>
#include <system_error>

namespace GameEngine
{

namespace
{

// Matches the native client's long-standing convention (and the server's
// ShouldIncludeSource filter): paths are emitted as generic (forward-slash)
// strings with no further escaping — the engine never places quotes or
// backslashes in the roots it controls.
void AppendJsonPathArray(std::ostringstream& json, const char* field,
                         const std::vector<std::filesystem::path>& paths)
{
    json << "\"" << field << "\":[";
    bool first = true;
    for (const auto& p : paths)
    {
        if (!first)
            json << ",";
        first = false;
        json << "\"" << p.generic_string() << "\"";
    }
    json << "],";
}

// Enumerate user-authored .cs sources under `root`, skipping generated trees
// (obj/bin/ScriptAssemblies) and MSBuild-generated files — the same filters the
// project-scripts request applies, so package modules and project scripts see
// identical inclusion rules.
void AppendJsonSourceFiles(std::ostringstream& json, const std::vector<std::filesystem::path>& roots)
{
    json << "\"AllFiles\":[";
    bool first = true;
    for (const auto& root : roots)
    {
        if (root.empty())
            continue;
        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(root, ec);
        if (ec)
            continue;
        for (; it != std::filesystem::recursive_directory_iterator(); ++it)
        {
            try
            {
                if (it->is_directory())
                {
                    const std::string name = it->path().filename().string();
                    if (name == "obj" || name == "bin" || name == "ScriptAssemblies")
                    {
                        it.disable_recursion_pending();
                        continue;
                    }
                }
                if (!it->is_regular_file() || it->path().extension() != ".cs")
                    continue;
                const std::string fileName = it->path().filename().string();
                const std::string pstr = it->path().generic_string();
                if (fileName == "AssemblyInfo.cs" ||
                    (fileName.size() > 5 && fileName.rfind(".g.cs") == fileName.size() - 5) ||
                    pstr.find(".NETCoreApp,Version=") != std::string::npos ||
                    pstr.find("/obj/") != std::string::npos)
                {
                    continue;
                }
                if (!first)
                    json << ",";
                first = false;
                json << "\"" << pstr << "\"";
            }
            catch (...)
            { /* ignore bad entries */
            }
        }
    }
    json << "],";
}

} // namespace

std::string BuildCompileServerRequestJson(const CompileServerRequestDesc& desc)
{
    std::ostringstream json;
    json << "{";
    json << "\"ProjectRoot\":\"" << desc.ProjectRoot.generic_string() << "\",";
    if (!desc.AssemblyName.empty())
        json << "\"AssemblyName\":\"" << desc.AssemblyName << "\",";
    json << "\"Config\":\"" << desc.Config << "\",";
    json << "\"Tfm\":\"" << desc.Tfm << "\",";
    if (!desc.EngineBinDir.empty())
        json << "\"EngineBinDir\":\"" << desc.EngineBinDir.generic_string() << "\",";
    if (!desc.References.empty())
        AppendJsonPathArray(json, "References", desc.References);
    if (!desc.Defines.empty())
    {
        json << "\"Defines\":[";
        bool first = true;
        for (const std::string& d : desc.Defines)
        {
            if (!first)
                json << ",";
            first = false;
            json << "\"" << d << "\"";
        }
        json << "],";
    }
    if (desc.AllowUnsafe)
        json << "\"AllowUnsafe\":true,";
    if (desc.ImplicitUsings)
        json << "\"ImplicitUsings\":true,";
    if (desc.Nullable)
        json << "\"Nullable\":true,";
    json << "\"ChangedFiles\":[],\"AffectedFiles\":[],";
    AppendJsonSourceFiles(json, desc.SourceRoots);
    json << "\"PreferredStrategy\":\"Full\",\"ForceFull\":true";
    json << "}";
    return json.str();
}

std::string GeneratePackageModuleCsprojXml(const PackageCodeModule& module,
                                           const std::filesystem::path& projectDir,
                                           const std::filesystem::path& outputDir,
                                           const std::filesystem::path& engineBinDir,
                                           const std::vector<std::filesystem::path>& dependencyAssemblyPaths)
{
    // Sources glob: csproj-relative when the module root sits inside the package
    // (always true for a discovered root) so the pair survives moving the
    // package; absolute otherwise.
    std::error_code ec;
    std::filesystem::path rel = std::filesystem::relative(module.RootDir, projectDir, ec);
    std::string includeRoot = (!ec && !rel.empty()) ? rel.generic_string() : module.RootDir.generic_string();
    if (!includeRoot.empty() && includeRoot.back() != '/')
        includeRoot += '/';
    std::string includeRootWin = includeRoot;
    for (char& ch : includeRootWin)
    {
        if (ch == '/')
            ch = '\\';
    }

    std::string engineBinDirStr = engineBinDir.generic_string();
    if (!engineBinDirStr.empty() && engineBinDirStr.back() != '/')
        engineBinDirStr += '/';
    std::string outputDirStr = outputDir.generic_string();
    if (!outputDirStr.empty() && outputDirStr.back() != '/')
        outputDirStr += '/';

    std::ostringstream xml;
    xml << "<Project Sdk=\"Microsoft.NET.Sdk\">\n";
    xml << "  <PropertyGroup>\n";
    xml << "    <TargetFramework>" << kScriptTargetFramework << "</TargetFramework>\n";
    xml << "    <OutputType>Library</OutputType>\n";
    xml << "    <AssemblyName>" << module.AssemblyName << "</AssemblyName>\n";
    xml << "    <RootNamespace>" << module.AssemblyName << "</RootNamespace>\n";
    // The language settings CompileServerRequestDesc sends on the live path.
    xml << "    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>\n";
    xml << "    <ImplicitUsings>enable</ImplicitUsings>\n";
    xml << "    <Nullable>enable</Nullable>\n";
    xml << "    <EnableDefaultItems>false</EnableDefaultItems>\n";
    xml << "    <AppendTargetFrameworkToOutputPath>false</AppendTargetFrameworkToOutputPath>\n";
    xml << "    <AppendRuntimeIdentifierToOutputPath>false</AppendRuntimeIdentifierToOutputPath>\n";
    xml << "    <OutputPath>" << outputDirStr << "</OutputPath>\n";
    xml << "    <EngineBinDir Condition=\"'$(EngineBinDir)' == ''\">" << engineBinDirStr << "</EngineBinDir>\n";
    if (!module.Defines.empty())
    {
        xml << "    <DefineConstants>$(DefineConstants)";
        for (const std::string& d : module.Defines)
            xml << ";" << d;
        xml << "</DefineConstants>\n";
    }
    xml << "    <ProduceReferenceAssembly>false</ProduceReferenceAssembly>\n";
    xml << "  </PropertyGroup>\n";
    xml << "  <ItemGroup>\n";
    // obj/bin are MSBuild output trees; their generated AssemblyInfo .cs files
    // must never join the compile (CS0579 duplicate attributes).
    xml << "    <Compile Include=\"" << includeRootWin << "**\\*.cs\" Exclude=\""
        << includeRootWin << "**\\obj\\**;" << includeRootWin << "**\\bin\\**\" />\n";
    xml << "  </ItemGroup>\n";

    // Engine ABI references, enumerated at generation time (same rationale as the
    // project-scripts csproj: reliable MSBuild resolution over item transforms).
    xml << "  <ItemGroup>\n";
    {
        int abiCount = 0;
        std::error_code iterEc;
        for (std::filesystem::directory_iterator it(engineBinDir, iterEc), end; !iterEc && it != end;
             it.increment(iterEc))
        {
            if (!it->is_regular_file())
                continue;
            const std::string fn = it->path().filename().string();
            if (fn.size() > 19 && fn.rfind("GameEngine.", 0) == 0 &&
                fn.compare(fn.size() - 8, 8, ".ABI.dll") == 0)
            {
                xml << "    <Reference Include=\"" << fn.substr(0, fn.size() - 4) << "\">\n";
                xml << "      <HintPath>$(EngineBinDir)" << fn << "</HintPath>\n";
                xml << "      <Private>false</Private>\n";
                xml << "    </Reference>\n";
                ++abiCount;
            }
        }
        if (abiCount == 0)
        {
            Logger::Log::Warning("[Packages] no GameEngine.*.ABI.dll found in '{}' while generating the csproj "
                                 "for package module '{}'",
                                 engineBinDir.generic_string(), module.AssemblyName);
        }
    }
    xml << "  </ItemGroup>\n";
    // Scripting.Runtime (GameSystemRunner in source-generated code).
    xml << "  <ItemGroup Condition=\"Exists('$(EngineBinDir)GameEngine.Scripting.Runtime.dll')\">\n";
    xml << "    <Reference Include=\"GameEngine.Scripting.Runtime\">\n";
    xml << "      <HintPath>$(EngineBinDir)GameEngine.Scripting.Runtime.dll</HintPath>\n";
    xml << "      <Private>false</Private>\n";
    xml << "    </Reference>\n";
    xml << "  </ItemGroup>\n";
    // Editor-kind modules compile against the editor scripts assembly.
    if (module.Kind == PackageModuleRecord::ModuleKind::Editor)
    {
        xml << "  <ItemGroup Condition=\"Exists('$(EngineBinDir)GameEngine.Editor.dll')\">\n";
        xml << "    <Reference Include=\"GameEngine.Editor\">\n";
        xml << "      <HintPath>$(EngineBinDir)GameEngine.Editor.dll</HintPath>\n";
        xml << "      <Private>false</Private>\n";
        xml << "    </Reference>\n";
        xml << "  </ItemGroup>\n";
    }
    // Dependency package assemblies.
    if (!dependencyAssemblyPaths.empty())
    {
        xml << "  <ItemGroup>\n";
        for (const auto& dep : dependencyAssemblyPaths)
        {
            xml << "    <Reference Include=\"" << dep.stem().generic_string() << "\">\n";
            xml << "      <HintPath>" << dep.generic_string() << "</HintPath>\n";
            xml << "      <Private>false</Private>\n";
            xml << "    </Reference>\n";
        }
        xml << "  </ItemGroup>\n";
    }
    // Source generator (component schemas + entity-system registration hubs).
    xml << "  <ItemGroup Condition=\"Exists('$(EngineBinDir)SourceGenerators/EntitySystemGenerator.dll')\">\n";
    xml << "    <Analyzer Include=\"$(EngineBinDir)SourceGenerators/EntitySystemGenerator.dll\" />\n";
    xml << "  </ItemGroup>\n";
    xml << "</Project>\n";
    return xml.str();
}

} // namespace GameEngine
