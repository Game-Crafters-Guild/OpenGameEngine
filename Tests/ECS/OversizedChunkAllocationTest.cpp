#include "ECS/ArchetypeTable.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <string>
#include <utility>

namespace
{
using namespace GameEngine::ECS;

template <std::size_t Bytes, int Tag = 0>
struct Payload
{
    std::array<std::byte, Bytes> BytesValue{};
};
using LargePayload = Payload<32 * 1024>;
using HalfPayloadA = Payload<9 * 1024, 1>;
using HalfPayloadB = Payload<9 * 1024, 2>;
struct Marker { int Value = 0; };

void ExpectAllocation(const ArchetypeTable& table)
{
    const auto& layout = table.GetLayout();
    EXPECT_GE(layout.AllocationBytes, kArchetypeChunkBytes);
    EXPECT_GE(layout.AllocationBytes, layout.TotalBytesUsed);
    EXPECT_EQ(layout.AllocationBytes % layout.AllocationAlignment, 0u);
    for (const auto& chunk : table.GetChunks())
    {
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(chunk.GetMemory()) % layout.AllocationAlignment, 0u);
#ifdef _MSC_VER
        // Check the actual CRT allocation, independently of the layout metadata.
        EXPECT_EQ(_aligned_msize(const_cast<std::byte*>(chunk.GetMemory()), layout.AllocationAlignment, 0),
                  layout.AllocationBytes);
#endif
        for (const auto& column : layout.Columns)
        {
            EXPECT_LE(std::size_t{column.Offset} + std::size_t{column.Size} * layout.Capacity,
                      layout.AllocationBytes);
        }
    }
}

TEST(OversizedChunkAllocation, NormalLayoutsKeepTheStandardAllocation)
{
    for (const std::vector<ComponentMeta>& metas :
         {std::vector<ComponentMeta>{}, {{1, 12, 4}, {2, 12, 4}}, {{1, 16368, 16}}})
    {
        ArchetypeTable table(metas);
        ASSERT_GT(table.GetLayout().Capacity, 0u);
        EXPECT_LE(table.GetLayout().TotalBytesUsed, kArchetypeChunkBytes);
        EXPECT_EQ(table.GetLayout().AllocationBytes, kArchetypeChunkBytes);
        EXPECT_EQ(table.GetLayout().AllocationAlignment, kCacheLineSize);
        table.Reserve(table.GetLayout().Capacity + 1);
        EXPECT_EQ(table.GetChunkCount(), 2u);
        ExpectAllocation(table);
    }
}

TEST(OversizedChunkAllocation, OneByteOverTheLimitRoundsTheAllocationUp)
{
    const std::array metas{ComponentMeta{1, 16369, 1}};
    ArchetypeTable table(metas);
    EXPECT_EQ(table.GetLayout().Capacity, 1u);
    EXPECT_EQ(table.GetLayout().TotalBytesUsed, kArchetypeChunkBytes + 1);
    EXPECT_EQ(table.GetLayout().AllocationBytes, kArchetypeChunkBytes + kCacheLineSize);
    table.AddEntity(EntityHandle(1, 1));
    ExpectAllocation(table);
    std::memset(table.GetChunks()[0].GetColumnRaw(0), 0xA7, metas[0].Size);
}

TEST(OversizedChunkAllocation, CompositeRowFitsEveryColumn)
{
    const std::array metas{ComponentMeta{1, 9 * 1024, 1}, ComponentMeta{2, 9 * 1024, 1}};
    ArchetypeTable table(metas);
    ASSERT_EQ(table.GetLayout().Capacity, 1u);
    for (uint32_t i = 0; i < 4; ++i)
    {
        const auto location = table.AddEntity(EntityHandle(i + 1, 1));
        EXPECT_EQ(location.ChunkIndex, i);
        for (int column = 0; column < 2; ++column)
            std::memset(table.GetChunks()[i].GetColumnRaw(column), int(i + column + 1), metas[column].Size);
    }
    ExpectAllocation(table);
    for (uint32_t i = 0; i < 4; ++i)
        for (int column = 0; column < 2; ++column)
        {
            const auto* bytes = table.GetChunks()[i].GetColumn<std::byte>(column);
            EXPECT_TRUE(std::all_of(bytes, bytes + metas[column].Size,
                                   [=](std::byte value) { return value == std::byte(i + column + 1); }));
        }
}

