#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "../Source/Vulkan/TextureUsagePolicy.h"

using namespace GameEngine::Rendering;

namespace
{

// The audit resolves GE_TEXTURE_USAGE_AUDIT once per process, so it must be set
// before anything touches the audit. This test owns its executable.
void EnableAudit()
{
#ifdef _WIN32
    _putenv_s("GE_TEXTURE_USAGE_AUDIT", "1");
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_TEXTURE_USAGE_AUDIT", "1", 1);
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif
}

TextureHandle CreateAuditTexture(IDevice& device, uint32_t usage, const char* debugName)
{
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    desc.usage = usage;
    desc.persistent = false;
    desc.debugName = debugName;
    return device.CreateTexture(desc);
}

bool HasKey(const std::vector<std::string>& keys, const std::string& key)
{
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

bool HasKeyPrefixed(const std::vector<std::string>& keys, const std::string& prefix)
{
    return std::any_of(keys.begin(), keys.end(),
                       [&](const std::string& key) { return key.rfind(prefix, 0) == 0; });
}

// Counts harvest lines by level, so a test can tell "reported at Debug" from
// "reported at Info" rather than only "reported".
struct HarvestLineCounts
{
    std::atomic<int> DetailAtInfo{0};
    std::atomic<int> DetailAtDebug{0};
    std::atomic<int> SummaryAtInfo{0};
    std::atomic<int> AnyLine{0};
};

// The summary lines themselves, in order, so a test can read the numbers a given
// report printed rather than only how many reports printed.
struct SummaryLines
{
    std::mutex Mutex;
    std::vector<std::string> Lines;
    int AnyLine = 0;

    size_t Count()
    {
        std::lock_guard<std::mutex> lock(Mutex);
        return Lines.size();
    }
    int AnyLineCount()
    {
        std::lock_guard<std::mutex> lock(Mutex);
        return AnyLine;
    }
    std::string Last()
    {
        std::lock_guard<std::mutex> lock(Mutex);
        return Lines.empty() ? std::string() : Lines.back();
    }
    void Clear()
    {
        std::lock_guard<std::mutex> lock(Mutex);
        Lines.clear();
    }
};

// Mirrors the production repeat rule, so the test states the growth it expects in the
// same terms the policy does.
constexpr uint64_t kSummaryGrowthFactor = 10;

// The first number on a summary line, i.e. the observed-use tally that report printed.
// Returns UINT64_MAX when the line does not carry one, so a mismatch reads as a failed
// comparison rather than a silent zero.
uint64_t ObservedUsesInSummary(const std::string& line)
{
    const size_t start = line.find_first_of("0123456789", line.find("transfer-usage audit:"));
    if (start == std::string::npos)
    {
        return UINT64_MAX;
    }
    return std::strtoull(line.c_str() + start, nullptr, 10);
}

// Transfer uses on handles the TextureManager does not back: the swapchain-image shape
// the audit already tolerates (TextureUsagePolicy.cpp counts them and moves on). It grows
// the observed tally without a device, which is what lets one test stand in for a
// suite-shaped sequence of teardowns.
void RecordSyntheticUses(uint64_t count)
{
    for (uint64_t i = 0; i < count; ++i)
    {
        TextureUsagePolicy::RecordTransferUse(TextureHandle{~0ull - i}, TextureUsage::TransferDst,
                                              "TextureUsagePolicyTests(synthetic)");
    }
}

} // namespace

// Report() runs at every device teardown over process-global state, so a host that builds
// a device per test reprints the same growing tally once per device — a multi-line block
// per teardown, which is most of what a suite log is made of. The tally is diagnostic
// detail and belongs at Debug; what a reader who did not ask for it still needs is one
// line saying how much was observed, how many violations there are, and where the detail
// went.
//
// Declared first in this file so the reports it counts are the process's first, before
// the device-building tests below have summarized anything.
TEST(TextureUsagePolicy, HarvestDetailIsDebugAndTheSummaryDoesNotRepeat)
{
    EnableAudit();
    ASSERT_TRUE(TextureUsagePolicy::IsAuditEnabled()) << "audit flag resolved before the test set it";

    Logger::Log::Initialize({Logger::LogLevel::Debug});
    auto sink = Logger::MakeUnique<Logger::CallbackSink>();
    auto counts = std::make_shared<HarvestLineCounts>();
    // Matches on the block markers, not on a phrasing, so the gate measures where the
    // harvest goes rather than the wording it uses.
    sink->RegisterCallback(
        [counts](const Logger::LogMessage& msg)
        {
            counts->AnyLine.fetch_add(1);
            if (msg.Message.find("[TextureUsagePolicy]") == Logger::String::npos)
                return;
            const bool isSummary = msg.Message.find("transfer-usage audit:") != Logger::String::npos;
            if (isSummary)
            {
                if (msg.Level == Logger::LogLevel::Info)
                    counts->SummaryAtInfo.fetch_add(1);
                return;
            }
            if (msg.Level == Logger::LogLevel::Info)
                counts->DetailAtInfo.fetch_add(1);
            else if (msg.Level == Logger::LogLevel::Debug)
                counts->DetailAtDebug.fetch_add(1);
        });
    Logger::Log::AddSink(std::move(sink));

    constexpr int kReports = 3;
    for (int i = 0; i < kReports; ++i)
        TextureUsagePolicy::Report();
    Logger::Log::Flush(); // delivery to sinks is asynchronous

    // A capture that sees nothing is indistinguishable from a report that never ran.
    ASSERT_GT(counts->AnyLine.load(), 0) << "the sink saw no log line at all";
    EXPECT_GE(counts->DetailAtDebug.load(), kReports) << "the harvest detail did not reach Debug";
    EXPECT_EQ(0, counts->DetailAtInfo.load()) << "the harvest detail reached an Info reader";
    EXPECT_EQ(1, counts->SummaryAtInfo.load())
        << "the summary should print once and stay silent on reports that found nothing new";

    // Drop the counting sink, then put the console back: a binary left with no sink
    // silences every test after this one, its errors above all.
    Logger::Log::ClearSinks();
    Logger::Log::Initialize({Logger::LogLevel::Info});
}

// Report() runs at every device teardown, so the summary cannot print every time — but a
// summary that prints strictly once is the first teardown's snapshot, and in a process
// that builds a device per test that is a single-digit count standing in for the whole
// run. Every summary that prints must carry the tally as it is at that moment, and a
// report whose tally has not moved far enough to say anything new must stay silent.
//
// Declared second so the tally it grows starts from zero: the device tests below observe
// real transfer uses, and the harvest test above records none.
TEST(TextureUsagePolicy, SummaryCarriesTheTallyAtPrintTimeAndRepeatsOnlyOnGrowth)
{
    EnableAudit();
    ASSERT_TRUE(TextureUsagePolicy::IsAuditEnabled()) << "audit flag resolved before the test set it";

    Logger::Log::Initialize({Logger::LogLevel::Info});
    auto summaries = std::make_shared<SummaryLines>();
    auto sink = Logger::MakeUnique<Logger::CallbackSink>();
    sink->RegisterCallback(
        [summaries](const Logger::LogMessage& msg)
        {
            std::lock_guard<std::mutex> lock(summaries->Mutex);
            ++summaries->AnyLine;
            if (msg.Level == Logger::LogLevel::Info &&
                msg.Message.find("transfer-usage audit:") != Logger::String::npos)
                summaries->Lines.emplace_back(msg.Message);
        });
    Logger::Log::AddSink(std::move(sink));

    // A capture that sees nothing is indistinguishable from a summary that never printed.
    Logger::Log::Info("TextureUsagePolicyTests: capture sink live");
    Logger::Log::Flush();
    ASSERT_GT(summaries->AnyLineCount(), 0) << "the sink saw no log line at all";

    // Settle whatever the tests before this one left summarized, then measure from there.
    TextureUsagePolicy::Report();
    Logger::Log::Flush();
    summaries->Clear();

    const uint64_t settled = TextureUsagePolicy::Snapshot().ObservedUses;

    // One order of magnitude past the settled tally: the reader learns something new.
    const uint64_t grown = std::max<uint64_t>(settled * kSummaryGrowthFactor, 1);
    RecordSyntheticUses(grown - settled);
    TextureUsagePolicy::Report();
    Logger::Log::Flush();
    ASSERT_EQ(1u, summaries->Count()) << "a tally an order of magnitude past the last summary did not reprint";
    EXPECT_EQ(grown, ObservedUsesInSummary(summaries->Last()))
        << "the summary carried a stale tally: " << summaries->Last();
    EXPECT_EQ(TextureUsagePolicy::Snapshot().ObservedUses, ObservedUsesInSummary(summaries->Last()));

    // One more use is not news, and this is the case that keeps hundreds of device
    // teardowns from each printing their own summary.
    summaries->Clear();
    RecordSyntheticUses(1);
    TextureUsagePolicy::Report();
    Logger::Log::Flush();
    EXPECT_EQ(0u, summaries->Count()) << "the summary reprinted for a tally that had barely moved";

    // And the next order of magnitude prints again, with its own numbers.
    const uint64_t grownAgain = grown * kSummaryGrowthFactor;
    RecordSyntheticUses(grownAgain - (grown + 1));
    TextureUsagePolicy::Report();
    Logger::Log::Flush();
    ASSERT_EQ(1u, summaries->Count()) << "the second order of magnitude did not reprint";
    EXPECT_EQ(grownAgain, ObservedUsesInSummary(summaries->Last()))
        << "the summary carried a stale tally: " << summaries->Last();

    Logger::Log::ClearSinks();
    Logger::Log::Initialize({Logger::LogLevel::Info});
}

// Positive and negative control in one process: a texture that omits the transfer
// bits it is actually used with must be harvested, and one that declares them must
// not — while both still produce correct pixels, because the backend ORs the
// transfer bits into every image regardless of what was declared.
TEST(TextureUsagePolicy, HarvestsUndeclaredTransferUseAndSparesDeclaredOne)
{
    EnableAudit();
    ASSERT_TRUE(TextureUsagePolicy::IsAuditEnabled()) << "audit flag resolved before the test set it";
    // The premise of the two controls below: the backend still ORs both transfer
    // bits in, so an undeclared copy succeeds and can be observed rather than
    // crashing. Strict mode is opt-in and must stay off by default on every config.
    ASSERT_FALSE(TextureUsagePolicy::IsStrict()) << "strict mode must not be a default";

    DeviceDesc deviceDesc{};
    deviceDesc.applicationName = "TextureUsagePolicyTests";
    deviceDesc.preferredAPI = GraphicsAPI::Vulkan;
    deviceDesc.enableDynamicRendering = true;
    auto device = DeviceFactory::CreateDevice(deviceDesc);
    if (!device || !device->Initialize(deviceDesc))
    {
        GTEST_SKIP() << "Device init failed";
    }

    constexpr uint32_t kWidth = 64;
    constexpr uint32_t kHeight = 64;
    constexpr size_t kReadbackBytes = static_cast<size_t>(kWidth) * kHeight * 4u;

    const uint32_t conformingUsage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                                     static_cast<uint32_t>(TextureUsage::TransferSrc) |
                                     static_cast<uint32_t>(TextureUsage::TransferDst);
    const uint32_t violatingUsage = static_cast<uint32_t>(TextureUsage::RenderTarget);

    TextureHandle conforming = CreateAuditTexture(*device, conformingUsage, "AuditConformingTex");
    TextureHandle violating = CreateAuditTexture(*device, violatingUsage, "AuditViolatingTex");
    ASSERT_NE(conforming, INVALID_HANDLE);
    ASSERT_NE(violating, INVALID_HANDLE);

    BufferHandle conformingReadback = device->CreateReadbackBuffer(kReadbackBytes);
    BufferHandle violatingReadback = device->CreateReadbackBuffer(kReadbackBytes);
    ASSERT_NE(conformingReadback, INVALID_HANDLE);
    ASSERT_NE(violatingReadback, INVALID_HANDLE);

    auto commandList = device->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(commandList);
    commandList->Begin();
    const float red[4] = {1.f, 0.f, 0.f, 1.f};
    commandList->ClearColorImageSubresource(conforming, 0, 0, red);
    commandList->CopyTextureSubresourceToBuffer(conforming, 0, 0, conformingReadback, kWidth, kHeight, 0, 0, 0, 0);
    commandList->ClearColorImageSubresource(violating, 0, 0, red);
    commandList->CopyTextureSubresourceToBuffer(violating, 0, 0, violatingReadback, kWidth, kHeight, 0, 0, 0, 0);
    commandList->End();

    device->ExecuteCommandLists({commandList.get()});
    device->FinalizeFrame();
    device->WaitForIdle();

    const TextureUsageSnapshot snapshot = TextureUsagePolicy::Snapshot();
    std::cout << "[audit] observed=" << snapshot.ObservedUses << " unresolved=" << snapshot.UnresolvedUses
              << " violations=" << snapshot.ViolationKeys.size() << "\n";
    for (const std::string& key : snapshot.ViolationKeys)
    {
        std::cout << "[audit]   " << key << "\n";
    }

    // The hook ran: silence for the conforming texture is silence, not a dead detector.
    EXPECT_GE(snapshot.ObservedUses, 4u);

    EXPECT_TRUE(HasKey(snapshot.ViolationKeys, "AuditViolatingTex#TransferDst"))
        << "clear of an image that never declared TransferDst was not harvested";
    EXPECT_TRUE(HasKey(snapshot.ViolationKeys, "AuditViolatingTex#TransferSrc"))
        << "readback of an image that never declared TransferSrc was not harvested";
    EXPECT_FALSE(HasKeyPrefixed(snapshot.ViolationKeys, "AuditConformingTex"))
        << "a correctly declared texture was reported as a violation";

    // The audit is passive: both copies still land, undeclared bits and all.
    for (BufferHandle readback : {conformingReadback, violatingReadback})
    {
        const uint8_t* pixels = static_cast<const uint8_t*>(device->MapBuffer(readback));
        ASSERT_NE(pixels, nullptr);
        EXPECT_GE(pixels[0], 200);
        EXPECT_LE(pixels[1], 80);
        EXPECT_LE(pixels[2], 80);
        EXPECT_GE(pixels[3], 200);
        device->UnmapBuffer(readback);
    }

    device->DestroyBuffer(conformingReadback);
    device->DestroyBuffer(violatingReadback);
    device->DestroyTexture(conforming);
    device->DestroyTexture(violating);
}

// CreateTexture asks IsTextureFormatSupported before allocating. A depth format
// requested as a colour attachment is the cheapest thing every backend must
// refuse, and in developer configs the refusal is the create failing, not a
// resource the API rejects later. The control shows the gate is not simply
// refusing everything.
TEST(TextureUsagePolicy, CreateTextureRefusesWhatTheFormatQueryRefuses)
{
    EnableAudit();
    DeviceDesc deviceDesc{};
    deviceDesc.applicationName = "TextureUsagePolicyTests";
    deviceDesc.preferredAPI = GraphicsAPI::Vulkan;
    deviceDesc.enableDynamicRendering = true;
    auto device = DeviceFactory::CreateDevice(deviceDesc);
    if (!device || !device->Initialize(deviceDesc))
        GTEST_SKIP() << "Device init failed";

    const uint32_t colourUsage =
        static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    ASSERT_FALSE(device->IsTextureFormatSupported(TextureFormat::D32_FLOAT, colourUsage))
        << "the query accepts a depth format as a colour attachment, so the gate has nothing to refuse";

    TextureDesc refused{};
    refused.width = 4;
    refused.height = 4;
    refused.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    refused.usage = colourUsage;
    refused.debugName = "GateRefusedTex";
    const TextureHandle refusedHandle = device->CreateTexture(refused);
#if defined(GE_DEV_DIAG)
    EXPECT_FALSE(refusedHandle.IsValid())
        << "a developer build created a texture its own format query reports as unsupported";
#else
    if (refusedHandle.IsValid())
        device->DestroyTexture(refusedHandle);
#endif

    TextureDesc control = refused;
    control.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    control.debugName = "GateControlTex";
    const TextureHandle controlHandle = device->CreateTexture(control);
    EXPECT_TRUE(controlHandle.IsValid()) << "the gate refused a plain RGBA8 colour target";
    if (controlHandle.IsValid())
        device->DestroyTexture(controlHandle);

    device->WaitForIdle();
    device->Shutdown();
}

// The render-graph shape of the drift the audit exists for: a pool import
// captured by a declared CopySrc read. The import's physical is created before
// the copy is declared, so the frame can only widen it for the NEXT
// materialization; an output policy that states the requirement a frame ahead
// makes the first capture copy a correctly declared image. The control arm —
// the same shape with nothing stated ahead — is reported, proving the detector
// sees this exact path rather than staying silent on it.
TEST(TextureUsagePolicy, PoolImportRequiredAheadOfItsCaptureIsNotReported)
{
    EnableAudit();
    ASSERT_TRUE(TextureUsagePolicy::IsAuditEnabled()) << "audit flag resolved before the test set it";

    DeviceDesc deviceDesc{};
    deviceDesc.applicationName = "TextureUsagePolicyTests";
    deviceDesc.preferredAPI = GraphicsAPI::Vulkan;
    deviceDesc.enableDynamicRendering = true;
    deviceDesc.enableSwapchain = false;
    auto device = DeviceFactory::CreateDevice(deviceDesc);
    if (!device || !device->Initialize(deviceDesc))
    {
        GTEST_SKIP() << "Device init failed";
    }

    constexpr uint32_t kWidth = 64;
    constexpr uint32_t kHeight = 64;
    constexpr size_t kRowPitch = static_cast<size_t>(kWidth) * 4u;
    constexpr size_t kReadbackBytes = kRowPitch * kHeight;

    RenderGraph::RGResourcePool persistent(device.get());
    RenderGraph::RGTransientPool transient(device.get());
    RenderGraph::RGUploadRing ring(device.get(), 2, 4096);
    RenderGraph::RGFrame frame(device.get(), &persistent, &transient, &ring);

    // One frame of the pipeline-output shape: a pool import cleared to red and
    // exported, optionally required ahead, optionally captured this frame.
    auto renderFrame = [&](uint64_t frameIndex, const char* poolName, bool requireAhead,
                           BufferHandle captureInto)
    {
        TextureDesc desc{};
        desc.width = kWidth;
        desc.height = kHeight;
        desc.mipLevels = 1;
        desc.arrayLayers = 1;
        desc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        desc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
        desc.debugName = poolName;

        frame.BeginFrame(frameIndex);
        const RenderGraph::RGTexture output = frame.ImportPersistentTexture(poolName, desc);
        frame.AddPass(
            "Fill", 0,
            [&](RenderGraph::RGPassBuilder& p)
            {
                RenderGraph::RGAttachmentOps ops{};
                ops.Load = RenderGraph::RGLoadOp::Clear;
                ops.Store = RenderGraph::RGStoreOp::Store;
                ops.Clear.Color[0] = 1.0f;
                ops.Clear.Color[3] = 1.0f;
                p.AttachColor(0, output, ops);
            },
            [](RenderGraph::RGContext&) {});
        frame.MarkOutput(output, RenderGraph::RGImageLayout::ShaderReadOnly);
        if (requireAhead)
            frame.RequireTransferUsage(output, TextureUsage::TransferSrc);
        if (captureInto.IsValid())
        {
            frame.AddPass(
                "Capture", 1,
                [&](RenderGraph::RGPassBuilder& p)
                {
                    p.Read(output, RenderGraph::RGTextureRead::CopySrc);
                    p.PreventCulling();
                },
                [output, captureInto](RenderGraph::RGContext& ctx)
                {
                    ctx.Cmd->CopyTextureToBuffer(ctx.GetTexture(output), captureInto, kWidth, kHeight, 0,
                                                 0, 0, kRowPitch);
                });
        }
        frame.Execute();
    };

    const BufferHandle requiredReadback = device->CreateReadbackBuffer(kReadbackBytes);
    const BufferHandle controlReadback = device->CreateReadbackBuffer(kReadbackBytes);
    ASSERT_NE(requiredReadback, INVALID_HANDLE);
    ASSERT_NE(controlReadback, INVALID_HANDLE);

    renderFrame(0, "Policy.RequiredOutput", /*requireAhead*/ true, BufferHandle{});
    renderFrame(1, "Policy.RequiredOutput", /*requireAhead*/ true, requiredReadback);
    renderFrame(2, "Policy.ControlOutput", /*requireAhead*/ false, BufferHandle{});
    renderFrame(3, "Policy.ControlOutput", /*requireAhead*/ false, controlReadback);
    frame.WaitForPendingWork();
    device->WaitForIdle();

    const TextureUsageSnapshot snapshot = TextureUsagePolicy::Snapshot();
    EXPECT_TRUE(HasKey(snapshot.ViolationKeys, "Policy.ControlOutput#TransferSrc"))
        << "control: the capture of an import nothing widened ahead must be reported";
    EXPECT_FALSE(HasKeyPrefixed(snapshot.ViolationKeys, "Policy.RequiredOutput"))
        << "an import required ahead of its capture was reported";

    // The widened image is the one the capture read: the clear landed.
    const uint8_t* pixels = static_cast<const uint8_t*>(device->MapBuffer(requiredReadback));
    ASSERT_NE(pixels, nullptr);
    EXPECT_GE(pixels[0], 200);
    EXPECT_LE(pixels[1], 80);
    EXPECT_LE(pixels[2], 80);
    EXPECT_GE(pixels[3], 200);
    device->UnmapBuffer(requiredReadback);

    device->DestroyBuffer(requiredReadback);
    device->DestroyBuffer(controlReadback);
}
