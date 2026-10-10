#include "Scripting/ScriptingABI.h"
#include "Logger/Logger.h"
#include "Core/Engine.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <cstdlib>
#include <chrono>

using namespace GameEngine;

namespace {

static std::filesystem::path WriteFile(const std::filesystem::path& p, const std::string& text)
{
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
    f.close();
    return p;
}

static int RunCommand(const std::string& cmd)
{
    return std::system(cmd.c_str());
}

static std::filesystem::path BuildTestProject(const std::filesystem::path& tempDir)
{
    const char* csproj = "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
                           "  <PropertyGroup>\n"
                           "    <TargetFramework>net10.0</TargetFramework>\n"
                           "    <OutputType>Library</OutputType>\n"
                           "    <Nullable>enable</Nullable>\n"
                           "  </PropertyGroup>\n"
                           "</Project>";

    const std::string srcV1 = "public static class HotReloadTest { public static int TestMethod() => 1; }";

    auto proj = WriteFile(tempDir / "TestScripts.csproj", csproj);
    auto src = WriteFile(tempDir / "HotReloadTest.cs", srcV1);

    std::string cmd = std::string("dotnet build \"") + proj.string() + "\" -c Debug -v q";
    int rc = RunCommand(cmd);
    EXPECT_EQ(rc, 0) << "dotnet build failed rc=" << rc;

    return tempDir / "bin" / "Debug" / "net10.0" / "TestScripts.dll";
}

static std::filesystem::path RebuildToV2(const std::filesystem::path& tempDir)
{
    const std::string srcV2 = "public static class HotReloadTest { public static int TestMethod() => 2; }";
    auto src = WriteFile(tempDir / "HotReloadTest.cs", srcV2);
    std::string cmd = std::string("dotnet build \"") + (tempDir / "TestScripts.csproj").string() + "\" -c Debug -v q";
    int rc = RunCommand(cmd);
    EXPECT_EQ(rc, 0) << "dotnet rebuild failed rc=" << rc;
    return tempDir / "bin" / "Debug" / "net10.0" / "TestScripts.dll";
}

}

TEST(ScriptingAbi, Native_Edit_Recompile_Preload_Swap_Invoke_Updates_Result)
{
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized()) {
        ApplicationConfig cfg; cfg.AssetDirectory = "Assets";

        ScriptsConfig scriptsConfig{};
        scriptsConfig.enableHotReload = true;
        EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);

        ASSERT_TRUE(engine.Initialize(cfg));
    }

    // Create temp project and build v1
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    auto tempDir = std::filesystem::temp_directory_path() / (std::string("TempNativeScripts_") + std::to_string(static_cast<long long>(ms)));
    auto dllV1 = BuildTestProject(tempDir);
    ASSERT_TRUE(std::filesystem::exists(dllV1)) << "Missing dll: " << dllV1.string();

    // Ensure CoreBridge can resolve HRM by probing current exe directory
#ifdef _WIN32
    _putenv_s("GE_HRM_DIR", std::filesystem::current_path().string().c_str());
#endif
    // Preload + swap
    auto dllUtf8 = dllV1.string();
    ASSERT_EQ(GE_PreloadAssemblyContext(dllUtf8.c_str(), (uint32_t)dllUtf8.size()), GE_Result_Ok);
    ASSERT_EQ(GE_SwapPreloadedContext(), GE_Result_Ok);

    // Invoke method: expect 1
    int32_t out = -999;
    const char* fqn = "HotReloadTest.TestMethod";
    ASSERT_EQ(GE_Invoke(0ULL, fqn, (uint32_t)strlen(fqn), &out), GE_Result_Ok);
    EXPECT_EQ(out, 1);

    // Edit to v2, rebuild, preload+swap
    auto dllV2 = RebuildToV2(tempDir);
    ASSERT_TRUE(std::filesystem::exists(dllV2));
    auto dll2 = dllV2.string();
    ASSERT_EQ(GE_PreloadAssemblyContext(dll2.c_str(), (uint32_t)dll2.size()), GE_Result_Ok);
    ASSERT_EQ(GE_SwapPreloadedContext(), GE_Result_Ok);

    // Invoke again: expect 2
    out = -999;
    ASSERT_EQ(GE_Invoke(0ULL, fqn, (uint32_t)strlen(fqn), &out), GE_Result_Ok);
    EXPECT_EQ(out, 2);
}

