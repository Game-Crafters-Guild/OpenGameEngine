// Capacity contract tests for GPUScene's mesh GPU table and the device-level
// UpdateBuffer bounds guard: the table is sized from the capacity declared to
// Initialize, uploads never write past a buffer's allocation, an oversized
// device-level update is rejected wholesale, and a clamped upload reports the
// rows it dropped exactly once.

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"

#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"

#include "TestDeviceHelper.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

// The row-count/byte-count arithmetic below is written against this stride.
static_assert(sizeof(GPUMesh) == 128, "GPUMesh row size drift breaks the capacity math below");

// Copy rows out and unmap before returning, so no assertion can ever fire while
// the allocation is still mapped. A gtest fatal failure inside a
// MapBuffer/UnmapBuffer bracket aborts the process in VMA's "Allocation was not
// unmapped before destruction" check: exit 3, no [ FAILED ] line, no summary
// block, and every test after it in the binary never runs — a real regression
// here would read as a clean suite.
template <typename Row>
[[nodiscard]] bool ReadRows(IDevice& device, BufferHandle buffer, size_t firstRow, size_t rowCount,
                            Row* out)
{
    const auto* rows = static_cast<const Row*>(device.MapBuffer(buffer));
    if (rows == nullptr)
        return false;
    std::memcpy(out, rows + firstRow, rowCount * sizeof(Row));
    device.UnmapBuffer(buffer);
    return true;
}

class GPUSceneBufferCapacityTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
        {
            GTEST_SKIP() << "No Vulkan device available";
        }
    }

    void TearDown() override
    {
        if (m_Scene)
            m_Scene->Shutdown();
        m_Scene.reset();
        if (m_Device)
            m_Device->Shutdown();
    }

    std::unique_ptr<IDevice> m_Device;
    std::unique_ptr<GPUScene> m_Scene;
};

} // namespace

TEST_F(GPUSceneBufferCapacityTest, OversizedDeviceUpdateIsRejectedWholesale)
{
    BufferDesc desc{};
    desc.size = 64;
    desc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferDst);
    desc.memoryUsage = BufferMemoryUsage::Upload;
    desc.debugName = "CapacityTest_Small";
    const BufferHandle buffer = m_Device->CreateBuffer(desc);
    ASSERT_TRUE(buffer.IsValid());

    const std::vector<uint8_t> prime(64, 0xA5);
    m_Device->UpdateBuffer(buffer, 0, prime.size(), prime.data());

    // 128 bytes into a 64-byte buffer: must be rejected without writing.
    const std::vector<uint8_t> oversized(128, 0x5A);
    m_Device->UpdateBuffer(buffer, 0, oversized.size(), oversized.data());
    std::vector<uint8_t> afterOversized(prime.size(), 0);
    const bool readAfterOversized =
        ReadRows(*m_Device, buffer, 0, afterOversized.size(), afterOversized.data());

    // Boundary case: offset + size == buffer size is in range and must land.
    const std::vector<uint8_t> tail(32, 0x3C);
    m_Device->UpdateBuffer(buffer, 32, tail.size(), tail.data());
    std::vector<uint8_t> afterTail(tail.size(), 0);
    const bool readAfterTail = ReadRows(*m_Device, buffer, 32, afterTail.size(), afterTail.data());

    m_Device->DestroyBuffer(buffer);

    ASSERT_TRUE(readAfterOversized);
    EXPECT_EQ(std::memcmp(afterOversized.data(), prime.data(), prime.size()), 0)
        << "oversized UpdateBuffer wrote into the buffer instead of rejecting";
    ASSERT_TRUE(readAfterTail);
    EXPECT_EQ(std::memcmp(afterTail.data(), tail.data(), tail.size()), 0)
        << "in-range boundary UpdateBuffer was rejected (guard off-by-one)";
}

