// S2 — sorted transparent run machinery (SortedTransparentRuns.h): run
// partitioning (coarse back-to-front slot order, contiguous regions), the CPU
// drain order (the past-capacity twin of the GPU key sort — must be BIT-EXACT
// against the packed 64-bit key order), the F4 tiebreak uniqueness guarantee,
// the old-ceiling boundary (>256 records is just a normal count now), and the
// run material key's share/split table, including the keyword width it shares
// with the opaque colour-class signature.

#include <gtest/gtest.h>

#include "Engine/Rendering/SortedTransparentRuns.h"
#include "Engine/Rendering/MaterialColorClassify.h"

#include <algorithm>
#include <random>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;

namespace
{
SortedTransparentRecord MakeRecord(uint32_t group, float viewDepth, uint32_t instanceIndex = 0)
{
    SortedTransparentRecord rec{};
    rec.InstanceIndex = instanceIndex;
    rec.IndexCount = 3;
    rec.RunGroup = group;
    rec.ViewDepth = viewDepth;
    return rec;
}

// Assert the drain order invariants against a record set + its partition:
// every run's region is contiguous and homogeneous, ordered back-to-front
// within, and the WHOLE order equals an ascending sort of the packed 64-bit
// keys (the exact thing the GPU bitonic sorts) — record index as tiebreak.
void ExpectDrainOrderInvariants(const std::vector<SortedTransparentRecord>& records,
                                const SortedTransparentRunPartition& partition,
                                const std::vector<uint32_t>& order)
{
    ASSERT_EQ(order.size(), records.size());

    // Permutation: every record drawn exactly once (no ceiling, no drops).
    std::vector<uint8_t> seen(records.size(), 0);
    for (uint32_t idx : order)
    {
        ASSERT_LT(idx, records.size());
        EXPECT_EQ(seen[idx], 0) << "record " << idx << " drawn twice";
        seen[idx] = 1;
    }

    // Region contiguity + within-run back-to-front.
    for (uint32_t slot = 0; slot < partition.RunCount.size(); ++slot)
    {
        const uint32_t group = partition.GroupOfSlot[slot];
        const uint32_t start = partition.RunStart[slot];
        const uint32_t count = partition.RunCount[slot];
        for (uint32_t i = 0; i < count; ++i)
        {
            const SortedTransparentRecord& rec = records[order[start + i]];
            EXPECT_EQ(rec.RunGroup, group)
                << "slot " << slot << " position " << i << " holds a foreign run's record";
            if (i > 0)
            {
                const SortedTransparentRecord& prev = records[order[start + i - 1]];
                // Back-to-front through the same monotonic transform the key uses.
                EXPECT_GE(FloatToSortableUint(prev.ViewDepth), FloatToSortableUint(rec.ViewDepth))
                    << "slot " << slot << " not back-to-front at " << i;
            }
        }
    }

    // Bit-exact key-order cross-check (only valid when indices fit the 16-bit
    // tiebreak — callers with more records skip this by keeping size <= 65536).
    if (records.size() <= 0x10000)
    {
        std::vector<uint32_t> keyOrder(records.size());
        for (uint32_t i = 0; i < records.size(); ++i)
            keyOrder[i] = i;
        std::vector<SortedTransparentKey> keys(records.size());
        for (uint32_t i = 0; i < records.size(); ++i)
        {
            keys[i] = MakeSortedTransparentKey(
                records[i].ViewDepth,
                static_cast<uint16_t>(partition.SlotOfGroup[records[i].RunGroup]),
                static_cast<uint16_t>(i));
        }
        std::sort(keyOrder.begin(), keyOrder.end(), [&](uint32_t a, uint32_t b)
                  { return SortedTransparentKeyLess(keys[a], keys[b]); });
        EXPECT_EQ(order, keyOrder) << "CPU drain order diverged from the packed key order";
    }
}
} // namespace

