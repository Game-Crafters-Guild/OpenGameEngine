// Surface shader for procedural terrain grass.
// Color and wind-darkening are adapted from the referenced Three.js meadow
// shader, but the placement mask comes from the terrain splatmap grass layer.

#extension GL_EXT_nonuniform_qualifier : require

layout(location = 3) flat in float vGrassWindStrength;

#if defined(GE_USER_GRASS_A2C)
// The colour pipeline resolves this surface's alpha by alpha-to-coverage, so its depth-only variant
// (the camera-prepass head) writes the same alpha for the same mask (adapter_forward.glsl).
#define GE_SURFACE_ALPHA_TO_COVERAGE
#endif

#include "Includes/bindless_textures.glsl"
#include "TerrainGrass/grass_clump.glsl"

// The resident-window atlas resolve, then the ground splat resolve built on it — the SAME file the
// placement compute runs, so a blade's colour and its placement mask read one answer. Spelled
// "CBT/..." because the surface is compiled by MaterialBuildService, whose include roots are the
// Assets/Shaders tree; the compute, whose path carries the CBT directory itself, says
// "cbt_atlas.glsl".
#include "CBT/cbt_atlas.glsl"
#define GRASS_ATLAS_SET 2
#define GRASS_ATLAS_PARAMS_BINDING 5
#define GRASS_ATLAS_ROWS_BINDING 6

// --- Compat profile (WebGPU class): named bindings, no descriptor indexing --------------------
// WebGPU exposes no binding array, so every texture this surface reads is a NAMED set-2 binding
// and the CPU resolves the same indices to handles when it fills them.
//
// SEPARATE texture + sampler, not combined sampler2D: the cook splits every combined sampler into
// a texture AND a sampler, and WebGPU allows 16 samplers per stage. Six combined here plus the lit
// path's own overran that, and an overrun does not degrade — the pipeline fails to create and
// nothing draws. One shared repeat sampler is what fits; the CLAMP data lookups clamp their UV in
// place instead.
//
// Two ground layers, the same cap and the same reason as cbt_surface: a blade must read the SAME
// ground answer the terrain surface does, so this mirrors that cap rather than choosing its own.
// Slots past the second wrap onto the last bound one — the weights still blend, the extra layer
// just repeats a neighbour's texture.
#if defined(GE_COMPAT_PROFILE)
const uint kGrassCompatLayers = 2u;

layout(set = 2, binding = 20) uniform texture2D gGrassLayer0Albedo;
layout(set = 2, binding = 21) uniform texture2D gGrassLayer1Albedo;
layout(set = 2, binding = 22) uniform texture2D gGrassSplatmap;
layout(set = 2, binding = 23) uniform texture2D gGrassBladeAlbedo;
layout(set = 2, binding = 24) uniform texture2D gGrassBladeAlpha;
layout(set = 2, binding = 25) uniform texture2D gGrassBladeNormal;
layout(set = 2, binding = 26) uniform texture2D gGrassVtxHeightmap;
layout(set = 2, binding = 27) uniform texture2D gGrassVtxNormalmap;
layout(set = 2, binding = 28) uniform sampler gGrassMapSampler;

// GLSL cannot return an opaque sampler from a function, so the slot walk completes the tap.
vec3 GrassCompatLayerAlbedo(uint slot, vec2 uv, vec2 ddx, vec2 ddy)
{
    if (slot == 0u)
        return textureGrad(sampler2D(gGrassLayer0Albedo, gGrassMapSampler), uv, ddx, ddy).rgb;
    return textureGrad(sampler2D(gGrassLayer1Albedo, gGrassMapSampler), uv, ddx, ddy).rgb;
}

// A terrain lattice map read at a terrain UV (CBT_LatticeTextureUV): explicit LOD, and a UV that
// stays inside the texture in place of the clamp sampler this profile cannot afford.
#define GRASS_COMPAT_LATTICE_TAP(tex, uv) \
    textureLod(sampler2D(tex, gGrassMapSampler), \
               CBT_LatticeTextureUV(uv, vec2(textureSize(sampler2D(tex, gGrassMapSampler), 0))), 0.0)
#endif

// The shared albedo include's three seams. Their defaults are the bindless spelling, so only the
// compat arm differs; see Includes/terrain_material_albedo.glsl for what each one exists for.
#if defined(GE_COMPAT_PROFILE)
// Presence rides a record FLAG here: every bindless index resolves to the unbound sentinel on this
// profile, so an index test reads "untextured" for a bound layer and the ground under every blade
// falls back to its tint. Mirror of kTerrainMaterialFlagHasAlbedo (TerrainMaterialRecord.h).
#define GRASS_MATFLAG_HAS_ALBEDO 8u
#define CBT_MAT_HAS_ALBEDO(mat) (((mat).Flags & GRASS_MATFLAG_HAS_ALBEDO) != 0u)
// Only ALBEDO is bound per ground layer here (a blade needs the ground's colour, not its normal or
// ORM), so the kind is dropped and the ordinal alone selects the binding.
#define CBT_LAYER_SLOT(ord, kind) (min(ord, kGrassCompatLayers - 1u))
#define CBT_LAYER_TAP(slot, tex, uv, ddx, ddy, bias) \
    GrassCompatLayerAlbedo(slot, uv, (ddx) * exp2(bias), (ddy) * exp2(bias))
// One sampler per named binding here, so the clamp-to-edge Planar tap is the same tap.
#define CBT_LAYER_TAP_EDGE(slot, tex, uv, ddx, ddy, bias) CBT_LAYER_TAP(slot, tex, uv, ddx, ddy, bias)
#endif

// The splat taps of TerrainGrass/grass_atlas_splat.glsl, which is written against the bindless
// array this profile does not have. The unified splatmap is bound by name. The ATLAS-resident
// pair is NOT bound on this profile — no CPU binding fills it — so those two answer with zero
// weights, which grassNormalizeLayerWeights turns into the same channel-0 result the file already
// returns when no splat source is bound. An atlas terrain's grass therefore takes the ground's
// first layer here instead of its resolved blend: a documented limitation of this profile, not a
// wrong texel.
#if defined(GE_COMPAT_PROFILE)
#define GRASS_SPLAT_TAP_UNIFIED(idx, uv) GRASS_COMPAT_LATTICE_TAP(gGrassSplatmap, uv)
#define GRASS_SPLAT_TAP_ATLAS_SLOT(idx, uv) vec4(0.0)
#define GRASS_SPLAT_TAP_ATLAS_COARSE(idx, uv) vec4(0.0)
#endif

#include "TerrainGrass/grass_atlas_splat.glsl"

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

