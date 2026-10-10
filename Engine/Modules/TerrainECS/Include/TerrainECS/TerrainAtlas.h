#pragma once

#include "TerrainECS/AtlasSlotPool.h" // kAtlasNoSlot
#include "Types/Types.h"

#include <cstdint>
#include <functional>
#include <vector>

// Resident-window atlas: geometry, the per-tile indirection row, the apron /
// mixed-LOD edge-ownership rule, and a CPU sampler that mirrors the GLSL resolve
// (cbt_atlas.glsl) bit-for-bit. This module is device-free and pure so the
// arc-law oracles (sampling parity, shared-edge continuity, out-of-window
// fallback) validate the exact math the shader will run — the same
// CPU-mirror-is-the-oracle discipline CBTPlanetShading.h holds for the sphere.
//
// The two risks this mirror exists to hold: mixed-LOD shared edges must resolve to
// the same texel from both sides, and a UV outside the resident window must take the
// coarse fallback rather than sampling a neighbour's page.
namespace GameEngine::TerrainECS
{

// 1-texel gutter per slot side, so hardware bilinear at a slot's interior edge
// reads a duplicated edge texel instead of the neighbouring slot's data
// (design §3.1 / Risk 3 correction: slotStride = tileRes + 2, not tileRes).
inline constexpr uint32 kAtlasApron = 1u;

// The per-terrain coarse height/normal/splat field (design §8 Risk 3) an out-of-window (slot-less)
// tile resolves through, so the fallback is height-continuous with resident relief at the window edge
// instead of a flat cliff / sky-gap. A fixed low-res square grid (a downsample of the whole terrain);
// coarseDim^2 * 12 B/texel (height R32F + splat RGBA8 + normal RG16F) = ~192 KiB at 128. Sampled
// endpoint-exact (SampleGridBilinearTexel),
// so the CPU AtlasHeightSampler and the GLSL fallback stay bit-for-bit locked.
//
// At a large world where most tiles are permanently slot-less (her 10240 m = 400 tiles vs ~81 slots),
// this field IS the surface for the majority of the terrain. 64 spread over 20 tiles was ~3 texels/tile —
// the coarse NORMAL derived from it collapsed toward flat-up, which reads as a tile-shaped flat-shaded
// trapezoid against a resident neighbour, and the coarse HEIGHT under-fit distant relief (the frontier
// step / torn-quad amplifier). 128 doubles the axis resolution (the #525-ledgered lever) at negligible
// VRAM/rebuild cost: it does NOT make a slot-less tile full-res (the resident-window design bounds that —
// true whole-terrain-at-altitude detail is the clipmap/paging arc), but it markedly softens both.
inline constexpr uint32 kAtlasCoarseFieldDim = 128u;

// A slot carries the tile's height (R32F, 4 B) + splat (RGBA8, 4 B) + normal (RG16F, 4 B) in three
// same-shaped slot textures; 12 B/texel total is the VRAM one slot's texel footprint consumes.
inline constexpr uint32 kAtlasSlotBytesPerTexel = 12u;

// VRAM budget the resident-window atlas (all three slot textures, square-padded to slotsPerRow^2)
// may occupy per terrain. DeriveAtlasSlotCount fits the slot budget to this, so a bigger world gets
// a bigger resident detail window until the budget caps it and the coarse field carries the rest.
// The historical fixed 36 slots (~455 MiB at 1025-res tiles) under-covered large terrains (her 10240 m
// world is 400 tiles at 512 m) — the resident detail became a small island in a coarse sea. This
// budget scales the window with the world. The #508 memory lever: raise for more resident detail at
// more VRAM, lower to reclaim it. GE_TERRAIN_ATLAS_SLOTS overrides the derived value outright.
inline constexpr uint64 kAtlasVramBudgetBytes = 1024ull * 1024ull * 1024ull; // 1 GiB

// Max NEW slot assignments the residency controller commits per frame. Each assign re-uploads a
// full slot (height + splat + normal ~= 12.6 MiB at 1025-res tiles), so an unbudgeted window that
// fills or recenters in one frame is a multi-hundred-MiB upload cliff — the movement hiccup at large
// terrains, made worse by a bigger window. Capping assigns spreads the fill over a few frames; the
// deferred tiles stay in the coarse fallback (now continuous base relief, not a flat sheet) until
// their turn, so the surface degrades gracefully instead of stalling. Nearest-first ordering fills
// the tiles under the camera first. Sentinel = unbudgeted (used by the pure residency oracles).
inline constexpr uint32 kAtlasMaxAssignsPerFrame = 6u;
inline constexpr uint32 kAtlasNoAssignBudget = 0xFFFFFFFFu;

// Resident-window slot budget scaled to the terrain's tile grid and capped by kAtlasVramBudgetBytes.
// The atlas is square-padded to slotsPerRow^2 = ceil(sqrt(slots))^2 slots, so the VRAM cap bounds
// slotsPerRow (not the raw slot count): the largest N with N^2 * bytesPerSlot <= budget. Returns at
// least 1 and never more than the terrain's total tile count (a world smaller than the budget is
// fully resident). Pure + directly unit-testable — the slot-derivation oracle runs this exact math.
uint32 DeriveAtlasSlotCount(uint32 tilesPerAxisX, uint32 tilesPerAxisZ, uint32 tileRes,
                            uint64 vramBudgetBytes, uint32 bytesPerTexel = kAtlasSlotBytesPerTexel);

// One indirection-table row (design §3.2). std430, 16 B — bound as an SSBO
// indexed by tileIndex = tileZ * tilesPerAxisX + tileX. Mirror TileAtlasSlot in
// cbt_atlas.glsl exactly.
struct TileAtlasSlot
{
    uint32 Slot = kAtlasNoSlot;   // atlas slot index, or kAtlasNoSlot (not resident)
    // The owning slot's generation from AtlasSlotPool. CPU-side it is the release/
    // reset staleness discriminator (AtlasSlotPool::Release refuses a stale index).
    // RESERVED for the GPU, and stays that way: no shader reads it. The stale-row hazard
    // (Risk 2, eviction vs frames-in-flight) is already prevented by the indirection ring +
    // slot quarantine >= frames-in-flight + single-queue serialization — the quarantine suffices,
    // so no shader-side generation check is needed (proof at cbt_layout.glsl binding 17). Kept in
    // the row only so the SSBO layout is final if a future multi-queue path ever needs it.
    uint32 Generation = 0;
    float32 LodBias = 0.0f;       // coarse tile: kAtlasLodBiasCoarse; Full tile: 0 (hint only)
    // Coarse->slot upgrade crossfade factor in [0,1]: 0 = render the out-of-window coarse field, 1 =
    // render the resident slot at full detail. Ramped by AtlasResidencyController when a tile assigns
    // (the residency-frontier pop killer — the surface mixes coarse<->slot normal/splat by this so a
    // newly-resident tile fades its detail in over a short window instead of swapping instantly). A
    // resident tile with no active fade settles to 1; the height sample never reads it (fragment-only),
    // so it stays a per-tile hint that costs the geometry path nothing. Occupies the old std430 pad word.
    float32 Fade = 1.0f;
};
static_assert(sizeof(TileAtlasSlot) == 16, "TileAtlasSlot must be std430 16 B");

// LodBias values (design §8 Risk 3, "Define LodBias"): a hint, never a
// correctness input to the height sample. A coarse-resident tile sets Coarse so
// the surface can soften shading / a debug view can tint coarse vs Full; a Full
// tile sets 0.
inline constexpr float32 kAtlasLodBiasFull = 0.0f;
inline constexpr float32 kAtlasLodBiasCoarse = 1.0f;

// Coarse->slot upgrade crossfade window (TileAtlasSlot::Fade ramp 0->1). CDLOD hides the same
// LOD-swap pop with distance-driven morph bands; the resident-window atlas has no continuous LOD
// parameter, so the fade is time-driven instead — a tile that just became resident blends its slot
// detail in over this many seconds. Short enough to feel instant on a settled camera, long enough
// that a streaming upgrade reads as a fade rather than a snap. A fadeSeconds of 0 restores the
// pre-fade instant swap (the disable-and-fail discriminator + the runtime kill switch).
inline constexpr float32 kAtlasUpgradeFadeSeconds = 0.4f;

// Fixed-size slot grid packed into one square texture per source map
// (height/splat/normal share this shape). slotStride texels per slot side incl.
// the apron; slotsPerRow slots per atlas row. A slot is NOT spatially adjacent
// to the tile it holds — the indirection table maps tile -> slot (design §3.1).
struct AtlasGeometry
{
    uint32 TileRes = 0;         // interior samples per tile axis (e.g. 1025)
    uint32 SlotStride = 0;      // TileRes + 2*kAtlasApron
    uint32 SlotsPerRow = 0;     // ceil(sqrt(SlotCount))
    uint32 SlotCount = 0;
    uint32 AtlasDim = 0;        // SlotsPerRow * SlotStride (square)
    uint32 TilesPerAxisX = 0;
    uint32 TilesPerAxisZ = 0;

