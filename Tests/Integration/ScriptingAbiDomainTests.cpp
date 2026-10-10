#include "ManagedFixturePaths.h"
#include "Scripting/ScriptingABI.h"
#include "Logger/Logger.h"
#include "Core/Engine.h"
#include <gtest/gtest.h>
#include <fstream>

#include "Scripting/ScriptManager.h"
using namespace GameEngine;

static std::vector<uint8_t> ReadAll(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}
TEST(ScriptingAbi, EnsureClrInitialized_Smoke)
{
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized()) {
        ApplicationConfig cfg; cfg.AssetDirectory = "Assets";

        ScriptsConfig scriptsConfig{};
        scriptsConfig.enableHotReload = false;
        engine.SetScriptsConfig(scriptsConfig);

        ASSERT_TRUE(engine.Initialize(cfg));
    }
    // Exercise existing ABI to trigger lazy CLR bootstrap: calling a bogus method causes a Fail but must not crash.
    int32_t out = 0x42;
    EXPECT_EQ(GE_Invoke(0ULL, "Nope.DoesNotExist", 18, &out), GE_Result_Fail);
    EXPECT_EQ(out, 0x42);
    // Now the host route should be alive; id may be 0 if no domain yet, but call should be safe
    auto id = engine.GetScriptManager().GetCLRHost().GetCurrentRuntimeDomainId();
    (void)id;
    SUCCEED();
}



TEST(ScriptingAbi, MultiDomainConcurrentLoadAndInvoke)
{
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized()) {
        ApplicationConfig cfg; cfg.AssetDirectory = "Assets";

        ScriptsConfig scriptsConfig{};
        scriptsConfig.enableHotReload = false;
        engine.SetScriptsConfig(scriptsConfig);

        ASSERT_TRUE(engine.Initialize(cfg));
    }

    // Use the tiny DomainRoutingTest managed assembly once for both domains
    std::filesystem::path dllPath = GameEngine::Tests::DomainRoutingTestDll();
    ASSERT_TRUE(std::filesystem::exists(dllPath));

    auto bytes = ReadAll(dllPath);
    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();


    GE_DomainHandle domA=0, domB=0;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &domA), GE_Result_Ok);
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &domB), GE_Result_Ok);
    ASSERT_NE(domA, 0ull); ASSERT_NE(domB, 0ull); ASSERT_NE(domA, domB);

    // Invoke from both domains using a non-legacy method in Scripts
    int32_t r1 = -999, r2 = -999;
    const char* fq = "GameEngine.Scripts.ScriptsEntryPoint.OnAssemblyLoaded";
    EXPECT_EQ(GE_Invoke(domA, fq, (uint32_t)strlen(fq), &r1), GE_Result_Ok);
    EXPECT_EQ(GE_Invoke(domB, fq, (uint32_t)strlen(fq), &r2), GE_Result_Ok);
    EXPECT_GE(r1, 0); EXPECT_GE(r2, 0);
}


TEST(ScriptingAbi, InvokeFailureDoesNotClobber)
{
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized()) {
        ApplicationConfig cfg; cfg.AssetDirectory = "Assets";

        ScriptsConfig scriptsConfig{};
        scriptsConfig.enableHotReload = false;
        engine.SetScriptsConfig(scriptsConfig);

        ASSERT_TRUE(engine.Initialize(cfg));
    }

    int32_t out = 12345;
    EXPECT_EQ(GE_Invoke(0ULL, "Nope.DoesNotExist", 18, &out), GE_Result_Fail);
    EXPECT_EQ(out, 12345);
}


TEST(ScriptingAbi, InvokeManagedNegativePreservesOutResult)
{
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized()) {
        ApplicationConfig cfg; cfg.AssetDirectory = "Assets";

        ScriptsConfig scriptsConfig{};
        scriptsConfig.enableHotReload = false;
        engine.SetScriptsConfig(scriptsConfig);

        ASSERT_TRUE(engine.Initialize(cfg));
    }

    // Prepare package from the DomainRoutingTest dll (fast, well-defined exports)
    std::filesystem::path dllPath = GameEngine::Tests::DomainRoutingTestDll();
    ASSERT_TRUE(std::filesystem::exists(dllPath));

    auto bytes = ReadAll(dllPath);
    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();

    GE_DomainHandle dom=0;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &dom), GE_Result_Ok);
    ASSERT_NE(dom, 0ull);

    int32_t out = 9999;
    // Fully-qualified name of the method we added to Scripts
    const char* fq = "GameEngine.Scripts.ScriptsEntryPoint.ReturnNegative";
    EXPECT_EQ(GE_Invoke(dom, fq, (uint32_t)strlen(fq), &out), GE_Result_Fail);
    EXPECT_EQ(out, 9999);
}


