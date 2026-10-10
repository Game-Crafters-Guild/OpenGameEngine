// GPU sort coverage for the sorted transparent path (transparency-scale S1/S2).
//
// Three layers:
//  1. CPU cross-checks on the 64-bit key packing (SortedTransparentKey.h) — the
//     monotonic float transform, back-to-front ordering, and the deterministic
//     index tiebreak that keeps equal-depth draws flicker-free (Risk 4 / F4).
//  2. A device-backed dispatch of the real sorted_transparent_sort.comp over a
//     crafted key/value array, asserting the GPU result is byte-identical to a
//     CPU std::sort of the same pairs — the "GPU sort matches the CPU sort"
//     ship-gate, exercised on real hardware.
//  3. A device-backed dispatch of the fused runtime drain
//     (sorted_transparent_drain.comp): crafted records + GPUInstance centres in,
//     per-run VkDrawIndexedIndirectCommand streams out, asserting the GPU
//     stream is byte-identical to the CPU twin (BuildSortedTransparentCpuOrder)
//     — which also cross-checks the GLSL key packing against the C++ one —
//     plus the F6 bounds contract (full capacity, tail untouched).

#include <gtest/gtest.h>

#include "Engine/Rendering/SortedTransparentKey.h"
#include "Engine/Rendering/SortedTransparentRuns.h"

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include "TestDeviceHelper.h"

#include <algorithm>
#include <cstring>
#include <random>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
namespace R = GameEngine::Engine::Renderer;

namespace
{
// Must match kCapacity in sorted_transparent_sort.comp /
// sorted_transparent_drain.comp (locked C++-side by
// kSortedTransparentSortCapacity).
constexpr uint32_t kCapacity = R::kSortedTransparentSortCapacity;

struct GpuKey
{
    uint32_t lo; // uvec2.x
    uint32_t hi; // uvec2.y
};
} // namespace

// --- CPU key-packing cross-checks -------------------------------------------

TEST(SortedTransparentKey, MonotonicFloatTransformOrdersLikeFloat)
{
    const float samples[] = {-1e30f, -100.0f, -1.5f, -0.0f, 0.0f, 0.5f, 1.0f, 100.0f, 1e30f};
    for (size_t i = 1; i < std::size(samples); ++i)
    {
        // Strictly increasing floats -> non-decreasing sortable uints.
        EXPECT_LE(R::FloatToSortableUint(samples[i - 1]), R::FloatToSortableUint(samples[i]))
            << "i=" << i;
    }
    // -0.0f orders immediately before +0.0f (a harmless, monotonically
    // consistent quirk of the transform), never after.
    EXPECT_LE(R::FloatToSortableUint(-0.0f), R::FloatToSortableUint(0.0f));
}

TEST(SortedTransparentKey, AscendingKeyOrderIsBackToFront)
{
    // Farther (larger viewDepth) must produce a SMALLER key so an ascending sort
    // draws it first (back-to-front).
    const auto near = R::MakeSortedTransparentKey(2.0f, 0, 0);
    const auto mid = R::MakeSortedTransparentKey(5.0f, 0, 0);
    const auto far = R::MakeSortedTransparentKey(9.0f, 0, 0);
    EXPECT_TRUE(R::SortedTransparentKeyLess(far, mid));
    EXPECT_TRUE(R::SortedTransparentKeyLess(mid, near));
}

TEST(SortedTransparentKey, EqualDepthBreaksByIndexDeterministically)
{
    const auto a = R::MakeSortedTransparentKey(4.0f, 0, 10);
    const auto b = R::MakeSortedTransparentKey(4.0f, 0, 20);
    // Same depth+priority -> lower index sorts first, and the order is total.
    EXPECT_TRUE(R::SortedTransparentKeyLess(a, b));
    EXPECT_FALSE(R::SortedTransparentKeyLess(b, a));
}

