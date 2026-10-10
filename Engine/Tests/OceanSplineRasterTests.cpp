#include <gtest/gtest.h>

#include "Ocean/OceanSplineRaster.h"
#include "Ocean/OceanTypes.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "TestDeviceHelper.h"

#include <cmath>
#include <cstring>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Ocean;

namespace
{
constexpr uint32 kResolution = 64u;
constexpr float kTexelMeters = 1.0f;
// The lower edge sits 0.05 m above a texel center, where a field that steps at
// the edge puts its reconstructed boundary about half a texel away from it.
constexpr float kRibbonCenterZ = 31.75f;
constexpr float kRibbonHalfWidth = 3.3f;
// Storage precision over a two-texel ramp resolves the boundary to about 0.01 m.
constexpr float kContourTolerance = 0.05f;
constexpr uint32 kDepthField = 0u;
constexpr uint32 kClipField = 2u;
constexpr float kDeepWaterMeters = 60000.0f;
constexpr float kSaturationDepthMeters = 10.0f;

float HalfToFloat(uint16 half)
{
    const uint32 sign = (half >> 15) & 1u, exponent = (half >> 10) & 0x1fu, mantissa = half & 0x3ffu;
    const float magnitude = exponent == 0u ? std::ldexp(static_cast<float>(mantissa), -24)
                                           : std::ldexp(static_cast<float>(mantissa | 0x400u),
                                                        static_cast<int>(exponent) - 25);
    return sign ? -magnitude : magnitude;
}

// Draws one straight ribbon along X into a single-layer cascade of `format`
// cleared to `clearValue`, one meter per texel, and reads the layer back.
std::vector<float> RasterizeRibbon(IDevice& device, uint32 field, TextureFormat format, float clearValue,
                                   const OceanRibbonStyle& style)
{
    const bool halfFloat = format == TextureFormat::R16_FLOAT;
    const size_t texelBytes = halfFloat ? 2u : 1u;
    TextureDesc desc{};
    desc.width = kResolution;
    desc.height = kResolution;
    desc.arrayLayers = 1;
    desc.format = static_cast<uint32>(format);
    desc.usage = static_cast<uint32>(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource |
                                     TextureUsage::TransferSrc | TextureUsage::TransferDst);
    desc.flags = TextureCreateFlags::ForceArrayView;
    desc.persistent = true;
    desc.debugName = "OceanSplineRasterTest.Field";
    const TextureHandle texture = device.CreateTexture(desc);
    EXPECT_TRUE(texture.IsValid());
    if (!texture.IsValid())
        return {};
    const float clear[4] = {clearValue, 0, 0, 0};
    auto commands = device.CreateCommandList(IDevice::QueueType::Graphics);
    commands->Begin();
    commands->ClearColorImageSubresource(texture, 0, 0, clear);
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopyDest,
                                                            ResourceState::ShaderResource));
    commands->End();
    device.ExecuteCommandLists({commands.get()});
    device.WaitForIdle();

    OceanSplineRaster raster;
    raster.SetTriangles(BuildOceanRibbon({{2, 0, kRibbonCenterZ, 1, 0}, {62, 0, kRibbonCenterZ, 1, 0}}, style));
    OceanCascadeLayoutGPU layout{};
    layout.CascadeOriginScale[0][2] = kTexelMeters;
    layout.CascadeOriginScale[0][3] = 1.0f;
    layout.LodCount = 1;
    RenderGraph::RGResourcePool persistent(&device);
    RenderGraph::RGTransientPool transient(&device);
    RenderGraph::RGUploadRing ring(&device, 2, 262144);
    {
        RenderGraph::RGFrame frame(&device, &persistent, &transient, &ring);
        frame.BeginFrame(0);
        const auto target = frame.ImportExternalTexture("OceanSplineRasterTest.Field", texture,
                                                        ResourceState::ShaderResource, format, 1, 1);
        EXPECT_TRUE(raster.Declare(frame, &device, field, target, layout, kResolution));
        frame.Execute();
        device.FinalizeFrame();
        device.WaitForIdle();
    }

    const BufferHandle readback = device.CreateReadbackBuffer(kResolution * kResolution * texelBytes);
    commands = device.CreateCommandList(IDevice::QueueType::Graphics);
    commands->Begin();
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::ShaderResource,
                                                            ResourceState::CopySource));
    commands->CopyTextureToBuffer(texture, readback, kResolution, kResolution);
    commands->End();
    device.ExecuteCommandLists({commands.get()});
    device.WaitForIdle();
    std::vector<float> values(kResolution * kResolution);
    if (const auto* mapped = static_cast<const uint8*>(device.MapBuffer(readback)))
    {
        for (size_t i = 0; i < values.size(); ++i)
        {
            if (halfFloat)
            {
                uint16 half = 0;
                std::memcpy(&half, mapped + i * 2u, 2u);
                values[i] = HalfToFloat(half);
            }
            else
                values[i] = mapped[i] / 255.0f;
        }
        device.UnmapBuffer(readback);
    }
    device.DestroyBuffer(readback);
    device.DestroyTexture(texture);
    return values;
}

