#pragma once

// TerrainProvisioning — the derivation + preset layer that lets a user create ONE
// Terrain component and get a rendering-ready configuration. It turns the
// user-facing Terrain fields (domain, size, samples-per-meter, planet radius) into
// the internal CBT refinement cap the renderer needs, and builds coherent
// field bundles for the editor's creation presets. Pure functions (no ECS world,
// no services) so they are directly unit-testable.

#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "Types/Types.h"

namespace GameEngine::ECS
{
class World;
struct EntityHandle;
} // namespace GameEngine::ECS

namespace GameEngine::CBTTerrainECS
{

// The active terrain's entity — the first enabled, live terrain in chunk order (liveness =
// single OR tiled handle set), or an invalid handle when none is live. THE liveness resolver
// FindActiveTerrain / ResolveActivePlanetRelief share. The CBT update system keys the service-
// global sculpt layer's reset on this so a deleted/recreated planet re-derives fresh geometry.
ECS::EntityHandle FindActiveTerrainEntity(ECS::World& world);

// Copies the single active terrain — the first enabled, live terrain in chunk
// order (liveness = single OR tiled handle set) — into `out` and returns true.
// Returns false (leaving `out` untouched) when none is live. This is THE terrain
// resolver: the renderer's tuning read and the editor's brush domain check both
// use it, so they never disagree about which terrain is active. Documented
// single-active-terrain scope (multi-terrain is not supported).
bool FindActiveTerrain(ECS::World& world, Components::Terrain& out);

// The base procedural relief of the single active terrain: its TerrainPlanetRelief
// component if present, otherwise a default-constructed copy (the old Terrain relief
// defaults, so a planet without the component renders as before). Resolves the SAME
// active-terrain entity FindActiveTerrain does, so the renderer, collider, and far-clip
// framing all read one relief source. Meaningful only for a spherical terrain.
Components::TerrainPlanetRelief ResolveActivePlanetRelief(ECS::World& world);

// The refinement subdivision ceiling (numSubdiv = depth - baseDepth) in force for a terrain.
// It is a REPRESENTATION limit, so it follows the gVertex storage mode (decode-precision arc
// S2b):
//   * fp32 WORLD corners (the shipped default): kMaxDecodeSubdiv (40). The store quantizes
//     ~0.25 m/corner at Earth magnitude regardless of depth, so past numSubdiv ~46 the noise
//     exceeds the facet (measured 69% at 46, 275% at 50) — 40 stays ~2x short of that
//     degeneracy. Mirrors the shader guard CBT_MAX_NUM_SUBDIV (locked by
//     CBTLayout.GlslMaxDecodeSubdivMatchesCpp). Deeper bisectors are skipped as corrupt.
//   * deep (sector, local) df64 store (GE_CBT_DEEP_DECODE, spherical only):
//     kDeepDecodeSubdiv (50). Decode error <= 2e-5 m at Earth radius, facet floor 0.298 m
//     (under the 0.5 m walking bar), 2 subdivisions below the int64 walk-exactness ceiling
//     (~52). Mirrors CBT_DEEP_NUM_SUBDIV (locked by GlslDeepDecodeMirrorsCpp).
//   * narrow-heap (u32) kernels, which a device without shaderInt64 runs:
//     kHeap32DecodeSubdiv (25) — the heap ID's own range, not a precision preference.
// The flag->cap coupling is LOAD-BEARING: lifting the cap without the deep store ships the
// measured degeneracy above, so the caller must pass the SAME deepDecode bit that drives
// CBTClassifyDesc::DeepDecode (CBTUpdateSystem reads it once per frame for both).
// Locked by TerrainMaxDepth.DecodeCapMatchesWhatTheRepresentationCanCarry.
uint32 SubdivCapFor(Components::TerrainDomain domain, bool deepDecode, bool narrowHeap);

// Auto-derives the CBT refinement depth cap (LEB heap depth) from the domain and
// size / radius, clamped to the decode ceiling the ACTIVE gVertex representation can
// carry (beyond it the decode loses precision and a deeper bisector is skipped as
// corrupt). `deepDecode` MUST be the same bit that drives CBTClassifyDesc::DeepDecode —
// the ceiling is baseDepth + kMaxDecodeSubdiv (40) under the shipped fp32 world store,
// and baseDepth + DeepDecode::kDeepDecodeSubdiv (50) only when the spherical (sector,
// local) df64 store is live (GE_CBT_DEEP_DECODE; planar ignores the flag). Lifting the
// cap without the deep store ships the measured Earth-magnitude storage degeneracy
// (decode-precision arc S2b, design §4) — the coupling is locked by
// TerrainMaxDepth.DecodeCapMatchesWhatTheRepresentationCanCarry. The cap is chosen so
// the finest facet lands near the useful detail floor: for planar, the lattice spacing the
// heightfield is provisioned at (TerrainECS::DeriveTerrainSizingPlan's MetresPerTexelNear,
// which can be finer than 1/samplesPerMeter because the sample count rounds up to 64 x 2^k
// intervals), counted along the longer of sizeX and sizeZ. On a single heightfield the
// finest leg equals that spacing; on a tiled terrain the CBT grid does not align with the
// tile lattice, so the cap is the nearest level and the leg lands within 2^+-0.25 of the
// spacing (1200 x 1100 m at 1 spm: 1.17 m legs on a 1 m lattice). For planets the floor is
// ~0.25 m (walking-eye detail under the <=0.5 m product bar). Planets ignore sizeX, sizeZ and
// samplesPerMeter.
// Measured: a too-LOW cap costs fps (the classifier thrashes against the clamp), so the
// derivation errs deep and pins at the ceiling for very large planets (deep OFF: Earth
// clamps at 45 -> 9.5 m facet floor; deep ON: Earth clamps at 55 -> 0.298 m floor).
// `narrowHeap` names the kernels' heap width (CBTRenderFeature::UsesNarrowHeap): a u32
// heap caps the depth at its own range, whatever the decode store could carry.
uint32 DeriveTerrainMaxDepth(Components::TerrainDomain domain, float32 sizeX, float32 sizeZ,
                             float32 samplesPerMeter, float32 planetRadius, bool deepDecode,
                             bool narrowHeap);

// The sample grid of the imported heightmap that drives a terrain's heights (BaseSource ==
// HeightmapAsset, decoded), or {0, 0} when the terrain's source is its own lattice (procedural
// noise, a flat base, modifiers, sculpting). ResolveTerrainMaxDepth ignores the import on a tiled
// terrain (see there). The import is stretched over the terrain, so its spacing along an axis is
// that axis's size / (samples - 1).
struct ImportedHeightSource
{
    uint32 Width = 0;
    uint32 Height = 0;
};

// The effective MaxDepth the renderer should use for a terrain: the internal
// MaxDepthOverride when set (non-zero, clamped to the representation ceiling above),
// otherwise the auto-derived cap. A planar terrain driven by an imported heightmap caps at
// the coarser of the provisioned lattice and the import's own spacing (along the longer
// axis, the first level whose leg is at or below that spacing): refining past the source
// samples adds triangles and no detail. A tiled terrain keeps its lattice cap even though its
// tiles read the import: its CBT spans the tile grid, which rounds up past the footprint the
// import is measured on (#2609), so the import's cap can stop above the import's spacing there
// (576 m at 2 samples per metre is a 1024 m grid: a 577 x 577 import would cap at depth 20, whose
// 1.41 m legs skip its 1 m samples). This is the single source of truth CBTUpdateSystem
// consumes; it reads `deepDecode` once per frame and feeds the SAME value here and into
// CBTClassifyDesc::DeepDecode, so cap and storage mode cannot desync.
uint32 ResolveTerrainMaxDepth(const Components::Terrain& terrain, ImportedHeightSource imported,
                              bool deepDecode, bool narrowHeap);

// Editor creation presets — coherent starting bundles so users pick a known-good
// scale instead of eleven fields. Planets set Domain = Spherical + radius and leave
// MaxDepth on auto; the base relief is a companion component (MakePlanetReliefPreset).
enum class TerrainPreset : uint8
{
    SmallPlanar = 0, // "Terrain (512 m)"
    LargePlanar = 1, // "Terrain - Large (4 km)"
    Planet5km = 2,   // "Planet (5 km)"
    Planet50km = 3,  // "Planet (50 km)"
};

// Builds the Terrain field bundle for a preset. Runtime handles stay zero — the
// caller provisions the TerrainService data and (for planar terrains) physics.
Components::Terrain MakeTerrainPreset(TerrainPreset preset);

// Builds the base relief the planet presets ship with (amplitude ~= R/40, 3 octaves).
// The planar presets return the component default (inert on a heightmap terrain). The
// caller attaches this to the entity only for a spherical terrain.
Components::TerrainPlanetRelief MakePlanetReliefPreset(TerrainPreset preset);

} // namespace GameEngine::CBTTerrainECS