// Layer-albedo sampler access, NONUNIFORM. A grass draw carries blades from several terrains — the
// terrain row arrives per instance through custom0.w — so a material's bindless index varies within
// the draw and the descriptor access must be decorated. cbt_surface.glsl draws one terrain and
// correctly uses the plain index; taking its definition here would be undefined behaviour that
// looks right on a single-terrain scene.
#if defined(GE_COMPAT_PROFILE)
// Never expanded on this profile — the tap seam below selects a named binding instead. Defined
// only because the include requires the symbol; expanding it is a compile error naming this line.
#define CBT_LAYER_TEX(texIdx) GE_COMPAT_PROFILE_HAS_NO_BINDLESS_TEXTURE_ARRAY
#else
#define CBT_LAYER_TEX(texIdx) GE_BTEX(texIdx, GE_TS_REPEAT)
#define CBT_LAYER_TEX_EDGE(texIdx) GE_BTEX(texIdx, GE_TS_CLAMP_ANISO)
#endif


// The material record and the ALBEDO resolve run on it — the SAME file cbt_surface.glsl includes,
// so "the terrain's albedo at this point" has one definition and a blade's grounded base lands on
// the colour the ground beside it actually renders. A local restatement of that resolve (a flat
// tint or single-tap approximation) silently drops the variation/hex/side-projection/UV-phase
// terms the ground applies — sample through the include only.
#include "Includes/terrain_material_albedo.glsl"

// The terrain's material table — the SAME buffer cbt_surface.glsl reads, so a blade and the ground
// under it resolve the same RECORD for a given splat channel.
//
// They also share the RESOLVE: Includes/terrain_blend_resolve.glsl folds duplicate roles into one
// contribution and cuts to the same heaviest few on both sides, so the ranking a blade blends by is
// the ranking its ground blends by.
//
// What that does NOT make identical is where the four raw weights come from. The ground picks
// between four sources (planet slope+altitude, the painted planar splat, the resident-window atlas
// splat, and channel 0 when nothing is bound); the blend below resolves the planar three of those
// through grass_atlas_splat.glsl — the same file the placement compute resolves them through — and
// addresses them from world XZ rather than the ground's interpolated uv0. The remaining gap is the
// SPHERE, whose radial slope+altitude blend has no splat to sample and which sources no grass
// today (placement is planar-only).
//
// Nameless on purpose: MaterialBinder keys set-2 buffers on the reflected INSTANCE name, so an
// instance name here would leave the binding unresolvable, and MaterialBinder then drops the draw
// with a throttled Error log — the symptom is invisible grass, not a device loss.
layout(std430, set = 2, binding = 4) readonly buffer TerrainMaterialTableBuffer
{
    TerrainMaterialRecordData TerrainMaterials[];
};

// The blend width, from the single shared definition the ground surface also takes. Included
// before the resolve below, which is parameterized by it.
#include "Includes/terrain_blend_width.glsl"

// The shared resolve — the weight floor, the duplicate-role fold and the soft top-K the ground
// runs. Included after the define above, which is what parameterizes it.
#include "Includes/terrain_blend_resolve.glsl"

// The terrain's own surface normal at a blade's XZ, for the triplanar projection weights the shared
// albedo resolve blends its taps by. Same textures, same decode, same resolve the GROUND taps at
// that point: the pre-baked R16G16 world nx/nz normalmap on a planar terrain, the resident-window
// normal atlas (or its coarse field) on an atlas-backed one.
//
// That includes the coarse->slot upgrade CROSSFADE: for the ~0.4 s (kAtlasUpgradeFadeSeconds) after
// a tile becomes resident, cbt_surface mixes the coarse normal into the slot normal by the row's
// Fade, and so does this. The weight comes from GrassAtlas_SlotUpgradeWeight on the row THIS resolve
// read, so the blade and the ground move on one clock rather than two that could drift.
//
// Only called when a contributing material is TEXTURED. An untextured record resolves to its tint
// through the value-noise variation and never touches the projection weights, so the shipped
// untextured palette pays nothing for this.
vec3 grassTerrainNormalWS(TerrainParamsEntry tp, vec2 uv)
{
    if (tp.NormalmapBindless != 0u)
    {
#if defined(GE_COMPAT_PROFILE)
        vec2 nXZ = GRASS_COMPAT_LATTICE_TAP(gGrassVtxNormalmap, uv).rg;
#else
        vec2 nXZ = textureLod(GE_BTEX(tp.NormalmapBindless, GE_TS_CLAMP),
                              CBT_LatticeTextureUV(uv, vec2(textureSize(GE_BTEX(tp.NormalmapBindless,
                                                                                GE_TS_CLAMP), 0))),
                              0.0).rg;
#endif
        return normalize(vec3(nXZ.x, sqrt(max(1.0 - dot(nXZ, nXZ), 0.0)), nXZ.y));
    }
#if !defined(GE_COMPAT_PROFILE)
    // The ATLAS normal pair is not bound on the compat profile — no CPU binding fills it, the same
    // limitation the splat seams document — so that profile falls through to the flat normal
    // below, which is the answer this function already gives an atlas terrain with nothing bound.
    if (grassAtlasBacked(tp.Flags))
    {
        AtlasResolve r = grassAtlasResolve(uv);
        bool haveSlot = r.Resident && Atlas.NormalBindless != 0u;
        bool haveCoarse = Atlas.NormalCoarseBindless != 0u;
        float slotWeight = GrassAtlas_SlotUpgradeWeight(haveSlot, haveCoarse, r.Fade, true);
        vec2 nXZ = vec2(0.0);
        if (haveSlot)
        {
            nXZ = textureLod(GE_BTEX(Atlas.NormalBindless, GE_TS_CLAMP), r.SlotUV, 0.0).rg;
            if (slotWeight < 1.0)
                nXZ = mix(textureLod(GE_BTEX(Atlas.NormalCoarseBindless, GE_TS_CLAMP),
                                     grassCoarseUV(uv, Atlas.CoarseDim), 0.0).rg,
                          nXZ, slotWeight);
        }
        else if (haveCoarse)
            nXZ = textureLod(GE_BTEX(Atlas.NormalCoarseBindless, GE_TS_CLAMP),
                             grassCoarseUV(uv, Atlas.CoarseDim), 0.0).rg;
        return normalize(vec3(nXZ.x, sqrt(max(1.0 - dot(nXZ, nXZ), 0.0)), nXZ.y));
    }
#endif
    return vec3(0.0, 1.0, 0.0);
}