TEST(OversizedChunkAllocation, HonorsAlignmentBeyondCacheLinesAndChunkSize)
{
    for (const uint32_t alignment : {256u, 32768u})
    {
        const std::array metas{ComponentMeta{1, alignment, alignment}};
        ArchetypeTable table(metas);
        const uint32_t capacity = table.GetLayout().Capacity;
        EXPECT_EQ(table.GetLayout().AllocationAlignment, alignment);
        for (uint32_t i = 0; i < capacity; ++i)
        {
            const auto location = table.AddEntity(EntityHandle(i + 1, 1));
            auto* component = table.GetChunks()[location.ChunkIndex].GetComponentRaw(0, location.IndexInChunk);
            EXPECT_EQ(reinterpret_cast<std::uintptr_t>(component) % alignment, 0u);
            std::memset(component, 0x91, alignment);
        }
        ExpectAllocation(table);
    }
}

TEST(OversizedChunkAllocation, RejectsUnrepresentableLayoutsBeforeAllocation)
{
    const auto maximum = std::numeric_limits<uint32_t>::max();
    const std::array hugeComponent{ComponentMeta{1, maximum, 1}};
    EXPECT_THROW(BuildColumnLayout(hugeComponent), std::length_error);
    ColumnLayout directLayout;
    EXPECT_THROW(ComputeColumnOffsets(directLayout, maximum, hugeComponent), std::length_error);
    const std::array hugeComposite{ComponentMeta{1, maximum / 2, 1}, ComponentMeta{2, maximum / 2, 1}};
    EXPECT_THROW(BuildColumnLayout(hugeComposite), std::length_error);
    const std::array hugeAlignment{ComponentMeta{1, 1, maximum}};
    EXPECT_THROW(BuildColumnLayout(hugeAlignment), std::length_error);
    const std::array paddingOverflow{ComponentMeta{1, maximum - 8, 16}};
    EXPECT_THROW(BuildColumnLayout(paddingOverflow), std::length_error);
    if constexpr (sizeof(std::size_t) > sizeof(uint32_t))
    {
        const std::array<ComponentTypeId, 1> ids{1};
        const std::unordered_map<ComponentTypeId, std::size_t> sizes{{1, std::size_t{maximum} + 1}};
        EXPECT_THROW(BuildSortedComponentMetas(ids, sizes), std::length_error);
    }
}

TEST(OversizedChunkAllocation, CompactionAndTableMovesPreserveOwnershipAndVersions)
{
    const std::array metas{ComponentMeta{1, sizeof(LargePayload), alignof(LargePayload)}};
    ArchetypeTable table(metas);
    for (uint32_t i = 0; i < 4; ++i)
    {
        table.AddEntity(EntityHandle(i + 1, 1));
        std::memset(table.GetChunks()[i].GetColumnRaw(0), int(i + 1), sizeof(LargePayload));
        table.StampColumnVersion(i, 0, 100 + i);
    }
    table.RemoveEntity({0, 0});
    table.RemoveEntity({2, 0});
    uint32_t relocations = 0;
    EXPECT_EQ(table.CompactChunks([&](uint32_t from, uint32_t to, const ArchetypeChunk& chunk) {
        ++relocations;
        EXPECT_EQ(from, 3u);
        EXPECT_EQ(to, 0u);
        EXPECT_EQ(chunk.GetEntityHandles()[0], EntityHandle(4, 1));
    }), 2u);
    EXPECT_EQ(relocations, 1u);
    EXPECT_EQ(table.GetEntityCount(), 2u);
    ArchetypeTable moved(std::move(table));
    ArchetypeTable assigned(metas);
    assigned.AddEntity(EntityHandle(99, 1)); // Move assignment must free its prior allocation.
    assigned = std::move(moved);
    ExpectAllocation(assigned);
    EXPECT_EQ(assigned.GetColumnVersion(0, 0), 103u);
    EXPECT_EQ(assigned.GetColumnVersion(1, 0), 101u);
    for (std::size_t i = 0; i < assigned.GetChunkCount(); ++i)
    {
        const auto& chunk = assigned.GetChunks()[i];
        EXPECT_EQ(chunk.GetLayout(), &assigned.GetLayout());
        const auto& bytes = chunk.GetColumn<LargePayload>(0)[0].BytesValue;
        EXPECT_TRUE(std::all_of(bytes.begin(), bytes.end(), [=](std::byte value) {
            return value == std::byte(i == 0 ? 4 : 2);
        }));
        assigned.RemoveEntity({static_cast<uint32_t>(i), 0});
    }
    EXPECT_EQ(assigned.CompactChunks([](auto...) {}), 2u);
    EXPECT_EQ(assigned.GetChunkCount(), 0u);
}

