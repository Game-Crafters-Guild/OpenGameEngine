// CBT terrain surface (plan §8 C4/C6). Implements EvaluateSurface() for the CBT
// material adapter pipeline. C6 brings full splatmap parity with the CDLOD surface:
// it samples the SAME RGBA8 splatmap + per-layer albedo/roughness the CDLOD path binds
// (from the shared TerrainParams SSBO the extraction system fills — renderer-blind),
// blended by the CDLOD splat blend formula (terrain_surface.glsl, deleted at the C6
// cutover — see git history), so a painted terrain reads identically. The pre-baked terrain
// normalmap is still sampled per-pixel (bindless index via custom0.w), matching C4.
//
// CBT is single-terrain (terrain 0 — plan §8 C4), so the surface reads terrains[0];
// the vertex modifier's custom0.x carries the bisector slot (debug), not a terrain
// index. The TerrainParams SSBO rides set 2 (binding 1) — the same DrawBindings route
// terrain uses — resolved by MaterialBinder by the reflected block name.
//
// Inputs from cbt_vertex_modifier.glsl:
//   sIn.positionWS = displaced world position
//   sIn.uv0        = terrain UV [0,1] (splatmap + normalmap lookup)
//   sIn.custom0.w  = terrain normalmap bindless index (0 = none)

#extension GL_EXT_nonuniform_qualifier : require

// Global bindless arrays + GE_BTEX — single declaration site (guarded include).
#include "Includes/bindless_textures.glsl"

// Domain math shared with the compute kernels (pure functions, no bindings): the multi-
// octave relief + its analytic gradient (per-pixel sphere normal) and the direction ->
// (face,uv) map (seam-free sculpt sampling). Staged beside this file (Assets/Shaders/CBT).
#include "cbt_domain.glsl"

// Analytic sphere-modifier math (sculpt shape-accuracy S2) — pure math, no bindings, shared with
// the compute layout. Declares CBTSphereAnalyticFlatten before CBTSurfaceParamsData carries it.
#include "cbt_analytic.glsl"

// Resident-window atlas resolve (terrain UV -> tile -> slot UV). Pure math, no bindings — bit-for-bit
// the SAME resolve the compute height path runs (cbt_layout.glsl includes this file too). The surface
// taps splat/normal at the resolved slot UV; the indirection rows ride set 2 (gAtlasRows) below.
#include "cbt_atlas.glsl"

// ---------------------------------------------------------------------------
// Compat-profile terrain textures (WebGPU class).
// ---------------------------------------------------------------------------
// The full profile reaches every terrain texture through a bindless index carried in the
// terrain params / material table. A WebGPU-class target has no descriptor indexing at all,
// and the compat MATERIAL slots (bindless_textures.glsl) are eight fixed textures reserved
// for a material's own properties — terrain needs more than that and does not author them as
// properties. So the compat arm gives terrain its OWN fixed bindings in the draw set, and the
// CPU resolves the same indices to handles when it fills them (CBTResources DrawBindings).
//
// The index fields stay authoritative for WHICH texture is bound; only the lookup moves.
// A zero index still means "unbound" and still short-circuits before any tap, so a terrain
// with no splatmap or no layer textures costs nothing here.
#if defined(GE_COMPAT_PROFILE)

// Layers x {albedo, normal, orm}. CBT_MAX_BLEND_MATERIALS layers would need 12 sampled
// textures on top of the 6 below, and WebGPU only GUARANTEES 16 per fragment stage — the
// device asks for the adapter's real limit, but the guarantee is what portability rests on.
// Two layers keep the total at 12 and leave headroom; a terrain painted with more blends the
// first two and reports the rest at bring-up rather than failing to compile.
const uint kCBTCompatLayers = 2u;
const uint kCBTLayerTexPerMaterial = 3u; // albedo, normal, orm — the slot stride

// Layer ordinal + texture kind -> compat binding slot. Layers past kCBTCompatLayers wrap
// onto the last bound layer rather than reading an unbound slot: the weights still blend,
// the extra layer just repeats a neighbour's textures instead of vanishing.
#define CBT_LAYER_SLOT(ord, kind) \
    (min(ord, kCBTCompatLayers - 1u) * kCBTLayerTexPerMaterial + (kind))

// SEPARATE texture + sampler, not twelve sampler2D. A combined sampler2D is split by the cook
// into a texture AND a sampler apiece, and WebGPU's per-stage sampler limit is 16 — twelve here
// plus the lit path's own (six shadow maps, IBL, GTAO, SSSR, scene grabs) came to 27 and the
// TERRAIN PIPELINE FAILED TO CREATE, which is a black terrain, not a degraded one. One shared
// sampler is what fits. (Textures are not the scarce resource here; samplers are.)
//
// That one sampler repeats, because the layer maps must tile. The six data maps want clamp, so
// they clamp their UV explicitly at the tap — identical to a clamp-to-edge sampler for a lookup
// that is already inside [0,1], and the atlas resolve is what keeps a slot from bleeding anyway.
layout(set = 2, binding = 20) uniform texture2D cbt_Normalmap;
layout(set = 2, binding = 21) uniform texture2D cbt_Splatmap;
layout(set = 2, binding = 22) uniform texture2D cbt_AtlasNormal;
layout(set = 2, binding = 23) uniform texture2D cbt_AtlasNormalCoarse;
layout(set = 2, binding = 24) uniform texture2D cbt_AtlasSplat;
layout(set = 2, binding = 25) uniform texture2D cbt_AtlasSplatCoarse;
layout(set = 2, binding = 26) uniform texture2D cbt_Layer0Albedo;
layout(set = 2, binding = 27) uniform texture2D cbt_Layer0Normal;
layout(set = 2, binding = 28) uniform texture2D cbt_Layer0Orm;
layout(set = 2, binding = 29) uniform texture2D cbt_Layer1Albedo;
layout(set = 2, binding = 30) uniform texture2D cbt_Layer1Normal;
layout(set = 2, binding = 31) uniform texture2D cbt_Layer1Orm;
layout(set = 2, binding = 32) uniform sampler cbt_MapSampler;

// A data-map lookup: explicit LOD, UV clamped in place of the clamp sampler this profile cannot
// afford.
#define CBT_COMPAT_DATA_TAP(tex, uv) \
    textureLod(sampler2D(tex, cbt_MapSampler), clamp(uv, 0.0, 1.0), 0.0)
// The same lookup into a terrain lattice texture, from a terrain UV (CBT_LatticeTextureUV).
#define CBT_COMPAT_LATTICE_TAP(tex, uv) \
    textureLod(sampler2D(tex, cbt_MapSampler), \
               CBT_LatticeTextureUV(uv, vec2(textureSize(sampler2D(tex, cbt_MapSampler), 0))), 0.0)

// GLSL cannot return an opaque sampler from a function, so the slot walk completes the tap —
// the same shape ge_CompatSampleSlot uses for material slots.
vec3 CBT_CompatLayerSample(uint slot, vec2 uv, vec2 ddx, vec2 ddy)
{
    switch (slot)
    {
    case 0u: return textureGrad(sampler2D(cbt_Layer0Albedo, cbt_MapSampler), uv, ddx, ddy).rgb;
    case 1u: return textureGrad(sampler2D(cbt_Layer0Normal, cbt_MapSampler), uv, ddx, ddy).rgb;
    case 2u: return textureGrad(sampler2D(cbt_Layer0Orm, cbt_MapSampler), uv, ddx, ddy).rgb;
    case 3u: return textureGrad(sampler2D(cbt_Layer1Albedo, cbt_MapSampler), uv, ddx, ddy).rgb;
    case 4u: return textureGrad(sampler2D(cbt_Layer1Normal, cbt_MapSampler), uv, ddx, ddy).rgb;
    default: return textureGrad(sampler2D(cbt_Layer1Orm, cbt_MapSampler), uv, ddx, ddy).rgb;
    }
}

// The six single-purpose terrain lookups. These are DATA maps (height-derived normals, splat
// weights, their atlas variants), not detail colour, and every one is read inside an atlas or
// weight branch — so they sample at LOD 0 rather than with an implicit derivative that would be
// illegal there. That is also exactly how the compute side reads the same textures
// (cbt_layout.glsl samples the height ring with textureLod(..., 0.0)). The unified normal map and
// splat map take a terrain UV and read their lattice there; the atlas taps take a resolved UV.
#define CBT_SAMPLE_NORMALMAP(idx, uv) CBT_COMPAT_LATTICE_TAP(cbt_Normalmap, uv)
#define CBT_SAMPLE_SPLATMAP(idx, uv) CBT_COMPAT_LATTICE_TAP(cbt_Splatmap, uv)
#define CBT_SAMPLE_ATLAS_NORMAL(idx, uv) CBT_COMPAT_DATA_TAP(cbt_AtlasNormal, uv)
#define CBT_SAMPLE_ATLAS_NORMAL_COARSE(idx, uv) CBT_COMPAT_DATA_TAP(cbt_AtlasNormalCoarse, uv)
#define CBT_SAMPLE_ATLAS_SPLAT(idx, uv) CBT_COMPAT_DATA_TAP(cbt_AtlasSplat, uv)
#define CBT_SAMPLE_ATLAS_SPLAT_COARSE(idx, uv) CBT_COMPAT_DATA_TAP(cbt_AtlasSplatCoarse, uv)

