#pragma once

#include "Components/AssetRef.h"
#include "Types/Types.h"

#include <limits>
#include <type_traits>

namespace GameEngine::Components
{

// Where a terrain's base heights come from before the modifier stack applies
// (edit-pipeline design §3.1). Deliberately the thin end of the CBT plan's
// ITerrainSource seam: the CBT arc virtualizes behind this without touching
// authoring or bake code.
enum class TerrainBaseSource : uint8
{
    ProceduralNoise = 0, // default fBM noise fill
    HeightmapAsset = 1,  // sampled from TerrainAssetGuid (16-bit PNG imports as a
                         // Texture asset; .r16/.r32 raw imports as TerrainHeightmap)
    Flat = 2,            // constant zero height
};

// Rendering domain. Planar is the heightmap terrain; Spherical renders a
// cube-sphere planet (6 faces x 4 pie-slice roots) of PlanetRadius, refining
// toward the camera at both globe and surface scale. Live-switchable — the CBT
// renderer re-seeds its roots when this changes. Values mirror
// CBTTerrain::kDomainPlanar / kDomainSpherical (CBTLayout.h) — keep in lockstep.
enum class TerrainDomain : uint32
{
    Planar = 0u,
    Spherical = 1u,
};

// CBT surface debug visualization (ledger cleanup). Off leaves shading untouched — planar
// and planet render exactly as normal. Facets tints each CBT bisector triangle by its slot
// so the live LEB bisection structure (triangle density, splits) is directly visible on the
// surface. A runtime/editor toggle: not serialized (like MaxDepthOverride), so no scene
// churn. Values mirror CBT_DEBUG_* in cbt_surface.glsl — keep in lockstep.
enum class TerrainDebugView : uint32
{
    Off = 0u,
    Facets = 1u,     // per-bisector facet tint — the live CBT triangulation
    AtlasSlots = 2u, // per-pixel atlas resolve: tint by resolved slot index (resident) / grey (coarse
                     // fallback) — visualizes the fragment splat/normal resolve path (quality-sweep slice 1)
};

// "No sea level" for Terrain::SeaLevel. The lowest finite float, as SplineExtrude::SeaLevelFloor
// chose it: every finite height is above it, so the renderer's "corner below the water" test is
// simply never true and the unset state needs no flag and no equality test.
inline constexpr float32 kNoTerrainSeaLevel = std::numeric_limits<float32>::lowest();

// Default Terrain::TargetPixelError: 11 px at 1080 rows, the reference implementation's 60 px^2
// split area expressed as an edge length.
inline constexpr float32 kDefaultTerrainTargetPixelError = 11.0f;

// ECS component for a terrain instance. Placed on an entity alongside Position
// to spawn a terrain in the world. This is THE single user-facing terrain
// control component: authoring data (source, size, resolution), the planet
// domain, and the renderer's refinement dials all live here. All heavyweight
// data (heightfield, quadtree, GPU textures, the CBT bisector pool) is owned by
// TerrainService / the CBT render feature; this component stores only
// lightweight configuration and runtime handles into those services.
struct Terrain
{
    // ---- Base source ----
    // TerrainAssetGuid is the heightmap reference when BaseSource is
    // HeightmapAsset (serialized + dependency-enumerated via TerrainSchema).
    Components::AssetRef<> TerrainAssetGuid;
    uint32 TerrainAssetType = 0;       // AssetType of the reference; 0 = none
    TerrainBaseSource BaseSource = TerrainBaseSource::ProceduralNoise;

    // ---- Dimensions (world units) ----
    // Note: SizeX and SizeZ should be equal (square terrain) for correct tiling.
    float32 SizeX = 1024.0f;           // world-space width (X axis)
    float32 SizeZ = 1024.0f;           // world-space depth (Z axis)
    float32 HeightScale = 256.0f;      // world-space vertical scale

    // ---- Resolution ----
    float32 SamplesPerMeter = 2.0f;    // heightmap samples per world unit (higher = finer)

    // ---- Streaming ----
    float32 StreamingRadius = 0.0f;    // tile streaming radius (0 = auto from LOD ranges)

    // ---- Planet / domain ----
    // Domain switches the terrain between a heightmap plane and a cube-sphere
    // planet. PlanetRadius only applies in Spherical mode. The base procedural
    // relief lives in the companion TerrainPlanetRelief component (auto-created on
    // spherical provisioning), not here — it is planet base-noise, not a heightmap
    // control. The sphere is centered at the WORLD origin: cbt_kernels.comp builds every
    // sphere vertex as dir * (radius + height), so the entity's transform does not move it.
    // Grouped under a "Planet" section in the inspector.
    TerrainDomain Domain = TerrainDomain::Planar;
    float32 PlanetRadius = 2000.0f;           // sphere radius (metres), Spherical only

