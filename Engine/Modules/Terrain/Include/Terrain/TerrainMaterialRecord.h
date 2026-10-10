#pragma once

#include <cstdint>
#include <type_traits>

namespace GameEngine::Terrain
{

// ---------------------------------------------------------------------------
// Terrain material record — the one per-material GPU record
// ---------------------------------------------------------------------------
// A terrain's materials live in a flat table indexed by SLOT ID, uploaded as a std430 SSBO.
// The fragment reads the whole record of the few materials a texel actually blends, so the
// per-fragment cost is independent of how many materials the table holds.
//
// AoS, not SoA: a fragment reads EVERY field of K records and no field of the other N-K, so two
// 80 B records touch two or three cache lines where SoA would touch up to twenty. MaterialData in
// adapter_forward.glsl is AoS for the same reason.
//
// Texture-or-value is one field pair, not a mode enum: a texture slot of 0 is the bindless
// sentinel (never sampled), which means "use the paired scalar". Binding a texture leaves the
// scalar live as a multiplier. No extra state, no shader variant.
//
// std430 mirror of TerrainMaterialRecordData in cbt_surface.glsl — 20 x 4 B, edited in lockstep.
// CBTLayoutTests.GlslSurfaceStructsMatchCppStd430 parses that declaration and fails on any drift
// in field name, type, order, offset or total size.
struct TerrainMaterialRecord
{
    // --- authored colour + scale ---------------------------------------------
    float AlbedoR = 1.0f; // linear tint; multiplies a bound albedo, and IS the colour unbound
    float AlbedoG = 1.0f;
    float AlbedoB = 1.0f;
    float Tiling = 1.0f;  // multiplies the terrain's global MaterialTiling (1 = the global rate)

    // --- bindless texture slots; 0 = unbound (the reserved sentinel, never sampled) ------
    uint32_t AlbedoTex = 0u;
    uint32_t NormalTex = 0u; // tangent-space, triplanar-reoriented
    uint32_t OrmTex = 0u;    // R = ambient occlusion, G = roughness, B = metallic
    uint32_t Flags = 0u;     // kTerrainMaterialFlag* below

    // --- scalars: a multiplier on the paired texture's sampled value. Defaults are the
    // --- multiplier IDENTITY, matching TerrainMaterialEntry's: a bound map reads unmodified
    // --- until an author trims it. The per-role palette below overrides Roughness with its own
    // --- authored look. Roughness and Ao are ALSO the value used when their map is unbound;
    // --- NormalStrength is not, because it scales a sampled tangent XY and there is nothing
    // --- to scale without a NormalTex. -------------------------------------------------
    float Roughness = 1.0f;
    float Ao = 1.0f;
    float NormalStrength = 1.0f;
    float HexRotStrength = 0.0f; // 0 = plain REPEAT tiling

    // --- procedural break-up for the untextured case -------------------------
    float VariationStrength = 0.0f; // value-noise brightness jitter [0,1]; 0 => flat tint
    float VariationHue = 0.0f;      // value-noise chroma jitter [0,1]
    float VariationScale = 0.0f;    // value-noise frequency (1/m); 0 => variation off

    // --- derived, not authored ------------------------------------------------
    // Planar footprint scale per axis (X here, Z after the phase): the terrain's render extent
    // divided by its authored size. The surface's footprint UV spans the extent its height,
    // normal and splat textures cover, and a tiled terrain's tile grid can overhang the authored
    // size by up to a tile; a Planar material multiplies by this so its image spans exactly the
    // authored footprint, which is where the heightmap is spread (FillTiledBaseRegion). 1 for an
    // untiled terrain. Read only by a Planar material.
    float PlanarUVScaleX = 1.0f;

