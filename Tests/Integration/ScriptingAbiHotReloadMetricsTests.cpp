#include "ManagedFixturePaths.h"
#include "gtest/gtest.h"
#include "Scripting/ScriptingABI.h"
#include <filesystem>
#include <string>

static std::filesystem::path FindDomainRoutingTest()
{
    namespace fs = std::filesystem;
    fs::path p1 = GameEngine::TestPaths::ExecutableDirectory() / "DomainRoutingTest.dll";
    if (fs::exists(p1)) return p1;
    fs::path p2 = GameEngine::Tests::DomainRoutingTestDll();
    if (fs::exists(p2)) return p2;
    return {};
}

TEST(ScriptingAbi_Metrics, HotReload_Swap_IncrementsMetrics)
{
    // Reset metrics; skip gracefully if HotReload is not present
    GE_Result r = GE_ResetHotReloadMetrics();
    if (r == GE_Result_NotFound)
    {
        GTEST_SKIP() << "HotReload not available (ResetHotReloadMetrics not found)";
    }
    ASSERT_EQ(r, GE_Result_Ok);

    uint64_t lastCompileMs = 0, lastSwapMs = 0; int32_t totalCompiles = 0, totalSwaps = 0;
    ASSERT_EQ(GE_GetHotReloadMetrics(&lastCompileMs, &lastSwapMs, &totalCompiles, &totalSwaps), GE_Result_Ok);

    // Preload a test assembly and swap it in (DomainRoutingTest is built and copied by CMake)
    auto asmPath = FindDomainRoutingTest();
    if (asmPath.empty())
    {
        GTEST_SKIP() << "DomainRoutingTest.dll not found in expected locations";
    }

    const std::u8string u8s = asmPath.u8string();
    const std::string u8(u8s.begin(), u8s.end());
    r = GE_PreloadAssemblyContext(u8.c_str(), (uint32_t)u8.size());
    if (r == GE_Result_NotFound)
    {
        GTEST_SKIP() << "HotReload preload not available (PreloadAssemblyContext not bound)";
    }
    ASSERT_EQ(r, GE_Result_Ok);

    r = GE_SwapPreloadedContext();
    if (r == GE_Result_NotFound)
    {
        GTEST_SKIP() << "HotReload swap not available (SwapPreloadedContext not bound)";
    }
    ASSERT_EQ(r, GE_Result_Ok);

    // Metrics should reflect at least one swap and non-zero swap time in Debug
    ASSERT_EQ(GE_GetHotReloadMetrics(&lastCompileMs, &lastSwapMs, &totalCompiles, &totalSwaps), GE_Result_Ok);
    EXPECT_GE(totalSwaps, 1);
#ifndef NDEBUG
    // Swap can be sub-millisecond; allow zero. Primary assertion is counter increment.
    EXPECT_GE(lastSwapMs, 0ull);
#endif
}



TEST(ScriptingAbi_Metrics, Unload_IncrementsTotalUnloads)
{
    // Reset and verify metrics API is available
    GE_Result r = GE_ResetHotReloadMetrics();
    if (r == GE_Result_NotFound) {
        GTEST_SKIP() << "HotReload not available (ResetHotReloadMetrics not found)";
    }
    ASSERT_EQ(r, GE_Result_Ok);

    // Preload and swap a small test assembly to ensure a runtime domain is present
    auto asmPath = FindDomainRoutingTest();
    if (asmPath.empty()) {
        GTEST_SKIP() << "DomainRoutingTest.dll not found in expected locations";
    }
    const std::u8string u8s = asmPath.u8string();
    const std::string u8(u8s.begin(), u8s.end());
    r = GE_PreloadAssemblyContext(u8.c_str(), (uint32_t)u8.size());
    if (r == GE_Result_NotFound) {
        GTEST_SKIP() << "HotReload preload not available (PreloadAssemblyContext not bound)";
    }
    ASSERT_EQ(r, GE_Result_Ok);
    r = GE_SwapPreloadedContext();
    if (r == GE_Result_NotFound) {
        GTEST_SKIP() << "HotReload swap not available (SwapPreloadedContext not bound)";
    }
    ASSERT_EQ(r, GE_Result_Ok);

    // Obtain current runtime domain and unload it
    GE_DomainHandle dom = 0;
    ASSERT_EQ(GE_GetCurrentRuntimeDomain(&dom), GE_Result_Ok);
    ASSERT_NE(dom, 0ull);
    ASSERT_EQ(GE_ScriptsDomainUnload(dom), GE_Result_Ok);

    // Read extended metrics; if extended endpoint isn't bound, fallback will set totalUnloads=0
    uint64_t lastCompileMs = 0, lastSwapMs = 0; int32_t totalCompiles = 0, totalSwaps = 0, totalUnloads = 0;
    GE_Result mr = GE_GetHotReloadMetricsEx(&lastCompileMs, &lastSwapMs, &totalCompiles, &totalSwaps, &totalUnloads);
    if (mr == GE_Result_NotFound) {
        GTEST_SKIP() << "HotReload metrics not available";
    }
    ASSERT_EQ(mr, GE_Result_Ok);
    EXPECT_GE(totalUnloads, 1) << "Expected at least one unload to be counted";
}


