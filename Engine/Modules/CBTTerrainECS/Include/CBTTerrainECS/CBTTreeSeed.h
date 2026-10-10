#pragma once

// CBTTreeSeed — what the CBT subdivision tree was seeded for, and when it must restart from its
// roots. A tree is valid only for the terrain and the depth cap it grew under: no kernel merges a
// leaf because the terrain under it was replaced or because it now sits below a lowered cap, so
// such leaves stay live at rest (a 512 m terrain opened after a 1 km one keeps leaves two levels
// deeper than its own cap) and the counters carry the previous terrain's history. Owned by
// CBTRenderFeature, which re-seeds the instance whenever RestartReason names a reason.

#include "CBTTerrain/CBTLayout.h" // kDomainPlanar

#include <cstdint>

namespace GameEngine::CBTTerrainECS
{

enum class CBTTreeRestart : uint8_t
{
    None,
    DomainChanged,   // planar <-> spherical
    TerrainRetired,  // a terrain was replaced, re-provisioned or removed (every scene open does this)
    DepthCapLowered, // the cap fell below the deepest cap the tree was allowed to reach
};

const char* ToString(CBTTreeRestart restart);

class CBTTreeSeed
{
  public:
    // The restart window. A request that arrives with no restart in the last kRestartSettleFrames
    // frames fires at once; requests inside the window are held until they have stopped changing
    // for this many frames. Longer than the gap between two steps of a drag: an inspector drag
    // writes one step per frame while the pointer moves, and a debug-port drag lands one every 3
    // to 7 frames (one set_component round trip). While a request is held the old tree keeps
    // drawing; a retire forces a full VertexEval, so it draws the new heights.
    static constexpr uint32_t kRestartSettleFrames = 16u;

    // The heap depth a restarted tree is refined to before its first draw (or the terrain's cap
    // when lower): 2^14 = 16,384 planar leaves, about 1/90 of the terrain's width per facet, so
    // a restart never draws the 4 or 24 root triangles that flatten every ridge and hill.
    static constexpr uint32_t kRestartSeedDepth = 14u;
    static_assert(kRestartSeedDepth >= 12u,
                  "a restart seeded shallower than about 4,000 planar leaves draws facets too coarse "
                  "for relief: the ridge-pose burst showed a 20 m wall vanish at 512 leaves");
    static_assert((1u << kRestartSeedDepth) <= CBTTerrain::kDefaultBisectorPoolSize / 32u,
                  "the restart seed must leave the pool to the screen-space refinement");

    // The tree was seeded from its roots for `domain` (kDomainPlanar / kDomainSpherical) under
    // `maxDepth`. Clears a pending retire and the settle hold.
    void Seeded(uint32_t domain, uint32_t maxDepth);

    // A terrain texture set was retired (TerrainRenderFeature::GetTerrainTextureRetireGeneration
    // advanced): the terrain under the tree is no longer the one it grew on.
    void NoteTerrainRetired()
    {
        m_TerrainRetired = true;
        ++m_RetireCount;
    }

    // Why the tree must restart in frame `frameCounter`, or None. A cap that rises is remembered
    // without a restart, so lowering it later back to where the tree was seeded still restarts:
    // leaves may have grown down to the higher cap in between.
    //
    // The first request fires at once, unless a restart fired in the last kRestartSettleFrames
    // frames; then the request is held, and fires once the domain, the cap and the retire count
    // have held unchanged for kRestartSettleFrames frames. A scene open or a terrain replacement
    // therefore restarts in the frame it lands in. An inspector drag of Size, Samples Per Meter,
    // Planet Radius or the depth override (a non-tiled resize retires a texture set per step)
    // restarts at its first step and once after it settles, rather than collapsing to its roots
    // on every step. Several calls in one frame (one per view) return the same answer once the
    // caller re-seeds (Seeded) after a restart.
    CBTTreeRestart RestartReason(uint32_t domain, uint32_t maxDepth, uint32_t frameCounter);

    uint32_t Domain() const { return m_Domain; }
    // A restart is held until its request settles (RestartReason); it fires on a later frame
    // with no change visible to the caller in that frame.
    bool IsRestartHeld() const { return m_Holding; }

  private:
    CBTTreeRestart Fire(CBTTreeRestart reason, uint32_t frameCounter);

    struct Request
    {
        uint32_t Domain = 0;
        uint32_t MaxDepth = 0;
        uint64_t RetireCount = 0;
        bool operator==(const Request&) const = default;
    };

    uint32_t m_Domain = CBTTerrain::kDomainPlanar;
    uint32_t m_DeepestAllowedDepth = 0;
    bool m_TerrainRetired = false;
    uint64_t m_RetireCount = 0;
    // The restart request as last seen, and the frame it was first seen in that form.
    bool m_Holding = false;
    Request m_HeldRequest{};
    uint32_t m_HeldSinceFrame = 0;
    // The frame the last restart fired in (the leading-edge window).
    bool m_HasRestarted = false;
    uint32_t m_LastRestartFrame = 0;
};

} // namespace GameEngine::CBTTerrainECS
