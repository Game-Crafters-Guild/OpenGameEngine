// P1 package C# modules: the compile-server request builder (References/Defines
// and the language-setting protocol fields), the generated package csproj shape,
// and end-to-end compiles through a REAL resident CompileServerHost proving that
// (a) a package assembly gets its own AssemblyName, (b) extra References resolve
// a dependency package assembly, (c) Defines reach the parse options, and (d) the
// live compile accepts what `dotnet build` of the generated csproj accepts;
// and the prebuilt rule: a declared DLL stands in for a module with no sources.

#include "CompileServerTestHost.h"
#include "Core/Application.h" // PathUtils
#include "Engine/Build/DotnetHost.h"
#include "EngineLogCapture.h" // Engine/Tests
#include "JobSystem/WorkStealingThreadPool.h"
#include "Jobs/CompileServerClient.h"
#include "Jobs/CompileServerTestDoubles.h"
#include "Jobs/HotReloadTestHooks.h"
#include "Jobs/IHotReloadTransport.h"
#include "Jobs/PipeTransport.h"
#include "Jobs/ScopedHotReloadTestHooks.h"
#include "Scripting/PackageScriptCompile.h"
#include "Scripting/ScriptManager.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace GameEngine;
namespace fs = std::filesystem;

namespace
{

fs::path MakeTempDir(const char* tag)
{
    const fs::path dir = fs::temp_directory_path() / (std::string("ge_pkgcs_") + tag);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void WriteFileAt(const fs::path& p, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::trunc);
    f << text;
}

std::vector<char> ReadBytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

PackageCodeModule MakeCsModule(const fs::path& pkgRoot, bool editorKind = false)
{
    PackageCodeModule module;
    module.PackageName = "ocean-pack";
    module.AssemblyName = editorKind ? "OceanPack.Editor" : "OceanPack";
    module.Kind = editorKind ? PackageModuleRecord::ModuleKind::Editor : PackageModuleRecord::ModuleKind::Runtime;
    module.Lang = PackageModuleRecord::ModuleLang::CSharp;
    module.RootDir = pkgRoot / "Runtime";
    module.PackageRootDir = pkgRoot;
    module.Defines = {"WATER_CORE", "OCEAN_PACK"};
    module.DependencyAssemblies = {"WaterCore"};
    module.DependencyPackages = {"water-core"};
    return module;
}

} // namespace

TEST(PackageCompileRequest, JsonCarriesAssemblyNameReferencesAndDefines)
{
    const fs::path root = MakeTempDir("req");
    WriteFileAt(root / "src" / "Ocean.cs", "public class Ocean {}\n");
    // Filtered: generated trees + MSBuild-generated files never reach the server.
    WriteFileAt(root / "src" / "obj" / "Gen.cs", "class Gen {}\n");
    WriteFileAt(root / "src" / "bin" / "Bin.cs", "class Bin {}\n");
    WriteFileAt(root / "src" / "AssemblyInfo.cs", "// generated\n");

    CompileServerRequestDesc desc;
    desc.ProjectRoot = root / "proj";
    desc.AssemblyName = "OceanPack";
    desc.EngineBinDir = root / "engine";
    desc.SourceRoots = {root / "src"};
    desc.References = {root / "Packages" / "WaterCore.dll"};
    desc.Defines = {"OCEAN_PACK", "WATER_CORE"};

    const std::string json = BuildCompileServerRequestJson(desc);
    EXPECT_NE(json.find("\"AssemblyName\":\"OceanPack\""), std::string::npos) << json;
    EXPECT_NE(json.find("\"References\":["), std::string::npos) << json;
    EXPECT_NE(json.find("WaterCore.dll"), std::string::npos) << json;
    EXPECT_NE(json.find("\"Defines\":[\"OCEAN_PACK\",\"WATER_CORE\"]"), std::string::npos) << json;
    EXPECT_NE(json.find("Ocean.cs"), std::string::npos) << json;
    EXPECT_NE(json.find("\"ForceFull\":true"), std::string::npos) << json;
    // The filters must drop generated sources.
    EXPECT_EQ(json.find("Gen.cs"), std::string::npos) << json;
    EXPECT_EQ(json.find("Bin.cs"), std::string::npos) << json;
    EXPECT_EQ(json.find("AssemblyInfo.cs"), std::string::npos) << json;
}