// The ground colour under a blade: the terrain's own albedo at the blade's XZ, resolved by the same
// expression on the same records with the same projection weights the ground uses.
//
// footprintWS is this fragment's world footprint, the argument the variation's Nyquist fade takes.
// It is the BLADE's footprint rather than the ground's — a near-vertical ribbon seen at a grazing
// angle spans differently from the ground under it — which matters only inside the fade band, at
// 0.35 to 1.2 noise cells per pixel. At the shipped VariationScale of 0.03 /m that band starts at a
// ~12 m footprint, and a blade fragment's is centimetres out to the placement range, so both sides
// sit at fade 1.0. A material authored with metre-scale cells would reach it and the two would fade
// differently.
//
// Evaluated HERE, in uniform control flow: EvaluateSurface calls this before any discard, and
// fwidth() inside the per-material loop below would be undefined where that loop diverges.
vec3 sampleTerrainBaseColor(TerrainParamsEntry tp, vec3 posWS, vec3 posRelWS)
{
    float footprintWS = length(fwidth(posWS));

    vec2 uv = clamp(vec2(
        (posWS.x - tp.WorldOriginX) / max(tp.WorldSizeX, 0.0001),
        (posWS.z - tp.WorldOriginZ) / max(tp.WorldSizeZ, 0.0001)
    ), vec2(0.0), vec2(1.0));

    // The blade's ground weights, through the shared resolve: the unified splatmap on a planar
    // terrain, the resident-window atlas slot (or its coarse field) on an atlas-backed one, and
    // channel 0 when nothing is bound. An atlas terrain carries SplatmapBindless == 0 by
    // construction, so reading that field alone answered channel 0 for the whole world.
    //
    // SHOWING the coarse->slot upgrade crossfade, unlike the placement compute: this is the colour
    // the ground beside the blade is rendering, and the ground spends kAtlasUpgradeFadeSeconds
    // arriving at it. Leading it there is a visible seam for the length of that window.
    vec4 weights = GrassAtlas_ResolveSplat(tp.Flags, tp.SplatmapBindless, uv, true);

    // The ground's own resolve, on the ground's own roles — resolved BEFORE the blend for the same
    // reason cbt_surface does it there: from here on a weight belongs to a material, not a channel.
    uint roles[4] = uint[4](tp.LayerRole[0], tp.LayerRole[1], tp.LayerRole[2],
                            tp.LayerRole[3]);
    weights = CBT_ResolveBlendWeights(weights, roles);

    // Whether any surviving material is textured, decided BEFORE the projection context is built so
    // the terrain-normal tap is paid for only where a projection weight is actually read.
    //
    // This is NOT dynamically uniform: the weights come from the splat, so two pixels of a quad
    // straddling a material boundary can disagree. That is why the normal taps it gates are
    // textureLod at level 0 — an implicit-LOD tap in divergent flow has undefined derivatives —
    // and it is the same level the placement compute reads the normal at, so a blade's shading
    // normal source and its placement's agree.
    bool anyTextured = false;
    for (int i = 0; i < 4; ++i)
        anyTextured = anyTextured || (weights[i] > 0.0 && TerrainMaterials[roles[i]].AlbedoTex != 0u);

    // `uv` is the ground's footprint UV under the blade, which a Planar material samples at.
    CBTTriplanarCtx tc = CBT_BuildTriplanarCtx(
        posRelWS, anyTextured ? grassTerrainNormalWS(tp, uv) : vec3(0.0, 1.0, 0.0), uv);
    float uvTiling = CBT_MaterialUVTiling(tp.MaterialTiling);

    vec3 color = vec3(0.0);
    for (int i = 0; i < 4; ++i)
    {
        // Floored, folded and cut-away channels come back EXACTLY zero, which is what makes this a
        // `> 0.0` test: a merely-small weight would still cost a fetch.
        if (weights[i] > 0.0)
        {
            TerrainMaterialRecordData mat = TerrainMaterials[roles[i]];
            CBTMaterialUV muv = CBT_MaterialUVs(mat, tc, uvTiling);
            // The blend ordinal, not the role: it selects the named binding on a profile with no
            // descriptor indexing, and is ignored on the bindless one.
            color += CBT_MaterialAlbedo(uint(i), mat, tc, muv, posWS, footprintWS) * weights[i];
        }
    }
    return color;
}

vec3 grassColorFromArgb(uint argb)
{
    return vec3(
        float((argb >> 16) & 0xFFu),
        float((argb >> 8) & 0xFFu),
        float(argb & 0xFFu)
    ) / 255.0;
}

