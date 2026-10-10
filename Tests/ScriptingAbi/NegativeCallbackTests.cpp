#include "gtest/gtest.h"
#include "Scripting/ScriptingABI.h"
#include <cstdint>

TEST(ScriptingAbi_NegativeCallbacks, UnknownIdReturnsNotFound)
{
    const void* table = nullptr; uint32_t size = 0;
    ASSERT_EQ(GE_GetInterface(GE_ABI_VERSION_CURRENT, &table, &size), GE_Result_Ok);
    ASSERT_NE(table, nullptr);
    auto* iface = reinterpret_cast<const GE_Interface_v1*>(table);

    void* fn = reinterpret_cast<void*>((uintptr_t)0xDEADBEEF);
    GE_Result rc = GE_Result_Fail;
    if (iface->GetManagedCallback)
        rc = iface->GetManagedCallback(0xFFFFu, &fn); // pick an ID far outside core range
    else
        rc = GE_GetManagedCallback(0xFFFFu, &fn);
    EXPECT_EQ(rc, GE_Result_NotFound);
}