TEST(PackageCompileRequest, JsonCarriesLanguageSettingsByDefault)
{
    CompileServerRequestDesc desc;
    desc.ProjectRoot = "X:/proj";
    const std::string json = BuildCompileServerRequestJson(desc);
    EXPECT_NE(json.find("\"AllowUnsafe\":true"), std::string::npos) << json;
    EXPECT_NE(json.find("\"ImplicitUsings\":true"), std::string::npos) << json;
    EXPECT_NE(json.find("\"Nullable\":true"), std::string::npos) << json;
}

TEST(PackageCompileRequest, OmitsOptionalFieldsWhenEmpty)
{
    CompileServerRequestDesc desc;
    desc.ProjectRoot = "X:/proj";
    desc.AllowUnsafe = false;
    desc.ImplicitUsings = false;
    desc.Nullable = false;
    const std::string json = BuildCompileServerRequestJson(desc);
    EXPECT_EQ(json.find("\"AssemblyName\""), std::string::npos) << json;
    EXPECT_EQ(json.find("\"References\""), std::string::npos) << json;
    EXPECT_EQ(json.find("\"Defines\""), std::string::npos) << json;
    EXPECT_EQ(json.find("\"AllowUnsafe\""), std::string::npos) << json;
    EXPECT_EQ(json.find("\"ImplicitUsings\""), std::string::npos) << json;
    EXPECT_EQ(json.find("\"Nullable\""), std::string::npos) << json;
}

TEST(PackageModuleCsproj, RuntimeModuleShape)
{
    const fs::path pkgRoot = MakeTempDir("csproj");
    const PackageCodeModule module = MakeCsModule(pkgRoot);
    const fs::path projectDir = pkgRoot / ".Cache" / "ScriptProjects";
    const fs::path outDir = fs::path("X:/ws/ScriptAssemblies/Packages");
    const fs::path engineBin = pkgRoot / "engine"; // empty — no ABI dlls, warning only
    const std::vector<fs::path> deps = {outDir / "WaterCore.dll"};

    const std::string xml = GeneratePackageModuleCsprojXml(module, projectDir, outDir, engineBin, deps);
    EXPECT_NE(xml.find("<AssemblyName>OceanPack</AssemblyName>"), std::string::npos) << xml;
    EXPECT_NE(xml.find("<OutputPath>X:/ws/ScriptAssemblies/Packages/</OutputPath>"), std::string::npos) << xml;
    // The language settings the compile-server request carries.
    EXPECT_NE(xml.find("<AllowUnsafeBlocks>true</AllowUnsafeBlocks>"), std::string::npos) << xml;
    EXPECT_NE(xml.find("<ImplicitUsings>enable</ImplicitUsings>"), std::string::npos) << xml;
    EXPECT_NE(xml.find("<Nullable>enable</Nullable>"), std::string::npos) << xml;
    EXPECT_NE(xml.find("<DefineConstants>$(DefineConstants);WATER_CORE;OCEAN_PACK</DefineConstants>"),
              std::string::npos)
        << xml;
    // Sources glob is csproj-relative (module root inside the package root)...
    EXPECT_NE(xml.find("..\\..\\Runtime\\**\\*.cs"), std::string::npos) << xml;
    // ...and excludes MSBuild output trees (their generated AssemblyInfo .cs
    // files duplicate assembly attributes — CS0579).
    EXPECT_NE(xml.find("Exclude=\"..\\..\\Runtime\\**\\obj\\**;..\\..\\Runtime\\**\\bin\\**\""),
              std::string::npos)
        << xml;
    // Dependency package assembly referenced by HintPath.
    EXPECT_NE(xml.find("<Reference Include=\"WaterCore\">"), std::string::npos) << xml;
    // Runtime modules never reference the editor scripts assembly.
    EXPECT_EQ(xml.find("GameEngine.Editor.dll"), std::string::npos) << xml;
}

TEST(PackageModuleCsproj, EditorKindReferencesEditorAssembly)
{
    const fs::path pkgRoot = MakeTempDir("csproj_ed");
    const PackageCodeModule module = MakeCsModule(pkgRoot, /*editorKind=*/true);
    const std::string xml = GeneratePackageModuleCsprojXml(
        module, pkgRoot / ".Cache" / "ScriptProjects", fs::path("X:/out/Packages/Editor"),
        pkgRoot / "engine", {});
    EXPECT_NE(xml.find("<AssemblyName>OceanPack.Editor</AssemblyName>"), std::string::npos) << xml;
    EXPECT_NE(xml.find("GameEngine.Editor.dll"), std::string::npos) << xml;
}

