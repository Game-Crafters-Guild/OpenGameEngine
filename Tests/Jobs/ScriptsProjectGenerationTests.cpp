// Pins the generated GameEngine.Scripts.csproj contract from issue #333:
//  - EngineBinDir is a Condition-guarded last-known-good default (live builds pass
//    -p:EngineBinDir per invocation), refreshed whenever the project is (re)opened.
//  - A stale EngineBinDir pointing at a dead directory self-heals on rebind and
//    fires a loud engine-side ERROR naming the dead path.
//  - The baked MSBuild validation target turns the old silent Exists()-drop into
//    a build error when EngineBinDir is gone.
//  - Source/output paths are anchored to the csproj (project-relative) while the
//    layout stays inside the workspace, and never emitted as a "..\..\.." climb
//    into unrelated trees.
//  - EngineBinDir is the engine managed-assembly directory (ScriptingPaths::
//    ResolveEngineManagedDirectory): beside the executable, or Contents/Resources/Managed
//    inside a macOS app bundle — never a bare executable directory.
//  - The language settings (AllowUnsafeBlocks, ImplicitUsings, Nullable) are the
//    ones the compile-server request carries.
//  - The project is generated at the project root (issue #3004), and one left
//    inside Assets/ is removed when the project opens.

#include <gtest/gtest.h>
#include "Assets/Packages/PackageCodeModules.h"
#include "Scripting/ScriptManager.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Scripting/PathResolver.h"
#include "Logger/Logger.h"
#include "Logger/CallbackSink.h"
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{
std::string ReadFileText(const std::filesystem::path& path)
{
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteFileText(const std::filesystem::path& path, const std::string& content)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream of(path, std::ios::trunc);
    of << content;
}

ScriptsConfig MakeConfig(const std::filesystem::path& ws)
{
    ScriptsConfig cfg{};
    cfg.workspaceRoot = ws;
    cfg.scriptsRoot = ws / "Assets";
    cfg.assembliesRoot = ws / "ScriptAssemblies";
    cfg.disableClr = true; // generation only; no CoreCLR needed
    cfg.enableHotReload = false;
    cfg.enableAsyncHotReload = false;
    cfg.enableAutoProjectGeneration = true;
    return cfg;
}

// What an earlier editor left in Assets/: a generated project, recognizable by
// the diagnostics target every generated scripts project carries.
constexpr const char* kStaleGeneratedProject =
    "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
    "  <Target Name=\"DumpCompileItems\" BeforeTargets=\"CoreCompile\" />\n"
    "</Project>\n";

std::string LiveEngineBinDir()
{
    std::string dir = ScriptingPaths::ResolveEngineManagedDirectory().generic_string();
    if (!dir.empty() && dir.back() != '/')
        dir += '/';
    return dir;
}

// Captures log messages of one level for the duration of a test.
class LogCapture
{
  public:
    explicit LogCapture(Logger::LogLevel level) : m_Level(level)
    {
        Logger::Log::Initialize({});
        auto sink = Logger::MakeUnique<Logger::CallbackSink>();
        m_Sink = sink.get();
        Logger::Log::AddSink(std::move(sink));
        m_CallbackId = m_Sink->RegisterCallback(
            [this](const Logger::LogMessage& msg)
            {
                if (msg.Level != m_Level)
                    return;
                std::lock_guard<std::mutex> lock(m_Mutex);
                m_Messages.push_back(std::string(msg.Message));
            });
    }

    ~LogCapture()
    {
        // The sink itself stays registered (Logger has no RemoveSink); with no
        // callbacks it is a no-op for subsequent tests.
        m_Sink->UnregisterCallback(m_CallbackId);
    }

    bool AnyContains(const std::string& needle)
    {
        Logger::Log::Flush(); // drain the async logger before inspecting
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (const auto& e : m_Messages)
        {
            if (e.find(needle) != std::string::npos)
                return true;
        }
        return false;
    }

  private:
    Logger::LogLevel m_Level;
    Logger::CallbackSink* m_Sink = nullptr;
    uint64_t m_CallbackId = 0;
    std::mutex m_Mutex;
    std::vector<std::string> m_Messages;
};
} // namespace

