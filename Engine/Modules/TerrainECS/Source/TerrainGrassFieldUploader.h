#pragma once

#include "TerrainECS/TerrainAtlas.h"
#include "TerrainECS/TerrainService.h"
#include <unordered_map>

namespace GameEngine::TerrainECS
{
class TerrainRenderFeature;

// Extraction-owned upload cache. Source generations and slot ownership decide
// uploads; idle views do not touch GPU control textures.
class TerrainGrassFieldUploader
{
public:
    bool UpdateUnified(TerrainRenderFeature& feature, TerrainHandle handle, TerrainGrassField& field);
    bool UpdateAtlas(TerrainRenderFeature& feature, TerrainHandle handle, TiledTerrainData& tiled,
                     const AtlasGeometry& geometry, std::span<const TileAtlasSlot> rows);
    void Forget(TerrainHandle handle);
    void ForgetRetired(const TerrainService& service);

private:
    struct SlotState
    {
        int32 TileX = -1, TileZ = -1;
        uint32 Generation = 0, Width = 0, Height = 0;
        uint64 Version = 0, CoarseVersion = 0;
        bool Initialized = false, Valid = false;
    };
    struct AtlasState
    {
        uint64 TextureId = 0;
        std::vector<SlotState> Slots;
    };
    std::unordered_map<uint64, AtlasState> m_Atlases;
    std::vector<uint8> m_Source;
    std::vector<uint8> m_Patch;
    static uint64 Key(TerrainHandle handle) { return (static_cast<uint64>(handle.Generation) << 32) | handle.Index; }
};
}