// ---------------------------------------------------------------------------
// End-to-end through a REAL CompileServerHost: WaterCore compiles as its own
// assembly; OceanPack references it via the request's References field and
// compiles a #if OCEAN_PACK-gated source via the Defines field. The negative
// leg (no References) proves the reference is what made it resolve.
// ---------------------------------------------------------------------------
#ifdef _WIN32
TEST(PackageCompileE2E, ReferencesAndDefinesFlowThroughCompileServer)
{
    const std::string pipeName = CompileServerTestHost::UniquePipeName("pkgp1");
    ASSERT_TRUE(CompileServerTestHost::StartTestCompileServer(pipeName));

    const fs::path root = MakeTempDir("e2e");
    WriteFileAt(root / "water" / "Waves.cs",
                "namespace WaterCore { public static class Waves { public static int Depth() => 42; } }\n");
    WriteFileAt(root / "ocean" / "Ocean.cs",
                "#if OCEAN_PACK\n"
                "namespace OceanPack { public class Ocean { public int D() => WaterCore.Waves.Depth(); } }\n"
                "#else\n"
                "#error OCEAN_PACK was not defined\n"
                "#endif\n");

    // 1) Dependency package assembly.
    CompileServerRequestDesc water;
    water.ProjectRoot = root / "water_ws";
    water.AssemblyName = "WaterCore";
    water.SourceRoots = {root / "water"};
    CompileServerClient waterClient(std::unique_ptr<IHotReloadTransport>{}, pipeName);
    CompileServerResponse waterResp;
    ASSERT_TRUE(waterClient.Compile(BuildCompileServerRequestJson(water), waterResp));
    ASSERT_TRUE(waterResp.Success);
    ASSERT_FALSE(waterResp.AssemblyBytes.empty());
    const fs::path waterDll = root / "Packages" / "WaterCore.dll";
    fs::create_directories(waterDll.parent_path());
    {
        std::ofstream f(waterDll, std::ios::binary);
        f.write(reinterpret_cast<const char*>(waterResp.AssemblyBytes.data()),
                (std::streamsize)waterResp.AssemblyBytes.size());
    }

    // 2) Dependent package: needs BOTH the reference and the define.
    CompileServerRequestDesc ocean;
    ocean.ProjectRoot = root / "ocean_ws";
    ocean.AssemblyName = "OceanPack";
    ocean.SourceRoots = {root / "ocean"};
    ocean.References = {waterDll};
    ocean.Defines = {"OCEAN_PACK"};
    CompileServerClient oceanClient(std::unique_ptr<IHotReloadTransport>{}, pipeName);
    CompileServerResponse oceanResp;
    ASSERT_TRUE(oceanClient.Compile(BuildCompileServerRequestJson(ocean), oceanResp));
    EXPECT_TRUE(oceanResp.Success);
    EXPECT_FALSE(oceanResp.AssemblyBytes.empty());

    // 3) Negative leg: same compile WITHOUT the reference must fail (proves the
    //    References field is what resolved WaterCore, not ambient state).
    CompileServerRequestDesc noRef = ocean;
    noRef.ProjectRoot = root / "ocean_noref_ws"; // fresh workspace: no cached trees
    noRef.References.clear();
    CompileServerClient noRefClient(std::unique_ptr<IHotReloadTransport>{}, pipeName);
    CompileServerResponse noRefResp;
    EXPECT_FALSE(noRefClient.Compile(BuildCompileServerRequestJson(noRef), noRefResp) && noRefResp.Success);

    CompileServerTestHost::ReapHostsOnPipe(pipeName);
}

