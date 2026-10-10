// GPUScene::UpdateInstances (batched row writes for bulk motion).
//
// The batch must be observationally identical to calling UpdateInstance for
// each write in span order: the same rows, the same scatter-hot mirror, the
// same BatchRegistry counts, and the same rows uploaded on the next flush —
// exactly the rows whose bytes changed, however the caller splits the row
// phase across threads.

#include <gtest/gtest.h>

#include "Rendering/Core/BatchRegistry.h"
#include "Rendering/Core/GPUInstanceDepthClass.h"
#include "Rendering/Core/GPUScene.h"

#include "EngineLogCapture.h"
#include "TestDeviceHelper.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <future>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

GPUInstance MakeRow(uint32_t i)
{
    GPUInstance instance{};
    instance.materialIndex = 1u + (i % 4u);
    instance.meshIndex = 10u + (i % 3u);
    instance.boundingRadius = 1.0f;
    instance.transform.Data()[12] = static_cast<float>(i);
    return instance;
}

// Runs the ranges on separate threads, last range first, so the row phase is
// exercised out of span order and concurrently.
void RunRangesConcurrentlyInReverse(size_t count, const std::function<void(size_t, size_t)>& body)
{
    constexpr size_t kRanges = 5;
    const size_t step = (count + kRanges - 1) / kRanges;
    std::vector<std::future<void>> running;
    for (size_t r = kRanges; r-- > 0;)
    {
        const size_t begin = std::min(count, r * step);
        const size_t end = std::min(count, begin + step);
        if (begin < end)
            running.push_back(std::async(std::launch::async, [&body, begin, end] { body(begin, end); }));
    }
    for (auto& job : running)
        job.get();
}

class GPUSceneUpdateInstancesTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
    }

    void TearDown() override
    {
        if (m_Device)
            m_Device->Shutdown();
    }

    // A scene of `count` rows whose upload ring has converged (every in-flight
    // slot flushed), so the next flush uploads only what changes after this.
    std::unique_ptr<GPUScene> MakeConvergedScene(uint32_t count)
    {
        auto scene = std::make_unique<GPUScene>(m_Device.get());
        EXPECT_TRUE(scene->Initialize(4096u, 256u));
        for (uint32_t i = 0; i < count; ++i)
            scene->AddInstance(MakeRow(i));
        for (uint32_t frame = 0; frame < m_Device->GetFramesInFlight() + 1u; ++frame)
        {
            scene->AdvanceFrameSlot();
            scene->FlushGPUBuffers();
        }
        return scene;
    }

    std::unique_ptr<IDevice> m_Device;
};

// Writes that move a third of the rows, rewrite a few rows with identical
// bytes, and change a few rows' batch keys (material, mesh, mirrored winding).
// The three sets take disjoint residues mod 3, so every index appears once.
std::vector<GPUScene::InstanceWrite> MakeWrites(uint32_t count)
{
    std::vector<GPUScene::InstanceWrite> writes;
    for (uint32_t i = 0; i < count; i += 3u)
    {
        GPUScene::InstanceWrite write{i, MakeRow(i)};
        write.Instance.transform.Data()[13] = 5.0f;
        writes.push_back(write);
    }
    for (uint32_t i = 1u; i < count; i += 3u * 97u)
        writes.push_back({i, MakeRow(i)}); // identical bytes: must stay clean
    for (uint32_t i = 2u; i < count; i += 3u * 61u)
    {
        GPUScene::InstanceWrite write{i, MakeRow(i)};
        write.Instance.materialIndex = 9u;
        write.Instance.meshIndex = 20u;
        write.Instance.flags |= kInstanceFlagMirrored;
        writes.push_back(write);
    }
    return writes;
}

