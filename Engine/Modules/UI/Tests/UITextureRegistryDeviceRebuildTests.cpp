// Device-rebuild pins for the UI texture registry.
//
// An in-place device rebuild destroys every VkObject while keeping the IDevice*
// alive, and a handle minted before the rebuild keeps reporting IsValid()
// (nothing re-stamps the id bits — see
// PerDeviceCacheCleanup.SamplerHandleFromBeforeTheRebuildStillReportsValidButIsDead
// in the Rendering suite). A registry that created its dummy texture and its two
// samplers once and dropped them only at Shutdown therefore kept writing dead
// handles into every UI descriptor set for the rest of the session.
//
// Every pin asserts its population is non-empty before asserting on the
// registry's behaviour: a registry holding nothing writes nothing and drops
// nothing, which would otherwise be indistinguishable from a fix.

#include "UI/UITextureRegistry.h"

#include "Core/Application.h"
#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Text/FontAtlas.h"

#include "ScopedEnvVar.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using GameEngine::Rendering::Tests::ScopedEnvVar;

namespace
{

// Headless device: no swapchain, no presentation paths.
std::unique_ptr<IDevice> MakeHeadlessDevice()
{
#ifdef _WIN32
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    dd.enableSwapchain = false;
    dd.enableDebugLayer = false;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd))
        return nullptr;
    return dev;
}

// Mirrors the render loop: TickDeviceRecovery drives the rebuild retry and must
// run unconditionally, before BeginFrame.
void RunOneFrame(IDevice& dev)
{
    dev.TickDeviceRecovery();
    if (!dev.BeginFrame())
        return;
    auto cl = dev.CreateCommandList(IDevice::QueueType::Graphics);
    if (cl)
    {
        cl->Begin();
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        dev.ExecuteCommandLists(lists);
    }
    dev.Present();
}

// Drives frames until the injected loss has rebuilt the device, then returns it
// to Healthy. Returns false when the rebuild never happened.
bool DriveRebuild(IDevice& dev)
{
    constexpr int kMaxFrames = 12;
    bool reachedAwaiting = false;
    for (int i = 0; i < kMaxFrames && !reachedAwaiting; ++i)
    {
        RunOneFrame(dev);
        reachedAwaiting = (dev.GetDeviceHealth() == DeviceHealth::AwaitingReprovision);
    }
    if (!reachedAwaiting)
        return false;
    dev.NotifyReprovisionComplete();
    return true;
}

TextureHandle CreatePixelTexture(IDevice& dev, const char* debugName)
{
    TextureDesc td{};
    td.width = 1;
    td.height = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource)
             | static_cast<uint32_t>(TextureUsage::TransferDst);
    td.debugName = debugName;
    return dev.CreateTexture(td);
}

constexpr uint32_t kRegisteredTextureCount = 3;

} // namespace

// The slot space is registry state keyed to one device generation. After a
// rebuild the registry must start allocating from the bottom again, because
// every slot it held named a destroyed texture. Continuing the pre-rebuild
// counter is the visible signature of a registry that never noticed the rebuild.
TEST(UITextureRegistryDeviceRebuild, SlotSpaceResetsAfterRebuild)
{
    ScopedEnvVar forcedLoss("GE_VK_FORCE_DEVICE_LOST", "2");
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "no Vulkan device available";

    UI::UITextureRegistry registry(dev.get());

    std::vector<uint32_t> slotsBefore;
    for (uint32_t i = 0; i < kRegisteredTextureCount; ++i)
    {
        const TextureHandle tex = CreatePixelTexture(*dev, "UIDevRebuild_Before");
        ASSERT_TRUE(tex.IsValid());
        const uint32_t slot = registry.Register(tex);
        ASSERT_NE(slot, 0u) << "registration failed, so this pin would prove nothing";
        slotsBefore.push_back(slot);
    }
    // Population guard: the assertions below are only meaningful because the
    // registry actually held slots when the device died.
    ASSERT_EQ(slotsBefore.size(), kRegisteredTextureCount);
    const uint32_t firstSlot = slotsBefore.front();

    if (!DriveRebuild(*dev))
        GTEST_SKIP() << "injected loss did not produce a rebuild on this adapter";
    ASSERT_GT(dev->GetDeviceRebuildGeneration(), 0u);

    const TextureHandle afterTex = CreatePixelTexture(*dev, "UIDevRebuild_After");
    ASSERT_TRUE(afterTex.IsValid());
    const uint32_t slotAfter = registry.Register(afterTex);

    ASSERT_NE(slotAfter, 0u);
    EXPECT_EQ(slotAfter, firstSlot)
        << "the registry kept its pre-rebuild slot space: it is still holding "
        << kRegisteredTextureCount << " slots that name destroyed textures";
}

