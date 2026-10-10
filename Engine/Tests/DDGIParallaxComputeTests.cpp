// Run the production reflection parallax correction on authored depth moments.
// The reflection moments carry the true hit distance, so an agreeing surface
// is a trusted proxy however far it lies: a wall several probe cells away is
// exactly where a mirror's reflection must aim.
#include <gtest/gtest.h>

#include "DDGICaseKernel.h"
#include "TestDeviceHelper.h"

#include <array>
#include <string>
#include <vector>

namespace
{
using Vec4 = std::array<float, 4>;

// Mirrors ParallaxCase in Tests/Shaders/ddgi_parallax_test.comp.
struct ParallaxCase
{
    Vec4 Moments;  // xy = depth moments
    Vec4 ReceiverWS;
    Vec4 ProbeWS;
    Vec4 ReflectDir;
};

Vec4 AgreeingMoments(float distance)
{
    return {distance, distance * distance, 0, 0};
}

TEST(DDGIParallaxComputeTest, TrustsAnAgreeingSurfaceNearOrFar)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    DDGICaseKernel kernel;
    std::string error;
    ASSERT_TRUE(kernel.Load(*device, "ddgi_parallax_test", error)) << error;

    // The receiver sits half a metre off the probe; the reflection runs along +Z.
    const Vec4 receiver{0.5f, 0, 0, 0};
    const Vec4 probe{0, 0, 0, 0};
    const Vec4 reflect{0, 0, 1, 0};
    const auto results = kernel.Run(std::vector<ParallaxCase>{
        {AgreeingMoments(1.0f), receiver, probe, reflect},
        {AgreeingMoments(10.0f), receiver, probe, reflect}});
    ASSERT_EQ(results.size(), 2u);

    // A surface one metre out and one ten metres out: the eight probes aim at
    // the shared proxy hit, which lies off the raw reflection direction and
    // converges on it as the surface recedes.
    EXPECT_FLOAT_EQ(results[0][3], 1.0f);
    EXPECT_LT(results[0][2], 0.99f);
    EXPECT_FLOAT_EQ(results[1][3], 1.0f);
    EXPECT_LT(results[1][2], 1.0f);
    EXPECT_GT(results[1][2], results[0][2]);
}
}  // namespace
