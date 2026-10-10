#version 450

// Engine-provided forward fragment adapter for the flexible render pipeline.
//
// Supports:
//   - Conditional varyings matching adapter_vertex.glsl
//   - Calls user-provided EvaluateSurface(SurfaceInput) -> SurfaceOutput
//   - Applies the selected lighting model BRDF
//   - Forward+ clustered lighting (when FORWARD_PLUS is defined)
//   - Single directional light fallback (when FORWARD_PLUS is not defined)
//   - Alpha test / blend support

// --- Varyings from vertex shader ---
// Shade at a covered location on MSAA boundary pixels. Pixel-center
// interpolation can extrapolate UVs/normals outside thin triangles; centroid
// preserves one fragment invocation per pixel and is a no-op at single sample.

// Alpha-tested surfaces evaluate coverage in separate depth and color passes.
// Centroid interpolation can select different locations after sample rejection,
// so those passes can disagree on opacity and leave sky-filled MSAA samples.
// Keep all surface inputs at pixel center for cutouts, including procedural
// opacity based on world position. Opaque shading uses centroid on desktop;
// GE_COMPAT_PROFILE has no centroid (naga drops the decoration).
//
// The motion variant joins them for the same reason at a different pairing: its
// current endpoint is gl_FragCoord, which is the pixel center, and its previous
// endpoint rides components of these same locations. Centroid there would build
// the delta from two different sample positions on a partially covered pixel.
#ifdef ALPHA_TEST
#define GE_SURFACE_CENTROID
#elif defined(GE_COMPAT_PROFILE)
#define GE_SURFACE_CENTROID
#elif defined(GE_MOTION_VECTORS)
#define GE_SURFACE_CENTROID
#else
#define GE_SURFACE_CENTROID centroid
#define GE_SURFACE_INPUTS_AT_CENTROID
#endif

// A surface input at the pixel centre. A centroid input on a partially covered MSAA pixel is taken
// at its covered samples' centroid, which moves from pixel to pixel along a triangle edge, so its
// screen derivatives there are wrong; derivatives that must hold across the edge take this instead.
// interpolateAtOffset needs the device's sampleRateShading (GE_INTERPOLATION_FUNCTIONS); without it
// the input is its centroid value, and the derivatives keep their error along those edges.
#if defined(GE_SURFACE_INPUTS_AT_CENTROID) && defined(GE_INTERPOLATION_FUNCTIONS)
#define GE_SURFACE_INPUT_AT_CENTRE(input) interpolateAtOffset(input, vec2(0.0))
#else
#define GE_SURFACE_INPUT_AT_CENTRE(input) (input)
#endif

layout(location = 0) GE_SURFACE_CENTROID in vec2 vUV0;

#if defined(HAS_NORMAL) || defined(HAS_VERTEX_OUTPUT_MODIFIER)
layout(location = 1) GE_SURFACE_CENTROID in vec3 vNormalWS;
#endif

layout(location = 2) GE_SURFACE_CENTROID in vec3 vPosWS;

#ifdef HAS_TANGENT
layout(location = 3) GE_SURFACE_CENTROID in vec4 vTangentWS;
#endif

#ifdef HAS_COLOR
layout(location = 4) GE_SURFACE_CENTROID in vec4 vColor;
#endif

#ifdef HAS_UV1
layout(location = 5) GE_SURFACE_CENTROID in vec2 vUV1;
#endif

flat layout(location = 6) in uint vMaterialIndex;

#ifdef GE_USER_PARTICLE_BUFFER
layout(location = 7) in vec4 vCustom0; // ribbon endpoint colors interpolate
#else
flat layout(location = 7) in vec4 vCustom0;
#endif

// Per-instance flags (bit0=castShadows, bit1=receiveShadows). Matches the vertex
// adapter's location 14 (above the conditional vUV2..vUV7 at 8..13).
#ifdef GE_LOD_CROSSFADE
flat layout(location = 14, component = 0) in uint vInstanceFlags;
// Packed LOD-crossfade code, sharing location 14 with the flags — see the
// vertex adapter for why it takes a component rather than a slot of its own.
flat layout(location = 14, component = 1) in uint vLodFadeCode;

// GE_LodCrossfadeKeep lives here: the depth prepass and the colour pass compile
// the SAME definition against the same record's code, which is what makes
// early-Z admit exactly the fragments the colour pass keeps.
#include "../Includes/lod_crossfade.glsl"
#else
flat layout(location = 14) in uint vInstanceFlags;
#endif

// Render-origin-relative world position (Earth-scale precision). The shadow
// receiver projects THIS through the rebased ge_shadowVP so caster and receiver
// agree at fp32-of-small-magnitude far from the origin (no self-shadow acne).
// Equals vPosWS when the origin is inactive. Matches the vertex adapter's loc 15.
layout(location = 15) GE_SURFACE_CENTROID in vec3 vPosRel;

#ifdef GE_USER_PARTICLE_BUFFER
flat layout(location = 8) in vec4 vParticleAnimation;
#if defined(GE_COMPAT_PROFILE)
// The basis as columns on the compatibility profile; see adapter_vertex.glsl.
flat layout(location = 9) in vec3 vParticleBasis0;
flat layout(location = 10) in vec3 vParticleBasis1;
flat layout(location = 11) in vec3 vParticleBasis2;
#else
flat layout(location = 9) in mat3 vParticleBasis;
#endif
flat layout(location = 12) in float vParticleColorScale;
flat layout(location = 13) in float vParticleNearFade;
#endif

#if defined(HAS_UV2) && !defined(GE_USER_PARTICLE_BUFFER)
layout(location = 8) GE_SURFACE_CENTROID in vec2 vUV2;
#endif

#if defined(HAS_UV3) && !defined(GE_USER_PARTICLE_BUFFER)
layout(location = 9) GE_SURFACE_CENTROID in vec2 vUV3;
#endif

#if defined(HAS_UV4) && !defined(GE_USER_PARTICLE_BUFFER)
layout(location = 10) GE_SURFACE_CENTROID in vec2 vUV4;
#endif

#if defined(HAS_UV5) && !defined(GE_USER_PARTICLE_BUFFER)
layout(location = 11) GE_SURFACE_CENTROID in vec2 vUV5;
#endif

#if defined(HAS_UV6) && !defined(GE_USER_PARTICLE_BUFFER)
layout(location = 12) GE_SURFACE_CENTROID in vec2 vUV6;
#endif

#if defined(HAS_UV7) && !defined(GE_USER_PARTICLE_BUFFER)
layout(location = 13) GE_SURFACE_CENTROID in vec2 vUV7;
#endif

