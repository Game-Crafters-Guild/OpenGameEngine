// GPU resource lifetime: destruction is always deferred outside teardown.
//
// The device destroys buffers/textures/views/samplers by queueing them against
// the value each queue's next submit will signal, or against a frame slot's
// fence on a device with no timeline semaphore. Nothing in the runtime bypasses
// that. These tests pin the observable contract via IDevice::GetResourcePoolStats.

#include <gtest/gtest.h>

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{
void SetEnvVar(const char* key, const char* value)
{
#if defined(_WIN32)
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

// Create + initialize a headless Vulkan device, or nullptr if none is available.
std::unique_ptr<IDevice> MakeHeadlessDevice()
{
    SetEnvVar("GE_HEADLESS_TEST", "1");
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd))
    {
        return nullptr;
    }
    return dev;
}

// One submitted frame, so the queues have GPU work for a destroy to outlive.
void RunOneFrame(IDevice& dev)
{
    if (!dev.BeginFrame())
    {
        return;
    }
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

TextureHandle MakeTexture(IDevice& dev)
{
    TextureDesc td{};
    td.width = 4;
    td.height = 4;
    td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource);
    return dev.CreateTexture(td);
}

BufferHandle MakeBuffer(IDevice& dev)
{
    BufferDesc bd{};
    bd.size = 256;
    bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    return dev.CreateBuffer(bd);
}

// Device-local copy destination: UpdateBuffer cannot map this, so it must route
// through a staging buffer and defer that staging buffer's destruction.
BufferHandle MakeDeviceLocalBuffer(IDevice& dev, size_t size)
{
    BufferDesc bd{};
    bd.size = size;
    bd.usage = static_cast<uint32_t>(BufferUsage::Storage) | static_cast<uint32_t>(BufferUsage::TransferDst);
    bd.memoryUsage = BufferMemoryUsage::DeviceLocal;
    return dev.CreateBuffer(bd);
}

SamplerHandle MakeSampler(IDevice& dev)
{
    SamplerDesc sd{};
    return dev.CreateSampler(sd);
}
} // namespace

// Destroying a resource used by an in-flight submission must outlive that
// submission. Original coverage for the timeline-deferred path.
TEST(DeferredDestructionTimeline, DestroyAfterSubmit_IsGPUChronologicallySafe)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "no Vulkan device available";
    }

    // Offscreen texture we actually render to.
    TextureDesc td{};
    td.width = 64;
    td.height = 64;
    td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    td.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    TextureHandle tex = dev->CreateTexture(td);
    ASSERT_TRUE(tex.IsValid());

    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl != nullptr);
    cl->Begin();
    RenderPassDesc rp{};
    rp.colorTargets[0] = tex;
    rp.colorTargetCount = 1;
    rp.clearColor[0] = true;
    rp.clearColorValue[0][3] = 1.0f;
    cl->BeginRenderPass(rp);
    cl->EndRenderPass();
    cl->End();

    std::vector<CommandList*> lists{cl.get()};
    dev->ExecuteCommandLists(lists);

    // Immediately destroy: the backend must defer past the submission.
    dev->DestroyTexture(tex);

    ASSERT_TRUE(dev->BeginFrame());
    dev->Present();
    dev->WaitForIdle();
}

