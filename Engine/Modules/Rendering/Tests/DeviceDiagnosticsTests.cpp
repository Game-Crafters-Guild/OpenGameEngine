#include "Source/Vulkan/VulkanDevice.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include "Source/Vulkan/ValidationStats.h"
#include "Source/Vulkan/VulkanDebugUtilsLabels.h"
#include "Source/Vulkan/VulkanDeviceFault.h"
#include "Source/Vulkan/VulkanGpuCheckpoints.h"
#include "Source/Vulkan/VulkanSharedInstance.h"
#include "Source/Vulkan/VulkanToolingInfo.h"
#include "Source/Vulkan/VulkanValidationLayerSettings.h"
#include "Source/Vulkan/VulkanVuidSuppressions.h"
#include "ScopedEnvVar.h"
#include "TestUtils.h"

using namespace GameEngine::Rendering;
using GameEngine::Rendering::Tests::ScopedEnvVar;

TEST(DeviceDiagnostics, HdrModeParsingAndFallbacks)
{
    EXPECT_EQ(HdrOutputModeFromString("HDR10 PQ"), HdrOutputMode::HDR10_PQ);
    EXPECT_EQ(HdrOutputModeFromString("ST2084"), HdrOutputMode::HDR10_PQ);
    EXPECT_EQ(HdrOutputModeFromString("HLG"), HdrOutputMode::HLG);
    EXPECT_EQ(HdrOutputModeFromString("scRGB"), HdrOutputMode::ScRGB);
    EXPECT_EQ(HdrOutputModeFromString("HDR10+"), HdrOutputMode::HDR10Plus);

    HdrDisplayInfo display{};
    display.hdrAvailable = true;
    display.supportsHDR10_PQ = true;
    display.supportsHLG = true;
    EXPECT_EQ(ResolveHdrOutputMode(HdrOutputMode::Auto, display), HdrOutputMode::HDR10_PQ);
    EXPECT_EQ(ResolveHdrOutputMode(HdrOutputMode::HDR10Plus, display), HdrOutputMode::HDR10_PQ);
    display.supportsHDR10_PQ = false;
    EXPECT_EQ(ResolveHdrOutputMode(HdrOutputMode::Auto, display), HdrOutputMode::HLG);
}

// The scRGB anchor is decided by the platform compositor from the HDR state alone,
// so a backend that never touches it (the default-constructed state) still gets
// the right one: the paper white on macOS (encode scale 1, the OS applies the
// lift), 80 nits elsewhere (encode scale paperWhite / 80).
TEST(DeviceDiagnostics, ScRGBAnchorIsTheCompositorReferenceWhite)
{
    HdrOutputState state{};
    for (const float paperWhite : {203.0f, 300.0f})
    {
        state.staticMetadata.paperWhiteNits = paperWhite;
#if defined(__APPLE__)
        EXPECT_FLOAT_EQ(GetScRGBFramebufferWhiteNits(state), paperWhite);
        EXPECT_FLOAT_EQ(paperWhite / GetScRGBFramebufferWhiteNits(state), 1.0f);
#else
        EXPECT_FLOAT_EQ(GetScRGBFramebufferWhiteNits(state), kHdrPaperWhiteFloorNits);
        EXPECT_FLOAT_EQ(paperWhite / GetScRGBFramebufferWhiteNits(state), paperWhite / 80.0f);
#endif
    }
}

// A launch flag or env var spelled wrong must not read as "Off". Collapsing an
// unrecognised value to Off is how a typo silently disables HDR, so the checked
// parse separates "spelled Off" from "not a mode at all"; only callers that opt
// into the lossy form get the Off fallback.
TEST(DeviceDiagnostics, TryParseHdrOutputModeSeparatesOffFromUnrecognised)
{
    EXPECT_EQ(TryParseHdrOutputMode("Off"), HdrOutputMode::Off);
    EXPECT_EQ(TryParseHdrOutputMode("off"), HdrOutputMode::Off);
    EXPECT_EQ(TryParseHdrOutputMode("0"), HdrOutputMode::Off);
    EXPECT_EQ(TryParseHdrOutputMode("false"), HdrOutputMode::Off);
    EXPECT_EQ(TryParseHdrOutputMode("SDR"), HdrOutputMode::Off);

    EXPECT_EQ(TryParseHdrOutputMode("Auto"), HdrOutputMode::Auto);
    EXPECT_EQ(TryParseHdrOutputMode("HDR10 PQ"), HdrOutputMode::HDR10_PQ);
    EXPECT_EQ(TryParseHdrOutputMode("scRGB"), HdrOutputMode::ScRGB);

    EXPECT_FALSE(TryParseHdrOutputMode("banana").has_value());
    EXPECT_FALSE(TryParseHdrOutputMode("hdr").has_value());
    EXPECT_FALSE(TryParseHdrOutputMode("").has_value());

    // The lossy wrapper keeps its contract for every existing caller: both an
    // explicit Off and an unrecognised value come back as Off.
    EXPECT_EQ(HdrOutputModeFromString("Off"), HdrOutputMode::Off);
    EXPECT_EQ(HdrOutputModeFromString("banana"), HdrOutputMode::Off);
}

// Env values routinely arrive with a trailing CR (CRLF files, shell pipelines,
// CI scripts). Refusing over invisible whitespace sends the caller back to its
// default with nothing wrong to see in the value it set — the same silent
// wrong-answer this parse exists to prevent.
TEST(DeviceDiagnostics, TryParseHdrOutputModeIgnoresSurroundingWhitespace)
{
    EXPECT_EQ(TryParseHdrOutputMode("Off\r"), HdrOutputMode::Off);
    EXPECT_EQ(TryParseHdrOutputMode("Off\r\n"), HdrOutputMode::Off);
    EXPECT_EQ(TryParseHdrOutputMode("Off\t"), HdrOutputMode::Off);
    EXPECT_EQ(TryParseHdrOutputMode(" Off "), HdrOutputMode::Off);
    EXPECT_EQ(TryParseHdrOutputMode("\tAuto\r\n"), HdrOutputMode::Auto);
    EXPECT_EQ(TryParseHdrOutputMode("HDR10_PQ\r\n"), HdrOutputMode::HDR10_PQ);

    // Whitespace tolerance must not manufacture a mode out of nothing.
    EXPECT_FALSE(TryParseHdrOutputMode("\r\n").has_value());
    EXPECT_FALSE(TryParseHdrOutputMode("   ").has_value());
}

// The negative spellings had aliases and the positive ones did not, so an
// operator who learned "Off" works and reached for "On" got a refusal — and a
// refusal means "whatever the project said", the opposite of an override.
TEST(DeviceDiagnostics, TryParseHdrOutputModeAcceptsSymmetricBooleanAliases)
{
    EXPECT_EQ(TryParseHdrOutputMode("On"), HdrOutputMode::Auto);
    EXPECT_EQ(TryParseHdrOutputMode("on"), HdrOutputMode::Auto);
    EXPECT_EQ(TryParseHdrOutputMode("1"), HdrOutputMode::Auto);
    EXPECT_EQ(TryParseHdrOutputMode("true"), HdrOutputMode::Auto);
    EXPECT_EQ(TryParseHdrOutputMode("yes"), HdrOutputMode::Auto);
    EXPECT_EQ(TryParseHdrOutputMode("enabled"), HdrOutputMode::Auto);

    // Each positive alias has its negative counterpart, or the asymmetry just
    // moves down a level.
    EXPECT_EQ(TryParseHdrOutputMode("0"), HdrOutputMode::Off);
    EXPECT_EQ(TryParseHdrOutputMode("false"), HdrOutputMode::Off);
    EXPECT_EQ(TryParseHdrOutputMode("no"), HdrOutputMode::Off);
    EXPECT_EQ(TryParseHdrOutputMode("disabled"), HdrOutputMode::Off);
}

TEST(DeviceDiagnostics, HdrMetadataDefaultsAndLgC7Names)
{
    HdrStaticMetadata metadata{};
    EXPECT_FLOAT_EQ(metadata.redPrimary[0], 0.708f);
    EXPECT_FLOAT_EQ(metadata.greenPrimary[1], 0.797f);
    EXPECT_FLOAT_EQ(metadata.whitePoint[0], 0.3127f);
    EXPECT_FLOAT_EQ(metadata.maxMasteringLuminance, 1000.0f);
    EXPECT_FLOAT_EQ(metadata.paperWhiteNits, 203.0f);

    EXPECT_TRUE(IsKnownLGOledC7DisplayName("LG OLED55C7P"));
    EXPECT_TRUE(IsKnownLGOledC7DisplayName("OLED65C7V"));
    EXPECT_TRUE(IsKnownLGOledC7DisplayName("LG TV"));
    EXPECT_FALSE(IsKnownLGOledC7DisplayName("Generic HDR Monitor"));
}

static void SetEnvHeadless()
{
#if defined(_WIN32)
    _putenv_s("GE_HEADLESS_TEST", "1");
    // This suite deliberately induces validation errors to test CAPTURE; the
    // Phase-3b default-on assert would abort the process before the
    // assertions run. Set process-wide (the assert's enable caches on first
    // error) so test ordering can never re-arm it mid-run.
    _putenv_s("GE_VK_VALIDATION_ASSERT", "0");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
    setenv("GE_VK_VALIDATION_ASSERT", "0", 1);
#endif
}

TEST(DeviceDiagnostics, DebugBindCounters_Smoke)
{
    SetEnvHeadless();
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    ASSERT_TRUE(dev->Initialize(dd));

    // After BeginFrame(), counters should be reset to zero
    ASSERT_TRUE(dev->BeginFrame());
    auto c0 = dev->DebugGetBindCounters();
    EXPECT_EQ(c0.pipelineBindsGraphics, 0u);
    EXPECT_EQ(c0.pipelineBindsCompute, 0u);
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(c0.descriptorBindsGraphics[i], 0u);
        EXPECT_EQ(c0.descriptorBindsCompute[i], 0u);
    }

    // Explicit reset leaves them at zero
    dev->DebugResetBindCounters();
    auto c1 = dev->DebugGetBindCounters();
    EXPECT_EQ(c1.pipelineBindsGraphics, 0u);
    EXPECT_EQ(c1.pipelineBindsCompute, 0u);
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(c1.descriptorBindsGraphics[i], 0u);
        EXPECT_EQ(c1.descriptorBindsCompute[i], 0u);
    }

    dev->Present();
}

TEST(DeviceDiagnostics, OwnershipTransferSuppression_SingleQueue_Smoke)
{
    SetEnvHeadless();
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    ASSERT_TRUE(dev->Initialize(dd));

    ASSERT_TRUE(dev->BeginFrame());

    // Submit an empty graphics command list to exercise the path without multi-queue ownership transfers
    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl != nullptr);
    cl->Begin();
    cl->End();
    std::vector<CommandList*> lists{ cl.get() };
    dev->ExecuteCommandLists(lists);

    // In single-queue mode (default), ownership transfers should be suppressed
    // We can only assert via the device's debug getter if available; otherwise we treat as smoke.
    auto counters = dev->DebugGetBindCounters(); // touch virtual to avoid unused warnings
    (void)counters;

    // Finalize/present to rotate frame
    dev->Present();
}



// A partial buffer->texture copy must write only the destination rect and
// preserve the rest of the subresource. The pre-copy barrier resolves the old
// layout from command-list-LOCAL tracking, so a fresh command list (the
// cross-frame case) would transition a persistent texture from UNDEFINED — a
// spec-legal discard of everything outside the rect. Supplying the resting
// ShaderResource state makes the transition content-preserving. This test seeds
// a texture in one command list, then in a SECOND command list (cleared
// tracking) overwrites a middle band, and asserts the outside rows survive.
TEST(DeviceDiagnostics, RegionCopyPreservesUntouchedTexels)
{
    SetEnvHeadless();
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    ASSERT_TRUE(dev->Initialize(dd));
    ASSERT_TRUE(dev->BeginFrame());

    constexpr uint32_t kW = 4u;
    constexpr uint32_t kH = 4u;
    constexpr size_t kRowPitch = static_cast<size_t>(kW) * sizeof(float);
    constexpr uint32_t kBandRow = 1u;   // overwrite rows [1,3)
    constexpr uint32_t kBandRows = 2u;

    TextureDesc td{};
    td.width = kW;
    td.height = kH;
    td.depth = 1u;
    td.mipLevels = 1u;
    td.arrayLayers = 1u;
    td.sampleCount = 1u;
    td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource |
                                     TextureUsage::TransferSrc | TextureUsage::TransferDst);
    td.debugName = "RegionCopyTest.Tex";
    TextureHandle tex = dev->CreateTexture(td);
    ASSERT_TRUE(tex.IsValid());

    // Full seed: texel(x,y) = y*10 + x.
    std::vector<float> seed(static_cast<size_t>(kW) * kH);
    for (uint32_t y = 0; y < kH; ++y)
        for (uint32_t x = 0; x < kW; ++x)
            seed[y * kW + x] = static_cast<float>(y * 10u + x);
    BufferHandle seedBuf = dev->CreateUploadBuffer(seed.size() * sizeof(float), "RegionCopyTest.Seed");
    ASSERT_TRUE(seedBuf.IsValid());
    dev->UpdateBuffer(seedBuf, 0, seed.size() * sizeof(float), seed.data());

    // Band values (rows 1..2): texel(x,y) = 500 + y*10 + x. Staging holds only
    // the band's rows, tightly packed, so the copy's source offset is 0.
    std::vector<float> band(static_cast<size_t>(kW) * kBandRows);
    for (uint32_t r = 0; r < kBandRows; ++r)
        for (uint32_t x = 0; x < kW; ++x)
            band[r * kW + x] = 500.0f + static_cast<float>((kBandRow + r) * 10u + x);
    BufferHandle bandBuf = dev->CreateUploadBuffer(band.size() * sizeof(float), "RegionCopyTest.Band");
    ASSERT_TRUE(bandBuf.IsValid());
    dev->UpdateBuffer(bandBuf, 0, band.size() * sizeof(float), band.data());

    BufferHandle readback = dev->CreateReadbackBuffer(seed.size() * sizeof(float), "RegionCopyTest.Read");
    ASSERT_TRUE(readback.IsValid());

    // CL1: full upload, then rest the texture in ShaderResource (as a live texture would).
    auto cl1 = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl1 != nullptr);
    cl1->Begin();
    cl1->CopyBufferToTextureSubresource(seedBuf, tex, 0, 0, kW, kH, 0, kRowPitch);
    cl1->Barrier(ResourceBarrier::CreateTextureBarrier(
        tex, ResourceState::CopyDest, ResourceState::ShaderResource));
    cl1->End();

    // CL2 (fresh tracking): band copy into rows [1,3), preserving rows 0 and 3 by
    // telling the copy the texture's resting ShaderResource layout, then read back.
    auto cl2 = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl2 != nullptr);
    cl2->Begin();
    cl2->CopyBufferToTextureSubresource(bandBuf, tex, 0, 0, kW, kBandRows,
                                        0, kRowPitch, /*depth=*/1, /*srcSlicePitchBytes=*/0,
                                        /*dstX=*/0, /*dstY=*/kBandRow, ResourceState::ShaderResource);
    cl2->CopyTextureSubresourceToBuffer(tex, 0, 0, readback, kW, kH);
    cl2->End();

    std::vector<CommandList*> lists{ cl1.get(), cl2.get() };
    dev->ExecuteCommandLists(lists);
    dev->WaitForIdle();

    std::vector<float> result(seed.size(), -1.0f);
    const void* mapped = dev->MapBuffer(readback);
    ASSERT_NE(mapped, nullptr);
    std::memcpy(result.data(), mapped, result.size() * sizeof(float));
    dev->UnmapBuffer(readback);

    for (uint32_t y = 0; y < kH; ++y)
    {
        const bool inBand = y >= kBandRow && y < kBandRow + kBandRows;
        for (uint32_t x = 0; x < kW; ++x)
        {
            const float expected = inBand ? 500.0f + static_cast<float>(y * 10u + x)
                                          : static_cast<float>(y * 10u + x);
            EXPECT_FLOAT_EQ(result[y * kW + x], expected)
                << "texel (" << x << "," << y << ") inBand=" << inBand;
        }
    }

    dev->DestroyBuffer(seedBuf);
    dev->DestroyBuffer(bandBuf);
    dev->DestroyBuffer(readback);
    dev->DestroyTexture(tex);
    dev->Present();
}

TEST(DeviceDiagnostics, OwnershipTransferSuppression_AssertZero_SingleQueue)
{
    SetEnvHeadless();
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    ASSERT_TRUE(dev->Initialize(dd));

    ASSERT_TRUE(dev->BeginFrame());

    // Submit an empty graphics command list
    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl != nullptr);
    cl->Begin();
    cl->End();
    std::vector<CommandList*> lists{ cl.get() };
    dev->ExecuteCommandLists(lists);

    // Present to complete frame
    dev->Present();

    // Vulkan-specific assertion: no ownership transfers should be recorded when single queue
    auto* vulkan = dynamic_cast<GameEngine::Rendering::VulkanDevice*>(dev.get());
    ASSERT_NE(vulkan, nullptr);
    EXPECT_EQ(vulkan->GetDebugOwnershipTransfers(), 0u);
}