TEST(ScriptsProjectGeneration, GeneratedProjectAnchorsPathsAndGuardsEngineBinDir)
{
    const std::filesystem::path ws = std::filesystem::temp_directory_path() / "GE_ScriptsCsprojGen";
    std::error_code ec;
    std::filesystem::remove_all(ws, ec);
    std::filesystem::create_directories(ws / "Assets", ec);

    JobSystem::WorkStealingThreadPool pool(2);
    ScriptManager mgr;
    ASSERT_TRUE(mgr.Initialize(MakeConfig(ws), pool));

    // At the project root, where an IDE opening it puts new files beside Assets/,
    // and the same path the build pipeline's no-session fallback resolves.
    const std::filesystem::path projectPath = ws / "GameEngine.Scripts.csproj";
    EXPECT_EQ(mgr.GeneratedProjectPath(), projectPath);
    EXPECT_EQ(ScriptManager::GeneratedProjectPathFor(ws), projectPath);
    ASSERT_TRUE(std::filesystem::exists(projectPath));
    EXPECT_FALSE(std::filesystem::exists(ws / "Assets" / "GameEngine.Scripts.csproj"));
    const std::string content = ReadFileText(projectPath);

    // EngineBinDir: Condition-guarded default holding the live engine location.
    EXPECT_NE(content.find("<EngineBinDir Condition=\"'$(EngineBinDir)' == ''\">" + LiveEngineBinDir() +
                           "</EngineBinDir>"),
              std::string::npos)
        << content;

    // Loud MSBuild-side validation replaces the silent Exists() drop.
    EXPECT_NE(content.find("ValidateEngineBinDir"), std::string::npos);
    EXPECT_NE(content.find("!Exists('$(EngineBinDir)')"), std::string::npos);

    // Sources anchored to the csproj: the scripts root beside it.
    EXPECT_NE(content.find("<Compile Include=\"Assets\\**\\*.cs\" Exclude=\"Assets\\**\\obj\\**;Assets\\**\\bin\\**\" />"),
              std::string::npos)
        << content;
    EXPECT_EQ(content.find("..\\..\\..\\..\\.."), std::string::npos) << content;

    // Output stays project-relative while assemblies live inside the workspace.
    EXPECT_NE(content.find("<OutputPath>ScriptAssemblies/</OutputPath>"), std::string::npos) << content;

    // The language settings the compile-server request carries.
    EXPECT_NE(content.find("<AllowUnsafeBlocks>true</AllowUnsafeBlocks>"), std::string::npos) << content;
    EXPECT_NE(content.find("<ImplicitUsings>enable</ImplicitUsings>"), std::string::npos) << content;
    EXPECT_NE(content.find("<Nullable>enable</Nullable>"), std::string::npos) << content;

    mgr.Shutdown();
    std::filesystem::remove_all(ws, ec);
}

TEST(ScriptsProjectGeneration, SelfHealsStaleEngineBinDirWithLoudDiagnostic)
{
    const std::filesystem::path ws = std::filesystem::temp_directory_path() / "GE_ScriptsCsprojSelfHeal";
    std::error_code ec;
    std::filesystem::remove_all(ws, ec);
    std::filesystem::create_directories(ws / "Assets", ec);

    JobSystem::WorkStealingThreadPool pool(2);
    ScriptManager mgr;
    const ScriptsConfig cfg = MakeConfig(ws);
    ASSERT_TRUE(mgr.Initialize(cfg, pool));

    const std::filesystem::path projectPath = ws / "GameEngine.Scripts.csproj";
    ASSERT_TRUE(std::filesystem::exists(projectPath));

    // Simulate a relocated Editor: the csproj was generated by an exe whose
    // directory no longer exists.
    const std::string deadBinDir = "C:/GE_DeadEditorBin_Issue333/bin/DebugFast/";
    ASSERT_FALSE(std::filesystem::exists(deadBinDir));
    std::string content = ReadFileText(projectPath);
    const std::string liveElement =
        "<EngineBinDir Condition=\"'$(EngineBinDir)' == ''\">" + LiveEngineBinDir() + "</EngineBinDir>";
    const std::string staleElement =
        "<EngineBinDir Condition=\"'$(EngineBinDir)' == ''\">" + deadBinDir + "</EngineBinDir>";
    const auto pos = content.find(liveElement);
    ASSERT_NE(pos, std::string::npos);
    content.replace(pos, liveElement.size(), staleElement);
    WriteFileText(projectPath, content);

    LogCapture errors(Logger::LogLevel::Error);

    // Re-opening the project (rebind) must regenerate with the live location...
    ASSERT_TRUE(mgr.RebindProjectScripts(cfg));

    const std::string healed = ReadFileText(projectPath);
    EXPECT_NE(healed.find(LiveEngineBinDir()), std::string::npos) << healed;
    EXPECT_EQ(healed.find(deadBinDir), std::string::npos) << healed;

    // ...and say loudly why: the old EngineBinDir is gone.
    EXPECT_TRUE(errors.AnyContains("GE_DeadEditorBin_Issue333"));
    EXPECT_TRUE(errors.AnyContains("EngineBinDir"));

    mgr.Shutdown();
    std::filesystem::remove_all(ws, ec);
}