// Depth-only fragment variants attach NO color target (DepthPrepass + the
// shadow cascades): declaring a Location-0 output there is a dead interface
// variable that spams Undefined-Value-ShaderOutputNotConsumed per draw under
// validation. GlassTint depth variants (GE_GLASS_SHADOW_COLOR) keep their
// transmittance attachment and so keep the output — the two defines are
// mutually exclusive (DepthDrawRecorder sets one or the other).
//
// The motion variant is a third shape: exactly one colour target, carrying the
// motion payload rather than shaded radiance. It is not a colour pass — no
// lighting, no image-based lighting, no reflection G-buffer — so it declares
// neither the shading output nor the reflection slices, either of which would
// be a dead interface variable on every draw it is compiled into.
//
// The three shapes are mutually exclusive, and every pair is refused by name
// rather than composed into whichever write happens to win or failing on an
// undeclared output that names neither keyword.
#if defined(GE_DEPTH_ONLY_FRAGMENT) && defined(GE_GLASS_SHADOW_COLOR)
#error "GE_DEPTH_ONLY_FRAGMENT and GE_GLASS_SHADOW_COLOR are different attachment shapes: a depth-only variant attaches NO colour target and the glass tint writes the transmittance attachment. The tint pass is a light-space cascade and draws no crossfade tail (DepthPassDrawsCrossfadeTails); compose the tint variant without GE_DEPTH_ONLY_FRAGMENT."
#endif
#ifdef GE_MOTION_VECTORS
#ifdef GE_DEPTH_ONLY_FRAGMENT
#error "GE_MOTION_VECTORS and GE_DEPTH_ONLY_FRAGMENT are different attachment shapes: a depth-only variant attaches NO colour target and a motion variant attaches exactly one. Compose the motion variant without GE_DEPTH_ONLY_FRAGMENT - it already runs the depth variant's coverage block."
#endif
#ifdef GE_GLASS_SHADOW_COLOR
#error "GE_MOTION_VECTORS and GE_GLASS_SHADOW_COLOR both claim the single colour output at location 0. The glass tint is a light-space cascade write and a motion variant is never drawn into a cascade; compose them as separate variants."
#endif

layout(location = 0) out vec4 oMotion;

// The PREVIOUS deformation endpoint's UNJITTERED clip position, packed into the
// four smooth components the 0..15 block leaves free — see the vertex adapter
// for which hosts they are and why the motion variant takes no location of its
// own. The current endpoint is gl_FragCoord.
layout(location = 0, component = 2) GE_SURFACE_CENTROID in vec2 vMotionPrevClipXY;
layout(location = 2, component = 3) GE_SURFACE_CENTROID in float vMotionPrevClipZ;
layout(location = 15, component = 3) GE_SURFACE_CENTROID in float vMotionPrevClipW;

// The per-view block: the fragment reads the viewport rect and the jitter from
// it, the vertex stage the previous view-projection and clock.
#include "../Includes/deformation_motion.glsl"
#include "../Includes/motion_vector_payload.glsl"

// The payload this fragment exports, from the one endpoint it carries and its
// own position. Named so the two write sites — the opaque early-out and the
// shared coverage block — cannot drift apart.
vec4 ge_DeformationMotionPayload()
{
    GE_ScreenRect viewport;
    viewport.originPx = MotionParams.uMotionViewportRect.xy;
    viewport.sizePx = MotionParams.uMotionViewportRect.zw;
    return GE_MotionVectorPayload(
        viewport, MotionParams.uMotionJitterUv.xy,
        vec4(vMotionPrevClipXY, vMotionPrevClipZ, vMotionPrevClipW));
}
#else
#ifndef GE_DEPTH_ONLY_FRAGMENT
layout(location = 0) out vec4 oColor;
// SSR G-buffer slices, attached by ReflectionsProvider only when SSSR is active
// for the view. Nested in the same guard as oColor: a depth-only variant
// attaches no color target at all, so these would be dead interface variables.
#ifdef GE_SSSR_NORMAL_ROUGHNESS
#include "../Includes/octahedral_normal.glsl"
layout(location = 1) out vec4 oNormalRoughness;
layout(location = 2) out vec4 oSpecularWeight;
layout(location = 3) out vec4 oSpecularRadiance;
#endif
#endif
#endif

// --- Includes ---

#include "../Includes/surface_io.glsl"

// The Parallax steps view's colour (GE_ParallaxStepsViewShade), for the variants that draw it.
#ifdef GE_PARALLAX_STEPS_VIEW
#include "../Includes/parallax_steps_view.glsl"
#endif

// The relief's depth. Reverse-Z: the relief is carved below the polygon, so its hit lies farther
// along the view ray and its depth is never larger than the fragment's own; depth_less tells the
// driver so, which keeps early depth testing sound under the GreaterOrEqual compare. A colour variant
// that reads the prepass's depth tests at exactly the depth it read: one value for the whole pixel, as
// the prepass wrote it, where the polygon's own depth would differ from sample to sample.
#ifdef GE_PARALLAX_RELIEF_DEPTH
layout(depth_less) out float gl_FragDepth;
#endif

// The depth the camera prepass wrote, attached read-only to this pass: a colour variant that reads the
// relief's depth (ParallaxDepthFromPrepass) rebuilds its hit from it. Multisampled, the relief's sample
// is the farthest covered one: every covered sample holds this surface's hit or something nearer.
#ifdef GE_PARALLAX_READS_DEPTH
  #ifdef GE_PARALLAX_PREPASS_DEPTH_MULTISAMPLE
layout(set = 0, binding = 49) uniform sampler2DMS ge_prepassDepth;
  #else
layout(set = 0, binding = 49) uniform sampler2D ge_prepassDepth;
  #endif
#endif

// Geometric specular AA (GE_ApplySpecularAA). Self-contained — no descriptor bindings;
// needs SurfaceOutput from surface_io.glsl above. Included unconditionally (the guard
// makes it free); the call site gates the work to the lit, normal-bearing path.
#include "../Includes/specular_aa.glsl"

// Glass caustic helpers (GE_GlassFocus / GE_GlassCaustic + constants). Self-contained — no
// descriptor bindings — and included unconditionally so the glass-tint WRITE path
// (GE_GLASS_SHADOW_COLOR) resolves them: that variant pulls in neither shadow_sampling.glsl
// nor clustered_lighting.glsl (the receiver paths that otherwise bring caustics.glsl in
// transitively). The include guard makes the transitive double-include a no-op.
#include "../Includes/caustics.glsl"

// clustered_lighting.glsl references GE_EvaluateSurfaceLight; include it whenever that header is
// compiled in.
#if defined(LIGHTING_MODEL_STANDARD_PBR) \
    || (defined(FORWARD_PLUS) && defined(LIGHTING_MODEL_SHADOW_ONLY))
#include "../Includes/surface_light_response.glsl"
#endif

// (clustered_lighting.glsl is included below, AFTER LightUBO is declared,
//  so GE_AccumulateClusteredLighting can shade the directional light from
//  LightUBO outside the cluster loop. Directionals are no longer in the
//  cluster index list as of the Phase D step 1 directional-out-of-band change.)

// Shadow sampling for the non-clustered-lighting path. The Forward+ lit arm is
// excluded because clustered_lighting.glsl — included below for exactly that arm —
// includes shadow_sampling.glsl itself whenever HAS_SHADOWS is set.
#if defined(HAS_SHADOWS) \
    && !(defined(FORWARD_PLUS) && (defined(LIGHTING_MODEL_STANDARD_PBR) || defined(LIGHTING_MODEL_SHADOW_ONLY)))
#include "../Includes/shadow_sampling.glsl"
#endif

// --- Engine resources ---

#if !defined(FORWARD_PLUS) \
    || (!defined(LIGHTING_MODEL_STANDARD_PBR) && !defined(LIGHTING_MODEL_SHADOW_ONLY))
