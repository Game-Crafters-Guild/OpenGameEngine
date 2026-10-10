// The ground splat resolve, shared by grass PLACEMENT and grass SHADING.
//
// One resolve, two stages: terrain_grass_place.comp decides where a blade may stand from it, and
// terrain_grass_surface.glsl takes the blade's ground colour from it. A blade whose placement mask
// came from the atlas splat and whose colour came from channel 0 is the defect this file exists to
// make impossible — so neither stage samples the splat itself, both call GrassAtlas_ResolveSplat.
//
// The includer supplies the descriptor slots, because the two stages sit in different sets:
//   #define GRASS_ATLAS_SET            <set>
//   #define GRASS_ATLAS_PARAMS_BINDING <binding of the params UBO>
//   #define GRASS_ATLAS_ROWS_BINDING   <binding of the rows SSBO>
// Only the SLOTS vary. The block LAYOUTS do not: the params block is std140 and the rows block
// std430 in both stages, so TerrainGrassRenderFeature::GrassAtlasParamsGPU mirrors one layout
// rather than one per stage. Both blocks carry an INSTANCE name, so reflection reports them as
// `Atlas` and `gAtlasRows` — the names the graphics-side binder must be given (the compute side
// binds by explicit index and never sees them).
//
// Include AFTER Includes/bindless_textures.glsl (the taps below use GE_BTEX) and AFTER
// cbt_atlas.glsl — which the INCLUDER pulls in, because the spelling that resolves differs by
// stage and this file cannot know which stage it is in. The placement compute is built by
// ShaderCompileService with the CBT directory on its include path and says "cbt_atlas.glsl"; the
// surface is built by MaterialBuildService, whose roots are the Assets/Shaders tree, and says
// "CBT/cbt_atlas.glsl". Neither spelling resolves on the other's path, so requiring it here is the
// honest contract — an unmet one is the #error below rather than a confusing not-found.
#ifndef GRASS_ATLAS_SPLAT_GLSL
#define GRASS_ATLAS_SPLAT_GLSL

#if !defined(GRASS_ATLAS_SET) || !defined(GRASS_ATLAS_PARAMS_BINDING) \
    || !defined(GRASS_ATLAS_ROWS_BINDING)
#error "grass_atlas_splat.glsl needs GRASS_ATLAS_SET / _PARAMS_BINDING / _ROWS_BINDING defined first"
#endif
#ifndef CBT_ATLAS_GLSL
#error "grass_atlas_splat.glsl needs cbt_atlas.glsl included first (see the note above)"
#endif

layout(std140, set = GRASS_ATLAS_SET, binding = GRASS_ATLAS_PARAMS_BINDING)
uniform GrassAtlasParamsBuffer
{
    uint Enabled;
    uint AtlasDim;
    uint SlotStride;
    uint SlotsPerRow;
    uint TileRes;
    uint TilesPerAxisX;
    uint TilesPerAxisZ;
    uint CoarseDim;
    uint HeightBindless;
    uint HeightCoarseBindless;
    uint NormalBindless;
    uint NormalCoarseBindless;
    uint SplatBindless;
    uint SplatCoarseBindless;
    uint RowCount;
    uint GrassEnabled;
    uint GrassBindless;
    uint GrassCoarseBindless;
    uint _Pad0;
    uint _Pad1;
} Atlas;

layout(std430, set = GRASS_ATLAS_SET, binding = GRASS_ATLAS_ROWS_BINDING)
readonly buffer GrassAtlasRowsBuffer
{
    CBTTileAtlasSlot Rows[];
} gAtlasRows;

// GE_SHARED_GRASS_SPLAT_SOURCE_BEGIN
// The two decisions placement and shading must not each invent: WHERE a point's ground weights come
// from, and how a coarse->slot upgrade is shown while it is in flight. Both are pure functions over
// the residency + binding facts, with no sampler in them, so the host suite executes this exact
// source rather than a mirror that can drift.

