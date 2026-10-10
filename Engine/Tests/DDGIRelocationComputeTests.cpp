// Run the production probe relocation policy. Movement inside the same free
// space keeps a probe's history; a change of activity or a move through a
// surface puts the probe in a different lighting domain and must reset it.
#include <gtest/gtest.h>

#include "DDGICaseKernel.h"
#include "TestDeviceHelper.h"

#include <array>
#include <string>
#include <vector>

namespace
{
// Mirrors RelocationCase in Tests/Shaders/ddgi_relocation_test.comp.
struct RelocationCase
{
    std::array<float, 4> PreviousOffset;
    std::array<float, 4> ProposedOffset;
    std::array<float, 4> Cell;
    std::array<uint32_t, 4> Flags;  // was active, buried, crossed geometry
};

constexpr float kCell = 1.0f;

RelocationCase Case(float previousX, float proposedX, float proposedY, bool wasActive, bool buried, bool crossed)
{
    return {{previousX, 0, 0, 0}, {proposedX, proposedY, 0, 0}, {kCell, 0, 0, 0},
            {wasActive ? 1u : 0u, buried ? 1u : 0u, crossed ? 1u : 0u, 0u}};
}

TEST(DDGIRelocationComputeTest, RelocationKeepsSameRoomHistoryButResetsAcrossGeometry)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    DDGICaseKernel kernel;
    std::string error;
    ASSERT_TRUE(kernel.Load(*device, "ddgi_relocation_test", error)) << error;

    const auto results = kernel.Run(std::vector<RelocationCase>{
        // A sub-threshold nudge keeps the previous offset and the history.
        Case(0.45f, 0.448f, 0.04f, true, false, false),
        // Larger same-room movement is allowed without reseeding history.
        Case(0.45f, 0.3f, 0.04f, true, false, false),
        // A move from free space through a wall is rejected, and resets.
        Case(0.45f, 0.3f, 0.04f, true, false, true),
        // A still-buried probe may escape through geometry; it contributes no
        // lighting yet, so the crossing does not reset.
        Case(0.45f, 0.3f, 0.04f, false, true, true),
        // Activation and deactivation both discard the previous domain.
        Case(0.45f, 0.3f, 0.04f, false, false, false),
        Case(0.45f, 0.3f, 0.04f, true, true, false)});
    ASSERT_EQ(results.size(), 6u);
    EXPECT_FLOAT_EQ(results[0][0], 0.45f);
    EXPECT_FLOAT_EQ(results[0][1], 0.0f);
    EXPECT_FLOAT_EQ(results[0][3], 0.0f);
    EXPECT_FLOAT_EQ(results[1][0], 0.3f);
    EXPECT_FLOAT_EQ(results[1][3], 0.0f);
    EXPECT_FLOAT_EQ(results[2][0], 0.45f);
    EXPECT_FLOAT_EQ(results[2][3], 1.0f);
    EXPECT_FLOAT_EQ(results[3][0], 0.3f);
    EXPECT_FLOAT_EQ(results[3][3], 0.0f);
    EXPECT_FLOAT_EQ(results[4][3], 1.0f);
    EXPECT_FLOAT_EQ(results[5][3], 1.0f);
}
}  // namespace