// Whether a map is there to tap. This profile has no descriptor indexing, so every bindless index
// above reads 0 (TextureService publishes none) and cannot say it. The unified maps' existence
// rides the terrain's flag bits instead (Terrain/TerrainTypes.h mirror), as the grass placement's
// compat arm reads them. The four atlas surface maps need no bit: extraction creates all four
// before it marks a terrain atlas-backed, and a WebGPU texture reads zero until its first upload,
// which the callers take as "no splat" (channel 0) and "no normal" (flat up), the same answers the
// full profile gives before an index is published.
const uint kCBTTerrainFlagHasNormalmap = 8u;
const uint kCBTTerrainFlagHasSplatmap = 16u;
#define CBT_HAS_NORMALMAP(tp, idx) ((tp.Flags & kCBTTerrainFlagHasNormalmap) != 0u)
#define CBT_HAS_SPLATMAP(tp) ((tp.Flags & kCBTTerrainFlagHasSplatmap) != 0u)
#define CBT_HAS_ATLAS_MAP(idx) true

#else // full profile — every lookup is a bindless index

const uint kCBTCompatLayers = 4u;
const uint kCBTLayerTexPerMaterial = 3u;

// Unused in this profile — the bindless index identifies the texture on its own.
#define CBT_LAYER_SLOT(ord, kind) 0u

// The unified normal map and splat map read their lattice at a terrain UV (CBT_LatticeTextureUV).
#define CBT_LATTICE_TAP(idx, uv) \
    texture(GE_BTEX(idx, GE_TS_CLAMP), \
            CBT_LatticeTextureUV(uv, vec2(textureSize(GE_BTEX(idx, GE_TS_CLAMP), 0))))
#define CBT_SAMPLE_NORMALMAP(idx, uv) CBT_LATTICE_TAP(idx, uv)
#define CBT_SAMPLE_SPLATMAP(idx, uv) CBT_LATTICE_TAP(idx, uv)
#define CBT_SAMPLE_ATLAS_NORMAL(idx, uv) texture(GE_BTEX(idx, GE_TS_CLAMP), uv)
#define CBT_SAMPLE_ATLAS_NORMAL_COARSE(idx, uv) texture(GE_BTEX(idx, GE_TS_CLAMP), uv)
#define CBT_SAMPLE_ATLAS_SPLAT(idx, uv) texture(GE_BTEX(idx, GE_TS_CLAMP), uv)
#define CBT_SAMPLE_ATLAS_SPLAT_COARSE(idx, uv) texture(GE_BTEX(idx, GE_TS_CLAMP), uv)

// A map is there when its bindless index is published (0 = nothing bound).
#define CBT_HAS_NORMALMAP(tp, idx) ((idx) != 0u)
#define CBT_HAS_SPLATMAP(tp) (tp.SplatmapBindless != 0u)
#define CBT_HAS_ATLAS_MAP(idx) ((idx) != 0u)

#endif // GE_COMPAT_PROFILE


// Requires: Includes/surface_io.glsl (included by the adapter)

// The blend width (CBT_MAX_BLEND_MATERIALS) and the commentary for that knob. Included before the
// resolve below, which is parameterized by it.
#include "Includes/terrain_blend_width.glsl"

// Terrain params SSBO — same struct + set/binding the CDLOD surface reads, so the two
// renderers pull identical per-terrain material config. std430, set 2 binding 1.
struct TerrainParamsEntry
{
    float WorldOriginX;
    float WorldOriginZ;
    float WorldSizeX;
    float WorldSizeZ;
    float HeightScale;
    float InvHeightmapWidth;
    float InvHeightmapHeight;
    float TexelSize;
    float WorldOriginY;
    uint SplatmapBindless;
    float MaterialTiling;
    uint LayerCount;
    uint LayerRole[4];
    float GridDimHalf;
    uint Flags;
    uint NormalmapBindless;
    uint HeightmapBindless;
    uint GrassEnabled;
    float GrassDensity;
    float GrassBladeHeight;
    float GrassBladeWidth;
    float GrassMaskThreshold;
    float GrassDensityFalloff;
    float GrassRange;
    float GrassWindStrength;
    uint GrassLayerIndex;
    float GrassBrightness;
    float GrassGroundingStrength;
    float GrassRandomScale;
    float GrassRandomBrightness;
    float GrassTranslucency;
    uint GrassRootColor;
    uint GrassTipColor;
    uint GrassBacklightColor;
    float GrassWindDirection;
    float GrassWindGustSpeed;
    float GrassWindGustScale;
    float GrassWindRestingLean;
    float GrassWindFlutterAmount;
    float GrassWindFlutterSpeed;
    float GrassWindSeed;
    uint GrassBladeSegments;
    uint GrassAlbedoBindless;
    uint GrassAlphaBindless;
    uint GrassNormalBindless;
    uint GrassAtlasColumns;
    uint GrassAtlasRows;
    uint GrassAtlasTileCount;
    float GrassAlphaCutoff;
    float GrassNormalStrength;
    float GrassTextureCardsPerSquareMeter;
    float GrassTextureSize;
    float GrassPlacementSeed;
    float GrassMaxWidthRatio;
    float GrassClumpSize;
    float GrassClumpHeightVariance;
    float GrassClumpAlignment;
    float GrassClumpGather;
    float GrassHueVariation;
    float GrassRootShade;
    float GrassBladeNormalForm;
    float GrassBladeScatterGain;
    float GrassRootFadeStart;
    float GrassRootFadeEnd;
    uint GrassControlBindless;
};

layout(std430, set = 2, binding = 1) readonly buffer TerrainParamsBuffer
{
    TerrainParamsEntry terrains[];
};

// Layer-albedo sampler access with a PLAIN (dynamically uniform) descriptor index. THIS surface
// draws one terrain, so a layer's bindless index is one per-draw palette/params value — identical
// across every invocation — and the NonUniform decoration GE_BTEX carries is unnecessary here;
// dropping it lets the driver hoist the descriptor load once per draw. The grass surface, whose
// draws span several terrains, defines this differently and must.
#if defined(GE_COMPAT_PROFILE)
// Never expanded on this profile: every layer tap goes through CBT_LAYER_TAP below, which selects
// a NAMED binding by slot because there is no array to index. Defined only because the include
// requires the symbol — expanding it is a compile error that names this line, which is the intent.
#define CBT_LAYER_TEX(texIdx) GE_COMPAT_PROFILE_HAS_NO_BINDLESS_TEXTURE_ARRAY
#else
#define CBT_LAYER_TEX(texIdx) sampler2D(ge_BindlessTextures[texIdx], ge_BindlessSamplers[GE_TS_REPEAT])
#define CBT_LAYER_TEX_EDGE(texIdx) sampler2D(ge_BindlessTextures[texIdx], ge_BindlessSamplers[GE_TS_CLAMP_ANISO])
#endif

// The material record and its ALBEDO resolve — the record layout, the triplanar context, the hex
// lattice, the untextured value-noise variation and CBT_MaterialAlbedo itself. Shared with the
// grass surface, which grounds a blade's base onto the colour this file resolves for the ground
// under it: one definition, so the two cannot answer differently. A terrain's materials live in a
// flat table indexed by SLOT ID; the fragment reads the whole record of the few materials a texel
// blends, so per-fragment cost does not grow with the table.
// Texture PRESENCE per slot. Mirrors of kTerrainMaterialFlagHas{Albedo,Normal,Orm}
// (TerrainMaterialRecord.h); CBTLayoutTests pins these to the C++ values.
//
// An index field answers WHICH texture only on a profile that has descriptor indexing. The compat
// profile resolves every index to the unbound sentinel and reaches the texture through a named
// binding instead, so "is a texture bound for this slot" has to ride these flags there — an
// index test reads untextured for every layer and the surface returns its tint before any tap.
const uint CBT_MATFLAG_HAS_ALBEDO = 8u;
const uint CBT_MATFLAG_HAS_NORMAL = 16u;
const uint CBT_MATFLAG_HAS_ORM = 32u;

#if defined(GE_COMPAT_PROFILE)
#define CBT_MAT_HAS_ALBEDO(mat) (((mat).Flags & CBT_MATFLAG_HAS_ALBEDO) != 0u)
#define CBT_MAT_HAS_NORMAL(mat) (((mat).Flags & CBT_MATFLAG_HAS_NORMAL) != 0u)
#define CBT_MAT_HAS_ORM(mat) (((mat).Flags & CBT_MATFLAG_HAS_ORM) != 0u)
#else
// The full profile keeps the index as the test: a GUID that failed to resolve leaves the index at
// the sentinel, and falling back to the tint beats sampling the reserved slot.
#define CBT_MAT_HAS_ALBEDO(mat) ((mat).AlbedoTex != 0u)
#define CBT_MAT_HAS_NORMAL(mat) ((mat).NormalTex != 0u)
#define CBT_MAT_HAS_ORM(mat) ((mat).OrmTex != 0u)
#endif

// One layer-material tap. The two profiles disagree about what identifies a texture, so the tap
// takes BOTH: the bindless index the full profile indexes with, and the layer SLOT the compat
// profile binds by. `slot` is layerOrdinal * kCBTLayerTexPerMaterial + kind (albedo/normal/orm),
// which is dynamically uniform per draw.
vec3 CBT_LayerTap(uint slot, uint tex, vec2 uv, vec2 ddx, vec2 ddy, float bias)
{
#if defined(GE_COMPAT_PROFILE)
    // No descriptor indexing here, so the slot cannot index a binding — it selects one. The switch
    // is the same shape the compat material path uses (ge_CompatSampleSlot). Explicit gradients
    // keep the mip chain live where an implicit tap would be illegal: the caller's control flow is
    // data-dependent, and the bias an implicit tap would carry is folded into the gradients
    // instead (bias b scales the footprint by exp2(b)).
    return CBT_CompatLayerSample(slot, uv, ddx * exp2(bias), ddy * exp2(bias));
#else
    return texture(CBT_LAYER_TEX(tex), uv, bias).rgb;
#endif
}
// The shared include's tap seam resolves to it.
#define CBT_LAYER_TAP(slot, tex, uv, ddx, ddy, bias) CBT_LayerTap(slot, tex, uv, ddx, ddy, bias)
#if defined(GE_COMPAT_PROFILE)
// The compat profile's named layer bindings share one sampler (cbt_MapSampler), so a Planar image
// at Tiling 1 samples REPEAT there and its borders filter in the opposite edge. A documented
// limitation of that profile.
#define CBT_LAYER_TAP_EDGE(slot, tex, uv, ddx, ddy, bias) CBT_LayerTap(slot, tex, uv, ddx, ddy, bias)
#endif