TEST_F(GPUSceneBufferCapacityTest, MeshTableIsSizedFromDeclaredCapacity)
{
    // 9000 rows crosses the former 1 MiB placeholder (8192 rows at 128 B/row);
    // the capacity declared to Initialize must be what sizes the GPU table.
    constexpr uint32_t kMaxMeshes = 9000;
    m_Scene = std::make_unique<GPUScene>(m_Device.get());
    ASSERT_TRUE(m_Scene->Initialize(16u, kMaxMeshes));

    for (uint32_t i = 0; i < kMaxMeshes; ++i)
    {
        GPUMesh mesh{};
        mesh.boundingRadius = static_cast<float>(i) + 1.0f;
        mesh.indexCount = i + 1u;
        m_Scene->AddMesh(mesh);
    }
    m_Scene->FlushGPUBuffers();

    // Row 0 first, and fatally: an upload rejected for exceeding the buffer
    // leaves even row 0 unwritten, and on an undersized table the tail row is
    // past the allocation — so this must stop the test before that read.
    GPUMesh firstRow{};
    ASSERT_TRUE(ReadRows(*m_Device, m_Scene->GetMeshBuffer(), 0u, 1u, &firstRow));
    ASSERT_EQ(firstRow.indexCount, 1u);
    ASSERT_FLOAT_EQ(firstRow.boundingRadius, 1.0f);

    GPUMesh lastRow{};
    ASSERT_TRUE(ReadRows(*m_Device, m_Scene->GetMeshBuffer(), kMaxMeshes - 1u, 1u, &lastRow));
    EXPECT_EQ(lastRow.indexCount, kMaxMeshes);
    EXPECT_FLOAT_EQ(lastRow.boundingRadius, static_cast<float>(kMaxMeshes));
}

TEST_F(GPUSceneBufferCapacityTest, UploadBeyondDeclaredCapacityIsClampedNotCorrupting)
{
    // Green-only guard for the clamp path (the red half of this file's
    // red-green story is the sizing test above): exceeding the declared
    // capacity clamps the upload to the table's row count — in-capacity rows
    // still reach the GPU and the flush is not rejected wholesale. The drop
    // report is pinned by ClampedUploadReportsDroppedRowsOnce below.
    constexpr uint32_t kCapacity = 8;
    m_Scene = std::make_unique<GPUScene>(m_Device.get());
    ASSERT_TRUE(m_Scene->Initialize(16u, kCapacity));

    for (uint32_t i = 0; i < kCapacity + 4u; ++i)
    {
        GPUMesh mesh{};
        mesh.indexCount = i + 100u;
        m_Scene->AddMesh(mesh);
    }
    m_Scene->FlushGPUBuffers();

    std::array<GPUMesh, kCapacity> rows{};
    ASSERT_TRUE(ReadRows(*m_Device, m_Scene->GetMeshBuffer(), 0u, rows.size(), rows.data()));
    for (uint32_t i = 0; i < kCapacity; ++i)
        EXPECT_EQ(rows[i].indexCount, i + 100u) << "in-capacity row " << i << " missing";
}

// The clamp's user-facing contract, which nothing else in this file observes:
// the logger's effective level starts at Off and these tests register no sink,
// so the clamp can drop rows in total silence and every other test still passes.
// Pin both halves — the drop IS reported, and reported once rather than per
// flush.
TEST_F(GPUSceneBufferCapacityTest, ClampedUploadReportsDroppedRowsOnce)
{
    Logger::Log::Initialize({});
    auto sink = Logger::MakeUnique<Logger::CallbackSink>();
    auto* sinkPtr = sink.get();
    auto errorCount = std::make_shared<std::atomic<int>>(0);
    const Logger::uint64 callbackId = sinkPtr->RegisterCallback(
        [errorCount](const Logger::LogMessage& msg)
        {
            if (msg.Level == Logger::LogLevel::Error &&
                msg.Message.find("exceed the declared capacity") != Logger::String::npos)
                errorCount->fetch_add(1);
        });
    Logger::Log::AddSink(std::move(sink));

    constexpr uint32_t kCapacity = 8;
    m_Scene = std::make_unique<GPUScene>(m_Device.get());
    ASSERT_TRUE(m_Scene->Initialize(16u, kCapacity));

    for (uint32_t i = 0; i < kCapacity + 4u; ++i)
    {
        GPUMesh mesh{};
        mesh.indexCount = i + 1u;
        m_Scene->AddMesh(mesh);
    }
    m_Scene->FlushGPUBuffers();
    Logger::Log::Flush();
    EXPECT_EQ(errorCount->load(), 1) << "rows were dropped without any diagnostic";

    // The overflow persists and one more mesh re-dirties every slot, so the
    // upload runs again: the latch must hold the report at one.
    GPUMesh extra{};
    extra.indexCount = 999u;
    m_Scene->AddMesh(extra);
    m_Scene->FlushGPUBuffers();
    Logger::Log::Flush();
    EXPECT_EQ(errorCount->load(), 1) << "capacity error re-fired; the latch is not holding";

    sinkPtr->UnregisterCallback(callbackId);
}