// TerrainGPUParams::Flags bit1 (mirror Terrain::kTerrainFlagAtlasBacked): the terrain feeds
// height/normal/splat through the atlas, not a unified bindless heightmap (HeightmapBindless == 0).
const uint kTerrainFlagAtlasBacked = 2u;

// Where the four ground weights at a point must be read from. Four cases, in priority order, and
// the ONE piece of logic placement and shading have to agree on — hence a pure function over the
// residency + binding facts, with no sampler in it, so the host suite executes this exact source.
const uint kGrassSplatSourceNone = 0u;        // nothing bound: channel 0, the unwritten answer
const uint kGrassSplatSourceAtlasSlot = 1u;   // the tile is resident: its full-detail atlas slot
const uint kGrassSplatSourceAtlasCoarse = 2u; // out of window: the terrain-wide coarse splat field
const uint kGrassSplatSourceUnified = 3u;     // non-atlas terrain: the one terrain-sized splatmap

// An atlas-backed terrain resolves through the atlas ONLY while the feature has an atlas bound this
// frame: the per-terrain flag says where the data lives, `atlasEnabled` says whether it arrived.
bool GrassAtlas_IsAtlasBacked(uint terrainFlags, uint atlasEnabled)
{
    return (terrainFlags & kTerrainFlagAtlasBacked) != 0u && atlasEnabled != 0u;
}

uint GrassAtlas_SelectSplatSource(uint terrainFlags, uint atlasEnabled, uint splatmapBindless,
                                  bool atlasResident, uint atlasSplatBindless,
                                  uint atlasSplatCoarseBindless)
{
    if (GrassAtlas_IsAtlasBacked(terrainFlags, atlasEnabled))
    {
        if (atlasResident && atlasSplatBindless != 0u)
            return kGrassSplatSourceAtlasSlot;
        if (atlasSplatCoarseBindless != 0u)
            return kGrassSplatSourceAtlasCoarse;
        // Atlas terrain whose splat has not been bound yet (first frames): NOT the unified
        // splatmap. An atlas terrain's SplatmapBindless is 0 by construction, so falling through
        // to it would only read slot 0 of the bindless table — a real but unrelated texture.
        return kGrassSplatSourceNone;
    }
    if (splatmapBindless != 0u)
        return kGrassSplatSourceUnified;
    return kGrassSplatSourceNone;
}

// How much of the resident SLOT a consumer shows against the COARSE field the tile is upgrading
// from: 1 = the slot outright, the row's Fade while a newly-resident tile is still crossfading in.
//
// Two consumers, two answers, one rule. The ground (cbt_surface.glsl) mixes coarse->slot by the
// row's Fade over kAtlasUpgradeFadeSeconds so a streaming upgrade reads as a fade rather than a
// snap; a blade's GROUND COLOUR has to travel with it, or it arrives at full detail while the
// ground under it is still halfway there. A blade's PLACEMENT must not: its mask is a refusal, not
// a colour, and a refusal that crossfades would spawn and unspawn blades across the window. So the
// CONSUMER says whether it shows the fade and this decides the rest — the caller never derives the
// weight itself, which is what keeps the two from disagreeing about when a tile is mid-upgrade.
//
// A crossfade needs BOTH endpoints, so a consumer with no slot or no coarse field answers 1: there
// is nothing to fade between, and the settled paths sample exactly as they did before the fade
// existed. `rowFade` is passed through unclamped, exactly as the ground uses it — a row with no
// slot never reaches here (cbt_atlas.glsl forces Fade to 1 for CBT_ATLAS_NO_SLOT), and clamping
// only on this side would be a guard the ground does not have.
float GrassAtlas_SlotUpgradeWeight(bool haveSlot, bool haveCoarse, float rowFade,
                                   bool showUpgradeFade)
{
    if (!showUpgradeFade || !haveSlot || !haveCoarse)
        return 1.0;
    return rowFade;
}
// GE_SHARED_GRASS_SPLAT_SOURCE_END