namespace
{
// Build a compute pipeline from a .comp.spv with a single set=0/binding=0
// storage buffer — the layout both F1 test kernels share.
PipelineHandle CreateStorageComputePipeline(IDevice& dev, const char* spvName, const char* debugName)
{
    std::vector<uint8_t> code = GameEngine::Rendering::Tests::ReadSpirvBytes(spvName);
    if (code.empty())
        return {};

    DescriptorSetLayoutDesc setLayout{};
    DescriptorBinding binding{};
    binding.binding = 0;
    binding.type = DescriptorType::StorageBuffer;
    binding.count = 1;
    binding.shaderStages = VK_SHADER_STAGE_COMPUTE_BIT;
    setLayout.bindings.push_back(binding);
    setLayout.debugName = debugName;

    PipelineDesc pd{};
    pd.type = PipelineType::Compute;
    pd.computeShader = std::move(code);
    pd.debugName = debugName;
    pd.descriptorSetLayouts.push_back(setLayout);
    return dev.CreatePipeline(pd);
}

DescriptorSetHandle CreateStorageDescriptor(IDevice& dev, BufferHandle buffer, size_t range, const char* debugName)
{
    DescriptorSetLayoutDesc setLayout{};
    DescriptorBinding binding{};
    binding.binding = 0;
    binding.type = DescriptorType::StorageBuffer;
    binding.count = 1;
    binding.shaderStages = VK_SHADER_STAGE_COMPUTE_BIT;
    setLayout.bindings.push_back(binding);

    DescriptorSetDesc ds{};
    ds.layout = setLayout;
    ds.debugName = debugName;
    DescriptorSetHandle handle = dev.CreateDescriptorSet(ds);
    if (!handle.IsValid())
        return handle;

    DescriptorSetUpdate upd{};
    upd.binding = 0;
    upd.type = DescriptorType::StorageBuffer;
    upd.buffers = {buffer};
    upd.bufferOffsets = {0};
    upd.bufferRanges = {range};
    dev.UpdateDescriptorSet(handle, upd);
    return handle;
}
} // namespace

// F1: DispatchIndirect consumes GPU-written workgroup counts.
// Kernel A writes {2,1,1} into an args buffer; a buffer barrier transitions it
// to IndirectArgs; DispatchIndirect(argsBuffer) drives kernel B, which tallies
// one atomic increment per workgroup and stamps each workgroup id. The readback
// proves exactly two workgroups ran off the GPU-produced args — not a CPU
// constant, which is the entire value of the indirect path.
TEST(DeviceDiagnostics, DispatchIndirectConsumesGpuWrittenArgs)
{
    SetEnvHeadless();
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    ASSERT_TRUE(dev->Initialize(dd));
    ASSERT_TRUE(dev->BeginFrame());

    // Args buffer: kernel A writes it (Storage), DispatchIndirect reads it (Indirect).
    // 4 u32 (16 B) — only the first three are the workgroup counts.
    constexpr size_t kArgsSize = 4u * sizeof(uint32_t);
    BufferDesc argsDesc{};
    argsDesc.size = kArgsSize;
    argsDesc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::Indirect);
    argsDesc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    argsDesc.debugName = "DispatchIndirect.Args";
    BufferHandle argsBuffer = dev->CreateBuffer(argsDesc);
    ASSERT_TRUE(argsBuffer.IsValid());

    // Tally buffer: word 0 = atomic workgroup counter, words 1..= per-workgroup marks.
    constexpr uint32_t kTallyWords = 8u;
    constexpr size_t   kTallySize  = kTallyWords * sizeof(uint32_t);
    BufferDesc tallyDesc{};
    tallyDesc.size = kTallySize;
    tallyDesc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferSrc | BufferUsage::TransferDst);
    tallyDesc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    tallyDesc.debugName = "DispatchIndirect.Tally";
    BufferHandle tallyBuffer = dev->CreateBuffer(tallyDesc);
    ASSERT_TRUE(tallyBuffer.IsValid());

    BufferHandle readback = dev->CreateReadbackBuffer(kTallySize, "DispatchIndirect.Read");
    ASSERT_TRUE(readback.IsValid());

    PipelineHandle writeArgs = CreateStorageComputePipeline(*dev, "dispatch_indirect_write_args.comp.spv", "DI.WriteArgs");
    ASSERT_TRUE(writeArgs.IsValid()) << "kernel A pipeline (or its SPIR-V) missing";
    PipelineHandle tally = CreateStorageComputePipeline(*dev, "dispatch_indirect_tally.comp.spv", "DI.Tally");
    ASSERT_TRUE(tally.IsValid()) << "kernel B pipeline (or its SPIR-V) missing";

    DescriptorSetHandle argsDs = CreateStorageDescriptor(*dev, argsBuffer, kArgsSize, "DI.ArgsDS");
    ASSERT_TRUE(argsDs.IsValid());
    DescriptorSetHandle tallyDs = CreateStorageDescriptor(*dev, tallyBuffer, kTallySize, "DI.TallyDS");
    ASSERT_TRUE(tallyDs.IsValid());

    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl != nullptr);
    cl->Begin();

    // Zero the tally on the GPU timeline, then make it visible to the compute write.
    cl->FillBuffer(tallyBuffer, 0, kTallySize, 0u);
    cl->Barrier(ResourceBarrier::CreateBufferBarrier(
        tallyBuffer, ResourceState::CopyDest, ResourceState::UnorderedAccess));

    // Kernel A: fill the indirect args (fresh buffer, no prior access to sync).
    cl->SetPipeline(writeArgs);
    cl->BindDescriptorSet(0, argsDs, writeArgs);
    cl->Dispatch(1, 1, 1);

    // The args write must be visible to the indirect fetch (DRAW_INDIRECT stage).
    cl->Barrier(ResourceBarrier::CreateBufferBarrier(
        argsBuffer, ResourceState::UnorderedAccess, ResourceState::IndirectArgs));

    // Kernel B: driven by the GPU-written {2,1,1}.
    cl->SetPipeline(tally);
    cl->BindDescriptorSet(0, tallyDs, tally);
    cl->DispatchIndirect(argsBuffer, 0);

    // Kernel B's writes must be visible to the readback copy.
    cl->Barrier(ResourceBarrier::CreateBufferBarrier(
        tallyBuffer, ResourceState::UnorderedAccess, ResourceState::CopySource));
    cl->CopyBuffer(tallyBuffer, readback, kTallySize, 0, 0);
    cl->End();

    std::vector<CommandList*> lists{cl.get()};
    dev->ExecuteCommandLists(lists);
    dev->WaitForIdle();

    std::vector<uint32_t> result(kTallyWords, 0xDEADBEEFu);
    const void* mapped = dev->MapBuffer(readback);
    ASSERT_NE(mapped, nullptr);
    std::memcpy(result.data(), mapped, result.size() * sizeof(uint32_t));
    dev->UnmapBuffer(readback);

    EXPECT_EQ(result[0], 2u) << "atomic counter should equal the two workgroups from GPU args";
    EXPECT_EQ(result[1], 100u) << "workgroup 0 did not run";
    EXPECT_EQ(result[2], 101u) << "workgroup 1 did not run";
    EXPECT_EQ(result[3], 0u) << "a third workgroup ran — indirect count was not {2,1,1}";

    dev->DestroyBuffer(readback);
    dev->DestroyBuffer(tallyBuffer);
    dev->DestroyBuffer(argsBuffer);
    dev->DestroyPipeline(writeArgs);
    dev->DestroyPipeline(tally);
    dev->Present();
}

// --- ValidationStatsStore unit tests (standalone instances; the callback-facing
// record path is exercised end-to-end by the induced-error test below) ---

TEST(ValidationStats, PerVuidExactCountingAndFirstOccurrenceCapture)
{
    ValidationStatsStore store;
    store.NoteFrameBegin(); // frame 1

    ValidationMessageInfo first{};
    first.IsError = true;
    first.Vuid = "VUID-test-0001";
    first.Message = "first full message text";
    first.Objects = "BufferA, BufferB";
    first.Labels = "ShadowPass > Cascade0";
    auto o1 = store.Record(first);
    EXPECT_TRUE(o1.FirstOccurrence);
    EXPECT_EQ(o1.Count, 1u);

    store.NoteFrameBegin(); // frame 2
    ValidationMessageInfo repeat = first;
    repeat.Message = "second message text differs";
    repeat.Objects = "BufferC";
    auto o2 = store.Record(repeat);
    EXPECT_FALSE(o2.FirstOccurrence);
    EXPECT_EQ(o2.Count, 2u);

    auto stats = store.Snapshot(true);
    EXPECT_TRUE(stats.Enabled);
    EXPECT_EQ(stats.ErrorCount, 2u);
    EXPECT_EQ(stats.WarningCount, 0u);
    ASSERT_EQ(stats.Vuids.size(), 1u);
    const auto& v = stats.Vuids[0];
    EXPECT_EQ(v.Vuid, "VUID-test-0001");
    EXPECT_TRUE(v.IsError);
    EXPECT_EQ(v.Count, 2u);
    // First occurrence wins the captured context; later hits only bump counters.
    EXPECT_EQ(v.FirstMessage, "first full message text");
    EXPECT_EQ(v.FirstObjects, "BufferA, BufferB");
    EXPECT_EQ(v.FirstLabels, "ShadowPass > Cascade0");
    EXPECT_EQ(v.FirstFrame, 1u);
    EXPECT_EQ(v.LastFrame, 2u);
}

TEST(ValidationStats, IdLessMessagesDedupeOnMessageHead)
{
    ValidationStatsStore store;
    ValidationMessageInfo m{};
    m.IsError = false;
    m.Vuid = "";
    m.Message = "loader diagnostic without a VUID";
    EXPECT_TRUE(store.Record(m).FirstOccurrence);
    EXPECT_FALSE(store.Record(m).FirstOccurrence); // same head -> same entry

    ValidationMessageInfo other = m;
    other.Message = "a different id-less diagnostic";
    EXPECT_TRUE(store.Record(other).FirstOccurrence); // different head -> new entry

    auto stats = store.Snapshot(true);
    EXPECT_EQ(stats.WarningCount, 3u);
    ASSERT_EQ(stats.Vuids.size(), 2u);
    EXPECT_EQ(stats.Vuids[0].Vuid.rfind("msg-", 0), 0u) << "synthetic key expected";
}

TEST(ValidationStats, SuppressedHitsCountSeparatelyAndExactly)
{
    ValidationStatsStore store;
    ValidationMessageInfo m{};
    m.IsError = true;
    m.Vuid = "VUID-suppressed-42";
    m.Message = "suppressed but never hidden";
    m.Suppressed = true;
    store.Record(m);
    store.Record(m);
    m.Suppressed = false;
    store.Record(m);

    auto stats = store.Snapshot(true);
    EXPECT_EQ(stats.ErrorCount, 3u);      // totals stay exact regardless of suppression
    EXPECT_EQ(stats.SuppressedCount, 2u); // reported separately so it can never hide
    ASSERT_EQ(stats.Vuids.size(), 1u);
    EXPECT_EQ(stats.Vuids[0].Count, 3u);
    EXPECT_EQ(stats.Vuids[0].SuppressedCount, 2u);
}

TEST(ValidationStats, OverflowPastVuidCapIsVisibleAndTotalsStayExact)
{
    ValidationStatsStore store;
    const uint32_t kOver = 8;
    for (uint32_t i = 0; i < ValidationStatsStore::kMaxTrackedVuids + kOver; ++i)
    {
        ValidationMessageInfo m{};
        m.IsError = true;
        std::string vuid = "VUID-distinct-" + std::to_string(i);
        m.Vuid = vuid;
        m.Message = "storm";
        auto o = store.Record(m);
        EXPECT_EQ(o.Overflowed, i >= ValidationStatsStore::kMaxTrackedVuids);
    }
    auto stats = store.Snapshot(true);
    EXPECT_EQ(stats.Vuids.size(), ValidationStatsStore::kMaxTrackedVuids);
    EXPECT_EQ(stats.OverflowCount, kOver);
    EXPECT_EQ(stats.ErrorCount, ValidationStatsStore::kMaxTrackedVuids + kOver);
}

TEST(ValidationStats, ResetClearsCountsButKeepsFrameSerial)
{
    ValidationStatsStore store;
    store.NoteFrameBegin();
    store.NoteFrameBegin();
    ValidationMessageInfo m{};
    m.IsError = true;
    m.Vuid = "VUID-reset-1";
    m.Message = "x";
    store.Record(m);
    store.Reset();

    auto stats = store.Snapshot(true);
    EXPECT_EQ(stats.ErrorCount, 0u);
    EXPECT_EQ(stats.WarningCount, 0u);
    EXPECT_EQ(stats.SuppressedCount, 0u);
    EXPECT_EQ(stats.OverflowCount, 0u);
    EXPECT_TRUE(stats.Vuids.empty());
    EXPECT_EQ(stats.FrameSerial, 2u) << "frame serial stamps time, not error state";
}

// The flood guard. The Khronos per-VUID cap is off by construction
// (duplicate_message_limit=0) so the store sees every message; log volume for a
// VUID that fires per-draw is therefore bounded by this policy alone, and the
// debug-utils messenger is the only sink that applies it.
TEST(ValidationLogPolicy, RepeatingVuidLogsOnceThenOncePerStrideWhileCountsStayExact)
{
    ValidationStatsStore store;
    // The ShaderOutputNotConsumed volume measured on 2026-07-23: a class that
    // consumed a 10k duplicate cap in every descriptor config.
    constexpr uint64_t kMessages = 10000;

    uint64_t logged = 0;
    for (uint64_t i = 0; i < kMessages; ++i)
    {
        ValidationMessageInfo m{};
        m.Vuid = "WARNING-Shader-OutputNotConsumed";
        m.Message = "vertex shader writes to output location 0.0 which is not consumed by fragment shader";
        if (ShouldLogValidationOccurrence(store.Record(m)))
            ++logged;
    }

    EXPECT_EQ(logged, 1 + kMessages / kValidationLogRepeatStride);
    EXPECT_EQ(logged, 101u) << "10k repeats must cost ~100 log lines, not 10k";

    const ValidationStats stats = store.Snapshot(true);
    ASSERT_EQ(stats.Vuids.size(), 1u);
    EXPECT_EQ(stats.Vuids[0].Count, kMessages) << "thinning the log must never thin the count";
    EXPECT_EQ(stats.WarningCount, kMessages);
}

TEST(ValidationLogPolicy, FirstOccurrenceAlwaysLogsAndOnlyStrideMultiplesRepeat)
{
    auto shouldLog = [](bool firstOccurrence, uint64_t count) {
        ValidationRecordOutcome outcome{};
        outcome.FirstOccurrence = firstOccurrence;
        outcome.Count = count;
        return ShouldLogValidationOccurrence(outcome);
    };

    EXPECT_TRUE(shouldLog(true, 1));
    EXPECT_FALSE(shouldLog(false, 2));
    EXPECT_FALSE(shouldLog(false, kValidationLogRepeatStride - 1));
    EXPECT_TRUE(shouldLog(false, kValidationLogRepeatStride));
    EXPECT_FALSE(shouldLog(false, kValidationLogRepeatStride + 1));
    EXPECT_TRUE(shouldLog(false, kValidationLogRepeatStride * 2));
}

TEST(ValidationLogPolicy, OverflowOrdinalsAreThinnedOnTheSameStride)
{
    // Past the distinct-VUID cap the ordinal is the overflow total, not a per-VUID
    // count, but it must be thinned the same way — overflow is exactly the case
    // where a storm is already in progress.
    ValidationRecordOutcome outcome{};
    outcome.Overflowed = true;

    outcome.Count = kValidationLogRepeatStride;
    EXPECT_TRUE(ShouldLogValidationOccurrence(outcome));
    outcome.Count = kValidationLogRepeatStride + 1;
    EXPECT_FALSE(ShouldLogValidationOccurrence(outcome));
}

TEST(VuidSuppressions, TableSeedsEmptyAndLookupMatchesExactly)
{
    // The checked-in table ships empty: burn-down fixes errors instead of hiding them.
    EXPECT_TRUE(kVuidSuppressions.empty());
    EXPECT_EQ(FindVuidSuppression("VUID-vkCmdCopyBuffer-size-00113"), nullptr);
    EXPECT_EQ(FindVuidSuppression(""), nullptr);

    // Lookup semantics against an injected table (what the callback consults).
    static constexpr VuidSuppression kTestTable[] = {
        {"VUID-test-known", "test reason", "test-owner", "2026-07-23"},
    };
    const VuidSuppression* hit = FindVuidSuppression("VUID-test-known", kTestTable);
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->Owner, "test-owner");
    EXPECT_EQ(FindVuidSuppression("VUID-test-unknown", kTestTable), nullptr);
}