TEST(SortedTransparentKey, PriorityDominatesDepth)
{
    const auto lowPrioFar = R::MakeSortedTransparentKey(1000.0f, 0, 0);
    const auto highPrioNear = R::MakeSortedTransparentKey(0.1f, 1, 0);
    // Priority is the high field: priority 0 sorts entirely before priority 1
    // regardless of depth.
    EXPECT_TRUE(R::SortedTransparentKeyLess(lowPrioFar, highPrioNear));
}

// --- Device-backed sort ------------------------------------------------------

namespace
{
class GPUSortComputeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";

        ShaderPackage pkg{};
        std::string err;
        if (!LoadShaderPkg("Shaders/sorted_transparent_sort.shaderpkg", ShaderSourceKind::SpirV, pkg, &err))
            FAIL() << "sorted_transparent_sort.shaderpkg not available: " << err;
        auto itCs = pkg.stageBytes.find("cs");
        ASSERT_NE(itCs, pkg.stageBytes.end());

        m_Layout.debugName = "GPUSort_SetLayout";
        for (uint32_t i = 0; i < 2u; ++i)
        {
            DescriptorBinding b{};
            b.binding = i;
            b.type = DescriptorType::StorageBuffer;
            b.count = 1u;
            b.shaderStages = kShaderStageCompute;
            m_Layout.bindings.push_back(b);
        }
        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(itCs->second);
        cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        cd.DebugName = "SortedTransparentSort";
        const auto id = m_Device->InternComputePipeline(cd);
        m_Pipeline = m_Device->GetOrCreateComputePipeline(id);
        ASSERT_TRUE(m_Pipeline.IsValid());
    }

    void TearDown() override
    {
        for (BufferHandle& b : m_Buffers)
            if (b.IsValid())
                m_Device->DestroyBuffer(b);
        if (m_Device)
            m_Device->Shutdown();
    }

    BufferHandle MakeHostBuffer(size_t bytes, const char* name)
    {
        BufferDesc d{};
        d.size = std::max<size_t>(bytes, 16);
        d.usage = static_cast<uint32_t>(BufferUsage::Storage) |
                  static_cast<uint32_t>(BufferUsage::TransferDst);
        d.memoryUsage = BufferMemoryUsage::Upload;
        d.debugName = name;
        BufferHandle h = m_Device->CreateBuffer(d);
        m_Buffers.push_back(h);
        return h;
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    std::vector<BufferHandle> m_Buffers;
};
} // namespace