// ---------------------------------------------------------------------------
// Compile-path parity: the language settings the generated csproj declares
// (AllowUnsafeBlocks, ImplicitUsings, Nullable) reach the live compile through
// the request, and an old-shape request (no such fields) keeps today's meaning.
// ---------------------------------------------------------------------------
namespace
{

// Uses one type from each namespace of the SDK's implicit set for
// Microsoft.NET.Sdk, with no using directive.
constexpr const char* kImplicitUsingsSource =
    "namespace Parity {\n"
    "public static class ImplicitProbe {\n"
    "    public static int Run() {\n"
    "        var list = new List<int> { 3, 1, 2 };\n"
    "        bool missing = File.Exists(\"no-such-file\");\n"
    "        Task done = Task.CompletedTask;\n"
    "        int id = Thread.CurrentThread.ManagedThreadId;\n"
    "        Type http = typeof(HttpClient);\n"
    "        return list.Where(v => v > 1).Count() + (missing ? 1 : 0) + (done.IsCompleted ? 0 : 1) + (id > 0 ? 0 : 1) + (http is null ? 1 : 0);\n"
    "    }\n"
    "}\n"
    "}\n";

constexpr const char* kUnsafeSource =
    "namespace Parity {\n"
    "public static class UnsafeProbe {\n"
    "    public static unsafe int Read() { int value = 7; int* p = &value; return *p; }\n"
    "}\n"
    "}\n";

// CS8602 (dereference of a possibly null reference) is reported only inside a
// nullable context.
constexpr const char* kNullableSource =
    "namespace Parity {\n"
    "public static class NullableProbe {\n"
    "    static string? Maybe() => null;\n"
    "    public static int Length() { string? s = Maybe(); return s.Length; }\n"
    "}\n"
    "}\n";

bool HasDiagnostic(const std::vector<CompileServerDiagnostic>& diagnostics, const char* code)
{
    for (const CompileServerDiagnostic& d : diagnostics)
    {
        if (d.Code == code)
            return true;
    }
    return false;
}

std::string DescribeDiagnostics(const CompileServerResponse& resp)
{
    std::string text;
    for (const CompileServerDiagnostic& d : resp.Errors)
        text += "\n  error " + d.Code + ": " + d.MessageUtf8;
    for (const CompileServerDiagnostic& d : resp.Warnings)
        text += "\n  warning " + d.Code + ": " + d.MessageUtf8;
    return text;
}

// Compiles one source file as its own workspace through the resident host.
CompileServerResponse CompileOneSource(const std::string& pipeName, const fs::path& root, const char* name,
                                       const char* source, bool languageSettings)
{
    WriteFileAt(root / name / "Source.cs", source);
    CompileServerRequestDesc desc;
    desc.ProjectRoot = root / (std::string(name) + "_ws");
    desc.AssemblyName = name;
    desc.SourceRoots = {root / name};
    desc.AllowUnsafe = languageSettings;
    desc.ImplicitUsings = languageSettings;
    desc.Nullable = languageSettings;
    CompileServerClient client(std::unique_ptr<IHotReloadTransport>{}, pipeName);
    CompileServerResponse resp;
    client.Compile(BuildCompileServerRequestJson(desc), resp);
    return resp;
}

} // namespace

TEST(PackageCompileE2E, UnsafeBlockCompiles)
{
    const std::string pipeName = CompileServerTestHost::UniquePipeName("pkgunsafe");
    ASSERT_TRUE(CompileServerTestHost::StartTestCompileServer(pipeName));
    const CompileServerResponse resp =
        CompileOneSource(pipeName, MakeTempDir("unsafe"), "UnsafeProbe", kUnsafeSource, true);
    EXPECT_TRUE(resp.Success) << DescribeDiagnostics(resp);
    EXPECT_FALSE(resp.AssemblyBytes.empty());
    CompileServerTestHost::ReapHostsOnPipe(pipeName);
}

TEST(PackageCompileE2E, ImplicitUsingsCompile)
{
    const std::string pipeName = CompileServerTestHost::UniquePipeName("pkgusings");
    ASSERT_TRUE(CompileServerTestHost::StartTestCompileServer(pipeName));
    const CompileServerResponse resp =
        CompileOneSource(pipeName, MakeTempDir("usings"), "ImplicitProbe", kImplicitUsingsSource, true);
    EXPECT_TRUE(resp.Success) << DescribeDiagnostics(resp);
    EXPECT_FALSE(resp.AssemblyBytes.empty());
    CompileServerTestHost::ReapHostsOnPipe(pipeName);
}

TEST(PackageCompileE2E, NullableContextReportsDereferenceWarning)
{
    const std::string pipeName = CompileServerTestHost::UniquePipeName("pkgnullable");
    ASSERT_TRUE(CompileServerTestHost::StartTestCompileServer(pipeName));
    const CompileServerResponse resp =
        CompileOneSource(pipeName, MakeTempDir("nullable"), "NullableProbe", kNullableSource, true);
    EXPECT_TRUE(resp.Success) << DescribeDiagnostics(resp);
    EXPECT_TRUE(HasDiagnostic(resp.Warnings, "CS8602")) << DescribeDiagnostics(resp);
    CompileServerTestHost::ReapHostsOnPipe(pipeName);
}

