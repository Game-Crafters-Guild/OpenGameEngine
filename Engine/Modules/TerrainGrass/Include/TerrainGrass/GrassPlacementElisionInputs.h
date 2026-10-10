#pragma once

#include "Rendering/Core/RecomputeElision.h"
#include "TerrainGrass/GrassPlacementModel.h"
#include "Types/Types.h"

#include <span>

namespace GameEngine::TerrainGrass
{

/// Everything this view's grass placement dispatch reads, gathered so the recompute-elision gate
/// can compare one frame's inputs against the last byte for byte.
///
/// The rule the members follow: a term belongs here when changing it changes what the placement
/// and plan kernels write. Texture CONTENT is the one input with no CPU-side bytes to compare, so
/// it enters as a monotonic epoch supplied by the terrain feature that owns it.
///
/// Deliberately absent, because the kernels' output does not depend on them: the terrain params
/// SSBO handle and ring slot (the ring rotates every frame while its CONTENT, which is here, may
/// not have changed), and the device frame index.
struct GrassPlacementElisionInputs
{
    /// The per-view GPU block (TerrainGrassRenderFeature::GrassPlaceParamsGPU) as raw bytes:
    /// frustum planes, camera position, cell window, bounds, view rotation, viewport height,
    /// the CPU fit's range scale and the terrain summary.
    std::span<const uint8> PlaceParams;

    /// The budget fit this view was planned against. Appended field by field below, never as a
    /// struct: padding bytes are indeterminate and would make two identical plans compare unequal.
    GrassPlacementPlan Plan{};

    /// The terrains[] array both kernels index, exactly as uploaded. Compared by content rather
    /// than by version so an authored edit that lands on the same version still recomputes.
    std::span<const uint8> TerrainParams;

    /// Monotonic counter over every terrain heightmap/splatmap/normalmap/atlas upload and every
    /// texture retirement. Height and splat content decide where blades stand and whether they
    /// are placed at all, and neither is readable from the CPU cheaply enough to compare.
    uint64 TerrainContentEpoch = 0;

    /// The resident-window atlas the placement resolves terrain UVs through. Identity survives a
    /// destroy-and-recreate that reuses a table version, which the version alone cannot see.
    uint64 AtlasIdentity = 0;
    uint64 AtlasTableVersion = 0;
    std::span<const uint8> AtlasRows;
    /// The eight atlas map indices the kernels tap (height, height-coarse, normal, normal-coarse,
    /// splat, splat-coarse, grass, grass-coarse). A re-registered bindless slot changes which texture is sampled
    /// without changing anything else here.
    std::span<const uint32> AtlasBindlessIndices;

    /// The words the CPU seeds into the indirect args block once per slot: each LOD's sub-mesh
    /// record (index count, first index, vertex offset) and then the pool size. They are seeded
    /// inside the placement pass, so an elided frame does not re-seed them and a change must
    /// recompute. The WORDS rather than the struct that holds them: a struct carries padding, and
    /// padding bytes are indeterminate.
    std::span<const uint32> SeededIndirectWords;
};

/// Append `inputs` to `blob` in a fixed order, so two frames with equal inputs produce equal bytes.
///
/// Spans are length-prefixed: without the length, a span that shrank while its remaining bytes
/// stayed equal would compare equal to the longer one it followed.
void BuildGrassPlacementElisionBlob(const GrassPlacementElisionInputs& inputs,
                                    Rendering::ElisionInputBlob& blob);

} // namespace GameEngine::TerrainGrass