// The end-to-end pin: materialising a UI descriptor set after a rebuild must not
// drop a single write. Both descriptor backends log this case at Error naming
// the set/binding, so a dropped write is directly observable.
TEST(UITextureRegistryDeviceRebuild, NoDroppedDescriptorWritesAfterRebuild)
{
    ScopedEnvVar forcedLoss("GE_VK_FORCE_DEVICE_LOST", "2");
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "no Vulkan device available";

    UI::UITextureRegistry registry(dev.get());

    uint32_t registeredBefore = 0;
    for (uint32_t i = 0; i < kRegisteredTextureCount; ++i)
    {
        const TextureHandle tex = CreatePixelTexture(*dev, "UIDropPin_Before");
        ASSERT_TRUE(tex.IsValid());
        if (registry.Register(tex) != 0u)
            ++registeredBefore;
    }
    ASSERT_EQ(registeredBefore, kRegisteredTextureCount);

    if (!DriveRebuild(*dev))
        GTEST_SKIP() << "injected loss did not produce a rebuild on this adapter";

    // Re-register on the rebuilt device so the set being written has real
    // content. Counted, so a set that ended up empty cannot pass by writing
    // nothing.
    uint32_t registeredAfter = 0;
    for (uint32_t i = 0; i < kRegisteredTextureCount; ++i)
    {
        const TextureHandle tex = CreatePixelTexture(*dev, "UIDropPin_After");
        ASSERT_TRUE(tex.IsValid());
        if (registry.Register(tex) != 0u)
            ++registeredAfter;
    }
    ASSERT_EQ(registeredAfter, kRegisteredTextureCount)
        << "nothing was registered post-rebuild, so a zero drop count would be vacuous";

    Logger::Log::Initialize({});
    auto sink = Logger::MakeUnique<Logger::CallbackSink>();
    auto* sinkPtr = sink.get();
    auto dropCount = std::make_shared<std::atomic<int>>(0);
    // Matches both backends: the descriptor-buffer path logs "dropped N of M
    // descriptor-buffer write(s)", the legacy pool path "dropping descriptor
    // write with unresolvable handle".
    const Logger::uint64 callbackId = sinkPtr->RegisterCallback(
        [dropCount](const Logger::LogMessage& msg)
        {
            const bool dropped = msg.Message.find("dropped") != Logger::String::npos
                              || msg.Message.find("dropping") != Logger::String::npos;
            if (dropped && msg.Message.find("descriptor") != Logger::String::npos)
                dropCount->fetch_add(1);
        });
    Logger::Log::AddSink(std::move(sink));

    Logger::Log::Flush();
    ASSERT_EQ(dropCount->load(), 0) << "nothing dropped before the set was materialised";

    const DescriptorSetHandle set = registry.CreateTransientFrameSet();
    Logger::Log::Flush();

    EXPECT_TRUE(set.IsValid()) << "the UI texture set could not be created after a rebuild";
    EXPECT_EQ(dropCount->load(), 0)
        << "the UI texture set was written with handles the rebuilt device cannot resolve "
           "(dead dummy texture and/or dead samplers held from the previous generation)";

    sinkPtr->UnregisterCallback(callbackId);
}