#include "Includes/terrain_material_albedo.glsl"

// Whether this material's ORM map carries metallic in B or padding there. Mirror of
// kTerrainMaterialFlagOrmHasMetallic (TerrainMaterialRecord.h); CBTLayoutTests pins the two
// together. Ungated, a padded white B would shade the whole surface as a black mirror.
const uint CBT_MATFLAG_ORM_HAS_METALLIC = 4u;

// The terrain's material table, authored by TerrainExtractionSystem and owned by
// TerrainRenderFeature — the SAME table the grass surface reads, so a blade and the ground under
// it cannot resolve different materials. Indexed by absolute table index: a splat channel resolves
// through TerrainParamsEntry::LayerRole.
//
// Declared NAMELESS on purpose. MaterialBinder keys set-2 buffers on the REFLECTED INSTANCE name,
// so a block with an instance name would have to be bound under that name instead of this one —
// getting that wrong leaves the binding unresolved, which is a null descriptor and a device loss,
// not a missing texture.
layout(std430, set = 2, binding = 4) readonly buffer TerrainMaterialTableBuffer
{
    TerrainMaterialRecordData TerrainMaterials[];
};

// Planet-shading surface params (plan §planet-shading) — mirror of CBTSurfaceParams
// (CBTLayout.h). Offset-bound to this frame's ring slot, so element [0] is the live struct.
struct CBTSurfaceParamsData
{
    float Radius;
    float ReliefAmplitude;
    float ReliefFrequency;
    uint ReliefOctaves;
    uint SphereSculptEnabled;
    // Paged sculpt geometry (mirror SphereSculptGeometry / CBTSurfaceParams) — the fragment's page
    // addressing (no hardcode). VirtualDim radius-scaled; Cap fixed table row stride.
    uint SphereSculptVirtualDim;
    uint SphereSculptCap;
    uint SphereSculptPagesPerAxis;
    uint SphereSculptPoolPageCount;
    uint DebugMode;           // TerrainDebugView mirror: 0 = off, 1 = facet tint (CBT triangulation)
    uint AtlasBacked;         // Phase E: 1 -> planar terrain resolves splat/normal per-pixel via the atlas
    // Quality-sweep slice 1 atlas geometry + bindless indices (mirror CBTSurfaceParams in CBTLayout.h).
    uint AtlasDim;
    uint AtlasSlotStride;
    uint AtlasSlotsPerRow;
    uint AtlasTileRes;
    uint AtlasTilesPerAxisX;
    uint AtlasTilesPerAxisZ;
    uint AtlasCoarseDim;
    uint AtlasSplatBindless;
    uint AtlasNormalBindless;
    uint AtlasSplatCoarseBindless;
    uint AtlasNormalCoarseBindless;
    // Analytic sphere modifiers (sculpt shape-accuracy S2/S3 — mirror CBTSurfaceParams tail in
    // CBTLayout.h; math in cbt_analytic.glsl). DabCount (S3, the old pad slot) keeps the
    // vec4-struct array 16-aligned (std430: 88 + 4 + 4 = 96); transient brush dabs occupy slots
    // [Count, Count+DabCount). Both 0 = every analytic branch below is skipped (flag-off dark-ship).
    uint SphereAnalyticCount;
    uint SphereAnalyticDabCount;
    CBTSphereAnalyticFlatten SphereAnalytic[CBT_MAX_SPHERE_ANALYTIC];
};

// CBT debug visualization modes (mirror TerrainDebugView in Terrain.h — keep in lockstep).
const uint CBT_DEBUG_OFF = 0u;
const uint CBT_DEBUG_FACETS = 1u;
const uint CBT_DEBUG_ATLAS_SLOTS = 2u; // tint by the per-pixel resolved atlas slot / grey for coarse
// A UNIFORM block, not a storage buffer, and the type is what matters rather than the size: WebGPU
// guarantees only 10 storage buffers per stage, and this surface reaches exactly that on its own
// (6 here + 4 from the lit path) — so the moment the lit path adds one, the terrain pipeline fails
// to CREATE and the terrain is not degraded but absent. Uniform buffers have their own, separate
// budget. The record fits: 864 B against the 64 KiB uniform binding limit, and the ring's
// per-slot stride is already 256-aligned, which is what a uniform offset-bind needs.
//
// One element, because the fragment only ever reads element 0 — CBTRenderFeature offset-binds this
// frame's slot. An unsized array is a storage-buffer shape and is not legal here.
layout(std140, set = 2, binding = 5) uniform CBTSurfaceParamsBuffer
{
    CBTSurfaceParamsData Params;
} gCbtSurf;

// The editable sphere sculpt physical page POOL (planet editing v2) — offset-bound to this frame's
// ring slot (so page ids index from 0). Read only for the per-pixel normal's sculpt gradient, gated
// on SphereSculptEnabled. Mirror of the compute binding-16 pool; the paged bilinear + the geometry
// come from CBTSurfaceParams (no hardcode).
layout(std430, set = 2, binding = 6) readonly buffer CBTSphereSculptSurfBuffer
{
    float Sculpt[];
} gCbtSculpt;
// The sculpt PAGE TABLE (set 2, binding 8) — also offset-bound per ring slot (entries index from 0).
layout(std430, set = 2, binding = 8) readonly buffer CBTSphereSculptTableSurfBuffer
{
    uint Table[];
} gCbtSculptTable;
// Paging index math (constants + CBT_SculptTableEntry / CBT_SculptPoolIndex), shared with the compute
// path so the read is single-sourced.
#include "cbt_sculpt.glsl"
// pi/2 — a cube face's angular half-extent. One virtual texel of arc is kSculptHalfPi / Dv radians
// (mirror of SphereSculptPaging.h SculptNormalAngularStep). The sculpt-gradient finite difference
// used to step by exactly that; the crease-quality fix widens the stencil to the pixel's screen
// footprint on steep walls (see the sculpt-gradient block below), so this is the BASE (1-texel)
// step it filters up from — well-resolved ground still steps one texel, matching the CPU oracle.
const float CBT_SCULPT_HALF_PI = 1.57079632679;
// Footprint cap for the resolution-appropriate step: on a near-vertical wall many pixels map to few
// sculpt texels, so the stencil widens to average across them (killing the bilinear-cell banding),
// but never past this many texels (bounds over-smoothing + keeps the 4 taps local to the store).
const float CBT_SCULPT_MAX_STEP_TEXELS = 16.0;

// Phase E atlas indirection rows (quality-sweep slice 1) — the SAME per-tile TileAtlasSlot rows the
// compute binds at binding 17, here OFFSET-bound to this frame's ring slot (CBTRenderFeature), so
// Rows[tileIndex] is the live row (element-0 base). Resolves terrain UV -> slot for the per-pixel
// splat/normal tap. CBTTileAtlasSlot comes from cbt_atlas.glsl. std430, set 2.
layout(std430, set = 2, binding = 7) readonly buffer CBTAtlasRowsSurfBuffer
{
    CBTTileAtlasSlot Rows[];
} gAtlasRows;
// CBT_ATLAS_MAX_TILES comes from cbt_atlas.glsl (included above) — single-sourced with the compute path.

// Build the atlas params from the surface params (mirror CBTRenderFeature::BuildFrameParams).
CBTAtlasParams CBT_SurfAtlasParams(CBTSurfaceParamsData sp)
{
    CBTAtlasParams ap;
    ap.TileRes = sp.AtlasTileRes;
    ap.SlotStride = sp.AtlasSlotStride;
    ap.SlotsPerRow = sp.AtlasSlotsPerRow;
    ap.AtlasDim = sp.AtlasDim;
    ap.TilesPerAxisX = sp.AtlasTilesPerAxisX;
    ap.TilesPerAxisZ = sp.AtlasTilesPerAxisZ;
    return ap;
}

// Resolve terrain UV -> tile -> slot UV. The rows are offset-bound to this frame's ring slot, so the
// row index is the tile index directly. A non-resident row yields Resident=false -> the coarse fallback.
AtlasResolve CBT_SurfResolve(vec2 uv, CBTSurfaceParamsData sp)
{
    CBTAtlasParams ap = CBT_SurfAtlasParams(sp);
    ivec2 tile;
    vec2 localUV;
    CBT_AtlasTile(uv, ap, tile, localUV);
    uint tileIndex = CBT_AtlasTileIndex(tile, ap);
    CBTTileAtlasSlot row;
    row.Slot = CBT_ATLAS_NO_SLOT;
    row.Generation = 0u;
    row.LodBias = 0.0;
    row.Fade = 1.0; // default settled; overwritten from gAtlasRows below when the tile is in-bounds
    // tileIndex is always < TilesX*TilesZ (CBT_AtlasTile clamps the tile), and the ring slot holds
    // CBT_ATLAS_MAX_TILES rows — cap at that so a terrain over the indirection cap can never read past
    // the offset-bound Range (its tail tiles resolve through the coarse fallback, matching the compute).
    if (tileIndex < CBT_ATLAS_MAX_TILES)
        row = gAtlasRows.Rows[tileIndex];
    return CBT_AtlasResolve(uv, ap, row);
}

// Endpoint-exact coarse-field UV (mirror CBT_AtlasCoarseSample in cbt_layout.glsl): the coarse
// field is a lattice like every terrain texture (CBT_LatticeTextureUV).
vec2 CBT_SurfCoarseUV(vec2 uv, uint coarseDim)
{
    if (coarseDim < 2u)
        return clamp(uv, 0.0, 1.0);
    return CBT_LatticeTextureUV(uv, vec2(float(coarseDim)));
}

