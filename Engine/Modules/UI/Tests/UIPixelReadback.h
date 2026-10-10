#pragma once

// Pixel-level counterpart to UIRgTestHarness's declare-only DriveUiRender:
// executes one REAL UI render into a transient RGBA16F target under a given
// target-space declaration and converts the readback to RGBA8 bytes through
// the production converter (ReadbackToRgba8Srgb), stating the source space the
// declaration produced — an EncodedSrgb render holds encoded bytes at rest
// (SrgbAuthored; legal at READBACK from a float target, see UITextureSpace.h),
// a LinearSdr render holds display-linear and takes the OETF in the converter,
// exactly like the editor screenshot path.
//
// Shared by the Chrome-parity pixel pins (UIEncodedBlendParityTests,
// BorderBackgroundCompositeTests). Fixtures come from IsolatedUIFixture at
// contentScale 1 over its 800x600 logical viewport, so readback coordinates
// equal fixture physical px 1:1.

#include "IsolatedUIFixture.h"

#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "UI/UIManager.h"
#include "UI/UITargetSpace.h"
#include "UI/UITextureSpace.h"

#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine::UITesting
{

// Readback target dimensions — must equal IsolatedUIFixture's logical viewport
// at contentScale 1 so fixture physical px map 1:1 to readback pixels.
inline constexpr uint32_t kReadbackW = 800;
inline constexpr uint32_t kReadbackH = 600;

inline std::vector<uint8_t> RenderUiToBytes(UIManager& ui, UI::UITargetSpace targetSpace)
{
    using namespace GameEngine::Rendering;

    IDevice* dev = ui.GetDevice();
    if (!dev)
        return {};

    const size_t pixelCount = static_cast<size_t>(kReadbackW) * kReadbackH;
    const size_t outBytes = pixelCount * 8; // RGBA16F
    const BufferHandle readback = dev->CreateReadbackBuffer(outBytes, "UIPixelReadback.Readback");
    if (!readback.IsValid())
        return {};

    ViewReadbackResult result;
    {
        RenderGraph::RGResourcePool persistent(dev);
        RenderGraph::RGTransientPool transient(dev);
        RenderGraph::RGUploadRing ring(dev, 2, 262144);
        RenderGraph::RGFrame frame(dev, &persistent, &transient, &ring);
        frame.BeginFrame(1);

        TextureDesc td{};
        td.width = kReadbackW;
        td.height = kReadbackH;
        td.mipLevels = 1;
        td.arrayLayers = 1;
        td.sampleCount = 1;
        td.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                   static_cast<uint32_t>(TextureUsage::ShaderResource) |
                   static_cast<uint32_t>(TextureUsage::TransferSrc);
        td.debugName = "UIPixelReadback.Target";
        const RenderGraph::RGTexture target = frame.CreateTexture("UIPixelReadback.Target", td);
        if (!target.IsValid() || !ui.RenderRG(frame, target, targetSpace))
        {
            dev->DestroyBuffer(readback);
            return {};
        }

        frame.AddPass(
            "UIPixelReadback.Copy", PassPhase::kFinalize,
            [&](RenderGraph::RGPassBuilder& p)
            {
                p.Read(target, RenderGraph::RGTextureRead::CopySrc);
                p.PreventCulling(); // consumed by the CPU, not the graph
            },
            [target, readback](RenderGraph::RGContext& ctx)
            {
                ctx.Cmd->CopyTextureSubresourceToBuffer(ctx.GetTexture(target), 0, 0, readback,
                                                        kReadbackW, kReadbackH);
            });

        frame.Execute();
        dev->WaitForIdle();

        const void* mapped = dev->MapBuffer(readback);
        if (mapped)
        {
            result.width = kReadbackW;
            result.height = kReadbackH;
            result.format = TextureFormat::R16G16B16A16_FLOAT;
            result.pixels.assign(static_cast<const uint8_t*>(mapped),
                                 static_cast<const uint8_t*>(mapped) + outBytes);
            dev->UnmapBuffer(readback);
        }
    }
    dev->DestroyBuffer(readback);
    if (result.pixels.empty())
        return {};

    const UI::UITextureSpace sourceSpace =
        targetSpace == UI::UITargetSpace::EncodedSrgb() ? UI::UITextureSpace::SrgbAuthored()
                                                        : UI::UITextureSpace::DisplayLinearSdr();
    return ReadbackToRgba8Srgb(result, sourceSpace);
}

struct Rgb
{
    int R = -1;
    int G = -1;
    int B = -1;
};

inline Rgb PixelAt(const std::vector<uint8_t>& rgba, uint32_t x, uint32_t y)
{
    const size_t i = (static_cast<size_t>(y) * kReadbackW + x) * 4;
    if (i + 3 >= rgba.size())
        return {};
    return {rgba[i + 0], rgba[i + 1], rgba[i + 2]};
}

// 5x5 uniformity gate around an arbitrary sample point: a contaminated patch
// (overlap, AA edge, layout drift against a mirror fixture) fails loudly
// instead of shifting the mean — the #767 record's instrument discipline.
inline bool UniformPatch(const std::vector<uint8_t>& rgba, uint32_t cx, uint32_t cy, Rgb& out,
                         std::string& why)
{
    const Rgb centre = PixelAt(rgba, cx, cy);
    for (int dy = -2; dy <= 2; ++dy)
    {
        for (int dx = -2; dx <= 2; ++dx)
        {
            const Rgb p = PixelAt(rgba, cx + dx, cy + dy);
            if (p.R != centre.R || p.G != centre.G || p.B != centre.B)
            {
                why = "5x5 patch not uniform at (" + std::to_string(cx + dx) + "," +
                      std::to_string(cy + dy) + ")";
                return false;
            }
        }
    }
    out = centre;
    return true;
}

inline bool UniformCentre(const std::vector<uint8_t>& rgba, const PhysicalRect& box, Rgb& out,
                          std::string& why)
{
    return UniformPatch(rgba, static_cast<uint32_t>(box.X + box.W * 0.5f),
                        static_cast<uint32_t>(box.Y + box.H * 0.5f), out, why);
}

} // namespace GameEngine::UITesting