TEST_F(GPUSceneBufferCapacityTest, SparseBatchPreservesGapsAndRejectsInvalidRanges)
{
    BufferDesc desc{};
    desc.size = 256;
    desc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferDst);
    desc.memoryUsage = BufferMemoryUsage::Upload;
    const auto buffer = m_Device->CreateBuffer(desc);
    ASSERT_TRUE(buffer.IsValid());
    std::array<uint8_t, 256> expected{};
    expected.fill(0xA5);
    m_Device->UpdateBuffer(buffer, 0, expected.size(), expected.data());
    const std::array<uint8_t, 4> value{1, 2, 3, 4};
    const std::array<BufferUpdateRange, 4> ranges{{
        {252, value.size(), value.data()}, {0, value.size(), value.data()},
        {63, value.size(), value.data()}, {0, 0, nullptr}}};
    m_Device->UpdateBufferRanges(buffer, ranges);
    for (const auto& range : ranges)
        if (range.size)
            std::memcpy(expected.data() + range.offset, range.data, range.size);

    // A valid prefix must not land when a later non-empty range is invalid.
    const std::array<uint8_t, 4> badValue{9, 9, 9, 9};
    const std::array<BufferUpdateRange, 2> invalid{{
        {0, badValue.size(), badValue.data()}, {255, value.size(), value.data()}}};
    m_Device->UpdateBufferRanges(buffer, invalid);
    const std::array<BufferUpdateRange, 2> overflow{{
        {0, badValue.size(), badValue.data()}, {SIZE_MAX, value.size(), value.data()}}};
    m_Device->UpdateBufferRanges(buffer, overflow);
    const std::array<BufferUpdateRange, 2> nullData{{
        {0, badValue.size(), badValue.data()}, {32, 1, nullptr}}};
    m_Device->UpdateBufferRanges(buffer, nullData);
    std::array<uint8_t, 256> actual{};
    const bool read = ReadRows(*m_Device, buffer, 0, actual.size(), actual.data());
    m_Device->DestroyBuffer(buffer);
    ASSERT_TRUE(read);
    EXPECT_EQ(actual, expected);
}