// Build the paged sculpt geometry from the surface params (mirror CBT_SculptGeomFromFrame).
CBTSculptGeom CBT_SurfSculptGeom(CBTSurfaceParamsData sp)
{
    CBTSculptGeom g;
    g.VirtualDim = sp.SphereSculptVirtualDim;
    g.Cap = sp.SphereSculptCap;
    g.PagesPerAxis = sp.SphereSculptPagesPerAxis;
    g.PoolPageCount = sp.SphereSculptPoolPageCount;
    return g;
}

// Fine texel (jx, jy) of the block `entry` decodes to. The SSBOs are offset-bound to this
// frame's ring slot, so bases are 0 (S4 level-aware pool index).
float CBT_SculptFineTexelSurf(uint entry, uint jx, uint jy)
{
    return gCbtSculpt.Sculpt[CBT_SculptPoolIndexFine(entry, jx, jy)];
}

// Resolve one virtual texel to its additive height (0 for an unallocated page). On an escalated
// page the base-ALIGNED fine texel is read. Bit-lock twin of SphereSculptPaging.h.
float CBT_SculptResolveSurf(uint face, uint vtx, uint vty, CBTSculptGeom g)
{
    uint pageX = vtx / CBT_SCULPT_PAGE_DIM;
    uint pageY = vty / CBT_SCULPT_PAGE_DIM;
    uint entry = gCbtSculptTable.Table[CBT_SculptTableEntry(face, pageX, pageY, g.Cap)];
    if (entry == CBT_SCULPT_NO_PAGE)
        return 0.0;
    uint level = CBT_SculptEntryLevel(entry);
    uint localX = vtx - pageX * CBT_SCULPT_PAGE_DIM;
    uint localY = vty - pageY * CBT_SCULPT_PAGE_DIM;
    return CBT_SculptFineTexelSurf(entry, localX << level, localY << level);
}

// S4 seam-stitch primitives (mirror SphereSculptPaging.h SculptEdgeColEval / SculptEdgeRowEval
// and the cbt_layout.glsl compute twins).
float CBT_SculptEdgeColEvalSurf(uint entry, uint jx, float tLocal)
{
    if (entry == CBT_SCULPT_NO_PAGE)
        return 0.0;
    uint lstep = 1u << CBT_SculptEntryLevel(entry);
    uint jmax = 127u * lstep;
    float sv = tLocal * float(lstep);
    uint k0 = min(uint(sv), jmax);
    uint k1 = min(k0 + 1u, jmax);
    float a = CBT_SculptFineTexelSurf(entry, jx, k0);
    float b = CBT_SculptFineTexelSurf(entry, jx, k1);
    return mix(a, b, sv - float(k0));
}

float CBT_SculptEdgeRowEvalSurf(uint entry, uint jy, float tLocal)
{
    if (entry == CBT_SCULPT_NO_PAGE)
        return 0.0;
    uint lstep = 1u << CBT_SculptEntryLevel(entry);
    uint jmax = 127u * lstep;
    float su = tLocal * float(lstep);
    uint k0 = min(uint(su), jmax);
    uint k1 = min(k0 + 1u, jmax);
    float a = CBT_SculptFineTexelSurf(entry, k0, jy);
    float b = CBT_SculptFineTexelSurf(entry, k1, jy);
    return mix(a, b, su - float(k0));
}

// Paged bilinear read of the sculpt store for a WORLD DIRECTION (dir need not be unit). Sampling by
// direction (dominant-face pick + face-local UV) is what makes the sculpt gradient seam-free: two
// pixels straddling a cube edge sample the SAME directions, and a dab wrote both bands identically
// along a shared column. Level-aware since S4 — the same four-case structure as the compute twin
// (cbt_layout.glsl CBT_SampleSphereSculpt) and SphereSculptPaging.h SampleSculptFaceUVWith:
// level-0 fast path / interior fine bilinear / seam edge-function lerp / corner. Metres.
float CBT_SampleSculptDirSurf(vec3 dir, CBTSculptGeom g)
{
    if (g.PoolPageCount == 0u || g.VirtualDim < 2u)
        return 0.0;
    uint face;
    vec2 uv;
    CBT_WorldDirToFaceUV(dir, face, uv);
    float dim = float(g.VirtualDim);
    vec2 t = clamp(uv, 0.0, 1.0) * (dim - 1.0);
    uint x0 = uint(floor(t.x));
    uint y0 = uint(floor(t.y));
    uint x1 = min(x0 + 1u, g.VirtualDim - 1u);
    uint y1 = min(y0 + 1u, g.VirtualDim - 1u);
    float fx = t.x - float(x0);
    float fy = t.y - float(y0);
    uint px0 = x0 / CBT_SCULPT_PAGE_DIM;
    uint py0 = y0 / CBT_SCULPT_PAGE_DIM;
    uint px1 = x1 / CBT_SCULPT_PAGE_DIM;
    uint py1 = y1 / CBT_SCULPT_PAGE_DIM;
    uint e00 = gCbtSculptTable.Table[CBT_SculptTableEntry(face, px0, py0, g.Cap)];
    uint e10 = gCbtSculptTable.Table[CBT_SculptTableEntry(face, px1, py0, g.Cap)];
    uint e01 = gCbtSculptTable.Table[CBT_SculptTableEntry(face, px0, py1, g.Cap)];
    uint e11 = gCbtSculptTable.Table[CBT_SculptTableEntry(face, px1, py1, g.Cap)];
    uint lv00 = e00 == CBT_SCULPT_NO_PAGE ? 0u : CBT_SculptEntryLevel(e00);
    uint lv10 = e10 == CBT_SCULPT_NO_PAGE ? 0u : CBT_SculptEntryLevel(e10);
    uint lv01 = e01 == CBT_SCULPT_NO_PAGE ? 0u : CBT_SculptEntryLevel(e01);
    uint lv11 = e11 == CBT_SCULPT_NO_PAGE ? 0u : CBT_SculptEntryLevel(e11);
    if ((lv00 | lv10 | lv01 | lv11) == 0u)
    {
        float s00 = e00 == CBT_SCULPT_NO_PAGE
                        ? 0.0
                        : CBT_SculptFineTexelSurf(e00, x0 - px0 * CBT_SCULPT_PAGE_DIM,
                                                  y0 - py0 * CBT_SCULPT_PAGE_DIM);
        float s10 = e10 == CBT_SCULPT_NO_PAGE
                        ? 0.0
                        : CBT_SculptFineTexelSurf(e10, x1 - px1 * CBT_SCULPT_PAGE_DIM,
                                                  y0 - py0 * CBT_SCULPT_PAGE_DIM);
        float s01 = e01 == CBT_SCULPT_NO_PAGE
                        ? 0.0
                        : CBT_SculptFineTexelSurf(e01, x0 - px0 * CBT_SCULPT_PAGE_DIM,
                                                  y1 - py1 * CBT_SCULPT_PAGE_DIM);
        float s11 = e11 == CBT_SCULPT_NO_PAGE
                        ? 0.0
                        : CBT_SculptFineTexelSurf(e11, x1 - px1 * CBT_SCULPT_PAGE_DIM,
                                                  y1 - py1 * CBT_SCULPT_PAGE_DIM);
        return mix(mix(s00, s10, fx), mix(s01, s11, fx), fy);
    }
    if (px0 == px1 && py0 == py1)
    {
        uint lstep = 1u << lv00;
        uint jmax = 127u * lstep;
        float sx = (t.x - float(px0 * CBT_SCULPT_PAGE_DIM)) * float(lstep);
        float sy = (t.y - float(py0 * CBT_SCULPT_PAGE_DIM)) * float(lstep);
        uint jx0 = min(uint(sx), jmax);
        uint jy0 = min(uint(sy), jmax);
        uint jx1 = min(jx0 + 1u, jmax);
        uint jy1 = min(jy0 + 1u, jmax);
        float gx = sx - float(jx0);
        float gy = sy - float(jy0);
        float s00 = CBT_SculptFineTexelSurf(e00, jx0, jy0);
        float s10 = CBT_SculptFineTexelSurf(e00, jx1, jy0);
        float s01 = CBT_SculptFineTexelSurf(e00, jx0, jy1);
        float s11 = CBT_SculptFineTexelSurf(e00, jx1, jy1);
        return mix(mix(s00, s10, gx), mix(s01, s11, gx), gy);
    }
    if (py0 == py1)
    {
        float tLoc = t.y - float(py0 * CBT_SCULPT_PAGE_DIM);
        float colL = CBT_SculptEdgeColEvalSurf(e00, 127u * (1u << lv00), tLoc);
        float colR = CBT_SculptEdgeColEvalSurf(e10, 0u, tLoc);
        return mix(colL, colR, fx);
    }
    if (px0 == px1)
    {
        float tLoc = t.x - float(px0 * CBT_SCULPT_PAGE_DIM);
        float rowB = CBT_SculptEdgeRowEvalSurf(e00, 127u * (1u << lv00), tLoc);
        float rowT = CBT_SculptEdgeRowEvalSurf(e01, 0u, tLoc);
        return mix(rowB, rowT, fy);
    }
    float c00 = e00 == CBT_SCULPT_NO_PAGE
                    ? 0.0
                    : CBT_SculptFineTexelSurf(e00, 127u * (1u << lv00), 127u * (1u << lv00));
    float c10 = e10 == CBT_SCULPT_NO_PAGE ? 0.0 : CBT_SculptFineTexelSurf(e10, 0u, 127u * (1u << lv10));
    float c01 = e01 == CBT_SCULPT_NO_PAGE ? 0.0 : CBT_SculptFineTexelSurf(e01, 127u * (1u << lv01), 0u);
    float c11 = e11 == CBT_SCULPT_NO_PAGE ? 0.0 : CBT_SculptFineTexelSurf(e11, 0u, 0u);
    return mix(mix(c00, c10, fx), mix(c01, c11, fx), fy);
}