    bool IsValid() const { return TileRes >= 2 && SlotCount > 0 && AtlasDim > 0; }

    // Slot grid position (col,row) and the top-left texel of the slot (incl. apron).
    uint32 SlotCol(uint32 slot) const { return slot % SlotsPerRow; }
    uint32 SlotRow(uint32 slot) const { return slot / SlotsPerRow; }
    uint32 SlotOriginTexelX(uint32 slot) const { return SlotCol(slot) * SlotStride; }
    uint32 SlotOriginTexelY(uint32 slot) const { return SlotRow(slot) * SlotStride; }
    // Top-left texel of the slot's INTERIOR (tile data), i.e. past the apron.
    uint32 SlotInteriorTexelX(uint32 slot) const { return SlotOriginTexelX(slot) + kAtlasApron; }
    uint32 SlotInteriorTexelY(uint32 slot) const { return SlotOriginTexelY(slot) + kAtlasApron; }

    uint32 TileIndex(int32 tx, int32 tz) const
    {
        return static_cast<uint32>(tz) * TilesPerAxisX + static_cast<uint32>(tx);
    }
};

// Build the atlas geometry for a tile resolution and slot budget. slotsPerRow is
// ceil(sqrt(slotCount)) so the square atlas holds every slot. tileRes is the
// per-tile native heightmap width (== TileConfig.HeightmapWidth).
AtlasGeometry MakeAtlasGeometry(uint32 tileRes, uint32 slotCount,
                                uint32 tilesPerAxisX, uint32 tilesPerAxisZ);

// The per-tile indirection table (design §3.2). One row per tile in the WHOLE
// terrain (tile counts are small even for 50 km worlds). Rows are rewritten each
// frame from the slot pool's state; a non-resident tile keeps kAtlasNoSlot.
struct AtlasIndirectionTable
{
    AtlasGeometry Geometry;
    std::vector<TileAtlasSlot> Rows; // TilesPerAxisX * TilesPerAxisZ