TEST(ScriptingAbi, PerDomainBehaviorIsolation)
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
    auto bytes = ReadAll(dllPath);
    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();

    GE_DomainHandle domA=0, domB=0;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &domA), GE_Result_Ok);
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &domB), GE_Result_Ok);
    ASSERT_NE(domA, 0ull); ASSERT_NE(domB, 0ull); ASSERT_NE(domA, domB);

    const char* reset = "GameEngine.Scripts.ScriptsEntryPoint.Reset";
    const char* inc   = "GameEngine.Scripts.ScriptsEntryPoint.IncrementAndGet";
    int32_t tmp;

    // Reset both domains, then mutate different numbers of times
    tmp = -1; EXPECT_EQ(GE_Invoke(domA, reset, (uint32_t)strlen(reset), &tmp), GE_Result_Ok);
    tmp = -1; EXPECT_EQ(GE_Invoke(domB, reset, (uint32_t)strlen(reset), &tmp), GE_Result_Ok);


    // A increments twice: result should be 1 then 2
    tmp = -1; EXPECT_EQ(GE_Invoke(domA, inc, (uint32_t)strlen(inc), &tmp), GE_Result_Ok); EXPECT_EQ(tmp, 1);
    tmp = -1; EXPECT_EQ(GE_Invoke(domA, inc, (uint32_t)strlen(inc), &tmp), GE_Result_Ok); EXPECT_EQ(tmp, 2);

    // B increments once: result should be 1
    tmp = -1; EXPECT_EQ(GE_Invoke(domB, inc, (uint32_t)strlen(inc), &tmp), GE_Result_Ok); EXPECT_EQ(tmp, 1);

    // Verify isolation by final reads
    const char* getVal = "GameEngine.Scripts.ScriptsEntryPoint.GetValue";
    int32_t aVal=-1, bVal=-1;
    EXPECT_EQ(GE_Invoke(domA, getVal, (uint32_t)strlen(getVal), &aVal), GE_Result_Ok);
    EXPECT_EQ(GE_Invoke(domB, getVal, (uint32_t)strlen(getVal), &bVal), GE_Result_Ok);
    EXPECT_EQ(aVal, 2);
    EXPECT_EQ(bVal, 1);
}

TEST(ScriptingAbi, TokenApi_QueryAndInvoke)
{
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized()) {
        ApplicationConfig cfg; cfg.AssetDirectory = "Assets";

        ScriptsConfig scriptsConfig{};
        scriptsConfig.enableHotReload = false;
        engine.SetScriptsConfig(scriptsConfig);

        ASSERT_TRUE(engine.Initialize(cfg));
    }

    // Use the DomainRoutingTest assembly for stability
    std::filesystem::path dllPath = GameEngine::Tests::DomainRoutingTestDll();
    ASSERT_TRUE(std::filesystem::exists(dllPath));

    auto bytes = ReadAll(dllPath);
    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();

    GE_DomainHandle dom=0;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &dom), GE_Result_Ok);
    ASSERT_NE(dom, 0ull);

    // Query export for a well-known method and invoke by token
    const char* fq = "GameEngine.Scripts.ScriptsEntryPoint.OnAssemblyLoaded";
    uint64_t token = 0ull;
    ASSERT_EQ(GE_QueryExport(dom, fq, (uint32_t)strlen(fq), &token), GE_Result_Ok);
    ASSERT_NE(token, 0ull);

    int32_t result = -777;
    ASSERT_EQ(GE_InvokeByToken(dom, token, &result), GE_Result_Ok);
    EXPECT_GE(result, 0);

    // Query again: token should be stable
    uint64_t token2 = 0ull;
    ASSERT_EQ(GE_QueryExport(dom, fq, (uint32_t)strlen(fq), &token2), GE_Result_Ok);
    EXPECT_EQ(token2, token);

    // Unload domain and verify token becomes invalid
    ASSERT_EQ(GE_ScriptsDomainUnload(dom), GE_Result_Ok);
    int32_t result2 = 1234;
    EXPECT_EQ(GE_InvokeByToken(dom, token, &result2), GE_Result_Fail);
    EXPECT_EQ(result2, 1234);
}


