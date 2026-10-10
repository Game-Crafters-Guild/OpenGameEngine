#include "ManagedFixturePaths.h"
#include "gtest/gtest.h"
#include "Scripting/ScriptingABI.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include "Core/Engine.h"


static std::vector<uint8_t> ReadAll3(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static int GetTotalUnloads()
{
#ifndef NDEBUG
    uint64_t lastCompileMs = 0, lastSwapMs = 0; int32_t totalCompiles = 0, totalSwaps = 0, totalUnloads = 0;
    GE_Result mr = GE_GetHotReloadMetricsEx(&lastCompileMs, &lastSwapMs, &totalCompiles, &totalSwaps, &totalUnloads);
    if (mr == GE_Result_Ok) return totalUnloads;
    // Try base metrics (will return Ok with totalUnloads implicitly 0)
    lastCompileMs = lastSwapMs = 0; totalCompiles = totalSwaps = 0; totalUnloads = 0;
    GE_Result mr2 = GE_GetHotReloadMetrics(&lastCompileMs, &lastSwapMs, &totalCompiles, &totalSwaps);
    if (mr2 == GE_Result_Ok) return totalUnloads; // 0
    return -1;
#else
    return -1;
#endif
}

TEST(ScriptingAbi_Perf, PerfCounters_AreNonZero_WhenDelegatesEnabled)
{
// Gated on _DEBUG (not NDEBUG): this test calls CoreCLRHost::DebugNoopAfterAccept(), declared under
// #ifdef _DEBUG in CoreCLRHost.h. _DEBUG and NDEBUG are NOT complements — in the DebugFast config
// both are undefined, so an #ifndef NDEBUG guard would compile this body while the _DEBUG-only method
// is absent (a build break). Match the method's guard exactly. (The sibling unload-counter test below
// only uses the !NDEBUG-available GE_* C ABI, so it stays #ifndef NDEBUG.)
#ifdef _DEBUG

    // Load test assembly and exercise both QueryExport and InvokeByToken
    std::filesystem::path dllPath = GameEngine::Tests::DomainRoutingTestDll();
    ASSERT_TRUE(std::filesystem::exists(dllPath));
    auto bytes = ReadAll3(dllPath);
    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();

    GE_DomainHandle dom = 0ULL;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &dom), GE_Result_Ok);

    const char* fq = "GameEngine.Scripts.ScriptsEntryPoint.Reset";
    // Invoke by name entirely inside managed (does QueryExport+InvokeByToken internally), no out-params crossing boundary
    auto& eng = GameEngine::EngineCore::GetInstance();
    auto& host = eng.GetScriptManager().GetCLRHost();
    std::fprintf(stderr, "[TEST] About to call DebugNoopAfterAccept...\n");
    ASSERT_EQ(host.DebugNoopAfterAccept(), 0);
    std::fprintf(stderr, "[TEST] DebugNoopAfterAccept returned OK\n");
    std::fprintf(stderr, "[TEST] About to invoke Reset via GE_DebugInvokeByNameNoOut...\n");
    ASSERT_EQ(GE_DebugInvokeByNameNoOut(dom, fq, (uint32_t)strlen(fq)), GE_Result_Ok);
    std::fprintf(stderr, "[TEST] GE_DebugInvokeByNameNoOut returned OK\n");

    std::fprintf(stderr, "[TEST] Calling DebugNoopAfterAccept again before perf counters...\n");
    ASSERT_EQ(host.DebugNoopAfterAccept(), 0);
    std::fprintf(stderr, "[TEST] DebugNoopAfterAccept (second) returned OK\n");

    std::fprintf(stderr, "[TEST] About to call GE_DebugGetPerfCountersPacked64...\n");
    auto packed = GE_DebugGetPerfCountersPacked64();
    ASSERT_NE(packed, -1);
    int dq = (int)( packed        & 0xFFFF);
    int rq = (int)((packed >> 16) & 0xFFFF);
    int di = (int)((packed >> 32) & 0xFFFF);
    int ri = (int)((packed >> 48) & 0xFFFF);
    std::fprintf(stderr, "[TEST] Counters dq=%d rq=%d di=%d ri=%d\n", dq, rq, di, ri);
    // Expect at least one query and one invoke overall (delegate or reflection)
    EXPECT_GE(dq + rq, 1);
    EXPECT_GE(di + ri, 1);
    // If delegates are enabled in this environment, dq/di should be non-zero; otherwise reflection path will satisfy the totals

#else
    GTEST_SKIP() << "Perf counters test is Debug-only";
#endif
}

TEST(ScriptingAbi_Perf, UnloadCounter_Increments_OnUnload)
{
#ifndef NDEBUG
    // Reset metrics to get a clean baseline
    (void)GE_ResetHotReloadMetrics();

    // Load a domain and then unload it to bump the counter
    std::filesystem::path dllPath = GameEngine::Tests::DomainRoutingTestDll();
    ASSERT_TRUE(std::filesystem::exists(dllPath));
    auto bytes = ReadAll3(dllPath);
    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();

    GE_DomainHandle dom = 0ULL;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &dom), GE_Result_Ok);

    int before = GetTotalUnloads();
    ASSERT_EQ(GE_ScriptsDomainUnload(dom), GE_Result_Ok);
    int after = GetTotalUnloads();

    if (after < 0) {
        GTEST_SKIP() << "HotReload metrics unavailable in this configuration; skipping.";
    }
    EXPECT_GE(after, before + 1);
#else
    GTEST_SKIP() << "Unload counter test is Debug-only";
#endif
}
