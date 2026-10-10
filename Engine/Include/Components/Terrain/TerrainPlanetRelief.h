#pragma once

#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// Base procedural relief of a cube-sphere planet: the closed-form multi-octave noise
// displaced along the surface normal that gives an un-sculpted globe its silhouette.
// Split out of the Terrain component (relief-unification slice) because these are the
// planet's base-noise dials, not a heightmap terrain control — a planar terrain never
// carries this, and the values are meaningless there. Auto-created on spherical
// provisioning and by the domain switch, so a spherical terrain always has one; when it
// is absent the renderer / collider fall back to a default-constructed copy (the old
// Terrain relief defaults), so a planet without it renders exactly as before.
//
// The single point that reads this into the CBT DomainConfig is CBTUpdateSystem; the
// planet-collider provisioner and the far-clip framing read it too. Reading the same values
// is not what keeps render and physics agreeing — the collider and the baked sphere modifiers
// are DERIVED data, so an edit here only reaches them by moving a baseline: the terrain-state
// hash (TerrainModifierSystem, which re-derives the sphere bake) and the planet-shape hash
// (TerrainPhysicsSystem, which re-cooks all six face patches). A new field added to this
// struct must be folded into both or it will render without colliding.
// The math itself is CBT_PlanetRelief in cbt_domain.glsl / CBTTerrain::PlanetRelief.
//
// @ge-no-add  Only meaningful alongside a spherical Terrain on the same entity; auto-
// provisioned, so it is reflected (MCP snapshot + its custom inspector) but kept out of
// the generic Add-Component menu, exactly like TerrainGrass.
struct TerrainPlanetRelief
{
    // @ge-tooltip Base surface displacement along the normal (metres). ~= R/40 reads well.
    float32 Amplitude = 60.0f;   // procedural relief along the normal (metres)
    // @ge-tooltip Angular frequency of the base noise (wavelength ~= 2*pi*R / frequency).
    float32 Frequency = 6.0f;    // angular frequency (wavelength ~= 2piR/frequency)
    // @ge-tooltip fBM octaves stacked onto the base relief shape for finer detail (1-8).
    // 4 is the readable-detail default: octaves 3 reads too smooth at surface scale (the analytic
    // per-pixel normal + slope/altitude splat pick up the extra octave without any more triangles),
    // while 5+ makes the axis-separable sin-product noise reinforce into a visible diagonal lattice
    // (cbt_domain.glsl lattice note) — 4 is the ceiling before the artefact for this noise function.
    uint32 Octaves = 4u;         // fBM octaves stacked onto the base relief shape
};

static_assert(std::is_trivially_copyable_v<TerrainPlanetRelief>,
              "TerrainPlanetRelief must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<TerrainPlanetRelief>,
              "TerrainPlanetRelief must be standard layout for ECS storage");

} // namespace GameEngine::Components