TEST(SortedTransparentRuns, PartitionOrdersRunsByFarthestMember)
{
    // Group 0 near-ish, group 1 has the farthest member, group 2 in between —
    // depth-INTERLEAVED across groups so the coarse order actually decides.
    std::vector<SortedTransparentRecord> records = {
        MakeRecord(0, 10.0f), MakeRecord(1, 90.0f), MakeRecord(2, 50.0f),
        MakeRecord(0, 30.0f), MakeRecord(1, 5.0f),  MakeRecord(2, 40.0f),
    };
    const auto part = BuildSortedTransparentRunPartition(records, 3);

    // Slot order: group 1 (max 90) -> group 2 (max 50) -> group 0 (max 30).
    ASSERT_EQ(part.GroupOfSlot.size(), 3u);
    EXPECT_EQ(part.GroupOfSlot[0], 1u);
    EXPECT_EQ(part.GroupOfSlot[1], 2u);
    EXPECT_EQ(part.GroupOfSlot[2], 0u);
    EXPECT_EQ(part.SlotOfGroup[1], 0u);
    EXPECT_EQ(part.SlotOfGroup[2], 1u);
    EXPECT_EQ(part.SlotOfGroup[0], 2u);

    // Contiguous regions: starts are the exclusive prefix of counts.
    EXPECT_EQ(part.RunCount[0], 2u);
    EXPECT_EQ(part.RunCount[1], 2u);
    EXPECT_EQ(part.RunCount[2], 2u);
    EXPECT_EQ(part.RunStart[0], 0u);
    EXPECT_EQ(part.RunStart[1], 2u);
    EXPECT_EQ(part.RunStart[2], 4u);

    const auto order = BuildSortedTransparentCpuOrder(records, part);
    ExpectDrainOrderInvariants(records, part, order);
}

TEST(SortedTransparentRuns, PartitionTieOnFarthestBreaksByDiscoveryOrder)
{
    std::vector<SortedTransparentRecord> records = {
        MakeRecord(0, 25.0f),
        MakeRecord(1, 25.0f), // same farthest member as group 0
    };
    const auto part = BuildSortedTransparentRunPartition(records, 2);
    EXPECT_EQ(part.GroupOfSlot[0], 0u); // discovery order wins the tie, deterministically
    EXPECT_EQ(part.GroupOfSlot[1], 1u);
}

TEST(SortedTransparentRuns, WithinRunOrderIsGlobalSortedOrder)
{
    // Randomized depths over 4 runs, including exact duplicates and the ±0
    // pair (the one input where a naive float compare and the monotonic
    // transform disagree — the CPU twin must match the GPU key bit-for-bit).
    std::mt19937 rng(777);
    std::uniform_real_distribution<float> depth(0.1f, 4000.0f);
    std::uniform_int_distribution<uint32_t> group(0, 3);

    std::vector<SortedTransparentRecord> records;
    for (uint32_t i = 0; i < 500; ++i)
        records.push_back(MakeRecord(group(rng), depth(rng), i));
    records.push_back(MakeRecord(2, -0.0f));
    records.push_back(MakeRecord(2, 0.0f));
    records.push_back(MakeRecord(1, 123.5f));
    records.push_back(MakeRecord(1, 123.5f)); // exact duplicate -> index tiebreak

    const auto part = BuildSortedTransparentRunPartition(records, 4);
    const auto order = BuildSortedTransparentCpuOrder(records, part);
    ExpectDrainOrderInvariants(records, part, order);
}