// End-to-end: a deliberately invalid vkCmdCopyBuffer (region larger than both
// buffers) must land in the store with an exact count, a real VUID id, and the
// named buffer visible in the captured context — proving debug-name propagation
// (vkSetDebugUtilsObjectNameEXT) and callback recording in one pass.
TEST(DeviceDiagnostics, InducedValidationErrorIsCapturedWithObjectName)
{
    SetEnvHeadless();
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    dd.enableDebugLayer = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    ASSERT_TRUE(dev->Initialize(dd));

    if (!dev->GetValidationStats().Enabled)
        GTEST_SKIP() << "Validation layer not available on this machine";

    ASSERT_TRUE(dev->BeginFrame());
    dev->ResetValidationStats(); // exclude any startup noise from the assertion window

    constexpr size_t kBufSize = 16;
    BufferDesc bdesc{};
    bdesc.size = kBufSize;
    bdesc.usage = static_cast<uint32_t>(BufferUsage::Storage);
    bdesc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    bdesc.debugName = "ValStats.InducedSrc";
    BufferHandle src = dev->CreateBuffer(bdesc);
    ASSERT_TRUE(src.IsValid());
    bdesc.debugName = "ValStats.InducedDst";
    BufferHandle dst = dev->CreateBuffer(bdesc);
    ASSERT_TRUE(dst.IsValid());

    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl != nullptr);
    cl->Begin();
    cl->CopyBuffer(src, dst, /*size=*/1024, 0, 0); // 1024 > 16: record-time VUID
    cl->End();
    // Record-time validation fires inside vkCmdCopyBuffer — no submit needed
    // (and submitting an invalid copy risks device-lost on some drivers).

    auto stats = dev->GetValidationStats();
    EXPECT_GE(stats.ErrorCount, 1u);
    ASSERT_FALSE(stats.Vuids.empty());

    bool sawCopyVuid = false;
    bool sawBufferName = false;
    for (const auto& v : stats.Vuids)
    {
        if (v.Vuid.find("vkCmdCopyBuffer") != std::string::npos)
            sawCopyVuid = true;
        if (v.FirstMessage.find("ValStats.Induced") != std::string::npos ||
            v.FirstObjects.find("ValStats.Induced") != std::string::npos)
            sawBufferName = true;
    }
    EXPECT_TRUE(sawCopyVuid) << "expected a VUID-vkCmdCopyBuffer-* entry";
    EXPECT_TRUE(sawBufferName)
        << "named buffer missing from captured context — debug-name propagation broken";

    dev->ResetValidationStats();
    EXPECT_EQ(dev->GetValidationStats().ErrorCount, 0u);

    dev->DestroyBuffer(src);
    dev->DestroyBuffer(dst);
    dev->Present();
}

// --- Shared VkInstance compatibility -----------------------------------------
// Instance-level configuration is settled by whichever device creates the
// process's VkInstance, and a device that shares one inherits that configuration
// without being able to change it. The sharing contract is therefore what keeps a
// device from running without what it asked for: a debug-requesting device landed
// on a non-debug instance has no validation layer and no debug messenger, and
// reports a clean run it never validated.

static std::unique_ptr<IDevice> MakeSharedInstanceProbeDevice(const char* appName, bool debugLayer)
{
    SetEnvHeadless();
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    dd.enableSwapchain = false;
    dd.enableDebugLayer = debugLayer;
    dd.applicationName = appName;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd))
        return nullptr;
    return dev;
}

// Whether a deliberately invalid copy (region larger than both buffers) comes
// back out of the process-wide store the debug messenger records into. Layer,
// messenger and callback all have to be live for that, which makes it the
// strongest observable the API offers for "this device is actually validating" —
// stronger than reading the debug flag back, which says only what was requested.
static bool ValidationErrorReachesTheStore(IDevice& dev)
{
    if (!dev.BeginFrame())
        return false;
    dev.ResetValidationStats(); // exclude startup noise from the observation window

    constexpr size_t kProbeBufferSize = 16;
    constexpr uint64_t kOversizedCopy = 1024; // > kProbeBufferSize: record-time VUID
    BufferDesc bdesc{};
    bdesc.size = kProbeBufferSize;
    bdesc.usage = static_cast<uint32_t>(BufferUsage::Storage);
    bdesc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    bdesc.debugName = "SharedInstance.ProbeSrc";
    BufferHandle src = dev.CreateBuffer(bdesc);
    bdesc.debugName = "SharedInstance.ProbeDst";
    BufferHandle dst = dev.CreateBuffer(bdesc);

    bool sawCopyVuid = false;
    std::unique_ptr<CommandList> cl;
    if (src.IsValid() && dst.IsValid())
    {
        cl = dev.CreateCommandList(IDevice::QueueType::Graphics);
        if (cl)
        {
            cl->Begin();
            // Record-time validation fires inside vkCmdCopyBuffer, so no submit is
            // needed — and submitting an invalid copy risks device-lost.
            cl->CopyBuffer(src, dst, kOversizedCopy, 0, 0);
            cl->End();
            for (const auto& v : dev.GetValidationStats().Vuids)
            {
                if (v.Vuid.find("vkCmdCopyBuffer") != std::string::npos)
                    sawCopyVuid = true;
            }
        }
    }

    if (src.IsValid())
        dev.DestroyBuffer(src);
    if (dst.IsValid())
        dev.DestroyBuffer(dst);
    dev.ResetValidationStats();
    dev.Present();
    return sawCopyVuid;
}

// Every member of the key has to discriminate, because a member that does not is
// a configuration difference the registry hands an instance that cannot serve it.
TEST(SharedVulkanInstance, KeyDistinguishesEveryInstanceLevelConfiguration)
{
    const SharedInstanceKey base{};
    EXPECT_EQ(base, SharedInstanceKey{});

    const auto differsFromBase = [&base](auto&& mutate)
    {
        SharedInstanceKey other = base;
        mutate(other);
        return !(other == base);
    };

    EXPECT_TRUE(differsFromBase([](SharedInstanceKey& k) { k.ApplicationName = "Other"; }));
    EXPECT_TRUE(differsFromBase([](SharedInstanceKey& k) { k.ApplicationVersion = 2; }));
    EXPECT_TRUE(differsFromBase([](SharedInstanceKey& k) { k.DebugLayer = true; }));
    EXPECT_TRUE(differsFromBase([](SharedInstanceKey& k) { k.VerboseMessenger = true; }));
    EXPECT_TRUE(differsFromBase([](SharedInstanceKey& k) { k.SurfaceExtensions = true; }));
    EXPECT_TRUE(differsFromBase([](SharedInstanceKey& k) { k.SyncValidation = true; }));
    EXPECT_TRUE(differsFromBase([](SharedInstanceKey& k) { k.GpuAssistedValidation = true; }));
    EXPECT_TRUE(differsFromBase([](SharedInstanceKey& k) { k.GpuAvShaderInstrumentation = true; }));
}

// The registry compares keys and reference-counts handles; it never calls Vulkan.
// So the reference-counting contract is exercised on handles that are only ever
// compared, and on a registry of the test's own rather than the process singleton
// the device tests in this binary share.
static VkInstance FakeInstance(uintptr_t id)
{
    return reinterpret_cast<VkInstance>(id);
}

// What the instance GOT, not what its key asked for. This is the registry half of
// the inheritance a sharing device performs, and the only way to cover a downgrade
// on a box where the validation layer IS installed — there, no real device can be
// made to publish a debug-requested instance with the layer missing.
TEST(SharedVulkanInstance, AcquireAnswersWithTheDowngradedTruthNotTheRequest)
{
    SharedInstanceRegistry registry;
    SharedInstanceKey requested{};
    requested.ApplicationName = "DowngradeProbe";
    requested.DebugLayer = true;

    SharedInstanceState granted{};
    granted.Instance = FakeInstance(0x1);
    granted.DebugLayerActive = false; // no validation layer on the box
    granted.SwapchainColorSpaceExtEnabled = true;
    granted.GpuAvChained = false;
    registry.Publish(requested, granted);

    const auto adopted = registry.Acquire(requested);
    ASSERT_TRUE(adopted.has_value());
    EXPECT_EQ(adopted->Instance, granted.Instance);
    EXPECT_FALSE(adopted->DebugLayerActive)
        << "a sharing device must inherit the instance's resolved state, not re-derive its own request";
    EXPECT_TRUE(adopted->SwapchainColorSpaceExtEnabled);

    EXPECT_FALSE(registry.Release(granted.Instance).has_value()); // adopter
    EXPECT_TRUE(registry.Release(granted.Instance).has_value());  // publisher
}

TEST(SharedVulkanInstance, TwoKeysAreTrackedIndependently)
{
    SharedInstanceRegistry registry;
    SharedInstanceKey debug{};
    debug.ApplicationName = "Interleaved";
    debug.DebugLayer = true;
    SharedInstanceKey plain{};
    plain.ApplicationName = "Interleaved";

    EXPECT_FALSE(registry.Acquire(debug).has_value()) << "an empty registry has nothing to share";
    SharedInstanceState debugState{};
    debugState.Instance = FakeInstance(0x10);
    registry.Publish(debug, debugState);

    EXPECT_FALSE(registry.Acquire(plain).has_value())
        << "a key that differs anywhere must miss and create its own instance";
    SharedInstanceState plainState{};
    plainState.Instance = FakeInstance(0x20);
    registry.Publish(plain, plainState);

    const auto debugAgain = registry.Acquire(debug);
    ASSERT_TRUE(debugAgain.has_value());
    EXPECT_EQ(debugAgain->Instance, FakeInstance(0x10)) << "each key answers with its own instance";

    EXPECT_FALSE(registry.Release(FakeInstance(0x10)).has_value());
    const auto lastDebug = registry.Release(FakeInstance(0x10));
    ASSERT_TRUE(lastDebug.has_value());
    EXPECT_EQ(lastDebug->Instance, FakeInstance(0x10));
    EXPECT_FALSE(registry.Acquire(debug).has_value()) << "an erased entry must not be found again";

    const auto lastPlain = registry.Release(FakeInstance(0x20));
    ASSERT_TRUE(lastPlain.has_value())
        << "draining one entry must leave the other's reference count untouched";
    EXPECT_EQ(lastPlain->Instance, FakeInstance(0x20));
}

TEST(SharedVulkanInstance, ReleasingAnUnregisteredInstanceDestroysNothing)
{
    SharedInstanceRegistry registry;
    EXPECT_FALSE(registry.Release(FakeInstance(0x30)).has_value());
}

TEST(SharedVulkanInstance, TheLastReleaseIsTheOneThatDestroys)
{
    SharedInstanceRegistry registry;
    SharedInstanceKey key{};
    key.ApplicationName = "RefCount";
    SharedInstanceState state{};
    state.Instance = FakeInstance(0x40);

    registry.Publish(key, state);
    ASSERT_TRUE(registry.Acquire(key).has_value());
    ASSERT_TRUE(registry.Acquire(key).has_value());

    EXPECT_FALSE(registry.Release(state.Instance).has_value())
        << "an instance other devices still hold must not be destroyed";
    EXPECT_FALSE(registry.Release(state.Instance).has_value());
    const auto last = registry.Release(state.Instance);
    ASSERT_TRUE(last.has_value()) << "the device dropping the last reference has to be told to destroy";
    EXPECT_EQ(last->Instance, state.Instance);

    EXPECT_FALSE(registry.Acquire(key).has_value())
        << "the entry is erased at zero, so no device can adopt a destroyed instance";
    EXPECT_FALSE(registry.Release(state.Instance).has_value())
        << "and no second device can be handed the same instance to destroy";
}

TEST(SharedVulkanInstance, IdenticalConfigurationsShareOneInstance)
{
    auto first = MakeSharedInstanceProbeDevice("SharedInstanceProbe", /*debugLayer=*/false);
    if (!first)
        GTEST_SKIP() << "no Vulkan device";
    auto second = MakeSharedInstanceProbeDevice("SharedInstanceProbe", /*debugLayer=*/false);
    ASSERT_TRUE(second) << "a second device with an identical configuration must initialize";

    EXPECT_EQ(static_cast<VulkanDevice*>(first.get())->GetVkInstance(),
              static_cast<VulkanDevice*>(second.get())->GetVkInstance())
        << "devices whose instance would have been created identically must share one VkInstance";

    // The instance is reference-counted, not owned by its creator: dropping the
    // device that created it must leave the sharer with a working device.
    first.reset();
    EXPECT_TRUE(second->BeginFrame())
        << "the shared instance was torn down while a second device still held it";
    second->Present();
}

TEST(SharedVulkanInstance, ADebugRequestingDeviceArrivingSecondStillValidates)
{
    // Positive control, and it has to come first: on a machine with no validation
    // layer installed the probe below cannot fire for any reason, which is
    // indistinguishable from the defect this test exists for.
    {
        auto control = MakeSharedInstanceProbeDevice("SharedInstanceControl", /*debugLayer=*/true);
        if (!control)
            GTEST_SKIP() << "no Vulkan device";
        if (!ValidationErrorReachesTheStore(*control))
            GTEST_SKIP() << "validation layer not available on this machine";
    }

    auto plain = MakeSharedInstanceProbeDevice("SharedInstanceMixed", /*debugLayer=*/false);
    ASSERT_TRUE(plain) << "no Vulkan device";
    auto validating = MakeSharedInstanceProbeDevice("SharedInstanceMixed", /*debugLayer=*/true);
    ASSERT_TRUE(validating) << "a debug-layer device must initialize alongside a plain one";

    EXPECT_NE(static_cast<VulkanDevice*>(plain.get())->GetVkInstance(),
              static_cast<VulkanDevice*>(validating.get())->GetVkInstance())
        << "a debug-layer request cannot be satisfied on a non-debug instance, so the two must not "
           "share one";
    EXPECT_TRUE(ValidationErrorReachesTheStore(*validating))
        << "the device that asked for the debug layer is not validating — arriving second must not "
           "cost it the validation layer and the debug messenger";
}

TEST(SharedVulkanInstance, AVerboseRequestingDeviceDoesNotInheritATerseMessenger)
{
    // Both arms pin GE_VK_VALIDATION_VERBOSE rather than inheriting whatever the
    // shell had: with it already set, the two requests would be identical and the
    // assertion below would report a difference the test never created.
    std::unique_ptr<IDevice> terse;
    {
        ScopedEnvVar terseArm("GE_VK_VALIDATION_VERBOSE", "0");
        terse = MakeSharedInstanceProbeDevice("SharedInstanceVerbose", /*debugLayer=*/true);
    }
    if (!terse)
        GTEST_SKIP() << "no Vulkan device";

    ScopedEnvVar verboseArm("GE_VK_VALIDATION_VERBOSE", "1");
    auto verbose = MakeSharedInstanceProbeDevice("SharedInstanceVerbose", /*debugLayer=*/true);
    ASSERT_TRUE(verbose) << "a verbose-requesting device must initialize alongside a terse one";

    // The severity mask is fixed when the messenger is created and no API reads it
    // back, so the observable is which instance the second device landed on: its own
    // instance carries its own messenger, built from its own mask. This holds whether
    // or not the validation layer is installed, because the key states the request.
    EXPECT_NE(static_cast<VulkanDevice*>(terse.get())->GetVkInstance(),
              static_cast<VulkanDevice*>(verbose.get())->GetVkInstance())
        << "GE_VK_VALIDATION_VERBOSE reaches only the messenger created with the instance, so a "
           "verbose request cannot be satisfied by inheriting a terse one";
}

// --- GPU device-loss breadcrumbs (VK_NV_device_diagnostic_checkpoints) --------
// The CPU marker ring reports how far RECORDING got; GPU checkpoints report how
// far EXECUTION got. These cover the parts that are provable without a real GPU
// fault: the toggle policy, the payload-lifetime contract that makes a pointer
// still resolvable after a device loss, the post-loss formatter, and (device
// gated) extension detection plus actual recording.

TEST(GpuCheckpoints, TogglePolicyIsOffByDefault)
{
    // Opposite polarity to ParseDeviceRecoveryEnabled: unset means OFF, because
    // emission costs a GPU command on per-frame marker paths.
    EXPECT_FALSE(ParseGpuCheckpointsEnabled(nullptr));
    EXPECT_FALSE(ParseGpuCheckpointsEnabled(""));
    EXPECT_FALSE(ParseGpuCheckpointsEnabled("0"));
    EXPECT_FALSE(ParseGpuCheckpointsEnabled("false"));
    EXPECT_FALSE(ParseGpuCheckpointsEnabled("False"));
    EXPECT_TRUE(ParseGpuCheckpointsEnabled("1"));
    EXPECT_TRUE(ParseGpuCheckpointsEnabled("true"));
}

TEST(GpuCheckpoints, InternIsStableAndResolvesBack)
{
    GpuCheckpointNameTable names;
    EXPECT_EQ(names.RecordedCount(), 0u);
    EXPECT_EQ(names.DistinctNameCount(), 0u);

    const void* first = names.Intern("CBT.Classify");
    ASSERT_NE(first, nullptr);
    // Same name interns to the same payload — the table is a name set, not a log.
    EXPECT_EQ(names.Intern("CBT.Classify"), first);
    const void* second = names.Intern("CBT.Reduce");
    ASSERT_NE(second, nullptr);
    EXPECT_NE(second, first);

    EXPECT_EQ(names.RecordedCount(), 3u); // three markers, two distinct names
    EXPECT_EQ(names.DistinctNameCount(), 2u);

    std::string_view name;
    uint64_t ordinal = 0;
    ASSERT_TRUE(names.Describe(first, name, ordinal));
    EXPECT_EQ(name, "CBT.Classify");
    EXPECT_EQ(ordinal, 2u) << "should carry the ordinal of the most recent recording of this name";
    ASSERT_TRUE(names.Describe(second, name, ordinal));
    EXPECT_EQ(name, "CBT.Reduce");
    EXPECT_EQ(ordinal, 3u);

    // A null/empty name records nothing and emits nothing.
    EXPECT_EQ(names.Intern(nullptr), nullptr);
    EXPECT_EQ(names.Intern(""), nullptr);
    EXPECT_EQ(names.RecordedCount(), 3u);
}

// The payload-lifetime contract, which is the whole reason the table exists: a
// pointer handed to vkCmdSetCheckpointNV comes back from the driver AFTER the
// device is lost, so no later interning may ever move it. Fill well past the
// internal block size and re-resolve the very first payload.
TEST(GpuCheckpoints, PayloadPointersNeverMoveAsTheTableGrows)
{
    GpuCheckpointNameTable names;
    const void* firstPayload = names.Intern("First.Marker");
    ASSERT_NE(firstPayload, nullptr);

    std::vector<const void*> payloads;
    constexpr int kNames = 600; // several internal blocks' worth
    for (int i = 0; i < kNames; ++i)
    {
        const void* p = names.Intern(("Filler." + std::to_string(i)).c_str());
        ASSERT_NE(p, nullptr) << "intern failed at " << i;
        payloads.push_back(p);
    }

    std::string_view name;
    uint64_t ordinal = 0;
    ASSERT_TRUE(names.Describe(firstPayload, name, ordinal))
        << "the first payload stopped resolving — records moved, so a post-loss "
           "pointer would dangle";
    EXPECT_EQ(name, "First.Marker");
    EXPECT_EQ(ordinal, 1u);

    // Every payload handed out remains distinct and resolvable.
    for (int i = 0; i < kNames; ++i)
    {
        ASSERT_TRUE(names.Describe(payloads[i], name, ordinal)) << "payload " << i;
        EXPECT_EQ(name, "Filler." + std::to_string(i));
    }
    EXPECT_EQ(names.DistinctNameCount(), static_cast<size_t>(kNames) + 1);
    EXPECT_FALSE(names.Overflowed());
}