TEST(ScriptsProjectGeneration, PackageReferencesBakedAndRemovedWithModuleSet)
{
    // P4a FINDING 2a: the generated csproj is the only reference carrier for
    // MSBuild consumers (packaged Debug `dotnet build`, IDE loads) — package
    // runtime assemblies must appear as <Reference> items, Editor-kind ones
    // must not (GameEngine.Scripts.dll loads in the Player), and clearing the
    // package set must remove them again.
    const std::filesystem::path ws = std::filesystem::temp_directory_path() / "GE_ScriptsCsprojPkgRefs";
    std::error_code ec;
    std::filesystem::remove_all(ws, ec);
    std::filesystem::create_directories(ws / "Assets", ec);

    JobSystem::WorkStealingThreadPool pool(2);
    ScriptManager mgr;
    ASSERT_TRUE(mgr.Initialize(MakeConfig(ws), pool));

    PackageCodeModule runtime;
    runtime.PackageName = "ocean-pack";
    runtime.AssemblyName = "OceanPack";
    runtime.Kind = PackageModuleRecord::ModuleKind::Runtime;
    runtime.Lang = PackageModuleRecord::ModuleLang::CSharp;
    PackageCodeModule editor = runtime;
    editor.AssemblyName = "OceanPack.Editor";
    editor.Kind = PackageModuleRecord::ModuleKind::Editor;
    mgr.SetPackageCodeModules({runtime, editor}, {"OCEAN_PACK"});

    const std::filesystem::path projectPath = ws / "GameEngine.Scripts.csproj";
    ASSERT_TRUE(std::filesystem::exists(projectPath));
    std::string content = ReadFileText(projectPath);

    // Packages dir anchored like OutputPath; reference presence-gated so IDE
    // loads before the first package compile stay clean.
    EXPECT_NE(content.find("<GamePackagesDir Condition=\"'$(GamePackagesDir)' == ''\">"
                           "ScriptAssemblies/Packages/</GamePackagesDir>"),
              std::string::npos)
        << content;
    EXPECT_NE(content.find("<Reference Include=\"OceanPack\" "
                           "Condition=\"Exists('$(GamePackagesDir)OceanPack.dll')\">"),
              std::string::npos)
        << content;
    EXPECT_NE(content.find("<HintPath>$(GamePackagesDir)OceanPack.dll</HintPath>"), std::string::npos)
        << content;
    EXPECT_EQ(content.find("OceanPack.Editor.dll"), std::string::npos) << content;

    // Clearing the set (project close / unmount) removes the baked references.
    mgr.SetPackageCodeModules({}, {});
    content = ReadFileText(projectPath);
    EXPECT_EQ(content.find("GamePackagesDir"), std::string::npos) << content;
    EXPECT_EQ(content.find("OceanPack"), std::string::npos) << content;

    mgr.Shutdown();
    std::filesystem::remove_all(ws, ec);
}

