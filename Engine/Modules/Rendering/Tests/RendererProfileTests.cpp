// The feature policy RendererProfile derives from a device's capabilities, for the flags that
// select shader code: what a device without the capability runs instead is decided here.

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Core/RendererProfile.h"

#include <cstdlib>
#include <string>

using namespace GameEngine::Rendering;

namespace
{
RenderingDeviceCapabilities DesktopCapabilities()
{
    RenderingDeviceCapabilities caps{};
    caps.supportsBindlessResources = true;
    caps.supportsBufferDeviceAddress = true;
    caps.supportsSampleRateShading = true;
    return caps;
}

// Sets GE_FORCE_COMPAT for a scope and restores what the process had.
struct ScopedForceCompat
{
    ScopedForceCompat()
    {
        const char* previous = std::getenv("GE_FORCE_COMPAT");
        Had = previous != nullptr;
        Previous = Had ? previous : "";
        Set("1");
    }
    ~ScopedForceCompat() { Set(Had ? Previous.c_str() : ""); }
    static void Set(const char* value)
    {
#if defined(_WIN32)
        _putenv_s("GE_FORCE_COMPAT", value);
#else
        if (value[0] == '\0')
            unsetenv("GE_FORCE_COMPAT");
        else
            setenv("GE_FORCE_COMPAT", value, 1);
#endif
    }
    bool Had = false;
    std::string Previous;
};
} // namespace

TEST(RendererProfile, InterpolationFunctionsFollowSampleRateShading)
{
    RenderingDeviceCapabilities caps = DesktopCapabilities();
    EXPECT_TRUE(RendererProfile::FromCapabilities(caps).UseInterpolationFunctions);
    caps.supportsSampleRateShading = false;
    EXPECT_FALSE(RendererProfile::FromCapabilities(caps).UseInterpolationFunctions)
        << "a device without sampleRateShading must not be handed interpolateAtOffset";
}

TEST(RendererProfile, ForcedCompatibilityTakesNoInterpolationFunctions)
{
    RenderingDeviceCapabilities caps = DesktopCapabilities();
    ScopedForceCompat forced;
    ASSERT_TRUE(ApplyForceCompatOverride(caps));
    EXPECT_FALSE(caps.supportsSampleRateShading);
    EXPECT_FALSE(RendererProfile::FromCapabilities(caps).UseInterpolationFunctions);
}