layout(set = 0, binding = 5) uniform CameraUBO {
#include "../Includes/camera_ubo_fields.glsl"
} Cam;
#endif

// Always bound: per-view light data (directionals + ambient). Layout is locked
// against the C++ mirror ForwardLightUBO (RenderServices.h) by static_asserts.
// Secondary directionals are additive and UNSHADOWED (cascades belong to the
// primary only); their colors arrive premultiplied by intensity.
layout(set = 0, binding = 6) uniform LightUBO {
    mat4 uLightVP;
    vec4 uLightDirWorld;    // xyz=direction, w=intensity (primary)
    vec4 uLightColorWorld;  // xyz=color, w=castsShadows (1.0 if true) (primary)
    vec4 uAmbient;          // xyz=color, w=intensity
    vec4 uTimeParams;       // x=elapsed time (seconds), yzw reserved
    vec4 uSecondaryCount;   // x=valid secondary directional count (as float)
    vec4 uSecondaryDirs[3];   // xyz=light->surface direction
    vec4 uSecondaryColors[3]; // rgb=color*intensity (premultiplied)
} Light;

#include "../Includes/view_params.glsl"

// Forward+ clustered lighting (when available and a lit model is active).
// Included AFTER LightUBO so GE_AccumulateClusteredLighting can read
// Light.uLightDirWorld / uLightColorWorld for the out-of-band directional.
#if defined(FORWARD_PLUS) && (defined(LIGHTING_MODEL_STANDARD_PBR) || defined(LIGHTING_MODEL_SHADOW_ONLY))
#include "../Includes/clustered_lighting.glsl"
#endif

// Image-based lighting (M1). Declared only for variants with an IBL consumer
// so the descriptor set-0 layout stays minimal for all other materials.
#if defined(GE_IBL_ENABLED) && defined(LIGHTING_MODEL_STANDARD_PBR)
#include "../Includes/ibl.glsl"
#endif

// Screen-space GTAO consumer (declares ge_gtao at set-0 b27 and GE_ApplyGTAO). Only
// the GTAO variant declares it, so non-AO pipelines reflect no b27 binding and pay no
// fetch; the RenderServices ge_gtao bind is gated on the same MaterialKeyword::GTAO.
#if defined(LIGHTING_MODEL_STANDARD_PBR) && defined(GE_GTAO_ENABLED)
#include "../Includes/gtao_consume.glsl"
#endif

// --- Material data ---

// Descriptor indexing does not exist on the compat target; the material
// texture bindings there are fixed (bindless_textures.glsl).
#if !defined(GE_COMPAT_PROFILE)
#extension GL_EXT_nonuniform_qualifier : require
#endif

// Material params + texture indices + texture ST from the shared SSBO,
// indexed per-instance by vMaterialIndex.
struct MaterialData
{
    // Param block — shared field list (see Includes/material_params.glsl).
#define GE_FIELD(type, name) type name;
#include "../Includes/material_params.glsl"
#undef GE_FIELD
    uint TextureIndices[8];
    vec4 TextureST[8];  // per-texture affine row 0: x'=dot(vec3(uv,1), xyz)
    vec4 TextureST2[8]; // per-texture affine row 1: y'=dot(vec3(uv,1), xyz)
    uint SamplerIndices; // 8 × 4-bit sampler-preset indices into ge_BindlessSamplers (slot i = bits[i*4..i*4+4))
};

layout(std430, set = 0, binding = 13) readonly buffer MaterialParamsSSBO
{
    MaterialData ge_Materials[];
} MaterialParams;
#include "../Includes/material_row_index.glsl"

// Global bindless arrays (set 1) — single declaration site (guarded include).
#include "../Includes/bindless_textures.glsl"

// Runtime-populated from SSBO in main().
MaterialData ge_MatData;
#define Mat ge_MatData
#include "../Includes/material_param_lanes.glsl"

// The adapter's own material reads, declared like any other producer's. A
// surface that declares one of these names (same type) shares the slot; a name
// no surface declares reads its default as a compile-time constant.
// @property float alphaCutoff        "Alpha Cutoff" default=0.5 range=0,1 visibleIf=alphaMode=Mask
// @property float specularIor        default=1.5 hidden
// @property color transmissionColor  default=1,1,1 hidden
// @property float transmissionWeight default=0 hidden

// The composer replaces the marker line below with this program's GE_Props
// struct, the `Props` accessor and GE_LoadDeclaredProperties() — see
// ShaderComposer::GenerateDeclaredPropertiesBlock.
// GE_DECLARED_PROPERTIES

// Raw bindless-slot access (dynamic-ordinal helpers: triplanar/collapse). Use only
// as a direct sampler2D(...) constructor operand inside a texture()/textureLod()
// call — never assign to a sampler2D local (the nonuniformEXT decoration must land
// on the sampled operand).
#if !defined(GE_COMPAT_PROFILE)
#define GE_SLOT_SAMPLER(slot) ge_BindlessSamplers[nonuniformEXT((ge_MatData.SamplerIndices >> ((slot)*4u)) & 0xFu)]
#define GE_SLOT_TEX(slot)     ge_BindlessTextures[nonuniformEXT(ge_MatData.TextureIndices[slot])]
#endif

// Texture aliases: surface shaders use these names unchanged. Each expands to a
// GE_MaterialTexture — the slot's bindless texture index plus its sampler index
// (from the material's packed SamplerIndices) — consumed by the texture()/
// textureLod() overloads below. Use only as a direct texture()/textureLod()
// operand.
// Compat carries the slot ordinal itself: the texture and its sampler are
// bound at fixed slot-indexed bindings, so there is no index to look up.
#if defined(GE_COMPAT_PROFILE)
struct GE_MaterialTexture
{
    uint Slot;
};
#else
struct GE_MaterialTexture
{
    uint TexIdx;
    uint SamplerIdx;
};
#endif
// Signals includes composed after this point (shader-graph node library) that
// they may declare GE_MaterialTexture overloads.
#define GE_MATERIAL_TEXTURE_DEFINED 1
#if defined(GE_COMPAT_PROFILE)
#define GE_MATERIAL_TEXTURE(slot) GE_MaterialTexture(uint(slot))
#else
#define GE_MATERIAL_TEXTURE(slot) GE_MaterialTexture(ge_MatData.TextureIndices[slot], (ge_MatData.SamplerIndices >> ((slot)*4u)) & 0xFu)
#endif
#define albedoMap             GE_MATERIAL_TEXTURE(0)
#define normalMap             GE_MATERIAL_TEXTURE(1)
#define metallicRoughnessMap  GE_MATERIAL_TEXTURE(2)
#define emissiveMap           GE_MATERIAL_TEXTURE(3)
#define aoMap                 GE_MATERIAL_TEXTURE(4)
#define coatNormalMap         GE_MATERIAL_TEXTURE(5)
#define roughnessMap          GE_MATERIAL_TEXTURE(6)
#define metallicMap           GE_MATERIAL_TEXTURE(7)

