#pragma once

#include "Terrain/TerrainMaterialRecord.h"
#include "Types/Types.h"
#include "Mathematics/Vector3.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Terrain
{

// ---- Constants ----

static constexpr uint32 kDefaultGridSize = 64;        // vertices per patch edge (power of 2)
static constexpr uint32 kMaxLODLevels = 16;
static constexpr float32 kDefaultLODRangeScale = 2.0f; // multiplier per LOD level
static constexpr uint32 kMinPatchSize = 8;
static constexpr uint32 kMaxPatchSize = 128;

// ---- Enums ----

enum class HeightfieldFormat : uint32
{
    Float32 = 0,
    UNorm16 = 1,
};

// ---- Structs ----

static constexpr uint32 kMaxHeightmapDimension = 8193;
static constexpr float32 kDefaultSamplesPerMeter = 1.0f;

// The per-tile sample cap. A terrain whose derived resolution exceeds this is split into a tile
// grid; the tile's world size follows from this cap and the sample density. Every consumer of
// "does this terrain tile" must resolve it through TerrainNeedsTiling below.
static constexpr uint32 kMaxTileResolution = 1025;

// Configuration for creating a terrain instance.
struct TerrainConfig
{
    uint32 HeightmapWidth = 1025;
    uint32 HeightmapHeight = 1025;
    float32 WorldSizeX = 1024.0f;
    float32 WorldSizeZ = 1024.0f;
    float32 HeightScale = 256.0f;
    uint32 LODLevels = 6;
    float32 LODRangeScale = kDefaultLODRangeScale;
    uint32 PatchGridSize = kDefaultGridSize;

    // Derive a config from user-facing parameters. Computes HeightmapWidth/Height
    // and LODLevels from world size, samples-per-meter, and patch grid size.
    static TerrainConfig FromSamplesPerMeter(float32 worldSizeX, float32 worldSizeZ,
                                              float32 heightScale, float32 samplesPerMeter,
                                              uint32 patchGridSize = kDefaultGridSize,
                                              float32 lodRangeScale = kDefaultLODRangeScale)
    {
        samplesPerMeter = std::max(samplesPerMeter, 0.01f);
        patchGridSize = std::clamp(patchGridSize, kMinPatchSize, kMaxPatchSize);

        // Counted in float and saturated before the conversion: a side past the float-to-uint32
        // range (or a NaN) takes the largest resolution rather than an undefined one.
        const float32 maxSize = std::max(worldSizeX, worldSizeZ);
        const float32 samples = std::floor(maxSize * samplesPerMeter) + 1.0f;
        uint32 desiredRes = samples < static_cast<float32>(kMaxHeightmapDimension)
                                ? static_cast<uint32>(std::max(samples, 0.0f))
                                : kMaxHeightmapDimension;
        desiredRes = std::clamp(desiredRes, 3u, kMaxHeightmapDimension);

        // Find smallest LODLevels such that gridSize * 2^(N-1) + 1 >= desiredRes.
        uint32 lodLevels = 1;
        while (patchGridSize * (1u << (lodLevels - 1)) + 1 < desiredRes && lodLevels < kMaxLODLevels)
            ++lodLevels;

        uint32 actualRes = patchGridSize * (1u << (lodLevels - 1)) + 1;
        actualRes = std::min(actualRes, kMaxHeightmapDimension);

        TerrainConfig config{};
        config.HeightmapWidth = actualRes;
        config.HeightmapHeight = actualRes;
        config.WorldSizeX = worldSizeX;
        config.WorldSizeZ = worldSizeZ;
        config.HeightScale = heightScale;
        config.LODLevels = lodLevels;
        config.LODRangeScale = lodRangeScale;
        config.PatchGridSize = patchGridSize;
        return config;
    }
};

// Does this terrain split into a tile grid? Editor provisioning, the extraction system and the
// tile-geometry derivation must give the SAME answer: provisioning withholds the eager single
// heightfield AND its heightfield collider for a terrain it expects to tile, while extraction
// owns tiled provisioning — so a terrain the two classify differently renders with no collision.
// Resolved through the renderer's own FromSamplesPerMeter rounding, never from raw
// size * density, because the derived resolution rounds the product down before adding the
// shared edge sample.
inline bool TerrainNeedsTiling(float32 worldSizeX, float32 worldSizeZ, float32 samplesPerMeter)
{
    const TerrainConfig derived = TerrainConfig::FromSamplesPerMeter(
        worldSizeX, worldSizeZ, /*heightScale*/ 0.0f, samplesPerMeter);
    // FromSamplesPerMeter derives one square resolution from the longer axis, so either
    // dimension answers for both.
    return derived.HeightmapWidth > kMaxTileResolution;
}

// Output of CDLOD selection: one patch to be rendered.
// 48 bytes (12 floats) per patch, matching kFloatsPerPatch in shaders.
struct CDLODPatch
{
    float32 WorldX;             // [0] patch world-space origin X
    float32 WorldZ;             // [1] patch world-space origin Z
    float32 Scale;              // [2] world-space size of this patch
    uint32 TerrainIndex = 0;    // [3] index into TerrainGPUParams SSBO array
    uint32 LODLevel;            // [4] 0 = finest
    float32 MorphStartDist;     // [5] distance at which per-vertex morph begins
    float32 MorphEndDist;       // [6] distance at which per-vertex morph reaches 1.0
    // Sub-quadrant flags packed into a uint32: bits 0-3 = TL, TR, BL, BR.
    // When a coarse node has finer children, only unoccupied quadrants are drawn.
    // 0xF = draw all 4 quadrants (full patch).
    uint32 SubQuadFlags = 0xF;  // [7]
    uint32 HeightmapBindless = 0; // [8] bindless descriptor index for this terrain's heightmap
    uint32 NormalmapBindless = 0; // [9] bindless descriptor index for this terrain's normal map
    uint32 _Reserved2 = 0;     // [10]
    uint32 _Reserved3 = 0;     // [11]
};

static_assert(sizeof(CDLODPatch) == 48, "CDLODPatch must be 48 bytes for GPU SSBO alignment");

// The splat's channel count: how many material layers a terrain can CARRY. How many of them
// actually blend in one fragment is a separate, smaller budget the surface owns
// (CBT_MAX_BLEND_MATERIALS in cbt_surface.glsl).
static constexpr uint32 kMaxTerrainMaterialLayers = 4;
// One role per splat channel: the bake writes channel i, and the surface resolves channel i
// through role i into the material table. The two constants name different concepts
// (storage width vs. semantic roles) and must stay equal.
static_assert(kMaxTerrainMaterialLayers == kTerrainLayerRoleCount,
              "one role per splat channel");

// TerrainGPUParams::Flags bits (shared with the terrain/grass shaders that read the
// per-terrain params SSBO). bit0 = receive shadows; bit1 marks an atlas-backed terrain
// (HeightmapBindless == 0, height/normal/splat resolve through the resident-window atlas),
// which the grass placement compute uses to route its height/normal/mask sampling through
// the atlas resolve instead of the (absent) unified heightmap.
static constexpr uint32 kTerrainFlagReceiveShadows = 1u;
static constexpr uint32 kTerrainFlagAtlasBacked = 2u;
// Unified (non-atlas) source textures exist for this terrain. Backend-agnostic truth about the
// HANDLES: the compat profile has no bindless indices, so *Bindless == 0 there says nothing about
// whether a map exists — these bits do. Mirrored in terrain_grass_place.comp.
static constexpr uint32 kTerrainFlagHasHeightmap = 4u;
static constexpr uint32 kTerrainFlagHasNormalmap = 8u;
static constexpr uint32 kTerrainFlagHasSplatmap = 16u;
static constexpr uint32 kTerrainFlagHasGrassControls = 32u;

// TerrainGPUParams::GrassEnabled bits (shared with the grass shaders that read the params SSBO).
// bit3 and bit4 are CPU-side only: the upload-time reduction composes the per-view grass draw mode
// from them (TerrainRenderFeature::GrassPlacementSummary), and no shader reads either.
// Set only when the row actually places a blade — see TerrainGrassRowPlaces below, which extraction
// applies and the budget summary reads. "Enabled but placing nothing" is not a state that reaches
// the GPU, because the placement kernels admit a cell on this bit alone.
static constexpr uint32 kTerrainGrassBitEnabled = 1u;
static constexpr uint32 kTerrainGrassBitSplatRootColor = 2u;
static constexpr uint32 kTerrainGrassBitTextureCard = 4u;
static constexpr uint32 kTerrainGrassBitBlendMode = 8u;
// This row's blade texture carries alpha that is not uniformly opaque, so an alpha path (cutout,
// screen-door or alpha-to-coverage) has something to resolve. Clear means the row's blades are
// solid by construction: geometric ribbon grass, which binds no texture at all, and card grass
// whose texture alpha probes uniformly opaque. Set from the PROBED texture, not from the authored
// render mode — the mode says how soft alpha would resolve, this says whether any exists.
static constexpr uint32 kTerrainGrassBitAlphaNeeded = 16u;

// Per-terrain GPU constants stored in an SSBO array (one entry per terrain).
// 272 bytes (68 words). Lighting is in the View UBO, not here; the per-material shading inputs
// are in the material table (TerrainMaterialRecord), which this buffer only points into.
struct TerrainGPUParams
{
    float32 WorldOriginX;
    float32 WorldOriginZ;
    float32 WorldSizeX;
    float32 WorldSizeZ;
    float32 HeightScale;
    float32 InvHeightmapWidth;
    float32 InvHeightmapHeight;
    float32 TexelSize;          // 1.0 / (resolution - 1)
    float32 WorldOriginY;
    uint32 SplatmapBindless = 0;//  bindless index for RGBA8 splatmap (0 = none)
    float32 MaterialTiling = 10.0f; // world-space UV scale for material textures
    uint32 LayerCount = 0;      // number of active material layers (0-4)
    // Material-table index each channel role resolves to: splat channel i shades with record
    // LayerRole[i] of the table bound beside this buffer (TerrainMaterialRecord). Which
    // material a channel means lives HERE rather than in the splat, so re-pointing a channel is
    // a params edit and never a repaint.
    uint32 LayerRole[kTerrainLayerRoleCount] = {}; // [12-15]
    // Grid morph parameter: half the patch grid size (e.g. 16 for grid=32).
    // Used by vertex shaders for CDLOD vertex morphing.
    float32 GridDimHalf = 16.0f;
    uint32 Flags = 0;                  // bit 0 = receive shadows
    uint32 NormalmapBindless = 0;      // bindless index for R16G16_FLOAT normal map (0 = none)
    uint32 HeightmapBindless = 0;      // bindless index for height map (0 = none)
    uint32 GrassEnabled = 1;           // kTerrainGrassBit*: bit0 enabled, bit1 splat root, bit2 texture card, bit3 blend mode
    float32 GrassDensity = 21.0f;      // blades per square metre at the camera
    // [22][23] The authored blade size, and the aspect ceiling ties them: Components::TerrainGrass
    // carries why the width sits just under BladeHeight * MaxWidthRatio.
    float32 GrassBladeHeight = 0.72f;
    float32 GrassBladeWidth = 0.043f;
    float32 GrassMaskThreshold = 0.45f;
    // density falloff exponent: 1 = linear to Range, higher spends more of the budget near the camera
    float32 GrassDensityFalloff = 2.0f;
    // metres at which density reaches zero; the hard visibility range
    float32 GrassRange = 500.0f;
    float32 GrassWindStrength = 1.0f;
    uint32 GrassLayerIndex = 0;
    float32 GrassBrightness = 1.0f;    // material brightness multiplier
    // master gate over the grounding read: the gust shading, the root-shade ramp and the
    // blade's blend into its ground. Nothing to do with cast shadows.
    float32 GrassGroundingStrength = 1.0f;
    float32 GrassRandomScale = 0.35f;  // random blade scale amount
    float32 GrassRandomBrightness = 0.42f;// per-blade value jitter: the field's tonal separation
    float32 GrassTranslucency = 0.35f; // fake sun backlight strength
    uint32 GrassRootColor = 0xFF335F1Au;// root color, ARGB
    uint32 GrassTipColor = 0xFFB3DB4Du; // tip color, ARGB
    uint32 GrassBacklightColor = 0xFFE6F06Au;// fake sun backlight color, ARGB
    float32 GrassWindDirection = -2.5f; // radians
    float32 GrassWindGustSpeed = 0.9f;
    float32 GrassWindGustScale = 0.05f;
    float32 GrassWindRestingLean = 0.12f;
    float32 GrassWindFlutterAmount = 0.16f;
    float32 GrassWindFlutterSpeed = 2.2f;
    float32 GrassWindSeed = 3.0f;
    uint32 GrassBladeSegments = 5u;     // shared blade mesh segment request
    uint32 GrassAlbedoBindless = 0u;    // optional grass color/atlas texture
    uint32 GrassAlphaBindless = 0u;     // optional separate grass alpha texture
    uint32 GrassNormalBindless = 0u;    // optional grass normal map
    uint32 GrassAtlasColumns = 1u;
    uint32 GrassAtlasRows = 1u;
    uint32 GrassAtlasTileCount = 1u;
    float32 GrassAlphaCutoff = 0.35f;
    float32 GrassNormalStrength = 0.75f;
    float32 GrassTextureCardsPerSquareMeter = 1.5f; // cards per square metre at the camera
    float32 GrassTextureSize = 1.0f;    // texture card square size
    float32 GrassPlacementSeed = 3.0f;  // authored placement seed; hashed per (cell, slot) so the same seed reproduces the same blades
    // aspect ceiling: blade width is clamped to GrassBladeHeight * this. A ceiling, not the
    // width control — GrassBladeWidth is that, and this only bites at extreme aspect ratios.
    float32 GrassMaxWidthRatio = 0.06f;
    // clump lattice edge in metres; <= 0 disables clumping and restores the plain field
    float32 GrassClumpSize = 1.1f;
    float32 GrassClumpHeightVariance = 0.3f;
    float32 GrassClumpAlignment = 0.45f;      // yaw pull toward the clump's common facing
    float32 GrassClumpGather = 0.25f;         // position pull toward the clump centre
    // +/- 30 deg between tufts at 1.0; blades inside a tuft take a fixed share of it
    float32 GrassHueVariation = 0.45f;
    float32 GrassRootShade = 0.82f;           // shade-ramp floor at the root
    // how far the near-field shading normal leaves the canopy normal toward the blade's own
    float32 GrassBladeNormalForm = 0.75f;
    // brightness handed back for the diffuse response that form gives up; rides the same weight
    float32 GrassBladeScatterGain = 0.6f;
    float32 GrassRootFadeStart = 0.0f;        // height fraction where the ground colour starts to leave
    float32 GrassRootFadeEnd = 1.0f;          // height fraction where the tip colour has fully arrived
    uint32 GrassControlBindless = 0;         // optional RG8: height, density; absent means (1,1)
};

static_assert(sizeof(TerrainGPUParams) == 272, "TerrainGPUParams must be 272 bytes");

// Blades per square metre at the camera for this row, in the units the row's mode actually places:
// cards for a texture-card terrain, blades otherwise. The placement kernels select the same way, so
// anything that reasons about this row's demand has to read it through here.
inline float32 TerrainGrassNearDensity(const TerrainGPUParams& params)
{
    return (params.GrassEnabled & kTerrainGrassBitTextureCard) != 0u
        ? params.GrassTextureCardsPerSquareMeter
        : params.GrassDensity;
}

// Whether this row places a blade at all. Enabled is not enough: a zero density places nothing, and
// a zero blade height or width places blades with no area.
//
// The single definition of the set. Extraction clears kTerrainGrassBitEnabled on a row this
// rejects, so the GPU classify's cheap bit-0 test admits exactly the rows the CPU budget summary
// reduced over — and the slot bound the plan kernel derives from that summary, which the emit
// kernel spends without a runtime clamp, covers every cell the classify hands it.
inline bool TerrainGrassRowPlaces(const TerrainGPUParams& params)
{
    return (params.GrassEnabled & kTerrainGrassBitEnabled) != 0u
        && TerrainGrassNearDensity(params) > 0.0f
        && params.GrassBladeHeight > 0.0f
        && params.GrassBladeWidth > 0.0f;
}

} // namespace GameEngine::Terrain