float grassHash11(float p)
{
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

#if defined(GE_USER_GRASS_DITHER) || defined(GE_USER_GRASS_A2C)
// Jimenez interleaved gradient noise: an even per-pixel threshold distribution with
// no lattice structure at screen-door scales. Deliberately static (no temporal
// term): without TAA a rotating pattern reads as crawling noise, and rotating it
// only under TAA needs a per-view TAA signal the pinned grass pipeline does not
// receive today.
float grassDitherThreshold(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}
#endif

#if defined(GE_USER_GRASS_A2C)
// Alpha-to-coverage quantizes alpha to sampleCount+1 coverage levels; +/- half of
// this amplitude of per-pixel dither on the alpha breaks those bands where a grass
// TEXTURE authors a soft edge. Geometric blades are alpha 1.0 and never reach it —
// the endpoint guard below is what keeps their interiors solid. Sized for 4x MSAA
// (1/4); at 8x it dithers slightly wider than one level, which reads as texture,
// not error.
//
// OPEN: the editor now defaults to 2x, where one coverage band is 0.5, so whether one
// amplitude serves every sample count is unmeasured. A first attempt sited its ROI where
// 2x produced no partially-covered pixels at all, which cannot answer the question.
const float kGrassCoverageDitherAmp = 0.25;
#endif

// Length profiles along the blade, all gated by GroundingStrength so one authored knob still turns
// the whole grounding read down. Height fractions, not metres: they hold as the blade rescales.
// The DEPTH is authored on the component (RootShade); only the SHAPE is fixed here.
// Height fraction over which the base-to-tip shade ramp recovers to full canopy value. It rides
// INSIDE the blade's colour schedule (grassBladeRise below), which weights this range to 0.03-0.12
// at the shipped span -- so RootShade shapes a blade only where a shorter RootFadeEnd lifts that.
const float kGrassRootShadeRange = 0.22;

// THE BLADE'S COLOUR SCHEDULE: how far this fragment's ALBEDO has left the ground it grows out of.
// 0 below RootFadeStart, where the blade IS the terrain, and 1 above RootFadeEnd, where it is
// entirely itself. This is a material lever, authored on the component, and it moves the colour
// only. The shading-normal settle below runs the whole blade whatever span is authored: the
// lighting handover is the physical part, and a shorter span for it is what put a dark band
// across the lower-middle of backlit blades (kGrassNormalSettleBladeRange carries that
// measurement). The shipped span is the whole blade, where colour and lighting hand over on the
// same curve. The clamp pass keeps RootFadeEnd above RootFadeStart, which smoothstep requires.
float grassBladeRise(TerrainParamsEntry tp, float t)
{
    return smoothstep(tp.GrassRootFadeStart, tp.GrassRootFadeEnd, t);
}

// Distance-settled blade normals. Near the camera a blade shades with the rounded normal the vertex
// stage builds across its width, so it reads as a curved body with a lit and a shaded side. Far
// away that variation is sub-pixel and feeds specular aliasing rather than form, so it gives way
// to the TERRAIN normal, shading the field as one lit surface with the ground it stands on.
// Fraction of the far-LOD band over which that handover completes, so it holds as GrassRange is
// authored rather than being a second distance control that can disagree with the first.
const float kGrassNormalSettleRange = 0.55;
// Height fraction over which the same handover completes along the BLADE: fully settled onto the
// terrain normal at the root, entirely the blade's own normal above this. It runs the WHOLE blade,
// independent of the authored colour span (RootFadeStart / RootFadeEnd), which is a material
// lever and must not compress the lighting handover.
//
// Spanning the whole blade is what keeps a dark band off the lower-middle of one. Under a low sun
// the settle and the blade's own normal pull opposite ways -- a near-horizontal blade normal
// catches more of a 15-degree sun than the terrain normal does -- so a range that ends early hands
// the fragment to its own normal while the colour is still mostly ground, and the two disagree
// across the gap. Measured into a 15-degree sun at eye height, blade/ground luma by height
// fraction, over the marker mask:
//
//   0-5%  5-12  12-20  20-30  30-42  42-55  55-70  70-85  85-100
//   0.95  0.89  0.75   0.75   0.99   1.31   1.63   1.93   2.16     at 0.55
//   0.96  0.96  0.95   0.92   0.96   1.19   1.54   1.90   2.15     at 1.0, with the tip mask below
//
// The trough at 12-30% is the band. Over the full blade a ~4% shallow remains at 20-30% -- about
// 1.3 levels at luma 30, below what reads as a band.
//
// The cost of the whole-blade settle is a HIGH sun. A blade settled onto the terrain's up-dominant
// normal catches an overhead sun that its own near-horizontal normal does not, so it brightens a
// noon field by up to 3.1 levels and moves blade-over-ground at noon from 1.103 to 1.148 -- away
// from the contact identity the settle exists to serve. Into a low sun it costs 0.9 to 1.5 levels.
// The transmission's start (the tip mask below) is the other half: neutral at noon to within 0.01
// of a level, worth 0.75 of a level into a low sun, and that is what carries the low-sun field
// brightness. The two do not cancel pose by pose; the noon brightening is a look call, not a wash.
//
// It does not share a value with the far-LOD range above: one is a fraction of the far-LOD band
// and the other a fraction of blade height, tuned against different measurements.
const float kGrassNormalSettleBladeRange = 1.0;
// Blade shading across the width, and the brightness it costs, are AUTHORED on the component
// (TerrainGrass::BladeNormalForm / BladeScatterGain) and arrive through the params SSBO. They are
// authored rather than fixed because one number has to serve a camera standing in the grass and a
// camera looking across a valley, and those two want different values. (The ~3.6% dark at eye level
// and ~3.5% bright at a vista once quoted here were measured before the deficit below was bounded,
// which moved the field mean by 1.3 levels at eye level and 2.6 at mid distance — a third to a half
// of those figures' own size. Treat them as stale until re-measured.) Tuning them must not mean
// editing this file.
//
// FORM is how far the near-field shading normal leaves the terrain normal toward the blade's own. A
// blade is a near-vertical ribbon, so its geometric normal is near-HORIZONTAL, and shading on it the
// whole way costs about a fifth of the field's brightness — a horizontal normal catches far less of
// a high sun than an up-dominant one. Brightness is not what it is wanted for: the vertex stage
// builds the normal as a facing term plus a ROUNDING term across the width, and only the rounding
// makes a blade read as a curved body. The rounding survives the blend as an azimuthal tilt; what is
// given up is the part of the swing that did nothing but darken the field. 0 is exactly the canopy
// NORMAL, and 1 is the blade's own normal and the dark field. Shipped default 0.75. Note 0 is not
// a full revert: roughness rides the same weight, so at 0 a blade is the canopy roughness rather
// than the flat 0.92 this surface used before the blade normal existed. That is deliberate — the
// terrain normal wants the canopy roughness — but it means 0 reproduces the old SHADING NORMAL, not
// the old pixels.
//
// SCATTER GAIN hands that brightness back, standing in for the inter-blade scattering this pipeline
// has no term for — light that misses one blade's face reaches it off its neighbours. It multiplies
// albedo, but it is NOT a uniform lift: the deficit below is largest exactly where the shading
// normal lost the most light, so the darkest fragments are lifted hardest — measured 28% on the
// darkest decile against 13% on the brightest at eye level, and 28% against 2% at mid distance.
//
// So the gain TRADES the field's mean brightness against its blade-scale contrast, and that trade
// is the thing to understand before touching it. The blade normal buys value break-up between
// blades (9x9 local contrast, normalised so a uniform albedo multiply cannot move it, reads ~9.4%
// at gain 0 against ~6.6% for the terrain normal alone); the gain spends it back, and by the shipped
// 0.60 the field is at that terrain-normal floor again.
//
// Shipped default 0.60, originally calibrated against main, which renders grass at 1.153x the
// brightness of the ground it stands on. Since the deficit was bounded to the loss actually taken
// it lands nearer 1.0x, so it now UNDER-pays against that figure. Do not simply raise the gain to
// close the gap: the excess that made up the difference was the mid-field band, and it returns by
// 0.9. Recovering the mean without it needs a second lever, not a bigger one.
// Gloss follows the normal detail it belongs to: a lobe justified by per-blade normal variation has
// to widen as that variation settles out, or it survives as glitter on a surface that no longer has
// the geometry to earn it.
const float kGrassBladeRoughness = 0.92;
const float kGrassCanopyRoughness = 1.0;

// Transmission gain, sized against the authored GrassTranslucency default of 0.35 under the
// camera-sun lobe this term used to be gated on: the tip band (t > 0.6) looking down into a
// 15-degree sun read a median 1.321 of the ground behind it, mid-way in a 1.2-1.6 target fixed
// before the sizing, with 0.125 bracketing it from below at 1.203 and no term at all at 1.093.
// Re-read under the face gate over the t > 0.5 band's marker mask, same pose:
//
//   no transmission at all (Translucency 0)   1.09
//   0.125                                     1.33   but 7.7 % of the blade pixels at the 3 m
//                                                    into-sun pose stay below 0.8x the ground
//   0.25 (this)                               1.52   0.6 % below 0.8x
//
// 0.125 centres the tip target and leaves the defect this gate exists to remove; 0.25 sits in
// the target's upper half and removes it. This is a gain on a RADIANCE term: it is not on the
// same scale as a gain on a reflectance term, which the lighting would multiply again.
const float kGrassTransmissionScale = 0.25;

// How much brightness the settle gave up at this fragment, for the scatter gain to hand back.
//
// Lifted verbatim into the host suite (ExtractShaderBlock.cmake), which is why the literals carry
// the `f` suffix — unsuffixed they are float in GLSL and double in C++, where MSVC's C4244 narrowing
// warning is an error. GrassScatterBoundTests EXECUTES this, so the bound below is pinned by the
// shipped expression rather than by a restatement of it that could drift.
//
// `grassNormal` is passed in rather than rebuilt here: it is the normal the fragment actually
// shades on, and EvaluateSurface already has it. Rebuilding the blend would put two copies of the
// shading normal in one file, and the whole defect this term had came from measuring against
// something other than what was shaded.
// GE_SHARED_GRASS_SCATTER_DEFICIT_BEGIN
float GrassScatterDeficit(vec3 settleNormal, vec3 bladeNormal, vec3 grassNormal, vec3 lightDir,
                          float bladeWeight)
{
    float ndlSettle = max(0.0f, dot(settleNormal, lightDir));
    float ndlBlade = max(0.0f, dot(bladeNormal, lightDir));
    float ndlShaded = max(0.0f, dot(grassNormal, lightDir));
    // The loss actually taken, capped by the swing the two endpoints could have cost. See the call
    // site for why each arm is there; the CAP is what keeps this from inventing shade-side light.
    return clamp(min(ndlSettle - ndlShaded, bladeWeight * (ndlSettle - ndlBlade)), 0.0f, 1.0f);
}
// GE_SHARED_GRASS_SCATTER_DEFICIT_END

uint grassAtlasTile(TerrainParamsEntry tp, float tint)
{
    uint cols = max(tp.GrassAtlasColumns, 1u);
    uint rows = max(tp.GrassAtlasRows, 1u);
    uint maxTiles = max(cols * rows, 1u);
    uint tileCount = clamp(tp.GrassAtlasTileCount, 1u, maxTiles);
    return min(uint(floor(fract(tint) * float(tileCount))), tileCount - 1u);
}

vec2 grassAtlasUV(TerrainParamsEntry tp, uint tile, float side, float heightT, float tint)
{
    uint cols = max(tp.GrassAtlasColumns, 1u);
    uint rows = max(tp.GrassAtlasRows, 1u);
    uint maxTiles = max(cols * rows, 1u);
    tile = min(tile, maxTiles - 1u);
    uint tx = tile % cols;
    uint ty = tile / cols;
    vec2 localUv = vec2(clamp(side, 0.0, 1.0), 1.0 - clamp(heightT, 0.0, 1.0));
    float flip = grassHash11(tint + 17.0) < 0.5 ? -1.0 : 1.0;
    localUv.x = flip < 0.0 ? 1.0 - localUv.x : localUv.x;
    localUv.x = clamp(localUv.x + (grassHash11(tint + 29.0) - 0.5) * 0.18, 0.0, 1.0);
    return (vec2(float(tx), float(ty)) + localUv) / vec2(float(cols), float(rows));
}

vec3 applyGrassNormalMap(TerrainParamsEntry tp, vec3 normalWS, vec3 viewDirWS, vec2 uv,
                         vec2 ddx, vec2 ddy)
{
    if (tp.GrassNormalBindless == 0u || tp.GrassNormalStrength <= 0.0)
        return normalWS;

    // Z reconstructed — see GE_DecodeTangentNormal (uniform for RGB8 and BC5 payloads).
#if defined(GE_COMPAT_PROFILE)
    // Gradients come from the caller: this function early-returns above, so nothing here is in
    // uniform control flow and WGSL rejects a derivative taken locally.
    vec3 packed = GE_DecodeTangentNormal(
        textureGrad(sampler2D(gGrassBladeNormal, gGrassMapSampler), uv, ddx, ddy));
#else
    vec3 packed = GE_DecodeTangentNormal(texture(GE_BTEX(tp.GrassNormalBindless, GE_TS_REPEAT), uv, ge_mipBiasParams.x));
#endif
    vec3 n = normalize(normalWS);
    vec3 tangent = normalize(cross(abs(n.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), n));
    vec3 bitangent = normalize(cross(n, tangent));
    vec3 mapped = normalize(tangent * packed.x + bitangent * packed.y + n * max(packed.z, 0.08));
    mapped = faceforward(mapped, -normalize(viewDirWS), n);
    return normalize(mix(n, mapped, clamp(tp.GrassNormalStrength, 0.0, 2.0)));
}

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();

    // Distance LOD, 1 at the camera and 0 at the placement range. It drives the blade-to-
    // ground COLOUR blend only: alpha is not distance-driven, because blades leave the field
    // by not being spawned and the placement compute already scales the outermost slots to
    // nothing. Fading a blade out while still rasterising and blending it is the overdraw the
    // placement rework exists to remove.
    float farLodTerm = GrassUnpackFarLod(sIn.custom0.x);
    float tint = sIn.custom0.y;
    float gust = GrassUnpackGust(sIn.custom0.x);
    // The ground's own normal at the blade's FOOT, carried from the vertex stage.
    vec3 terrainNormal = GrassUnpackTerrainNormal(sIn.custom0.z);
    int terrainIdx = int(GrassUnpackTerrainIndex(sIn.custom0.w));
    float clumpRand = GrassUnpackClumpRand(sIn.custom0.w);
    TerrainParamsEntry tp = terrains[terrainIdx];
    float brightness = max(tp.GrassBrightness, 0.0);
    float groundingStrength = clamp(tp.GrassGroundingStrength, 0.0, 1.0);
    float t = clamp(sIn.uv0.x, 0.0, 1.0);
    float side = clamp(sIn.uv0.y, 0.0, 1.0);
    float farLod = 1.0 - clamp(farLodTerm, 0.0, 1.0);
    float bladeRise = grassBladeRise(tp, t);

    vec3 terrainColor = sampleTerrainBaseColor(tp, sIn.positionWS, sIn.positionRelWS);
    vec3 base = ((tp.GrassEnabled & 2u) != 0u) ? terrainColor : grassColorFromArgb(tp.GrassRootColor);
    vec3 tip = grassColorFromArgb(tp.GrassTipColor);
    vec3 backlightColor = grassColorFromArgb(tp.GrassBacklightColor);

    vec3 col = mix(base, tip, bladeRise);
    uint atlasTile = grassAtlasTile(tp, tint);
    vec2 textureUv = grassAtlasUV(tp, atlasTile, side, t, tint);
    // Hoisted here, the last uniform-flow point before the blade-texture branches below: WGSL
    // rejects an implicit derivative inside them. The TAAU bias folds in, since textureGrad takes
    // no bias of its own. Zero on the profile that samples with an implicit derivative and never
    // reads these.
#if defined(GE_COMPAT_PROFILE)
    vec2 bladeGradScale = vec2(exp2(ge_mipBiasParams.x));
    vec2 bladeDdx = dFdx(textureUv) * bladeGradScale;
    vec2 bladeDdy = dFdy(textureUv) * bladeGradScale;
#else
    const vec2 bladeDdx = vec2(0.0);
    const vec2 bladeDdy = vec2(0.0);
#endif
    float textureAlpha = 1.0;
    if (tp.GrassAlbedoBindless != 0u)
    {
#if defined(GE_COMPAT_PROFILE)
        vec4 texel =
            textureGrad(sampler2D(gGrassBladeAlbedo, gGrassMapSampler), textureUv, bladeDdx, bladeDdy);
#else
        vec4 texel = texture(GE_BTEX(tp.GrassAlbedoBindless, GE_TS_REPEAT), textureUv, ge_mipBiasParams.x);
#endif
        col *= texel.rgb;
        textureAlpha *= clamp(texel.a, 0.0, 1.0);
    }
    if (tp.GrassAlphaBindless != 0u)
    {
#if defined(GE_COMPAT_PROFILE)
        float alpha =
            textureGrad(sampler2D(gGrassBladeAlpha, gGrassMapSampler), textureUv, bladeDdx, bladeDdy).r;
#else
        float alpha = texture(GE_BTEX(tp.GrassAlphaBindless, GE_TS_REPEAT), textureUv, ge_mipBiasParams.x).r;
#endif
        textureAlpha *= clamp(alpha, 0.0, 1.0);
    }
#if !defined(GE_USER_GRASS_OPAQUE)
    // The card cutout, on the texture-card bit (bit2) rather than the alpha bit: a card blade is a
    // quad, so the texture's alpha is the only thing that gives it its shape. Geometric ribbons
    // leave this bit clear and are shaped by their geometry.
    //
    // KEYWORD-gated, not merely runtime-gated. A discard anywhere in a fragment module is a static
    // property of the pipeline: the hardware cannot retire the depth test before the shader has run
    // if the shader might yet kill the fragment, so an OpKill costs early-Z on every fragment the
    // module ever shades, including the ones that provably never reach it. The GRASS_OPAQUE variant
    // is the view where this test is unreachable by construction — it is compiled only when no
    // active grass row carries soft alpha, which leaves textureAlpha at 1.0, and a cutoff clamped
    // to [0,1] can never exceed it. Branching around a kill does not buy early-Z back; not
    // compiling it does.
    bool textureGrass = (tp.GrassEnabled & 4u) != 0u;
    if (textureGrass && textureAlpha < clamp(tp.GrassAlphaCutoff, 0.0, 1.0))
        discard;
#endif

    // The blade's own authored alpha, and nothing else: the sampled texture alpha. Geometric grass
    // binds no texture, so this is exactly 1.0 and the blade body is opaque end to end — its shape
    // is geometry, and nothing below may punch holes in it. Distance does not enter — see
    // farLodTerm. How a sub-1.0 alpha reaches the framebuffer is the material's GRASS_DITHER /
    // GRASS_A2C keyword's decision, at the end.
    float opacity = textureAlpha;

    float viewFacing = abs(dot(normalize(sIn.normalWS), normalize(sIn.viewDirWS)));
    float fresnelTips = pow(clamp(1.0 - viewFacing, 0.0, 1.0), 2.2);
    vec3 lodColor = mix(base, tip, smoothstep(0.08, 0.92, fresnelTips));
    col = mix(col, lodColor, smoothstep(0.15, 0.85, farLod) * 0.85);
    // Hue jitter, at two granularities from ONE authored amount: per-clump moves whole tufts
    // apart, per-blade breaks up the inside of one. Both rotate about the achromatic axis, so they
    // change WHICH green a blade is without changing how bright it is — the value-only spread the
    // two Brightness knobs give reads as one colour lit unevenly rather than as many plants.
    // Applied BEFORE the root grounding below, so the base still resolves exactly onto the ground
    // colour instead of onto a rotated version of it.
    // The per-blade draw is rehashed off `tint` rather than used raw: `tint` is the instance's
    // width random, and hue tracking width would read as wide blades being systematically yellower.
    float hueVariation = clamp(tp.GrassHueVariation, 0.0, 1.0);
    float hueTurn = (clumpRand - 0.5) * 2.0 * hueVariation
        + (grassHash11(tint + 41.0) - 0.5) * 2.0 * hueVariation * kGrassBladeHueShare;
    col = GrassHueShift(col, hueTurn * kGrassMaxHueShiftRadians);
    // The two granularities sum: at the maximum the rotation reaches +/-42 degrees, which can push
    // a saturated channel negative. Never emit negative albedo.
    col = max(col, vec3(0.0));
    // Gust shading is WIND shading: its amplitude follows the authored wind strength, reaching
    // the full 0.66-1.08 swing at strength >= 1 and vanishing at 0. Ungated it kept modulating a
    // windless meadow — the gust field flows regardless of strength, so a static scene shimmered.
    // The vertex stage supplies the spatially resolved strength, including calm volumes.
    float windShade = groundingStrength * vGrassWindStrength;
    col *= mix(1.0, mix(0.66, 1.08, gust), windShade);
    float rootShade = clamp(tp.GrassRootShade, 0.0, 1.0);
    col *= mix(1.0, mix(rootShade, 1.0, smoothstep(0.0, kGrassRootShadeRange, t)), groundingStrength);
    // Both brightness terms belong to the CANOPY end of the schedule below, which is why they are
    // applied here, before it. The grounded end is the terrain and nothing else: scaling a colour
    // that has already resolved onto the ground would put per-blade variation back into the one
    // place a blade is supposed to disappear into its surroundings, and with RandomBrightness at
    // its shipped 0.42 the roots would spread +/-42% around the ground they stand in.
    col *= max(0.0, 1.0 + (tint * 2.0 - 1.0) * tp.GrassRandomBrightness);
    col *= brightness;

    // THE GROUNDED END. A blade's root belongs to the ground it stands in, not to the canopy, so
    // the two ends of the blade are the terrain's own resolved colour and the fully modulated
    // canopy colour above, and the blade is the single mix between them on its one schedule.
    //
    // At t = 0 with a non-negative RootFadeStart the mix is entirely the grounded end, so the root
    // albedo IS the terrain's own resolved colour — a structural guarantee at the default rather
    // than the product of numbers that happen to multiply out, and a blade emerges from the ground
    // instead of merely standing near it. A negative RootFadeStart is the deliberate exception: it
    // starts the blend below the root, so the root carries some tip colour and a value step sits on
    // the contact line by choice.
    //
    // By default the DEPARTURE spans the whole blade; a shorter authored span concentrates it near
    // the base. Crossing the entire
    // distance from the ground to a fully modulated blade inside a fraction of the height puts the
    // steepest slope just above the contact, which reads as a bright band across the roots with a
    // clean gradient above it. The canopy chain carries its own root-shade ramp, but the schedule
    // weights that region to a few percent, so it shapes the canopy without becoming a second ramp.
    //
    // Nothing occludes the contact's indirect diffuse either — o.ao at the end of this function
    // carries why.
    //
    // GroundingStrength still turns the whole grounding read off: at 0 the weight is 1 everywhere and
    // the blade is its canopy chain from root to tip.
    col = mix(terrainColor, col, mix(1.0, bladeRise, groundingStrength));

    float terrainFade = smoothstep(0.28, 0.92, farLod);
    col = mix(col, terrainColor, terrainFade * 0.92);

    // The blade's OWN normal, turned to face the viewer. Blades are drawn two-sided, so half of
    // them present the face whose normal points away; shading that face on its unflipped normal
    // puts the lit side of the field on whichever blades happen to be yawed toward the sun.
    vec3 bladeNormal = normalize(sIn.normalWS);
    bladeNormal = faceforward(bladeNormal, -normalize(sIn.viewDirWS), bladeNormal);

    // Light through a thin blade reaches the face the viewer sees when the FAR face is the one the
    // sun lights, and the amount is that far face's own irradiance -- so the gate is the blade's
    // facing, max(dot(-bladeNormal, lightDir), 0), not the angle between the camera and the sun.
    //
    // A camera-sun lobe cannot find the faces that need this light. Looking down at a field into a
    // 15-degree sun it reads about 0.1 on every blade, while the faces turned away from the sun
    // are exactly the ones the direct term has already lost: on such a face the settle blend's
    // residual tilt collapses N.L against a grazing sun (a 16% blade weight is a 10-degree tilt,
    // which takes N.L from 0.26 to 0.07 -- 28% of the ground's -- where the same tilt costs under
    // 2% at noon), and the scatter hand-back is capped there by design, so the face renders on sky
    // ambient alone: a blue stub on the lower body of the blade, darker than the ground it grows
    // out of. Gated on the face, the term lights exactly those faces, adds nothing to a front-lit
    // face (its direct term already carries the sun), and is inert at noon, when no visible face
    // is turned away from an overhead sun.
    vec3 lightDir = normalize(-Light.uLightDirWorld.xyz);
    float backlit = max(dot(-bladeNormal, lightDir), 0.0);
    // Starts at the root, not a fifth of the way up. Beginning higher left the blade's lower
    // middle with neither the settle's grounding nor the transmission's light, which measured as a
    // trough of 0.60 at 20-30% of blade height against the ground; starting at 0.05 fills it while
    // the full-blade settle removes the rest.
    float tipMask = smoothstep(0.05, 1.0, t);
    vec3 sunColor = max(Light.uLightColorWorld.rgb, vec3(0.0));

    // Transmission is RADIANCE and must leave through o.emissive, which the adapter adds AFTER
    // lighting. baseColor is reflectance: anything put there is scaled by the blade's own N.L,
    // which is weakest exactly where this term is needed, and is multiplied by the sun's
    // uLightDirWorld.w a second time on the directional path. This slot is what lets the term
    // carry that intensity once and land on a fragment the blade's own normal has turned away
    // from the sun. The ocean surface routes its subsurface body the same way.
    //
    // It is added with no shadow term: adapter_forward.glsl applies emissive after the shadowed
    // lighting, so a blade standing inside a cast shadow still transmits at full strength.
    //
    // There is no distance term in this expression, but its OUTPUT still rises with distance,
    // because a far fragment is nearly all TIP, where tipMask is 1, over a darker denominator. So
    // what this draws into a low sun is a bright strip under it, and how wide and how bright that
    // strip should be is a LOOK choice with two dials, neither taken here: an attenuation on
    // farLod, and whether the term should be shadowed at all.
    o.emissive = backlightColor * sunColor * max(Light.uLightDirWorld.w, 0.0)
                 * backlit * tipMask * max(0.0, tp.GrassTranslucency) * kGrassTransmissionScale;
    o.metallic = 0.0;
    // ONE settle term with two reasons to be settled, so the blade cannot end up shaded as a
    // ribbon in either place the field needs it shaded as ground.
    //
    // Both settle onto `terrainNormal`, the GROUND'S OWN normal at this blade's foot, so the
    // grounded end of a blade takes the same shading normal as the ground beside it and its DIRECT
    // term matches under any sun. Settling onto anything else — including a fixed near-up vector —
    // reintroduces an angle between a fragment coloured as terrain and the terrain it sits on, and
    // the lower the sun the more of the base's light that angle costs.
    //
    // The NORMAL is all this settles. A root is not therefore indistinguishable from the ground:
    // under a low sun a departure survives even with the contact's indirect-diffuse dip taken out
    // entirely -- measured with that occlusion pinned open, the 0.028-0.075 band's mean reaches
    // 1.00 while its darkest quarter stays at 0.867. What is left is micro-normal mismatch between
    // a blade base and the ground under a grazing sun.
    //
    // With DISTANCE, because per-blade normal variation is sub-pixel out there and feeds specular
    // aliasing rather than form.
    //
    // Along the BLADE, because a fragment coloured as ground only READS as ground if it is LIT as
    // ground. Colouring a fragment as the terrain while lighting it as a near-vertical ribbon is
    // what makes the contact read wrong. The blade term here is the whole-blade smoothstep,
    // complemented -- the curve the shipped colour span rides. A fragment is therefore lit as ground to the
    // degree it is coloured as ground, all the way up, and no band can open between the two. It
    // stays on the whole blade when the authored colour span is shorter: the colour is a material
    // choice, the lighting handover is not. What spanning the whole blade costs under a high sun
    // is measured on kGrassNormalSettleBladeRange.
    // Settling here also gives the blade its dark-root, bright-tip structure out of geometry rather
    // than out of a second shade ramp.
    float normalSettle = max(smoothstep(0.0, kGrassNormalSettleRange, farLod),
                             1.0 - smoothstep(0.0, kGrassNormalSettleBladeRange, t));
    // How much of the blade's own normal survives here: its form strength, faded out by the settle.
    // Folding the two into one weight is an algebraic identity, not an approximation —
    // mix(mix(C, B, f), C, s) == mix(C, B, f * (1 - s)) — so this is one mix and one normalize
    // where the nested form needed two of each, and at f = 1 it reduces exactly to the
    // mix(bladeNormal, terrainNormal, settle) it replaces.
    float bladeWeight = clamp(tp.GrassBladeNormalForm, 0.0, 1.0) * (1.0 - normalSettle);
    o.roughness = mix(kGrassCanopyRoughness, kGrassBladeRoughness, bladeWeight);
    vec3 grassNormal = normalize(mix(terrainNormal, bladeNormal, bladeWeight));
    // Albedo carries the scattering the blade normal gives up, so a blade base and a distant blade —
    // which shade on the terrain normal and give up nothing — are lit exactly as they would be
    // without this term, and the two ground parities hold.
    //
    // Only what it ACTUALLY gives up, for THIS sun, measured against the normal the fragment is
    // ACTUALLY shaded on. That is the whole reason the deficit is not taken between the two
    // endpoint normals and scaled by bladeWeight: the loss is not linear in bladeWeight. The shaded
    // normal is normalize(mix(terrainNormal, bladeNormal, w)), and |mix(C, B, w)| <= 1 for unit C and
    // B, so normalizing lifts the interior blend back toward the terrain normal and the real loss
    // falls SLOWER than the straight line between the endpoints — it stays BELOW that chord across
    // the whole swing, which is the property this argument needs. Below the chord, NOT concave: the
    // loss starts out quadratic in w, so it is convex near 0; concavity would be a stronger claim
    // and a false one. Handing back a linear estimate of a sub-linear loss over-pays in the middle
    // of the swing, and over-paying here does not merely over-brighten: it renders the field
    // brighter than shading purely on the terrain normal, which a term whose entire job is to return
    // a loss has no licence to do. Measured, it reached +6.2 8-bit levels above that reference at
    // the shipped gain, at mid distance, where it read as a bright band across the field.
    //
    // BOUNDED by the endpoint budget, which is what the min is. ndlBlade is clamped at zero, so a
    // blade turned away from the sun reports no blade-side light at all, while the blended normal
    // above still carries the negative dot; on those fragments the shaded-normal deficit EXCEEDS the
    // swing the endpoints describe. Unbounded, the term would quietly become a second scattering
    // contribution on the shade side — the field already has one, at the backlight above, gated by
    // the authored GrassTranslucency. This one is not authored, so it must not invent light: the min
    // caps the hand-back at the budget the swing could have cost. On the lit side it is slack — the
    // shaded-normal deficit is the smaller of the two and wins — and it is what binds once a blade
    // is turned well away from the sun. (For a blade only grazingly averted the shaded-normal arm
    // can still be the smaller one, so the cap is not the active arm everywhere on the shade side;
    // it does not need to be, because min is safe either way.)
    //
    // WHAT THIS TERM DOES NOT KNOW: whether any light reaches the fragment. It sees normals and
    // lightDir only, and it multiplies ALBEDO, which the shadow term does not attenuate on the
    // ambient path — so a blade standing in a cast shadow is lifted too, and slightly harder than a
    // sunlit one, because the deficit is largest where the shading normal is darkest. Pre-existing
    // and not introduced here (this bound strictly lowers the lift everywhere), but it is the reason
    // tuning the gain by eye on a sunlit field can surprise you in shade.
    //
    // Both ends are exact. At bladeWeight 0 the shaded normal IS the terrain normal, the deficit is 0
    // and there is no lift; at bladeWeight 1 it IS the blade normal and both arms of the min agree.
    // Under a LOW sun, where the near-horizontal blade normal is the better-lit of the two, the
    // deficit goes negative and clamps to zero — no lift, because nothing was lost. A difference
    // rather than a ratio, so there is no vanishing denominator to guard.
    //
    // Deliberately measured on grassNormal and not on o.normalWS below: what is being compensated is
    // the SETTLE giving up the blade's form, not the material's normal-map detail, which perturbs
    // both the shaded normal and its own lighting and is authored to.
    //
    // TRAP: this term reads `bladeNormal`, which is faceforward'd, so a blade's two faces no longer
    // shade alike. Placement allocates instance slots through an atomicAdd whose order across
    // subgroups is not fixed, so which face wins a depth tie can differ between two frames of an
    // otherwise identical scene. Shading that was face-symmetric absorbed that; this does not, and
    // a grass still frame is therefore not guaranteed byte-identical — the band pose carries one
    // 8-bit level. The varying input is the slot ORDER, not the sun: a static sun and a static
    // camera do not make this term vary by themselves. It sits under the churn gate's threshold
    // (pctOver16 reads 0.0000 at every still pose), and a still-frame gate over grass wants a
    // threshold rather than bit-equality.
    float formDeficit = GrassScatterDeficit(terrainNormal, bladeNormal, grassNormal, lightDir,
                                            bladeWeight);
    o.baseColor = col * (1.0 + max(0.0, tp.GrassBladeScatterGain) * formDeficit);
    o.normalWS = applyGrassNormalMap(tp, grassNormal, sIn.viewDirWS, textureUv, bladeDdx, bladeDdy);
#if defined(GE_USER_GRASS_DITHER)
    // Screen-door: the soft alpha becomes a per-pixel coverage test, so surviving
    // fragments are fully opaque and the pipeline's depth write stays valid.
    if (opacity < grassDitherThreshold(gl_FragCoord.xy))
        discard;
    o.opacity = 1.0;
#elif defined(GE_USER_GRASS_A2C)
    // Alpha-to-coverage: the pipeline maps this alpha onto the MSAA sample mask.
    // Endpoints stay exact — dithering a fully-opaque fragment would knock MSAA
    // samples out of solid blade interiors as speckle.
    o.opacity = (opacity >= 1.0 || opacity <= 0.0)
        ? opacity
        : clamp(opacity + (grassDitherThreshold(gl_FragCoord.xy) - 0.5) * kGrassCoverageDitherAmp,
                0.0, 1.0);
#else
    o.opacity = opacity;
#endif
    // o.ao stays fully open, at the DefaultSurfaceOutput value. A blade-side occlusion at the
    // contact dims the base while nothing dims the ground beside it, and outdoors the indirect is
    // sky-dominated, so it lands as a CHROMA step at the contact line — the blade reading as a
    // different colour from its own ground rather than as shadow. A seating darkening has to
    // occlude BOTH sides, which is a ground-side term this surface does not own.
    return o;
}