TEST_F(GPUSortComputeTest, MatchesCpuSortBackToFront)
{
    // 700 "live" transparents at pseudo-random depths, plus padding to capacity.
    // Live count deliberately exceeds the old 256 ceiling to prove the GPU path
    // has no such limit.
    constexpr uint32_t kLive = 700u;
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> depthDist(0.5f, 5000.0f);

    std::vector<GpuKey> keys(kCapacity);
    std::vector<uint32_t> values(kCapacity);

    // Reference pairs for the CPU sort.
    std::vector<std::pair<R::SortedTransparentKey, uint32_t>> ref;
    ref.reserve(kLive);

    for (uint32_t i = 0; i < kCapacity; ++i)
    {
        if (i < kLive)
        {
            const float depth = depthDist(rng);
            const R::SortedTransparentKey k =
                R::MakeSortedTransparentKey(depth, 0, static_cast<uint16_t>(i & 0xFFFFu));
            keys[i] = {k.Lo, k.Hi};
            values[i] = i;
            ref.push_back({k, i});
        }
        else
        {
            keys[i] = {0xFFFFFFFFu, 0xFFFFFFFFu}; // padding sorts to the end
            values[i] = 0xFFFFFFFFu;
        }
    }

    std::stable_sort(ref.begin(), ref.end(), [](const auto& a, const auto& b)
                     { return R::SortedTransparentKeyLess(a.first, b.first); });

    BufferHandle keyBuf = MakeHostBuffer(kCapacity * sizeof(GpuKey), "Sort.Keys");
    BufferHandle valBuf = MakeHostBuffer(kCapacity * sizeof(uint32_t), "Sort.Values");
    {
        void* m = m_Device->MapBuffer(keyBuf);
        ASSERT_NE(m, nullptr);
        std::memcpy(m, keys.data(), kCapacity * sizeof(GpuKey));
        m_Device->UnmapBuffer(keyBuf);
        m = m_Device->MapBuffer(valBuf);
        ASSERT_NE(m, nullptr);
        std::memcpy(m, values.data(), kCapacity * sizeof(uint32_t));
        m_Device->UnmapBuffer(valBuf);
    }

    DescriptorSetDesc setDesc{};
    setDesc.layout = m_Layout;
    setDesc.transient = true;
    setDesc.debugName = "Sort.DS";
    DescriptorSetHandle ds = m_Device->CreateDescriptorSet(setDesc);
    m_Device->UpdateStorageBufferBinding(ds, 0, keyBuf, 0, kCapacity * sizeof(GpuKey));
    m_Device->UpdateStorageBufferBinding(ds, 1, valBuf, 0, kCapacity * sizeof(uint32_t));

    auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->SetPipeline(m_Pipeline);
    cl->BindDescriptorSet(0, ds, m_Pipeline);
    cl->Dispatch(1, 1, 1); // single-workgroup bitonic
    cl->End();
    m_Device->ExecuteCommandLists({cl.get()});
    m_Device->WaitForIdle();

    std::vector<GpuKey> outKeys(kCapacity);
    std::vector<uint32_t> outVals(kCapacity);
    {
        void* m = m_Device->MapBuffer(keyBuf);
        ASSERT_NE(m, nullptr);
        std::memcpy(outKeys.data(), m, kCapacity * sizeof(GpuKey));
        m_Device->UnmapBuffer(keyBuf);
        m = m_Device->MapBuffer(valBuf);
        ASSERT_NE(m, nullptr);
        std::memcpy(outVals.data(), m, kCapacity * sizeof(uint32_t));
        m_Device->UnmapBuffer(valBuf);
    }

    // Keys must be globally non-decreasing.
    for (uint32_t i = 1; i < kCapacity; ++i)
    {
        const R::SortedTransparentKey a{outKeys[i - 1].lo, outKeys[i - 1].hi};
        const R::SortedTransparentKey b{outKeys[i].lo, outKeys[i].hi};
        ASSERT_FALSE(R::SortedTransparentKeyLess(b, a)) << "keys not ascending at " << i;
    }

    // The first kLive entries must equal the CPU sort element-by-element (keys
    // are distinct via the index tiebreak, so the order is total).
    for (uint32_t i = 0; i < kLive; ++i)
    {
        EXPECT_EQ(outKeys[i].lo, ref[i].first.Lo) << "lo mismatch at " << i;
        EXPECT_EQ(outKeys[i].hi, ref[i].first.Hi) << "hi mismatch at " << i;
        EXPECT_EQ(outVals[i], ref[i].second) << "value/payload mismatch at " << i;
    }
    // Padding stays at the tail.
    for (uint32_t i = kLive; i < kCapacity; ++i)
    {
        EXPECT_EQ(outKeys[i].lo, 0xFFFFFFFFu);
        EXPECT_EQ(outKeys[i].hi, 0xFFFFFFFFu);
    }
}

// --- Device-backed fused drain (sorted_transparent_drain.comp) ---------------

namespace
{
// Mirrors the drain shader's DrainPC block.
struct DrainPC
{
    float CameraPos[3];
    uint32_t LiveCount;
    float CameraForward[3];
    uint32_t RunCount;
};
static_assert(sizeof(DrainPC) == 32);

constexpr uint32_t kArgsStride = 5u; // uints per VkDrawIndexedIndirectCommand
constexpr uint32_t kArgsSentinel = 0xDEADBEEFu;

class GPUDrainComputeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";

