#include "UI/StyleOverrides.h"

#include <gtest/gtest.h>

namespace GameEngine {

// The CSS parser reads 8-digit hex as #RRGGBBAA, so the ARGB alpha byte must
// move to the tail when SetCustomColor formats the stored string. Storing
// #AARRGGBB renders an opaque dark gray as translucent red.
TEST(StyleOverrideCustomColorTests, ArgbFormatsAsRrggbbaa)
{
    StyleOverrides overrides;
    const StringId name = HashStringId("--test-color");

    overrides.SetCustomColor(name, 0xFF202020u);
    ASSERT_NE(overrides.FindCustom(name), nullptr);
    EXPECT_EQ(*overrides.FindCustom(name), "#202020FF");

    overrides.SetCustomColor(name, 0x80FF0000u);
    EXPECT_EQ(*overrides.FindCustom(name), "#FF000080");

    overrides.SetCustomColor(name, 0x00000000u);
    EXPECT_EQ(*overrides.FindCustom(name), "#00000000");
}

} // namespace GameEngine