// A project last opened when the generated csproj lived inside Assets/: that
// file (and the IDE's .lscache beside it) is removed with an Info line when
// the project opens, at startup or by a project switch, and never rewritten.
TEST(ScriptsProjectGeneration, StaleProjectInsideAssetsIsRemovedOnOpen)
{
    const std::filesystem::path wsA = std::filesystem::temp_directory_path() / "GE_ScriptsCsprojStaleA";
    const std::filesystem::path wsB = std::filesystem::temp_directory_path() / "GE_ScriptsCsprojStaleB";
    std::error_code ec;
    std::filesystem::remove_all(wsA, ec);
    std::filesystem::remove_all(wsB, ec);
    for (const std::filesystem::path& ws : {wsA, wsB})
    {
        WriteFileText(ws / "Assets" / "GameEngine.Scripts.csproj", kStaleGeneratedProject);
        WriteFileText(ws / "Assets" / "GameEngine.Scripts.csproj.lscache", "cache");
    }

    LogCapture infos(Logger::LogLevel::Info);
    JobSystem::WorkStealingThreadPool pool(2);
    ScriptManager mgr;
    ASSERT_TRUE(mgr.Initialize(MakeConfig(wsA), pool));

    EXPECT_FALSE(std::filesystem::exists(wsA / "Assets" / "GameEngine.Scripts.csproj"));
    EXPECT_FALSE(std::filesystem::exists(wsA / "Assets" / "GameEngine.Scripts.csproj.lscache"));
    EXPECT_TRUE(std::filesystem::exists(wsA / "GameEngine.Scripts.csproj"));
    EXPECT_TRUE(infos.AnyContains("Removed stale auto-generated scripts project '" +
                                  (wsA / "Assets" / "GameEngine.Scripts.csproj").string() + "'"));

    ASSERT_TRUE(mgr.RebindProjectScripts(MakeConfig(wsB)));

    EXPECT_FALSE(std::filesystem::exists(wsB / "Assets" / "GameEngine.Scripts.csproj"));
    EXPECT_FALSE(std::filesystem::exists(wsB / "Assets" / "GameEngine.Scripts.csproj.lscache"));
    EXPECT_TRUE(std::filesystem::exists(wsB / "GameEngine.Scripts.csproj"));
    EXPECT_TRUE(infos.AnyContains("Removed stale auto-generated scripts project '" +
                                  (wsB / "Assets" / "GameEngine.Scripts.csproj").string() + "'"));

    mgr.Shutdown();
    std::filesystem::remove_all(wsA, ec);
    std::filesystem::remove_all(wsB, ec);
}

// A hand-written project at the same path is the user's own: it stays,
// unchanged, and nothing claims it was removed.
TEST(ScriptsProjectGeneration, HandWrittenProjectInsideAssetsSurvivesOpen)
{
    const std::filesystem::path ws = std::filesystem::temp_directory_path() / "GE_ScriptsCsprojHandWritten";
    std::error_code ec;
    std::filesystem::remove_all(ws, ec);
    const std::string handWritten =
        "<Project Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup><TargetFramework>net10.0</TargetFramework>"
        "</PropertyGroup></Project>";
    const std::filesystem::path userProject = ws / "Assets" / "GameEngine.Scripts.csproj";
    WriteFileText(userProject, handWritten);

    LogCapture infos(Logger::LogLevel::Info);
    JobSystem::WorkStealingThreadPool pool(2);
    ScriptManager mgr;
    ASSERT_TRUE(mgr.Initialize(MakeConfig(ws), pool));

    ASSERT_TRUE(std::filesystem::exists(userProject));
    EXPECT_EQ(ReadFileText(userProject), handWritten);
    EXPECT_FALSE(infos.AnyContains("Removed stale auto-generated scripts project"));

    mgr.Shutdown();
    std::filesystem::remove_all(ws, ec);
}

// A stale generated project another program holds open (an IDE) cannot be
// deleted: the open continues, and the log says so with the fix instead of
// claiming a removal.
TEST(ScriptsProjectGeneration, StaleProjectThatCannotBeRemovedIsReportedNotClaimed)
{
#ifdef _WIN32
    const std::filesystem::path ws = std::filesystem::temp_directory_path() / "GE_ScriptsCsprojStaleHeld";
    std::error_code ec;
    std::filesystem::remove_all(ws, ec);
    std::filesystem::create_directories(ws / "Assets", ec);
    const std::filesystem::path staleProject = ws / "Assets" / "GameEngine.Scripts.csproj";

    LogCapture infos(Logger::LogLevel::Info);
    LogCapture warnings(Logger::LogLevel::Warning);
    {
        // The MSVC runtime opens without delete sharing, so the file cannot be
        // deleted while this stream holds it, as with an IDE's open handle.
        std::ofstream held(staleProject, std::ios::trunc);
        held << kStaleGeneratedProject;
        held.flush();

        JobSystem::WorkStealingThreadPool pool(2);
        ScriptManager mgr;
        ASSERT_TRUE(mgr.Initialize(MakeConfig(ws), pool));

        EXPECT_TRUE(std::filesystem::exists(staleProject));
        EXPECT_FALSE(infos.AnyContains("Removed stale auto-generated scripts project"));
        EXPECT_TRUE(warnings.AnyContains("Could not remove stale auto-generated scripts project '" +
                                         staleProject.string() + "'"));
        EXPECT_TRUE(warnings.AnyContains("then delete it"));
        mgr.Shutdown();
    }
    std::filesystem::remove_all(ws, ec);
#else
    GTEST_SKIP() << "Holding a file open blocks its deletion only on Windows";
#endif
}

