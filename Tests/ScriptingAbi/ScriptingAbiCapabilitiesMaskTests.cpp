#include "gtest/gtest.h"
#include "Scripting/ScriptingABI.h"

TEST(ScriptingAbi_CapabilitiesMask, ReportsDomainSwapBit)
{
    uint32_t mask = 0xFFFFFFFFu;
    GE_Result rc = GE_Capabilities(&mask);
    ASSERT_EQ(rc, GE_Result_Ok);

    // Domain swap capability must be reported during the transition period
    EXPECT_NE(mask & GE_Cap_HasDomainSwap, 0u);

    // Mask may include additional bits in the future; test is forward-compatible
}