TEST(GpuCheckpoints, DescribeRejectsForeignPayloads)
{
    GpuCheckpointNameTable names;
    const void* payload = names.Intern("Real.Marker");
    ASSERT_NE(payload, nullptr);

    std::string_view name;
    uint64_t ordinal = 0;
    EXPECT_FALSE(names.Describe(nullptr, name, ordinal));
    int stackLocal = 0;
    EXPECT_FALSE(names.Describe(&stackLocal, name, ordinal));
    // Interior of a record is not a payload the table ever handed out; a driver
    // returning one must not be treated as a valid name.
    EXPECT_FALSE(names.Describe(static_cast<const char*>(payload) + 1, name, ordinal));
}

TEST(GpuCheckpoints, LongNamesTruncateAndStayResolvable)
{
    GpuCheckpointNameTable names;
    const std::string longName(kMaxGpuCheckpointNameLength + 40, 'x');
    const void* payload = names.Intern(longName.c_str());
    ASSERT_NE(payload, nullptr);

    std::string_view name;
    uint64_t ordinal = 0;
    ASSERT_TRUE(names.Describe(payload, name, ordinal));
    EXPECT_EQ(name.size(), kMaxGpuCheckpointNameLength - 1);
    // Truncation happens before the lookup, so names sharing a prefix past the
    // limit deliberately collapse onto one record rather than exhausting it.
    EXPECT_EQ(names.Intern((longName + "-suffix").c_str()), payload);
    EXPECT_EQ(names.DistinctNameCount(), 1u);
}

// Consequence of the truncation rule above, for the render graph's per-pass
// start/end bracket (RGRecord emits "<pass>" then "<pass>.end" so a device-loss
// dump can tell a pass that BEGAN from one that RETIRED). Appending the suffix
// to a long name and letting the table truncate would fold the end marker onto
// the start marker's record and destroy the distinction; reserving room for the
// suffix before truncating keeps them separate. RGRecord's
// BuildPassEndMarkerName implements the reserving form.
TEST(GpuCheckpoints, PassEndMarkerMustReserveSuffixRoomToStayDistinct)
{
    constexpr char kSuffix[] = ".end";
    constexpr size_t kSuffixLength = sizeof(kSuffix) - 1;
    const std::string longPass(kMaxGpuCheckpointNameLength + 12, 'p');

    {
        GpuCheckpointNameTable names;
        const void* start = names.Intern(longPass.c_str());
        ASSERT_NE(start, nullptr);
        // The trap: suffix first, truncate second — both names keep the same
        // leading bytes, so the table hands back one record for two markers.
        EXPECT_EQ(names.Intern((longPass + kSuffix).c_str()), start)
            << "naive concatenation is expected to collide; this pins the hazard";
        EXPECT_EQ(names.DistinctNameCount(), 1u);
    }
    {
        GpuCheckpointNameTable names;
        const void* start = names.Intern(longPass.c_str());
        ASSERT_NE(start, nullptr);
        const std::string reserved =
            longPass.substr(0, kMaxGpuCheckpointNameLength - 1 - kSuffixLength) + kSuffix;
        const void* end = names.Intern(reserved.c_str());
        ASSERT_NE(end, nullptr);
        EXPECT_NE(end, start) << "reserving suffix room must keep start and end distinct";
        EXPECT_EQ(names.DistinctNameCount(), 2u);

        std::string_view resolved;
        uint64_t ordinal = 0;
        ASSERT_TRUE(names.Describe(end, resolved, ordinal));
        EXPECT_EQ(resolved.size(), kMaxGpuCheckpointNameLength - 1)
            << "the reserved form must still fill the record exactly, not overrun it";
        EXPECT_NE(resolved.find(kSuffix), std::string_view::npos)
            << "the suffix must survive truncation; got: " << resolved;
    }
}

TEST(GpuCheckpoints, OverflowDegradesToASharedRecord)
{
    GpuCheckpointNameTable names;
    for (size_t i = 0; i < kMaxGpuCheckpointNames; ++i)
    {
        ASSERT_NE(names.Intern(("N" + std::to_string(i)).c_str()), nullptr);
    }
    EXPECT_FALSE(names.Overflowed());

    // Past the cap, emission still happens against a shared overflow record so a
    // checkpoint continues to prove execution reached a marker.
    const void* overflow = names.Intern("PastTheCap");
    ASSERT_NE(overflow, nullptr);
    EXPECT_TRUE(names.Overflowed());
    std::string_view name;
    uint64_t ordinal = 0;
    ASSERT_TRUE(names.Describe(overflow, name, ordinal));
    EXPECT_NE(name.find("full"), std::string_view::npos) << "got: " << name;
    EXPECT_EQ(names.Intern("AlsoPastTheCap"), overflow) << "overflow record is shared";
}

// The post-loss formatter, driven by a stub in place of the driver so the
// pointer round-trip and the ordering are provable without a GPU fault.
namespace
{
std::vector<const void*> g_StubPayloads;

void VKAPI_CALL StubGetQueueCheckpointData(VkQueue, uint32_t* pCount, VkCheckpointDataNV* pData)
{
    if (pData == nullptr)
    {
        *pCount = static_cast<uint32_t>(g_StubPayloads.size());
        return;
    }
    for (uint32_t i = 0; i < *pCount && i < g_StubPayloads.size(); ++i)
    {
        pData[i].stage = (i == 0) ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                  : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        pData[i].pCheckpointMarker = const_cast<void*>(g_StubPayloads[i]);
    }
}

void VKAPI_CALL StubGetNoCheckpointData(VkQueue, uint32_t* pCount, VkCheckpointDataNV*)
{
    *pCount = 0;
}
} // namespace

TEST(GpuCheckpoints, FormatterReportsNamesOldestFirst)
{
    GpuCheckpointNameTable names;
    const void* older = names.Intern("CBT.Classify");
    const void* newer = names.Intern("CBT.Reduce");
    ASSERT_NE(older, nullptr);
    ASSERT_NE(newer, nullptr);
    // Deliberately hand them back newest-first: the formatter must reorder to
    // oldest-first so it reads like the CPU marker ring.
    g_StubPayloads = {newer, older};

    // A synthetic non-null queue handle: the stub never dereferences it, and the
    // formatter only uses it for identity/dedup.
    VkQueue queues[1] = {reinterpret_cast<VkQueue>(0x1)};
    const char* queueNames[1] = {"graphics"};
    const VkPipelineStageFlags supported[1] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    const std::string report = FormatExecutedGpuCheckpoints(&StubGetQueueCheckpointData, queues,
                                                            queueNames, supported, 1, names);

    const size_t classifyAt = report.find("CBT.Classify");
    const size_t reduceAt = report.find("CBT.Reduce");
    ASSERT_NE(classifyAt, std::string::npos) << report;
    ASSERT_NE(reduceAt, std::string::npos) << report;
    EXPECT_LT(classifyAt, reduceAt) << "expected oldest-first ordering: " << report;
    EXPECT_NE(report.find("COMPUTE_SHADER"), std::string::npos) << report;
    EXPECT_NE(report.find("graphics"), std::string::npos) << report;
    EXPECT_NE(report.find("recorded 2 marker(s)"), std::string::npos) << report;
    g_StubPayloads.clear();
}

TEST(GpuCheckpoints, FormatterHandlesEmptyAndUnavailableCases)
{
    GpuCheckpointNameTable names;
    VkQueue queues[1] = {reinterpret_cast<VkQueue>(0x1)};
    const char* queueNames[1] = {"graphics"};
    const VkPipelineStageFlags supported[1] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};

    // No entry point (extension absent) — never an empty log line.
    EXPECT_NE(
        FormatExecutedGpuCheckpoints(nullptr, queues, queueNames, supported, 1, names).find("unavailable"),
        std::string::npos);
    // All queue handles null.
    VkQueue nullQueues[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    const char* nullNames[2] = {"graphics", "compute"};
    const VkPipelineStageFlags nullStages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    EXPECT_NE(FormatExecutedGpuCheckpoints(&StubGetQueueCheckpointData, nullQueues, nullNames,
                                           nullStages, 2, names)
                  .find("no queues"),
              std::string::npos);
}

// The distinction this exists for: an empty result from a family that CAN report
// checkpoints means the driver returned no outstanding markers, while an empty
// result from a family that cannot report them at all means the instrument is
// blind. Collapsing both to one string is what let `compute: <none>` read as a
// finding. Neither string may claim nothing EXECUTED — the query reports
// outstanding work, so a cleanly drained queue also returns nothing.
TEST(GpuCheckpoints, FormatterSeparatesAnUnsupportedFamilyFromAnIdleOne)
{
    GpuCheckpointNameTable names;
    VkQueue queues[1] = {reinterpret_cast<VkQueue>(0x1)};
    const char* queueNames[1] = {"compute"};

    const VkPipelineStageFlags supported[1] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    const std::string idle =
        FormatExecutedGpuCheckpoints(&StubGetNoCheckpointData, queues, queueNames, supported, 1, names);
    EXPECT_NE(idle.find("no outstanding checkpoints reported"), std::string::npos) << idle;
    EXPECT_EQ(idle.find("unsupported"), std::string::npos) << idle;
    EXPECT_EQ(idle.find("executed"), std::string::npos)
        << "an empty result must not assert an execution fact the query cannot support: " << idle;

    const VkPipelineStageFlags unsupported[1] = {0};
    const std::string blind =
        FormatExecutedGpuCheckpoints(&StubGetNoCheckpointData, queues, queueNames, unsupported, 1, names);
    EXPECT_NE(blind.find("unsupported on this queue family"), std::string::npos) << blind;
    EXPECT_EQ(blind.find("no outstanding checkpoints reported"), std::string::npos) << blind;

    // The two states must not render alike — that equality is the whole defect.
    EXPECT_NE(idle, blind);
}

// An unsupported family must be reported as blind even when the driver would have
// returned data, because recording into it was never valid in the first place.
TEST(GpuCheckpoints, AnUnsupportedFamilyIsReportedBlindWithoutBeingQueried)
{
    GpuCheckpointNameTable names;
    const void* payload = names.Intern("Should.Not.Appear");
    ASSERT_NE(payload, nullptr);
    g_StubPayloads = {payload};

    VkQueue queues[1] = {reinterpret_cast<VkQueue>(0x1)};
    const char* queueNames[1] = {"transfer"};
    const VkPipelineStageFlags unsupported[1] = {0};
    const std::string report = FormatExecutedGpuCheckpoints(&StubGetQueueCheckpointData, queues,
                                                            queueNames, unsupported, 1, names);
    EXPECT_NE(report.find("unsupported on this queue family"), std::string::npos) << report;
    EXPECT_EQ(report.find("Should.Not.Appear"), std::string::npos) << report;
    g_StubPayloads.clear();
}

// A mixed device is the realistic case: graphics reports, compute is blind. Both
// must appear, each labelled with its own state.
TEST(GpuCheckpoints, MixedSupportLabelsEachQueueIndependently)
{
    GpuCheckpointNameTable names;
    const void* payload = names.Intern("Pipeline.ForwardPlus");
    ASSERT_NE(payload, nullptr);
    g_StubPayloads = {payload};

    VkQueue queues[2] = {reinterpret_cast<VkQueue>(0x1), reinterpret_cast<VkQueue>(0x2)};
    const char* queueNames[2] = {"graphics", "compute"};
    const VkPipelineStageFlags stages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0};
    const std::string report =
        FormatExecutedGpuCheckpoints(&StubGetQueueCheckpointData, queues, queueNames, stages, 2, names);

    EXPECT_NE(report.find("Pipeline.ForwardPlus"), std::string::npos) << report;
    EXPECT_NE(report.find("compute: <unsupported"), std::string::npos) << report;
    g_StubPayloads.clear();
}

TEST(GpuCheckpoints, FormatterDeduplicatesAliasedQueues)
{
    GpuCheckpointNameTable names;
    const void* payload = names.Intern("Only.Marker");
    ASSERT_NE(payload, nullptr);
    g_StubPayloads = {payload};

    // Present commonly aliases graphics; the same queue must be reported once.
    VkQueue queues[2] = {reinterpret_cast<VkQueue>(0x1), reinterpret_cast<VkQueue>(0x1)};
    const char* queueNames[2] = {"graphics", "present"};
    const VkPipelineStageFlags stages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    const std::string report = FormatExecutedGpuCheckpoints(&StubGetQueueCheckpointData, queues,
                                                            queueNames, stages, 2, names);
    EXPECT_NE(report.find("graphics"), std::string::npos) << report;
    EXPECT_EQ(report.find("present"), std::string::npos)
        << "aliased queue reported twice: " << report;
    g_StubPayloads.clear();
}

