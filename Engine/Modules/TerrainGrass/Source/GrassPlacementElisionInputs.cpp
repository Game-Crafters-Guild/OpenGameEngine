#include "TerrainGrass/GrassPlacementElisionInputs.h"

namespace GameEngine::TerrainGrass
{

namespace
{

// Length-prefixed, so a span that shrank while its remaining bytes stayed equal cannot compare
// equal to the longer one it followed.
void AppendSpan(Rendering::ElisionInputBlob& blob, std::span<const uint8> bytes)
{
    blob.Append(static_cast<uint64>(bytes.size()));
    blob.AppendBytes(bytes.data(), bytes.size());
}

} // namespace

void BuildGrassPlacementElisionBlob(const GrassPlacementElisionInputs& inputs,
                                    Rendering::ElisionInputBlob& blob)
{
    AppendSpan(blob, inputs.PlaceParams);

    // Field by field: GrassPlacementPlan carries padding, and padding bytes are indeterminate.
    blob.Append(inputs.Plan.Params.NearDensity);
    blob.Append(inputs.Plan.Params.FarRadius);
    blob.Append(inputs.Plan.Params.Falloff);
    blob.Append(inputs.Plan.Params.Seed);
    blob.Append(inputs.Plan.CellSpan);
    blob.Append(inputs.Plan.CellCount);
    blob.Append(inputs.Plan.PlannedCandidates);
    blob.Append(inputs.Plan.Capacity);
    blob.Append(inputs.Plan.Lod1StartDistance);
    blob.Append(static_cast<uint8>(inputs.Plan.RangeReduced ? 1u : 0u));

    AppendSpan(blob, inputs.TerrainParams);
    blob.Append(inputs.TerrainContentEpoch);

    blob.Append(inputs.AtlasIdentity);
    blob.Append(inputs.AtlasTableVersion);
    AppendSpan(blob, inputs.AtlasRows);
    blob.Append(static_cast<uint64>(inputs.AtlasBindlessIndices.size()));
    for (const uint32 index : inputs.AtlasBindlessIndices)
        blob.Append(index);

    blob.Append(static_cast<uint64>(inputs.SeededIndirectWords.size()));
    for (const uint32 word : inputs.SeededIndirectWords)
        blob.Append(word);
}

} // namespace GameEngine::TerrainGrass
