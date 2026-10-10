#pragma once

#include "Types/Types.h"
#include <vector>
#include <span>

namespace GameEngine::TerrainECS
{

// Channels a control texel carries, in order: height target, density target.
// The GPU side is R8G8_UNORM, so this is also the CPU bytes per texel. Both
// channels are continuous and sampled through a LINEAR clamp sampler — a future
// discrete channel (a grass profile index) cannot simply join them here, because
// filtering would blend two indices into a third that names nothing.
inline constexpr uint32 kTerrainGrassFieldChannels = 2u;

// Derived grass control data. RG8 stores height/density targets, with 255/255
// exactly neutral. Empty Pixels means a neutral field and needs no GPU texture.
// The terrain compositor owns these bytes; neither height nor splat versions
// change when they change. Dimensions also record a neutral tile's bake lattice.
struct TerrainGrassField
{
    std::vector<uint8> Pixels;
    uint32 Width = 0;
    uint32 Height = 0;
    uint64 Version = 0;
    uint64 AppliedHash = 0;
    uint64 NonNeutralTexels = 0;
    bool Initialized = false;
    bool Dirty = false;
    uint32 DirtyMinX = 0, DirtyMinZ = 0, DirtyMaxX = 0, DirtyMaxZ = 0; // exclusive max

    bool IsActive() const { return !Pixels.empty(); }
};

struct ResolvedModifier;

// Re-evaluate an inclusive sample rectangle from neutral through the ordered
// targets. Negative maxima mean the full field. No height/splat data is read.
void ComposeTerrainGrassField(TerrainGrassField& field, uint32 width, uint32 height,
                              float32 sizeX, float32 sizeZ, float32 originX, float32 originZ,
                              std::span<const ResolvedModifier> modifiers,
                              int32 minX = 0, int32 minZ = 0, int32 maxX = -1, int32 maxZ = -1);

} // namespace GameEngine::TerrainECS
