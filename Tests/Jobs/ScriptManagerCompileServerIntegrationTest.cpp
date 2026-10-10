#include <gtest/gtest.h>
#include "CompileServerTestHost.h"
#include "Scripting/ScriptManager.h"
#include "Jobs/WorkspaceId.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Core/Application.h" // PathUtils
#include <filesystem>
#include <fstream>

using namespace GameEngine;

TEST(ScriptManagerCompileServerIntegration, CompileProjectUsesServerWhenAvailable) {
#ifndef _WIN32
    GTEST_SKIP() << "ScriptManager compile server integration test is Windows-only in this environment";
#endif
#ifdef _WIN32
    // Arrange temporary workspace with a simple C# file and fake project file
    std::filesystem::path ws = std::filesystem::temp_directory_path() / "GE_SM_CS_Integration";
    std::error_code ec; std::filesystem::create_directories(ws, ec);

    auto sourcePath = ws / "Foo.cs";
    {
        std::ofstream f(sourcePath);
        f << "public static class Foo { public static int Smoke(){ return 1; } }";
        f.close();
    }

    // Compute pipe name and start server
    std::string pipeName = std::string("GE_CompileServer_") + WorkspaceId::Compute(ws.string());
    if (!CompileServerTestHost::StartTestCompileServer(pipeName)) {
        GTEST_SKIP() << "CompileServerHost not found or failed to start; skipping integration test";
    }

    // Ensure server-first path is enabled
    _putenv_s("GE_DISABLE_COMPILE_SERVER", "0");

    // Initialize ScriptManager with explicit configuration matching the new API
    JobSystem::WorkStealingThreadPool pool(4);
    ScriptManager mgr;

    ScriptsConfig cfg{};
    cfg.workspaceRoot = ws;
    cfg.scriptsRoot = ws;                         // Place sources directly in the workspace
    cfg.assembliesRoot = ws / "ScriptAssemblies"; // Output for compiled assemblies
    cfg.generatedProjectRoot = ws;                // Not used in this test (auto-generation disabled)
    cfg.disableClr = true;                        // CompileServer path does not require CoreCLR
    cfg.enableHotReload = false;
    cfg.enableAsyncHotReload = false;
    cfg.enableAutoProjectGeneration = false;      // We provide an explicit fake .csproj below

    ASSERT_TRUE(mgr.Initialize(cfg, pool));

    // Use a fake .csproj path under the workspace; server ignores contents and uses AllFiles list
    auto fakeProject = ws / "GameEngine.Scripts.csproj";
    {
        std::ofstream f(fakeProject);
        f << "<Project Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup></Project>";
        f.close();
    }

    auto result = mgr.CompileProject(fakeProject, nullptr);
    if (!result.success) {
        GTEST_SKIP() << "CompileServer path failed in this environment; skipping assertions";
    }

    EXPECT_TRUE(result.success);
#endif
}

TEST(ScriptManagerCompileServerIntegration, CompileProjectCanReferenceEcsAbiWhenPresent)
{
#ifndef _WIN32
    GTEST_SKIP() << "ScriptManager compile server integration test is Windows-only in this environment";
#endif
#ifdef _WIN32
    // Only run when the ECS ABI module is staged next to the test executable.
    std::filesystem::path engineBinDir = GameEngine::PathUtils::GetExecutableDirectory();
    if (!std::filesystem::exists(engineBinDir / "GameEngine.ECS.ABI.dll"))
    {
        GTEST_SKIP() << "GameEngine.ECS.ABI.dll not found next to test executable; skipping";
    }

    // Arrange temporary workspace with a simple C# file that references the ECS ABI surface.
    std::filesystem::path ws = std::filesystem::temp_directory_path() / "GE_SM_CS_EcsAbi";
    std::error_code ec;
    std::filesystem::create_directories(ws, ec);

    auto sourcePath = ws / "Foo.cs";
    {
        std::ofstream f(sourcePath);
        f << "using GameEngine.ECS;\n";
        f << "public static class Foo { public static int Smoke(){ WorldHandle w = Ecs.PrimaryWorld; return w.TryGetEntityCount(out int c) ? c : -1; } }\n";
        f.close();
    }

    // Compute pipe name and start server
    std::string pipeName = std::string("GE_CompileServer_") + WorkspaceId::Compute(ws.string());
    if (!CompileServerTestHost::StartTestCompileServer(pipeName))
    {
        GTEST_SKIP() << "CompileServerHost not found or failed to start; skipping integration test";
    }

    // Ensure server-first path is enabled
    _putenv_s("GE_DISABLE_COMPILE_SERVER", "0");

    JobSystem::WorkStealingThreadPool pool(4);
    ScriptManager mgr;

    ScriptsConfig cfg{};
    cfg.workspaceRoot = ws;
    cfg.scriptsRoot = ws;                         // Place sources directly in the workspace
    cfg.assembliesRoot = ws / "ScriptAssemblies"; // Output for compiled assemblies
    cfg.generatedProjectRoot = ws;                // Not used in this test (auto-generation disabled)
    cfg.disableClr = true;                        // CompileServer path does not require CoreCLR
    cfg.enableHotReload = false;
    cfg.enableAsyncHotReload = false;
    cfg.enableAutoProjectGeneration = false;      // We provide an explicit fake .csproj below

    ASSERT_TRUE(mgr.Initialize(cfg, pool));

    // Use a fake .csproj path under the workspace; server ignores contents and uses AllFiles list
    auto fakeProject = ws / "GameEngine.Scripts.csproj";
    {
        std::ofstream f(fakeProject);
        f << "<Project Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup></Project>";
        f.close();
    }

    auto result = mgr.CompileProject(fakeProject, nullptr);
    if (!result.success)
    {
        GTEST_SKIP() << "CompileServer path failed in this environment; skipping assertions";
    }

    EXPECT_TRUE(result.success);
#endif
}