    void Resize(const AtlasGeometry& geo);
    void Clear(); // every row -> kAtlasNoSlot

    bool InBounds(int32 tx, int32 tz) const
    {
        return tx >= 0 && tz >= 0 &&
               tx < static_cast<int32>(Geometry.TilesPerAxisX) &&
               tz < static_cast<int32>(Geometry.TilesPerAxisZ);
    }
    const TileAtlasSlot& Row(int32 tx, int32 tz) const { return Rows[Geometry.TileIndex(tx, tz)]; }
    TileAtlasSlot& Row(int32 tx, int32 tz) { return Rows[Geometry.TileIndex(tx, tz)]; }
};

// ---- Risk 1: mixed-LOD shared edges across disjoint slots ----
//
// Tile T's slot and tile T+1's slot are disjoint regions, each keeping its OWN
// copy of the shared boundary texels. Those two copies must be kept EQUAL. When
// neighbour LODs match (both Full or both coarse) the copies are computed
// identically (the terrain's base + endpoint-exact upsample) and agree by
// construction. When they differ — coarse T beside Full T+1, the frontier case
// — the coarse slot must copy the Full slot's edge into its own edge/apron
// rather than write its approximate value (mirrors the shipped interior-clamp
// fix as an apron-fill). The Full side always owns the shared texel.
enum class AtlasEdgeSource : uint8
{
    Self,     // keep this slot's own edge (self authoritative or LODs match)
    Neighbor, // copy the neighbour's edge (this slot is coarse, neighbour Full)
};

// ---- R1: per-tile atlas dirty-channel routing ----
//
// HEIGHT and SPLAT are INDEPENDENT dirty channels: a modifier height edit re-patches a slot's
// height+normal+splat (and re-evaluates bisectors), whereas an interactive paint edit sets ONLY
// SplatmapDirty and must re-upload the slot's SPLAT alone — no height/normal, no CBT re-evaluation
// (splat is sampled per-pixel fresh). The pre-fix extraction loop handled only the height channel, so
// a splat-only tile routed to nothing and the paint was INVISIBLE on atlas terrains until unrelated
// churn re-patched the slot. This pure routing function is the fix, unit-tested with a disable-and-fail.
enum class AtlasTileDirtyRoute : uint8
{
    None = 0,             // neither channel dirty this frame
    ResidentFull,         // height dirty, resident slot -> re-pack height + normal + splat
    ResidentSplatOnly,    // splat-only, resident slot -> re-upload the splat slot ONLY
    NonResidentHeight,    // height dirty, no slot -> publish + rebuild coarse (height + splat)
    NonResidentSplatOnly, // splat-only, no slot -> rebuild coarse splat only
};

// Route one tile's dirty state. Height dominates (a full re-patch re-uploads splat too), so a
// height+splat tile routes ResidentFull/NonResidentHeight; splat-only routes to the splat channel.
AtlasTileDirtyRoute ResolveAtlasTileDirtyRoute(bool heightDirty, bool splatDirty, bool resident);

// The edge-ownership decision for one side. selfIsFull / neighborIsFull are true
// for Full LOD, false for coarse. neighborResident false (no slot) keeps Self —
// the horizon seam (Risk 3) owns the in-window vs out-of-window contract instead.
// Disabling this rule (always Self) is exactly what a naive apron gets wrong, and
// what the CoarseYieldsSharedEdgeToFullNeighbor oracle catches.
AtlasEdgeSource ResolveEdgeOwnership(bool selfIsFull, bool neighborResident, bool neighborIsFull);

// The four axis-aligned neighbours of a tile (design §3.3 edge cases).
struct AtlasTileNeighbors
{
    bool LeftResident = false,  LeftFull = false;   // -X
    bool RightResident = false, RightFull = false;  // +X
    bool TopResident = false,   TopFull = false;    // -Z
    bool BottomResident = false, BottomFull = false; // +Z
};

// A tile's four interior edges (mirror the side numbering used throughout).
enum class AtlasEdgeSide : uint8
{
    NegX = 0, // left column (x = 0), indexed by z
    PosX = 1, // right column (x = tileRes-1), indexed by z
    NegZ = 2, // top row (z = 0), indexed by x
    PosZ = 3, // bottom row (z = tileRes-1), indexed by x
};

// A tile's four interior corners. A corner texel is shared by up to FOUR tiles
// (a 4-tile junction), so it needs its own reconciliation across the diagonal —
// the axis-edge ownership alone leaves a coarse tile diagonal to the only Full
// tile with a divergent corner copy (design §8 Risk 1, the 4-tile corner case).
enum class AtlasCorner : uint8
{
    TopLeft = 0,     // (x=0,      z=0)
    TopRight = 1,    // (x=res-1,  z=0)
    BottomLeft = 2,  // (x=0,      z=res-1)
    BottomRight = 3, // (x=res-1,  z=res-1)
};

// Per-corner authoritative value a tile copies when it does NOT own the shared
// corner. Unused corners keep Apply=false (same-LOD junctions agree by
// construction; a Full owner keeps its own corner). Applied AFTER edge ownership
// so all four physical copies of a shared corner end up bit-equal.
struct AtlasCornerYields
{
    struct Corner { bool Apply = false; float32 Value = 0.0f; };
    Corner TopLeft, TopRight, BottomLeft, BottomRight;
};

// Deterministic owner of a corner shared by `count` (<=4) tiles: the index of the
// Full tile earliest in the caller's order, else 0 (an all-coarse corner is equal
// by construction so the choice is immaterial). Global consistency: every sharer
// runs this over the SAME ordered set of (isFull) flags and agrees on one owner,
// so all copies converge on that owner's corner value. `isFull` has `count` entries.
uint32 ResolveCornerOwner(const bool* isFull, uint32 count);

// Pack a tile's tileRes*tileRes heightfield into `slot`'s interior in the atlas,
// resolving each shared edge per ResolveEdgeOwnership, reconciling each shared
// corner per `corners`, then filling the 1-texel apron gutter with the resolved
// edges/corners (so bilinear straddle never bleeds into a neighbour slot).
// `neighborEdges` supplies the neighbour's shared-edge line when that neighbour is
// resident-Full and this tile is coarse (else ignored). `corners` overwrites each
// interior corner this tile does not own (default: none). atlas is
// AtlasDim*AtlasDim row-major floats. Pure + directly unit-testable.
struct AtlasNeighborEdges
{
    // Each is tileRes long, or empty when not supplied. Left/Right are columns
    // (indexed by z); Top/Bottom are rows (indexed by x).
    std::vector<float32> Left, Right, Top, Bottom;
};

void PackTileHeightIntoSlot(float32* atlas, const AtlasGeometry& geo, uint32 slot,
                            const float32* tileHeights, bool tileIsFull,
                            const AtlasTileNeighbors& neighbors,
                            const AtlasNeighborEdges& neighborEdges,
                            const AtlasCornerYields& corners = {});

// Extract a slot's resolved shared-edge line (interior edge, post-ownership) so a
// neighbour that must yield can copy it. Returns tileRes values read from the atlas.
void ReadSlotInteriorEdge(const float32* atlas, const AtlasGeometry& geo, uint32 slot,
                          AtlasEdgeSide side, std::vector<float32>& outEdge);

// Which resident neighbours of a tile must have their slot edge/apron refreshed
// when that tile's LOD changes (design §8 Risk 1, "apron refresh on a NEIGHBOUR's
// LOD change"). A slot's edge AND its four shared corners depend on its neighbours'
// LOD, so a coarse->Full upgrade of tile (tx,tz) makes each resident axis neighbour
// (edge) AND each resident diagonal neighbour (shared corner) stale. Returns those
// neighbour tile coords (axis + diagonal).
std::vector<TileCoord> NeighborsNeedingApronRefresh(const AtlasIndirectionTable& table,
                                                    int32 tx, int32 tz);

// ---- Risk 3: out-of-window fallback + horizon seam ----
//
// A kAtlasNoSlot row means the tile is outside the resident window. The sampler
// must degrade to the coarse source, never garbage or a hole. The coarse source
// is a low-resolution per-terrain field (the global quadtree's coarse data,
// which the streaming manager keeps current) sampled bilinearly in terrain UV.
// The innermost out-of-window tile (fallback) sits against the outermost
// in-window tile; their shared edge must match to within the coarse tolerance,
// or tiles crossing the horizon show a ring seam.
struct AtlasCoarseField
{
    std::vector<float32> Heights; // Dim*Dim row-major, normalized (nominally [0,1]; bakes accumulate unclamped) over the whole terrain
    uint32 Dim = 0;