// F4: at EQUAL depth and equal run, the dense record-index tiebreak makes
// every key unique — a full-capacity worst case must produce strictly
// ascending keys and submission-order draws (deterministic, flicker-free).
TEST(SortedTransparentRuns, EqualDepthKeysAreUniqueAtFullCapacity)
{
    std::vector<SortedTransparentRecord> records;
    records.reserve(kSortedTransparentSortCapacity);
    for (uint32_t i = 0; i < kSortedTransparentSortCapacity; ++i)
        records.push_back(MakeRecord(0, 42.0f, /*instanceIndex=*/i * 70000u));

    const auto part = BuildSortedTransparentRunPartition(records, 1);

    SortedTransparentKey prev{};
    for (uint32_t i = 0; i < records.size(); ++i)
    {
        const SortedTransparentKey key = MakeSortedTransparentKey(
            records[i].ViewDepth, static_cast<uint16_t>(part.SlotOfGroup[0]),
            static_cast<uint16_t>(i));
        if (i > 0)
        {
            EXPECT_TRUE(SortedTransparentKeyLess(prev, key))
                << "key collision at record " << i << " (F4)";
        }
        prev = key;
    }

    // Equal depth everywhere -> the order IS submission order.
    const auto order = BuildSortedTransparentCpuOrder(records, part);
    for (uint32_t i = 0; i < order.size(); ++i)
        EXPECT_EQ(order[i], i);
}

// The old 256-draw ceiling boundary: 300 visible records are simply a normal
// count for the drain — every one drawn, strict back-to-front, no fallback.
TEST(SortedTransparentRuns, PastOldCeiling300RecordsDrawSorted)
{
    std::mt19937 rng(300);
    std::uniform_real_distribution<float> depth(1.0f, 900.0f);
    std::vector<SortedTransparentRecord> records;
    for (uint32_t i = 0; i < 300; ++i)
        records.push_back(MakeRecord(0, depth(rng), i));

    const auto part = BuildSortedTransparentRunPartition(records, 1);
    const auto order = BuildSortedTransparentCpuOrder(records, part);
    ExpectDrainOrderInvariants(records, part, order);
}

// Past the GPU sort capacity (F6 clamp): the CPU twin carries ANY count with
// the same run layout + order contract — this is what makes "no ceiling, no
// fallback" true above kSortedTransparentSortCapacity.
TEST(SortedTransparentRuns, PastGpuCapacityCpuTwinStaysCorrect)
{
    std::mt19937 rng(2049);
    std::uniform_real_distribution<float> depth(0.5f, 10000.0f);
    std::uniform_int_distribution<uint32_t> group(0, 5);

    const uint32_t count = kSortedTransparentSortCapacity + 453u;
    std::vector<SortedTransparentRecord> records;
    records.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        records.push_back(MakeRecord(group(rng), depth(rng), i));

    const auto part = BuildSortedTransparentRunPartition(records, 6);
    const auto order = BuildSortedTransparentCpuOrder(records, part);
    ExpectDrainOrderInvariants(records, part, order);
}

// --- Run material key: what shares a run, what splits one ------------------

namespace
{
Material MakeRunMaterial(const char* name, const char* surface,
                         const std::optional<MaterialBlendState>& blend,
                         uint64_t userKeywordHash = 0, bool customVertexShader = false)
{
    Material mat = Material::TestFactory::Create(GUID{}, name, 256);
    Material::TestFactory::SetAlphaMode(mat, MaterialAlphaMode::Blend);
    Material::TestFactory::SetBlendState(mat, blend);
    MaterialCompileSpec spec{};
    spec.surfaceShaderPath = surface;
    spec.lightingModel = "standardpbr";
    spec.customVertexShader = customVertexShader;
    Material::TestFactory::SetCompileSpec(mat, spec);
    Rendering::ShaderVariantKey vk{};
    vk.materialKeywords = Rendering::MaterialKeyword::AlphaBlend;
    vk.userKeywordHash = userKeywordHash;
    Material::TestFactory::SetVariantKey(mat, vk);
    return mat;
}
} // namespace

TEST(SortedTransparentRuns, MaterialKeySharesAcrossParamOnlyVariance)
{
    // Two glass materials that differ only in params/textures (which ride the
    // bindless materialIndex fetch) — SAME key, one run, one bind.
    const Material a = MakeRunMaterial("glassA", "standard_pbr.surface", std::nullopt);
    const Material b = MakeRunMaterial("glassB", "standard_pbr.surface", std::nullopt);
    EXPECT_TRUE(MakeSortedTransparentRunMaterialKey(a) == MakeSortedTransparentRunMaterialKey(b));
}

