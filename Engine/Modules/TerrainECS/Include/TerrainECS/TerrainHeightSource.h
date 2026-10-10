#pragma once

#include "Types/Types.h"

namespace GameEngine::TerrainECS
{

/// What decides where a tiled terrain's rendered height comes from: its height pages (the paged
/// resolve, cbt_page.glsl) or its unified or atlas height texture.
struct TerrainHeightSourceInputs
{
    /// The modifier set has not been gathered yet, or the terrain's height page overlay
    /// (TiledTerrainData::PageOverlay) does not reflect it: not built yet, or refused over the
    /// platform's overlay cap (OverlayRefusal). Its pages would show its base without its modifiers
    /// and edits.
    bool ModifiersPending = true;
    /// The terrain's base is generated (procedural noise or flat): its pages are evaluated on demand,
    /// in the editor and the Player alike.
    bool GeneratedBase = false;
    /// The terrain's base is a heightmap whose cooked store is open: cooked into the project's cache
    /// in the editor, or the packaged terrain container in the Player (which never cooks on load).
    bool CookedStoreOpen = false;
    /// The terrain's page table is within the platform's cap (PageTableRefusal, HeightPageProfile) and
    /// its pyramid lies on the tile lattice where the CBT and the modifier overlay need it.
    bool PageTableFits = false;
    /// The device runs the wide CBT arm, the one that binds the page table and the page cache
    /// (bindings 22 and 23). The narrow arm (no 64-bit shader integers, WebGPU) has no storage
    /// buffer left for the page table and keeps the texture.
    bool GpuReadsPages = false;
};

/// True when the terrain's vertex height is read from its height pages: its modifiers are baked
/// into its page overlay, a page source exists for it (a generated base, or a heightmap's cooked store), its page
/// table fits on the GPU and the GPU arm reads pages. Otherwise it keeps its unified or atlas height
/// texture.
bool TerrainHeightIsPaged(const TerrainHeightSourceInputs& inputs);

} // namespace GameEngine::TerrainECS