// Where the bilinear reconstruction of one texel column first crosses
// `threshold`, walking from texel row `fromZ` toward `toZ`, in meters.
float Crossing(const std::vector<float>& values, uint32 column, int fromZ, int toZ, float threshold)
{
    const int step = toZ > fromZ ? 1 : -1;
    for (int z = fromZ; z != toZ; z += step)
    {
        const float a = values[static_cast<size_t>(z) * kResolution + column];
        const float b = values[static_cast<size_t>(z + step) * kResolution + column];
        if ((a < threshold) != (b < threshold))
            return (static_cast<float>(z) + 0.5f + static_cast<float>(step) * (threshold - a) / (b - a)) *
                   kTexelMeters;
    }
    return -1.0f;
}

void ExpectBoundariesOnTheTrueEdges(const std::vector<float>& values, float threshold)
{
    ASSERT_EQ(values.size(), static_cast<size_t>(kResolution * kResolution));
    const int center = static_cast<int>(kRibbonCenterZ / kTexelMeters);
    for (uint32 column = 16; column < 48; column += 8)
    {
        EXPECT_NEAR(Crossing(values, column, 0, center, threshold), kRibbonCenterZ - kRibbonHalfWidth,
                    kContourTolerance)
            << "lower edge, column " << column;
        EXPECT_NEAR(Crossing(values, column, static_cast<int>(kResolution) - 1, center, threshold),
                    kRibbonCenterZ + kRibbonHalfWidth, kContourTolerance)
            << "upper edge, column " << column;
    }
}
} // namespace

// The surface discards where the bilinearly sampled clip value passes one half,
// so that contour is the visible water edge. It must follow the ribbon's true
// edge, not the texel grid, or the bank reads as a staircase.
TEST(OceanSplineRasterTest, ClipHalfCoverageFollowsTheTrueEdgeBetweenTexels)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    OceanRibbonStyle style{};
    style.Width = kRibbonHalfWidth;
    style.Feather = 0.1f * kRibbonHalfWidth;
    style.Flags = 4u;
    ExpectBoundariesOnTheTrueEdges(RasterizeRibbon(*device, kClipField, TextureFormat::R8_UNORM, 0.0f, style), 0.5f);
    device->Shutdown();
}

// The shallow tint, shoreline foam and wave attenuation all switch on where the
// sampled depth drops below the saturation depth. That boundary must follow the
// ribbon's true edge too, which needs the field linear across it rather than
// stepping to the deep-water sentinel at the edge.
TEST(OceanSplineRasterTest, DepthBandSaturationContourFollowsTheTrueEdgeBetweenTexels)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    OceanRibbonStyle style{};
    style.Width = kRibbonHalfWidth;
    style.Depth = 2.0f;
    style.DepthSaturation = kSaturationDepthMeters;
    style.Flags = 1u;
    ExpectBoundariesOnTheTrueEdges(
        RasterizeRibbon(*device, kDepthField, TextureFormat::R16_FLOAT, kDeepWaterMeters, style),
        kSaturationDepthMeters);
    device->Shutdown();
}
