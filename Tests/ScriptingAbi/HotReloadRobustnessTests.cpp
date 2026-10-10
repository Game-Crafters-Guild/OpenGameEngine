#include "gtest/gtest.h"
#include <chrono>
#include "Scripting/ScriptingABI.h"

TEST(HotReloadRobustness, ClearCompilerCacheAndGetStats)
{
    const void* table = nullptr; uint32_t size = 0;
    ASSERT_EQ(GE_GetInterface(GE_ABI_VERSION_CURRENT, &table, &size), GE_Result_Ok);
    ASSERT_NE(table, nullptr);
    auto* iface = reinterpret_cast<const GE_Interface_v1*>(table);

    // Try resolve managed callbacks; fall back to typed C ABI if not registered
    using ClearFn = int (*)();
    using StatsFn = int (*)();
    void* pClear = nullptr;
    void* pStats = nullptr;
    GE_Result rcClear = GE_Result_NotFound;
    GE_Result rcStats = GE_Result_NotFound;
    if (iface->GetManagedCallback)
    {
        rcClear = iface->GetManagedCallback(GE_CB_ClearCompilerCache, &pClear);
        rcStats = iface->GetManagedCallback(GE_CB_GetCompilerStats, &pStats);
    }
    bool haveCallbacks = (rcClear == GE_Result_Ok && rcStats == GE_Result_Ok && pClear && pStats);

    // Probe capability once and emit a single informational line if absent
    bool capabilityPresent = haveCallbacks;
    if (!capabilityPresent)
    {
        int probe = GE_GetCompilerStats();
        if (probe == 0)
            capabilityPresent = true;
        else if (probe == GE_Result_NotFound)
            std::cout << "[HotReload] capability not present (GetCompilerStats returned NotFound)\n";
    }

    // Call a few times; expect success via callbacks or typed API
    for (int i = 0; i < 3; ++i)
    {
        auto t0 = std::chrono::high_resolution_clock::now();
        int rc1 = 0, rc2 = 0;
        if (haveCallbacks)
        {
            rc1 = reinterpret_cast<ClearFn>(pClear)();
            rc2 = reinterpret_cast<StatsFn>(pStats)();
        }
        else
        {
            rc1 = GE_ClearCompilerCache();
            rc2 = GE_GetCompilerStats();
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        EXPECT_TRUE(rc1 == 0 || rc1 == GE_Result_NotFound);
        EXPECT_TRUE(rc2 == 0 || rc2 == GE_Result_NotFound);
        EXPECT_LT(ms, 2500);
    }
}

