#include "ManagedFixturePaths.h"
#include "Scripting/ScriptingABI.h"
#include "Logger/Logger.h"
#include "Core/Engine.h"
#include <gtest/gtest.h>
#include <fstream>

using namespace GameEngine;

static std::vector<uint8_t> ReadAll(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

TEST(ScriptingAbi, CreateDomainFromPackageAndInvoke)
{
    // Ensure engine is initialized
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized()) {
        ApplicationConfig cfg; cfg.AssetDirectory = "Assets";

        ScriptsConfig scriptsConfig{};
        scriptsConfig.enableHotReload = false;
        engine.SetScriptsConfig(scriptsConfig);

        ASSERT_TRUE(engine.Initialize(cfg));
    }

    // Use the tiny DomainRoutingTest managed assembly as the package payload for stability
    std::filesystem::path dllPath = GameEngine::Tests::DomainRoutingTestDll();
    ASSERT_TRUE(std::filesystem::exists(dllPath)) << dllPath.string();

    auto bytes = ReadAll(dllPath);
    ASSERT_FALSE(bytes.empty());

    GE_PackageEntry entry{};
    entry.assembly.data = bytes.data();
    entry.assembly.length = (uint32_t)bytes.size();
    entry.pdb.data = nullptr; entry.pdb.length = 0;

    GE_DomainHandle domain = 0;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &domain), GE_Result_Ok);
    ASSERT_NE(domain, 0ull);

    // Invoke a well-known method in the scripts entrypoint; OnAssemblyLoaded should be discoverable and return >=0
    int32_t result = -1;
    const char* kMethod = "GameEngine.Scripts.ScriptsEntryPoint.OnAssemblyLoaded";
    ASSERT_EQ(GE_Invoke(domain, kMethod, (uint32_t)strlen(kMethod), &result), GE_Result_Ok);
    EXPECT_GE(result, 0);
}



TEST(ScriptingAbi, CreateFromPackage_ReturnsManagedDomainId)
{
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized()) {
        ApplicationConfig cfg; cfg.AssetDirectory = "Assets";

        ScriptsConfig scriptsConfig{};
        scriptsConfig.enableHotReload = false;
        engine.SetScriptsConfig(scriptsConfig);

        ASSERT_TRUE(engine.Initialize(cfg));
    }

    std::filesystem::path dllPath = GameEngine::Tests::FindScriptsAssemblyDll();
    if (dllPath.empty()) {
        GTEST_SKIP() << GameEngine::Tests::kScriptsAssemblyMissingReason;
    }
    ASSERT_TRUE(std::filesystem::exists(dllPath));

    auto bytes = ReadAll(dllPath);
    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();

    GE_DomainHandle dom = 0;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &dom), GE_Result_Ok);
    EXPECT_NE(dom, 0ull);
}