// A full GPU drain does not license immediate destruction. The deferred queue
// frees these on its next pass anyway, so the shortcut bought ~one frame of
// latency and cost the guarantee.
TEST(DeferredDestructionTimeline, DestroyAfterDrainStillDefers)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "no Vulkan device available";
    }

    // Advance the graphics timeline, then drain: the state in which the device
    // knows it is idle.
    RunOneFrame(*dev);
    dev->WaitForIdle();

    const IDevice::ResourcePoolStats before = dev->GetResourcePoolStats();

    TextureHandle tex = MakeTexture(*dev);
    BufferHandle buf = MakeBuffer(*dev);
    SamplerHandle smp = MakeSampler(*dev);
    ASSERT_TRUE(tex.IsValid());
    ASSERT_TRUE(buf.IsValid());
    ASSERT_TRUE(smp.IsValid());

    TextureViewDesc vd{};
    TextureViewHandle view = dev->CreateTextureView(tex, vd);
    ASSERT_TRUE(view.IsValid());

    dev->DestroyTextureView(view);
    dev->DestroySampler(smp);
    dev->DestroyTexture(tex);
    dev->DestroyBuffer(buf);

    const IDevice::ResourcePoolStats after = dev->GetResourcePoolStats();
    EXPECT_EQ(after.deferredTextures, before.deferredTextures + 1);
    EXPECT_EQ(after.deferredBuffers, before.deferredBuffers + 1);
    EXPECT_EQ(after.deferredTextureViews, before.deferredTextureViews + 1);
    EXPECT_EQ(after.deferredSamplers, before.deferredSamplers + 1);

    dev->WaitForIdle();
}

// Before the first submit the tags name values no queue has signaled yet, so
// every resource type is still deferred rather than freed outright — samplers
// included.
TEST(DeferredDestructionTimeline, DestroyBeforeFirstSubmitStillDefers)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "no Vulkan device available";
    }

    // Deliberately no frame has been submitted yet.
    const IDevice::ResourcePoolStats before = dev->GetResourcePoolStats();

    TextureHandle tex = MakeTexture(*dev);
    BufferHandle buf = MakeBuffer(*dev);
    SamplerHandle smp = MakeSampler(*dev);
    ASSERT_TRUE(tex.IsValid());
    ASSERT_TRUE(buf.IsValid());
    ASSERT_TRUE(smp.IsValid());

    TextureViewDesc vd{};
    TextureViewHandle view = dev->CreateTextureView(tex, vd);
    ASSERT_TRUE(view.IsValid());

    dev->DestroyTextureView(view);
    dev->DestroySampler(smp);
    dev->DestroyTexture(tex);
    dev->DestroyBuffer(buf);

    const IDevice::ResourcePoolStats after = dev->GetResourcePoolStats();
    EXPECT_EQ(after.deferredTextures, before.deferredTextures + 1);
    EXPECT_EQ(after.deferredBuffers, before.deferredBuffers + 1);
    EXPECT_EQ(after.deferredTextureViews, before.deferredTextureViews + 1);
    EXPECT_EQ(after.deferredSamplers, before.deferredSamplers + 1);

    dev->WaitForIdle();
}

// A frame slot whose fence was never armed has nothing in flight, so BeginFrame
// must drain the resources parked on it. Otherwise they survive until the next
// full device drain — and runtime code no longer performs one.
TEST(DeferredDestructionTimeline, UnarmedFrameFenceStillDrainsAtBeginFrame)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "no Vulkan device available";
    }

    // No submit yet -> the destroy parks on frame slot 0, whose fence is unarmed.
    TextureHandle tex = MakeTexture(*dev);
    BufferHandle buf = MakeBuffer(*dev);
    ASSERT_TRUE(tex.IsValid());
    ASSERT_TRUE(buf.IsValid());
    dev->DestroyTexture(tex);
    dev->DestroyBuffer(buf);

    const IDevice::ResourcePoolStats parked = dev->GetResourcePoolStats();
    ASSERT_GE(parked.deferredTextures, 1u);
    ASSERT_GE(parked.deferredBuffers, 1u);

    ASSERT_TRUE(dev->BeginFrame());

    const IDevice::ResourcePoolStats drained = dev->GetResourcePoolStats();
    EXPECT_EQ(drained.deferredTextures, parked.deferredTextures - 1);
    EXPECT_EQ(drained.deferredBuffers, parked.deferredBuffers - 1);

    dev->Present();
    dev->WaitForIdle();
}