// A request without the fields (a client from before them) compiles plain code
// and keeps unsafe refused: absence means false.
TEST(PackageCompileE2E, OldShapeRequestKeepsItsMeaning)
{
    const std::string pipeName = CompileServerTestHost::UniquePipeName("pkgoldshape");
    ASSERT_TRUE(CompileServerTestHost::StartTestCompileServer(pipeName));
    const fs::path root = MakeTempDir("oldshape");
    const CompileServerResponse plain = CompileOneSource(
        pipeName, root, "PlainProbe", "namespace Parity { public class Plain { public int V() => 1; } }\n", false);
    EXPECT_TRUE(plain.Success) << DescribeDiagnostics(plain);
    const CompileServerResponse unsafeResp = CompileOneSource(pipeName, root, "UnsafeProbe", kUnsafeSource, false);
    EXPECT_FALSE(unsafeResp.Success);
    EXPECT_TRUE(HasDiagnostic(unsafeResp.Errors, "CS0227")) << DescribeDiagnostics(unsafeResp);
    CompileServerTestHost::ReapHostsOnPipe(pipeName);
}

namespace
{

// The namespaces of the `global using [global::]<ns>;` lines in a GlobalUsings.g.cs
// the SDK generated.
std::set<std::string> ReadGlobalUsings(const fs::path& path)
{
    constexpr std::string_view kPrefix = "global using ";
    constexpr std::string_view kGlobalAlias = "global::";
    std::set<std::string> namespaces;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);)
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind(kPrefix, 0) != 0 || line.back() != ';')
            continue;
        std::string name = line.substr(kPrefix.size(), line.size() - kPrefix.size() - 1);
        if (name.rfind(kGlobalAlias, 0) == 0)
            name.erase(0, kGlobalAlias.size());
        namespaces.insert(name);
    }
    return namespaces;
}

// The implicit-usings set a resident host reports in its `__status__` reply.
std::set<std::string> QueryServerImplicitUsings(const std::string& pipeName)
{
    PipeTransport transport(pipeName);
    std::string status;
    if (!transport.Connect() || !transport.SendRequest("__status__", status))
        return {};
    const nlohmann::json reply = nlohmann::json::parse(status, nullptr, /*allow_exceptions=*/false);
    if (!reply.is_object() || !reply.contains("ImplicitUsings"))
        return {};
    return reply["ImplicitUsings"].get<std::set<std::string>>();
}

// Type names live in the metadata #Strings heap as NUL-terminated UTF-8.
bool DeclaresName(const std::vector<char>& assembly, const std::string& name)
{
    const std::string needle = std::string(1, '\0') + name + std::string(1, '\0');
    return std::search(assembly.begin(), assembly.end(), needle.begin(), needle.end()) != assembly.end();
}

} // namespace