// Extended: also assert that CoreBridge reports distinct current domain IDs after loads
TEST(ScriptingAbi, PerDomainBehaviorIsolation_ReportsDistinctDomainIds)
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
    auto bytes = ReadAll(dllPath);
    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();

    GE_DomainHandle domA=0, domB=0;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &domA), GE_Result_Ok);
    auto idAfterA = engine.GetScriptManager().GetCLRHost().GetCurrentRuntimeDomainId();
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &domB), GE_Result_Ok);
    auto idAfterB = engine.GetScriptManager().GetCLRHost().GetCurrentRuntimeDomainId();

    ASSERT_NE(domA, 0ull); ASSERT_NE(domB, 0ull);
    EXPECT_NE(domA, domB);
    EXPECT_NE(idAfterA, 0ull); EXPECT_NE(idAfterB, 0ull);
    EXPECT_NE(idAfterA, idAfterB);
}





TEST(ScriptingAbi, DomainLifecycle_CycleUnloadRecreate_TokensRepair)
{
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized()) {
        ApplicationConfig cfg; cfg.AssetDirectory = "Assets";

        ScriptsConfig scriptsConfig{};
        scriptsConfig.enableHotReload = false;
        engine.SetScriptsConfig(scriptsConfig);

        ASSERT_TRUE(engine.Initialize(cfg));
    }

    // Use the tiny DomainRoutingTest.dll for fast domain spins
    std::filesystem::path dllPath = GameEngine::Tests::DomainRoutingTestDll();
    ASSERT_TRUE(std::filesystem::exists(dllPath));
    auto bytes = ReadAll(dllPath);
    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();

    GE_DomainHandle domA=0, domB=0;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &domA), GE_Result_Ok);
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &domB), GE_Result_Ok);
    ASSERT_NE(domA, 0ull); ASSERT_NE(domB, 0ull); ASSERT_NE(domA, domB);

    // Query tokens in each domain for a known method
    const char* fqReset = "GameEngine.Scripts.ScriptsEntryPoint.Reset";
    uint64_t tokA=0, tokB=0;
    ASSERT_EQ(GE_QueryExport(domA, fqReset, (uint32)strlen(fqReset), &tokA), GE_Result_Ok);
    ASSERT_EQ(GE_QueryExport(domB, fqReset, (uint32)strlen(fqReset), &tokB), GE_Result_Ok);
    ASSERT_NE(tokA, 0ull); ASSERT_NE(tokB, 0ull);

    // Cycle A: unload, verify old token fails, recreate, verify new token works
    ASSERT_EQ(GE_ScriptsDomainUnload(domA), GE_Result_Ok);
    int32_t outA = 4321; EXPECT_EQ(GE_InvokeByToken(domA, tokA, &outA), GE_Result_Fail); EXPECT_EQ(outA, 4321);

    GE_DomainHandle domA2=0; ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &domA2), GE_Result_Ok);
    ASSERT_NE(domA2, 0ull); EXPECT_NE(domA2, domA);
    uint64_t tokA2=0; ASSERT_EQ(GE_QueryExport(domA2, fqReset, (uint32)strlen(fqReset), &tokA2), GE_Result_Ok); ASSERT_NE(tokA2, 0ull);
    int32_t resA2 = -7; ASSERT_EQ(GE_InvokeByToken(domA2, tokA2, &resA2), GE_Result_Ok);

    // Cycle B: same procedure
    ASSERT_EQ(GE_ScriptsDomainUnload(domB), GE_Result_Ok);
    int32_t outB = 9876; EXPECT_EQ(GE_InvokeByToken(domB, tokB, &outB), GE_Result_Fail); EXPECT_EQ(outB, 9876);

    GE_DomainHandle domB2=0; ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &domB2), GE_Result_Ok);
    ASSERT_NE(domB2, 0ull); EXPECT_NE(domB2, domB);
    uint64_t tokB2=0; ASSERT_EQ(GE_QueryExport(domB2, fqReset, (uint32)strlen(fqReset), &tokB2), GE_Result_Ok); ASSERT_NE(tokB2, 0ull);
    int32_t resB2 = -9; ASSERT_EQ(GE_InvokeByToken(domB2, tokB2, &resB2), GE_Result_Ok);
}


