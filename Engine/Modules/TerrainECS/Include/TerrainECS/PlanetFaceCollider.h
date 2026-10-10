#pragma once

// PlanetFaceCollider — the CPU-authoritative height source for a spherical (planet)
// terrain's physics colliders (planet-collider slice, C7 spike recommendation). A
// planet is 6 cube faces; each face gets ONE Jolt heightfield patch whose local Y
// (the shape's height axis) is that face's outward normal and whose local XZ grid
// spans the two tangent world axes — a gnomonic tangent-plane patch.
//
// The patch heights come from the SAME height field the CBT renderer draws
// (CBTPlanetShading.h: base radius + multi-octave PlanetRelief + the additive sphere
// sculpt atlas), sampled at each grid cell's world DIRECTION. Sampling by direction
// keeps physics and render heights identical at every grid point (the HEIGHT-MATCH
// oracle is exact by construction) — a body rests on the rendered surface, not a
// diverged approximation.
//
// Approximation (documented): a single tangent-plane patch per 90-degree face stores
// the surface's face-normal projection at the gnomonic grid position, so the tangent
// (horizontal) placement skews toward the face edges. The radial render-vs-physics
// divergence at tangent offset c = x/R from the face centre is
// R*(sqrt(c^2 + 1/(1+c^2)) - 1): ~0.10 m at 200 m (5.7 deg), ~1.5 m at 400 m, ~49 m at
// 1000 m, ~449 m at the edge MIDPOINT (45 deg, one axis), and ~R*(sqrt(7/3)-1) ~= 1055 m
// at the cube CORNER (both axes) for R=2000. Bodies near a face centre (the demo: a box
// near the pole of a small planet) are well within budget; full-face accuracy is the
// chunked / streamed follow-up. Because the patch bulges OUTWARD everywhere (adjacent
// patches overlap at the cube edges — no fall-through gaps), a body placed on the RENDER
// surface near an edge/corner starts slightly UNDER the collider and is depenetrated
// outward. See PlanetFaceUVRectToGridRect for the edit-refresh mapping.

#include "CBTTerrain/SphereAnalyticModifiers.h" // SphereAnalyticModifierSet (S2 composed sample)
#include "CBTTerrain/SphereSculptPaging.h"      // SphereSculptSampler (paged sculpt read)
#include "Types/Types.h"

#include <array>
#include <cstdint>
#include <vector>

namespace GameEngine::TerrainECS
{

// Per-face heightfield grid dimension (samples per axis). 2^7 + 1 matches the terrain
// heightmap resolution convention (patchGridSize * 2^k + 1); Jolt rounds the sample
// count up to its block size internally, so the +1 is safe.
inline constexpr uint32 kPlanetFaceColliderDim = 129u;

// The six cube faces, indexed like CBTSphereFaceMap / CBTSphereRoots
// (0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z).
inline constexpr uint32 kPlanetFaceCount = 6u;

// The procedural shape parameters of the planet, straight off the Terrain component.
// The sculpt atlas (the editable layer) is passed separately since it is dynamic.
struct PlanetColliderParams
{
    float32 Radius = 2000.0f;
    float32 ReliefAmplitude = 60.0f;
    float32 ReliefFrequency = 6.0f;
    uint32 ReliefOctaves = 4u; // fallback; the physics system fills this from the relief component
};

// The proper right-handed body frame for cube face `face`: columns (Ta, N, Tb) with
// N the face outward normal (a signed world axis), Ta/Tb the two tangent world axes.
// The heightfield grid's local X runs along Ta, local Z along Tb, local Y (height)
// along N. Ta x N == Tb, so the frame is a valid rotation (det +1). `face` must be 0-5.
void ComputePlanetFaceFrame(uint32 face, std::array<float32, 3>& outTa,
                            std::array<float32, 3>& outN, std::array<float32, 3>& outTb);

// The unit world direction sampled at grid cell (i, j) of a `dim`-wide face patch:
// dir = normalize(N + Ta*(2i/(dim-1)-1) + Tb*(2j/(dim-1)-1)). Shared by the generator
// and the oracles so both agree on the grid parametrization.
std::array<float32, 3> PlanetFaceGridDir(uint32 face, uint32 dim, int32 i, int32 j);

// Fill face-patch height samples in grid rect [minI, maxI] x [minJ, maxJ] (inclusive,
// clamped to the grid) from the planet height field: for each cell, dir = the grid
// direction, height = (Radius + PlanetRelief(dir) + composedSculpt(dir)) * dot(dir, N) — the
// surface point's projection onto the face normal, in metres. composedSculpt is the store
// sample PLUS the analytic modifier set (SampleSphereSculptComposed, sculpt shape-accuracy S2)
// with the relief cancelled per direction — the SAME composition the GPU VertexEval displaces
// with, so a body rests on the rendered pad (CPU/GPU parity by shared closed form).
//
// `samples` must already be sized dim*dim (row-major, index = j*dim + i, matching the
// Jolt provider's sampleCount-major layout, x=i along Ta, z=j along Tb). Only the rect
// is written, so a sculpt edit re-generates just the touched region (E1 in-place path).
// `sculpt` is a VIEW over the paged SphereSculptLayer store (invalid when the planet has no
// edits — then base+relief+analytic contribute). outMin/outMax bound the samples written.
void GeneratePlanetFacePatch(uint32 face, const PlanetColliderParams& params, uint32 dim,
                             const CBTTerrain::SphereSculptSampler& sculpt,
                             const CBTTerrain::SphereAnalyticModifierSet& analytic, int32 minI,
                             int32 minJ, int32 maxI, int32 maxJ, std::vector<float32>& samples,
                             float32& outMinH, float32& outMaxH);

// Map a sculpt (face-local UV) rect — as reported by SphereSculptLayer::DirtyFaceRect in
// the CBTSphereFaceMap parametrization — to this patch's grid index rect (inclusive,
// clamped to [0, dim-1]). The two parametrizations differ (the sculpt atlas is affine in
// the cube-corner UV; this patch is gnomonic in the tangent frame), so the four UV-rect
// corners are transformed and their grid-index bounding box is returned. `face` must be
// the same face index the sculpt reported.
void PlanetFaceUVRectToGridRect(uint32 face, uint32 dim, float32 minU, float32 minV,
                                float32 maxU, float32 maxV, int32& outMinI, int32& outMinJ,
                                int32& outMaxI, int32& outMaxJ);

} // namespace GameEngine::TerrainECS
