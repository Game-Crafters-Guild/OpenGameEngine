#include "ManagedFixturePaths.h"
#include "Scripting/ScriptingABI.h"
#include "Core/Engine.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

using namespace GameEngine;

static std::vector<uint8_t> ReadAllFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

TEST(ScriptingAbi_EditorDomain, EditorLoadSetsOnlyTheEditorDomain)
{
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized()) {
        ApplicationConfig cfg; cfg.AssetDirectory = "Assets";

        ScriptsConfig scriptsConfig{};
        scriptsConfig.enableHotReload = false;
        engine.SetScriptsConfig(scriptsConfig);

        ASSERT_TRUE(engine.Initialize(cfg));
    }

    // Use DomainRoutingTest to create a domain deterministically
    std::filesystem::path dllPath = GameEngine::Tests::DomainRoutingTestDll();
    ASSERT_TRUE(std::filesystem::exists(dllPath));
    auto bytes = ReadAllFile(dllPath);

    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();

    GE_DomainHandle rtBefore = ~0ull;
    ASSERT_EQ(GE_GetCurrentRuntimeDomain(&rtBefore), GE_Result_Ok);

    GE_DomainHandle ed = 0;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Editor | GE_Pkg_Collectible, &ed), GE_Result_Ok);
    ASSERT_NE(ed, 0ull);

    GE_DomainHandle gotEd = 0, gotRt = 0;
    ASSERT_EQ(GE_GetCurrentEditorDomain(&gotEd), GE_Result_Ok);
    ASSERT_EQ(GE_GetCurrentRuntimeDomain(&gotRt), GE_Result_Ok);
    EXPECT_EQ(gotEd, ed);
    EXPECT_EQ(gotRt, rtBefore) << "An editor package must not change the runtime domain";
}

TEST(ScriptingAbi_EditorDomain, EditorAndRuntimeDomainsAreDistinct)
{
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized()) {
        ApplicationConfig cfg; cfg.AssetDirectory = "Assets";

        ScriptsConfig scriptsConfig{};
        scriptsConfig.enableHotReload = false;
        engine.SetScriptsConfig(scriptsConfig);

        ASSERT_TRUE(engine.Initialize(cfg));
    }

    std::filesystem::path dllPath = GameEngine::Tests::DomainRoutingTestDll();
    ASSERT_TRUE(std::filesystem::exists(dllPath));
    auto bytes = ReadAllFile(dllPath);

    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();

    GE_DomainHandle ed = 0, rt = 0;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Editor | GE_Pkg_Collectible, &ed), GE_Result_Ok);
    ASSERT_NE(ed, 0ull);
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &rt), GE_Result_Ok);
    ASSERT_NE(rt, 0ull);

    GE_DomainHandle gotEd = 0, gotRt = 0;
    ASSERT_EQ(GE_GetCurrentEditorDomain(&gotEd), GE_Result_Ok);
    ASSERT_EQ(GE_GetCurrentRuntimeDomain(&gotRt), GE_Result_Ok);
    EXPECT_EQ(gotEd, ed);
    EXPECT_EQ(gotRt, rt);
    EXPECT_NE(gotEd, gotRt);
}

TEST(ScriptingAbi_EditorDomain, UnloadEditorDoesNotAffectRuntime)
{
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized()) {
        ApplicationConfig cfg; cfg.AssetDirectory = "Assets";

        ScriptsConfig scriptsConfig{};
        scriptsConfig.enableHotReload = false;
        engine.SetScriptsConfig(scriptsConfig);

        ASSERT_TRUE(engine.Initialize(cfg));
    }

    std::filesystem::path dllPath = GameEngine::Tests::DomainRoutingTestDll();
    ASSERT_TRUE(std::filesystem::exists(dllPath));
    auto bytes = ReadAllFile(dllPath);

    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();

    GE_DomainHandle ed = 0, rt = 0;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Editor | GE_Pkg_Collectible, &ed), GE_Result_Ok);
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &rt), GE_Result_Ok);

    // Unload editor domain
    ASSERT_EQ(GE_ScriptsDomainUnload(ed), GE_Result_Ok);

    GE_DomainHandle gotEd = 1234, gotRt = 0;
    ASSERT_EQ(GE_GetCurrentEditorDomain(&gotEd), GE_Result_Ok);
    ASSERT_EQ(GE_GetCurrentRuntimeDomain(&gotRt), GE_Result_Ok);
    EXPECT_EQ(gotEd, 0ull);
    EXPECT_EQ(gotRt, rt);
}