// The destroy entry points are reached from JobSystem workers in the default
// config (ECS extraction resolves bindless textures off the render thread), so
// the deferral queues must tolerate concurrent producers. Every handed-in
// handle must land in a queue: a lost or duplicated push shows up as a count
// mismatch, a corrupted one as a crash.
TEST(DeferredDestructionTimeline, ConcurrentDestroysQueueEveryHandle)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "no Vulkan device available";
    }

    RunOneFrame(*dev);
    dev->WaitForIdle();

    constexpr int kThreads = 8;
    constexpr int kPerThread = 64;

    std::vector<std::vector<TextureHandle>> textures(kThreads);
    std::vector<std::vector<BufferHandle>> buffers(kThreads);
    std::vector<std::vector<SamplerHandle>> samplers(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        for (int i = 0; i < kPerThread; ++i)
        {
            TextureHandle tex = MakeTexture(*dev);
            BufferHandle buf = MakeBuffer(*dev);
            SamplerHandle smp = MakeSampler(*dev);
            ASSERT_TRUE(tex.IsValid());
            ASSERT_TRUE(buf.IsValid());
            ASSERT_TRUE(smp.IsValid());
            textures[t].push_back(tex);
            buffers[t].push_back(buf);
            samplers[t].push_back(smp);
        }
    }

    const IDevice::ResourcePoolStats before = dev->GetResourcePoolStats();

    std::atomic<int> ready{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back(
            [&, t]()
            {
                // Start together so the pushes actually overlap.
                ready.fetch_add(1);
                while (ready.load() < kThreads)
                {
                    std::this_thread::yield();
                }
                for (int i = 0; i < kPerThread; ++i)
                {
                    dev->DestroyTexture(textures[t][i]);
                    dev->DestroyBuffer(buffers[t][i]);
                    dev->DestroySampler(samplers[t][i]);
                }
            });
    }
    for (auto& w : workers)
    {
        w.join();
    }

    constexpr size_t kTotal = static_cast<size_t>(kThreads) * kPerThread;
    const IDevice::ResourcePoolStats after = dev->GetResourcePoolStats();
    EXPECT_EQ(after.deferredTextures, before.deferredTextures + kTotal);
    EXPECT_EQ(after.deferredBuffers, before.deferredBuffers + kTotal);
    EXPECT_EQ(after.deferredSamplers, before.deferredSamplers + kTotal);

    dev->WaitForIdle();
}

// Upload staging buffers retire on the transfer timeline rather than on the
// per-queue destroy tags, so they carry their own depth. This pins that depth as
// an instrument: it must climb one entry per staged upload and return to its
// baseline once the GPU passes the value and a frame runs the retire sweep. A
// staging retire path that stops retiring shows up here and nowhere else.
TEST(DeferredDestructionTimeline, StagingBufferDepthClimbsThenRetiresToBaseline)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "no Vulkan device available";
    }

    RunOneFrame(*dev);
    dev->WaitForIdle();
    ASSERT_TRUE(dev->BeginFrame());
    dev->Present();
    dev->WaitForIdle();

    const IDevice::ResourcePoolStats before = dev->GetResourcePoolStats();

    constexpr size_t kUploads = 5;
    constexpr size_t kBufferBytes = 256;
    const std::vector<uint8_t> payload(kBufferBytes, 0xAB);

    std::vector<BufferHandle> targets;
    for (size_t i = 0; i < kUploads; ++i)
    {
        BufferHandle dst = MakeDeviceLocalBuffer(*dev, kBufferBytes);
        ASSERT_TRUE(dst.IsValid());
        targets.push_back(dst);
        dev->UpdateBuffer(dst, 0, kBufferBytes, payload.data());
    }

    const IDevice::ResourcePoolStats queued = dev->GetResourcePoolStats();

    // Whether the staging path ran is decided by liveBuffers, never by the
    // counter under test: a staged upload creates a second live buffer per
    // target, a mapped one creates none. Keying the assertion on deferred-
    // StagingBuffers instead would let a broken accessor silently pass.
    // A DeviceLocal buffer always stages: CreateBuffer records
    // VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT alone for it and nothing ever
    // re-reads the allocation's real memory properties, so device class does
    // not enter into it.
    const size_t liveDelta = queued.liveBuffers - before.liveBuffers;
    ASSERT_EQ(liveDelta, 2 * kUploads) << "expected one staging buffer per target";

    EXPECT_EQ(queued.deferredStagingBuffers, before.deferredStagingBuffers + kUploads);

    // Let the GPU pass the transfer timeline value, then run the frame that
    // performs the retire sweep.
    dev->WaitForIdle();
    ASSERT_TRUE(dev->BeginFrame());
    dev->Present();
    dev->WaitForIdle();

    EXPECT_EQ(dev->GetResourcePoolStats().deferredStagingBuffers, before.deferredStagingBuffers);

    for (BufferHandle h : targets)
    {
        dev->DestroyBuffer(h);
    }
    dev->WaitForIdle();
}