    bool IsValid() const { return Dim >= 2 && Heights.size() == static_cast<size_t>(Dim) * Dim; }
};

// CPU mirror of the GLSL height resolve (cbt_atlas.glsl). Given terrain UV in
// [0,1]^2: locate the tile, read its indirection row, and either sample the
// atlas slot's interior (bilinear, apron-safe) or fall back to the coarse field.
// Height is the normalized heightfield value (nominally [0,1], unclamped under modifier bakes; before HeightScale), exactly
// like CBT_SampleHeight's texel. Sampling parity, shared-edge continuity, and
// fallback are all provable against this. `atlas` is AtlasDim*AtlasDim floats.
struct AtlasHeightSampler
{
    AtlasGeometry Geometry;
    const TileAtlasSlot* Rows = nullptr; // TilesPerAxis rows
    const float32* Atlas = nullptr;      // AtlasDim*AtlasDim, or null
    const AtlasCoarseField* Coarse = nullptr; // fallback source, or null

    // Sample the normalized height at terrain UV. Never reads out of any slot's
    // interior+apron footprint, and never returns garbage: a non-resident tile
    // (or a null atlas) resolves to the coarse fallback (0 when no coarse field).
    float32 SampleHeightNormalized(float32 u, float32 v) const;

    // True when the tile containing (u,v) is resident (has a slot). Lets an
    // oracle assert the horizon boundary is exactly where residency flips.
    bool IsResidentAt(float32 u, float32 v) const;
};

// Bilinear sample of a Dim*Dim row-major grid at continuous texel coords
// (integer t = texel t's centre; endpoints exact), clamped to [0,Dim-1]. Shared
// by the atlas-interior and coarse-field reads and exposed for the oracles.
float32 SampleGridBilinearTexel(const float32* grid, uint32 width, uint32 height,
                                float32 tx, float32 ty);

// One tile's row-major heightfield, or Heights==nullptr when the tile is not
// streamed (its coarse texels take the fallback value).
struct CoarseTileSource
{
    const float32* Heights = nullptr;
    uint32 Width = 0;
    uint32 Height = 0;
};

// ---- Multi-component slot sources (splat = 4, normal = 2) ----
//
// Splat and normal ride the EXACT same slot geometry, apron, and edge/corner ownership as
// height (a slot's height/splat/normal stay in lockstep by construction — same indirection
// row, same resolve). They differ only in components-per-texel, so the height machinery is
// generalized here rather than re-derived. All arrays are componentsPerTexel-interleaved
// float32: atlas is AtlasDim*AtlasDim*components, a tile is tileRes*tileRes*components, one
// texel's components are contiguous. GPU quantization (RGBA8 splat, R16G16F normal) is applied
// at the upload boundary; the pure mirror + its oracles work in float so packing/edge/corner
// parity is provable exactly, the same as the R32F height mirror.

// componentsPerTexel-interleaved neighbour edge lines (each components*tileRes floats, or empty
// when not supplied). Mirror of AtlasNeighborEdges for multi-component sources.
struct AtlasNeighborEdgesN
{
    std::vector<float32> Left, Right, Top, Bottom;
};

// Per-corner authoritative value (componentsPerTexel floats) a tile copies when it does not own
// a shared corner. Mirror of AtlasCornerYields for multi-component sources.
struct AtlasCornerYieldsN
{
    struct Corner { bool Apply = false; std::vector<float32> Value; };
    Corner TopLeft, TopRight, BottomLeft, BottomRight;
};

// Pack a componentsPerTexel-interleaved tile into `slot`'s interior, resolving each shared edge
// per ResolveEdgeOwnership and each shared corner per `corners`, then filling the apron gutter —
// identical geometry to PackTileHeightIntoSlot, generalized over componentsPerTexel. Pure +
// directly unit-testable. (PackTileHeightIntoSlot is the componentsPerTexel==1 specialization.)
void PackTileComponentsIntoSlot(float32* atlas, const AtlasGeometry& geo, uint32 slot,
                                const float32* tileData, uint32 componentsPerTexel, bool tileIsFull,
                                const AtlasTileNeighbors& neighbors,
                                const AtlasNeighborEdgesN& neighborEdges,
                                const AtlasCornerYieldsN& corners = {});

// A slot's resolved shared-edge line for a multi-component source (componentsPerTexel*tileRes
// floats, interleaved), so a neighbour that must yield can copy it. Mirror of ReadSlotInteriorEdge.
void ReadSlotInteriorEdgeN(const float32* atlas, const AtlasGeometry& geo, uint32 slot,
                           AtlasEdgeSide side, uint32 componentsPerTexel,
                           std::vector<float32>& outEdge);

// Bilinear read of channel `comp` of a componentsPerTexel-interleaved Dim*Dim grid (endpoint-exact,
// clamped). The multi-component analogue of SampleGridBilinearTexel; the atlas-interior and
// coarse-field reads share it, and it is exposed for the oracles.
float32 SampleGridBilinearTexelN(const float32* grid, uint32 width, uint32 height,
                                 uint32 componentsPerTexel, uint32 comp, float32 tx, float32 ty);

// The out-of-window coarse field for a multi-component source (componentsPerTexel-interleaved,
// Dim*Dim). Mirror of AtlasCoarseField for splat.
struct AtlasCoarseFieldN
{
    std::vector<float32> Data; // Dim*Dim*Components, row-major, interleaved
    uint32 Dim = 0;
    uint32 Components = 0;