CBTAtlasParams grassAtlasParams()
{
    CBTAtlasParams ap;
    ap.TileRes = Atlas.TileRes;
    ap.SlotStride = Atlas.SlotStride;
    ap.SlotsPerRow = Atlas.SlotsPerRow;
    ap.AtlasDim = Atlas.AtlasDim;
    ap.TilesPerAxisX = Atlas.TilesPerAxisX;
    ap.TilesPerAxisZ = Atlas.TilesPerAxisZ;
    return ap;
}

AtlasResolve grassAtlasResolve(vec2 uv)
{
    CBTAtlasParams ap = grassAtlasParams();
    ivec2 tile;
    vec2 localUV;
    CBT_AtlasTile(uv, ap, tile, localUV);
    uint tileIndex = CBT_AtlasTileIndex(tile, ap);
    CBTTileAtlasSlot row;
    row.Slot = CBT_ATLAS_NO_SLOT;
    row.Generation = 0u;
    row.LodBias = 0.0;
    row.Fade = 1.0; // default settled; overwritten from gAtlasRows below when the tile is in-bounds
    if (tileIndex < Atlas.RowCount)
        row = gAtlasRows.Rows[tileIndex];
    return CBT_AtlasResolve(uv, ap, row);
}

// Endpoint-exact coarse-field UV (mirror CBT_SurfCoarseUV / SampleGridBilinearTexel): the coarse
// field is a lattice like every terrain texture (CBT_LatticeTextureUV).
vec2 grassCoarseUV(vec2 uv, uint coarseDim)
{
    if (coarseDim < 2u)
        return clamp(uv, 0.0, 1.0);
    return CBT_LatticeTextureUV(uv, vec2(float(coarseDim)));
}

bool grassAtlasBacked(uint terrainFlags)
{
    return GrassAtlas_IsAtlasBacked(terrainFlags, Atlas.Enabled);
}

vec4 grassNormalizeLayerWeights(vec4 weights)
{
    float wSum = weights.x + weights.y + weights.z + weights.w;
    if (wSum > 0.0)
        return weights / wSum;
    return vec4(1.0, 0.0, 0.0, 0.0);
}

// Whether an AUTHORED splat backs this point — the placement mask reads this to tell "grass was
// not selected here" from "nothing has assigned this surface a material yet", which get opposite
// defaults. Shading needs no such distinction: both answer channel 0.
//
// Deliberately NOT GrassAtlas_SelectSplatSource() != None. This asks only whether a full-detail
// splat exists, so an atlas terrain holding nothing but the coarse field answers false and takes
// the loose feathering rather than the hard refusal — the mask semantics the placement rework and
// the TerrainAtlas oracles were fixed against. The two questions differing is why the mask asks
// this one and the colour asks the selection.
bool grassHasSplat(uint terrainFlags, uint splatmapBindless)
{
    return splatmapBindless != 0u
        || (grassAtlasBacked(terrainFlags) && Atlas.SplatBindless != 0u);
}

