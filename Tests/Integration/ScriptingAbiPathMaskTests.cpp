#include "gtest/gtest.h"
#include "Scripting/ScriptingABI.h"

// The fast path is the typed delegates over HotReloadManager; a wrapper standing in for one
// that failed to bind (a signature drift CreateDelegate rejects) would pass every call and
// only show here.
TEST(ScriptingAbi_Paths, TypedDelegates_Bound_OnTheFastPath)
{
    int mask = GE_DebugGetPathMask();
    if (mask < 0)
    {
        GTEST_SKIP() << "Path mask not available (Debug_GetPathMask not bound)";
    }

    EXPECT_NE(mask & 0x01, 0) << "Typed Query delegate not bound";
    EXPECT_NE(mask & 0x02, 0) << "Typed Invoke delegate not bound";
    EXPECT_EQ(mask & 0x04, 0) << "Wrapper Query unexpectedly bound";
    EXPECT_EQ(mask & 0x08, 0) << "Wrapper Invoke unexpectedly bound";
}