// True when the world direction's sculpt page is allocated — the spatial gate for the per-pixel
// sculpt gradient. Gating on page RESIDENCY (not a height-magnitude threshold) is what removes the
// crease ring around every dab: a dab's outer shoulder has heights below a centimetre but a non-zero
// SLOPE, so the old `abs(centerSculpt) > 1cm` gate dropped the gradient exactly where the surface is
// still tilting and snapped the normal ~5 degrees at that contour. A page covers the whole brush
// footprint (a 128-texel page vs a ~32-texel brush), so wherever the sculpt height OR its slope is
// non-zero the page is resident and the gradient runs; on the unsculpted planet no page is resident
// so the four taps are skipped (the perf gate the height threshold was really standing in for).
uint CBT_SculptDirEntrySurf(vec3 dir, CBTSculptGeom g)
{
    if (g.PoolPageCount == 0u || g.VirtualDim < 2u)
        return CBT_SCULPT_NO_PAGE;
    uint face;
    vec2 uv;
    CBT_WorldDirToFaceUV(dir, face, uv);
    float dim = float(g.VirtualDim);
    vec2 t = clamp(uv, 0.0, 1.0) * (dim - 1.0);
    uint vtx = uint(floor(t.x));
    uint vty = uint(floor(t.y));
    uint pageX = vtx / CBT_SCULPT_PAGE_DIM;
    uint pageY = vty / CBT_SCULPT_PAGE_DIM;
    return gCbtSculptTable.Table[CBT_SculptTableEntry(face, pageX, pageY, g.Cap)];
}

bool CBT_SculptDirHasPage(vec3 dir, CBTSculptGeom g)
{
    return CBT_SculptDirEntrySurf(dir, g) != CBT_SCULPT_NO_PAGE;
}

// ---- Analytic sphere modifiers over the surface params (sculpt shape-accuracy S2/S3) ----
// The fragment twins of the compute helpers (CBT_SphereAnalyticOffsetMasked / CoversMasked in
// cbt_layout.glsl), reading the placement set from the surface params tail: flattens at
// [0, Count), transient brush dabs at [Count, Count+DabCount). Total 0 = no-ops. The fragment
// path keeps the plain per-placement loop (no cell mask — the S3 mask rides the compute frame
// params only; the fragment loop was not the measured CBT.Update driver and its flatten/dab
// evals already open with a one-dot reject).

uint CBT_SphereAnalyticTotalSurf(CBTSurfaceParamsData sp)
{
    return min(sp.SphereAnalyticCount + sp.SphereAnalyticDabCount, CBT_MAX_SPHERE_ANALYTIC);
}

// Sum of the analytic placement offsets at UNIT world direction `dir` (metres). reliefAtDir is
// the closed-form base relief at that direction (flatten cancels it; dabs are additive).
float CBT_SphereAnalyticOffsetSurf(CBTSurfaceParamsData sp, vec3 dir, float reliefAtDir)
{
    uint count = min(sp.SphereAnalyticCount, CBT_MAX_SPHERE_ANALYTIC);
    uint total = CBT_SphereAnalyticTotalSurf(sp);
    float sum = 0.0;
    for (uint i = 0u; i < count; ++i)
        sum += CBT_EvalSphereAnalyticFlatten(sp.SphereAnalytic[i], sp.Radius, dir, reliefAtDir);
    for (uint i = count; i < total; ++i)
        sum += CBT_EvalSphereAnalyticDab(sp.SphereAnalytic[i], sp.Radius, dir);
    return sum;
}

// True when any analytic placement's footprint covers unit `dir` — the sculpt-gradient tap gate's
// analytic extension (an analytic pad/dab has no resident page, but its rim has a real gradient).
bool CBT_SphereAnalyticCoversSurf(CBTSurfaceParamsData sp, vec3 dir)
{
    uint count = min(sp.SphereAnalyticCount, CBT_MAX_SPHERE_ANALYTIC);
    uint total = CBT_SphereAnalyticTotalSurf(sp);
    for (uint i = 0u; i < count; ++i)
        if (CBT_SphereAnalyticFlattenCovers(sp.SphereAnalytic[i], sp.Radius, dir))
            return true;
    for (uint i = count; i < total; ++i)
        if (CBT_SphereAnalyticDabCovers(sp.SphereAnalytic[i], sp.Radius, dir))
            return true;
    return false;
}

// COMPOSED sculpt sample for the gradient stencil taps: store + the analytic set, the fragment twin
// of the CPU chokepoint SampleSphereSculptComposed. The analytic term needs the closed-form base
// relief at the TAP direction (a flatten's offset cancels it, so the finite-difference gradient of
// the composed field flattens the pad's shading exactly like its geometry) — evaluated here only
// when a set exists, so a store-only planet pays no extra relief eval per tap.
float CBT_SampleSculptComposedSurf(CBTSurfaceParamsData sp, CBTSculptGeom g, vec3 d)
{
    float s = CBT_SampleSculptDirSurf(d, g);
    if (CBT_SphereAnalyticTotalSurf(sp) != 0u)
    {
        vec3 ud = normalize(d);
        float reliefAtTap = CBT_PlanetRelief(ud, sp.ReliefAmplitude, sp.ReliefFrequency,
                                             max(sp.ReliefOctaves, 1u));
        s += CBT_SphereAnalyticOffsetSurf(sp, ud, reliefAtTap);
    }
    return s;
}

// A unit tangent to dir (cross with the world axis of smallest component). Any orthonormal
// tangent basis reconstructs the same tangential gradient, so this need not match the CPU
// mirror's exact choice. Mirror of AnyTangent (CBTPlanetShading.h).
vec3 CBT_AnyTangent(vec3 d)
{
    vec3 ad = abs(d);
    vec3 up = (ad.x <= ad.y && ad.x <= ad.z) ? vec3(1.0, 0.0, 0.0)
              : (ad.y <= ad.z)               ? vec3(0.0, 1.0, 0.0)
                                             : vec3(0.0, 0.0, 1.0);
    return normalize(cross(d, up));
}

// Radial slope + altitude material blend for the sphere (plan §planet-shading). "Up" on a
// globe is the RADIAL direction (dir), not world Y — so slope is measured against dir, and
// altitude is metres above the sphere radius R. Weights (grass, rock, dirt, snow) sum to 1
// and are a continuous function of (normal, dir, altitude) -> seam-free across cube edges.
// Mirror of PlanetSlopeAltitudeWeights (CBTPlanetShading.h) — lockstep constants.
const float CBT_SLOPE_ROCK_LO = 0.30;
const float CBT_SLOPE_ROCK_HI = 0.65;
const float CBT_SNOW_LO = 0.45;
const float CBT_SNOW_HI = 0.80;
const float CBT_DIRT_LO = -0.10;
const float CBT_DIRT_HI = 0.40;
vec4 CBT_PlanetSlopeAltitudeWeights(vec3 normalWS, vec3 dir, float altitude, float amplitude)
{
    float slope01 = clamp(1.0 - dot(normalWS, dir), 0.0, 1.0);
    float altNorm = amplitude > 0.0
                        ? clamp(altitude / (CBT_RELIEF_ENVELOPE * amplitude), -1.0, 1.0)
                        : 0.0;
    float rock = smoothstep(CBT_SLOPE_ROCK_LO, CBT_SLOPE_ROCK_HI, slope01);
    float flatW = 1.0 - rock; // 'flat' is a reserved GLSL interpolation keyword
    float snow = flatW * smoothstep(CBT_SNOW_LO, CBT_SNOW_HI, altNorm);
    float belowSnow = flatW - snow;
    float dirtMix = smoothstep(CBT_DIRT_LO, CBT_DIRT_HI, altNorm);
    float grass = belowSnow * (1.0 - dirtMix);
    float dirt = belowSnow * dirtMix;
    return vec4(grass, rock, dirt, snow);
}

// --- Material normal + ORM ----------------------------------------------------------------
// The ALBEDO half of the material resolve — the triplanar context, the hex lattice, the
// untextured value-noise variation, CBT_MaterialUVs and CBT_MaterialAlbedo — lives in
// Includes/terrain_material_albedo.glsl, included at the top, because the grass surface grounds
// blade bases onto it. The normal and ORM taps below stay here: only the ground shades with them.

// One projection's tangent-space normal, from an already-sampled normal-map texel. NormalStrength
// scales the tangent XY only; Z is left as decoded so the vector still leans the way the map says,
// just less far.
//
// THE SIGN OF TANGENT Y IS THE LOAD-BEARING PART. Negating it is self-consistent across all three
// projections, so it renders every authored bump as a dent and nothing about the geometry looks
// wrong — only the lighting is inverted. The convention is the standard_pbr mesh path's: terrain
// and mesh must light an authored bump the same way. CBTProjNormalSignTests.cpp pins it.
//
// This is split out of the tap below so the host tests can EXECUTE it: the texture sample stays
// outside the block, which is what makes the sign checkable off-GPU
// (CBTProjNormalSign.*, Engine/Modules/CBTTerrain/Tests). Written swizzle-free with `f`-suffixed
// literals for the same reason (GlslShim.h). Change the sign here and that suite reds; change it
// and the A/B record has to be re-run.
// GE_SHARED_PROJ_NORMAL_FROM_SAMPLE_BEGIN
vec3 CBT_ProjNormalFromSample(vec3 sampled, float normalStrength)
{
    vec3 tn = GE_DecodeTangentNormal(vec4(sampled, 1.0f));
    return vec3(tn.x * normalStrength, tn.y * normalStrength, tn.z);
}
// GE_SHARED_PROJ_NORMAL_FROM_SAMPLE_END