// The same module through `dotnet build` of its generated csproj and through the
// compile server: both succeed, both report the nullable warning, and both
// assemblies declare the module's types. The server's implicit-usings set equals
// the one the installed SDK generated for the build (GlobalUsings.g.cs).
TEST(PackageCompileE2E, GeneratedCsprojAndServerCompileTheSameModule)
{
    if (!IsDotnetSdkAvailable())
        GTEST_SKIP() << "no .NET SDK: `dotnet build` of the generated csproj cannot run";

    const fs::path exeDir = PathUtils::GetExecutableDirectory();
    const fs::path pkgRoot = MakeTempDir("parity");
    PackageCodeModule module = MakeCsModule(pkgRoot);
    module.AssemblyName = "ParityPack";
    module.Defines.clear();
    module.DependencyAssemblies.clear();
    module.DependencyPackages.clear();
    WriteFileAt(module.RootDir / "ImplicitProbe.cs", kImplicitUsingsSource);
    WriteFileAt(module.RootDir / "UnsafeProbe.cs", kUnsafeSource);
    WriteFileAt(module.RootDir / "NullableProbe.cs", kNullableSource);

    // 1) dotnet build of the generated csproj.
    const fs::path projectDir = pkgRoot / ".Cache" / "ScriptProjects";
    const fs::path msbuildOut = pkgRoot / "msbuild_out";
    const fs::path csproj = projectDir / "ParityPack.csproj";
    WriteFileAt(csproj, GeneratePackageModuleCsprojXml(module, projectDir, msbuildOut, exeDir, {}));
    const fs::path buildLog = pkgRoot / "build.log";
    const std::string command = "\"\"" + DotnetHostCommand() + "\" build \"" + csproj.string() +
                                "\" -c Debug -nologo -v q > \"" + buildLog.string() + "\" 2>&1\"";
    const int exitCode = std::system(command.c_str());
    std::ifstream logStream(buildLog);
    const std::string log((std::istreambuf_iterator<char>(logStream)), std::istreambuf_iterator<char>());
    ASSERT_EQ(exitCode, 0) << log;
    EXPECT_NE(log.find("CS8602"), std::string::npos) << log;
    const std::vector<char> msbuildAssembly = ReadBytes(msbuildOut / "ParityPack.dll");
    ASSERT_FALSE(msbuildAssembly.empty());
    const std::set<std::string> sdkUsings =
        ReadGlobalUsings(projectDir / "obj" / "Debug" / "ParityPack.GlobalUsings.g.cs");
    ASSERT_FALSE(sdkUsings.empty());

    // 2) The compile server, with the request the engine builds for the module.
    const std::string pipeName = CompileServerTestHost::UniquePipeName("pkgparity");
    ASSERT_TRUE(CompileServerTestHost::StartTestCompileServer(pipeName));
    CompileServerRequestDesc desc;
    desc.ProjectRoot = projectDir;
    desc.AssemblyName = module.AssemblyName;
    desc.EngineBinDir = exeDir;
    desc.SourceRoots = {module.RootDir};
    CompileServerClient client(std::unique_ptr<IHotReloadTransport>{}, pipeName);
    CompileServerResponse resp;
    client.Compile(BuildCompileServerRequestJson(desc), resp);
    const std::set<std::string> serverUsings = QueryServerImplicitUsings(pipeName);
    CompileServerTestHost::ReapHostsOnPipe(pipeName);
    EXPECT_EQ(serverUsings, sdkUsings);
    ASSERT_TRUE(resp.Success) << DescribeDiagnostics(resp);
    EXPECT_TRUE(HasDiagnostic(resp.Warnings, "CS8602")) << DescribeDiagnostics(resp);
    const std::vector<char> serverAssembly(resp.AssemblyBytes.begin(), resp.AssemblyBytes.end());

    for (const char* typeName : {"ImplicitProbe", "UnsafeProbe", "NullableProbe", "Parity"})
    {
        EXPECT_TRUE(DeclaresName(msbuildAssembly, typeName)) << typeName;
        EXPECT_TRUE(DeclaresName(serverAssembly, typeName)) << typeName;
    }
}
#endif

// ---------------------------------------------------------------------------
// Prebuilt C# modules: a module that declares a `prebuilt` directory and has no
// sources uses <PrebuiltDir>/<AssemblyName>.dll instead of compiling; a module
// with sources always compiles; a DLL is loaded only from where the manifest
// declares it, and only when it is a .NET assembly. The compile is observed at
// CompileServerClient's started notification (one per request, carrying the
// request's assembly name) behind a transport that refuses to connect, so no
// server ever runs.
// ---------------------------------------------------------------------------
namespace
{

// A workspace with no CLR and no hot reload: only the compile orchestration runs.
struct PrebuiltWorkspace
{
    explicit PrebuiltWorkspace(const char* tag) : Root(MakeTempDir(tag)), Pool(2)
    {
        ScriptsConfig config{};
        config.workspaceRoot = Root;
        config.scriptsRoot = Root / "Assets";
        config.assembliesRoot = Root / "ScriptAssemblies";
        config.disableClr = true;
        config.enableHotReload = false;
        config.enableAsyncHotReload = false;
        std::error_code ec;
        fs::create_directories(config.scriptsRoot, ec);
        Initialized = Manager.Initialize(config, Pool);
        SetCompileServerTransportFactoryForTests([](const std::string&) -> std::unique_ptr<IHotReloadTransport> {
            return std::make_unique<Tests::RefusingTransport>();
        });
    }
    ~PrebuiltWorkspace() { Manager.Shutdown(); }

    fs::path PackageAssembly(const std::string& fileName) const { return Root / "ScriptAssemblies" / "Packages" / fileName; }

