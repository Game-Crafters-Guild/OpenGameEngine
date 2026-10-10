// The material trilinear sampler (SamplerPreset::LinearRepeat) filters
// anisotropically: ResolveSamplerPreset gives it the renderer profile's material
// anisotropy, one 16x request on every tier that RendererProfile clamps to the
// device limit. Every other preset stays isotropic.

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Core/RendererProfile.h"

#include <array>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <utility>

using namespace GameEngine::Rendering;

namespace
{
RenderingDeviceCapabilities FullProfileDevice(float maxSamplerAnisotropy)
{
    RenderingDeviceCapabilities caps{};
    caps.supportsBindlessResources = true;
    caps.supportsBufferDeviceAddress = true;
    caps.maxSamplerAnisotropy = maxSamplerAnisotropy;
    return caps;
}

// No bindless indexing selects the compatibility profile: what the WebGPU
// backend reports, and what GE_FORCE_COMPAT leaves on a desktop device.
RenderingDeviceCapabilities CompatibilityProfileDevice(float maxSamplerAnisotropy)
{
    RenderingDeviceCapabilities caps{};
    caps.supportsBindlessResources = false;
    caps.maxSamplerAnisotropy = maxSamplerAnisotropy;
    return caps;
}

SamplerDesc ResolveOn(SamplerPreset preset, const RenderingDeviceCapabilities& caps)
{
    return ResolveSamplerPreset(preset, RendererProfile::FromCapabilities(caps));
}
} // namespace

TEST(SamplerPresetResolution, LinearRepeatIsSixteenTimesAnisotropicOnTheFullProfile)
{
    const RendererProfile profile = RendererProfile::FromCapabilities(FullProfileDevice(16.0f));
    ASSERT_FALSE(profile.IsCompat());

    const SamplerDesc desc = ResolveSamplerPreset(SamplerPreset::LinearRepeat, profile);
    EXPECT_EQ(desc.maxAnisotropy, 16.0f);
    // Still the trilinear repeat sampler: anisotropy widens the footprint and
    // changes nothing else.
    EXPECT_EQ(desc.minFilter, 1u);
    EXPECT_EQ(desc.magFilter, 1u);
    EXPECT_EQ(desc.mipFilter, 1u);
    EXPECT_EQ(desc.addressModeU, 0u);
    EXPECT_EQ(desc.addressModeV, 0u);
    EXPECT_EQ(desc.mipLodBias, 0.0f);
}

TEST(SamplerPresetResolution, LinearRepeatIsSixteenTimesAnisotropicOnTheCompatibilityProfile)
{
    const RendererProfile profile = RendererProfile::FromCapabilities(CompatibilityProfileDevice(16.0f));
    ASSERT_TRUE(profile.IsCompat());

    EXPECT_EQ(ResolveSamplerPreset(SamplerPreset::LinearRepeat, profile).maxAnisotropy, 16.0f);
}

TEST(SamplerPresetResolution, LinearRepeatIsClampedToTheDeviceLimit)
{
    EXPECT_EQ(ResolveOn(SamplerPreset::LinearRepeat, FullProfileDevice(4.0f)).maxAnisotropy, 4.0f);
    EXPECT_EQ(ResolveOn(SamplerPreset::LinearRepeat, CompatibilityProfileDevice(8.0f)).maxAnisotropy, 8.0f);
    // Without the anisotropy feature the device reports 1, and the sampler must
    // not enable anisotropy at all.
    EXPECT_EQ(ResolveOn(SamplerPreset::LinearRepeat, FullProfileDevice(1.0f)).maxAnisotropy, 1.0f);
    // A limit above the request does not raise it on either tier.
    EXPECT_EQ(ResolveOn(SamplerPreset::LinearRepeat, FullProfileDevice(32.0f)).maxAnisotropy, 16.0f);
    EXPECT_EQ(ResolveOn(SamplerPreset::LinearRepeat, CompatibilityProfileDevice(32.0f)).maxAnisotropy, 16.0f);
}

// The clamp-to-edge colour sampler (a Planar terrain basemap) filters like the material sampler:
// isotropic, it reads a basemap up to four mips softer at a grazing angle than the materials
// beside it.
TEST(SamplerPresetResolution, LinearClampAnisotropicIsTheMaterialSamplerClampedToEdge)
{
    for (const RenderingDeviceCapabilities& caps : {FullProfileDevice(16.0f), CompatibilityProfileDevice(16.0f)})
    {
        const SamplerDesc desc = ResolveOn(SamplerPreset::LinearClampAnisotropic, caps);
        EXPECT_EQ(desc.maxAnisotropy, 16.0f) << "bindless " << caps.supportsBindlessResources;
        EXPECT_EQ(desc.minFilter, 1u);
        EXPECT_EQ(desc.magFilter, 1u);
        EXPECT_EQ(desc.mipFilter, 1u);
        EXPECT_EQ(desc.addressModeU, 2u) << "not clamp-to-edge";
        EXPECT_EQ(desc.addressModeV, 2u) << "not clamp-to-edge";
    }
    EXPECT_EQ(ResolveOn(SamplerPreset::LinearClampAnisotropic, FullProfileDevice(4.0f)).maxAnisotropy, 4.0f);
    EXPECT_EQ(ResolveOn(SamplerPreset::LinearClampAnisotropic, FullProfileDevice(1.0f)).maxAnisotropy, 1.0f);
}

// The bindless sampler array is filled in SamplerPreset enum order, so each GE_TS_* index the
// shaders select a sampler by must equal its preset's value. Read from the shipped include: a
// drifted literal samples through a different preset (4 is PointRepeat) and still compiles.
TEST(SamplerPresetResolution, TheShaderSamplerIndicesAreTheEnumValues)
{
    std::ifstream in(GE_BINDLESS_TEXTURES_GLSL);
    ASSERT_TRUE(in.is_open()) << "cannot read " << GE_BINDLESS_TEXTURES_GLSL;
    const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    const std::pair<const char*, SamplerPreset> kIndices[] = {
        {"GE_TS_REPEAT", SamplerPreset::LinearRepeat},
        {"GE_TS_CLAMP", SamplerPreset::LinearClamp},
        {"GE_TS_CLAMP_ANISO", SamplerPreset::LinearClampAnisotropic},
    };
    for (const auto& [name, preset] : kIndices)
    {
        std::smatch m;
        ASSERT_TRUE(std::regex_search(
            src, m, std::regex(std::string(R"(#define\s+)") + name + R"(\s+([0-9]+)u\b)")))
            << name << " is not defined in bindless_textures.glsl";
        EXPECT_EQ(std::stoi(m[1].str()), static_cast<int>(preset))
            << name << " does not select the sampler of its preset";
    }
}

TEST(SamplerPresetResolution, EveryOtherPresetStaysIsotropic)
{
    constexpr std::array kIsotropicPresets = {SamplerPreset::LinearClamp, SamplerPreset::BilinearRepeat,
                                              SamplerPreset::PointClamp, SamplerPreset::PointRepeat};
    for (const RenderingDeviceCapabilities& caps : {FullProfileDevice(16.0f), CompatibilityProfileDevice(16.0f)})
    {
        for (const SamplerPreset preset : kIsotropicPresets)
        {
            EXPECT_EQ(ResolveOn(preset, caps).maxAnisotropy, 1.0f)
                << "preset " << static_cast<int>(preset) << ", bindless " << caps.supportsBindlessResources;
        }
    }
}