// The staging buffer holds the payload, not the payload plus the destination
// offset. The copy reads it from offset 0 and writes the destination at
// `offset`, so bytes below `offset` in a size+offset staging buffer are never
// written and never read — pure host-visible waste, proportional to how far
// into the destination the write lands.
TEST(DeferredDestructionTimeline, UpdateBufferStagingSizeIsPayloadNotOffset)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "no Vulkan device available";
    }

    constexpr size_t kPayloadBytes = 64u * 1024u;
    constexpr size_t kWriteOffset  = 4u * 1024u * 1024u;

    BufferDesc bd{};
    bd.size = kWriteOffset + kPayloadBytes;
    bd.usage = static_cast<uint32_t>(BufferUsage::Storage)
               | static_cast<uint32_t>(BufferUsage::TransferDst)
               | static_cast<uint32_t>(BufferUsage::TransferSrc);
    bd.memoryUsage = BufferMemoryUsage::DeviceLocal;
    bd.debugName = "StagingSize.Dst";
    const BufferHandle dst = dev->CreateBuffer(bd);
    ASSERT_TRUE(dst.IsValid());

    std::vector<uint8_t> payload(kPayloadBytes);
    for (size_t i = 0; i < payload.size(); ++i)
    {
        payload[i] = static_cast<uint8_t>(i * 31u + 7u);
    }

    const IDevice::ResourcePoolStats before = dev->GetResourcePoolStats();
    const size_t bytesBefore = dev->DebugGetBufferBytes();

    dev->UpdateBuffer(dst, kWriteOffset, kPayloadBytes, payload.data());

    const IDevice::ResourcePoolStats queued = dev->GetResourcePoolStats();
    // Path arm: the staged branch ran, so the size arm below is measuring a
    // staging allocation rather than a mapped write that allocated nothing.
    ASSERT_EQ(queued.liveBuffers, before.liveBuffers + 1u) << "no staging buffer was created";
    ASSERT_EQ(queued.deferredStagingBuffers, before.deferredStagingBuffers + 1u);

    // Size arm. VmaAllocationInfo::size is padded to allocation alignment, so
    // this is a band rather than an equality; the offset-sized allocation
    // overshoots it by ~64x.
    const size_t stagingBytes = dev->DebugGetBufferBytes() - bytesBefore;
    EXPECT_LT(stagingBytes, 2u * kPayloadBytes)
        << "staging allocation is sized by the destination offset, not the payload";

    // Contents arm: the payload has to land at `offset` in the destination, so
    // a smaller staging buffer cannot be paying for itself with a truncated or
    // misplaced copy.
    dev->WaitForIdle();
    const BufferHandle readback = dev->CreateReadbackBuffer(kPayloadBytes, "StagingSize.Read");
    ASSERT_TRUE(readback.IsValid());
    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl != nullptr);
    cl->Begin();
    cl->CopyBuffer(dst, readback, kPayloadBytes, kWriteOffset, 0);
    cl->End();
    {
        std::vector<CommandList*> lists{cl.get()};
        dev->ExecuteCommandLists(lists);
        dev->WaitForIdle();
    }
    const void* mapped = dev->MapBuffer(readback);
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(std::memcmp(mapped, payload.data(), kPayloadBytes), 0)
        << "the payload did not land at the destination offset";
    dev->UnmapBuffer(readback);

    dev->DestroyBuffer(readback);
    dev->DestroyBuffer(dst);
    dev->WaitForIdle();
}