TEST_F(GPUSceneBufferCapacityTest, DirtyInstancesConvergeAcrossFrameSlotsWithoutCopyingNeighbours)
{
    constexpr uint32_t count = 4097;
    m_Scene = std::make_unique<GPUScene>(m_Device.get());
    ASSERT_TRUE(m_Scene->Initialize(count, 1));
    std::vector<GPUInstance> expected(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        expected[i].boundingRadius = static_cast<float>(i + 1);
        expected[i].materialIndex = 0;
        expected[i].meshIndex = 0;
        m_Scene->AddInstance(expected[i]);
    }
    const uint32_t slots = m_Device->GetFramesInFlight();
    ASSERT_GT(slots, 1u); // This oracle must exercise a real rotating ring.
    std::vector<std::vector<GPUInstance>> previous(slots);
    const size_t stride = sizeof(GPUInstance) +
        (m_Scene->GetScatterHotBuffer().IsValid() ? sizeof(GPUInstanceScatterHot) : 0);
    for (uint32_t frame = 0; frame < slots * 5; ++frame)
    {
        ASSERT_TRUE(m_Device->BeginFrame());
        const uint32_t slot = m_Device->GetFrameIndex();
        if (frame >= slots && frame < slots * 3)
        {
            // Non-sorted sparse indices, adjacent word boundaries, and the
            // partial final word. Repeated identical writes must cost nothing.
            const std::array<uint32_t, 6> dirty{4096, 64, frame, 127, 63, 128};
            for (uint32_t index : dirty)
            {
                expected[index].boundingRadius += 0.5f;
                m_Scene->UpdateInstance(index, expected[index]);
                m_Scene->UpdateInstance(index, expected[index]);
            }
            if (frame == slots)
                for (uint32_t index = 192; index < 320; ++index)
                {
                    expected[index].boundingRadius += 1.0f;
                    m_Scene->UpdateInstance(index, expected[index]);
                }
        }
        size_t changed = previous[slot].empty() ? count : 0;
        if (!previous[slot].empty())
            for (uint32_t i = 0; i < count; ++i)
                changed += std::memcmp(&previous[slot][i], &expected[i], sizeof(GPUInstance)) != 0;
        const auto before = m_Scene->GetInstanceUploadBytesTotal();
        m_Scene->BeginFrame();
        EXPECT_EQ(m_Scene->GetInstanceUploadBytesTotal() - before, changed * stride) << "frame " << frame;
        std::vector<GPUInstance> actual(count);
        EXPECT_TRUE(ReadRows(*m_Device, m_Scene->GetInstanceBuffer(), 0, count, actual.data()));
        EXPECT_EQ(std::memcmp(actual.data(), expected.data(), count * sizeof(GPUInstance)), 0)
            << "frame " << frame << " slot " << slot;
        if (m_Scene->GetScatterHotBuffer().IsValid())
        {
            std::vector<GPUInstanceScatterHot> hot(count);
            EXPECT_TRUE(ReadRows(*m_Device, m_Scene->GetScatterHotBuffer(), 0, count, hot.data()));
            for (uint32_t i = 0; i < count; ++i)
            {
                const auto row = MakeScatterHot(expected[i]);
                EXPECT_EQ(std::memcmp(&hot[i], &row, sizeof(row)), 0) << "row " << i;
            }
        }
        previous[slot] = expected;
        m_Scene->EndFrame();
        m_Device->Present();
    }
    m_Device->WaitForIdle();
}

TEST_F(GPUSceneBufferCapacityTest, DirtyTailRemovalAndReuseUploadOnlyLiveChangedRows)
{
    m_Scene = std::make_unique<GPUScene>(m_Device.get());
    ASSERT_TRUE(m_Scene->Initialize(256, 1));
    GPUInstance instance{};
    instance.meshIndex = 0;
    instance.materialIndex = 0;
    instance.boundingRadius = 1.0f;
    for (uint32_t i = 0; i < 256; ++i)
        m_Scene->AddInstance(instance);
    m_Scene->FlushGPUBuffers();
    m_Scene->RemoveInstance(255);
    m_Scene->RemoveInstance(7);
    auto before = m_Scene->GetInstanceUploadBytesTotal();
    const size_t stride = sizeof(GPUInstance) +
        (m_Scene->GetScatterHotBuffer().IsValid() ? sizeof(GPUInstanceScatterHot) : 0);
    m_Scene->FlushGPUBuffers();
    EXPECT_EQ(m_Scene->GetInstanceUploadBytesTotal() - before, stride);
    GPUInstance tombstone{};
    ASSERT_TRUE(ReadRows(*m_Device, m_Scene->GetInstanceBuffer(), 7, 1, &tombstone));
    EXPECT_EQ(tombstone.meshIndex, 0xFFFFFFFFu);
    EXPECT_EQ(m_Scene->AddInstance(instance), 7u);
    EXPECT_EQ(m_Scene->AddInstance(instance), 255u);
    before = m_Scene->GetInstanceUploadBytesTotal();
    m_Scene->FlushGPUBuffers();
    EXPECT_EQ(m_Scene->GetInstanceUploadBytesTotal() - before, 2 * stride);
    std::vector<uint32_t> all(256);
    for (uint32_t i = 0; i < 256; ++i)
        all[i] = i;
    m_Scene->RemoveInstances(all);
    before = m_Scene->GetInstanceUploadBytesTotal();
    m_Scene->FlushGPUBuffers();
    EXPECT_EQ(m_Scene->GetInstanceUploadBytesTotal(), before);
    EXPECT_EQ(m_Scene->AddInstance(instance), 0u);
    m_Scene->FlushGPUBuffers();
    EXPECT_EQ(m_Scene->GetInstanceUploadBytesTotal() - before, stride);
}

