// Pins the draw-range lookup axes. Every reader of GPUDrawStreamBuilder's
// range map — the world color pass, the depth/shadow recorder, the debug
// server's draw-attribution surfaces — must derive the same (class, mesh) pair
// the publisher keyed the range on. The expectations below are taken from the
// batch tables ScheduleUnifiedScatter keys m_RangeMap from, not restated from
// the resolver, so a reader that drifts back onto the raw BatchKey fields
// fails against the real publisher.

#include <gtest/gtest.h>

#include "Engine/Rendering/DrawStreamLookupKey.h"

#include "Rendering/Core/BatchRegistry.h"

#include <cstdint>
#include <vector>

using GameEngine::Engine::Renderer::DrawStreamLookupKey;
using GameEngine::Engine::Renderer::ResolveDrawStreamLookupKey;
using GameEngine::Engine::Renderer::WorldDrawBuilder;
using GameEngine::Rendering::BatchRegistry;
using GameEngine::Rendering::GPUDrawStreamBuilder;
using GameEngine::Rendering::MaterialDepthClass;
using GameEngine::Rendering::MeshPoolGroupPlan;

namespace
{

using BatchTableEntry = GPUDrawStreamBuilder::BatchTableEntry;

// A published range exists for a table row iff some row carries that exact
// (class, mesh) pair — MakeStreamKey masks both fields to 24 bits and the
// registries below are mirror-free, so no parity bit is in play.
bool HasRow(const std::vector<BatchTableEntry>& table, uint32_t classKey, uint32_t meshKey)
{
    for (const auto& e : table)
        if (e.materialIndex == classKey && e.meshIndex == meshKey)
            return true;
    return false;
}

constexpr uint8_t kSingleSided =
    static_cast<uint8_t>(MaterialDepthClass::EligibleSingleSided);

WorldDrawBuilder::BatchKey MakeKey(uint32_t materialIndex, uint32_t meshIndex,
                                   uint32_t colorClassId)
{
    WorldDrawBuilder::BatchKey key{};
    key.material      = nullptr; // unused by the axis derivation
    key.materialIndex = materialIndex;
    key.meshIndex     = meshIndex;
    key.colorClassId  = colorClassId;
    return key;
}

} // namespace

// The default configuration: the P2 color merge folds two materials into one
// class id, and draw consolidation folds two meshes into one pool group. The
// range lives under (classId, group) and under NOTHING else — a reader keying
// on the raw materialIndex/meshIndex finds no range and reports a healthy draw
// as skipped.
TEST(DrawStreamLookupKey, ColorSliceResolvesClassIdAndPoolGroup)
{
    BatchRegistry registry;
    registry.OnInstanceAdded(/*materialIndex=*/0u, /*meshIndex=*/0u, /*mirrored=*/false);
    registry.OnInstanceAdded(/*materialIndex=*/1u, /*meshIndex=*/1u, /*mirrored=*/false);

    constexpr uint32_t kClassId = GPUDrawStreamBuilder::kColorClassBase;
    constexpr uint32_t kGroup   = 7u;
    const std::vector<uint32_t> colorClass{kClassId, kClassId};
    const std::vector<uint32_t> meshToGroup{kGroup, kGroup};

    const auto table = GPUDrawStreamBuilder::BuildColorBatchTable(registry, colorClass, meshToGroup);

    const WorldDrawBuilder::BatchKey key = MakeKey(1u, 1u, kClassId);
    const DrawStreamLookupKey lookup = ResolveDrawStreamLookupKey(
        key, GPUDrawStreamBuilder::kCascadeIndexNone,
        MaterialDepthClass::MaterialDependent, meshToGroup);

    EXPECT_EQ(lookup.classKey, kClassId);
    EXPECT_EQ(lookup.meshKey, kGroup);
    EXPECT_TRUE(HasRow(table, lookup.classKey, lookup.meshKey))
        << "resolved axes must address the row the scatter published";
    EXPECT_FALSE(HasRow(table, key.materialIndex, key.meshIndex))
        << "the raw key fields address no published range on either axis";
    EXPECT_FALSE(HasRow(table, key.colorClassId, key.meshIndex))
        << "the mesh axis alone is enough to miss under consolidation";
}

