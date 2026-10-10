#pragma once
// Drives a UIManager render through the immediate-mode RenderGraph for the legacy UI
// behavior tests (replaces the deleted retained-mode RenderGraph + UIManager::Render
// (RenderGraph&) arm). These tests assert UI state — layout, hover, scroll
// virtualization, draw order, invalidation — never pixels. RenderRG performs the
// per-frame primitive generation synchronously (PrepareSdfFrameData) and only DECLARES
// the draw pass, so the harness never calls Execute(): the target is a declared,
// never-realized attachment and its dimensions are immaterial to the assertions. Pools
// are owned once and reused; each call declares a fresh frame (immediate-mode rebuild).
#include <cstdint>
#include <cstdlib>
#include <memory>

#include "SharedHeadlessDevice.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "UI/Controls/TextField.h"
#include "UI/UIManager.h"

// A headless GPU device of the caller's own: no swapchain, and GE_HEADLESS_TEST=1 so
// device init skips presentation paths. Returns null when no Vulkan device
// is available — callers GTEST_SKIP on that. For a test that asserts on the
// device itself; every other test takes SharedHeadlessDevice().
inline std::unique_ptr<GameEngine::Rendering::IDevice> MakeHeadlessDevice()
{
#ifdef _WIN32
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif
    GameEngine::Rendering::DeviceDesc dd{};
    dd.preferredAPI = GameEngine::Rendering::GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    dd.enableSwapchain = false; // headless
    dd.enableDebugLayer = false;
    auto dev = GameEngine::Rendering::DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd))
        return nullptr;
    return dev;
}

// Find the first TextInput in a subtree. Typed fields embed their editor as
// a child element; tests reach it through this walk instead of the protected
// TextFieldBase accessor.
inline GameEngine::TextInput* FindFirstTextInput(GameEngine::UIElement* root)
{
    if (!root)
        return nullptr;
    if (auto* ti = dynamic_cast<GameEngine::TextInput*>(root))
        return ti;
    for (const auto& c : root->GetChildren())
    {
        if (auto* ti = FindFirstTextInput(c.get()))
            return ti;
    }
    return nullptr;
}

struct UiRgHarness
{
    GameEngine::Rendering::IDevice* Dev = nullptr;
    GameEngine::Rendering::RenderGraph::RGResourcePool Persistent;
    GameEngine::Rendering::RenderGraph::RGTransientPool Transient;
    GameEngine::Rendering::RenderGraph::RGUploadRing Ring;
    uint64_t FrameIndex = 0;

    explicit UiRgHarness(GameEngine::Rendering::IDevice* d)
        : Dev(d), Persistent(d), Transient(d), Ring(d, 2, 262144)
    {
    }
};

inline void DriveUiRender(GameEngine::UIManager& ui, UiRgHarness& h)
{
    using namespace GameEngine::Rendering;
    RenderGraph::RGFrame frame(h.Dev, &h.Persistent, &h.Transient, &h.Ring);
    frame.BeginFrame(++h.FrameIndex);

    TextureDesc td{};
    td.width = 256;
    td.height = 256;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.sampleCount = 1;
    td.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
               static_cast<uint32_t>(TextureUsage::ShaderResource);
    td.debugName = "UITest.Target";

    // FLIPPED (#767 slice i): the offscreen UI test harness is the first host
    // to declare the encoded-blend target. Every behaviour test therefore
    // exercises the EncodedSrgb declaration path — outputEncoding, the
    // subpixel gate, the coverage-constant resolve and encoded-pipeline
    // selection — at declare time. Pixel semantics of the flip are pinned by
    // UIEncodedBlendParityTests, which executes and reads back both arms.
    const RenderGraph::RGTexture target = frame.CreateTexture("UITest.Target", td);
    ui.RenderRG(frame, target, GameEngine::UI::UITargetSpace::EncodedSrgb());
}
