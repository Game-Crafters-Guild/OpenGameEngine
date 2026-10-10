#include "TerrainGrass/GrassPlacementElisionInputs.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::TerrainGrass;

namespace
{

// A fully populated input set, so every test below perturbs ONE term of something that is
// otherwise identical. Values are arbitrary but distinct: a builder that dropped a term would
// still produce equal blobs if the terms it kept happened to collide.
struct Fixture
{
    std::vector<uint8> PlaceParams{1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<uint8> TerrainParams{9, 10, 11, 12};
    std::vector<uint8> AtlasRows{13, 14};
    std::vector<uint32> AtlasBindless{101, 102, 103, 104, 105, 106, 107, 108};
    std::vector<uint32> SeededIndirectWords{201, 0, 0, 202, 201, 7, 524288};

    GrassPlacementElisionInputs Make() const
    {
        GrassPlacementElisionInputs inputs;
        inputs.PlaceParams = PlaceParams;
        inputs.Plan.Params.NearDensity = 21.0f;
        inputs.Plan.Params.FarRadius = 369.5f;
        inputs.Plan.Params.Falloff = 2.0f;
        inputs.Plan.Params.Seed = 3.0f;
        inputs.Plan.CellSpan = 95;
        inputs.Plan.CellCount = 9025;
        inputs.Plan.PlannedCandidates = 524288;
        inputs.Plan.Capacity = 524288;
        inputs.Plan.Lod1StartDistance = 66.5f;
        inputs.Plan.RangeReduced = true;
        inputs.TerrainParams = TerrainParams;
        inputs.TerrainContentEpoch = 7;
        inputs.AtlasIdentity = 0xABCDEF0102030405ull;
        inputs.AtlasTableVersion = 53;
        inputs.AtlasRows = AtlasRows;
        inputs.AtlasBindlessIndices = AtlasBindless;
        inputs.SeededIndirectWords = SeededIndirectWords;
        return inputs;
    }
};

Rendering::ElisionInputBlob Build(const GrassPlacementElisionInputs& inputs)
{
    Rendering::ElisionInputBlob blob;
    BuildGrassPlacementElisionBlob(inputs, blob);
    return blob;
}

} // namespace

// The whole gate rests on this: identical inputs must produce identical bytes, or the placement
// dispatch recomputes forever and the elision never engages.
TEST(GrassPlacementElision, IdenticalInputsProduceIdenticalBytes)
{
    const Fixture fixture;
    const Rendering::ElisionInputBlob first = Build(fixture.Make());
    const Rendering::ElisionInputBlob second = Build(fixture.Make());
    EXPECT_TRUE(first == second);
    EXPECT_GT(first.SizeBytes(), 0u);
}

// The other half: every term the kernels read must be able to force a recompute. A term left out
// of the blob is a term whose change is invisible to the gate, which is how an elided frame shows
// grass for the wrong camera, the wrong terrain, or the wrong atlas.
TEST(GrassPlacementElision, EveryTermChangesTheBlob)
{
    const Fixture fixture;
    const Rendering::ElisionInputBlob base = Build(fixture.Make());

    {
        Fixture perturbed = fixture;
        perturbed.PlaceParams[3] ^= 0x01u; // the camera moved by one bit
        EXPECT_FALSE(Build(perturbed.Make()) == base) << "place params are not in the blob";
    }
    {
        Fixture perturbed = fixture;
        perturbed.TerrainParams[1] ^= 0x01u;
        EXPECT_FALSE(Build(perturbed.Make()) == base) << "terrain params are not in the blob";
    }
    {
        Fixture perturbed = fixture;
        perturbed.AtlasRows[0] ^= 0x01u;
        EXPECT_FALSE(Build(perturbed.Make()) == base) << "atlas rows are not in the blob";
    }
    {
        Fixture perturbed = fixture;
        for (size_t index = 0; index < fixture.AtlasBindless.size(); ++index)
        {
            perturbed = fixture;
            perturbed.AtlasBindless[index] += 1u;
            EXPECT_FALSE(Build(perturbed.Make()) == base) << "atlas map " << index << " missing from elision inputs";
        }
    }
    {
        // The last word is the pool size; the ones before it are the two LOD sub-mesh records.
        Fixture perturbed = fixture;
        perturbed.SeededIndirectWords.back() += 1u;
        EXPECT_FALSE(Build(perturbed.Make()) == base) << "the seeded pool size is not in the blob";
    }
    {
        Fixture perturbed = fixture;
        perturbed.SeededIndirectWords[4] += 1u; // LOD 1's first index: a rebuilt blade mesh
        EXPECT_FALSE(Build(perturbed.Make()) == base) << "the seeded LOD records are not in the blob";
    }

    const auto mutated = [&](auto&& mutate)
    {
        GrassPlacementElisionInputs inputs = fixture.Make();
        mutate(inputs);
        return Build(inputs);
    };
    EXPECT_FALSE(mutated([](auto& i) { i.TerrainContentEpoch += 1; }) == base)
        << "the terrain content epoch is not in the blob";
    EXPECT_FALSE(mutated([](auto& i) { i.AtlasIdentity ^= 1ull; }) == base)
        << "the atlas identity is not in the blob";
    EXPECT_FALSE(mutated([](auto& i) { i.AtlasTableVersion += 1; }) == base)
        << "the atlas table version is not in the blob";
    EXPECT_FALSE(mutated([](auto& i) { i.Plan.Params.NearDensity += 1.0f; }) == base)
        << "the authored density is not in the blob";
    EXPECT_FALSE(mutated([](auto& i) { i.Plan.Params.FarRadius += 1.0f; }) == base)
        << "the fitted range is not in the blob";
    EXPECT_FALSE(mutated([](auto& i) { i.Plan.Params.Falloff += 1.0f; }) == base)
        << "the falloff is not in the blob";
    EXPECT_FALSE(mutated([](auto& i) { i.Plan.Params.Seed += 1.0f; }) == base)
        << "the placement seed is not in the blob";
    EXPECT_FALSE(mutated([](auto& i) { i.Plan.CellSpan += 1; }) == base)
        << "the cell span is not in the blob";
    EXPECT_FALSE(mutated([](auto& i) { i.Plan.CellCount += 1; }) == base)
        << "the cell count is not in the blob";
    EXPECT_FALSE(mutated([](auto& i) { i.Plan.PlannedCandidates += 1; }) == base)
        << "the planned candidate count is not in the blob";
    EXPECT_FALSE(mutated([](auto& i) { i.Plan.Capacity += 1; }) == base)
        << "the pool capacity is not in the blob";
    EXPECT_FALSE(mutated([](auto& i) { i.Plan.Lod1StartDistance += 1.0f; }) == base)
        << "the LOD 1 distance is not in the blob";
    EXPECT_FALSE(mutated([](auto& i) { i.Plan.RangeReduced = false; }) == base)
        << "the range-reduced flag is not in the blob";
}

// Spans are length-prefixed for this case: a shorter run whose remaining bytes match the longer
// one it followed would otherwise compare equal, and a terrain leaving the scene is exactly that
// shape.
TEST(GrassPlacementElision, AShorterSpanDoesNotCompareEqualToItsPrefix)
{
    Fixture longer;
    longer.TerrainParams = {9, 10, 11, 12};
    Fixture shorter = longer;
    shorter.TerrainParams = {9, 10, 11};
    EXPECT_FALSE(Build(longer.Make()) == Build(shorter.Make()));
}

// Empty spans are legal (no atlas terrain, no seeded constants yet) and must not make the builder
// produce a blob that collides with a populated one.
TEST(GrassPlacementElision, EmptySpansAreDistinctFromPopulatedOnes)
{
    const Fixture fixture;
    GrassPlacementElisionInputs empty = fixture.Make();
    empty.AtlasRows = {};
    empty.AtlasBindlessIndices = {};
    EXPECT_FALSE(Build(empty) == Build(fixture.Make()));

    Rendering::ElisionInputBlob first;
    Rendering::ElisionInputBlob second;
    BuildGrassPlacementElisionBlob(empty, first);
    BuildGrassPlacementElisionBlob(empty, second);
    EXPECT_TRUE(first == second);
}