// The four normalized ground weights at `uv`, from whichever source the selection above names.
//
// `showUpgradeFade` is this consumer's answer for a tile still crossfading into its slot — see
// GrassAtlas_SlotUpgradeWeight. Ground COLOUR shows the fade so it tracks the ground beside it; a
// placement MASK does not.
//
// The fallback is channel 0 — the same answer grassNormalizeLayerWeights gives an all-zero splat,
// so a terrain whose rules have not baked reads identically here and on the ground.
//
// Do NOT re-derive the splat from the heightfield here. The blades must read the SAME answer as the
// ground they stand on, and only the bake knows the authored rows and the committed splat range
// they normalize against; anything derived at shade time would band a different height domain and
// disagree with the ground.
//
// EXPLICIT LOD 0 in both stages, not implicit-LOD `texture()` in the fragment. A compute stage has
// no derivatives to select a mip from, so a shared tap has to name its level — and 0 is the level
// that makes a blade's colour read the same texel its placement mask read. The blade footprint it
// is sampled over is a thin quad at one world position, so implicit selection landed on the top
// mip here anyway; the splat is low-frequency and carries weights, not detail.
// The splat taps as seams. GE_BTEX exists only on a profile that HAS a bindless texture array;
// the compat profile (WebGPU class) has none, so this file would not compile there at all. The
// defaults below are the bindless spelling, and the includer overrides them to named bindings.
// The unified tap takes a terrain UV and reads the splat lattice there (CBT_LatticeTextureUV); the
// atlas taps take a resolved UV.
#ifndef GRASS_SPLAT_TAP_UNIFIED
#define GRASS_SPLAT_TAP_UNIFIED(idx, uv) \
    textureLod(GE_BTEX(idx, GE_TS_CLAMP), \
               CBT_LatticeTextureUV(uv, vec2(textureSize(GE_BTEX(idx, GE_TS_CLAMP), 0))), 0.0)
#endif
#ifndef GRASS_SPLAT_TAP_ATLAS_SLOT
#define GRASS_SPLAT_TAP_ATLAS_SLOT(idx, uv) textureLod(GE_BTEX(idx, GE_TS_CLAMP), uv, 0.0)
#endif
#ifndef GRASS_SPLAT_TAP_ATLAS_COARSE
#define GRASS_SPLAT_TAP_ATLAS_COARSE(idx, uv) textureLod(GE_BTEX(idx, GE_TS_CLAMP), uv, 0.0)
#endif

vec4 GrassAtlas_ResolveSplat(uint terrainFlags, uint splatmapBindless, vec2 uv, bool showUpgradeFade)
{
    // Resolved before the selection because the selection needs residency, and the resolve is the
    // only thing that knows it. On a non-atlas terrain Atlas.RowCount is 0, so the guarded row
    // fetch inside grassAtlasResolve never executes.
    AtlasResolve r = grassAtlasResolve(uv);
    uint source = GrassAtlas_SelectSplatSource(terrainFlags, Atlas.Enabled, splatmapBindless,
                                               r.Resident, Atlas.SplatBindless,
                                               Atlas.SplatCoarseBindless);

    float slotWeight = GrassAtlas_SlotUpgradeWeight(source == kGrassSplatSourceAtlasSlot,
                                                    Atlas.SplatCoarseBindless != 0u, r.Fade,
                                                    showUpgradeFade);
    if (source == kGrassSplatSourceAtlasSlot)
    {
        vec4 weights = GRASS_SPLAT_TAP_ATLAS_SLOT(Atlas.SplatBindless, r.SlotUV);
        // Mid-upgrade only: the RAW taps are mixed and normalized ONCE, which is what the ground
        // does. Mixing two already-normalized weight vectors instead would trace a different path
        // between the same endpoints, so a blade and its ground would agree only at the ends.
        // A settled tile takes the single tap it always did.
        if (slotWeight < 1.0)
            weights = mix(GRASS_SPLAT_TAP_ATLAS_COARSE(Atlas.SplatCoarseBindless,
                                                       grassCoarseUV(uv, Atlas.CoarseDim)),
                          weights, slotWeight);
        return grassNormalizeLayerWeights(weights);
    }
    if (source == kGrassSplatSourceAtlasCoarse)
        return grassNormalizeLayerWeights(
            GRASS_SPLAT_TAP_ATLAS_COARSE(Atlas.SplatCoarseBindless,
                                         grassCoarseUV(uv, Atlas.CoarseDim)));
    if (source == kGrassSplatSourceUnified)
        return grassNormalizeLayerWeights(
            GRASS_SPLAT_TAP_UNIFIED(splatmapBindless, uv));

    return grassNormalizeLayerWeights(vec4(0.0));
}

#endif // GRASS_ATLAS_SPLAT_GLSL
