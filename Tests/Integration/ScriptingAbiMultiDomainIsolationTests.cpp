#include "ManagedFixturePaths.h"
#include "gtest/gtest.h"
#include "Scripting/ScriptingABI.h"
#include "Core/Engine.h"
#include <filesystem>
#include <fstream>

using namespace GameEngine;

static std::vector<uint8_t> ReadAllIso(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

TEST(ScriptingAbi_MultiDomainIsolation, InterleavedQueryInvokeAndUnload)
{
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized()) {
        ApplicationConfig cfg; cfg.AssetDirectory = "Assets";

        ScriptsConfig scriptsConfig{};
        scriptsConfig.enableHotReload = false;
        engine.SetScriptsConfig(scriptsConfig);

        ASSERT_TRUE(engine.Initialize(cfg));
    }

    // Use deterministic test assembly with reset/inc/get
    std::filesystem::path dllPath = GameEngine::Tests::DomainRoutingTestDll();
    ASSERT_TRUE(std::filesystem::exists(dllPath));
    auto bytes = ReadAllIso(dllPath);

    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();

    GE_DomainHandle dA=0, dB=0, dC=0;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &dA), GE_Result_Ok);
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &dB), GE_Result_Ok);
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &dC), GE_Result_Ok);
    ASSERT_NE(dA, 0ull); ASSERT_NE(dB, 0ull); ASSERT_NE(dC, 0ull);
    ASSERT_NE(dA, dB); ASSERT_NE(dA, dC); ASSERT_NE(dB, dC);

    const char* reset = "GameEngine.Scripts.ScriptsEntryPoint.Reset";
    const char* inc   = "GameEngine.Scripts.ScriptsEntryPoint.IncrementAndGet";
    const char* getV  = "GameEngine.Scripts.ScriptsEntryPoint.GetValue";

    int32_t tmp;
    // Reset all
    tmp=-1; ASSERT_EQ(GE_Invoke(dA, reset, (uint32_t)strlen(reset), &tmp), GE_Result_Ok);
    tmp=-1; ASSERT_EQ(GE_Invoke(dB, reset, (uint32_t)strlen(reset), &tmp), GE_Result_Ok);
    tmp=-1; ASSERT_EQ(GE_Invoke(dC, reset, (uint32_t)strlen(reset), &tmp), GE_Result_Ok);

    // Interleave increments: A=2, B=1, C=3
    tmp=-1; ASSERT_EQ(GE_Invoke(dA, inc, (uint32_t)strlen(inc), &tmp), GE_Result_Ok);
    tmp=-1; ASSERT_EQ(GE_Invoke(dB, inc, (uint32_t)strlen(inc), &tmp), GE_Result_Ok);
    tmp=-1; ASSERT_EQ(GE_Invoke(dC, inc, (uint32_t)strlen(inc), &tmp), GE_Result_Ok);
    tmp=-1; ASSERT_EQ(GE_Invoke(dA, inc, (uint32_t)strlen(inc), &tmp), GE_Result_Ok);
    tmp=-1; ASSERT_EQ(GE_Invoke(dC, inc, (uint32_t)strlen(inc), &tmp), GE_Result_Ok);
    tmp=-1; ASSERT_EQ(GE_Invoke(dC, inc, (uint32_t)strlen(inc), &tmp), GE_Result_Ok);

    // Query tokens for GetValue in each domain
    uint64_t tA=0, tB=0, tC=0;
    ASSERT_EQ(GE_QueryExport(dA, getV, (uint32_t)strlen(getV), &tA), GE_Result_Ok);
    ASSERT_EQ(GE_QueryExport(dB, getV, (uint32_t)strlen(getV), &tB), GE_Result_Ok);
    ASSERT_EQ(GE_QueryExport(dC, getV, (uint32_t)strlen(getV), &tC), GE_Result_Ok);
    ASSERT_NE(tA, 0ull); ASSERT_NE(tB, 0ull); ASSERT_NE(tC, 0ull);

    // Verify values via tokens (swap current runtime domain for HRM before invoking)
    int32_t a=-1,b=-1,c=-1;
    ASSERT_EQ(GE_ScriptsDomainSwap(dA), GE_Result_Ok);
    ASSERT_EQ(GE_InvokeByToken(dA, tA, &a), GE_Result_Ok);
    ASSERT_EQ(GE_ScriptsDomainSwap(dB), GE_Result_Ok);
    ASSERT_EQ(GE_InvokeByToken(dB, tB, &b), GE_Result_Ok);
    ASSERT_EQ(GE_ScriptsDomainSwap(dC), GE_Result_Ok);
    ASSERT_EQ(GE_InvokeByToken(dC, tC, &c), GE_Result_Ok);
    EXPECT_EQ(a, 2); EXPECT_EQ(b, 1); EXPECT_EQ(c, 3);

    // Unload B; A/C tokens should still work; B's token must fail and not clobber out
    ASSERT_EQ(GE_ScriptsDomainUnload(dB), GE_Result_Ok);
    a=-1; c=-1; int32_t keep=7777;
    ASSERT_EQ(GE_ScriptsDomainSwap(dA), GE_Result_Ok);
    ASSERT_EQ(GE_InvokeByToken(dA, tA, &a), GE_Result_Ok);
    ASSERT_EQ(GE_ScriptsDomainSwap(dC), GE_Result_Ok);
    ASSERT_EQ(GE_InvokeByToken(dC, tC, &c), GE_Result_Ok);
    EXPECT_EQ(a, 2); EXPECT_EQ(c, 3);
    ASSERT_EQ(GE_ScriptsDomainSwap(dB), GE_Result_Ok);
    EXPECT_EQ(GE_InvokeByToken(dB, tB, &keep), GE_Result_Fail);
    EXPECT_EQ(keep, 7777);
}