// One projection's tangent-space normal, ready for the blend below.
vec3 CBT_DecodeProjNormal(uint layerOrd, TerrainMaterialRecordData mat, vec2 uv,
                          vec2 ddx, vec2 ddy)
{
    return CBT_ProjNormalFromSample(
        CBT_ProjTap(CBT_LAYER_SLOT(layerOrd, 1u), mat.NormalTex, mat.HexRotStrength, uv, ddx, ddy),
        mat.NormalStrength);
}

// A material's detail normal, in world space, composed onto the surface normal the terrain already
// has (baked normalmap / atlas tap for a heightfield, analytic for a sphere).
//
// WHITEOUT BLEND, the triplanar_pbr.glsl precedent: fold the base normal into each projection's
// tangent XY, force the tangent Z along that projection's own axis, then swizzle each result into
// world orientation and triblend. No vertex tangent is involved anywhere — the tangent frame IS the
// projection axis pair, which is what lets this run on CBT's tangent-less terrain mesh.
//
// THE SWIZZLES ARE NOT triplanar_pbr's. That shader projects the X-facing plane as pos.zy; this one
// projects it as pos.yz (CBT_MaterialUVs, matching the albedo taps). So for the X projection the
// tangent U axis is world Y and V is world Z, giving .zxy — copying triplanar_pbr's .zyx would swap
// this projection's world Y and Z contributions and tilt cliff detail the wrong way.
//
// NormalStrength scales the tangent XY before the fold: 0 collapses to exactly the base normal, 1
// is the map unmodified. Z is always reconstructed from XY (GE_DecodeTangentNormal), which is what
// makes a BC5 two-channel upload and an RGB8 one decode identically.
vec3 CBT_MaterialNormal(uint layerOrd, TerrainMaterialRecordData mat, CBTTriplanarCtx tc,
                        CBTMaterialUV uv, vec3 baseNormalWS)
{
    vec3 n = baseNormalWS;
    // Planar: one tap at the footprint UV, whose U runs along world +X and V along world -Z (row 0
    // is the north edge; see CBTMaterialUV::Planar). That is the Y-facing projection's frame with V
    // reversed, so tangent V is negated before the same fold and swizzle; keeping +t.y would light
    // every Planar bump from the south. The normal map stays registered with its albedo.
    if (CBT_MatIsPlanar(mat))
    {
        vec3 t = CBT_ProjNormalFromSample(
            CBT_PlanarTap(CBT_LAYER_SLOT(layerOrd, 1u), mat.NormalTex, mat.HexRotStrength, uv),
            mat.NormalStrength);
        return vec3(vec2(t.x, -t.y) + n.xz, abs(t.z) * n.y).xzy;
    }
    vec3 w = tc.weights;
    vec3 acc = vec3(0.0);
    if (w.z > 0.0) // Z-facing: U = world X, V = world Y, axis = world Z
    {
        vec3 t = CBT_DecodeProjNormal(layerOrd, mat, uv.XY, uv.dXYdx, uv.dXYdy);
        acc += w.z * vec3(t.xy + n.xy, abs(t.z) * n.z).xyz;
    }
    if (w.y > 0.0) // Y-facing: U = world X, V = world Z, axis = world Y
    {
        vec3 t = CBT_DecodeProjNormal(layerOrd, mat, uv.XZ, uv.dXZdx, uv.dXZdy);
        acc += w.y * vec3(t.xy + n.xz, abs(t.z) * n.y).xzy;
    }
    if (w.x > 0.0) // X-facing: U = world Y, V = world Z, axis = world X
    {
        vec3 t = CBT_DecodeProjNormal(layerOrd, mat, uv.YZ, uv.dYZdx, uv.dYZdy);
        acc += w.x * vec3(t.xy + n.yz, abs(t.z) * n.x).zxy;
    }
    return acc;
}

// A material's ORM tap (R = ambient occlusion, G = roughness, B = metallic), triblended on the same
// UVs and with the same hex opt-in as its albedo so the maps stay registered with each other. The
// caller applies the paired scalars — this returns the raw sampled channels.
vec3 CBT_MaterialOrm(uint layerOrd, TerrainMaterialRecordData mat, CBTTriplanarCtx tc,
                     CBTMaterialUV uv)
{
    const uint slot = CBT_LAYER_SLOT(layerOrd, 2u); // 2 == orm
    if (CBT_MatIsPlanar(mat))
        return CBT_PlanarTap(slot, mat.OrmTex, mat.HexRotStrength, uv);
    vec3 w = tc.weights;
    vec3 orm = vec3(0.0);
    if (w.z > 0.0)
        orm += w.z * CBT_ProjTap(slot, mat.OrmTex, mat.HexRotStrength, uv.XY, uv.dXYdx, uv.dXYdy);
    if (w.y > 0.0)
        orm += w.y * CBT_ProjTap(slot, mat.OrmTex, mat.HexRotStrength, uv.XZ, uv.dXZdx, uv.dXZdy);
    if (w.x > 0.0)
        orm += w.x * CBT_ProjTap(slot, mat.OrmTex, mat.HexRotStrength, uv.YZ, uv.dYZdx, uv.dYZdy);
    return orm;
}

// --- Contribution selection --------------------------------------------------------------------
// The resolve is SHARED with the grass surface (Includes/terrain_blend_resolve.glsl): a blade's
// base tint is the ground colour under it, so both surfaces fold and rank one splat identically.
//
// Included HERE rather than beside the other includes at the top of the file because it reads
// CBT_MAX_BLEND_MATERIALS, defined above — the preprocessor has no other way to hand a width to an
// include, so the define must precede this line.
//
// It declares CBT_FoldDuplicateRoles, CBT_SelectTopWeights and CBT_ResolveBlendWeights. Those are
// the functions ExtractShaderBlock.cmake lifts out of that file for CBTMaterialBlendTests to compile
// as C++, so the tests execute them exactly as they ship here.
#include "Includes/terrain_blend_resolve.glsl"

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();

#ifdef GE_TERRAIN_SHADOW_RECEIVER_DECLARED
    // The terrain receives its own shadow on the field at its XZ, not on its facets.
    ge_ShadowReceiverOnTerrain = 1.0;
#endif

    // CBT single-terrain: the first (and only) params entry, the same terrain
    // BuildFrameParams selected for the height/normal source (plan §8 C4).
    TerrainParamsEntry tp = terrains[0];

    // Per-pixel world normal. custom0.w carries a DOMAIN-DISAMBIGUATED value from
    // VertexEval: a bindless normalmap index (>= 0) for planar terrain, or the C7 sphere
    // sentinel -1. Branch on the sentinel FIRST so a planar terrain that transiently lacks
    // a normalmap keeps the flat (0,1,0) fallback (correct at any world Y) rather than the
    // planet's outward-from-origin derivative normal (which would invert lighting below Y=0).
    vec3 normalWS;
    // Total surface height above the planet radius (relief + sculpt) at this pixel, evaluated
    // ANALYTICALLY in the sphere branch below. The splat's altitude band reads it instead of
    // length(positionWS) - R (the interpolated chord, blurred to the coarse triangle) so sculpted
    // mesas/pits get crisp, tessellation-independent rock/snow/grass bands (plan §planet-shading splat).
    float sphereAltitude = 0.0;
    // World-space pixel footprint (m), evaluated in UNIFORM control flow (the domain branch below is
    // uniform per draw) so the per-layer variation can Nyquist-fade without an fwidth() in the divergent
    // layer loop. Sphere: pixel arc * radius (fwidth(dir) is precision-safe at planetary magnitude,
    // unlike derivatives of the huge world position). 0 => no fade (the pre-slice full-variation look).
    float pixelFootprintWS = 0.0;
    const bool isSphere = sIn.custom0.w < -0.5;
#if defined(GE_COMPAT_PROFILE)
    // BOTH footprints, before the branch. `isSphere` varies per fragment and a static uniformity
    // analysis cannot see through it, so a derivative taken inside either arm is non-uniform
    // control flow — which WGSL rejects outright rather than leaving undefined. Taking both costs
    // one extra fwidth on the arm that discards it. dir is unit, so its derivative is
    // precision-safe at any planet radius, unlike a derivative of the huge world position.
    const float pixelArcUnit = length(fwidth(normalize(sIn.positionWS)));
    const float planarFootprintWS = length(fwidth(sIn.positionWS));