    // World-anchoring UV phase per world axis: fract(renderOriginWorld * effectiveTiling),
    // computed in DOUBLE because fp32 cannot hold it at planetary magnitude. It depends on the
    // material's own Tiling AND on the camera's render-origin sector, so the table is rebuilt
    // when the origin rebases, not only when a material is edited — which is why the whole table
    // is authored per frame in TerrainExtractionSystem's AppendTerrainMaterials rather than
    // cached. A table treated as "static, upload on edit" swims on planets and looks fine on
    // planar terrain, which is the nastiest half-visible version of this bug.
    float UVPhaseX = 0.0f;
    float UVPhaseY = 0.0f;
    float UVPhaseZ = 0.0f;
    float PlanarUVScaleZ = 1.0f; // see PlanarUVScaleX
};

static_assert(sizeof(TerrainMaterialRecord) == 80,
              "TerrainMaterialRecord must be 20 x 4B (std430 mirror, 16-byte aligned)");
static_assert(sizeof(TerrainMaterialRecord) % 16 == 0,
              "TerrainMaterialRecord must stay 16-byte aligned for std430 array stride");
static_assert(std::is_trivially_copyable_v<TerrainMaterialRecord>,
              "TerrainMaterialRecord is memcpy'd into the upload buffer");

// TerrainMaterialRecord::Flags bits. Reserved bits stay free so per-material sampler and
// triplanar toggles can be added without moving a field.
inline constexpr uint32_t kTerrainMaterialFlagHexTiling = 1u << 0;
// A retired (tombstoned) material keeps rendering for already-painted texels but disappears from
// pickers. Its slot ID stays reserved until an explicit purge frees it.
inline constexpr uint32_t kTerrainMaterialFlagRetired = 1u << 1;
inline constexpr uint32_t kTerrainMaterialFlagOrmHasMetallic = 1u << 2;
// Texture PRESENCE, authored from the source GUID rather than from a resolved index. A profile
// with no descriptor indexing resolves every index to the unbound sentinel below, so an index
// cannot answer "is a texture bound for this slot" there — the texture arrives through a named
// binding instead. Same split the terrain maps already use (kTerrainFlagHas* in TerrainTypes.h).
inline constexpr uint32_t kTerrainMaterialFlagHasAlbedo = 1u << 3;
inline constexpr uint32_t kTerrainMaterialFlagHasNormal = 1u << 4;
inline constexpr uint32_t kTerrainMaterialFlagHasOrm = 1u << 5;
// Planar projection: every texture of the material is sampled at the terrain's own footprint UV
// (0..1 across the terrain, times Tiling) instead of through the three world-space triplanar
// projections. One image then lies on the heightmap it was captured with (an orthophoto as a
// basemap), and a cliff shows that image stretched rather than another part of it. Clear for
// every material authored before the choice existed, so those render exactly as before.
inline constexpr uint32_t kTerrainMaterialFlagPlanar = 1u << 6;

// The bindless sentinel: index 0 is reserved and never written, so it is the natural "unbound"
// marker for every texture slot in the record (BindlessResourceManager reserves it).
inline constexpr uint32_t kTerrainMaterialUnboundTexture = 0u;

// Slot IDs are 8-bit, so a terrain's table holds at most 256 materials — 20 KiB at 80 B each.
// The table is indexed DIRECTLY by slot ID (no indirection), so unused slots are zero-filled
// rather than compacted, and a slot ID is stable for the life of the material.
inline constexpr uint32_t kMaxTerrainMaterials = 256u;

// An RGBA8 splat carries exactly four channel weights. A ROLE is the binding from one of those
// channels to a material: the weight says how much, the role says of what, so material placement
// is not coupled to layer indices 0-3. Locked to the splat channel count
// (kMaxTerrainMaterialLayers) by a static_assert in TerrainTypes.h — one role per channel.
inline constexpr uint32_t kTerrainLayerRoleCount = 4u;

// What each channel role MEANS, in splat channel order. These are the names a terrain shows for a
// channel it has bound no material to, and the names a library minted from a terrain's per-layer
// fields gives its four entries — so the engine's mint and the editor's pickers cannot disagree
// about which channel is "Rock". They are a naming convention over the channels, NOT a claim that
// anything derives them: what lands in each channel is whatever the authored surface rules put
// there, and a row is free to put rock in the channel named "Grass".
inline constexpr const char* kTerrainLayerRoleNames[kTerrainLayerRoleCount] = {
    "Grass", "Rock", "Dirt", "Snow"};

// The material each channel role resolves to before a terrain authors its own. These four
// records ARE the untextured terrain palette: this is the single source the whole engine reads it
// from — the editor's unbound-slot swatch (TerrainLayers::FallbackTintSwatchArgb), the planet
// surface palette (CBTRenderFeature) and the grass blade's grounded base colour all resolve here.
//
// Reading the same RECORD is necessary for the ground and the grass standing on it to agree, and it
// is not sufficient: what a record MEANS is the expression evaluated on it, and the variation below
// is part of that meaning. The two surfaces therefore share the expression as well — both include
// Includes/terrain_material_albedo.glsl — so a blade's base resolves onto the colour the ground
// beside it actually renders rather than onto the record's flat tint.
//
// Roughness carries the per-role values the surface used to blend out of TerrainGPUParams; the
// variation amplitudes are footprint-faded in the fragment (CBT_ApplyMaterialVariation), so they
// are enough break-up to kill the flat tint without reading as seams.
inline constexpr TerrainMaterialRecord kDefaultTerrainMaterials[kTerrainLayerRoleCount] = {
    // grass
    {.AlbedoR = 0.35f, .AlbedoG = 0.55f, .AlbedoB = 0.18f, .Roughness = 0.85f,
     .VariationStrength = 0.16f, .VariationHue = 0.10f, .VariationScale = 0.030f},
    // rock — the strongest break-up: a bare cliff face is the largest flat expanse
    {.AlbedoR = 0.55f, .AlbedoG = 0.50f, .AlbedoB = 0.42f, .Roughness = 0.65f,
     .VariationStrength = 0.24f, .VariationHue = 0.12f, .VariationScale = 0.022f},
    // dirt
    {.AlbedoR = 0.50f, .AlbedoG = 0.38f, .AlbedoB = 0.25f, .Roughness = 0.75f,
     .VariationStrength = 0.18f, .VariationHue = 0.10f, .VariationScale = 0.030f},
    // snow — near-white, so the same jitter reads far stronger; kept low
    {.AlbedoR = 0.90f, .AlbedoG = 0.92f, .AlbedoB = 0.95f, .Roughness = 0.90f,
     .VariationStrength = 0.07f, .VariationHue = 0.03f, .VariationScale = 0.040f},
};

} // namespace GameEngine::Terrain