TEST(SortedTransparentRuns, MaterialKeySplitsOnBlendEquation)
{
    MaterialBlendState premult{};
    premult.SrcColorFactor = MaterialBlendFactor::One;
    premult.DstColorFactor = MaterialBlendFactor::OneMinusSrcAlpha;
    const Material straight = MakeRunMaterial("straight", "standard_pbr.surface", std::nullopt);
    const Material hwPremult = MakeRunMaterial("premult", "standard_pbr.surface", premult);
    // Hardware premultiplied is a distinct blend equation -> its own PSO ->
    // its own run (the design's premultiplied caveat, F1).
    EXPECT_FALSE(MakeSortedTransparentRunMaterialKey(straight) ==
                 MakeSortedTransparentRunMaterialKey(hwPremult));
}

TEST(SortedTransparentRuns, MaterialKeySplitsOnSurfaceProgramAndUserKeywords)
{
    const Material pbr = MakeRunMaterial("pbr", "standard_pbr.surface", std::nullopt);
    const Material water = MakeRunMaterial("water", "water_stylized.surface", std::nullopt);
    EXPECT_FALSE(MakeSortedTransparentRunMaterialKey(pbr) ==
                 MakeSortedTransparentRunMaterialKey(water));

    // User keywords compile different SPIR-V; ColorClassSignature never needed
    // this lane (Blend is not color-merge eligible) but the run key must.
    const Material plain = MakeRunMaterial("plain", "standard_pbr.surface", std::nullopt, 0);
    const Material keyworded = MakeRunMaterial("kw", "standard_pbr.surface", std::nullopt, 0xABCDEF12u);
    EXPECT_FALSE(MakeSortedTransparentRunMaterialKey(plain) ==
                 MakeSortedTransparentRunMaterialKey(keyworded));
}

TEST(SortedTransparentRuns, MaterialKeySplitsOnCustomVertexShader)
{
    // customVertexShader changes the compiled variant AND clamps vertexFlags
    // to None at compile time, so the geometry key cannot catch the
    // difference — the material key must split it or two Blend materials
    // differing only here would fuse into one run and the representative's
    // pipeline would draw the other's records (R2-3).
    const Material plain = MakeRunMaterial("plain", "standard_pbr.surface", std::nullopt);
    const Material procedural = MakeRunMaterial("proc", "standard_pbr.surface", std::nullopt,
                                                /*userKeywordHash=*/0,
                                                /*customVertexShader=*/true);
    EXPECT_FALSE(MakeSortedTransparentRunMaterialKey(plain) ==
                 MakeSortedTransparentRunMaterialKey(procedural));
}

TEST(SortedTransparentRuns, MaterialKeyAndColorClassSplitOnEveryKeywordBit)
{
    // Both keys that put several materials on one pipeline, this run key and
    // the opaque colour-class signature, carry the whole 64-bit keyword field:
    // two materials that differ in one keyword compile different programs, so
    // a key that dropped the bit would draw one through the other's pipeline.
    constexpr uint32_t kKeywordBits = 64;
    const Material plain = MakeRunMaterial("plain", "standard_pbr.surface", std::nullopt);
    for (uint32_t bit = 0; bit < kKeywordBits; ++bit)
    {
        const auto keyword = static_cast<Rendering::MaterialKeyword>(uint64_t{1} << bit);
        if (keyword == Rendering::MaterialKeyword::AlphaBlend)
            continue; // every run material carries it
        Material keyed = MakeRunMaterial("keyed", "standard_pbr.surface", std::nullopt);
        Rendering::ShaderVariantKey variantKey = keyed.GetVariantKey();
        variantKey.materialKeywords |= keyword;
        Material::TestFactory::SetVariantKey(keyed, variantKey);
        EXPECT_FALSE(MakeSortedTransparentRunMaterialKey(plain) ==
                     MakeSortedTransparentRunMaterialKey(keyed))
            << "bit " << bit;
        EXPECT_FALSE(ComputeColorClassSignature(plain) == ComputeColorClassSignature(keyed))
            << "bit " << bit;
    }
}