#endif
    if (isSphere)
    {
        // Spherical planet (plan §planet-shading): per-pixel ANALYTIC normal. The surface is
        // dir*(R + h(dir)) with h = multi-octave relief (+ the editable sculpt layer); its
        // outward normal is normalize(dir - gradTangential(h)/(R+h)). The relief gradient is
        // closed-form (CBT_PlanetReliefGradient); the sculpt gradient is a central finite
        // difference of the direction-sampled sculpt height along an orthonormal tangent frame
        // (seam-free — sampling by direction agrees across cube faces). This kills the facet
        // look the old screen-space-derivative normal produced, and reveals the relief/sculpt
        // detail between vertices. Falls back to the pure radial normal where h is smooth.
        CBTSurfaceParamsData sp = gCbtSurf.Params;
        vec3 dir = normalize(sIn.positionWS);
#if defined(GE_COMPAT_PROFILE)
        // Angular footprint of this pixel on the unit sphere, taken above the branch.
        float pixelArc = pixelArcUnit;
#else
        // Angular footprint of this pixel on the unit sphere. dir is unit, so fwidth(dir) is
        // precision-safe at any planet radius (unlike derivatives of the huge world position).
        // Evaluated here in the sphere branch's uniform control flow (every CBT sphere fragment
        // takes this branch), so the derivative is well-defined before the divergent sculpt gate.
        float pixelArc = length(fwidth(dir));
#endif
        pixelFootprintWS = pixelArc * sp.Radius; // arc (rad) * radius (m) = surface footprint (m)
        uint oct = max(sp.ReliefOctaves, 1u);
        float h = CBT_PlanetRelief(dir, sp.ReliefAmplitude, sp.ReliefFrequency, oct);
        vec3 g3 = CBT_PlanetReliefGradient(dir, sp.ReliefAmplitude, sp.ReliefFrequency, oct);
        vec3 gradTangential = g3 - dot(g3, dir) * dir;
        if (sp.SphereSculptEnabled != 0u)
        {
            // Sample the sculpt height at the pixel once (needed for the R+h scale + the splat
            // altitude). The 4 gradient taps run wherever a sculpt PAGE is resident — the whole
            // brush footprint including its sub-centimetre shoulder — so the tangential gradient is
            // continuous across a dab (no crease ring); on the unsculpted planet no page is resident
            // so the taps are skipped (the perf gate the old height threshold stood in for).
            CBTSculptGeom sg = CBT_SurfSculptGeom(sp);
            float centerSculpt = CBT_SampleSculptDirSurf(dir, sg);
            // Analytic sphere modifiers (S2/S3): compose the closed-form set (flattens + transient
            // brush dabs) at the pixel, reusing the relief already evaluated at this dir (h still
            // holds the pure closed form here) — the same composition VertexEval displaced the
            // geometry with, so the splat altitude and the R+h normal scale agree with the mesh.
            // Total 0 (dark-ship) adds no float op.
            if (CBT_SphereAnalyticTotalSurf(sp) != 0u)
                centerSculpt += CBT_SphereAnalyticOffsetSurf(sp, dir, h);
            h += centerSculpt;
            uint centerEntry = CBT_SculptDirEntrySurf(dir, sg);
            if (centerEntry != CBT_SCULPT_NO_PAGE ||
                (CBT_SphereAnalyticTotalSurf(sp) != 0u && CBT_SphereAnalyticCoversSurf(sp, dir)))
            {
                vec3 t1 = CBT_AnyTangent(dir);
                vec3 t2 = cross(dir, t1);
                // Resolution-appropriate finite-difference step (crease-quality normal fix). One
                // virtual texel of arc under-resolves a near-vertical wall: many pixels map to few
                // sculpt texels, so a 1-texel central difference reads the piecewise-constant
                // bilinear-cell gradient and bands into VERTICAL COLUMNS while under-tilting the wall
                // (the flat look). Widening the stencil to the pixel's footprint averages the
                // gradient across the cells the pixel actually covers -> smooth + correctly tilted.
                // Reduces to one texel where the wall is well-resolved (footprint <= 1 texel).
                // S4: the texel is the PAGE's texel — an escalated page's fine grid shrinks the
                // step by 2^level (same physical MAX_STEP arc), otherwise a base-texel stencil
                // alias-shades sub-base-texel content into a four-dot halo along the tap axes.
                // Level 0 reduces to the exact pre-S4 arithmetic.
                uint lvl = centerEntry != CBT_SCULPT_NO_PAGE ? CBT_SculptEntryLevel(centerEntry) : 0u;
                float texelArc = CBT_SCULPT_HALF_PI / float(max(sg.VirtualDim, 1u) << lvl);
                float stepTexels = clamp(pixelArc / max(texelArc, 1e-9), 1.0,
                                         CBT_SCULPT_MAX_STEP_TEXELS * float(1u << lvl));
                float sculptStep = texelArc * stepTexels;
                // The taps sample the COMPOSED field (store + analytic, CBT_SampleSculptComposedSurf)
                // so a flatten's gradient cancels the relief gradient on the pad interior (flat
                // shading on the flat pad) and tilts the rim exactly where the geometry does. With
                // no analytic set the composed tap IS the plain store tap — byte-identical.
                float g1 = (CBT_SampleSculptComposedSurf(sp, sg, dir + t1 * sculptStep) -
                            CBT_SampleSculptComposedSurf(sp, sg, dir - t1 * sculptStep)) /
                           (2.0 * sculptStep);
                float g2 = (CBT_SampleSculptComposedSurf(sp, sg, dir + t2 * sculptStep) -
                            CBT_SampleSculptComposedSurf(sp, sg, dir - t2 * sculptStep)) /
                           (2.0 * sculptStep);
                gradTangential += g1 * t1 + g2 * t2;
            }
        }
        sphereAltitude = h; // relief + sculpt, per-pixel analytic — the splat altitude band reads this
        normalWS = normalize(dir - gradTangential / (sp.Radius + h));
    }
    else
    {
        // Planar terrain: the SAME pre-baked normalmap the CDLOD surface sampled
        // (R16G16_FLOAT world nx/nz, ny reconstructed) — smooth + crack-free. Flat up
        // when no normalmap is bound yet (index 0).
#if defined(GE_COMPAT_PROFILE)
        pixelFootprintWS = planarFootprintWS;
#else
        pixelFootprintWS = length(fwidth(sIn.positionWS)); // planar posWS is not at planetary scale
#endif
        uint normalmapIdx = uint(sIn.custom0.w);
        CBTSurfaceParamsData spN = gCbtSurf.Params;
        if (CBT_HAS_NORMALMAP(tp, normalmapIdx))
        {
            vec2 nXZ = CBT_SAMPLE_NORMALMAP(normalmapIdx, sIn.uv0).rg;
            float ny = sqrt(max(1.0 - dot(nXZ, nXZ), 0.0));
            normalWS = normalize(vec3(nXZ.x, ny, nXZ.y));
        }
        else if (spN.AtlasBacked != 0u)
        {
            // Phase E resident-window atlas: resolve terrain UV -> tile -> slot UV per-pixel and tap the
            // R16G16F normal atlas (world nx/nz, ny reconstructed) — smooth + apron-seamless, matching
            // the unified normalmap. Out of the resident window the coarse normal field (derived from the
            // coarse heights, so it matches the coarse geometry) keeps distant shading continuous. This
            // DELETES the geometric screen-space-derivative stand-in — the #490 faceting class that had
            // returned for atlas terrains — with a real per-pixel normal.
            AtlasResolve r = CBT_SurfResolve(sIn.uv0, spN);
            // Coarse->slot upgrade crossfade (residency pop killer): a newly-resident tile fades its
            // slot normal in from the coarse-field normal it was rendering a frame earlier, instead of
            // snapping (the flat/dark shading patch that read as a hard tile-aligned pop). r.Fade is 1
            // for a settled resident tile (pure slot) and 1 for an out-of-window tile (pure coarse), so
            // both steady states pay a SINGLE tap; only a tile mid-fade takes the second tap + the mix.
            const bool haveSlot = r.Resident && CBT_HAS_ATLAS_MAP(spN.AtlasNormalBindless);
            const bool haveCoarse = CBT_HAS_ATLAS_MAP(spN.AtlasNormalCoarseBindless);
            vec2 nSlot = haveSlot ? CBT_SAMPLE_ATLAS_NORMAL(spN.AtlasNormalBindless, r.SlotUV).rg
                                  : vec2(0.0);
            vec2 nXZ;
            if (haveSlot && haveCoarse && r.Fade < 1.0)
            {
                vec2 nCoarse = CBT_SAMPLE_ATLAS_NORMAL_COARSE(spN.AtlasNormalCoarseBindless,
                                               CBT_SurfCoarseUV(sIn.uv0, spN.AtlasCoarseDim)).rg;
                nXZ = mix(nCoarse, nSlot, r.Fade);
            }
            else if (haveSlot)
                nXZ = nSlot;
            else if (haveCoarse)
                nXZ = CBT_SAMPLE_ATLAS_NORMAL_COARSE(spN.AtlasNormalCoarseBindless,
                                              CBT_SurfCoarseUV(sIn.uv0, spN.AtlasCoarseDim)).rg;
            else
                nXZ = vec2(0.0); // no atlas normal bound yet -> flat up (first frames only)
            float ny = sqrt(max(1.0 - dot(nXZ, nXZ), 0.0));
            normalWS = normalize(vec3(nXZ.x, ny, nXZ.y));
        }
        else
        {
            normalWS = vec3(0.0, 1.0, 0.0);
        }
    }

    // Splat weights. Three cases:
    //   * SPHERE (plan §planet-shading): a principled RADIAL slope + altitude material blend.
    //     The old path took a hard world-Y slope fallback that read "up" as world +Y, which
    //     is wrong on a globe (a whole cube face reads as cliff -> all rock); this measures
    //     slope against the radial direction and layers grass/dirt/snow by altitude. A planar
    //     splatmap has no meaning wrapped onto a sphere, so per-face sphere splat painting is
    //     a documented v2 — this blend is the sane, seam-free default at any planet scale.
    //   * PLANAR painted: sample the shared RGBA8 splatmap at the terrain UV (CDLOD parity).
    //   * PLANAR unpainted: the world-Y slope fallback (correct for a heightfield terrain).
    vec4 weights = vec4(0.0);
    if (isSphere)
    {
        CBTSurfaceParamsData sp = gCbtSurf.Params;
        vec3 dir = normalize(sIn.positionWS);
        // Altitude is the per-pixel analytic height (relief + sculpt) the normal branch evaluated,
        // NOT length(positionWS) - R: the latter is the interpolated chord, so a coarse triangle
        // over a relief/sculpt bump blurs the altitude bands to the facet. The analytic height tracks
        // the true surface between vertices, so rock/snow/grass bands stay crisp on sculpted mesas.
        weights = CBT_PlanetSlopeAltitudeWeights(normalWS, dir, sphereAltitude, sp.ReliefAmplitude);
    }
    else if (CBT_HAS_SPLATMAP(tp))
    {
        weights = CBT_SAMPLE_SPLATMAP(tp.SplatmapBindless, sIn.uv0);
    }
    else if (gCbtSurf.Params.AtlasBacked != 0u)
    {
        // Resident-window atlas splat: resolve per-pixel and tap the RGBA8 splat at the slot UV, or the
        // coarse splat field out of window (continuous with resident splat at the frontier). This DELETES
        // the slope-splat stand-in for atlas terrains — painted/procedural splat now reads at full LOD.
        CBTSurfaceParamsData spS = gCbtSurf.Params;
        AtlasResolve r = CBT_SurfResolve(sIn.uv0, spS);
        // Crossfade the slot splat in from the coarse splat field on upgrade (same pop killer as the
        // normal above), so the texture-detail seam where full-res meets coarse arrives gradually. The
        // mix runs in weight space; the wSum normalization below keeps the blended weights valid.
        const bool haveSlotS = r.Resident && CBT_HAS_ATLAS_MAP(spS.AtlasSplatBindless);
        const bool haveCoarseS = CBT_HAS_ATLAS_MAP(spS.AtlasSplatCoarseBindless);
        vec4 wSlot = haveSlotS ? CBT_SAMPLE_ATLAS_SPLAT(spS.AtlasSplatBindless, r.SlotUV)
                               : vec4(1.0, 0.0, 0.0, 0.0);
        if (haveSlotS && haveCoarseS && r.Fade < 1.0)
        {
            vec4 wCoarse = CBT_SAMPLE_ATLAS_SPLAT_COARSE(spS.AtlasSplatCoarseBindless,
                                   CBT_SurfCoarseUV(sIn.uv0, spS.AtlasCoarseDim));
            weights = mix(wCoarse, wSlot, r.Fade);
        }
        else if (haveSlotS)
            weights = wSlot;
        else if (haveCoarseS)
            weights = CBT_SAMPLE_ATLAS_SPLAT_COARSE(spS.AtlasSplatCoarseBindless,
                              CBT_SurfCoarseUV(sIn.uv0, spS.AtlasCoarseDim));
        else
            weights = vec4(1.0, 0.0, 0.0, 0.0); // no atlas splat bound yet (first frames only)
    }
    else
    {
        // PLANAR, no splat bound: channel 0, the same answer the wSum guard below
        // gives an all-zero splat. A terrain whose surface rules have not baked has
        // no material assigned, and that is what it reads as.
        //
        // Do NOT derive weights from slope or altitude here. Authored surface rules own
        // material placement, and a stand-in derived at SHADE time cannot see the rows,
        // the volumes that scope them, or the committed height range they normalize
        // against — so it necessarily disagrees with the bake it stands in for.
        weights = vec4(1.0, 0.0, 0.0, 0.0);
    }

    float wSum = weights.x + weights.y + weights.z + weights.w;
    if (wSum > 0.0)
        weights /= wSum;
    else
        weights.x = 1.0;

    // The channel -> material indirection (SF2 telemetry seam): the splat carries four channel
    // weights and this is where a weight stops meaning "layer i" and starts meaning "whatever
    // material role i points at". Both domains take it — the sphere's slope+altitude blend and the
    // planar splat write the same four channels — so a material authored once shades a planet and
    // a heightfield identically. When the splat carries slot IDs directly, only this expression
    // changes; everything below it already reads a record.
    //
    // Resolved BEFORE the blend so the resolve can compare records: from here on a weight belongs
    // to a material, not to a channel.
    uint roles[4] = uint[4](tp.LayerRole[0], tp.LayerRole[1], tp.LayerRole[2],
                            tp.LayerRole[3]);
    weights = CBT_ResolveBlendWeights(weights, roles);

    float uvTiling = CBT_MaterialUVTiling(tp.MaterialTiling);
    // Precision-safe UVs + projection weights shared by every textured material (CBTTriplanarCtx).
    CBTTriplanarCtx tpc = CBT_BuildTriplanarCtx(sIn.positionRelWS, normalWS, sIn.uv0);
    vec3 baseColor = vec3(0.0);
    vec3 blendedNormal = vec3(0.0);
    float roughness = 0.0;
    float metallic = 0.0;
    float ao = 0.0;
    bool anyNormalMap = false;
    for (int i = 0; i < 4; ++i)
    {
        // Zeroed entries are the floored and folded-away ones; the survivors carry the whole blend.
        float w = weights[i];
        if (w <= 0.0)
            continue;
        TerrainMaterialRecordData mat = TerrainMaterials[roles[i]];
        // A planet's uv0 is a cube-face UV, so a footprint projection would repeat the image per
        // face. Planar is a heightfield-terrain projection: on a sphere the material shades as
        // Triplanar. isSphere is uniform per draw.
        if (isSphere)
            mat.Flags &= ~CBT_MATFLAG_PLANAR;
        CBTMaterialUV uv = CBT_MaterialUVs(mat, tpc, uvTiling);
        // The blend ordinal, not the role: it is what selects a named binding on a profile with
        // no descriptor indexing (CBT_LAYER_SLOT), and it is ignored on the bindless one.
        const uint layerOrd = uint(i);
        baseColor +=
            CBT_MaterialAlbedo(layerOrd, mat, tpc, uv, sIn.positionWS, pixelFootprintWS) * w;

        // Unbound ORM reads as (1, 1, 0), which leaves the paired scalars AS the value — the
        // record's texture-or-value contract. Bound, the scalars multiply the sampled channels.
        // Metallic has no scalar and no meaning without the author's opt-in, so an ORM whose blue
        // is padding stays dielectric.
        //
        // Roughness and metallic reproduce the pre-ORM shading exactly when nothing is bound.
        // Ambient occlusion does NOT: it was emitted as a constant 1.0 and mat.Ao was never read,
        // so a record authoring Ao < 1 darkens where it previously did not. That is the field
        // becoming live, not a regression.
        vec3 orm =
            CBT_MAT_HAS_ORM(mat) ? CBT_MaterialOrm(layerOrd, mat, tpc, uv) : vec3(1.0, 1.0, 0.0);
        roughness += mat.Roughness * orm.g * w;
        ao += mat.Ao * orm.r * w;
        if ((mat.Flags & CBT_MATFLAG_ORM_HAS_METALLIC) != 0u)
            metallic += orm.b * w;

        // A material with no normal map contributes the terrain's own normal, so a blend of
        // textured and untextured materials interpolates toward the surface rather than toward zero.
        blendedNormal +=
            w * (CBT_MAT_HAS_NORMAL(mat) ? CBT_MaterialNormal(layerOrd, mat, tpc, uv, normalWS)
                                         : normalWS);
        anyNormalMap = anyNormalMap || CBT_MAT_HAS_NORMAL(mat);
    }

    o.baseColor = baseColor;
    // Renormalize only when a map actually contributed. With none bound the accumulation is just
    // normalWS scaled by the weight sum, so leaving it alone keeps every untextured terrain
    // byte-identical to the pre-Phase-B path rather than routing it through a normalize().
    o.normalWS = anyNormalMap ? normalize(blendedNormal) : normalWS;
    o.metallic = metallic;
    o.roughness = roughness;
    o.opacity = 1.0;
    o.ao = ao;
    o.emissive = vec3(0.0);

    // Debug visualization (ledger cleanup): tint each CBT bisector triangle by its slot so the
    // live LEB bisection structure (triangle density, splits) reads directly on the surface.
    // custom0.x carries the bisector slot (flat -> constant per triangle), so adjacent triangles
    // get distinct hues and every edge is visible. Albedo-based (keeps the lit normal) so it
    // reads at any exposure. Off (the default) leaves shading untouched — planar/planet render
    // exactly as normal. Both domains reach this: DebugMode is uploaded for planar and sphere.
    uint debugMode = gCbtSurf.Params.DebugMode;
    if (debugMode == CBT_DEBUG_FACETS)
    {
        uint slot = uint(max(sIn.custom0.x, 0.0));
        vec3 facet = fract(vec3(float(slot) * 0.6180339887,
                                float(slot) * 0.4501836112,
                                float(slot) * 0.7548776662));
        o.baseColor = 0.15 + 0.85 * facet; // lift off near-black so every facet reads
        o.metallic = 0.0;
        o.roughness = 1.0;
        o.emissive = vec3(0.0);
    }
    else if (debugMode == CBT_DEBUG_ATLAS_SLOTS && gCbtSurf.Params.AtlasBacked != 0u)
    {
        // Atlas-resolve visualization (quality-sweep slice 1): run the SAME per-pixel resolve the
        // splat/normal taps use and tint by the resolved slot index (distinct hue per resident slot),
        // grey where the tile is out-of-window (coarse fallback). This is the manual check for the
        // fragment resolve path (offset-bound gAtlasRows + CBT_SurfResolve): slot boundaries and the
        // residency frontier read directly, and a set-2 binding-7 offset regression turns the terrain
        // a flat single colour. Albedo-based so it reads at any exposure.
        AtlasResolve r = CBT_SurfResolve(sIn.uv0, gCbtSurf.Params);
        if (r.Resident)
        {
            uint col = uint(r.SlotUV.x * float(gCbtSurf.Params.AtlasSlotsPerRow));
            uint rw = uint(r.SlotUV.y * float(gCbtSurf.Params.AtlasSlotsPerRow));
            uint sid = rw * gCbtSurf.Params.AtlasSlotsPerRow + col;
            o.baseColor = 0.15 + 0.85 * fract(vec3(float(sid) * 0.6180339887,
                                                   float(sid) * 0.4501836112,
                                                   float(sid) * 0.7548776662));
        }
        else
        {
            o.baseColor = vec3(0.4); // out-of-window: coarse fallback
        }
        o.metallic = 0.0;
        o.roughness = 1.0;
        o.emissive = vec3(0.0);
    }
    return o;
}
