#include "CBTTerrainECS/TerrainProvisioning.h"

#include "CBTTerrain/CBTDeepDecode.h"  // kDeepDecodeSubdiv (the deep-representation cap)
#include "CBTTerrain/CBTLayout.h"      // kDefaultBaseDepth, kMaxDecodeSubdiv
#include "CBTTerrain/CBTSphereRoots.h" // kSphereBaseDepth

#include "ECS/ECS.h"
#include "ECS/ECSTemplates.h"
#include "TerrainECS/TerrainSizingPlan.h"
#include "Terrain/TerrainTypes.h" // TerrainNeedsTiling

#include <algorithm>
#include <cmath>

namespace GameEngine::CBTTerrainECS
{
namespace
{

// Base depth of each domain's root mesh, mirrored from the CBT layout so a drift
// fails the build here rather than silently clipping the derived cap.
constexpr uint32 kPlanarBaseDepth = CBTTerrain::kDefaultBaseDepth;
constexpr uint32 kSphereBaseDepth = CBTTerrain::kSphereBaseDepth;
static_assert(kPlanarBaseDepth == 1u, "planar base mesh = 2 twin triangles, log2(2) = 1");
static_assert(kSphereBaseDepth == 5u, "cube-sphere roots sit at heap depth 5");

// Detail floor target for planets (metres). Planar refines to the provisioned heightfield
// lattice instead — the guide's "heightfield resolution is the real ceiling". Planets
// target sub-half-metre facets so walking-eye detail clears the <=0.5 m product bar (the
// shipped 1 m target capped walking detail at ~1 m — the #599 0.3-0.4 m numbers were only
// reachable at the +40 probe cap, not this default).
// The derived subdiv stays under the fp32-store cap up to ~R=50000 (lands ~37 there,
// cap 40); beyond that (Earth-scale) the clamp holds and the facet floor rises above
// 0.25 m unless the deep (sector, local) store lifts the cap to 50 (GE_CBT_DEEP_DECODE
// -> floor 0.298 m at Earth). The unclamped Earth derivation wants 51, so even the deep
// cap clamps there — by 1, not by 11.
constexpr float32 kPlanetTargetFacetMeters = 0.25f;

constexpr float32 kPi = 3.14159265358979323846f;

constexpr uint32 BaseDepthFor(Components::TerrainDomain domain)
{
    return domain == Components::TerrainDomain::Spherical ? kSphereBaseDepth : kPlanarBaseDepth;
}

// Turns a halvings ratio into a depth. Two LEB levels halve an edge, so the
// subdivision count is 2*log2(ratio); round it and clamp to [base, base + capSubdiv].
uint32 DepthFromRatio(uint32 baseDepth, float32 ratio, uint32 capSubdiv)
{
    if (!(ratio > 1.0f))
        return baseDepth;
    const float32 subdiv = 2.0f * std::log2(ratio);
    const long rounded = std::lround(subdiv);
    const long clamped = std::clamp<long>(rounded, 0, static_cast<long>(capSubdiv));
    return baseDepth + static_cast<uint32>(clamped);
}

} // namespace

uint32 SubdivCapFor(Components::TerrainDomain domain, bool deepDecode, bool narrowHeap)
{
    // Narrow-heap kernels: the heap ID is a u32, so this cap is not a precision preference
    // but the representation's own ceiling — exceed it and the decode reads a truncated ID.
    // Deep decode does not exist on that arm (it is int64 by construction), so the flag is
    // moot there.
    if (narrowHeap)
        return CBTTerrain::kHeap32DecodeSubdiv;
    const bool sphericalDeep =
        deepDecode && domain == Components::TerrainDomain::Spherical;
    return sphericalDeep ? CBTTerrain::DeepDecode::kDeepDecodeSubdiv
                         : CBTTerrain::kMaxDecodeSubdiv;
}

uint32 DeriveTerrainMaxDepth(Components::TerrainDomain domain, float32 sizeX, float32 sizeZ,
                             float32 samplesPerMeter, float32 planetRadius, bool deepDecode,
                             bool narrowHeap)
{
    const uint32 capSubdiv = SubdivCapFor(domain, deepDecode, narrowHeap);
    if (domain == Components::TerrainDomain::Spherical)
    {
        // A cube-sphere facet floor follows (piR/2) * 2^(-(depth-base)/2). Solve for
        // the depth whose floor hits the target facet: subdiv = 2*log2(rootArc/target).
        const float32 rootArc = kPi * std::max(planetRadius, 0.0f) * 0.5f;
        const float32 ratio = rootArc / kPlanetTargetFacetMeters;
        return DepthFromRatio(kSphereBaseDepth, ratio, capSubdiv);
    }

    // Planar: refine to the lattice the heightfield is provisioned at, not the authored density.
    // The root legs span the longer axis and facet = longer * 2^(-(depth-base)/2), so the
    // halvings ratio is the number of lattice intervals along that axis. The sizing plan rounds
    // the sample count up to 64 x 2^k intervals (per tile when tiled), so the spacing can be
    // finer than 1/spm: a 576 m terrain at 1 spm holds 1024 intervals, 0.5625 m apart.
    const TerrainECS::TerrainSizingPlan plan =
        TerrainECS::DeriveTerrainSizingPlan(sizeX, sizeZ, samplesPerMeter);
    if (!(plan.MetresPerTexelNear > 0.0f))
        return kPlanarBaseDepth;
    const float32 intervals = std::max(sizeX, sizeZ) / plan.MetresPerTexelNear;
    return DepthFromRatio(kPlanarBaseDepth, intervals, capSubdiv);
}

uint32 ResolveTerrainMaxDepth(const Components::Terrain& terrain, ImportedHeightSource imported,
                              bool deepDecode, bool narrowHeap)
{
    if (terrain.MaxDepthOverride != 0u)
    {
        const uint32 baseDepth = BaseDepthFor(terrain.Domain);
        const uint32 capSubdiv = SubdivCapFor(terrain.Domain, deepDecode, narrowHeap);
        return std::clamp(terrain.MaxDepthOverride, baseDepth, baseDepth + capSubdiv);
    }
    const uint32 latticeCap = DeriveTerrainMaxDepth(terrain.Domain, terrain.SizeX, terrain.SizeZ,
                                                    terrain.SamplesPerMeter, terrain.PlanetRadius,
                                                    deepDecode, narrowHeap);
    // A tiled terrain keeps the lattice cap. Its tiles read the import, but its CBT spans the tile
    // grid (tilesPerAxis x tileWorldSize), which rounds up past the footprint (#2609), while the
    // import's spacing below is measured on the footprint. Where the grid overhangs, the import's
    // cap can stop above its spacing (576 m at 2 spm is a 1024 m grid: a 577 x 577 import caps at
    // 20, 1.41 m legs on 1 m samples). The lattice cap errs deep instead, as the rule does
    // (TerrainProvisioning.h). The tiling clause can go when the grid stops overhanging.
    if (terrain.Domain != Components::TerrainDomain::Planar || imported.Width < 2u ||
        imported.Height < 2u ||
        Terrain::TerrainNeedsTiling(terrain.SizeX, terrain.SizeZ, terrain.SamplesPerMeter))
        return latticeCap;
    // The import is stretched over the terrain: along the longer axis (the root legs' axis) it
    // holds samples - 1 intervals. The import's cap is the first level whose leg is at or below
    // its spacing (2 x log2(intervals), rounded up), the shallowest that still shows every source
    // sample; the lattice cap stays the nearest level to the lattice.
    const uint32 importedIntervals =
        (terrain.SizeX >= terrain.SizeZ ? imported.Width : imported.Height) - 1u;
    const float32 subdiv =
        std::ceil(2.0f * std::log2(static_cast<float32>(importedIntervals)));
    const uint32 capSubdiv = SubdivCapFor(terrain.Domain, deepDecode, narrowHeap);
    const float32 clamped = std::clamp(subdiv, 0.0f, static_cast<float32>(capSubdiv));
    const uint32 importedCap = kPlanarBaseDepth + static_cast<uint32>(clamped);
    return std::min(latticeCap, importedCap);
}

// The one liveness predicate: the first enabled terrain whose single OR tiled handle
// is set, in chunk order. Both FindActiveTerrain and ResolveActivePlanetRelief resolve
// off this so the Terrain and its companion relief always come from the same entity.
// Exported so the CBT update system can key the service-global sculpt layer's reset on the
// planet's entity identity (a delete/recreate must not inherit the old planet's frozen geometry).
ECS::EntityHandle FindActiveTerrainEntity(ECS::World& world)
{
    ECS::EntityHandle found{};
    world.Query<ECS::Read<Components::Terrain>>().Each(
        [&](ECS::EntityHandle entity, const Components::Terrain& terrain)
        {
            if (found.IsValid())
                return;
            const bool singleLive =
                terrain.TerrainDataHandle != 0 || terrain.TerrainDataGeneration != 0;
            const bool tiledLive =
                terrain.TiledTerrainHandle != 0 || terrain.TiledTerrainGeneration != 0;
            if (!(singleLive || tiledLive))
                return;
            found = entity;
        });
    return found;
}

bool FindActiveTerrain(ECS::World& world, Components::Terrain& out)
{
    const ECS::EntityHandle entity = FindActiveTerrainEntity(world);
    if (!entity.IsValid())
        return false;
    out = *world.GetComponent<Components::Terrain>(entity);
    return true;
}

Components::TerrainPlanetRelief ResolveActivePlanetRelief(ECS::World& world)
{
    const ECS::EntityHandle entity = FindActiveTerrainEntity(world);
    if (entity.IsValid())
        if (const auto* relief = world.GetComponent<Components::TerrainPlanetRelief>(entity))
            return *relief;
    return {};
}

Components::Terrain MakeTerrainPreset(TerrainPreset preset)
{
    Components::Terrain t{};
    switch (preset)
    {
    case TerrainPreset::SmallPlanar:
        t.Domain = Components::TerrainDomain::Planar;
        t.SizeX = 512.0f;
        t.SizeZ = 512.0f;
        t.HeightScale = 128.0f;
        t.SamplesPerMeter = 2.0f;
        t.MaterialTiling = 10.0f;
        break;
    case TerrainPreset::LargePlanar:
        t.Domain = Components::TerrainDomain::Planar;
        t.SizeX = 4096.0f;
        t.SizeZ = 4096.0f;
        t.HeightScale = 512.0f;
        t.SamplesPerMeter = 1.0f; // 1 spm for 4 km+: halves memory, holds the size ceiling
        t.MaterialTiling = 80.0f; // ~10 per 512 m of extent to hold texel density at eye level
        break;
    case TerrainPreset::Planet5km:
        t.Domain = Components::TerrainDomain::Spherical;
        t.SizeX = 512.0f;
        t.SizeZ = 512.0f;
        t.HeightScale = 128.0f;
        t.SamplesPerMeter = 2.0f;
        t.MaterialTiling = 10.0f;
        t.PlanetRadius = 5000.0f;
        break;
    case TerrainPreset::Planet50km:
        t.Domain = Components::TerrainDomain::Spherical;
        t.SizeX = 512.0f;
        t.SizeZ = 512.0f;
        t.HeightScale = 128.0f;
        t.SamplesPerMeter = 2.0f;
        t.MaterialTiling = 10.0f;
        t.PlanetRadius = 50000.0f;
        break;
    }
    return t;
}

Components::TerrainPlanetRelief MakePlanetReliefPreset(TerrainPreset preset)
{
    Components::TerrainPlanetRelief relief{};
    switch (preset)
    {
    case TerrainPreset::Planet5km:
        relief.Amplitude = 125.0f; // ~= R/40 for a readable orbit silhouette
        relief.Frequency = 6.0f;
        relief.Octaves = 4u; // readable-detail default (see TerrainPlanetRelief.h); 5+ lattices
        break;
    case TerrainPreset::Planet50km:
        relief.Amplitude = 1250.0f; // ~= R/40
        relief.Frequency = 6.0f;
        relief.Octaves = 4u;
        break;
    case TerrainPreset::SmallPlanar:
    case TerrainPreset::LargePlanar:
        break; // planar terrains carry no relief — the default is inert
    }
    return relief;
}

} // namespace GameEngine::CBTTerrainECS