TEST_F(GPUSceneUpdateInstancesTest, BatchEqualsPerWriteUpdates)
{
    constexpr uint32_t kCount = 1500u;
    auto perWrite = MakeConvergedScene(kCount);
    auto batched = MakeConvergedScene(kCount);
    const std::vector<GPUScene::InstanceWrite> writes = MakeWrites(kCount);

    for (const auto& write : writes)
        perWrite->UpdateInstance(write.InstanceIndex, write.Instance);
    batched->UpdateInstances(writes, RunRangesConcurrentlyInReverse);

    const auto& a = perWrite->GetInstances();
    const auto& b = batched->GetInstances();
    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size() * sizeof(GPUInstance))) << "rows differ";
    const auto& ha = perWrite->GetScatterHot();
    const auto& hb = batched->GetScatterHot();
    ASSERT_EQ(ha.size(), hb.size());
    EXPECT_EQ(0, std::memcmp(ha.data(), hb.data(), ha.size() * sizeof(GPUInstanceScatterHot)))
        << "scatter-hot mirror differs";
    EXPECT_EQ(perWrite->GetBatchRegistry().Counts(), batched->GetBatchRegistry().Counts())
        << "batch registry counts differ";

    // The next flush uploads the same bytes on both paths.
    const uint64_t perWriteBefore = perWrite->GetInstanceUploadBytesTotal();
    const uint64_t batchedBefore = batched->GetInstanceUploadBytesTotal();
    perWrite->AdvanceFrameSlot();
    perWrite->FlushGPUBuffers();
    batched->AdvanceFrameSlot();
    batched->FlushGPUBuffers();
    EXPECT_EQ(perWrite->GetInstanceUploadBytesTotal() - perWriteBefore,
              batched->GetInstanceUploadBytesTotal() - batchedBefore);
}

// I1: after a batch, the next flush uploads exactly the rows whose bytes
// changed — the moved and re-keyed rows — and none of the unmoved ones, and
// the flushed slot's buffer then holds every row as the scene does: a missed
// or misplaced dirty bit leaves a stale row there.
TEST_F(GPUSceneUpdateInstancesTest, FlushUploadsExactlyTheChangedRows)
{
    constexpr uint32_t kCount = 1500u;
    auto scene = MakeConvergedScene(kCount);
    const std::vector<GPUScene::InstanceWrite> writes = MakeWrites(kCount);
    uint64_t changedRows = 0;
    for (const auto& write : writes)
        if (std::memcmp(&scene->GetInstances()[write.InstanceIndex], &write.Instance, sizeof(GPUInstance)) != 0)
            ++changedRows;
    ASSERT_GT(changedRows, 0u);
    ASSERT_LT(changedRows, writes.size()) << "the identical-bytes writes are part of the test";

    scene->UpdateInstances(writes, RunRangesConcurrentlyInReverse);
    const uint64_t before = scene->GetInstanceUploadBytesTotal();
    scene->AdvanceFrameSlot();
    scene->FlushGPUBuffers();
    const uint64_t rowBytes =
        sizeof(GPUInstance) + (scene->IsScatterHotEnabled() ? sizeof(GPUInstanceScatterHot) : 0u);
    EXPECT_EQ(scene->GetInstanceUploadBytesTotal() - before, changedRows * rowBytes);

    const std::vector<GPUInstance>& rows = scene->GetInstances();
    const BufferHandle buffer = scene->GetInstanceBuffer();
    const auto* uploaded = static_cast<const GPUInstance*>(m_Device->MapBuffer(buffer));
    ASSERT_NE(uploaded, nullptr) << "the instance buffer is not host-visible";
    size_t staleRows = 0;
    size_t firstStale = rows.size();
    for (size_t i = 0; i < rows.size(); ++i)
    {
        if (std::memcmp(&uploaded[i], &rows[i], sizeof(GPUInstance)) != 0)
        {
            ++staleRows;
            firstStale = std::min(firstStale, i);
        }
    }
    m_Device->UnmapBuffer(buffer);
    EXPECT_EQ(staleRows, 0u) << "the flushed slot differs from the scene, first at row " << firstStale;
}

// A write whose index is past the scene is reported and skipped; the rest of
// the batch applies as per-write updates would.
TEST_F(GPUSceneUpdateInstancesTest, IndexPastTheSceneIsReportedAndSkipped)
{
    constexpr uint32_t kCount = 300u;
    auto perWrite = MakeConvergedScene(kCount);
    auto batched = MakeConvergedScene(kCount);
    std::vector<GPUScene::InstanceWrite> writes = MakeWrites(kCount);
    writes.insert(writes.begin() + static_cast<std::ptrdiff_t>(writes.size() / 2), {kCount + 7u, MakeRow(0u)});

    for (const auto& write : writes)
        if (write.InstanceIndex < kCount)
            perWrite->UpdateInstance(write.InstanceIndex, write.Instance);
    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Warning);
        batched->UpdateInstances(writes, RunRangesConcurrentlyInReverse);
    }
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "Invalid instance index " + std::to_string(kCount + 7u)), 1u);
    const auto& a = perWrite->GetInstances();
    const auto& b = batched->GetInstances();
    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size() * sizeof(GPUInstance))) << "rows differ";
    EXPECT_EQ(perWrite->GetBatchRegistry().Counts(), batched->GetBatchRegistry().Counts());
}

} // namespace
