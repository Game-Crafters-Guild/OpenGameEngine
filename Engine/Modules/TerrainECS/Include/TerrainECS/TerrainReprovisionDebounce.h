#pragma once

#include "Types/Types.h"

#include <unordered_map>

namespace GameEngine::TerrainECS
{

// Consecutive Update ticks a tiled terrain's structural config (SamplesPerMeter /
// world size) must hold unchanged before the extraction system tears the terrain
// down and rebuilds it. An inspector numeric-field drag emits a distinct value
// almost every frame, and the re-provision compare is a raw float !=, so without a
// settle window a single 1->2 SamplesPerMeter drag fired dozens of full
// destroy+recreate cycles per second — each rebuilding the whole tile set, the
// unified/atlas GPU textures, and restarting streaming. That churn thrashed VRAM
// (every cycle allocates a new texture set while the old is still quarantined) and,
// combined with the bindless-descriptor leak on terrain texture teardown, left
// dangling descriptors in the global bindless array -> device-lost. Five ticks
// (~80 ms at 60 fps) is imperceptible for a settled edit yet collapses a whole drag
// to one re-provision on release.
inline constexpr uint32 kReprovisionSettleFrames = 4;

// Coalesces an interactive edit of a tiled terrain's structural config into a
// SINGLE re-provision once the value settles. Observe() records this frame's desired
// config and returns true only when the SAME config has held for `settleFrames`
// consecutive ticks; while it is still changing it returns false so the caller keeps
// rendering the existing terrain untouched (no teardown, no churn). Pure + header-only
// so the coalescing law is directly unit-tested (TerrainAtlasTests) with no device.
struct TerrainReprovisionDebounce
{
    float32 SamplesPerMeter = 0.0f;
    float32 SizeX = 0.0f;
    float32 SizeZ = 0.0f;
    uint32 StableFrames = 0;
    bool Seeded = false;

    bool Observe(float32 samplesPerMeter, float32 sizeX, float32 sizeZ, uint32 settleFrames)
    {
        if (!Seeded || samplesPerMeter != SamplesPerMeter || sizeX != SizeX || sizeZ != SizeZ)
        {
            SamplesPerMeter = samplesPerMeter;
            SizeX = sizeX;
            SizeZ = sizeZ;
            StableFrames = 0;
            Seeded = true;
            return false;
        }
        ++StableFrames;
        return StableFrames >= settleFrames;
    }
};

// How a live tiled terrain differs from the Components::Terrain it was provisioned for
// (TerrainService::CompareTiledTerrainConfig).
struct TiledTerrainEdit
{
    bool ResolutionChanged = false; // SamplesPerMeter
    bool SizeChanged = false;       // SizeX / SizeZ
    // A new base source, another heightmap asset, or new content for the same asset
    // (TerrainService::IsTiledTerrainBaseCurrent).
    bool BaseChanged = false;

    bool Structural() const { return ResolutionChanged || SizeChanged; }
};

// The extraction system's re-provision rule for the tiled terrain in slot `terrainIndex`,
// one Update tick. Returns true when the terrain is to be torn down and provisioned again
// from its component this tick.
//   - A structural edit (density or size) re-provisions once the same values have held for
//     `settleFrames` ticks (TerrainReprovisionDebounce: an inspector drag emits a new value
//     almost every frame).
//   - A base change re-provisions at once. It is a discrete edit, and every resident,
//     coarse and in-flight tile was filled from the old base.
//   - With no structural edit, the slot's debounce state is dropped, so the next edit
//     settles from scratch.
// Pure, so the rule is tested directly (TerrainRegionBakeTests) with no device.
inline bool StepTiledReprovision(std::unordered_map<uint32, TerrainReprovisionDebounce>& debounces,
                                 uint32 terrainIndex, const TiledTerrainEdit& edit,
                                 float32 samplesPerMeter, float32 sizeX, float32 sizeZ,
                                 uint32 settleFrames)
{
    bool settled = false;
    if (edit.Structural())
        settled = debounces[terrainIndex].Observe(samplesPerMeter, sizeX, sizeZ, settleFrames);
    else
        debounces.erase(terrainIndex);
    return settled || edit.BaseChanged;
}

} // namespace GameEngine::TerrainECS