TEST(ScriptsProjectGeneration, CrossRootLayoutFallsBackToAbsoluteSourcePath)
{
    // The observed #333 instance: generated project directory and scripts root not
    // sharing the workspace produced a "..\..\..\..\.." Compile Include that
    // re-anchored to the repo root. Such layouts must bake the absolute scripts
    // root instead of a relative climb.
    const std::filesystem::path wsA = std::filesystem::temp_directory_path() / "GE_ScriptsCsprojCrossRootA";
    const std::filesystem::path wsB = std::filesystem::temp_directory_path() / "GE_ScriptsCsprojCrossRootB";
    std::error_code ec;
    std::filesystem::remove_all(wsA, ec);
    std::filesystem::remove_all(wsB, ec);
    std::filesystem::create_directories(wsA / "Assets", ec);
    std::filesystem::create_directories(wsB, ec);

    JobSystem::WorkStealingThreadPool pool(2);
    ScriptManager mgr;
    ScriptsConfig cfg = MakeConfig(wsA);
    cfg.generatedProjectRoot = wsB; // outside the workspace
    ASSERT_TRUE(mgr.Initialize(cfg, pool));

    const std::filesystem::path projectPath = wsB / "GameEngine.Scripts.csproj";
    ASSERT_TRUE(std::filesystem::exists(projectPath));
    const std::string content = ReadFileText(projectPath);

    // Absolute scripts root, no relative climb out of the project directory.
    std::string scriptsRootWin = (wsA / "Assets").generic_string() + "/";
    for (auto& ch : scriptsRootWin)
    {
        if (ch == '/')
            ch = '\\';
    }
    EXPECT_NE(content.find("<Compile Include=\"" + scriptsRootWin + "**\\*.cs\" Exclude=\"" + scriptsRootWin
                           + "**\\obj\\**;" + scriptsRootWin + "**\\bin\\**\" />"),
              std::string::npos)
        << content;
    EXPECT_EQ(content.find("<Compile Include=\"..\\"), std::string::npos) << content;

    mgr.Shutdown();
    std::filesystem::remove_all(wsA, ec);
    std::filesystem::remove_all(wsB, ec);
}

// P4b FINDING B(1): a compiled package assembly left in ScriptAssemblies/Packages
// after its package is removed from Packages/manifest.json still LOADS next
// session (the hot-reload domain loads that directory wholesale), so its module
// initializer re-registers components for a package that no longer exists.
// SetPackageCodeModules — the per-project-open (re)mount hook — must prune
// dll/pdb files no resolved module produces, and keep the resolved ones.
TEST(ScriptsProjectGeneration, StalePackageAssembliesArePrunedOnPackageSetRebind)
{
    const std::filesystem::path ws = std::filesystem::temp_directory_path() / "GE_ScriptsPkgPrune";
    std::error_code ec;
    std::filesystem::remove_all(ws, ec);
    std::filesystem::create_directories(ws / "Assets", ec);

    JobSystem::WorkStealingThreadPool pool(2);
    ScriptManager mgr;
    ASSERT_TRUE(mgr.Initialize(MakeConfig(ws), pool));

    const std::filesystem::path pkgDir = ws / "ScriptAssemblies" / "Packages";
    const std::filesystem::path editorDir = pkgDir / "Editor";
    std::filesystem::create_directories(editorDir, ec);
    WriteFileText(pkgDir / "LivePack.dll", "live");
    WriteFileText(pkgDir / "GhostPack.dll", "stale");
    WriteFileText(pkgDir / "GhostPack.pdb", "stale");
    WriteFileText(editorDir / "GhostPack.Editor.dll", "stale");
    WriteFileText(pkgDir / "notes.txt", "unrelated"); // non-assembly files are left alone

    PackageCodeModule live;
    live.PackageName = "live-pack";
    live.AssemblyName = "LivePack";
    live.Lang = PackageModuleRecord::ModuleLang::CSharp;
    mgr.SetPackageCodeModules({live}, {});

    EXPECT_TRUE(std::filesystem::exists(pkgDir / "LivePack.dll"));
    EXPECT_FALSE(std::filesystem::exists(pkgDir / "GhostPack.dll"));
    EXPECT_FALSE(std::filesystem::exists(pkgDir / "GhostPack.pdb"));
    EXPECT_FALSE(std::filesystem::exists(editorDir / "GhostPack.Editor.dll"));
    EXPECT_TRUE(std::filesystem::exists(pkgDir / "notes.txt"));

    // Project close (empty resolution): nothing resolved -> nothing may load.
    mgr.SetPackageCodeModules({}, {});
    EXPECT_FALSE(std::filesystem::exists(pkgDir / "LivePack.dll"));

    mgr.Shutdown();
    std::filesystem::remove_all(ws, ec);
}