#ifndef NDEBUG
TEST(OversizedChunkAllocation, DebugFillCoversTheEntireAllocation)
{
    const std::array metas{ComponentMeta{1, sizeof(LargePayload), alignof(LargePayload)}};
    ArchetypeTable table(metas);
    table.Reserve(1);
    const auto* memory = table.GetChunks()[0].GetMemory();
    EXPECT_TRUE(std::all_of(memory, memory + table.GetLayout().AllocationBytes,
                           [](std::byte value) { return value == std::byte{0}; }));
}
#endif

TEST(OversizedChunkAllocation, WorldTransitionsKeepLargeAndCompositePayloadsIntact)
{
    World world(nullptr);
    LargePayload large;
    HalfPayloadA first;
    HalfPayloadB second;
    large.BytesValue.fill(std::byte{0xD7});
    first.BytesValue.fill(std::byte{0x4A});
    second.BytesValue.fill(std::byte{0xB3});
    const auto big = world.CreateHandle(large);
    const auto composite = world.CreateHandle(first, second);
    for (const auto entity : {big, composite})
    {
        const auto* archetype = world.GetEntityArchetype(entity);
        ASSERT_NE(archetype, nullptr);
        EXPECT_GT(archetype->GetTable().GetLayout().TotalBytesUsed, kArchetypeChunkBytes);
        ExpectAllocation(archetype->GetTable());
        world.AddComponentImmediate(entity, Marker{42});
        world.RemoveComponentImmediate<Marker>(entity);
    }
    ASSERT_NE(world.GetComponent<LargePayload>(big), nullptr);
    EXPECT_EQ(world.GetComponent<LargePayload>(big)->BytesValue, large.BytesValue);
    ASSERT_NE(world.GetComponent<HalfPayloadA>(composite), nullptr);
    EXPECT_EQ(world.GetComponent<HalfPayloadA>(composite)->BytesValue, first.BytesValue);
    EXPECT_EQ(world.GetComponent<HalfPayloadB>(composite)->BytesValue, second.BytesValue);
    world.RemoveComponentImmediate<HalfPayloadB>(composite);
    EXPECT_EQ(world.GetComponent<HalfPayloadA>(composite)->BytesValue, first.BytesValue);
    world.DestroyEntityImmediate(big);
    world.DestroyEntityImmediate(composite);
    world.CompactAllChunks();
    EXPECT_FALSE(world.IsValid(big));
    EXPECT_FALSE(world.IsValid(composite));
}

// Components are byte-relocatable by contract; externally owned resources use
// removal hooks. Pin those lifetimes instead of admitting nontrivial C++ types.
static_assert(Component<LargePayload>);
static_assert(!Component<std::string>);
std::array<unsigned, 4> releases{};
void ReleaseLargePayload(LargePayload& value)
{
    const unsigned resource = std::to_integer<unsigned>(value.BytesValue.back());
    ASSERT_LT(resource, releases.size());
    EXPECT_EQ(value.BytesValue.front(), value.BytesValue.back());
    ++releases[resource];
}

TEST(OversizedChunkAllocation, RemovalHooksReleaseEachResourceOnce)
{
    releases.fill(0);
    {
        World world(nullptr);
        world.RegisterOnRemove<LargePayload>(ReleaseLargePayload);
        std::array<EntityHandle, 4> entities;
        for (unsigned i = 0; i < entities.size(); ++i)
        {
            LargePayload value;
            value.BytesValue.fill(std::byte(i));
            entities[i] = world.CreateHandle(value);
            world.AddComponentImmediate(entities[i], Marker{int(i)});
            world.RemoveComponentImmediate<Marker>(entities[i]);
        }
        EXPECT_EQ(releases, (std::array<unsigned, 4>{0, 0, 0, 0}));
        world.RemoveComponentImmediate<LargePayload>(entities[0]);
        world.DestroyEntityImmediate(entities[1]);
        world.CompactAllChunks();
        EXPECT_EQ(releases, (std::array<unsigned, 4>{1, 1, 0, 0}));
        // Clear and destructor independently release live oversized rows.
        world.Clear();
        EXPECT_EQ(releases, (std::array<unsigned, 4>{1, 1, 1, 1}));
        LargePayload value;
        value.BytesValue.fill(std::byte{3});
        world.CreateHandle(value);
    }
    EXPECT_EQ(releases, (std::array<unsigned, 4>{1, 1, 1, 2}));
}
} // namespace