        ShaderPackage pkg{};
        std::string err;
        if (!LoadShaderPkg("Shaders/sorted_transparent_drain.shaderpkg", ShaderSourceKind::SpirV, pkg, &err))
            FAIL() << "sorted_transparent_drain.shaderpkg not available: " << err;
        auto itCs = pkg.stageBytes.find("cs");
        ASSERT_NE(itCs, pkg.stageBytes.end());

        m_Layout.debugName = "GPUDrain_SetLayout";
        for (uint32_t i = 0; i < 6u; ++i)
        {
            DescriptorBinding b{};
            b.binding = i;
            b.type = DescriptorType::StorageBuffer;
            b.count = 1u;
            b.shaderStages = kShaderStageCompute;
            m_Layout.bindings.push_back(b);
        }
        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(itCs->second);
        cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        cd.PushConstants.Size = sizeof(DrainPC);
        cd.PushConstants.StageMask = kShaderStageCompute;
        cd.DebugName = "SortedTransparentDrainTest";
        const auto id = m_Device->InternComputePipeline(cd);
        m_Pipeline = m_Device->GetOrCreateComputePipeline(id);
        ASSERT_TRUE(m_Pipeline.IsValid());
    }

    void TearDown() override
    {
        for (BufferHandle& b : m_Buffers)
            if (b.IsValid())
                m_Device->DestroyBuffer(b);
        if (m_Device)
            m_Device->Shutdown();
    }

    BufferHandle MakeHostBuffer(size_t bytes, const char* name)
    {
        BufferDesc d{};
        d.size = std::max<size_t>(bytes, 16);
        d.usage = static_cast<uint32_t>(BufferUsage::Storage) |
                  static_cast<uint32_t>(BufferUsage::TransferDst);
        d.memoryUsage = BufferMemoryUsage::Upload;
        d.debugName = name;
        BufferHandle h = m_Device->CreateBuffer(d);
        m_Buffers.push_back(h);
        return h;
    }

    void UploadBytes(BufferHandle buf, const void* src, size_t bytes)
    {
        void* m = m_Device->MapBuffer(buf);
        ASSERT_NE(m, nullptr);
        std::memcpy(m, src, bytes);
        m_Device->UnmapBuffer(buf);
    }

    // Craft liveCount records spread over runCount runs at pseudo-random
    // integer depths (integer centre.z + camera at the origin looking down +Z
    // makes the CPU and GPU depth computations bit-identical, so the stream
    // compare below is exact, not approximate). Dispatches the drain once and
    // asserts the GPU-written args/counts/indirection streams are
    // byte-identical to the CPU twin's.
    void RunDrainAndExpectCpuTwin(uint32_t liveCount, uint32_t runCount, uint32_t seed,
                                  int32_t depthMin = 1, int32_t depthMax = 60000)
    {
        std::mt19937 rng(seed);
        std::uniform_int_distribution<int32_t> depth(depthMin, depthMax);
        std::uniform_int_distribution<uint32_t> group(0, runCount - 1u);

        // GPUInstance table: record i uses instance (i * 3 + 1) so the
        // indirection payload is distinguishable from the record index.
        std::vector<GPUInstance> instances(liveCount * 3u + 2u);
        std::vector<R::SortedTransparentRecord> records(liveCount);
        std::vector<R::SortedTransparentRecordGPU> recordsGPU(liveCount);
        for (uint32_t i = 0; i < liveCount; ++i)
        {
            const uint32_t instanceIndex = i * 3u + 1u;
            const float z = static_cast<float>(depth(rng));
            instances[instanceIndex] = GPUInstance{};
            instances[instanceIndex].boundingCenter = Vector3(0.0f, 0.0f, z);

            records[i].InstanceIndex = instanceIndex;
            records[i].IndexCount = 3u + (i % 7u);
            records[i].FirstIndex = i * 3u;
            records[i].VertexOffset = static_cast<int32_t>(i) - 5; // negatives exercised
            records[i].RunGroup = (runCount > 1u) ? group(rng) : 0u;
            records[i].ViewDepth = z; // camera at origin, forward +Z
        }
        const auto partition = R::BuildSortedTransparentRunPartition(records, runCount);
        for (uint32_t i = 0; i < liveCount; ++i)
        {
            recordsGPU[i] = {records[i].InstanceIndex, records[i].IndexCount,
                             records[i].FirstIndex, records[i].VertexOffset,
                             partition.SlotOfGroup[records[i].RunGroup]};
        }
        const std::vector<uint32_t> order = R::BuildSortedTransparentCpuOrder(records, partition);

        BufferHandle recordsBuf =
            MakeHostBuffer(liveCount * sizeof(R::SortedTransparentRecordGPU), "Drain.Records");
        BufferHandle instBuf = MakeHostBuffer(instances.size() * sizeof(GPUInstance), "Drain.Instances");
        BufferHandle argsBuf = MakeHostBuffer(kCapacity * kArgsStride * sizeof(uint32_t), "Drain.Args");
        BufferHandle countsBuf = MakeHostBuffer(kCapacity * sizeof(uint32_t), "Drain.Counts");
        BufferHandle indirBuf = MakeHostBuffer(kCapacity * sizeof(uint32_t), "Drain.Indirection");
        BufferHandle runCountsBuf = MakeHostBuffer(runCount * sizeof(uint32_t), "Drain.RunCounts");

        UploadBytes(recordsBuf, recordsGPU.data(),
                    liveCount * sizeof(R::SortedTransparentRecordGPU));
        UploadBytes(instBuf, instances.data(), instances.size() * sizeof(GPUInstance));
        UploadBytes(runCountsBuf, partition.RunCount.data(), runCount * sizeof(uint32_t));
        // Sentinel-fill args + indirection: the tail past liveCount must stay
        // untouched (F6 — the drain writes only [0, liveCount)).
        {
            const std::vector<uint32_t> sentinelArgs(kCapacity * kArgsStride, kArgsSentinel);
            UploadBytes(argsBuf, sentinelArgs.data(), sentinelArgs.size() * sizeof(uint32_t));
            const std::vector<uint32_t> sentinelIndir(kCapacity, kArgsSentinel);
            UploadBytes(indirBuf, sentinelIndir.data(), sentinelIndir.size() * sizeof(uint32_t));
        }

        DescriptorSetDesc setDesc{};
        setDesc.layout = m_Layout;
        setDesc.transient = true;
        setDesc.debugName = "Drain.DS";
        DescriptorSetHandle ds = m_Device->CreateDescriptorSet(setDesc);
        m_Device->UpdateStorageBufferBinding(ds, 0, recordsBuf, 0, 0);
        m_Device->UpdateStorageBufferBinding(ds, 1, instBuf, 0, 0);
        m_Device->UpdateStorageBufferBinding(ds, 2, argsBuf, 0, 0);
        m_Device->UpdateStorageBufferBinding(ds, 3, countsBuf, 0, 0);
        m_Device->UpdateStorageBufferBinding(ds, 4, indirBuf, 0, 0);
        m_Device->UpdateStorageBufferBinding(ds, 5, runCountsBuf, 0, 0);

        DrainPC pc{};
        pc.CameraPos[0] = pc.CameraPos[1] = pc.CameraPos[2] = 0.0f;
        pc.LiveCount = liveCount;
        pc.CameraForward[0] = pc.CameraForward[1] = 0.0f;
        pc.CameraForward[2] = 1.0f;
        pc.RunCount = runCount;

        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->SetPipeline(m_Pipeline);
        cl->BindDescriptorSet(0, ds, m_Pipeline);
        cl->SetPushConstants(pc);
        cl->Dispatch(1, 1, 1); // exactly one workgroup (F6)
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();

        std::vector<uint32_t> outArgs(kCapacity * kArgsStride);
        std::vector<uint32_t> outCounts(kCapacity);
        std::vector<uint32_t> outIndir(kCapacity);
        {
            void* m = m_Device->MapBuffer(argsBuf);
            ASSERT_NE(m, nullptr);
            std::memcpy(outArgs.data(), m, outArgs.size() * sizeof(uint32_t));
            m_Device->UnmapBuffer(argsBuf);
            m = m_Device->MapBuffer(countsBuf);
            ASSERT_NE(m, nullptr);
            std::memcpy(outCounts.data(), m, outCounts.size() * sizeof(uint32_t));
            m_Device->UnmapBuffer(countsBuf);
            m = m_Device->MapBuffer(indirBuf);
            ASSERT_NE(m, nullptr);
            std::memcpy(outIndir.data(), m, outIndir.size() * sizeof(uint32_t));
            m_Device->UnmapBuffer(indirBuf);
        }

        // Per-run counts copied verbatim.
        for (uint32_t r = 0; r < runCount; ++r)
            ASSERT_EQ(outCounts[r], partition.RunCount[r]) << "count mismatch for run " << r;

        // The GPU stream must equal the CPU twin position-for-position: the
        // drain's key sort is total (F4 unique tiebreak), so this is exact.
        for (uint32_t p = 0; p < liveCount; ++p)
        {
            const R::SortedTransparentRecord& rec = records[order[p]];
            const uint32_t base = p * kArgsStride;
            ASSERT_EQ(outArgs[base + 0], rec.IndexCount) << "indexCount @" << p;
            ASSERT_EQ(outArgs[base + 1], 1u) << "instanceCount @" << p;
            ASSERT_EQ(outArgs[base + 2], rec.FirstIndex) << "firstIndex @" << p;
            ASSERT_EQ(outArgs[base + 3], static_cast<uint32_t>(rec.VertexOffset))
                << "vertexOffset @" << p;
            ASSERT_EQ(outArgs[base + 4], p) << "firstInstance/indirection slot @" << p;
            ASSERT_EQ(outIndir[p], rec.InstanceIndex) << "indirection payload @" << p;
        }

        // F6: the tail past liveCount is untouched (the sentinel survives).
        for (uint32_t w = liveCount * kArgsStride; w < kCapacity * kArgsStride; ++w)
            ASSERT_EQ(outArgs[w], kArgsSentinel) << "args tail written at word " << w;
        for (uint32_t p = liveCount; p < kCapacity; ++p)
            ASSERT_EQ(outIndir[p], kArgsSentinel) << "indirection tail written at " << p;
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    std::vector<BufferHandle> m_Buffers;
};
} // namespace