// --- Band dummy (binding 1 slot 0) across a rebuild ---
//
// Binding 1 reserves slot 0 for a zero-filled integer dummy, and
// CreateTransientFrameSet writes every band slot from it, because the shader
// turns band texel data into a loop trip count — an unwritten slot is a
// recycled descriptor and a potential GPU hang, not a wrong pixel. That gives
// the heal two extra obligations, pinned separately below: the band dummy is
// device state and must be re-provisioned with the white one, and the band
// slot allocator must reset to 1, never 0, or the dummy's reserved slot is
// handed to the first Slug page.

namespace
{
// Load the Roboto staged next to the test executable (see UITests' POST_BUILD
// copy) and shape a string through it, which is what drives Slug glyph packing
// and page creation. Returns false when the font is not staged.
bool LoadShapedSlugAtlas(Rendering::Text::FontAtlas& atlas, unsigned pixelSize)
{
    const std::filesystem::path fontPath =
        PathUtils::GetExecutableDirectory() / "Assets" / "Fonts" / "Roboto-Regular.ttf";
    std::error_code ec;
    if (!std::filesystem::exists(fontPath, ec))
        return false;

    std::vector<unsigned char> bytes;
    {
        FILE* f = std::fopen(fontPath.string().c_str(), "rb");
        if (!f)
            return false;
        std::fseek(f, 0, SEEK_END);
        long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size > 0)
        {
            bytes.resize(static_cast<size_t>(size));
            if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size())
                bytes.clear();
        }
        std::fclose(f);
    }
    if (bytes.empty())
        return false;

    if (!atlas.LoadFontBytes(bytes.data(), bytes.size(), pixelSize))
        return false;

    Rendering::Text::FontAtlas::ShapeResult shaped;
    atlas.ShapeText("Slug band slots", static_cast<float>(pixelSize), shaped);
    return atlas.GetSlugPageCount() > 0;
}
} // namespace

// The band dummy is a device resource like the white dummy and the samplers:
// after a rebuild the pre-rebuild handle names a destroyed VkObject while still
// reporting IsValid(). The heal must re-provision it, and the healed handle
// must be one the rebuilt device resolves — observable as the transient set
// materialising every band slot with zero dropped descriptor writes.
TEST(UITextureRegistryDeviceRebuild, BandDummyIsReprovisionedAfterRebuild)
{
    ScopedEnvVar forcedLoss("GE_VK_FORCE_DEVICE_LOST", "2");
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "no Vulkan device available";

    UI::UITextureRegistry registry(dev.get());

    if (!DriveRebuild(*dev))
        GTEST_SKIP() << "injected loss did not produce a rebuild on this adapter";
    ASSERT_GT(dev->GetDeviceRebuildGeneration(), 0u);

    Logger::Log::Initialize({});
    // Drain anything earlier tests enqueued before the sink below exists, so
    // the baseline asserts observe only this test's messages.
    Logger::Log::Flush();
    auto sink = Logger::MakeUnique<Logger::CallbackSink>();
    auto* sinkPtr = sink.get();
    auto dropCount = std::make_shared<std::atomic<int>>(0);
    auto healLineSeen = std::make_shared<std::atomic<bool>>(false);
    auto bandDummyLive = std::make_shared<std::atomic<bool>>(false);
    // Drop matching as in NoDroppedDescriptorWritesAfterRebuild; the heal line
    // additionally reports whether each re-provisioned resource came up valid.
    const Logger::uint64 callbackId = sinkPtr->RegisterCallback(
        [dropCount, healLineSeen, bandDummyLive](const Logger::LogMessage& msg)
        {
            const bool dropped = msg.Message.find("dropped") != Logger::String::npos
                              || msg.Message.find("dropping") != Logger::String::npos;
            if (dropped && msg.Message.find("descriptor") != Logger::String::npos)
                dropCount->fetch_add(1);
            if (msg.Message.find("UITextureRegistry: device rebuild") != Logger::String::npos)
            {
                healLineSeen->store(true);
                if (msg.Message.find("bandDummy=true") != Logger::String::npos)
                    bandDummyLive->store(true);
            }
        });
    Logger::Log::AddSink(std::move(sink));

    Logger::Log::Flush();
    ASSERT_FALSE(healLineSeen->load())
        << "the heal ran before CreateTransientFrameSet, so the pin below would "
           "not be observing this call";
    ASSERT_EQ(dropCount->load(), 0) << "nothing dropped before the set was materialised";

    // The heal runs inside this call, ahead of the descriptor writes; the set
    // then writes every band slot, unoccupied ones from the band dummy.
    const DescriptorSetHandle set = registry.CreateTransientFrameSet();
    Logger::Log::Flush();

    EXPECT_TRUE(set.IsValid()) << "the UI texture set could not be created after a rebuild";
    ASSERT_TRUE(healLineSeen->load())
        << "CreateTransientFrameSet did not heal, so every band-slot write below "
           "used the pre-rebuild band dummy";
    EXPECT_TRUE(bandDummyLive->load())
        << "the heal did not re-provision the band dummy (bandDummy=false): every "
           "unoccupied band slot is left unwritten and reads recycled descriptor "
           "memory, which is the defect the band dummy exists to close";
    EXPECT_EQ(dropCount->load(), 0)
        << "band-slot descriptor writes were dropped: the band dummy the heal "
           "kept is a handle the rebuilt device cannot resolve (the pre-rebuild "
           "one), not a live re-provisioned texture";

    sinkPtr->UnregisterCallback(callbackId);
}

