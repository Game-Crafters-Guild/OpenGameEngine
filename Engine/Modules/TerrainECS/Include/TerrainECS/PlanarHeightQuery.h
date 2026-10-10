#pragma once

#include "Types/Types.h"

namespace GameEngine::ECS { class World; }

namespace GameEngine::TerrainECS
{

struct TerrainData;
struct TiledTerrainData;

// World-XZ height lookup against the active planar terrain, resolved once and
// then sampled many times.
//
// This is the cheap half of what a downward conform ray answers: it reads the
// CPU heightfield directly instead of traversing the scene, so it sees TERRAIN
// ONLY — no meshes, no primitives, no pick providers. A caller that needs "what
// would I land on" wants ConformRayDown; a caller that needs "how high is the
// ground" wants this, and pays neither the BVH traversal nor the terrain
// ray-march for it.
//
// Spherical terrain has no planar heightfield to read and resolves INVALID
// here; callers that draw on planets keep their own spherical path.
struct PlanarHeightQuery
{
    // Either of the two planar residencies: a single heightfield, or a tiled set
    // whose resident tile is chosen per sample.
    const TerrainData* Single = nullptr;
    const TiledTerrainData* Tiled = nullptr;
    // Footprint corner (centre minus half size) and base altitude, in world
    // metres — the terrain's transform is a translation for these purposes, the
    // same reading TerrainPicking takes.
    float32 OriginX = 0.0f;
    float32 OriginZ = 0.0f;
    float32 OriginY = 0.0f;
    float32 SizeX = 0.0f;
    float32 SizeZ = 0.0f;
    float32 HeightScale = 0.0f;

    [[nodiscard]] bool IsValid() const { return Single != nullptr || Tiled != nullptr; }

    // World-space ground altitude at (worldX, worldZ). False outside the
    // footprint, or — on a tiled terrain — when the tile containing the point is
    // not resident. A false is NOT "no ground": it is "no trustworthy answer",
    // and a caller that treats it as ground level will sink geometry into a
    // streaming hole.
    [[nodiscard]] bool SampleHeight(float32 worldX, float32 worldZ, float32& outY) const;

    // Is this world XZ inside the terrain's footprint at all?
    //
    // The distinction from SampleHeight matters: OUTSIDE the footprint there is
    // no ground and never will be, which to a surface is a wall; INSIDE it but
    // unsampleable means the tile has not streamed in, which is a temporary hole
    // a caller must wait out rather than build across.
    [[nodiscard]] bool ContainsXZ(float32 worldX, float32 worldZ) const;

    // The footprint SampleHeight actually answers over: the tiled residency's own
    // extent when there is one, the terrain component's Origin/Size otherwise.
    // The two disagree on a tiled terrain — the tiled path never reads
    // Origin/Size — so every footprint number a caller sees comes from here
    // rather than from the fields directly.
    [[nodiscard]] float32 FootprintOriginX() const;
    [[nodiscard]] float32 FootprintOriginZ() const;
    [[nodiscard]] float32 FootprintSizeX() const;
    [[nodiscard]] float32 FootprintSizeZ() const;

    // The terrain's own sample lattice: where corner (0, 0) sits and the metres
    // between corners. A caller that aligns to this reads heights as array
    // lookups rather than interpolations, and compares its geometry against the
    // same data the author edits. Zero spacing means there is no lattice to
    // align to.
    //
    // Corner (0, 0) sits ON the footprint's minimum corner in both residencies,
    // so the lattice origin IS the footprint origin.
    [[nodiscard]] float32 LatticeOriginX() const { return FootprintOriginX(); }
    [[nodiscard]] float32 LatticeOriginZ() const { return FootprintOriginZ(); }
    [[nodiscard]] float32 LatticeSpacingX() const;
    [[nodiscard]] float32 LatticeSpacingZ() const;
};

// The first enabled PLANAR terrain in the world, matching the single-active-
// terrain scope CBTTerrainECS::FindActiveTerrain and the gizmo already use.
// Returns an invalid query when the active terrain is spherical, absent, or
// carries no resident heightfield data.
[[nodiscard]] PlanarHeightQuery ResolvePlanarHeightQuery(ECS::World& world);

// Highest terrain elevation angle seen from an eye looking along a horizontal azimuth, as a
// TANGENT (rise over run). Marches the composed heightfield outward and keeps the largest
// (height - eyeY) / distance it finds, which is the terrain's skyline in that direction.
//
// This is what answers "is the sun behind a hill" independently of the frame. A screen-space
// depth probe cannot: it is gated on the sun projecting inside the viewport, so a sun one degree
// outside the frame edge is unoccludable by definition, and the halo steps the moment the sun's
// centre crosses in. The skyline is a property of the terrain and the eye, not of where the
// camera happens to point, so it holds on- and off-frame and past any shadow-cascade distance.
//
// A tangent rather than an angle because the comparison against the sun is a tangent comparison
// and the caller needs no trigonometry. Returns -1e30 when there is no terrain to see: not
// "flat", but "this term has nothing to say", which a caller must treat as fully visible.
//
// `stepMetres` should be the terrain's own lattice spacing or coarser -- a finer step buys
// nothing but samples, because the heightfield cannot resolve detail below its lattice.
[[nodiscard]] float32 TerrainHorizonTangent(const PlanarHeightQuery& query, float32 eyeX,
                                            float32 eyeY, float32 eyeZ, float32 dirX,
                                            float32 dirZ, float32 maxDistance,
                                            float32 stepMetres);

// The value TerrainHorizonTangent returns when it has no terrain to report on.
inline constexpr float32 kNoTerrainHorizon = -1.0e30f;

} // namespace GameEngine::TerrainECS