// Shadow slices are the other table: shared-depth-eligible casters of a
// (mesh, side) collapse onto a per-side class sentinel, so the color class is
// the wrong axis there even though it is the right one at cascade None.
TEST(DrawStreamLookupKey, ShadowCascadeResolvesSharedDepthSentinel)
{
    BatchRegistry registry;
    registry.OnInstanceAdded(0u, 0u, /*mirrored=*/false);
    registry.OnInstanceAdded(1u, 1u, /*mirrored=*/false);

    constexpr uint32_t kClassId = GPUDrawStreamBuilder::kColorClassBase;
    constexpr uint32_t kGroup   = 2u;
    const std::vector<uint8_t>  depthClass{kSingleSided, kSingleSided};
    const std::vector<uint32_t> meshToGroup{kGroup, kGroup};

    const auto table = GPUDrawStreamBuilder::BuildShadowBatchTable(registry, depthClass, meshToGroup);

    const WorldDrawBuilder::BatchKey key = MakeKey(1u, 1u, kClassId);
    const DrawStreamLookupKey lookup = ResolveDrawStreamLookupKey(
        key, /*cascadeIndex=*/0u, MaterialDepthClass::EligibleSingleSided, meshToGroup);

    EXPECT_EQ(lookup.classKey, GPUDrawStreamBuilder::kSharedDepthSingleSidedSentinel);
    EXPECT_EQ(lookup.meshKey, kGroup);
    EXPECT_TRUE(HasRow(table, lookup.classKey, lookup.meshKey));
    EXPECT_FALSE(HasRow(table, key.materialIndex, key.meshIndex));
    EXPECT_FALSE(HasRow(table, key.colorClassId, lookup.meshKey))
        << "the color class is not the shadow table's axis for an eligible caster";
}

// The main-view depth prepass shares the color table, so it must key on the
// color class even for a caster whose depth class would earn a sentinel in a
// cascade. cascadeIndex is the only thing that decides this.
TEST(DrawStreamLookupKey, CascadeNoneKeepsColorClassRegardlessOfDepthClass)
{
    constexpr uint32_t kClassId = GPUDrawStreamBuilder::kColorClassBase;
    const std::vector<uint32_t> meshToGroup{4u};
    const WorldDrawBuilder::BatchKey key = MakeKey(0u, 0u, kClassId);

    const DrawStreamLookupKey lookup = ResolveDrawStreamLookupKey(
        key, GPUDrawStreamBuilder::kCascadeIndexNone,
        MaterialDepthClass::EligibleSingleSided, meshToGroup);

    EXPECT_EQ(lookup.classKey, kClassId);
    EXPECT_EQ(lookup.meshKey, 4u);
}

// GE_DRAW_CONSOLIDATION=0 with the color merge off: both axes degrade to the
// raw key fields and the published table is the per-(material, mesh) one, so
// the resolver is byte-identical to the pre-merge lookup.
TEST(DrawStreamLookupKey, ConsolidationOffAndMergeOffDegradeToRawAxes)
{
    BatchRegistry registry;
    registry.OnInstanceAdded(3u, 5u, /*mirrored=*/false);

    const auto table = GPUDrawStreamBuilder::BuildColorBatchTable(registry, {}, {});

    const WorldDrawBuilder::BatchKey key = MakeKey(3u, 5u, /*colorClassId=*/3u);
    const DrawStreamLookupKey lookup = ResolveDrawStreamLookupKey(
        key, GPUDrawStreamBuilder::kCascadeIndexNone,
        MaterialDepthClass::MaterialDependent, /*meshPoolGroups=*/{});

    EXPECT_EQ(lookup.classKey, key.materialIndex);
    EXPECT_EQ(lookup.meshKey, key.meshIndex);
    EXPECT_TRUE(HasRow(table, lookup.classKey, lookup.meshKey));
}

// A mesh row with no plan entry resolves to the absent pseudo group, which the
// walks filter before looking anything up. The publisher merges such rows under
// the same pseudo key, so reader and publisher agree here too.
TEST(DrawStreamLookupKey, MeshPastTheGroupSpanResolvesToAbsentGroup)
{
    BatchRegistry registry;
    registry.OnInstanceAdded(1u, 5u, /*mirrored=*/false); // past the 2-entry span

    const std::vector<uint32_t> meshToGroup{0u, 0u};
    const auto table = GPUDrawStreamBuilder::BuildBatchTable(registry, meshToGroup);

    const WorldDrawBuilder::BatchKey key = MakeKey(1u, 5u, /*colorClassId=*/1u);
    const DrawStreamLookupKey lookup = ResolveDrawStreamLookupKey(
        key, GPUDrawStreamBuilder::kCascadeIndexNone,
        MaterialDepthClass::MaterialDependent, meshToGroup);

    EXPECT_EQ(lookup.meshKey, MeshPoolGroupPlan::kAbsentGroup);
    EXPECT_TRUE(HasRow(table, lookup.classKey, lookup.meshKey));
}

// The mesh-axis sentinel the renderer core carries must equal the engine-layer
// plan's, or an absent row would resolve to a key the publisher never wrote.
static_assert(MeshPoolGroupPlan::kAbsentGroup == GPUDrawStreamBuilder::kAbsentPoolGroup,
              "absent-group sentinels must match across the module boundary");