    bool IsValid() const
    {
        return Dim >= 2 && Components >= 1 &&
               Data.size() == static_cast<size_t>(Dim) * Dim * Components;
    }
};

// CPU mirror of the GLSL multi-component resolve: given terrain UV in [0,1]^2, locate the tile,
// read its indirection row, and either sample the atlas slot's interior (bilinear, apron-safe) or
// fall back to the coarse field, writing componentsPerTexel outputs. Mirror of AtlasHeightSampler.
struct AtlasComponentSampler
{
    AtlasGeometry Geometry;
    const TileAtlasSlot* Rows = nullptr;    // TilesPerAxis rows
    const float32* Atlas = nullptr;         // AtlasDim*AtlasDim*Components, or null
    uint32 Components = 0;
    const AtlasCoarseFieldN* Coarse = nullptr; // fallback source, or null (-> zeros)

    // Sample the resident slot (or the coarse fallback) at terrain UV, writing `Components` floats
    // into `out`. Never reads out of any slot's interior+apron footprint; a non-resident tile (or
    // null atlas) resolves to the coarse fallback (zeros when no coarse field).
    void Sample(float32 u, float32 v, float32* out) const;

    bool IsResidentAt(float32 u, float32 v) const;
};

// One tile's row-major componentsPerTexel-interleaved source, or Data==nullptr when not streamed.
struct CoarseTileSourceN
{
    const float32* Data = nullptr;
    uint32 Width = 0;
    uint32 Height = 0;
    uint32 Components = 0;
};

// Build the out-of-window coarse field for a multi-component source (mirror BuildCoarseHeightField):
// a coarseDim*coarseDim downsample of the WHOLE terrain in terrain-UV space, using the SAME UV->tile
// mapping the atlas resolve uses, so it is continuous with resident content at the frontier by
// construction. `fallback` points at componentsPerTexel floats a non-streamed texel takes. Pure so
// the frontier oracle runs this exact math.
void BuildCoarseComponentField(uint32 coarseDim, uint32 tilesX, uint32 tilesZ,
                               uint32 componentsPerTexel, const float32* fallback,
                               const std::function<CoarseTileSourceN(int32 tx, int32 tz)>& getTile,
                               std::vector<float32>& out);

// ---- GPU-side byte packers (the extraction's tool for splat/normal, R8-typed formats) ----
//
// Splat is RGBA8 and normal is R16G16F on the GPU, so the extraction packs those slots in BYTE
// space (no float<->byte round-trip). Interior copy + 1-texel apron duplication only, matching the
// SHIPPED height extraction, which packs each slot independently with empty neighbours (self-edge
// apron; the cross-slot edge/corner ownership above is oracle-machinery not yet wired into
// extraction). For the empty-neighbour case this is byte-for-byte the float PackTileComponentsIntoSlot,
// locked by the ByteSlotPackMatchesFloatPack oracle — so the geometry stays single-sourced.
void PackTileBytesIntoSlot(uint8* atlas, const AtlasGeometry& geo, uint32 slot,
                           const uint8* tileData, uint32 bytesPerTexel);

// ---- Region (sub-rect) slot upload for interactive editing ----
//
// The absolute atlas destination rect + tightly-packed bytes for uploading ONLY the sub-rect a
// brush dab touched, instead of the whole slotStride^2 block. PackTileEditRegionIntoSlot below
// fills the bytes; the render feature's Upload*SlotRegion copies them content-preserving.
struct AtlasSlotUploadRect
{
    uint32 DstTexelX = 0; // absolute atlas texel origin (x)
    uint32 DstTexelY = 0; // absolute atlas texel origin (y)
    uint32 Width = 0;     // region width in texels (0 => nothing to upload)
    uint32 Height = 0;
};

// Pack a Full tile's inclusive tile-local rect [x0,x1]x[z0,z1] (clamped to [0,TileRes-1]) of a
// bytesPerTexel-typed source (R32F height, R16G16F normal, RGBA8 splat = 4 B each) into a tightly-
// packed regionW*regionH scratch and return its absolute atlas destination rect. The rect is
// extended into the 1-texel slot apron on any side it touches the tile edge, with the apron texel
// duplicating the interior edge (bit-for-bit what PackTileBytesIntoSlot writes there) — so a
// settled sequence of region patches leaves the slot byte-identical to a whole-slot pack. Pure +
// directly unit-testable; the byte-identity is locked by RegionPatchSettlesToWholeSlotBitEqual.
AtlasSlotUploadRect PackTileEditRegionIntoSlot(std::vector<uint8>& outScratch,
                                               const uint8* tileData, uint32 bytesPerTexel,
                                               const AtlasGeometry& geo, uint32 slot,
                                               int32 x0, int32 z0, int32 x1, int32 z1);

// One tile's row-major channels-interleaved uint8 source (or Data==nullptr when not streamed).
struct CoarseTileSourceU8
{
    const uint8* Data = nullptr;
    uint32 Width = 0;
    uint32 Height = 0;
    uint32 Channels = 0;
};

// Byte analogue of BuildCoarseComponentField for the coarse SPLAT field (RGBA8): a coarseDim*coarseDim
// downsample of the whole terrain via the SAME UV->tile mapping, per-channel bilinear + round. `fallback`
// points at `channels` bytes a non-streamed texel takes. Locked to the float builder within rounding by
// the CoarseSplatByteMatchesFloat oracle.
void BuildCoarseFieldU8(uint32 coarseDim, uint32 tilesX, uint32 tilesZ, uint32 channels,
                        const uint8* fallback,
                        const std::function<CoarseTileSourceU8(int32 tx, int32 tz)>& getTile,
                        std::vector<uint8>& out);

// Build the out-of-window coarse height field (design §8 Risk 3): a coarseDim*coarseDim
// downsample of the WHOLE terrain in terrain-UV space, resized into `out` (row-major,
// normalized, nominally [0,1]). For each coarse texel it maps terrain UV -> tile -> tile-local UV and
// samples that tile's heightfield endpoint-exact — the SAME UV->tile mapping the atlas resolve
// (cbt_atlas.glsl / AtlasHeightSampler) uses. It matches resident relief EXACTLY at the coarse
// sample points and is C0-bilinear between them; the frontier discontinuity is therefore the
// resident tile's relief energy above the coarse Nyquist (~1/coarseDim UV, ~800 m/texel at 50 km),
// not zero — a bounded low-frequency skirt that removes the flat cliff, not a perfect match.
// `getTile(tx,tz)` returns that tile's heights (or a null source when not streamed). A texel with
// no streamed tile takes `baseField[texel]` when a `baseField` (the whole-terrain base relief from
// the deterministic source, coarseDim*coarseDim) is supplied, else the flat `fallback` constant.
// The base-field path keeps an out-of-window tile that never streamed height-continuous with the
// resident relief (same source) instead of collapsing a large terrain to a flat sheet at altitude.
// Pure so the frontier oracle runs this exact math.
void BuildCoarseHeightField(uint32 coarseDim, uint32 tilesX, uint32 tilesZ, float32 fallback,
                            const std::function<CoarseTileSource(int32 tx, int32 tz)>& getTile,
                            std::vector<float32>& out, const float32* baseField = nullptr);

} // namespace GameEngine::TerrainECS