TEST(ScriptingAbi, DomainLifecycle_RepeatedCycles_TokensAlwaysRepair)
{
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized()) {
        ApplicationConfig cfg; cfg.AssetDirectory = "Assets";

        ScriptsConfig scriptsConfig{};
        scriptsConfig.enableHotReload = false;
        engine.SetScriptsConfig(scriptsConfig);

        ASSERT_TRUE(engine.Initialize(cfg));
    }

    // Use tiny DomainRoutingTest.dll for fast cycles
    std::filesystem::path dllPath = GameEngine::Tests::DomainRoutingTestDll();
    ASSERT_TRUE(std::filesystem::exists(dllPath));
    auto bytes = ReadAll(dllPath);
    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();


    auto cycle = [&](){
        GE_DomainHandle dom=0;
        ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &dom), GE_Result_Ok);
        ASSERT_NE(dom, 0ull);
        const char* fqReset = "GameEngine.Scripts.ScriptsEntryPoint.Reset";
        uint64_t tok=0; ASSERT_EQ(GE_QueryExport(dom, fqReset, (uint32_t)strlen(fqReset), &tok), GE_Result_Ok);
        int32_t out= -1; ASSERT_EQ(GE_InvokeByToken(dom, tok, &out), GE_Result_Ok);
        // Unload
        ASSERT_EQ(GE_ScriptsDomainUnload(dom), GE_Result_Ok);
        GE_DomainHandle got=123; ASSERT_EQ(GE_GetCurrentRuntimeDomain(&got), GE_Result_Ok); EXPECT_EQ(got, 0ull);
        int32_t out2 = 777; EXPECT_EQ(GE_InvokeByToken(dom, tok, &out2), GE_Result_Fail); EXPECT_EQ(out2, 777);
    };

    for (int i=0;i<6;++i) {
        cycle();
    }
}

TEST(ScriptingAbi, DomainLifecycle_InterleavedHotReloadAndUnload)
{
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized()) {
        ApplicationConfig cfg; cfg.AssetDirectory = "Assets";

        ScriptsConfig scriptsConfig{};
        scriptsConfig.enableHotReload = false;
        engine.SetScriptsConfig(scriptsConfig);

        ASSERT_TRUE(engine.Initialize(cfg));
    }

    // DomainRoutingTest keeps test fast and deterministic
    std::filesystem::path dllPath = GameEngine::Tests::DomainRoutingTestDll();
    ASSERT_TRUE(std::filesystem::exists(dllPath));
    auto bytes = ReadAll(dllPath);
    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();


    // Create A then B
    GE_DomainHandle domA=0, domB=0;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &domA), GE_Result_Ok);
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &domB), GE_Result_Ok);
    ASSERT_NE(domA, 0ull); ASSERT_NE(domB, 0ull); ASSERT_NE(domA, domB);

    const char* fqReset = "GameEngine.Scripts.ScriptsEntryPoint.Reset";
    uint64_t tokA=0, tokB=0; ASSERT_EQ(GE_QueryExport(domA, fqReset, (uint32_t)strlen(fqReset), &tokA), GE_Result_Ok);
    ASSERT_EQ(GE_QueryExport(domB, fqReset, (uint32_t)strlen(fqReset), &tokB), GE_Result_Ok);
    int32_t r=0; ASSERT_EQ(GE_InvokeByToken(domA, tokA, &r), GE_Result_Ok); ASSERT_EQ(GE_InvokeByToken(domB, tokB, &r), GE_Result_Ok);

    // Interleave: unload A, recreate A, then unload B, recreate B
    ASSERT_EQ(GE_ScriptsDomainUnload(domA), GE_Result_Ok);
    int32_t outA = 123; EXPECT_EQ(GE_InvokeByToken(domA, tokA, &outA), GE_Result_Fail); EXPECT_EQ(outA, 123);
    GE_DomainHandle domA2=0; ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &domA2), GE_Result_Ok);
    uint64_t tokA2=0; ASSERT_EQ(GE_QueryExport(domA2, fqReset, (uint32_t)strlen(fqReset), &tokA2), GE_Result_Ok);
    ASSERT_NE(tokA2, 0ull); int32_t rA2=0; ASSERT_EQ(GE_InvokeByToken(domA2, tokA2, &rA2), GE_Result_Ok);

    ASSERT_EQ(GE_ScriptsDomainUnload(domB), GE_Result_Ok);
    int32_t outB = 456; EXPECT_EQ(GE_InvokeByToken(domB, tokB, &outB), GE_Result_Fail); EXPECT_EQ(outB, 456);
    GE_DomainHandle domB2=0; ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &domB2), GE_Result_Ok);
    uint64_t tokB2=0; ASSERT_EQ(GE_QueryExport(domB2, fqReset, (uint32_t)strlen(fqReset), &tokB2), GE_Result_Ok);
    ASSERT_NE(tokB2, 0ull); int32_t rB2=0; ASSERT_EQ(GE_InvokeByToken(domB2, tokB2, &rB2), GE_Result_Ok);
}