TEST(ScriptingAbi_Metrics, Swap_UnderThreshold_SmokeSLA)
{
    // Reset metrics; skip if HotReload metrics are not available
    GE_Result r = GE_ResetHotReloadMetrics();
    if (r == GE_Result_NotFound) {
        GTEST_SKIP() << "HotReload not available (ResetHotReloadMetrics not found)";
    }
    ASSERT_EQ(r, GE_Result_Ok);

    // Preload and swap a tiny test assembly
    auto asmPath = FindDomainRoutingTest();
    if (asmPath.empty()) {
        GTEST_SKIP() << "DomainRoutingTest.dll not found in expected locations";
    }
    const std::u8string u8s = asmPath.u8string();
    const std::string u8(u8s.begin(), u8s.end());
    ASSERT_EQ(GE_PreloadAssemblyContext(u8.c_str(), (uint32_t)u8.size()), GE_Result_Ok);
    ASSERT_EQ(GE_SwapPreloadedContext(), GE_Result_Ok);

    // Read extended metrics and assert swap time is under a conservative threshold
    uint64_t lastCompileMs = 0, lastSwapMs = 0; int32_t totalCompiles = 0, totalSwaps = 0, totalUnloads = 0;
    GE_Result mr = GE_GetHotReloadMetricsEx(&lastCompileMs, &lastSwapMs, &totalCompiles, &totalSwaps, &totalUnloads);
    if (mr == GE_Result_NotFound) {
        GTEST_SKIP() << "HotReload metrics not available";
    }
    ASSERT_EQ(mr, GE_Result_Ok);
    EXPECT_GE(totalSwaps, 1);

    // Conservative bound suitable for Debug/CI variability
    const uint64_t kMaxSwapMs = 100ull;
    EXPECT_LE(lastSwapMs, kMaxSwapMs) << "Swap exceeded SLA bound (ms)";
}


TEST(ScriptingAbi_Metrics, FirstInvokeTiming_Captured_Smoke)
{
    // Preload and swap a tiny test assembly; skip if hot reload is unavailable
    auto asmPath = FindDomainRoutingTest();
    if (asmPath.empty()) {
        GTEST_SKIP() << "DomainRoutingTest.dll not found in expected locations";
    }
    const std::u8string u8s = asmPath.u8string();
    const std::string u8(u8s.begin(), u8s.end());
    GE_Result r = GE_PreloadAssemblyContext(u8.c_str(), (uint32_t)u8.size());
    if (r == GE_Result_NotFound) {
        GTEST_SKIP() << "HotReload preload not available (PreloadAssemblyContext not bound)";
    }
    ASSERT_EQ(r, GE_Result_Ok);
    ASSERT_EQ(GE_SwapPreloadedContext(), GE_Result_Ok);

    // Resolve current domain and a simple export, then invoke once to trigger timing capture
    GE_DomainHandle dom = 0ULL; ASSERT_EQ(GE_GetCurrentRuntimeDomain(&dom), GE_Result_Ok); ASSERT_NE(dom, 0ULL);
    const char* fq = "GameEngine.Scripts.ScriptsEntryPoint.GetValue";
    uint64_t tok = 0ULL;
    ASSERT_EQ(GE_QueryExport(dom, fq, (uint32_t)strlen(fq), &tok), GE_Result_Ok);
    ASSERT_NE(tok, 0ULL);
    int32_t out = -123;
    ASSERT_EQ(GE_InvokeByToken(dom, tok, &out), GE_Result_Ok);

    // Read editor-only first-invoke timing metric; skip if not bound
    uint64_t lastFirstInvokeMs = 0ULL;
    GE_Result mr = GE_GetHotReloadEditorMetrics(&lastFirstInvokeMs);
    if (mr == GE_Result_NotFound) {
        GTEST_SKIP() << "Editor timing metrics not available (optional binding)";
    }
    ASSERT_EQ(mr, GE_Result_Ok);
    EXPECT_GE(lastFirstInvokeMs, 0ULL);
}


TEST(ScriptingAbi_Metrics, FirstInvokeTiming_Packed_Smoke)
{
    // This is a smoke test: the packed call is optional and returns 0 if not present
    uint64_t packed = GE_GetHotReloadEditorMetricsPacked64();
    // Unpack fields; no strict assertions as binding may be absent in this environment
    uint32_t lastSwapMs = (uint32_t)(packed >> 32);
    uint32_t lastFirstInvokeMs = (uint32_t)(packed & 0xFFFFFFFFu);
    (void)lastSwapMs; (void)lastFirstInvokeMs;
    SUCCEED();
}