// Band slot 0 belongs to the band dummy, and the heal resets the allocator.
// A reset to 0 hands slot 0 to the first Slug page registered after the
// rebuild; the fallback path then overwrites that page's binding and its
// glyphs sample the dummy. The first allocation after a heal must be slot 1,
// exactly as it is on a fresh registry.
TEST(UITextureRegistryDeviceRebuild, BandSlotAllocationRestartsAtOneAfterRebuild)
{
    ScopedEnvVar forcedLoss("GE_VK_FORCE_DEVICE_LOST", "2");
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "no Vulkan device available";

    UI::UITextureRegistry registry(dev.get());

    Rendering::Text::FontAtlas atlas;
    if (!LoadShapedSlugAtlas(atlas, 32))
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    {
        const auto& indices = registry.RegisterSlugTextures(atlas);
        ASSERT_FALSE(indices.empty()) << "no Slug pages registered, so the pin would prove nothing";
        // Population + instrument guard: on a fresh registry the first page
        // takes band slot 1 (slot 0 is the dummy's). This is the same
        // observable the post-rebuild assertion reads, shown working before
        // the rebuild.
        ASSERT_EQ(indices.front().BandTexIdx, 1u);
    }

    if (!DriveRebuild(*dev))
        GTEST_SKIP() << "injected loss did not produce a rebuild on this adapter";
    ASSERT_GT(dev->GetDeviceRebuildGeneration(), 0u);

    // The heal inside RegisterSlugTextures discards the Slug cache, so the same
    // atlas re-registers from scratch on the rebuilt device and re-runs the
    // band slot allocator from its healed state.
    const auto& indices = registry.RegisterSlugTextures(atlas);
    ASSERT_FALSE(indices.empty()) << "no Slug pages re-registered after the rebuild";
    for (const auto& idx : indices)
        EXPECT_NE(idx.BandTexIdx, 0u)
            << "a real Slug page landed on the reserved dummy band slot: the heal "
               "reset the band allocator to 0 instead of 1";
    EXPECT_EQ(indices.front().BandTexIdx, 1u)
        << "post-heal band slot numbering does not restart at 1 as it does on a "
           "fresh registry";
}