// Device-gated: the runtime-support probe. Machine-independent oracle — whatever
// the physical device advertises, the device must have ENABLED and RESOLVED it.
// This is what catches "enumerated but never wired".
TEST(GpuCheckpoints, RuntimeSupportMatchesPhysicalDeviceAdvertisement)
{
    SetEnvHeadless();
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    if (!dev->Initialize(dd))
    {
        GTEST_SKIP() << "no Vulkan device available";
    }
    auto* vk = static_cast<VulkanDevice*>(dev.get());

    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(vk->GetVkPhysicalDevice(), nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> available(extensionCount);
    vkEnumerateDeviceExtensionProperties(vk->GetVkPhysicalDevice(), nullptr, &extensionCount,
                                         available.data());
    bool advertised = false;
    for (const auto& e : available)
    {
        if (std::strcmp(e.extensionName, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME) == 0)
        {
            advertised = true;
        }
    }

    // Printed so the probe result is visible in the run log on any machine.
    std::cout << "[ probe    ] VK_NV_device_diagnostic_checkpoints advertised="
              << (advertised ? "yes" : "no")
              << " available=" << (vk->GpuCheckpointsAvailable() ? "yes" : "no") << std::endl;
    EXPECT_EQ(vk->GpuCheckpointsAvailable(), advertised)
        << "the device must enable and resolve the extension exactly when the "
           "physical device advertises it. Advertised-but-unavailable also means "
           "vkCreateDevice took its minimal fallback path (presentation "
           "extensions only) or the driver failed to resolve the entry points — "
           "check the startup log before assuming a wiring bug";
    // Emission stays off without the env var, whatever the support situation.
    EXPECT_FALSE(vk->GpuCheckpointsEnabled());
    EXPECT_EQ(vk->DebugGpuCheckpointsRecorded(), 0u);

    dev->Shutdown();
}

// Device-gated: which queue families can actually report checkpoints. The
// extension being present says nothing about per-family support, and a family
// reporting no stages is blind — an empty checkpoint result from one of its queues
// is an instrument limit, not a finding. Printed so the answer is in the run log on
// whatever adapter this executes on.
TEST(GpuCheckpoints, PerQueueFamilyCheckpointSupportIsProbedAndReported)
{
    SetEnvHeadless();
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    if (!dev->Initialize(dd))
    {
        GTEST_SKIP() << "no Vulkan device available";
    }
    auto* vk = static_cast<VulkanDevice*>(dev.get());

    const uint32_t graphicsFamily = vk->GetGraphicsQueueFamilyIndex();
    const uint32_t computeFamily = vk->GetComputeQueueFamilyIndex();
    const uint32_t transferFamily = vk->GetTransferQueueFamilyIndex();

    std::cout << "[ probe    ] checkpoint stage masks: graphics(family " << graphicsFamily << ")=0x"
              << std::hex << vk->CheckpointStagesForFamily(graphicsFamily) << ", compute(family "
              << std::dec << computeFamily << ")=0x" << std::hex
              << vk->CheckpointStagesForFamily(computeFamily) << ", transfer(family " << std::dec
              << transferFamily << ")=0x" << std::hex << vk->CheckpointStagesForFamily(transferFamily)
              << std::dec << std::endl;

    if (!vk->GpuCheckpointsAvailable())
    {
        // Unprobed devices report 0 everywhere, which the formatter renders as
        // "blind" — correct, since retrieval is genuinely unavailable.
        EXPECT_EQ(vk->CheckpointStagesForFamily(graphicsFamily), 0u);
        dev->Shutdown();
        GTEST_SKIP() << "VK_NV_device_diagnostic_checkpoints unavailable on this adapter";
    }

    // The oracle: an adapter that advertises the extension must report checkpoint
    // stages on the family the engine records its own markers into. A zero here
    // means we have been emitting vkCmdSetCheckpointNV into a family that cannot
    // report it, and every graphics breadcrumb we have ever trusted is suspect.
    EXPECT_NE(vk->CheckpointStagesForFamily(graphicsFamily), 0u)
        << "graphics family advertises no checkpoint stages while the extension is enabled";

    dev->Shutdown();
}

// Device-gated: with emission armed, a marker on a recording command list must
// actually record a checkpoint.
TEST(GpuCheckpoints, EnabledEmissionRecordsCheckpointsForMarkers)
{
    SetEnvHeadless();
#if defined(_WIN32)
    _putenv_s("GE_VK_GPU_CHECKPOINTS", "1");
#else
    setenv("GE_VK_GPU_CHECKPOINTS", "1", 1);
#endif
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    const bool initialized = dev->Initialize(dd);
    auto* vk = initialized ? static_cast<VulkanDevice*>(dev.get()) : nullptr;
    const bool usable = vk != nullptr && vk->GpuCheckpointsAvailable();
    if (usable)
    {
        EXPECT_TRUE(vk->GpuCheckpointsEnabled());
        ASSERT_TRUE(dev->BeginFrame());
        auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
        ASSERT_TRUE(cl != nullptr);
        cl->Begin();
        cl->SetMarker("Test.Checkpoint.A");
        cl->SetMarker("Test.Checkpoint.B");
        cl->SetMarker("Test.Checkpoint.A"); // repeat: 3 markers, 2 distinct names
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        dev->ExecuteCommandLists(lists);
        EXPECT_EQ(vk->DebugGpuCheckpointsRecorded(), 3u);
        dev->Present();
    }
#if defined(_WIN32)
    _putenv_s("GE_VK_GPU_CHECKPOINTS", "");
#else
    unsetenv("GE_VK_GPU_CHECKPOINTS");
#endif
    if (initialized)
    {
        dev->Shutdown();
    }
    if (!usable)
    {
        GTEST_SKIP() << "no Vulkan device with VK_NV_device_diagnostic_checkpoints";
    }
}

// --- Synchronization validation toggle (GE_VK_SYNC_VALIDATION) ---------------
// Sync-val is the instrument for cross-queue hazards, and it is expensive enough
// that "inert unless asked" is the property worth pinning: these cover the toggle
// policy, that the off arm leaves the pre-existing layer settings byte-identical,
// and that the on arm appends exactly one well-formed entry.

TEST(SyncValidation, TogglePolicyIsOffByDefault)
{
    // Same polarity and idiom as ParseGpuCheckpointsEnabled: unset means OFF,
    // because sync-val costs multiples of the frame time.
    EXPECT_FALSE(ParseSyncValidationEnabled(nullptr));
    EXPECT_FALSE(ParseSyncValidationEnabled(""));
    EXPECT_FALSE(ParseSyncValidationEnabled("0"));
    EXPECT_FALSE(ParseSyncValidationEnabled("false"));
    EXPECT_FALSE(ParseSyncValidationEnabled("False"));
    EXPECT_TRUE(ParseSyncValidationEnabled("1"));
    EXPECT_TRUE(ParseSyncValidationEnabled("true"));
    EXPECT_TRUE(ParseSyncValidationEnabled("on"));
}

namespace {

constexpr const char* kTestLayerName = "VK_LAYER_KHRONOS_validation";

const VkLayerSettingEXT* FindSetting(const ValidationLayerSettings& settings, const char* name)
{
    for (uint32_t i = 0; i < settings.Count(); ++i)
    {
        const VkLayerSettingEXT& setting = settings.Data()[i];
        if (setting.pSettingName && std::strcmp(setting.pSettingName, name) == 0)
            return &setting;
    }
    return nullptr;
}

} // namespace

// The no-op-when-off contract. Not just "sync-val absent" — the entry that was
// there before this toggle existed must be untouched, so a normal validation run
// sends the layer exactly what it always did.
TEST(SyncValidation, OffArmLeavesTheExistingLayerSettingsUntouched)
{
    const ValidationLayerSettings settings(kTestLayerName, ValidationLayerSettingsDesc{});

    EXPECT_FALSE(settings.SyncValidationEnabled());
    EXPECT_EQ(settings.Count(), 1u) << "the off arm must send exactly the pre-existing setting";
    EXPECT_EQ(FindSetting(settings, kSyncValidationSettingName), nullptr);

    const VkLayerSettingEXT* dedup = FindSetting(settings, "duplicate_message_limit");
    ASSERT_NE(dedup, nullptr);
    EXPECT_STREQ(dedup->pLayerName, kTestLayerName);
    EXPECT_EQ(dedup->type, VK_LAYER_SETTING_TYPE_UINT32_EXT);
    EXPECT_EQ(dedup->valueCount, 1u);
    ASSERT_NE(dedup->pValues, nullptr);
    EXPECT_EQ(*static_cast<const uint32_t*>(dedup->pValues), kDuplicateMessageLimitUnlimited);
}

TEST(SyncValidation, OnArmAppendsExactlyTheSyncSettingToTheSameArray)
{
    const ValidationLayerSettings off(kTestLayerName, ValidationLayerSettingsDesc{});
    const ValidationLayerSettings on(kTestLayerName, ValidationLayerSettingsDesc{/*SyncValidation=*/true});

    EXPECT_TRUE(on.SyncValidationEnabled());
    EXPECT_EQ(on.Count(), off.Count() + 1) << "additive: one chain, one extra entry";

    // The pre-existing entry survives the append unchanged, field for field.
    const VkLayerSettingEXT* dedupOff = FindSetting(off, "duplicate_message_limit");
    const VkLayerSettingEXT* dedupOn = FindSetting(on, "duplicate_message_limit");
    ASSERT_NE(dedupOff, nullptr);
    ASSERT_NE(dedupOn, nullptr);
    EXPECT_STREQ(dedupOn->pLayerName, dedupOff->pLayerName);
    EXPECT_EQ(dedupOn->type, dedupOff->type);
    EXPECT_EQ(dedupOn->valueCount, dedupOff->valueCount);
    EXPECT_EQ(*static_cast<const uint32_t*>(dedupOn->pValues),
              *static_cast<const uint32_t*>(dedupOff->pValues));

    const VkLayerSettingEXT* sync = FindSetting(on, kSyncValidationSettingName);
    ASSERT_NE(sync, nullptr) << "sync-val requested but no validate_sync entry was chained";
    EXPECT_STREQ(sync->pSettingName, "validate_sync");
    EXPECT_STREQ(sync->pLayerName, kTestLayerName);
    EXPECT_EQ(sync->type, VK_LAYER_SETTING_TYPE_BOOL32_EXT);
    EXPECT_EQ(sync->valueCount, 1u);
    ASSERT_NE(sync->pValues, nullptr);
    EXPECT_EQ(*static_cast<const VkBool32*>(sync->pValues), VK_TRUE);
}

// The layer dereferences pValues inside vkCreateInstance, so every value pointer
// has to address storage the settings object owns, and the object must not be
// relocatable — a copy's entries would still point at the original.
TEST(SyncValidation, SettingValuesLiveInsideTheOwningObject)
{
    static_assert(!std::is_copy_constructible_v<ValidationLayerSettings>,
                  "copying would leave the copy's pValues addressing the original");
    static_assert(!std::is_move_constructible_v<ValidationLayerSettings>,
                  "moving would leave the moved-to object's pValues addressing the source");

    const ValidationLayerSettings settings(kTestLayerName,
                                          ValidationLayerSettingsDesc{/*SyncValidation=*/true, /*GpuAssistedValidation=*/true});
    const auto* base = reinterpret_cast<const unsigned char*>(&settings);
    for (uint32_t i = 0; i < settings.Count(); ++i)
    {
        const auto* value = reinterpret_cast<const unsigned char*>(settings.Data()[i].pValues);
        EXPECT_GE(value, base) << "setting " << i << " points outside the object";
        EXPECT_LT(value, base + sizeof(ValidationLayerSettings)) << "setting " << i;
    }
}

// --- GPU-assisted validation toggle (GE_VK_GPU_AV) ---------------------------
// The half of GPU-AV that survives VK_EXT_descriptor_buffer is buffer-content
// validation; shader instrumentation is force-disabled by the layer whenever the
// extension is enabled. These pin the toggle policy and that the request the
// engine sends matches what it claims in the log.

TEST(GpuAssistedValidation, TogglePolicyIsOffByDefault)
{
    EXPECT_FALSE(ParseGpuAssistedValidationEnabled(nullptr));
    EXPECT_FALSE(ParseGpuAssistedValidationEnabled(""));
    EXPECT_FALSE(ParseGpuAssistedValidationEnabled("0"));
    EXPECT_FALSE(ParseGpuAssistedValidationEnabled("false"));
    EXPECT_FALSE(ParseGpuAssistedValidationEnabled("F"));
    EXPECT_TRUE(ParseGpuAssistedValidationEnabled("1"));
    EXPECT_TRUE(ParseGpuAssistedValidationEnabled("on"));
}

TEST(GpuAssistedValidation, OffArmChainsNoGpuAvSettings)
{
    const ValidationLayerSettings settings(kTestLayerName, ValidationLayerSettingsDesc{});

    EXPECT_FALSE(settings.GpuAssistedValidationEnabled());
    EXPECT_EQ(FindSetting(settings, kGpuAvEnableSettingName), nullptr);
    EXPECT_EQ(FindSetting(settings, kGpuAvShaderInstrumentationSettingName), nullptr);
    EXPECT_EQ(FindSetting(settings, kGpuAvBuffersValidationSettingName), nullptr);
    EXPECT_EQ(FindSetting(settings, kGpuAvIndirectDrawsBuffersSettingName), nullptr);
    EXPECT_EQ(FindSetting(settings, kGpuAvIndirectDispatchesBuffersSettingName), nullptr);
    EXPECT_EQ(FindSetting(settings, kGpuAvIndirectTraceRaysBuffersSettingName), nullptr);
    EXPECT_EQ(FindSetting(settings, kGpuAvBufferCopiesSettingName), nullptr);
    EXPECT_EQ(FindSetting(settings, kGpuAvIndexBuffersSettingName), nullptr);
    EXPECT_EQ(FindSetting(settings, kGpuAvDescriptorChecksSettingName), nullptr);
    EXPECT_EQ(FindSetting(settings, kGpuAvPostProcessDescriptorIndexingSettingName), nullptr);
    EXPECT_EQ(FindSetting(settings, kGpuAvBufferAddressOobSettingName), nullptr);
}

// GE_VK_GPU_AV on its own asks for buffer checks and explicitly asks
// instrumentation OFF. The explicit VK_FALSE matters: the layer's own default for
// gpuav_shader_instrumentation is TRUE (manifest, SDK 1.4.321.0), so omitting the
// entry would arm instrumentation by accident under a descriptor-buffer device.
// Turning it on is a separate decision — see the ShaderInstrumentation suite.
TEST(GpuAssistedValidation, OnArmRequestsBufferChecksAndDefaultsShaderInstrumentationOff)
{
    const ValidationLayerSettings off(kTestLayerName, ValidationLayerSettingsDesc{});
    const ValidationLayerSettings on(kTestLayerName,
                                     ValidationLayerSettingsDesc{/*SyncValidation=*/false,
                                                                 /*GpuAssistedValidation=*/true});

    EXPECT_TRUE(on.GpuAssistedValidationEnabled());
    EXPECT_EQ(on.Count(), off.Count() + 8)
        << "additive: enable + instrumentation + buffers + its five individual checks";

    const VkLayerSettingEXT* enable = FindSetting(on, kGpuAvEnableSettingName);
    ASSERT_NE(enable, nullptr) << "GPU-AV requested but no gpuav_enable entry was chained";
    EXPECT_STREQ(enable->pSettingName, "gpuav_enable");
    EXPECT_STREQ(enable->pLayerName, kTestLayerName);
    EXPECT_EQ(enable->type, VK_LAYER_SETTING_TYPE_BOOL32_EXT);
    EXPECT_EQ(enable->valueCount, 1u);
    ASSERT_NE(enable->pValues, nullptr);
    EXPECT_EQ(*static_cast<const VkBool32*>(enable->pValues), VK_TRUE);

    const VkLayerSettingEXT* instrumentation =
        FindSetting(on, kGpuAvShaderInstrumentationSettingName);
    ASSERT_NE(instrumentation, nullptr);
    EXPECT_STREQ(instrumentation->pSettingName, "gpuav_shader_instrumentation");
    ASSERT_NE(instrumentation->pValues, nullptr);
    EXPECT_EQ(*static_cast<const VkBool32*>(instrumentation->pValues), VK_FALSE)
        << "the layer defaults this to true; GE_VK_GPU_AV alone must still ask it off";

    // The three instrumentation sub-checks are meaningless with the parent off and
    // must not be chained — the layer reads them only when both parents are true.
    for (const char* check : {kGpuAvDescriptorChecksSettingName,
                              kGpuAvPostProcessDescriptorIndexingSettingName,
                              kGpuAvBufferAddressOobSettingName})
    {
        EXPECT_EQ(FindSetting(on, check), nullptr) << check << " chained without its parent";
    }

    const VkLayerSettingEXT* buffers = FindSetting(on, kGpuAvBuffersValidationSettingName);
    ASSERT_NE(buffers, nullptr);
    EXPECT_STREQ(buffers->pSettingName, "gpuav_buffers_validation");
    ASSERT_NE(buffers->pValues, nullptr);
    EXPECT_EQ(*static_cast<const VkBool32*>(buffers->pValues), VK_TRUE);

    // The parent alone does not pin the family: with it true the layer reads each
    // of these five and otherwise leaves them at their own defaults, so a flip on
    // any one would empty exactly the check being relied on while the parent still
    // reported the family enabled.
    for (const char* check : {kGpuAvIndirectDrawsBuffersSettingName,
                              kGpuAvIndirectDispatchesBuffersSettingName,
                              kGpuAvIndirectTraceRaysBuffersSettingName,
                              kGpuAvBufferCopiesSettingName,
                              kGpuAvIndexBuffersSettingName})
    {
        const VkLayerSettingEXT* sub = FindSetting(on, check);
        ASSERT_NE(sub, nullptr) << check << " was not chained; a default flip could empty it silently";
        EXPECT_STREQ(sub->pLayerName, kTestLayerName) << check;
        EXPECT_EQ(sub->type, VK_LAYER_SETTING_TYPE_BOOL32_EXT) << check;
        EXPECT_EQ(sub->valueCount, 1u) << check;
        ASSERT_NE(sub->pValues, nullptr) << check;
        EXPECT_EQ(*static_cast<const VkBool32*>(sub->pValues), VK_TRUE) << check;
    }

    // The pre-existing entry survives the append unchanged.
    const VkLayerSettingEXT* dedup = FindSetting(on, "duplicate_message_limit");
    ASSERT_NE(dedup, nullptr);
    EXPECT_EQ(*static_cast<const uint32_t*>(dedup->pValues), kDuplicateMessageLimitUnlimited);
}

// Both families at once must fit the fixed array and stay independent.
TEST(GpuAssistedValidation, SyncAndGpuAvComposeWithoutOverflowingTheSettingArray)
{
    const ValidationLayerSettings both(kTestLayerName,
                                       ValidationLayerSettingsDesc{/*SyncValidation=*/true,
                                                                   /*GpuAssistedValidation=*/true});

    EXPECT_TRUE(both.SyncValidationEnabled());
    EXPECT_TRUE(both.GpuAssistedValidationEnabled());
    EXPECT_EQ(both.Count(), 10u);
    EXPECT_NE(FindSetting(both, kSyncValidationSettingName), nullptr);
    EXPECT_NE(FindSetting(both, kGpuAvEnableSettingName), nullptr);
}

// --- GPU-AV shader instrumentation ------------------------------------------
// The half that descriptor buffers cost us: descriptor-indexing and
// buffer-device-address out-of-bounds. Two independent gates decide whether it
// runs — the layer's (it drops instrumentation while VK_EXT_descriptor_buffer is
// enabled) and this engine's (what it asks for). These pin the engine's gate and
// the reporting that tells the two apart, because a run that finds nothing means
// three different things depending on which gate stopped it.

TEST(GpuAvShaderInstrumentation, ModeDefaultsToAutoAndParsesBothOverrides)
{
    using Mode = GpuAvShaderInstrumentationMode;

    // Unset is Auto, so GE_VK_GPU_AV keeps deciding on its own.
    EXPECT_EQ(ParseGpuAvShaderInstrumentationMode(nullptr), Mode::Auto);
    EXPECT_EQ(ParseGpuAvShaderInstrumentationMode(""), Mode::Auto);
    EXPECT_EQ(ParseGpuAvShaderInstrumentationMode("auto"), Mode::Auto);
    EXPECT_EQ(ParseGpuAvShaderInstrumentationMode("A"), Mode::Auto);

    EXPECT_EQ(ParseGpuAvShaderInstrumentationMode("0"), Mode::ForceOff);
    EXPECT_EQ(ParseGpuAvShaderInstrumentationMode("false"), Mode::ForceOff);
    EXPECT_EQ(ParseGpuAvShaderInstrumentationMode("F"), Mode::ForceOff);

    EXPECT_EQ(ParseGpuAvShaderInstrumentationMode("1"), Mode::ForceOn);
    EXPECT_EQ(ParseGpuAvShaderInstrumentationMode("on"), Mode::ForceOn);
    EXPECT_EQ(ParseGpuAvShaderInstrumentationMode("true"), Mode::ForceOn);
}

// The engine's gate, stated as a table. Auto keys on whether the extension can
// still be enabled — instance creation cannot know that it WILL be, only that it
// cannot be, which is the only direction that is sound that early.
TEST(GpuAvShaderInstrumentation, AutoRequestsItExactlyWhenDescriptorBufferIsRuledOut)
{
    using Mode = GpuAvShaderInstrumentationMode;

    EXPECT_TRUE(ShouldRequestGpuAvShaderInstrumentation(
        /*gpuAvEnabled=*/true, /*descriptorBufferPossible=*/false, Mode::Auto));
    EXPECT_FALSE(ShouldRequestGpuAvShaderInstrumentation(
        /*gpuAvEnabled=*/true, /*descriptorBufferPossible=*/true, Mode::Auto));
}

// Instrumentation is a child of gpuav_enable in the layer's dependency tree, so
// asking for it without GPU-AV would be an inert request that still reads as one.
TEST(GpuAvShaderInstrumentation, IsNeverRequestedWithoutGpuAvItself)
{
    using Mode = GpuAvShaderInstrumentationMode;

    for (Mode mode : {Mode::Auto, Mode::ForceOn, Mode::ForceOff})
    {
        for (bool possible : {false, true})
        {
            EXPECT_FALSE(ShouldRequestGpuAvShaderInstrumentation(
                /*gpuAvEnabled=*/false, possible, mode))
                << "mode " << static_cast<int>(mode) << " possible " << possible;
        }
    }
}

// Both control arms of the diagnostic matrix have to be reachable: instrumentation
// asked for WITH the extension (proves the layer's gate) and withheld WITHOUT it
// (proves this engine's). Auto alone yields neither.
TEST(GpuAvShaderInstrumentation, OverridesReachBothControlArms)
{
    using Mode = GpuAvShaderInstrumentationMode;

    // Arm A: extension possible, still requested.
    EXPECT_TRUE(ShouldRequestGpuAvShaderInstrumentation(
        /*gpuAvEnabled=*/true, /*descriptorBufferPossible=*/true, Mode::ForceOn));
    // Arm B: extension ruled out, deliberately withheld.
    EXPECT_FALSE(ShouldRequestGpuAvShaderInstrumentation(
        /*gpuAvEnabled=*/true, /*descriptorBufferPossible=*/false, Mode::ForceOff));
}

TEST(GpuAvShaderInstrumentation, RequestChainsTheSettingOnWithItsThreeSubChecks)
{
    const ValidationLayerSettings off(kTestLayerName, ValidationLayerSettingsDesc{});
    const ValidationLayerSettings on(kTestLayerName,
                                     ValidationLayerSettingsDesc{/*SyncValidation=*/false,
                                                                 /*GpuAssistedValidation=*/true,
                                                                 /*GpuAvShaderInstrumentation=*/true});

    EXPECT_TRUE(on.GpuAvShaderInstrumentationRequested());
    EXPECT_EQ(on.Count(), off.Count() + 11)
        << "enable + instrumentation + its three checks + buffers + its five checks";

    const VkLayerSettingEXT* instrumentation =
        FindSetting(on, kGpuAvShaderInstrumentationSettingName);
    ASSERT_NE(instrumentation, nullptr);
    EXPECT_STREQ(instrumentation->pSettingName, "gpuav_shader_instrumentation");
    ASSERT_NE(instrumentation->pValues, nullptr);
    EXPECT_EQ(*static_cast<const VkBool32*>(instrumentation->pValues), VK_TRUE)
        << "instrumentation was requested but the chained value still asks the layer for off";

    // Pinned individually: the layer's manifest gives each of these `dependence` on
    // gpuav_enable AND gpuav_shader_instrumentation with mode ALL, and otherwise
    // leaves them at their own defaults. A default flip would empty exactly the
    // coverage the request exists for while the parent still reported it on.
    for (const char* check : {kGpuAvDescriptorChecksSettingName,
                              kGpuAvPostProcessDescriptorIndexingSettingName,
                              kGpuAvBufferAddressOobSettingName})
    {
        const VkLayerSettingEXT* sub = FindSetting(on, check);
        ASSERT_NE(sub, nullptr) << check << " was not chained; a default flip could empty it silently";
        EXPECT_STREQ(sub->pLayerName, kTestLayerName) << check;
        EXPECT_EQ(sub->type, VK_LAYER_SETTING_TYPE_BOOL32_EXT) << check;
        EXPECT_EQ(sub->valueCount, 1u) << check;
        ASSERT_NE(sub->pValues, nullptr) << check;
        EXPECT_EQ(*static_cast<const VkBool32*>(sub->pValues), VK_TRUE) << check;
    }

    // The buffer-content half is unaffected by the instrumentation decision.
    const VkLayerSettingEXT* buffers = FindSetting(on, kGpuAvBuffersValidationSettingName);
    ASSERT_NE(buffers, nullptr);
    EXPECT_EQ(*static_cast<const VkBool32*>(buffers->pValues), VK_TRUE);
}

// Everything at once still fits the fixed array — the assert in Append is the only
// thing standing between a new setting and a silent out-of-bounds write.
TEST(GpuAvShaderInstrumentation, EveryFamilyAtOnceFitsTheSettingArray)
{
    const ValidationLayerSettings all(kTestLayerName,
                                      ValidationLayerSettingsDesc{/*SyncValidation=*/true,
                                                                  /*GpuAssistedValidation=*/true,
                                                                  /*GpuAvShaderInstrumentation=*/true});

    EXPECT_EQ(all.Count(), 13u);

    // Same self-referential-storage contract as the sync-val case: the layer
    // dereferences pValues inside vkCreateInstance.
    const auto* base = reinterpret_cast<const unsigned char*>(&all);
    for (uint32_t i = 0; i < all.Count(); ++i)
    {
        const auto* value = reinterpret_cast<const unsigned char*>(all.Data()[i].pValues);
        EXPECT_GE(value, base) << "setting " << i << " points outside the object";
        EXPECT_LT(value, base + sizeof(ValidationLayerSettings)) << "setting " << i;
    }
}

// --- The three reported states ----------------------------------------------
// "We did not ask" and "the layer refused" are different facts. Collapsing them is
// how an instrument that was never armed gets read as a clean result, which is the
// specific failure this reporting exists to prevent.

TEST(GpuAvShaderInstrumentation, StateDistinguishesOurGateFromTheLayers)
{
    using State = GpuAvShaderInstrumentationState;

    // Not requested — our gate, whatever the extension did.
    EXPECT_EQ(ClassifyGpuAvShaderInstrumentation(/*requested=*/false, /*descriptorBufferEnabled=*/false),
              State::NotRequested);
    EXPECT_EQ(ClassifyGpuAvShaderInstrumentation(/*requested=*/false, /*descriptorBufferEnabled=*/true),
              State::NotRequested);

    // Requested and the extension is enabled — the layer's gate.
    EXPECT_EQ(ClassifyGpuAvShaderInstrumentation(/*requested=*/true, /*descriptorBufferEnabled=*/true),
              State::RequestedButUnavailable);

    // Requested with the extension out of the way — the only state that checks.
    EXPECT_EQ(ClassifyGpuAvShaderInstrumentation(/*requested=*/true, /*descriptorBufferEnabled=*/false),
              State::Active);
}

// Each arm of the matrix has to be matchable as a string rather than judged, and
// no arm's text may be a substring of another's or a grep would report two hits.
TEST(GpuAvShaderInstrumentation, EachStateReportsADistinctGreppableString)
{
    using State = GpuAvShaderInstrumentationState;

    const std::string active = DescribeGpuAvShaderInstrumentation(State::Active);
    const std::string unavailable = DescribeGpuAvShaderInstrumentation(State::RequestedButUnavailable);
    const std::string notRequested = DescribeGpuAvShaderInstrumentation(State::NotRequested);

    EXPECT_NE(active.find("ACTIVE"), std::string::npos);
    EXPECT_NE(unavailable.find("REQUESTED-BUT-UNAVAILABLE"), std::string::npos);
    EXPECT_NE(notRequested.find("NOT REQUESTED"), std::string::npos);

    // The Active line is the one a run is read against, so it must not be
    // reachable by grepping for either negative state's marker.
    EXPECT_EQ(active.find("REQUESTED-BUT-UNAVAILABLE"), std::string::npos);
    EXPECT_EQ(active.find("NOT REQUESTED"), std::string::npos);
    EXPECT_EQ(unavailable.find("NOT REQUESTED"), std::string::npos);

    // "ACTIVE" must not appear in either negative state — that is the string the
    // matrix is read with.
    EXPECT_EQ(unavailable.find("ACTIVE"), std::string::npos);
    EXPECT_EQ(notRequested.find("ACTIVE"), std::string::npos);

    // Only the Active line may claim coverage; both others must say NOT checked.
    EXPECT_NE(active.find("ARE checked"), std::string::npos);
    EXPECT_NE(unavailable.find("NOT checked"), std::string::npos);
    EXPECT_NE(notRequested.find("NOT checked"), std::string::npos);
}

// --- Device fault records (VK_EXT_device_fault) ------------------------------
// Checkpoints answer "where did execution stop"; these answer "what did the
// hardware fault on". The formatter is pure, so the record-shaping contract is
// testable without a lost device.

TEST(DeviceFault, AddressTypeNamesCoverTheEnumAndSurviveUnknowns)
{
    EXPECT_EQ(DescribeDeviceFaultAddressType(VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT), "NONE");
    EXPECT_EQ(DescribeDeviceFaultAddressType(VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT), "READ_INVALID");
    EXPECT_EQ(DescribeDeviceFaultAddressType(VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT), "WRITE_INVALID");
    EXPECT_EQ(DescribeDeviceFaultAddressType(VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT), "EXECUTE_INVALID");
    EXPECT_EQ(DescribeDeviceFaultAddressType(VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT), "IP_FAULT");
    // A driver newer than these headers must still print something identifiable.
    EXPECT_EQ(DescribeDeviceFaultAddressType(static_cast<VkDeviceFaultAddressTypeEXT>(99)), "UNKNOWN(99)");
}

// The reported address is only precise to a power-of-two granule, so the RANGE
// is what a buffer VA gets matched against. Getting this wrong would point an
// investigation at the wrong allocation.
TEST(DeviceFault, AddressRangeIsWidenedByThePrecisionGranule)
{
    const DeviceFaultAddressRange page = ResolveDeviceFaultAddressRange(0x12345678u, 0x1000);
    EXPECT_EQ(page.Begin, 0x12345000u);
    EXPECT_EQ(page.End, 0x12346000u);

    // Precision 1 is exact.
    const DeviceFaultAddressRange exact = ResolveDeviceFaultAddressRange(0xdeadbeefu, 1);
    EXPECT_EQ(exact.Begin, 0xdeadbeefu);
    EXPECT_EQ(exact.End, 0xdeadbef0u);

    // A driver declining to state a precision, or stating a nonsense one, must
    // not synthesize a wild range from a bad mask.
    for (VkDeviceSize bogus : {VkDeviceSize{0}, VkDeviceSize{3}, VkDeviceSize{100}})
    {
        const DeviceFaultAddressRange degraded = ResolveDeviceFaultAddressRange(0xabc, bogus);
        EXPECT_EQ(degraded.Begin, 0xabcu) << "precision " << bogus;
        EXPECT_EQ(degraded.End, 0xabdu) << "precision " << bogus;
    }
}

TEST(DeviceFault, DescriptionFieldIsBoundedNotNulTerminated)
{
    // A driver that fills every byte must not walk the formatter off the end.
    char full[4] = {'a', 'b', 'c', 'd'};
    EXPECT_EQ(DescribeFaultDescriptionField(full, sizeof(full)), "abcd");

    char terminated[8] = {'h', 'i', '\0', 'X', 'X', 'X', 'X', 'X'};
    EXPECT_EQ(DescribeFaultDescriptionField(terminated, sizeof(terminated)), "hi");

    EXPECT_EQ(DescribeFaultDescriptionField(nullptr, 8), "");
    EXPECT_EQ(DescribeFaultDescriptionField(full, 0), "");
}

TEST(DeviceFault, FormatterReportsAddressAndVendorRecords)
{
    VkDeviceFaultInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
    std::snprintf(info.description, sizeof(info.description), "%s", "page fault");

    VkDeviceFaultAddressInfoEXT addresses[1]{};
    addresses[0].addressType = VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT;
    addresses[0].reportedAddress = 0x7f001234u;
    addresses[0].addressPrecision = 0x1000;

    VkDeviceFaultVendorInfoEXT vendors[1]{};
    std::snprintf(vendors[0].description, sizeof(vendors[0].description), "%s", "MMU fault");
    vendors[0].vendorFaultCode = 0x42;
    vendors[0].vendorFaultData = 0x99;

    const std::string text = FormatDeviceFaultInfo(info, addresses, 1, vendors, 1);
    EXPECT_NE(text.find("page fault"), std::string::npos);
    EXPECT_NE(text.find("WRITE_INVALID"), std::string::npos);
    EXPECT_NE(text.find("0x000000007f001234"), std::string::npos);
    // The widened range, not just the raw address, is what makes the record actionable.
    EXPECT_NE(text.find("range=[0x000000007f001000,0x000000007f002000)"), std::string::npos);
    EXPECT_NE(text.find("MMU fault"), std::string::npos);
}

// Silence must never read as "clean": each distinct no-data case says which one
// it is. Two investigations have already been misled by an ambiguous null result.
TEST(DeviceFault, EmptyAndUnavailableCasesAreDistinguishable)
{
    VkDeviceFaultInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;

    const std::string none = FormatDeviceFaultInfo(info, nullptr, 0, nullptr, 0);
    EXPECT_NE(none.find("address records: <none reported>"), std::string::npos);
    EXPECT_NE(none.find("vendor records: <none reported>"), std::string::npos);

    EXPECT_EQ(QueryAndFormatDeviceFault(nullptr, VK_NULL_HANDLE),
              "<device-fault retrieval unavailable>");
}

// --- The two-call retrieval protocol, against a fake driver -------------------
// In production QueryAndFormatDeviceFault is only reachable on a lost device, and
// every test above hands it a null function pointer, so the protocol itself — how
// it sizes, what it declines, and what it claims was lost — had no coverage at
// all. This driver stands in for the real entry point, which makes each edge
// provable without a TDR: a declined vendor binary, a capped record count, a
// driver whose counts move under it, and the clean control.

namespace {

struct FakeFaultDriver
{
    /// What the sizing call (pFaultInfo == NULL) reports as available. The spec
    /// pins repeated sizing calls identical, so this is what the caller sizes to.
    uint32_t SizingAddressCount = 0;
    uint32_t SizingVendorCount = 0;
    VkDeviceSize VendorBinarySize = 0;

    /// What the retrieval call treats as available. Equal to the sizing counts on
    /// a conformant driver; a difference models one that is not.
    uint32_t RetrievalAddressCount = 0;
    uint32_t RetrievalVendorCount = 0;

    /// Ceiling on what the retrieval call writes, independent of the capacity it
    /// was handed — a driver returning fewer records than it accepted room for.
    uint32_t WriteLimit = UINT32_MAX;

    const char* Description = "";
    VkDeviceFaultAddressInfoEXT AddressRecord{};
    VkDeviceFaultVendorInfoEXT VendorRecord{};

    uint32_t SizingCalls = 0;
    uint32_t RetrievalCalls = 0;
};

FakeFaultDriver MakeFakeFaultDriver(uint32_t addressRecords, uint32_t vendorRecords)
{
    FakeFaultDriver driver{};
    driver.SizingAddressCount = addressRecords;
    driver.RetrievalAddressCount = addressRecords;
    driver.SizingVendorCount = vendorRecords;
    driver.RetrievalVendorCount = vendorRecords;
    driver.Description = "page fault on write";
    driver.AddressRecord.addressType = VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT;
    driver.AddressRecord.reportedAddress = 0x7f001234u;
    driver.AddressRecord.addressPrecision = 0x1000;
    std::snprintf(driver.VendorRecord.description, sizeof(driver.VendorRecord.description), "%s",
                  "MMU fault");
    driver.VendorRecord.vendorFaultCode = 0x42;
    driver.VendorRecord.vendorFaultData = 0x99;
    return driver;
}

/// The retrieval never dereferences the device — it only hands it back to the
/// driver entry point, which is exactly how the fake recovers its state without
/// a global. Each test therefore owns its driver outright.
VkDevice AsFakeDevice(FakeFaultDriver& driver)
{
    return reinterpret_cast<VkDevice>(&driver);
}

VKAPI_ATTR VkResult VKAPI_CALL FakeGetDeviceFaultInfo(VkDevice device,
                                                      VkDeviceFaultCountsEXT* pFaultCounts,
                                                      VkDeviceFaultInfoEXT* pFaultInfo)
{
    FakeFaultDriver& driver = *reinterpret_cast<FakeFaultDriver*>(device);

    if (pFaultInfo == nullptr)
    {
        ++driver.SizingCalls;
        pFaultCounts->addressInfoCount = driver.SizingAddressCount;
        pFaultCounts->vendorInfoCount = driver.SizingVendorCount;
        pFaultCounts->vendorBinarySize = driver.VendorBinarySize;
        return VK_SUCCESS;
    }

    ++driver.RetrievalCalls;
    std::snprintf(pFaultInfo->description, sizeof(pFaultInfo->description), "%s", driver.Description);

    const uint32_t addressesWritten =
        std::min({pFaultCounts->addressInfoCount, driver.RetrievalAddressCount, driver.WriteLimit});
    if (pFaultInfo->pAddressInfos != nullptr)
    {
        for (uint32_t i = 0; i < addressesWritten; ++i)
        {
            pFaultInfo->pAddressInfos[i] = driver.AddressRecord;
        }
    }
    const uint32_t vendorsWritten =
        std::min({pFaultCounts->vendorInfoCount, driver.RetrievalVendorCount, driver.WriteLimit});
    if (pFaultInfo->pVendorInfos != nullptr)
    {
        for (uint32_t i = 0; i < vendorsWritten; ++i)
        {
            pFaultInfo->pVendorInfos[i] = driver.VendorRecord;
        }
    }
    const VkDeviceSize binaryWritten = std::min(pFaultCounts->vendorBinarySize, driver.VendorBinarySize);

    pFaultCounts->addressInfoCount = addressesWritten;
    pFaultCounts->vendorInfoCount = vendorsWritten;
    pFaultCounts->vendorBinarySize = binaryWritten;

    const bool anythingShort = addressesWritten < driver.RetrievalAddressCount ||
                               vendorsWritten < driver.RetrievalVendorCount ||
                               binaryWritten < driver.VendorBinarySize;
    return anythingShort ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL FailingGetDeviceFaultInfo(VkDevice,
                                                         VkDeviceFaultCountsEXT*,
                                                         VkDeviceFaultInfoEXT*)
{
    return VK_ERROR_UNKNOWN;
}

} // namespace

// Control arm: nothing was short, so the report carries no note whatsoever.
TEST(DeviceFaultQuery, AFullRetrievalReportsEveryRecordAndAddsNoNote)
{
    FakeFaultDriver driver = MakeFakeFaultDriver(/*addressRecords=*/2, /*vendorRecords=*/1);

    const std::string report = QueryAndFormatDeviceFault(&FakeGetDeviceFaultInfo, AsFakeDevice(driver));

    EXPECT_EQ(driver.SizingCalls, 1u);
    EXPECT_EQ(driver.RetrievalCalls, 1u);
    EXPECT_NE(report.find("address records (2)"), std::string::npos) << report;
    EXPECT_NE(report.find("vendor records (1)"), std::string::npos) << report;
    EXPECT_EQ(report.find("NOTE"), std::string::npos)
        << "a complete retrieval must add no note at all: " << report;
}

// The vendor binary is declined by setting its capacity to zero, which returns
// VK_INCOMPLETE on its own. Calling that "more records than were retrieved" sends
// the next investigation hunting evidence that was never missing.
TEST(DeviceFaultQuery, DecliningTheVendorBinaryIsNotReportedAsLostRecords)
{
    FakeFaultDriver driver = MakeFakeFaultDriver(2, 1);
    driver.VendorBinarySize = 4096;

    const std::string report = QueryAndFormatDeviceFault(&FakeGetDeviceFaultInfo, AsFakeDevice(driver));

    EXPECT_NE(report.find("address records (2)"), std::string::npos) << report;
    EXPECT_NE(report.find("vendor records (1)"), std::string::npos) << report;
    EXPECT_EQ(report.find("TRUNCATED"), std::string::npos)
        << "every record came back; only the blob we declined stayed behind: " << report;
    EXPECT_NE(report.find("all records retrieved"), std::string::npos) << report;
    EXPECT_NE(report.find("4096-byte vendor binary"), std::string::npos) << report;
}

// Genuine loss, caused by our own cap. Reaching the assertions at all is the
// proof that nothing tried to allocate the ~103 GB and ~1.16 TB these counts ask
// for inside the crash handler.
TEST(DeviceFaultQuery, AHostileRecordCountIsCappedAndTheCapIsNamed)
{
    FakeFaultDriver driver = MakeFakeFaultDriver(UINT32_MAX, UINT32_MAX);

    const std::string report = QueryAndFormatDeviceFault(&FakeGetDeviceFaultInfo, AsFakeDevice(driver));

    const std::string expected = "records TRUNCATED (" + std::to_string(kMaxFaultRecords) +
                                 "/4294967295 address, " + std::to_string(kMaxFaultRecords) +
                                 "/4294967295 vendor)";
    EXPECT_NE(report.find(expected), std::string::npos) << report;
    EXPECT_NE(report.find("capped at " + std::to_string(kMaxFaultRecords)), std::string::npos) << report;
}

// Genuine loss, caused by the driver: it wrote fewer records than the room it was
// given. Naming the cap here would name the wrong cause.
TEST(DeviceFaultQuery, ARecordArrayComingBackShortIsNamedWithItsCounts)
{
    FakeFaultDriver driver = MakeFakeFaultDriver(3, 2);
    driver.WriteLimit = 1;

    const std::string report = QueryAndFormatDeviceFault(&FakeGetDeviceFaultInfo, AsFakeDevice(driver));

    EXPECT_NE(report.find("records TRUNCATED (1/3 address, 1/2 vendor)"), std::string::npos) << report;
    EXPECT_EQ(report.find("capped at"), std::string::npos) << report;
}

// A driver whose counts grow between the sizing and retrieval calls violates the
// spec's stability guarantee, and its VK_INCOMPLETE cannot be attributed from the
// numbers. Say that, rather than inventing a loss the numbers do not support.
TEST(DeviceFaultQuery, VkIncompleteWithNothingShortIsReportedAsADriverInconsistency)
{
    FakeFaultDriver driver = MakeFakeFaultDriver(1, 0);
    driver.RetrievalAddressCount = 4;

    const std::string report = QueryAndFormatDeviceFault(&FakeGetDeviceFaultInfo, AsFakeDevice(driver));

    EXPECT_NE(report.find("VK_INCOMPLETE but reported nothing short"), std::string::npos) << report;
    EXPECT_EQ(report.find("TRUNCATED"), std::string::npos) << report;
}

// description is the driver's own sentence about the fault and only the second
// call fills it in, so a zero-record fault must still make that call.
TEST(DeviceFaultQuery, ADescriptionOnlyFaultStillReachesTheSecondCall)
{
    FakeFaultDriver driver = MakeFakeFaultDriver(0, 0);
    driver.Description = "GPU hang in compute";

    const std::string report = QueryAndFormatDeviceFault(&FakeGetDeviceFaultInfo, AsFakeDevice(driver));

    EXPECT_EQ(driver.RetrievalCalls, 1u)
        << "returning early on zero records would discard the driver's description";
    EXPECT_NE(report.find("GPU hang in compute"), std::string::npos) << report;
    EXPECT_NE(report.find("address records: <none reported>"), std::string::npos) << report;
    EXPECT_NE(report.find("vendor records: <none reported>"), std::string::npos) << report;
    EXPECT_EQ(report.find("NOTE"), std::string::npos) << report;
}

// The graceful-degrade string is how a run tells a real loss (records, or a
// driver that failed to produce them) from an injected one. It has to survive.
TEST(DeviceFaultQuery, ACountQueryFailureKeepsTheDiagnosticString)
{
    FakeFaultDriver driver = MakeFakeFaultDriver(0, 0);

    const std::string report =
        QueryAndFormatDeviceFault(&FailingGetDeviceFaultInfo, AsFakeDevice(driver));

    EXPECT_EQ(report, "<device-fault count query failed (VkResult=" +
                          std::to_string(static_cast<int>(VK_ERROR_UNKNOWN)) + ")>");
    EXPECT_EQ(driver.RetrievalCalls, 0u);
}

// The toggle resolves against a config default rather than being plain on/off,
// so both directions of the override and the fall-through are pinned here.
TEST(DeviceFault, TogglePolicyOverridesTheConfigDefaultInBothDirections)
{
    // Unset says nothing: the config default decides.
    EXPECT_TRUE(ParseDeviceFaultEnabled(nullptr, true));
    EXPECT_FALSE(ParseDeviceFaultEnabled(nullptr, false));
    EXPECT_TRUE(ParseDeviceFaultEnabled("", true));
    EXPECT_FALSE(ParseDeviceFaultEnabled("", false));

    // Arm a Release investigation.
    EXPECT_TRUE(ParseDeviceFaultEnabled("1", false));
    EXPECT_TRUE(ParseDeviceFaultEnabled("on", false));

    // Disarm a developer build, to A/B whether the extension itself changed
    // anything — worthless if the toggle could only ever turn it on.
    EXPECT_FALSE(ParseDeviceFaultEnabled("0", true));
    EXPECT_FALSE(ParseDeviceFaultEnabled("false", true));
    EXPECT_FALSE(ParseDeviceFaultEnabled("F", true));
}

// The one toggle in this file whose polarity is inverted — GE_VK_NO_DEBUG_UTILS
// names what it removes — so the unset case is pinned in the direction that
// matters: saying nothing leaves labels ON in every config.
TEST(DebugUtilsLabels, TogglePolicyIsOnByDefaultAndNamedForWhatItRemoves)
{
    EXPECT_FALSE(ParseDebugUtilsDisabled(nullptr));
    EXPECT_FALSE(ParseDebugUtilsDisabled(""));

    EXPECT_TRUE(ParseDebugUtilsDisabled("1"));
    EXPECT_TRUE(ParseDebugUtilsDisabled("true"));
    EXPECT_TRUE(ParseDebugUtilsDisabled("on"));

    // Spelling the escape hatch off is not the same as never setting it, and both
    // have to leave the extension enabled — a "GE_VK_NO_DEBUG_UTILS=0" that killed
    // labels would read as the opposite of what it says.
    EXPECT_FALSE(ParseDebugUtilsDisabled("0"));
    EXPECT_FALSE(ParseDebugUtilsDisabled("false"));
    EXPECT_FALSE(ParseDebugUtilsDisabled("False"));
}

// A freshly constructed or half-resolved set is unavailable: Available() demands
// all three entry points, so a set that could open labels it cannot close never
// reports usable.
TEST(DebugUtilsLabels, AnUnresolvedSetIsUnavailable)
{
    DebugUtilsLabelFns fns{};
    EXPECT_FALSE(fns.Available());
    EXPECT_EQ(fns.Begin, nullptr);
    EXPECT_EQ(fns.End, nullptr);
    EXPECT_EQ(fns.Insert, nullptr);

    // A half-resolved set must not report available either. Never called; the
    // pointer only needs to be non-null.
    static constexpr auto kDummyBegin = [](VkCommandBuffer, const VkDebugUtilsLabelEXT*) VKAPI_ATTR -> void {};
    fns.Begin = static_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(kDummyBegin);
    EXPECT_FALSE(fns.Available());
}

// The point of the extension being independent of the validation layer: this
// device asks for no debug layer (DeviceDesc::enableDebugLayer defaults false,
// the DebugFast/Release shape), and labels must still be live. Device-gated the
// same way as the checkpoint and device-fault probes — the loader's own
// advertisement decides what is expected, so the assertion means something on a
// machine that genuinely lacks the extension.
TEST(DebugUtilsLabels, ResolveWithoutTheValidationLayerAndAllOrNone)
{
    SetEnvHeadless();
    if (ParseDebugUtilsDisabled(std::getenv("GE_VK_NO_DEBUG_UTILS")))
    {
        // The toggle is memoized at first instance creation, so an environment
        // that already carries a truthy value cannot be undone from inside this
        // process. A falsy spelling ("0") leaves labels on and the test valid.
        GTEST_SKIP() << "GE_VK_NO_DEBUG_UTILS is set truthy in this environment";
    }

    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    ASSERT_FALSE(dd.enableDebugLayer) << "this test is only meaningful without the validation layer";
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    if (!dev->Initialize(dd))
    {
        GTEST_SKIP() << "no Vulkan device available";
    }
    auto* vk = static_cast<VulkanDevice*>(dev.get());

    uint32_t instanceExtensionCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, nullptr);
    std::vector<VkExtensionProperties> available(instanceExtensionCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, available.data());
    bool advertised = false;
    for (const auto& e : available)
    {
        if (std::strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0)
        {
            advertised = true;
            break;
        }
    }

    const DebugUtilsLabelFns& fns = vk->GetDebugUtilsLabelFns();
    // Same probe line as the DeviceFault sibling: a green run must say which arm
    // executed, or a vacuous pass on a debug_utils-less machine is
    // indistinguishable from the meaningful one.
    std::cout << "[ probe ] VK_EXT_debug_utils advertised=" << (advertised ? "true" : "false")
              << " labelsAvailable=" << (fns.Available() ? "true" : "false") << "\n";
    EXPECT_EQ(fns.Available(), advertised)
        << "labels must follow the loader's advertisement, not the validation layer";

    // All-or-none: a set that resolved Begin without End would open labels it can
    // never close, and Available() would wave it through.
    if (fns.Available())
    {
        EXPECT_NE(fns.End, nullptr);
        EXPECT_NE(fns.Insert, nullptr);
    }
    else
    {
        EXPECT_EQ(fns.End, nullptr);
        EXPECT_EQ(fns.Insert, nullptr);
    }
}

// Device-gated: with the toggle explicitly armed, the engine must enable the
// extension exactly when the physical device advertises it, mirroring the
// checkpoint probe above. Armed by hand rather than by config default so the
// assertion means the same thing in a Release run of this suite.
TEST(DeviceFault, RuntimeSupportMatchesPhysicalDeviceAdvertisement)
{
    SetEnvHeadless();
    // Scoped: the skip and the ASSERTs below return early, and a leaked arming
    // would silently re-arm every later test in this process.
    GameEngine::Rendering::Tests::ScopedEnvVar deviceFault("GE_VK_DEVICE_FAULT", "1");
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    if (!dev->Initialize(dd))
    {
        GTEST_SKIP() << "no Vulkan device available";
    }
    auto* vk = static_cast<VulkanDevice*>(dev.get());

    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(vk->GetVkPhysicalDevice(), nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> available(extensionCount);
    vkEnumerateDeviceExtensionProperties(vk->GetVkPhysicalDevice(), nullptr, &extensionCount,
                                         available.data());
    bool advertised = false;
    // Probed alongside because the Crash Diagnostic Layer picks its marker
    // mechanism from these two: VK_AMD_buffer_marker gives it per-command
    // resolution, while falling back to NV checkpoints inherits this adapter's
    // TOP/BOTTOM-only checkpointExecutionStageMask and buys far less.
    bool bufferMarkerAdvertised = false;
    for (const auto& e : available)
    {
        if (std::strcmp(e.extensionName, VK_EXT_DEVICE_FAULT_EXTENSION_NAME) == 0)
        {
            advertised = true;
        }
        if (std::strcmp(e.extensionName, "VK_AMD_buffer_marker") == 0)
        {
            bufferMarkerAdvertised = true;
        }
    }

    VkPhysicalDeviceFaultFeaturesEXT faultFeatures{};
    faultFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 query{};
    query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    query.pNext = &faultFeatures;
    vkGetPhysicalDeviceFeatures2(vk->GetVkPhysicalDevice(), &query);
    const bool featureSupported = faultFeatures.deviceFault == VK_TRUE;

    // Printed so the probe result is visible in the run log on any machine.
    std::cout << "[ probe    ] VK_EXT_device_fault advertised=" << (advertised ? "yes" : "no")
              << " deviceFaultFeature=" << (featureSupported ? "yes" : "no")
              << " armed=" << (vk->DeviceFaultReportingAvailable() ? "yes" : "no") << std::endl;
    std::cout << "[ probe    ] VK_AMD_buffer_marker advertised="
              << (bufferMarkerAdvertised ? "yes" : "no")
              << " (crash-diagnostic-layer per-command granularity)" << std::endl;
    EXPECT_EQ(vk->DeviceFaultReportingAvailable(), advertised && featureSupported)
        << "the device must enable and resolve VK_EXT_device_fault exactly when the physical "
           "device advertises the extension AND its deviceFault feature";
    // No loss has happened, so there is nothing retrieved yet.
    EXPECT_TRUE(vk->LastDeviceFaultReport().empty());

    dev->Shutdown();
}

// --- GPU vendor classification ----------------------------------------------
// The classification a vendor gate reads. Wrong here means a vendor-specific
// path armed on hardware that cannot run it, so the mapping is pinned by id.

TEST(GpuVendorClassification, KnownVendorIdsMapToTheirEnumerators)
{
    EXPECT_EQ(ClassifyGpuVendor(0x10DEu), GpuVendor::Nvidia);
    EXPECT_EQ(ClassifyGpuVendor(0x1002u), GpuVendor::Amd);
    EXPECT_EQ(ClassifyGpuVendor(0x8086u), GpuVendor::Intel);
}

// An unrecognized id must stay Unknown rather than land on the nearest
// enumerator: a gate that guessed would enable a vendor path on foreign
// hardware. Zero is the "backend reports no vendor" case and behaves the same.
TEST(GpuVendorClassification, UnrecognizedAndAbsentIdsStayUnknown)
{
    EXPECT_EQ(ClassifyGpuVendor(0u), GpuVendor::Unknown);
    EXPECT_EQ(ClassifyGpuVendor(0x13B5u), GpuVendor::Unknown); // ARM
    EXPECT_EQ(ClassifyGpuVendor(0x5143u), GpuVendor::Unknown); // Qualcomm
    EXPECT_EQ(ClassifyGpuVendor(0xFFFFFFFFu), GpuVendor::Unknown);
}

// Every enumerator names itself, including Unknown — a vendor field that
// rendered as an empty string would read as a missing field in a report.
TEST(GpuVendorClassification, EveryEnumeratorHasANonEmptyName)
{
    EXPECT_STREQ(GpuVendorName(GpuVendor::Nvidia), "NVIDIA");
    EXPECT_STREQ(GpuVendorName(GpuVendor::Amd), "AMD");
    EXPECT_STREQ(GpuVendorName(GpuVendor::Intel), "Intel");
    EXPECT_STREQ(GpuVendorName(GpuVendor::Unknown), "Unknown");
}

// --- Attached-tool reporting (VK_EXT_tooling_info) ---------------------------
// The formatter and the purpose decode are pure, so they are pinned without a
// device. What they must never do is make "nothing attached" and "never asked"
// look alike.

TEST(AttachedTools, PurposeFlagsDecodeIndividuallyAndInCombination)
{
    EXPECT_EQ(DescribeToolPurposes(VK_TOOL_PURPOSE_VALIDATION_BIT), "VALIDATION");
    EXPECT_EQ(DescribeToolPurposes(VK_TOOL_PURPOSE_PROFILING_BIT), "PROFILING");
    EXPECT_EQ(DescribeToolPurposes(VK_TOOL_PURPOSE_TRACING_BIT), "TRACING");
    EXPECT_EQ(DescribeToolPurposes(VK_TOOL_PURPOSE_ADDITIONAL_FEATURES_BIT), "ADDITIONAL_FEATURES");
    EXPECT_EQ(DescribeToolPurposes(VK_TOOL_PURPOSE_MODIFYING_FEATURES_BIT), "MODIFYING_FEATURES");
    EXPECT_EQ(DescribeToolPurposes(VK_TOOL_PURPOSE_DEBUG_REPORTING_BIT_EXT), "DEBUG_REPORTING");
    EXPECT_EQ(DescribeToolPurposes(VK_TOOL_PURPOSE_DEBUG_MARKERS_BIT_EXT), "DEBUG_MARKERS");

    // Order follows the bit order, not the order the caller happened to OR them.
    EXPECT_EQ(DescribeToolPurposes(VK_TOOL_PURPOSE_TRACING_BIT | VK_TOOL_PURPOSE_VALIDATION_BIT),
              "VALIDATION|TRACING");
}

// A tool built against a newer SDK than this one must not silently lose the bit
// that says it MODIFIES device behaviour. Unknown bits render numerically, and
// named bits alongside them still render.
TEST(AttachedTools, UnknownPurposeBitsRenderNumericallyInsteadOfVanishing)
{
    constexpr VkToolPurposeFlags kUnknownBit = 0x8000u;
    EXPECT_EQ(DescribeToolPurposes(kUnknownBit), "UNKNOWN(0x8000)");
    EXPECT_EQ(DescribeToolPurposes(VK_TOOL_PURPOSE_TRACING_BIT | kUnknownBit),
              "TRACING|UNKNOWN(0x8000)");

    // No bits at all is still a statement, not an absence.
    EXPECT_EQ(DescribeToolPurposes(0), "NONE");
}

// The distinction the whole query exists to preserve: an empty list means the
// runtime answered "nothing attached", while an unavailable query means it was
// never asked. A reader that saw the same string for both would conclude a
// capture layer was absent from a run that never checked.
TEST(AttachedTools, NoneAttachedAndQueryUnavailableAreDistinctStrings)
{
    const std::vector<AttachedGraphicsTool> none;
    const std::string asked   = FormatAttachedTools(none, true);
    const std::string notAsked = FormatAttachedTools(none, false);

    EXPECT_EQ(asked, "<none attached>");
    EXPECT_EQ(notAsked, "<tooling query unavailable>");
    EXPECT_NE(asked, notAsked);
}

// An unavailable query reports itself as unavailable even if a caller somehow
// hands it tools: the flag is the authority on whether anything was learned.
TEST(AttachedTools, UnavailableQueryOutranksAnyToolsHandedToTheFormatter)
{
    std::vector<AttachedGraphicsTool> tools(1);
    tools[0].Name = "RenderDoc";
    EXPECT_EQ(FormatAttachedTools(tools, false), "<tooling query unavailable>");
}

TEST(AttachedTools, FormatterNamesEveryToolWithItsVersionAndPurposes)
{
    std::vector<AttachedGraphicsTool> tools(2);
    tools[0].Name     = "RenderDoc";
    tools[0].Version  = "v1.45";
    tools[0].Purposes = "TRACING|ADDITIONAL_FEATURES";
    tools[1].Name     = "NVIDIA Nsight Graphics";
    tools[1].Version  = "2025.1";
    tools[1].Purposes = "PROFILING|TRACING";

    const std::string formatted = FormatAttachedTools(tools, true);
    EXPECT_NE(formatted.find("2 attached:"), std::string::npos);
    EXPECT_NE(formatted.find("'RenderDoc' version=v1.45"), std::string::npos);
    EXPECT_NE(formatted.find("TRACING|ADDITIONAL_FEATURES"), std::string::npos);
    EXPECT_NE(formatted.find("'NVIDIA Nsight Graphics' version=2025.1"), std::string::npos);
    EXPECT_NE(formatted.find("PROFILING|TRACING"), std::string::npos);
    // Neither placeholder may appear once there is something real to report.
    EXPECT_EQ(formatted.find("<none attached>"), std::string::npos);
    EXPECT_EQ(formatted.find("<tooling query unavailable>"), std::string::npos);
}

// One exact single-tool rendering, so the index prefix, quoting and separators
// are pinned somewhere rather than only substring-checked.
TEST(AttachedTools, SingleToolRendersExactly)
{
    std::vector<AttachedGraphicsTool> tools(1);
    tools[0].Name     = "RenderDoc";
    tools[0].Version  = "v1.45";
    tools[0].Purposes = "TRACING";

    EXPECT_EQ(FormatAttachedTools(tools, true), "1 attached: [0] 'RenderDoc' version=v1.45 purposes=TRACING");
}

// The version is free-form driver text, and tools disagree about whether it
// carries its own "v" — RenderDoc really does report "v1.45". A formatter that
// prefixed one itself rendered that as "vv1.45", so the label is fixed and the
// value is passed through untouched whichever shape it arrives in.
TEST(AttachedTools, AVersionCarryingItsOwnPrefixIsNotDoubledUp)
{
    std::vector<AttachedGraphicsTool> tools(1);
    tools[0].Name     = "RenderDoc";
    tools[0].Version  = "v1.45 (2fc0bc04cb95)";
    tools[0].Purposes = "TRACING";

    const std::string formatted = FormatAttachedTools(tools, true);
    EXPECT_NE(formatted.find("version=v1.45 (2fc0bc04cb95)"), std::string::npos);
    EXPECT_EQ(formatted.find("vv"), std::string::npos) << formatted;

    // A version with no prefix of its own is passed through just as literally.
    tools[0].Version = "2025.1";
    EXPECT_NE(FormatAttachedTools(tools, true).find("version=2025.1"), std::string::npos);
}

// A tool that reports no version must not leave a dangling label in the log.
TEST(AttachedTools, AMissingVersionIsOmittedRatherThanLeftDangling)
{
    std::vector<AttachedGraphicsTool> tools(1);
    tools[0].Name     = "Anonymous Layer";
    tools[0].Purposes = "NONE";

    const std::string formatted = FormatAttachedTools(tools, true);
    EXPECT_NE(formatted.find("'Anonymous Layer'"), std::string::npos);
    EXPECT_EQ(formatted.find("version="), std::string::npos);
}

// Device-gated: the query either resolves on this runtime or it does not, and
// availability must never be reported alongside a fabricated tool list. Run
// against a real device so the two-call retrieval is exercised, not just the
// formatter above.
TEST(AttachedTools, RuntimeQueryReportsAvailabilityConsistentlyWithItsResults)
{
    SetEnvHeadless();
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    if (!dev->Initialize(dd))
    {
        GTEST_SKIP() << "no Vulkan device available";
    }
    auto* vk = static_cast<VulkanDevice*>(dev.get());

    // Mirror the engine's instance-creation rule (min of loader and 1.3): the
    // free function judges core-query legality against the version the instance
    // was created with, which is not exposed publicly.
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    vkEnumerateInstanceVersion(&loaderVersion);
    const uint32_t instanceApiVersion = std::min(loaderVersion, static_cast<uint32_t>(VK_API_VERSION_1_3));

    bool queryAvailable = false;
    const std::vector<AttachedGraphicsTool> tools =
        QueryAttachedTools(vk->GetVkInstance(), vk->GetVkPhysicalDevice(), instanceApiVersion, queryAvailable);

    // Printed so the run log shows what this machine actually reported.
    std::cout << "[ probe    ] tooling query available=" << (queryAvailable ? "yes" : "no")
              << " tools=" << FormatAttachedTools(tools, queryAvailable) << std::endl;

    // An unavailable query may never carry results: that pairing would mean the
    // list came from somewhere other than the runtime.
    if (!queryAvailable)
    {
        EXPECT_TRUE(tools.empty());
    }
    // Whatever came back must be self-describing — an unnamed tool tells a
    // reader nothing, and every purpose field is non-empty by construction.
    for (const AttachedGraphicsTool& tool : tools)
    {
        EXPECT_FALSE(tool.Name.empty());
        EXPECT_FALSE(tool.Purposes.empty());
    }

    // The device-level report must agree with the direct query about
    // availability, and must report labels from the resolved entry points.
    const GpuToolingReport report = vk->GetGpuToolingReport();
    EXPECT_EQ(report.ToolingQueryAvailable, queryAvailable);
    EXPECT_EQ(report.AttachedTools.size(), tools.size());
    EXPECT_EQ(report.DebugLabelsAvailable, vk->GetDebugUtilsLabelFns().Available());
    // This device asked for no validation layer, so the report must not claim one.
    ASSERT_FALSE(dd.enableDebugLayer);
    EXPECT_FALSE(report.ValidationLayerEnabled);

    // The vendor must be classified from the id the device actually reports,
    // rather than left at the Unknown default on known hardware. The nonzero
    // check is what makes that claim: the classification EQ alone is satisfied
    // by the all-defaults state (0 -> Unknown).
    const RenderingDeviceCapabilities& caps = vk->GetCapabilities();
    std::cout << "[ probe    ] vendor=" << GpuVendorName(caps.vendor) << " vendorId=0x" << std::hex
              << caps.vendorId << std::dec << std::endl;
    EXPECT_NE(caps.vendorId, 0u);
    EXPECT_EQ(caps.vendor, ClassifyGpuVendor(caps.vendorId));

    dev->Shutdown();
}


// Device support and one buffer's addressability are separate facts. A
// compatibility profile can allocate ordinary SSBOs on BDA-capable hardware.
TEST(BufferDeviceAddress, AddresslessBufferReturnsZeroWithoutValidationErrors)
{
    SetEnvHeadless();
    ScopedEnvVar compat("GE_FORCE_COMPAT", "0");
    ScopedEnvVar validation("GE_VK_VALIDATION", "1");
    ScopedEnvVar validationAssert("GE_VK_VALIDATION_ASSERT", "0");
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDebugLayer = true;
    dd.descriptorBuffers = DescriptorBufferMode::Disabled;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    ASSERT_TRUE(dev->Initialize(dd));
    ASSERT_TRUE(dev->GetValidationStats().Enabled);
    if (!dev->GetCapabilities().supportsBufferDeviceAddress)
        GTEST_SKIP() << "buffer-device-address feature unavailable";
    ASSERT_FALSE(dev->IsDescriptorBufferEnabled());

    BufferDesc desc{};
    desc.size = 64;
    desc.usage = static_cast<uint32_t>(BufferUsage::Storage);
    desc.debugName = "AddresslessStorage";
    const auto plain = dev->CreateBuffer(desc);
    ASSERT_TRUE(plain.IsValid());
    desc.usage |= static_cast<uint32_t>(BufferUsage::ShaderDeviceAddress);
    desc.debugName = "AddressableStorage";
    const auto addressable = dev->CreateBuffer(desc);
    ASSERT_TRUE(addressable.IsValid());
    dev->ResetValidationStats();
    EXPECT_EQ(dev->GetBufferDeviceAddress({}), 0u);
    EXPECT_EQ(dev->GetBufferDeviceAddress(plain), 0u);
    EXPECT_NE(dev->GetBufferDeviceAddress(addressable), 0u);
    EXPECT_EQ(dev->GetValidationStats().ErrorCount, 0u);
    dev->DestroyBuffer(plain);
    dev->DestroyBuffer(addressable);
}

TEST(BufferDeviceAddress, DescriptorBuffersPreserveImplicitAddressability)
{
    SetEnvHeadless();
    ScopedEnvVar compat("GE_FORCE_COMPAT", "0");
    ScopedEnvVar descriptors("GE_VK_USE_DESCRIPTOR_BUFFER", "1");
    ScopedEnvVar validation("GE_VK_VALIDATION", "1");
    ScopedEnvVar validationAssert("GE_VK_VALIDATION_ASSERT", "0");
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDebugLayer = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    ASSERT_TRUE(dev->Initialize(dd));
    ASSERT_TRUE(dev->GetValidationStats().Enabled);
    if (!dev->IsDescriptorBufferEnabled())
        GTEST_SKIP() << "descriptor-buffer extension unavailable";
    BufferDesc desc{};
    desc.size = 64;
    desc.usage = static_cast<uint32_t>(BufferUsage::Storage);
    desc.debugName = "ImplicitAddressableStorage";
    const auto buffer = dev->CreateBuffer(desc);
    ASSERT_TRUE(buffer.IsValid());
    dev->ResetValidationStats();
    EXPECT_NE(dev->GetBufferDeviceAddress(buffer), 0u);
    EXPECT_EQ(dev->GetValidationStats().ErrorCount, 0u);
    dev->DestroyBuffer(buffer);
}

TEST(BufferDeviceAddress, ForcedCompatibilityHandlesAddresslessInstances)
{
    SetEnvHeadless();
    ScopedEnvVar compat("GE_FORCE_COMPAT", "1");
    ScopedEnvVar validation("GE_VK_VALIDATION", "1");
    ScopedEnvVar validationAssert("GE_VK_VALIDATION_ASSERT", "0");
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDebugLayer = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    ASSERT_TRUE(dev->Initialize(dd));
    ASSERT_TRUE(dev->GetValidationStats().Enabled);
    ASSERT_FALSE(dev->GetCapabilities().supportsBufferDeviceAddress);
    ASSERT_FALSE(dev->IsDescriptorBufferEnabled());
    BufferDesc desc{};
    desc.size = 64;
    desc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::Vertex);
    desc.debugName = "CompatibilityInstanceBuffer";
    const auto buffer = dev->CreateBuffer(desc);
    ASSERT_TRUE(buffer.IsValid());
    dev->ResetValidationStats();
    EXPECT_EQ(dev->GetBufferDeviceAddress(buffer), 0u);
    EXPECT_EQ(dev->GetValidationStats().ErrorCount, 0u);
    dev->DestroyBuffer(buffer);
}

// GE_VK_FILL_NEW_TARGETS_NAN makes a read before the first write visible: a new
// float target starts as NaN, so a pass that consumes it unwritten shows NaN on
// every run instead of whatever freed memory happened to hold (#2768).
TEST(DeviceDiagnostics, FillNewTargetsNaNClearsNewFloatTargets)
{
    SetEnvHeadless();
    const ScopedEnvVar fill("GE_VK_FILL_NEW_TARGETS_NAN", "1");
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    ASSERT_TRUE(dev->Initialize(dd));
    ASSERT_TRUE(dev->BeginFrame());

    constexpr uint32_t kW = 4u;
    constexpr uint32_t kH = 4u;
    TextureDesc td{};
    td.width = kW;
    td.height = kH;
    td.depth = 1u;
    td.mipLevels = 1u;
    td.arrayLayers = 1u;
    td.sampleCount = 1u;
    td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource |
                                     TextureUsage::TransferSrc);
    td.debugName = "FillNaNTest.Target";
    const TextureHandle target = dev->CreateTexture(td);
    ASSERT_TRUE(target.IsValid());

    const BufferHandle readback = dev->CreateReadbackBuffer(kW * kH * sizeof(float), "FillNaNTest.Read");
    ASSERT_TRUE(readback.IsValid());
    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl != nullptr);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(target, ResourceState::CopyDest, ResourceState::CopySource));
    cl->CopyTextureSubresourceToBuffer(target, 0, 0, readback, kW, kH);
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    dev->ExecuteCommandLists(lists);
    dev->WaitForIdle();

    std::vector<float> texels(static_cast<size_t>(kW) * kH, 0.0f);
    const void* mapped = dev->MapBuffer(readback);
    ASSERT_NE(mapped, nullptr);
    std::memcpy(texels.data(), mapped, texels.size() * sizeof(float));
    dev->UnmapBuffer(readback);
    for (size_t i = 0; i < texels.size(); ++i)
        EXPECT_TRUE(std::isnan(texels[i])) << "texel " << i << " of an unwritten target is " << texels[i];

    dev->DestroyBuffer(readback);
    dev->DestroyTexture(target);
    dev->Present();
}