TEST(ScriptingAbi, CoreBridgeResolver_CwdFallback_Smoke)
{
    // Require CoreBridge present in CWD to validate fallback; otherwise skip (environment-dependent)
    auto cwdCoreBridge = std::filesystem::current_path() / "GameEngine.CoreBridge.dll";
    if (!std::filesystem::exists(cwdCoreBridge)) {
        GTEST_SKIP() << "GameEngine.CoreBridge.dll not present in CWD; skipping fallback smoke test";
    }

    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized()) {
        ApplicationConfig cfg; cfg.AssetDirectory = "Assets";

        ScriptsConfig scriptsConfig{};
        scriptsConfig.enableHotReload = false;
        engine.SetScriptsConfig(scriptsConfig);

        ASSERT_TRUE(engine.Initialize(cfg));
    }

    // Use tiny DomainRoutingTest.dll for a quick preload
    std::filesystem::path dllPath = GameEngine::Tests::DomainRoutingTestDll();
    ASSERT_TRUE(std::filesystem::exists(dllPath));
    const auto dllUtf8 = dllPath.u8string();
    ASSERT_EQ(GE_PreloadAssemblyContext(reinterpret_cast<const char*>(dllUtf8.c_str()), (uint32_t)dllUtf8.size()), GE_Result_Ok);

    ASSERT_EQ(GE_SwapPreloadedContext(), GE_Result_Ok);

    GE_DomainHandle dom = 0;
    ASSERT_EQ(GE_GetCurrentRuntimeDomain(&dom), GE_Result_Ok);
    ASSERT_NE(dom, 0ull);

    // Cleanup
    ASSERT_EQ(GE_ScriptsDomainUnload(dom), GE_Result_Ok);
}

TEST(ScriptingAbi, HotReload_SwapPreloaded_IndicesPublished)
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
    const auto dllUtf8 = dllPath.u8string();
    ASSERT_EQ(GE_PreloadAssemblyContext(reinterpret_cast<const char*>(dllUtf8.c_str()), (uint32_t)dllUtf8.size()), GE_Result_Ok);
    ASSERT_EQ(GE_SwapPreloadedContext(), GE_Result_Ok);

    GE_DomainHandle dom = 0; ASSERT_EQ(GE_GetCurrentRuntimeDomain(&dom), GE_Result_Ok); ASSERT_NE(dom, 0ull);

    // Immediately query a known export; if indices were published on swap, this is fast and succeeds
    const char* fqReset = "GameEngine.Scripts.ScriptsEntryPoint.Reset";
    uint64_t tok = 0ull;
    ASSERT_EQ(GE_QueryExport(dom, fqReset, (uint32_t)strlen(fqReset), &tok), GE_Result_Ok);
    ASSERT_NE(tok, 0ull);

    // Cleanup
    ASSERT_EQ(GE_ScriptsDomainUnload(dom), GE_Result_Ok);
}