// The ElvenRealm-overview shape: several hundred records (past the old 256
// ceiling), a handful of (surface x blend) runs. GPU stream == CPU twin,
// within-run back-to-front, contiguous runs.
TEST_F(GPUDrainComputeTest, MatchesCpuTwinAcrossRuns)
{
    RunDrainAndExpectCpuTwin(/*liveCount=*/548u, /*runCount=*/5u, /*seed=*/2026u);
}

// Full capacity (F6 boundary): every shared-memory slot live, single run.
TEST_F(GPUDrainComputeTest, FullCapacitySingleRun)
{
    RunDrainAndExpectCpuTwin(/*liveCount=*/kCapacity, /*runCount=*/1u, /*seed=*/7u);
}

// Degenerate small set: below the old ceiling, many runs relative to records.
TEST_F(GPUDrainComputeTest, SmallSetManyRuns)
{
    RunDrainAndExpectCpuTwin(/*liveCount=*/12u, /*runCount=*/6u, /*seed=*/99u);
}

// Mixed-sign depths (R1-F5): behind-camera centres (negative view depth) reach
// the drain when their frustum-straddling spheres survive the conservative
// cull. Integer-valued negatives are exactly representable, so the CPU/GPU
// stream compare stays bit-exact through the monotonic float transform's
// negative branch (flip-all-bits).
TEST_F(GPUDrainComputeTest, MixedSignDepthsMatchCpuTwin)
{
    RunDrainAndExpectCpuTwin(/*liveCount=*/400u, /*runCount=*/3u, /*seed=*/424242u,
                             /*depthMin=*/-30000, /*depthMax=*/30000);
}