TEST_F(GPUSceneBufferCapacityTest, InstanceCapacityClampPreservesValidSparseWritesAndReportsOnce)
{
    Logger::Log::Initialize({});
    auto sink = Logger::MakeUnique<Logger::CallbackSink>();
    auto* sinkPtr = sink.get();
    auto errorCount = std::make_shared<std::atomic<int>>(0);
    const Logger::uint64 callbackId = sinkPtr->RegisterCallback(
        [errorCount](const Logger::LogMessage& msg)
        {
            if (msg.Level == Logger::LogLevel::Error &&
                msg.Message.find("instances exceed the declared capacity") != Logger::String::npos)
                errorCount->fetch_add(1);
        });
    Logger::Log::AddSink(std::move(sink));

    // A capacity inside a dirty word exercises both a crossing run and a wholly
    // out-of-capacity run. The allocation is larger than its minimum byte size.
    constexpr uint32_t kCapacity = 70;
    m_Scene = std::make_unique<GPUScene>(m_Device.get());
    ASSERT_TRUE(m_Scene->Initialize(kCapacity, 1u));
    std::vector<GPUInstance> expected(kCapacity + 4u);
    for (uint32_t i = 0; i < expected.size(); ++i)
    {
        expected[i].meshIndex = 0u;
        expected[i].materialIndex = 0u;
        expected[i].boundingRadius = static_cast<float>(i + 1u);
        m_Scene->AddInstance(expected[i]);
    }
    const size_t stride = sizeof(GPUInstance) +
        (m_Scene->GetScatterHotBuffer().IsValid() ? sizeof(GPUInstanceScatterHot) : 0);
    auto before = m_Scene->GetInstanceUploadBytesTotal();
    m_Scene->FlushGPUBuffers();
    EXPECT_EQ(m_Scene->GetInstanceUploadBytesTotal() - before, kCapacity * stride);
    std::array<GPUInstance, kCapacity> actual{};
    ASSERT_TRUE(ReadRows(*m_Device, m_Scene->GetInstanceBuffer(), 0, actual.size(), actual.data()));
    EXPECT_EQ(std::memcmp(actual.data(), expected.data(), sizeof(actual)), 0)
        << "the initial full upload must retain every in-capacity row";

    for (const uint32_t index : {2u, kCapacity - 1u, kCapacity, kCapacity + 3u})
    {
        expected[index].boundingRadius += 100.0f;
        m_Scene->UpdateInstance(index, expected[index]);
    }
    before = m_Scene->GetInstanceUploadBytesTotal();
    m_Scene->FlushGPUBuffers();
    EXPECT_EQ(m_Scene->GetInstanceUploadBytesTotal() - before, 2u * stride);
    ASSERT_TRUE(ReadRows(*m_Device, m_Scene->GetInstanceBuffer(), 0, actual.size(), actual.data()));
    EXPECT_EQ(std::memcmp(actual.data(), expected.data(), sizeof(actual)), 0)
        << "an invalid row must not poison valid updates in the same sparse batch";
    if (m_Scene->GetScatterHotBuffer().IsValid())
    {
        std::array<GPUInstanceScatterHot, kCapacity> hot{};
        ASSERT_TRUE(ReadRows(*m_Device, m_Scene->GetScatterHotBuffer(), 0, hot.size(), hot.data()));
        for (uint32_t i = 0; i < kCapacity; ++i)
        {
            const auto row = MakeScatterHot(expected[i]);
            EXPECT_EQ(std::memcmp(&hot[i], &row, sizeof(row)), 0) << "scatter row " << i;
        }
    }
    Logger::Log::Flush();
    EXPECT_EQ(errorCount->load(), 1) << "instance capacity overflow must be reported exactly once";
    sinkPtr->UnregisterCallback(callbackId);
}