// User-declared texture slots (project surfaces). A surface declares its set with
// `// @texture <name>` comment tags; the composer resolves each name to an ordinal
// and injects `#define GE_TEXSLOT_<name> <ordinal>` above. GE_USER_TEXTURE(name)
// builds the GE_MaterialTexture for a declared name exactly like the fixed aliases,
// routing through that composed ordinal. Use only as a direct texture()/textureLod()
// operand.
#define GE_USER_TEXTURE(name) GE_MATERIAL_TEXTURE(GE_TEXSLOT_##name)

// Material texture sampling. Every slot-alias sample funnels through these
// overloads (user overloads of the builtins on a novel first-parameter type),
// so the per-view TAAU mip bias (ge_mipBiasParams.x, ViewParams) applies at
// every material sampling site and ONLY there — screen grabs, environment
// cubes, and shadow maps resolve to the builtins unbiased. nonuniformEXT is
// applied here, at the descriptor access, so the NonUniform decoration lands
// on the sampled operand regardless of the call shape.
#if defined(GE_COMPAT_PROFILE)
vec4 texture(GE_MaterialTexture t, vec2 uv)
{
    return ge_CompatSampleSlot(t.Slot, uv, ge_mipBiasParams.x);
}
#else
vec4 texture(GE_MaterialTexture t, vec2 uv)
{
    return texture(sampler2D(ge_BindlessTextures[nonuniformEXT(t.TexIdx)],
                             ge_BindlessSamplers[nonuniformEXT(t.SamplerIdx)]),
                   uv, ge_mipBiasParams.x);
}
#endif

// Explicit-gradient variant of the alias contract, for samples at a UV whose own
// derivatives are meaningless (the relief march's displaced UV): the caller supplies
// the footprint in repeats per pixel along x and y. The view's TAAU bias scales it,
// so a gradient sample and a texture() sample of the same footprint pick the same mip.
#if defined(GE_COMPAT_PROFILE)
vec4 textureGrad(GE_MaterialTexture t, vec2 uv, vec2 uvPerPixelX, vec2 uvPerPixelY)
{
    float biasScale = exp2(ge_mipBiasParams.x);
    return ge_CompatSampleSlotGrad(t.Slot, uv, uvPerPixelX * biasScale, uvPerPixelY * biasScale);
}
#else
vec4 textureGrad(GE_MaterialTexture t, vec2 uv, vec2 uvPerPixelX, vec2 uvPerPixelY)
{
    float biasScale = exp2(ge_mipBiasParams.x);
    return textureGrad(sampler2D(ge_BindlessTextures[nonuniformEXT(t.TexIdx)],
                                 ge_BindlessSamplers[nonuniformEXT(t.SamplerIdx)]),
                       uv, uvPerPixelX * biasScale, uvPerPixelY * biasScale);
}
#endif

// A material texture's size at a mip level, in texels.
#if defined(GE_COMPAT_PROFILE)
ivec2 textureSize(GE_MaterialTexture t, int lod)
{
    return ge_CompatSlotSize(t.Slot, lod);
}
#else
ivec2 textureSize(GE_MaterialTexture t, int lod)
{
    return textureSize(ge_BindlessTextures[nonuniformEXT(t.TexIdx)], lod);
}
#endif

// Explicit-LOD variant of the alias contract. The caller pinned the LOD, so the
// TAAU bias intentionally does not apply.
#if defined(GE_COMPAT_PROFILE)
vec4 textureLod(GE_MaterialTexture t, vec2 uv, float lod)
{
    return ge_CompatSampleSlotLod(t.Slot, uv, lod);
}
#else
vec4 textureLod(GE_MaterialTexture t, vec2 uv, float lod)
{
    return textureLod(sampler2D(ge_BindlessTextures[nonuniformEXT(t.TexIdx)],
                                ge_BindlessSamplers[nonuniformEXT(t.SamplerIdx)]),
                      uv, lod);
}
#endif

// --- Surface shader include ---

#ifndef GE_SURFACE_SHADER_PATH
#define GE_SURFACE_SHADER_PATH "../Surfaces/standard_surface.glsl"
#endif
#include GE_SURFACE_SHADER_PATH

// A surface whose emission follows the view's exposure only partly defines
// GE_SURFACE_EMISSIVE_EXPOSURE_WEIGHT as the expression that reads its weight, in [0, 1]. The
// shading shape then reads the view's exposure (one frame behind the tonemap: the forward passes run
// before this frame's metering) to scale the emission by exposure^(weight - 1). Every other surface,
// and every shape that shades no colour, reads no exposure.
#if defined(GE_SURFACE_EMISSIVE_EXPOSURE_WEIGHT) && !defined(GE_DEPTH_ONLY_FRAGMENT) && \
    !defined(GE_MOTION_VECTORS) && !defined(GE_GLASS_SHADOW_COLOR)
#define GE_EMISSION_FOLLOWS_EXPOSURE_WEIGHT
#include "../Includes/view_exposure.glsl"
#endif

// A surface whose coverage the colour variant resolves by alpha-to-coverage (it defines
// GE_SURFACE_ALPHA_TO_COVERAGE under the keyword that selects that pipeline, as terrain grass's
// GRASS_A2C does) gets a depth-only variant that writes the same alpha at location 0. The hardware
// derives the sample mask from that output whether or not a colour target is attached, so a depth
// pipeline that enables alpha-to-coverage keeps exactly the samples the colour draw keeps. The value
// lands in no attachment.
#if defined(GE_DEPTH_ONLY_FRAGMENT) && defined(GE_SURFACE_ALPHA_TO_COVERAGE)
layout(location = 0) out vec4 oCoverageAlpha;
#endif

#ifdef GE_PARALLAX_RELIEF_DEPTH
#include "../Includes/parallax_depth.glsl"
#endif

#ifdef GE_PARALLAX_WRITES_DEPTH
// The fragment depth a marching variant writes (Includes/parallax_depth.glsl), from this fragment's
// depth and the view's projection: the hit's depth, in the prepass and in a colour pass that is the
// depth's sole writer; the tolerant test value in a colour pass after a prepass whose depth it cannot
// read (GE_PARALLAX_DEPTH_TOLERANCE, depth write off).
float GE_ParallaxFragmentDepth(SurfaceOutput so, float eyeDistance)
{
    bool perspective = ge_proj[2][3] != 0.0;
  #ifdef GE_PARALLAX_DEPTH_TOLERANCE
    return GE_ParallaxToleratedDepth(gl_FragCoord.z, ge_proj[2][2], perspective, so.depthOffset,
                                     so.depthOffsetStep, eyeDistance);
  #else
    return GE_ParallaxExactDepth(gl_FragCoord.z, ge_proj[2][2], perspective, so.depthOffset, eyeDistance);
  #endif
}
#endif

#ifdef GE_PARALLAX_READS_DEPTH
// The depth the prepass wrote at this pixel, never nearer than this fragment's own: the relief's hit,
// unless something nearer covers the pixel. Multisampled, the farthest covered sample: each covered
// sample holds this surface's hit or something nearer.
float GE_ParallaxPrepassDepth()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
  #ifdef GE_PARALLAX_PREPASS_DEPTH_MULTISAMPLE
    float depth = gl_FragCoord.z;
    int samples = textureSamples(ge_prepassDepth);
    for (int s = 0; s < samples; ++s)
    {
        if ((gl_SampleMaskIn[0] & (1 << s)) != 0)
            depth = min(depth, texelFetch(ge_prepassDepth, pixel, s).r);
    }
    return depth;
  #else
    return min(gl_FragCoord.z, texelFetch(ge_prepassDepth, pixel, 0).r);
  #endif
}
#endif

