#pragma once

#include "TerrainECS/AtlasSlotPool.h"
#include "TerrainECS/TerrainAtlas.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace GameEngine::TerrainECS
{

// One tile the streaming manager wants resident this frame, with the priority
// distance the slot pool evicts on and its LOD (Full vs Coarse).
struct AtlasResidentTile
{
    TileCoord Coord{};
    float32 PriorityDistanceSq = 0.0f;
    bool IsFull = false;
};

// A slot whose GPU content must be (re)patched this frame — a freshly assigned tile
// (slot content stale) or an in-place LOD change (coarse<->Full). The Phase E extraction
// path packs the tile's heightfield into this slot and uploads it; the falsifiable
// Terrain.AtlasUpload signal is emitted for the same set.
struct AtlasUploadRequest
{
    TileCoord Coord{};
    uint32 Slot = 0;
    bool IsFull = false;
};

// Drives the resident-window atlas each frame: assigns/evicts slots via the
// AtlasSlotPool, rebuilds the indirection table, and emits the falsifiable
// runtime signals. Every signal is EDGE-triggered (fires only on a residency
// transition), so a parked camera — the same desired set every frame — produces
// zero signals and flat counters (the work-quiescence law; the #490 GPU
// quiescence law on the CPU side). Owned by the extraction system, which creates one lazily per
// terrain that outgrows the unified height source; a terrain on the unified path does zero atlas
// work and allocates no controller.
//
// Signals (grepped by the orchestrator; design §5):
//   Terrain.AtlasMap tile=(x,z) slot=N lod=full|coarse   — slot assigned
//   Terrain.AtlasMap tile=(x,z) slot=free                — slot released
//   Terrain.AtlasUpload tile=(x,z) slot=N lod=full|coarse — content (re)written
//   Terrain.AtlasEvict tile=(x,z) slot=N                 — slot evicted/reclaimed
//   Terrain.AtlasFallback tile=(x,z)                     — in-window, no slot -> coarse
class AtlasResidencyController
{
public:
    // (Re)configure for a terrain's geometry + slot budget. No-op (geometry/pool) when the grid
    // parameters are unchanged (so a steady terrain never rebuilds mid-flight). fadeSeconds is the
    // coarse->slot upgrade crossfade window (0 = instant swap, the pre-fade behavior); it is applied
    // every call without forcing a rebuild so a runtime toggle never churns the table.
    void Configure(uint32 tileRes, uint32 slotCount, uint32 framesInFlight,
                   uint32 tilesPerAxisX, uint32 tilesPerAxisZ, bool logSignals,
                   float32 fadeSeconds = kAtlasUpgradeFadeSeconds);
    bool IsConfigured() const { return m_Table.Geometry.IsValid(); }

    // Per-frame residency update from the streaming manager's desired tile set. deltaSeconds advances
    // the per-tile upgrade crossfades (time-driven, framerate-independent); pass the frame delta.
    // maxAssignsPerFrame caps NEW slot assignments this frame (kAtlasMaxAssignsPerFrame) so the
    // per-frame slot-upload spike stays bounded; deferred tiles stay in the coarse fallback until a
    // later frame. kAtlasNoAssignBudget disables the cap (the pure residency oracles).
    void Update(uint64 frameIndex, float32 deltaSeconds,
                const std::vector<AtlasResidentTile>& residentTiles,
                uint32 maxAssignsPerFrame = kAtlasNoAssignBudget);

    const AtlasIndirectionTable& Table() const { return m_Table; }
    const AtlasGeometry& Geometry() const { return m_Table.Geometry; }
    const AtlasSlotPool& Pool() const { return m_Pool; }

    // Slots whose GPU content the extraction path must (re)patch this frame (new assign or
    // LOD change). Empty on a quiescent frame — the atlas-upload quiescence oracle. (Distinct
    // from UploadsThisFrame(), which is the COUNT of these for the falsifiable counter.)
    const std::vector<AtlasUploadRequest>& UploadRequestsThisFrame() const { return m_UploadRequests; }
    // Tiles whose indirection row changed this frame — the effective height SOURCE flipped
    // (assign coarse->slot, evict/release slot->coarse, in-place coarse<->Full re-patch). The
    // extraction path publishes each tile's terrain-UV rect into the shared height dirty rect so
    // CBT bisectors cached under the previous source re-evaluate (the #505 dirty-rect law lifted
    // to indirection-row changes, not just texel uploads — a source flip that only the patch loops
    // cover leaves evicted-tile corners stale = the frontier sky-slivers). Empty on a quiescent
    // frame (edge-triggered), so a parked stable window publishes nothing.
    const std::vector<TileCoord>& RowTransitionsThisFrame() const { return m_RowTransitions; }
    // Bumps whenever a slot (re)assignment changes the indirection table (assign or evict),
    // so the indirection SSBO is re-uploaded only when the rows actually changed (quiescence).
    // An in-place LOD upgrade (same slot) does NOT bump it — the row is unchanged; only the
    // atlas texture content + the dirty rect carry that.
    uint64 TableVersion() const { return m_TableVersion; }

    // ---- Falsifiable per-frame counters (tests assert flat-when-parked) ----
    uint32 AssignsThisFrame() const { return m_AssignsThisFrame; }
    uint32 EvictionsThisFrame() const { return m_EvictionsThisFrame; }
    uint32 UploadsThisFrame() const { return m_UploadsThisFrame; }
    uint32 FallbacksThisFrame() const { return m_FallbacksThisFrame; }
    uint32 ResidentCount() const { return m_Pool.ResidentCount(); }
    // Tiles whose upgrade crossfade advanced this frame (still ramping 0->1). Zero once every
    // resident tile has settled to Fade=1, so a parked+settled camera keeps it flat — the crossfade
    // is the ONLY thing that ticks the table version between residency transitions (quiescence law:
    // a fade is active work; a settled frame is not). The crossfade count oracle reads this.
    uint32 ActiveFadesThisFrame() const { return m_ActiveFadesThisFrame; }

private:
    struct PrevTileState
    {
        bool IsFull = false;
        bool WasResident = false;
    };

    void RebuildTable(const std::vector<AtlasResidentTile>& residentTiles);

    // Advance active upgrade crossfades by deltaSeconds and return the count still ramping. A tile's
    // fade lives in m_Fades only while active (0 <= Fade < 1); it is erased the frame it reaches 1, so
    // a settled window holds no fade state and does zero work. Fade(coord) returns 1 for any resident
    // tile without an active entry (already settled) — RebuildTable writes that into the row.
    uint32 TickFades(float32 deltaSeconds);
    float32 FadeOf(TileCoord coord) const;

    AtlasSlotPool m_Pool;
    AtlasIndirectionTable m_Table;
    std::unordered_map<TileCoord, PrevTileState, TileCoordHash> m_PrevDesired;
    std::unordered_map<TileCoord, float32, TileCoordHash> m_Fades; // active upgrade crossfades only
    std::vector<AtlasUploadRequest> m_UploadRequests; // rebuilt each Update
    std::vector<TileCoord> m_RowTransitions;          // rebuilt each Update (source-flip publish)
    uint64 m_TableVersion = 0;
    float32 m_FadeSeconds = kAtlasUpgradeFadeSeconds;

    uint32 m_ConfiguredSlotCount = 0;
    uint32 m_ConfiguredFramesInFlight = 0;
    bool m_LogSignals = false;

    uint32 m_AssignsThisFrame = 0;
    uint32 m_EvictionsThisFrame = 0;
    uint32 m_UploadsThisFrame = 0;
    uint32 m_FallbacksThisFrame = 0;
    uint32 m_ActiveFadesThisFrame = 0;
};

} // namespace GameEngine::TerrainECS