// Host-visible so the test reads the buffer straight after CreateBuffer: the
// fill must already have landed, as a host write through a mapping would.
TEST(DeviceDiagnostics, FillNewTargetsNaNFillsNewStorageBuffers)
{
    SetEnvHeadless();
    const ScopedEnvVar fill("GE_VK_FILL_NEW_TARGETS_NAN", "1");
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    ASSERT_TRUE(dev->Initialize(dd));
    EXPECT_TRUE(dev->DebugFillsNewResourcesWithNaN());

    constexpr size_t kWords = 16u;
    BufferDesc bd{};
    bd.size = kWords * sizeof(float);
    bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    bd.memoryUsage = BufferMemoryUsage::Readback;
    bd.debugName = "FillNaNTest.Storage";
    const BufferHandle storage = dev->CreateBuffer(bd);
    ASSERT_TRUE(storage.IsValid());

    std::vector<uint32_t> words(kWords, 0u);
    const void* mapped = dev->MapBuffer(storage);
    ASSERT_NE(mapped, nullptr);
    std::memcpy(words.data(), mapped, words.size() * sizeof(uint32_t));
    dev->UnmapBuffer(storage);
    for (size_t i = 0; i < words.size(); ++i)
    {
        float asFloat = 0.0f;
        std::memcpy(&asFloat, &words[i], sizeof(asFloat));
        EXPECT_TRUE(std::isnan(asFloat)) << "word " << i << " of an unwritten storage buffer is 0x" << std::hex << words[i];
        for (const uint32_t half : {words[i] & 0xFFFFu, words[i] >> 16u})
            EXPECT_TRUE((half & 0x7C00u) == 0x7C00u && (half & 0x03FFu) != 0u)
                << "half of word " << i << " is 0x" << std::hex << half << ", not a float16 NaN";
    }
    dev->DestroyBuffer(storage);
}