// --- Main ---

void main()
{
#if defined(HAS_SHADOWS) && defined(GE_COMPAT_PROFILE)
    ge_shadowReceiverDx = dFdx(vPosRel);
    ge_shadowReceiverDy = dFdy(vPosRel);
#endif
#if defined(HAS_SHADOWS) && defined(GE_SCREEN_SPACE_SHADOWS_ENABLED)
    ge_contactReceiverDepthFootprint = fwidth(gl_FragCoord.z);
#endif
#ifdef GE_PARALLAX_MARCH
    // The relief march's footprint, taken before any branch: WGSL admits derivatives only in
    // uniform control flow, and the march itself runs in non-uniform flow. uv0 in repeats and the
    // render-origin-relative position in metres (the precise one far from the origin), per pixel.
    // Both at the pixel centre, so the footprint holds along a triangle's MSAA edge pixels.
    vec2 parallaxUv0AtCentre = GE_SURFACE_INPUT_AT_CENTRE(vUV0);
    vec3 parallaxPositionAtCentre = GE_SURFACE_INPUT_AT_CENTRE(vPosRel);
    vec4 parallaxUvFootprint = vec4(dFdx(parallaxUv0AtCentre), dFdy(parallaxUv0AtCentre));
    vec3 parallaxPositionFootprintX = dFdx(parallaxPositionAtCentre);
    vec3 parallaxPositionFootprintY = dFdy(parallaxPositionAtCentre);
#endif
#if defined(GE_DEPTH_ONLY_FRAGMENT) && defined(GE_LOD_CROSSFADE) && !defined(ALPHA_TEST)     && !defined(GE_PARALLAX_WRITES_DEPTH) \
    && !defined(GE_SURFACE_ALPHA_TO_COVERAGE)
    // Crossfade-only depth fragment (the prepass draw of a fading tail on an
    // opaque material). Coverage is the dither alone, so nothing below this
    // point can change it: no material fetch, no surface eval, no varying use
    // beyond the fade code. An opaque material's depth draw has no fragment
    // shader at all when it is not fading, so this is the whole added cost and
    // it is paid only by tail records. A tail that writes the relief's depth
    // takes the full path: its depth comes from the surface, and the shared
    // coverage block below runs the same dither.
    if (!GE_LodCrossfadeKeep(vLodFadeCode, gl_FragCoord.xy))
        discard;
    return;
#endif
#if defined(GE_MOTION_VECTORS) && !defined(ALPHA_TEST)
    // Opaque motion draw. Coverage is the crossfade dither alone, so nothing
    // below this point can change it: no material fetch, no surface evaluation,
    // no varying use beyond the fade code and the previous endpoint.
  #ifdef GE_LOD_CROSSFADE
    if (!GE_LodCrossfadeKeep(vLodFadeCode, gl_FragCoord.xy))
        discard;
  #endif
    oMotion = ge_DeformationMotionPayload();
    return;
#endif

#ifdef GE_SSSR_NORMAL_ROUGHNESS
    // Safe value for every adapter early-out below (glass transmittance, alpha
    // discard, debug); regular opaque shading overwrites it once EvaluateSurface
    // has finalized the normal. .rg = oct(+Z view normal) = (0.5,0.5);
    // .b = roughness 1 (SSR classify skips it); .a = metallic 0.
    oNormalRoughness = vec4(0.5, 0.5, 1.0, 0.0);
    oSpecularWeight = vec4(0.0);
    oSpecularRadiance = vec4(0.0);
#endif
    // Fetch material data from shared SSBO using per-instance material index.
    ge_MatData = MaterialParams.ge_Materials[GE_MATERIAL_ROW_INDEX(
        MaterialParams.ge_Materials, vMaterialIndex)];
    GE_LoadDeclaredProperties();

#ifdef GE_GLASS_SHADOW_COLOR
    // Light-space glass transmittance (translucent shadows): emit the tinted
    // fraction of light that survives the glass — transmissionColor × weight —
    // into the per-cascade transmittance array. Multiplicatively blended so stacked
    // glass compounds (Beer-Lambert product); the receiver multiplies its lit colour
    // by this sample. White (= no glass) leaves the light unchanged. No surface eval.
    //
    // SURFACE TINT ONLY. The Beer-Lambert VOLUME colour (attenuationColor/attenuationDistance,
    // uParams21) is not in this write and has no route into the cascade — absorption needs the
    // in-glass path length in light space, and this is one unpaired front-face fragment with no
    // back face to difference against. So a glass whose colour lives in attenuationColor with
    // transmissionColor left at white (Examples/Materials/OpenPBR/glass_thick_absorbing) reads
    // deep green through and casts a COLOURLESS shadow. Give it a transmissionColor if you want
    // its shadow coloured.
    //
    // Alpha (C1.5): the refraction convergence at this glass texel — how sharply it focuses
    // the light. GE_GlassFocus returns [1, kGlassCausticFocusMax]; dividing by the cap encodes
    // it as [1/cap, 1] for the 8-bit MAX-blended presence/focus mask (0 = no glass; gates the
    // receiver caustic). The cascade upload carries the light direction in Cam.uCameraPos
    // (unused for an ortho light); IOR = Props.specularIor.
    //
    // While the dapple is off (kGlassCausticStrength = 0, caustics.glsl) the focus term
    // constant-folds out here too — its only consumer is the receiver caustic, so the
    // cascade pass pays none of its derivative work. Flat focus (1.0) keeps the encode
    // contract for re-enablement.
#if defined(HAS_NORMAL)
    float glassFocus = (kGlassCausticStrength > 0.0)
        ? GE_GlassFocus(vPosWS, vNormalWS, Cam.uCameraPos.xyz, Props.specularIor)
        : 1.0;
#else
    float glassFocus = 1.0; // no normals -> flat (parity with the un-focused dapple)
#endif
    oColor = vec4(Props.transmissionColor * Props.transmissionWeight, glassFocus / kGlassCausticFocusMax);
    return;
#endif

    // Build SurfaceInput from varyings.
    SurfaceInput si = DefaultSurfaceInput();

#if defined(HAS_UV0) || defined(HAS_VERTEX_OUTPUT_MODIFIER)
    si.uv0 = vUV0;
#endif

    si.positionWS = vPosWS;
    si.positionRelWS = vPosRel; // == vPosWS when the render origin is inactive

#if defined(HAS_NORMAL) || defined(HAS_VERTEX_OUTPUT_MODIFIER)
    si.normalWS = normalize(vNormalWS);
#endif

    si.custom0 = vCustom0;
#ifdef GE_USER_PARTICLE_BUFFER
    si.particleAnimation = vParticleAnimation;
#if defined(GE_COMPAT_PROFILE)
    si.particleBasis = mat3(vParticleBasis0, vParticleBasis1, vParticleBasis2);
#else
    si.particleBasis = vParticleBasis;
#endif
    si.particleColorScale = vParticleColorScale;
    si.particleNearFade = vParticleNearFade;
#endif

#ifdef HAS_COLOR
    si.vertexColor = vColor;
#endif

#ifdef HAS_UV1
    si.uv1 = vUV1;
#endif

#if defined(HAS_UV2) && !defined(GE_USER_PARTICLE_BUFFER)
    si.uv2 = vUV2;
#endif

#if defined(HAS_UV3) && !defined(GE_USER_PARTICLE_BUFFER)
    si.uv3 = vUV3;
#endif

#if defined(HAS_UV4) && !defined(GE_USER_PARTICLE_BUFFER)
    si.uv4 = vUV4;
#endif

#if defined(HAS_UV5) && !defined(GE_USER_PARTICLE_BUFFER)
    si.uv5 = vUV5;
#endif

#if defined(HAS_UV6) && !defined(GE_USER_PARTICLE_BUFFER)
    si.uv6 = vUV6;
#endif

#if defined(HAS_UV7) && !defined(GE_USER_PARTICLE_BUFFER)
    si.uv7 = vUV7;
#endif

#ifdef HAS_TANGENT
    si.tangentWS = vTangentWS;
    // Build TBN matrix from tangent + normal.
    vec3 T = normalize(vTangentWS.xyz);
    vec3 N = si.normalWS;
    vec3 B = cross(N, T) * vTangentWS.w;
    si.TBN = mat3(T, B, N);
#endif

    // View direction.
#if defined(FORWARD_PLUS) \
    && (defined(LIGHTING_MODEL_STANDARD_PBR) || defined(LIGHTING_MODEL_SHADOW_ONLY))
    // With Forward+ lit path, camera position is precomputed in ViewParams UBO.
    vec3 camPosWS = ge_cameraPosWS.xyz;
#else
    vec3 camPosWS = Cam.uCameraPos.xyz;
#endif
    si.viewDirWS = normalize(camPosWS - vPosWS);
    si.screenUV = gl_FragCoord.xy * ge_screenSize.zw;
    si.linearDepth = (ge_view * vec4(vPosWS, 1.0)).z;

    // Populate per-texture UV transforms.
    for (int i = 0; i < 8; ++i)
    {
        si.textureST[i] = ge_MatData.TextureST[i];
        si.textureST2[i] = ge_MatData.TextureST2[i];
    }

#ifdef GE_PARALLAX_MARCH
    si.uvFootprint = parallaxUvFootprint;
    si.positionFootprintX = parallaxPositionFootprintX;
    si.positionFootprintY = parallaxPositionFootprintY;
#endif
#if defined(GE_PARALLAX_MARCH) && !defined(GE_DEPTH_ONLY_FRAGMENT)
    // The relief's self-shadow marches toward the light both lighting paths below shade as the
    // shadowed directional term, gated like them on the light it delivers. The prepass shades
    // nothing, so its direction stays zero and the self-shadow skips.
    {
        vec3 primaryLight = Light.uLightColorWorld.rgb * Light.uLightDirWorld.w;
        si.primaryLightDirectionWS = max(primaryLight.r, max(primaryLight.g, primaryLight.b)) > 0.0
            ? normalize(-Light.uLightDirWorld.xyz)
            : vec3(0.0);
    }
#endif

#ifdef GE_PARALLAX_READS_DEPTH
    float parallaxPrepassDepth = GE_ParallaxPrepassDepth();
    {
        bool perspective = ge_proj[2][3] != 0.0;
        float eyeDistance = length(camPosWS - vPosWS);
        si.prepassDepthOffset = GE_ParallaxOffsetToDepth(gl_FragCoord.z, ge_proj[2][2], perspective,
                                                         parallaxPrepassDepth, eyeDistance);
        si.prepassDepthOffsetError = GE_ParallaxOffsetError(gl_FragCoord.z, ge_proj[2][2], perspective,
                                                            parallaxPrepassDepth, eyeDistance);
    }
#endif

    // Call user surface shader.
    SurfaceOutput so = EvaluateSurface(si);

#ifdef GE_PARALLAX_READS_DEPTH
    // Tested at the depth read: the samples holding this surface's hit pass, a nearer surface's fail.
    gl_FragDepth = parallaxPrepassDepth;
    // Something nearer stands inside the relief here (a prop sunk into it): its depth is what the
    // prepass kept, and the relief's colour must not cover it.
    if (so.reliefHidden)
        discard;
#endif

#ifdef GE_PARALLAX_WRITES_DEPTH
    // Written before any discard, on every path that reaches the end, so no covered sample keeps an
    // undefined depth.
    gl_FragDepth = GE_ParallaxFragmentDepth(so, length(camPosWS - vPosWS));
#endif

#if defined(GE_DEPTH_ONLY_FRAGMENT) || defined(GE_MOTION_VECTORS)
  // Coverage, shared by every variant that decides it and shades nothing: the
  // depth prepass, the shadow cascades and the motion variant. One block, so a
  // motion vector is written for exactly the fragments the prepass gave depth.
  #ifdef ALPHA_TEST
    if (so.opacity < clamp(Props.alphaCutoff, 0.0, 1.0))
        discard;
  #endif
  #ifdef GE_LOD_CROSSFADE
    // Same call, same inputs, same file as the colour pass's discard below: the
    // prepass writes depth for exactly the fragments the colour pass will keep,
    // so each level of a fading pair is admitted by early-Z on its own dither
    // half and neither ever writes where the other does. Reached only on the
    // masked path — an opaque crossfade tail took the early-out at the top of
    // main() and never evaluated the surface.
    if (!GE_LodCrossfadeKeep(vLodFadeCode, gl_FragCoord.xy))
        discard;
  #endif
  #ifdef GE_MOTION_VECTORS
    // Masked motion draw: the payload is exported after the same cutoff the
    // prepass ran, so the deforming surface and its depth agree on coverage.
    oMotion = ge_DeformationMotionPayload();
  #endif
  #if defined(GE_DEPTH_ONLY_FRAGMENT) && defined(GE_SURFACE_ALPHA_TO_COVERAGE)
    // The alpha the shading path writes (outputAlpha), which alpha-to-coverage turns into this
    // fragment's sample mask.
    oCoverageAlpha = vec4(0.0, 0.0, 0.0, so.opacity);
  #endif
#else // the shading path — the only one that declares oColor

    so.normalWS = normalize(so.normalWS);
#ifndef GE_TWO_SIDED_KEEP_NORMAL
    if (!gl_FrontFacing)
        so.normalWS = -so.normalWS;
#endif
#ifdef GE_COAT_NORMAL_ENABLED
    // The coat normal rides the same surface: re-normalize and apply the identical two-sided
    // flip so the coat lobe agrees with the (already-flipped) base normal on backfaces.
    so.coatNormalWS = normalize(so.coatNormalWS);
  #ifndef GE_TWO_SIDED_KEEP_NORMAL
    if (!gl_FrontFacing)
        so.coatNormalWS = -so.coatNormalWS;
  #endif
#endif
    // Default the bent normal to the shading normal; GTAO refines it below. Keeps
    // GE_EvaluateIBL's irradiance lookup identical to N on non-GTAO pipelines.
    so.bentNormalWS = so.normalWS;

    // Geometric specular AA: floor roughness by the screen-space normal variance now
    // that so.normalWS is final, before any lobe / IBL-LOD reads so.roughness.
#if defined(LIGHTING_MODEL_STANDARD_PBR) && defined(HAS_NORMAL)
    GE_ApplySpecularAA(so);
#endif

    // Coat roughening: a rough clear coat blurs the base reflection. Raise so.roughness by the
    // coat variance now (unconditional, not HAS_NORMAL-gated) before any lobe / IBL-LOD reads it.
#if defined(LIGHTING_MODEL_STANDARD_PBR) && defined(GE_CLEARCOAT_ENABLED)
    GE_ApplyCoatRoughening(so);
#endif

#ifdef GE_SSSR_NORMAL_ROUGHNESS
    // Material-derived signal for SSSR, written AFTER the roughness modifiers so
    // the SSR lobe matches the one the BRDF and IBL actually shade with: the
    // specular-AA floor and coat roughening both widen so.roughness, and a
    // narrower SSR lobe would re-introduce the specular aliasing the AA floor
    // exists to suppress. Normals are stored in VIEW space so the stochastic
    // intersection and denoise taps avoid a matrix multiply per sample.
    vec3 sssrNormalVS = normalize(mat3(ge_view) * so.normalWS);
    // .rg = octahedral view normal, .b = roughness, .a = metallic. SSR reads the
    // normal and the roughness; the DDGI glossy resolve reads the normal.
    oNormalRoughness = vec4(GE_OctEncode(sssrNormalVS), clamp(so.roughness, 0.0, 1.0),
                            clamp(so.metallic, 0.0, 1.0));
#endif

    // Per-instance receiveShadows (instance flags bit 1): when clear, the global
    // ge_ReceiveShadows makes GE_SampleShadow return fully-lit (1.0) so this mesh
    // ignores all cast shadows. The global lives in shadow_sampling.glsl, whose
    // include above is conditional — so gate on the declaration's own
    // announcement rather than on a second copy of those conditions. A variant
    // without the header has no shadow sampler to read the mask either.
#ifdef GE_RECEIVE_SHADOWS_DECLARED
    ge_ReceiveShadows = ((vInstanceFlags & 2u) != 0u) ? 1.0 : 0.0;
#endif

    // --- Evaluate lighting ---

    vec3 finalColor = vec3(0.0);
    vec3 V = normalize(si.viewDirWS);
    float ambientMultiplier = 1.0;
    float emissiveMultiplier = 1.0;
    float outputAlpha = so.opacity;

#ifdef LIGHTING_MODEL_STANDARD_PBR

  #ifdef FORWARD_PLUS
    // Forward+ path: traverse the cluster's light list.
    // Use view-space Z (not Euclidean distance) so cascade splits match.
    float linearDepth = (ge_view * vec4(vPosWS, 1.0)).z;
    finalColor = GE_AccumulateClusteredLighting(
        so, V, vPosWS, vPosRel,
        gl_FragCoord.xy, linearDepth);
  #else
    // Fallback: single directional light.
    vec3 L = normalize(-Light.uLightDirWorld.xyz);
    g_BaseMultiscatter = GE_ComputeBaseMultiscatter(
        mix(GE_DielectricF0(so.specularWeight, so.specularColor, so.specularIor), so.baseColor, so.metallic),
        max(dot(so.normalWS, V), 0.0), so.roughness);
    float shadowFactor = 1.0;
    vec3 shadowTint = vec3(1.0); // glass transmittance between the light and this surface
    #ifdef HAS_SHADOWS
    // Use view-space Z (not Euclidean distance) so cascade splits match.
    float viewZ = (Cam.uV * vec4(vPosWS, 1.0)).z;
    shadowFactor = GE_SampleShadow(vPosRel, so.normalWS, viewZ, L, gl_FragCoord.xy);
    shadowTint = ge_lastShadowTint;
    #endif
    #ifdef GE_PARALLAX_MARCH
    // The surface's own relief shadows the primary light as its cascade shadow does.
    shadowFactor *= so.primaryLightOcclusion;
    #endif
    // Subsurface transmission added outside shadowFactor/shadowTint so back-lit thin objects glow through.
    finalColor = GE_EvaluateSurfaceLight(so, V, L)
                 * Light.uLightColorWorld.rgb * Light.uLightDirWorld.w * shadowFactor * shadowTint
                 + GE_SubsurfaceTransmission(so, V, L)
                 * Light.uLightColorWorld.rgb * Light.uLightDirWorld.w;
    #ifdef HAS_SHADOWS
    // Glass caustics: focused-light dapple (see GE_GlassCaustic) — gated by presence + NdotL,
    // tinted by the glass, x the shadow factor so there is no caustic in an opaque shadow, and
    // returned through the receiver's own diffuse albedo.
    finalColor += GE_GlassCaustic(vPosWS, so.normalWS, L, ge_lastGlassPresence,
                                  shadowTint, so.baseColor * (1.0 - so.metallic),
                                  Light.uLightColorWorld.rgb, Light.uLightDirWorld.w, shadowFactor);
    #endif
    // Secondary directionals: same BRDF, additive, UNSHADOWED (cascades belong to
    // the primary only). Colors arrive premultiplied (color * intensity).
    {
        int secondaryCount = int(Light.uSecondaryCount.x);
        for (int i = 0; i < secondaryCount; ++i)
        {
            vec3 secL = normalize(-Light.uSecondaryDirs[i].xyz);
            vec3 secCol = Light.uSecondaryColors[i].rgb;
            finalColor += GE_EvaluateSurfaceLight(so, V, secL) * secCol
                        + GE_SubsurfaceTransmission(so, V, secL) * secCol;
        }
    }
  #endif

#elif defined(LIGHTING_MODEL_SHADOW_ONLY)
    // Shadow catcher: transparent where lit, tinted only where the directional
    // cascaded shadow map reports occlusion. Requires Blend alpha mode.
    float shadowFactorRs = 1.0;
#ifdef HAS_SHADOWS
    vec3 Lrs = normalize(-Light.uLightDirWorld.xyz);
  #ifdef FORWARD_PLUS
    float linearDepthRs = (ge_view * vec4(vPosWS, 1.0)).z;
    shadowFactorRs = GE_SampleShadow(vPosRel, so.normalWS, linearDepthRs, Lrs, gl_FragCoord.xy);
  #else
    float viewZRs = (Cam.uV * vec4(vPosWS, 1.0)).z;
    shadowFactorRs = GE_SampleShadow(vPosRel, so.normalWS, viewZRs, Lrs, gl_FragCoord.xy);
  #endif
#endif
    float shadowOpacity = clamp(1.0 - shadowFactorRs, 0.0, 1.0) * so.opacity;
    finalColor = so.baseColor;
    outputAlpha = shadowOpacity;
    ambientMultiplier = 0.0;
    emissiveMultiplier = 0.0;

#elif defined(LIGHTING_MODEL_UNLIT)
    finalColor = so.baseColor;
#else
    finalColor = so.baseColor;
#endif

    // Shadow debug visualization: override finalColor if a debug mode is active.
    // ge_lastCascadeIndex and ge_lastShadowResult are set by GE_SampleShadow
    // inside the lighting paths above.
#ifdef HAS_SHADOWS
    {
        vec3 debugColor;
        if (GE_GetShadowDebugColor(ge_lastShadowResult, debugColor))
        {
            oColor = vec4(debugColor, 1.0);
            return;
        }
    }
#endif

    // Screen-space GTAO: min the visibility into so.ao and fold the bent normal into
    // so.bentNormalWS, so the whole ambient term (diffuse, sheen, specular AO) sees it.
    // Only the GTAO variant (AO active for the view) compiles this; otherwise so.ao
    // stays the material's own occlusion map — which the ambient term applies either
    // way — and so.bentNormalWS stays the shading normal.
#if defined(LIGHTING_MODEL_STANDARD_PBR) && defined(GE_GTAO_ENABLED)
    GE_ApplyGTAO(so, si.screenUV);
#endif

    // Ambient / environment. When IBL is enabled, the split-sum environment term
    // replaces the flat analytic ambient; the IBLSet's fallback cube is ambient-
    // seeded so an unbaked scene still matches the old look (never black).
    // ambientMultiplier (0 for SHADOW_ONLY) still suppresses both paths.
#if defined(LIGHTING_MODEL_STANDARD_PBR)
  #if defined(GE_USER_PARTICLE_LIT)
    vec3 responseAmbient = vec3(0.0);
    for (int axis = 0; axis < 3; ++axis)
    {
      #ifdef GE_IBL_ENABLED
        responseAmbient += GE_SampleEnvironmentIrradiance(so.particleBasis[axis], 0.0) * so.particlePositive[axis];
        responseAmbient += GE_SampleEnvironmentIrradiance(-so.particleBasis[axis], 0.0) * so.particleNegative[axis];
      #else
        responseAmbient += Light.uAmbient.xyz * Light.uAmbient.w * (so.particlePositive[axis] + so.particleNegative[axis]);
      #endif
    }
    finalColor += so.baseColor * responseAmbient * (1.0 / 6.0) * ambientMultiplier;
  #elif defined(GE_IBL_ENABLED)
    finalColor += GE_EvaluateIBL(so, V, si.positionWS) * ambientMultiplier;
    #ifdef GE_SSSR_NORMAL_ROUGHNESS
    // RGB is the exact replaceable lobe: radiance * weight. The two alpha
    // channels carry its oct-encoded VIEW-space reflection direction, including
    // the anisotropy bend used by IBL, without adding a direction attachment.
    vec2 reflectionOct = GE_OctEncode(normalize(mat3(ge_view) * ge_sssrReflectionDirection));
    oSpecularWeight = vec4(ge_sssrSpecularWeight * ambientMultiplier, reflectionOct.x);
    oSpecularRadiance = vec4(ge_sssrIncidentRadiance, reflectionOct.y);
    #endif
  #else
    finalColor += so.baseColor * so.ao * Light.uAmbient.xyz * Light.uAmbient.w * ambientMultiplier;
  #endif
#endif

    // Emissive contribution. Under an exposure weight below 1 the emission is divided by part of the
    // view's exposure; the branch is uniform per material and costs nothing at the physical default.
    vec3 emission = so.emissive;
#ifdef GE_EMISSION_FOLLOWS_EXPOSURE_WEIGHT
    float emissiveExposureWeight = GE_SURFACE_EMISSIVE_EXPOSURE_WEIGHT;
    if (emissiveExposureWeight < 1.0)
        emission *= GE_EmissiveExposureScale(emissiveExposureWeight, GE_ViewExposureScale());
#endif
    finalColor += emission * emissiveMultiplier;

    // Alpha handling.
    float alpha = outputAlpha;

#ifdef ALPHA_TEST
    if (alpha < clamp(Props.alphaCutoff, 0.0, 1.0))
        discard;
    alpha = 1.0;
#elif !defined(GE_ALPHA_BLEND) && !defined(GE_SURFACE_ALPHA_TO_COVERAGE)
    // An opaque surface without an explicit coverage contract covers its pixel: texture or vertex
    // alpha has nothing to blend with. Consumers that keep this target's alpha
    // (transparent-background previews, the planar reflection weight) read it
    // as coverage.
    alpha = 1.0;
#endif

#ifdef GE_LOD_CROSSFADE
    // Independent of the alpha test above — a plain conjunction of coverage, so
    // the order does not matter. The depth-only form of this shader runs the
    // identical test, so this discard never removes a fragment whose depth the
    // prepass wrote.
    if (!GE_LodCrossfadeKeep(vLodFadeCode, gl_FragCoord.xy))
        discard;
#endif

#ifdef GE_PARALLAX_STEPS_VIEW
    // The Parallax steps view: the height samples the view march and the self-shadow took, as a
    // palette colour at the lit colour's luminance. The reflection G-buffer's weight is zeroed so
    // screen-space reflections leave the palette colour as written.
    finalColor = GE_ParallaxStepsViewShade(finalColor, so.parallaxHeightSamples);
  #ifdef GE_SSSR_NORMAL_ROUGHNESS
    oSpecularWeight.rgb = vec3(0.0);
  #endif
#endif

    // Guard matches the ibl.glsl include condition (line ~126) so ge_sceneColor/ge_sceneDepth and
    // the eye-depth helper are always declared when this block compiles.
#if defined(GE_SCENECOLOR_GRAB) && defined(GE_IBL_ENABLED) && defined(LIGHTING_MODEL_STANDARD_PBR)
    // Glass-behind-opaque occlusion — the MSAA fallback for the hardware depth test. The dedicated
    // transmissive pass attaches the opaque depth read-only and hardware-tests against it whenever
    // that depth matches the resolved colour's sample count. Under MSAA it attaches none (the
    // multisampled depth cannot share a pass with the resolved colour, and View.DepthResolved is
    // R32F, not an attachable depth format), and only then does this block do the occluding,
    // against View.DepthResolved, which is resolved after the depth prepass. It stays compiled
    // in both cases: with the attachment bound, hardware rejects these fragments before this stage
    // runs, so the test never fires.
    // Glass is peeled from the depth prepass, so ge_sceneDepth at this
    // pixel is the OPAQUE surface; where that opaque is NEARER (reverse-Z: larger raw depth) than the
    // glass fragment, emit the opaque colour — a no-op over the loaded framebuffer (the grab IS that
    // framebuffer), so the glass vanishes behind the occluder. Only the compare SIGN matters, so we
    // test raw reverse-Z directly and skip two eye-depth un-projections; the slack absorbs silhouette
    // reconstruction noise (glass is absent from the depth, so no self-occlusion).
    // NOTE: this no-op is exact for a SINGLE glass layer (dst == grab). Overlapping transmissive is
    // order-dependent — the pass sorts nothing and writes no depth — so a partially-occluded
    // stack reads approximately; accepted until stacked glass is a target.
    {
        const float kRawDepthSlack = 1e-4; // reverse-Z raw-depth silhouette guard
        float sceneRaw = texelFetch(ge_sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
        if (sceneRaw > gl_FragCoord.z + kRawDepthSlack) // opaque is in front of the glass
            finalColor = texelFetch(ge_sceneColor, ivec2(gl_FragCoord.xy), 0).rgb;
    }
#endif

    oColor = vec4(finalColor, alpha);
#endif // GE_DEPTH_ONLY_FRAGMENT || GE_MOTION_VECTORS
}
