#include "gtest/gtest.h"
#include "Scripting/ScriptingABI.h"

TEST(ScriptingAbi_Negative, QueryExport_InvalidArgs)
{
    uint64_t token = 0ull;
    // Empty name
    EXPECT_EQ(GE_QueryExport(0, "", 0u, &token), GE_Result_InvalidArg);
    // Null outToken
    EXPECT_EQ(GE_QueryExport(0, "Foo", 3u, nullptr), GE_Result_InvalidArg);
}

