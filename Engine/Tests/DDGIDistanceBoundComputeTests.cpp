// Run the production visibility-distance bound on authored grids. Distance
// moments describe visibility between neighbouring probes and a receiver, so
// the bound must follow probe spacing, not the volume's extent: a volume-sized
// miss mixed into a near-wall direction would make that wall vanish from the
// visibility test.
#include <gtest/gtest.h>

#include "DDGICaseKernel.h"
#include "TestDeviceHelper.h"

#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace
{
// Mirrors DistanceCase in Tests/Shaders/ddgi_distance_bound_test.comp.
struct DistanceCase
{
    std::array<float, 4> GridSizeWS;
    std::array<int32_t, 4> ProbeCount;
    std::array<float, 4> RayDistances;
};

TEST(DDGIDistanceBoundComputeTest, DistanceMomentsStayLocalAsTheVolumeGrows)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    DDGICaseKernel kernel;
    std::string error;
    ASSERT_TRUE(kernel.Load(*device, "ddgi_distance_bound_test", error)) << error;

    // Both volumes have two-metre probe spacing; the second is a hundred times larger.
    constexpr float kNearHit = 1.25f;
    constexpr float kFarHit = 10000.0f;
    constexpr float kMiss = -1.0f;
    std::vector<DistanceCase> cases;
    for (const auto& [extent, probes] : {std::pair{10.0f, 6}, std::pair{1000.0f, 501}})
        cases.push_back({{extent, extent, extent, 0}, {probes, probes, probes, 0}, {kNearHit, kFarHit, kMiss, 0}});
    const auto results = kernel.Run(cases);
    ASSERT_EQ(results.size(), cases.size());
    for (const auto& result : results)
    {
        EXPECT_GT(result[0], std::sqrt(12.0f)); // all neighbouring corners, with relocation clearance
        EXPECT_LT(result[0], 6.0f);            // local visibility, independent of volume extent
        EXPECT_FLOAT_EQ(result[1], kNearHit);  // near occluders retain their real distance
        EXPECT_FLOAT_EQ(result[2], result[0]); // distant geometry and sky have the same cap
        EXPECT_FLOAT_EQ(result[3], result[0]);
    }
}
}  // namespace