// Transition-session ordering: the managed domain's initial load (during
// ScriptManager::Initialize) ingests ScriptAssemblies/Packages[/Editor]
// wholesale BEFORE the app mounts packages and calls SetPackageCodeModules,
// so the mount-time prune alone lets a removed package's assembly load once
// more in the very session that removed it. Initialize must prune from a
// fresh resolution of the workspace manifest before any initial load.
TEST(ScriptsProjectGeneration, InitializePrunesStalePackageAssembliesFromFreshResolution)
{
    const std::filesystem::path ws = std::filesystem::temp_directory_path() / "GE_ScriptsPkgPruneInit";
    std::error_code ec;
    std::filesystem::remove_all(ws, ec);
    std::filesystem::create_directories(ws / "Assets", ec);

    // A package that still resolves, with Runtime + Editor CSharp modules
    // ("sample-pack" -> assemblies "SamplePack" / "SamplePack.Editor").
    // Both are discovered from the sources: the C# extension gives the
    // language, the Editor folder the kind. The manifest declares no modules.
    const std::filesystem::path pkgSrcDir = ws / "Packages" / "sample-pack";
    WriteFileText(pkgSrcDir / "Scripts" / "Sample.cs", "// package module fixture");
    WriteFileText(pkgSrcDir / "Editor" / "SampleTool.cs", "// package module fixture");
    WriteFileText(pkgSrcDir / "package.json", R"({
        "name": "sample-pack",
        "version": "1.0.0"
    })");
    WriteFileText(ws / "Packages" / "manifest.json", R"({
        "dependencies": { "sample-pack": "embedded" }
    })");

    // Its compiled outputs, plus outputs of a package that no longer resolves.
    const std::filesystem::path pkgOutDir = ws / "ScriptAssemblies" / "Packages";
    const std::filesystem::path editorOutDir = pkgOutDir / "Editor";
    std::filesystem::create_directories(editorOutDir, ec);
    WriteFileText(pkgOutDir / "SamplePack.dll", "live");
    WriteFileText(editorOutDir / "SamplePack.Editor.dll", "live");
    WriteFileText(pkgOutDir / "GhostPack.dll", "stale");
    WriteFileText(pkgOutDir / "GhostPack.pdb", "stale");
    WriteFileText(editorOutDir / "GhostTool.Editor.dll", "stale");

    JobSystem::WorkStealingThreadPool pool(2);
    ScriptManager mgr;
    ASSERT_TRUE(mgr.Initialize(MakeConfig(ws), pool));

    // No SetPackageCodeModules call yet: Initialize itself must have pruned,
    // keeping everything the resolution accounts for (including Editor-kind).
    EXPECT_TRUE(std::filesystem::exists(pkgOutDir / "SamplePack.dll"));
    EXPECT_TRUE(std::filesystem::exists(editorOutDir / "SamplePack.Editor.dll"));
    EXPECT_FALSE(std::filesystem::exists(pkgOutDir / "GhostPack.dll"));
    EXPECT_FALSE(std::filesystem::exists(pkgOutDir / "GhostPack.pdb"));
    EXPECT_FALSE(std::filesystem::exists(editorOutDir / "GhostTool.Editor.dll"));

    mgr.Shutdown();
    std::filesystem::remove_all(ws, ec);
}