    Tests::ScopedHotReloadTestHooks Hooks;
    fs::path Root;
    JobSystem::WorkStealingThreadPool Pool;
    ScriptManager Manager;
    bool Initialized = false;
};

// A runtime C# module of package "closed-pack" declaring prebuilt dir `Binaries/`.
PackageCodeModule MakePrebuiltModule(const fs::path& pkgRoot)
{
    PackageCodeModule module;
    module.PackageName = "closed-pack";
    module.AssemblyName = "ClosedPack";
    module.Lang = PackageModuleRecord::ModuleLang::CSharp;
    module.PackageRootDir = pkgRoot;
    module.CacheDir = pkgRoot / ".Cache";
    module.PrebuiltDir = pkgRoot / "Binaries";
    return module;
}

// Ships a real .NET assembly as the module's prebuilt DLL: the staged
// Scripting.ABI assembly beside the test executable. Its identity is not the
// module's; only the image format is checked before staging.
void ShipManagedAssembly(const PackageCodeModule& module)
{
    const fs::path source = PathUtils::GetExecutableDirectory() / "GameEngine.Scripting.ABI.dll";
    ASSERT_TRUE(fs::exists(source)) << source;
    std::error_code ec;
    fs::create_directories(module.PrebuiltDir, ec);
    fs::copy_file(source, module.PrebuiltDir / (module.AssemblyName + ".dll"), fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();
}

bool Requested(const Tests::CompileNotificationLog& log, const std::string& assemblyName)
{
    const std::vector<std::string>& entries = log.Entries();
    return std::find(entries.begin(), entries.end(), "started " + assemblyName) != entries.end();
}

} // namespace

TEST(PrebuiltPackageModule, DeclaredDllWithoutSourcesIsUsedWithoutACompile)
{
    PrebuiltWorkspace ws("prebuilt_used");
    ASSERT_TRUE(ws.Initialized);
    const PackageCodeModule module = MakePrebuiltModule(ws.Root / "closed-pack");
    ShipManagedAssembly(module);
    WriteFileAt(module.PrebuiltDir / "ClosedPack.pdb", "prebuilt symbols");

    std::vector<std::string> log;
    {
        TestLog::ScopedEngineLogCapture capture(&log);
        ws.Manager.SetPackageCodeModules({module}, {});
    }
    EXPECT_EQ(TestLog::CountLinesContaining(log, "[Packages] using prebuilt assembly ClosedPack.dll"), 1u);
    EXPECT_EQ(ReadBytes(ws.PackageAssembly("ClosedPack.dll")), ReadBytes(module.PrebuiltDir / "ClosedPack.dll"));
    EXPECT_EQ(ReadBytes(ws.PackageAssembly("ClosedPack.pdb")), ReadBytes(module.PrebuiltDir / "ClosedPack.pdb"));

    Tests::CompileNotificationLog requests;
    ws.Manager.CompileScripts(nullptr);
    EXPECT_TRUE(Requested(requests, "GameEngine.Scripts")); // the log sees this pass's requests
    EXPECT_FALSE(Requested(requests, "ClosedPack"));
    EXPECT_TRUE(fs::exists(ws.PackageAssembly("ClosedPack.dll")));
    const std::vector<std::string> compiled = ws.Manager.GetCompiledAssemblyNames();
    EXPECT_EQ(std::find(compiled.begin(), compiled.end(), "ClosedPack"), compiled.end());
}

TEST(PrebuiltPackageModule, SourcesWinOverADeclaredDll)
{
    PrebuiltWorkspace ws("prebuilt_sources_win");
    ASSERT_TRUE(ws.Initialized);
    PackageCodeModule module = MakePrebuiltModule(ws.Root / "closed-pack");
    module.RootDir = module.PackageRootDir / "Runtime";
    WriteFileAt(module.RootDir / "Closed.cs", "namespace Closed { public class Pack {} }\n");
    ShipManagedAssembly(module);

    std::vector<std::string> log;
    {
        TestLog::ScopedEngineLogCapture capture(&log);
        ws.Manager.SetPackageCodeModules({module}, {});
    }
    EXPECT_EQ(TestLog::CountLinesContaining(log, "using prebuilt assembly"), 0u);
    EXPECT_EQ(TestLog::CountLinesContaining(log, "has sources, so it compiles"), 1u);
    EXPECT_FALSE(fs::exists(ws.PackageAssembly("ClosedPack.dll")));

    Tests::CompileNotificationLog requests;
    ws.Manager.CompileScripts(nullptr);
    EXPECT_TRUE(Requested(requests, "ClosedPack"));
    EXPECT_FALSE(fs::exists(ws.PackageAssembly("ClosedPack.dll")));
}

// The declared directory is the only place a module's DLL is taken from: the
// same file elsewhere in the mount is a plain file. Without it the module is
// refused with the path the manifest promised.
TEST(PrebuiltPackageModule, DllOutsideTheDeclaredDirectoryIsNeverLoaded)
{
    PrebuiltWorkspace ws("prebuilt_outside");
    ASSERT_TRUE(ws.Initialized);
    const PackageCodeModule module = MakePrebuiltModule(ws.Root / "closed-pack");
    WriteFileAt(module.PackageRootDir / "Plugins" / "ClosedPack.dll", "undeclared bytes");
    WriteFileAt(module.PackageRootDir / "ClosedPack.dll", "undeclared bytes");

    std::vector<std::string> log;
    {
        TestLog::ScopedEngineLogCapture capture(&log);
        ws.Manager.SetPackageCodeModules({module}, {});
    }
    const std::string refusal = TestLog::FirstLineContaining(log, "declares prebuilt assembly");
    EXPECT_NE(refusal.find((module.PrebuiltDir / "ClosedPack.dll").string()), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("modules[]"), std::string::npos) << refusal;
    EXPECT_FALSE(fs::exists(ws.PackageAssembly("ClosedPack.dll")));

    Tests::CompileNotificationLog requests;
    ws.Manager.CompileScripts(nullptr);
    EXPECT_FALSE(Requested(requests, "ClosedPack"));
    EXPECT_FALSE(fs::exists(ws.PackageAssembly("ClosedPack.dll")));
    const std::vector<std::string> compiled = ws.Manager.GetCompiledAssemblyNames();
    EXPECT_EQ(std::find(compiled.begin(), compiled.end(), "ClosedPack"), compiled.end());
}

// A declared file that is not a .NET assembly (arbitrary bytes, or a native
// DLL) is refused when the package mounts, naming the package, the manifest
// entry and the fix, instead of failing later in the script domain's load.
TEST(PrebuiltPackageModule, DeclaredFileThatIsNotADotNetAssemblyIsRefused)
{
    PrebuiltWorkspace ws("prebuilt_not_managed");
    ASSERT_TRUE(ws.Initialized);
    const PackageCodeModule module = MakePrebuiltModule(ws.Root / "closed-pack");
    const fs::path declaredDll = module.PrebuiltDir / "ClosedPack.dll";
    const fs::path nativeDll = PathUtils::GetExecutableDirectory() / "Engine.dll";
    ASSERT_TRUE(fs::exists(nativeDll)) << nativeDll;

    for (const char* fixture : {"plain bytes", "native DLL"})
    {
        SCOPED_TRACE(fixture);
        if (std::string_view(fixture) == "plain bytes")
            WriteFileAt(declaredDll, "not an assembly");
        else
            fs::copy_file(nativeDll, declaredDll, fs::copy_options::overwrite_existing);

        std::vector<std::string> log;
        {
            TestLog::ScopedEngineLogCapture capture(&log);
            ws.Manager.SetPackageCodeModules({module}, {});
        }
        EXPECT_EQ(TestLog::CountLinesContaining(log, "using prebuilt assembly"), 0u);
        const std::string refusal = TestLog::FirstLineContaining(log, "is not a .NET assembly");
        EXPECT_NE(refusal.find("closed-pack"), std::string::npos) << refusal;
        EXPECT_NE(refusal.find("modules[]"), std::string::npos) << refusal;
        EXPECT_FALSE(fs::exists(ws.PackageAssembly("ClosedPack.dll")));
    }
}

// A prebuilt that ships no symbols leaves none beside it: the .pdb an earlier
// compile of the module staged would describe a different assembly.
TEST(PrebuiltPackageModule, StaleSymbolsAreRemovedWhenThePrebuiltShipsNone)
{
    PrebuiltWorkspace ws("prebuilt_stale_pdb");
    ASSERT_TRUE(ws.Initialized);
    const PackageCodeModule module = MakePrebuiltModule(ws.Root / "closed-pack");
    ShipManagedAssembly(module);
    WriteFileAt(ws.PackageAssembly("ClosedPack.pdb"), "symbols of an earlier compile");

    ws.Manager.SetPackageCodeModules({module}, {});
    EXPECT_TRUE(fs::exists(ws.PackageAssembly("ClosedPack.dll")));
    EXPECT_FALSE(fs::exists(ws.PackageAssembly("ClosedPack.pdb")));
}