    // The planet's saved sphere sculpt payload (.tsculpt sidecar next to the scene, Spherical
    // only). Null until the first explicit Save mints the file (SphereSculptSaver swaps in the
    // file-backed GUID, the zones' mint-and-swap contract); CBTUpdateSystem restores it on
    // load via TerrainService::EnsureSphereSculptLoaded.
    Components::AssetRef<> SphereSculptGuid;

    // ---- Water ----
    // Height (world Y) of the calm water surface this planar terrain sits under, when the water
    // is not the Ocean module's surface (a mesh plane with a water material, say). The CBT
    // renderer keeps the seabed hidden below it coarse instead of refining it to
    // TargetPixelError. An enabled OceanSurface in the scene supplies its own sea level and takes
    // precedence. kNoTerrainSeaLevel (the default) = no water: every bisector refines normally.
    float32 SeaLevel = kNoTerrainSeaLevel;

    // ---- Advanced refinement ----
    // TargetPixelError is the CBT screen-space split target, in pixels at 1080 rows: the
    // renderer splits a bisector when its projected split edge exceeds this, scaled by the
    // view's render height / 1080 (CBTTerrain::SplitThresholdPixels), so the triangle count
    // does not depend on the display resolution. Lowering it costs triangles (about 1/T^2)
    // without changing the look on smooth ground.
    float32 TargetPixelError = kDefaultTerrainTargetPixelError;
    // Refinement depth cap (LEB heap depth). 0 = auto-derive from domain + size /
    // radius, clamped to the fp32-exact ceiling (baseDepth + 23). A non-zero value
    // is an internal/debug override (not shown in the default inspector, not
    // serialized) — the correct value is computed, so users never tune a heap depth.
    uint32 MaxDepthOverride = 0u;

    // CBT surface debug visualization (Advanced group; not serialized). Off = normal shading.
    TerrainDebugView DebugView = TerrainDebugView::Off;

    // ---- Runtime handles (not serialized) ----
    uint32 TerrainDataHandle = 0;      // GPU TerrainData slot (quadtree for render node)
    uint32 TerrainDataGeneration = 0;
    uint32 TiledTerrainHandle = 0;     // TiledTerrainData (tile management, 0 = not tiled)
    uint32 TiledTerrainGeneration = 0;
    // [DoNotSerialize] The scene asset this terrain was loaded from, set by its scene schema
    // at load. With the entity's SceneEntityTag it names the terrain's one baked-terrain
    // cache artifact (TerrainBakeCache.h). Null for a terrain no saved scene loaded, which
    // bakes uncached.
    GUID BakeOriginScene{};

    // ---- Materials ----
    float32 MaterialTiling = 10.0f;     // world-space UV tiling for material textures

    // The terrain material library (.terrainmatlib) this terrain shades from: an ordered list of
    // authored materials, each carrying a stable slot ID. Referenced rather than inlined because
    // undo snapshots this component as raw bytes, so a variable-length list cannot live on it —
    // and because two terrains sharing one library is the common authoring case. Splat channel i
    // resolves to the entry holding slot ID i.
    //
    // Null on a terrain authored before libraries existed, which then shades from the per-layer
    // fields below instead.
    Components::AssetRef<> MaterialLibraryGuid;

    // Which library material each channel role shades with, as the stable SLOT ID the library
    // entry carries — never its row position, so reordering the library cannot repaint a terrain.
    // Index is the role (0 grass, 1 rock, 2 dirt, 3 snow), which IS the splat channel.
    // The identity default binds role r to slot r, which is what makes a
    // freshly-migrated terrain shade exactly as it did before it had a library.
    //
    // Distinct from TerrainGPUParams::LayerRole, which holds the role's ABSOLUTE index into
    // the frame's material table: this is the authored binding, that is the per-frame resolution
    // of it.
    uint8 LayerRoleSlot[4] = {0u, 1u, 2u, 3u};

    // Per-layer albedo textures + tiling, in the splat/blend layer order the surface
    // shades (0 grass, 1 rock, 2 dirt, 3 snow). A null ref leaves the layer on the
    // data-driven tint+variation fallback; a bound texture flips the surface to the
    // triplanar texture path. Read only while MaterialLibraryGuid is null — a bound library is
    // the sole authority for what a terrain's channels shade with, so there is no path by which
    // a texture can be bound for one consumer and missing for another.
    // LayerTiling multiplies the global MaterialTiling per layer (1 = global).
    Components::TextureRef LayerAlbedoTexture[4];
    float32 LayerTiling[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    // Hex-tiling opt-in bitmask (bit i = layer i): breaks visible repetition by blending
    // three hash-rotated copies of the texture on a hexagonal lattice (Mikkelsen,
    // "Practical Real-Time Hex-Tiling", JCGT 2022) at ~3x the layer's sample cost.
    uint32 LayerHexTiling = 0u;

    // ---- Rendering ----
    uint32 RenderLayerMask = 1u;
    bool CastShadows = true;
    bool ReceiveShadows = true;
};

static_assert(std::is_trivially_copyable_v<Terrain>,
              "Terrain must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<Terrain>,
              "Terrain must be standard layout for ECS storage");

} // namespace GameEngine::Components
