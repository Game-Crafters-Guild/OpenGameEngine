#version 450

#ifdef GE_INSTANCED
// Instanced path reads the instance-indirection buffer + GPUInstances via
// GL_EXT_buffer_reference with device addresses in push constants,
// leaving set 0 free of per-frame-rotating handles. See instance_io.glsl.
// int64 is needed for the uint64_t buffer-address scalars in the push-const
// block; newer glslang requires it explicitly even when buffer_reference is on.
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#endif

// Engine-provided vertex adapter for the flexible render pipeline.
//
// Supports:
//   - Optional vertex attributes via #ifdef (HAS_NORMAL, HAS_UV0, HAS_TANGENT, etc.)
//   - Instanced rendering via GE_INSTANCED (SSBO fetch) or push constants (legacy)
//   - Skinned meshes via SKINNED (bone transform in vertex shader)
//   - User vertex modifier hook via HAS_VERTEX_MODIFIER
//
// Binding conventions match VertexLayoutBuilder.h / VertexLocation / VertexBinding.
// Varyings are emitted for all attributes; the fragment adapter reads what it needs.

#ifdef GE_MOTION_VECTORS
#ifndef GE_VERTEX_DEFORMATION
#error "GE_MOTION_VECTORS needs a vertex modifier. A rigid surface's motion is an instance transform delta and the per-instance mover pass already writes it exactly; the composed variant exists for the deformation the mover pass cannot reproduce."
#endif
#ifdef HAS_VERTEX_OUTPUT_MODIFIER
#error "GE_MOTION_VECTORS does not support the extended vertex-output modifier form. That form produces world positions and never applies the instance transform, so its previous endpoint has no prevTransform to pair with. Compose the motion variant for the simple vec3 ModifyVertex(vec3, InstanceData) form."
#endif
#endif

// Cross-module raster-position contract: the depth prepass rasterizes eligible
// draws with shadow_depth_shared{,_skinned}.vert and the color pass re-shades
// through a read-only GreaterOrEqual test against that depth, so gl_Position
// must be bit-exact across modules for the same expression chain
// (camera_relative.glsl). Invariant forbids the driver from contracting or
// reassociating the position math differently per module. Declared in every
// shader that reproduces the chain: this adapter, shadow_depth_shared*.vert,
// taa_motion_vectors.vert.
invariant gl_Position;

// --- Vertex inputs (conditional on mesh attributes) ---
// When HAS_VERTEX_OUTPUT_MODIFIER is defined, the modifier declares its own
// vertex inputs (e.g. vec2 aGridPos for terrain). Standard inputs are skipped.

#ifndef HAS_VERTEX_OUTPUT_MODIFIER
layout(location = 0) in vec3 aPosition; // always present (standard path)

#ifdef HAS_NORMAL
layout(location = 1) in vec3 aNormal;
#endif

#ifdef HAS_UV0
layout(location = 2) in vec2 aUV0;
#endif

#ifdef HAS_TANGENT
layout(location = 3) in vec4 aTangent; // xyz = tangent, w = handedness
#endif

#ifdef HAS_COLOR
layout(location = 4) in vec4 aColor;
#endif

#ifdef HAS_UV1
layout(location = 5) in vec2 aUV1;
#endif

#if defined(HAS_UV2) && !defined(GE_USER_PARTICLE_BUFFER)
layout(location = 10) in vec2 aUV2;
#endif

#if defined(HAS_UV3) && !defined(GE_USER_PARTICLE_BUFFER)
layout(location = 11) in vec2 aUV3;
#endif

#if defined(HAS_UV4) && !defined(GE_USER_PARTICLE_BUFFER)
layout(location = 12) in vec2 aUV4;
#endif

#if defined(HAS_UV5) && !defined(GE_USER_PARTICLE_BUFFER)
layout(location = 13) in vec2 aUV5;
#endif

#if defined(HAS_UV6) && !defined(GE_USER_PARTICLE_BUFFER)
layout(location = 14) in vec2 aUV6;
#endif

#if defined(HAS_UV7) && !defined(GE_USER_PARTICLE_BUFFER)
layout(location = 15) in vec2 aUV7;
#endif

#ifdef SKINNED
layout(location = 6) in uvec4 aJoints;
layout(location = 7) in vec4 aWeights;
#ifdef SKINNED_8
layout(location = 8) in uvec4 aJoints1;
layout(location = 9) in vec4 aWeights1;
#endif
#endif
#endif // !HAS_VERTEX_OUTPUT_MODIFIER

// --- Varyings to fragment shader ---
// Locations must match adapter_forward.glsl inputs.

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
#endif

layout(location = 0) GE_SURFACE_CENTROID out vec2 vUV0;

#if defined(HAS_NORMAL) || defined(HAS_VERTEX_OUTPUT_MODIFIER)
layout(location = 1) GE_SURFACE_CENTROID out vec3 vNormalWS;
#endif

layout(location = 2) GE_SURFACE_CENTROID out vec3 vPosWS;

#ifdef HAS_TANGENT
layout(location = 3) GE_SURFACE_CENTROID out vec4 vTangentWS;
#endif

#ifdef HAS_COLOR
layout(location = 4) GE_SURFACE_CENTROID out vec4 vColor;
#endif

#ifdef HAS_UV1
layout(location = 5) GE_SURFACE_CENTROID out vec2 vUV1;
#endif

flat layout(location = 6) out uint vMaterialIndex;

#ifdef GE_USER_PARTICLE_BUFFER
layout(location = 7) out vec4 vCustom0; // ribbon endpoint colors interpolate
#else
flat layout(location = 7) out vec4 vCustom0;
#endif

// Per-instance flags (bit0=castShadows, bit1=receiveShadows). location 8..13 are
// the conditional vUV2..vUV7, so this sits above them at 14.
#ifdef GE_LOD_CROSSFADE
flat layout(location = 14, component = 0) out uint vInstanceFlags;
// Packed LOD-crossfade code (lod_crossfade.glsl). It shares location 14 rather
// than taking a slot of its own: 0..15 are all occupied and 15 is the last
// guaranteed varying slot, so a 16th would raise the hardware floor for a
// prototype feature. Same `flat uint` interpolation as the flags it sits
// beside, which is what makes two variables on one location legal.
flat layout(location = 14, component = 1) out uint vLodFadeCode;
#else
flat layout(location = 14) out uint vInstanceFlags;
#endif

// Render-origin-relative world position (Earth-scale precision): PRECISE small-
// magnitude position the fragment shadow receiver projects through the rebased
// ge_shadowVP. Equals vPosWS when the render origin is inactive (near-origin
// scenes), so the shadow path is byte-identical there. location 15 is the last
// guaranteed varying slot (16 total); above the conditional vUV2..vUV7 at 8..13.
layout(location = 15) GE_SURFACE_CENTROID out vec3 vPosRel;

#ifdef GE_MOTION_VECTORS
// The PREVIOUS deformation endpoint's UNJITTERED clip position. The perspective
// divide is per-fragment, so the clip position interpolates and the viewport-UV
// delta is formed in the fragment stage (motion_vector_payload.glsl). The
// current endpoint is not a varying at all: a clip position interpolated to a
// fragment is that fragment's own position, which gl_FragCoord already carries.
//
// The four components below are every smooth component the 0..15 block leaves
// free in EVERY keyword combination — vUV0 (vec2 at 0), vPosWS (vec3 at 2) and
// vPosRel (vec3 at 15) are the three unconditional surface varyings, and a vec4
// is exactly what they leave over. Taking them instead of locations 16 and 17
// keeps the motion variant inside the 16 locations every other variant stays
// inside, so it needs no more varying budget than the depth variant of the same
// material. Each carries its host's GE_SURFACE_CENTROID: components sharing a
// location must agree on interpolation qualification, which glslang enforces.
layout(location = 0, component = 2) GE_SURFACE_CENTROID out vec2 vMotionPrevClipXY;
layout(location = 2, component = 3) GE_SURFACE_CENTROID out float vMotionPrevClipZ;
layout(location = 15, component = 3) GE_SURFACE_CENTROID out float vMotionPrevClipW;
#endif

#ifdef GE_USER_PARTICLE_BUFFER
flat layout(location = 8) out vec4 vParticleAnimation;
#if defined(GE_COMPAT_PROFILE)
// WGSL stage interfaces carry only scalars and vectors: the basis travels as its
// three columns, at the three locations the mat3 occupies on the desktop.
flat layout(location = 9) out vec3 vParticleBasis0;
flat layout(location = 10) out vec3 vParticleBasis1;
flat layout(location = 11) out vec3 vParticleBasis2;
#else
flat layout(location = 9) out mat3 vParticleBasis;
#endif
// Location 12 is free under the particle buffer: particles carry no UV6 (see below).
flat layout(location = 12) out float vParticleColorScale;
// Location 13 is free under the particle buffer as well: particles carry no UV7.
flat layout(location = 13) out float vParticleNearFade;
#endif

#if defined(HAS_UV2) && !defined(GE_USER_PARTICLE_BUFFER)
layout(location = 8) GE_SURFACE_CENTROID out vec2 vUV2;
#endif

#if defined(HAS_UV3) && !defined(GE_USER_PARTICLE_BUFFER)
layout(location = 9) GE_SURFACE_CENTROID out vec2 vUV3;
#endif

#if defined(HAS_UV4) && !defined(GE_USER_PARTICLE_BUFFER)
layout(location = 10) GE_SURFACE_CENTROID out vec2 vUV4;
#endif

#if defined(HAS_UV5) && !defined(GE_USER_PARTICLE_BUFFER)
layout(location = 11) GE_SURFACE_CENTROID out vec2 vUV5;
#endif

#if defined(HAS_UV6) && !defined(GE_USER_PARTICLE_BUFFER)
layout(location = 12) GE_SURFACE_CENTROID out vec2 vUV6;
#endif

#if defined(HAS_UV7) && !defined(GE_USER_PARTICLE_BUFFER)
layout(location = 13) GE_SURFACE_CENTROID out vec2 vUV7;
#endif

// --- Engine-provided resources ---

// Vertex modifiers of EITHER form deform against the per-view time block, which
// instance_io.glsl stamps into the InstanceData endpoint (GE_VERTEX_DEFORMATION) —
// so the block is declared before that include. Binding/layout matches
// adapter_forward.glsl, which declares this block unconditionally, so neither
// form adds a binding the descriptor set was not already carrying.
#if defined(HAS_VERTEX_MODIFIER) || defined(HAS_VERTEX_OUTPUT_MODIFIER)
layout(set = 0, binding = 6) uniform LightUBO {
    mat4 uLightVP;
    vec4 uLightDirWorld;
    vec4 uLightColorWorld;
    vec4 uAmbient;
    // xy = the process-wide animation and bounded scroll clocks; zw = this
    // view's endpoint lanes, which ge_StampDeformationClock reads.
    vec4 uTimeParams;
    vec4 uSecondaryCount;
    vec4 uSecondaryDirs[3];
    vec4 uSecondaryColors[3];
} Light;
#endif

#include "../Includes/instance_io.glsl"

// Camera UBO (consistent with existing composed shaders).
layout(set = 0, binding = 5) uniform CameraUBO {
#include "../Includes/camera_ubo_fields.glsl"
} Cam;

// CUSTOM_VERTEX_SHADER supplies its own identity ge_FetchInstanceData (see
// instance_io.glsl) and declares no push-constant block — fully-procedural
// geometry has no per-draw mesh data, and GLSL allows only one PC block per
// stage (which the CBT modifier owns on the graphics side).
#if !defined(GE_INSTANCED) && !defined(CUSTOM_VERTEX_SHADER)
// Push constants (non-instanced path).
layout(push_constant) uniform PC
{
    mat4 uM;  // model matrix
    vec4 uN0; // normal matrix col0 (xyz)
    vec4 uN1; // normal matrix col1 (xyz)
    vec4 uN2; // normal matrix col2 (xyz)
    vec4 uExtra; // x = materialIndex (as uint bits), y = skinPaletteOffset (as uint bits)
} pc;

InstanceData ge_FetchInstanceData()
{
    InstanceData inst;
    inst.modelMatrix = pc.uM;
    inst.normalMatrix = mat3(pc.uN0.xyz, pc.uN1.xyz, pc.uN2.xyz);
    inst.instanceIndex = 0u;
    inst.materialIndex = floatBitsToUint(pc.uExtra.x);
    inst.skinPaletteOffset = floatBitsToUint(pc.uExtra.y);
    inst.flags = 2u; // non-instanced path: default receiveShadows on (bit 1)
    inst.custom0 = vec4(1.0);
#ifdef GE_USER_PARTICLE_BUFFER
    inst.particleVelocityAngle = vec4(0.0);
    inst.particleAnimation = vec4(0.0);
    inst.particleAlignment = 1u;
    inst.particleGeometry = 0u;
#endif
    inst.sector = ivec3(0); // push-constant path is editor/near-origin: no sector
#ifdef GE_LOD_CROSSFADE
    inst.lodFadeCode = 0u; // non-instanced draws carry no per-record fade channel
#endif
#ifdef GE_VERTEX_DEFORMATION
    ge_StampDeformationClock(inst);
#endif
    return inst;
}

#ifdef GE_MOTION_VECTORS
// The non-instanced path is a single per-draw push-constant block with no
// history in it, so the previous endpoint keeps this endpoint's transform: its
// motion is the deformation alone, and the scene index is not consulted. Object
// motion on this path belongs to the per-instance mover lane, which carries its
// own previous transform.
mat4 ge_FetchPreviousModelMatrix(uint sceneIdx)
{
    return pc.uM;
}
#endif
#endif

#ifdef GE_MOTION_VECTORS
// The previous endpoint is this frame's InstanceData with exactly two fields
// moved back in time: the transform the instance carried when this view last
// rendered, and the clock that frame deformed at. Everything else — material
// row, instance index, custom payload, sector, fade code — holds the same value
// at both endpoints, which is what stops a per-instance wind phase seeded from
// inst.instanceIndex from sliding between them. It takes the current endpoint
// rather than fetching its own so the instance row and its indirection word are
// read ONCE per vertex: two fields move, and the fetch that produced the other
// twenty already happened.
//
// Defined here rather than in instance_io.glsl because this is the first point
// at which every fetch form's ge_FetchPreviousModelMatrix is in scope: the
// non-instanced form is declared above, not in that file.
InstanceData ge_PreviousEndpoint(InstanceData inst)
{
    inst.modelMatrix = ge_FetchPreviousModelMatrix(inst.instanceIndex);
    ge_StampPreviousDeformationClock(inst);
    return inst;
}
#endif

// Material data is shared by simple modifiers and compact particle modifiers.
// Particle meshes use the same authored property table for deformation and shading.
#if defined(HAS_VERTEX_MODIFIER) || defined(GE_USER_PARTICLE_BUFFER)
struct MaterialData
{
    // Keep the vertex-stage row byte-identical to adapter_forward.glsl and
    // MaterialGpuParams. A shortened param block changes the std430 array
    // stride, so material indices greater than zero read another row's bytes
    // for vertex-modifier parameters such as EZ Tree wind.
#define GE_FIELD(type, name) type name;
#include "../Includes/material_params.glsl"
#undef GE_FIELD
    uint TextureIndices[8];
    vec4 TextureST[8];
    vec4 TextureST2[8];
    uint SamplerIndices;
};

layout(std430, set = 0, binding = 13) readonly buffer MaterialParamsSSBO
{
    MaterialData ge_Materials[];
} MaterialParams;
#include "../Includes/material_row_index.glsl"

MaterialData ge_MatData;
#define Mat ge_MatData
#include "../Includes/material_param_lanes.glsl"
// The composer replaces the marker line with the same GE_Props block the
// fragment adapter gets, so a vertex modifier reads Props.<name> too.
// GE_DECLARED_PROPERTIES
#endif // HAS_VERTEX_MODIFIER

// --- Skinning (optional) ---
//
// Uses a shared bone palette atlas SSBO. Each entity's palette starts at
// inst.skinPaletteOffset (in bone-slot units; one slot = 3 vec4 rows in
// the mat3x4 storage layout, see Includes/bone_palette.glsl). Joint
// indices are local to the entity's palette.

#ifdef SKINNED
layout(set = 0, binding = 12) readonly buffer BonePaletteAtlasSSBO
{
    vec4 rows[];
} BonePaletteAtlas;

#define GE_BONE_PALETTE_READABLE
#include "../Includes/bone_palette.glsl"

void ge_ApplySkinning4(inout vec3 position, inout vec3 normal,
                       uvec4 joints, vec4 weights, uint paletteOffset)
{
    ge_ApplyBonePalette(position, normal, joints, weights, paletteOffset);
}

#ifdef SKINNED_8
void ge_ApplySkinning8(inout vec3 position, inout vec3 normal,
                       uvec4 joints0, vec4 weights0,
                       uvec4 joints1, vec4 weights1,
                       uint paletteOffset)
{
    vec4 r0, r1, r2;
    ge_BlendBonePaletteRows(paletteOffset, joints0, weights0, r0, r1, r2);

    vec4 r0b, r1b, r2b;
    ge_BlendBonePaletteRows(paletteOffset, joints1, weights1, r0b, r1b, r2b);
    r0 += r0b;
    r1 += r1b;
    r2 += r2b;

    vec4 ph = vec4(position, 1.0);
    position = vec3(dot(r0, ph), dot(r1, ph), dot(r2, ph));
    normal = vec3(dot(r0.xyz, normal), dot(r1.xyz, normal), dot(r2.xyz, normal));
}
#endif
#endif

// --- User vertex modifier (optional) ---
//
// Two forms are supported:
//
// 1. Simple (HAS_VERTEX_MODIFIER): position-only modification.
//      vec3 ModifyVertex(vec3 position, InstanceData inst);
//
// 2. Extended (HAS_VERTEX_OUTPUT_MODIFIER): full vertex output control.
//      void ModifyVertex(inout VertexOutput v, InstanceData inst);
//    The modifier declares its own vertex inputs and fills position, normal, UV.
//    Used by terrain, water, and other procedural geometry.

#include "../Includes/vertex_output.glsl"

#ifdef HAS_VERTEX_OUTPUT_MODIFIER
#include GE_VERTEX_MODIFIER_PATH
#elif defined(HAS_VERTEX_MODIFIER)
#include GE_VERTEX_MODIFIER_PATH
#endif

// Camera-relative reconstruction + depth projection helpers (shared with the
// depth/shadow vertex path). Requires the `Cam` CameraUBO block above.
#include "../Includes/camera_relative.glsl"

#ifdef GE_MOTION_VECTORS
// Emit the motion payload's previous endpoint. The argument is a position with
// the previous endpoint's own instance transform already applied, in the
// instance's own sector; the sector is a per-instance constant that does not
// move between endpoints, so one offset recomposes it to the full world the
// unjittered view-projection is built in. Mirrors the per-instance mover pass's
// emit (taa_motion_vectors_common.glsl), which recomposes the same way.
//
// The raster position is NOT set here: gl_Position stays the jittered,
// camera-relative chain every other pass rasterizes, so the motion draw covers
// exactly the fragments the colour and depth passes covered — and that chain is
// also what carries the CURRENT endpoint to the fragment, through gl_FragCoord.
void ge_EmitPreviousDeformationEndpoint(vec3 prevSectorLocalWorldPos, ivec3 sector,
                                       uint instanceIndex)
{
    // Per-instance validity. An instance whose rendered history broke this
    // frame has no previous surface this endpoint could describe, and the
    // payload's own "no usable previous" encoding is a non-positive w — so
    // every fragment of it exports the sentinel and its pixels reproject
    // analytically. A zero delta would be the other answer, and it is the
    // wrong one: it claims the surface did not move.
    if (!ge_DeformationHistoryContinuous(instanceIndex))
    {
        vMotionPrevClipXY = vec2(0.0);
        vMotionPrevClipZ = 0.0;
        vMotionPrevClipW = 0.0;
        return;
    }
    vec3 sectorOffset = vec3(sector) * GE_SECTOR_SIZE;
    vec4 prevClip =
        MotionParams.uMotionPrevViewProj * vec4(prevSectorLocalWorldPos + sectorOffset, 1.0);
    vMotionPrevClipXY = prevClip.xy;
    vMotionPrevClipZ = prevClip.z;
    vMotionPrevClipW = prevClip.w;
}
#endif

// --- Main ---

void main()
{
#ifdef GE_PROCEDURAL_VERTEX_OUTPUT
    // Procedural output modifiers own their geometry and may use gl_InstanceIndex
    // for non-mesh data such as terrain patch indices.
    InstanceData inst;
    inst.modelMatrix = mat4(1.0);
    inst.normalMatrix = mat3(1.0);
    inst.instanceIndex = 0u;
    inst.materialIndex = 0u;
    inst.skinPaletteOffset = 0u;
    inst.flags = 2u; // procedural path: default receiveShadows on (bit 1)
    inst.custom0 = vec4(1.0);
#ifdef GE_USER_PARTICLE_BUFFER
    inst.particleVelocityAngle = vec4(0.0);
    inst.particleAnimation = vec4(0.0);
    inst.particleAlignment = 1u;
    inst.particleGeometry = 0u;
#endif
    inst.sector = ivec3(0); // procedural geometry rebases in its own domain (CBT)
#ifdef GE_VERTEX_DEFORMATION
    ge_StampDeformationClock(inst);
#endif
#else
    InstanceData inst = ge_FetchInstanceData();
#endif

#if defined(HAS_VERTEX_MODIFIER) || defined(GE_USER_PARTICLE_BUFFER)
    ge_MatData = MaterialParams.ge_Materials[GE_MATERIAL_ROW_INDEX(
        MaterialParams.ge_Materials, inst.materialIndex)];
    GE_LoadDeclaredProperties();
#endif

    // Clip position, set per path below: camera-relative reconstruction for the
    // standard mesh path; full-world for extended vertex-output modifiers, which
    // own their world positions and rebase in their own domain (e.g. CBT terrain).
    vec4 geClipPos;

#ifdef HAS_VERTEX_OUTPUT_MODIFIER
    // Extended modifier: modifier owns all vertex inputs and fills output.
    // Vertex output modifiers produce world-space positions directly, so
    // the instance model matrix is NOT applied (it would require valid
    // per-draw instance SSBO entries which procedural geometry doesn't have).
    VertexOutput vo = DefaultVertexOutput();
    ModifyVertex(vo, inst);

    if (vo.useSector)
    {
        // Earth-scale (sector, local) vertex (CBT deep decode, terrain S2a): the modifier
        // handed a SECTOR-LOCAL position + this vertex's integer world sector. Project it
        // through the mesh path's GE_ClipFromSectorLocal — the clip position is built from
        // the EXACT integer sector delta + a small local offset, so no world-magnitude fp32
        // rounding ever enters the reconstruction. vPosWS keeps the recomposed full world
        // for the world-space fragment inputs (lighting/fog), vPosRel the precise
        // render-origin-relative shadow receiver position.
        vec3 fullWorldPos;
        geClipPos = GE_ClipFromSectorLocal(vo.position, vo.sector, fullWorldPos, vPosRel);
        vPosWS = fullWorldPos;
    }
    else
    {
        vec4 worldPos = vec4(vo.position, 1.0);
        vPosWS = worldPos.xyz;
        // Vertex-output modifiers (terrain, water) own their world positions; they carry
        // no per-instance sector, so they subtract the render origin here and project the
        // small camera-relative position through the rebased view-proj (Earth-scale slice
        // 1b). vPosRel is the render-origin-relative receiver position the fragment shadow
        // path projects through the rebased ge_shadowVP. At origin (0,0,0) both reduce to
        // the full-world path — byte-identical to the pre-feature build (dark-ship).
        geClipPos = GE_ClipFromWorld(worldPos.xyz, vPosRel);
    }
    vNormalWS = normalize(vo.normal);
    vUV0 = vo.uv0;
    vCustom0 = vo.custom0;
#ifdef GE_USER_PARTICLE_BUFFER
    vParticleAnimation = vo.particleAnimation;
#if defined(GE_COMPAT_PROFILE)
    vParticleBasis0 = vo.particleBasis[0];
    vParticleBasis1 = vo.particleBasis[1];
    vParticleBasis2 = vo.particleBasis[2];
#else
    vParticleBasis = vo.particleBasis;
#endif
    vParticleColorScale = vo.particleColorScale;
    vParticleNearFade = vo.particleNearFade;
    // Extended modifiers skip the shared mesh-attribute output assignments.
    // Particle color is per instance, and its material uses atlas UV0 only.
#ifdef HAS_COLOR
    vColor = vec4(1.0);
#endif
#ifdef HAS_TANGENT
    vTangentWS = vec4(vo.particleBasis[0], 1.0);
#endif
#ifdef HAS_UV1
    vUV1 = vo.uv0;
#endif
#endif

#else
    // Standard path: read from vertex attributes.
    vec3 localPos = aPosition;

#ifdef HAS_NORMAL
    vec3 localNormal = aNormal;
#else
    vec3 localNormal = vec3(0.0, 1.0, 0.0);
#endif

    // Apply skinning in local space before world transform.
#ifdef SKINNED
#ifdef SKINNED_8
    ge_ApplySkinning8(localPos, localNormal, aJoints, aWeights, aJoints1, aWeights1, inst.skinPaletteOffset);
#else
    ge_ApplySkinning4(localPos, localNormal, aJoints, aWeights, inst.skinPaletteOffset);
#endif
#endif

    // Simple vertex modifier (operates in local space).
#ifdef HAS_VERTEX_MODIFIER
#ifdef GE_MOTION_VECTORS
    // The previous endpoint, evaluated before the current one overwrites the
    // shared pre-modifier vertex. Both endpoints deform the same skinned pose:
    // the composed lane carries no previous bone palette (the per-instance
    // mover lane holds that in a push constant), so a skinned surface's pose
    // delta is that lane's to describe and this variant's delta is the
    // modifier's displacement plus the instance transform delta.
    InstanceData gePrevInst = ge_PreviousEndpoint(inst);
    vec3 gePrevLocalPos = ModifyVertex(localPos, gePrevInst);
#endif
    localPos = ModifyVertex(localPos, inst);
#endif

    // Transform to sector-local world space, then reconstruct the camera-relative
    // clip position. vPosWS stays FULL world for the fragment stage (lighting /
    // shadows / fog are world-space this slice).
    vec4 sectorLocalWorldPos = inst.modelMatrix * vec4(localPos, 1.0);
#ifdef GE_MOTION_VECTORS
    ge_EmitPreviousDeformationEndpoint(
        (gePrevInst.modelMatrix * vec4(gePrevLocalPos, 1.0)).xyz, inst.sector,
        inst.instanceIndex);
#endif
    vec3 fullWorldPos;
    vec3 relWorldPos;
    geClipPos = GE_ClipFromSectorLocal(sectorLocalWorldPos.xyz, inst.sector, fullWorldPos, relWorldPos);
    vPosWS = fullWorldPos;
    vPosRel = relWorldPos;

#ifdef HAS_NORMAL
    vNormalWS = normalize(inst.normalMatrix * localNormal);
#endif

#ifdef HAS_UV0
    vUV0 = aUV0;
#else
    vUV0 = vec2(0.0);
#endif
    vCustom0 = inst.custom0;
#endif // HAS_VERTEX_OUTPUT_MODIFIER

    // Per-instance flags to the fragment stage (receiveShadows gate). Both paths.
    vInstanceFlags = inst.flags;
#ifdef GE_LOD_CROSSFADE
    vLodFadeCode = inst.lodFadeCode;
#endif

    // Shared outputs (both paths).
#ifndef HAS_VERTEX_OUTPUT_MODIFIER
#ifdef HAS_TANGENT
    vTangentWS = vec4(normalize(mat3(inst.modelMatrix) * aTangent.xyz), aTangent.w);
#endif

#ifdef HAS_COLOR
    vColor = aColor;
#endif

#ifdef HAS_UV1
    vUV1 = aUV1;
#endif

#if defined(HAS_UV2) && !defined(GE_USER_PARTICLE_BUFFER)
    vUV2 = aUV2;
#endif

#if defined(HAS_UV3) && !defined(GE_USER_PARTICLE_BUFFER)
    vUV3 = aUV3;
#endif

#if defined(HAS_UV4) && !defined(GE_USER_PARTICLE_BUFFER)
    vUV4 = aUV4;
#endif

#if defined(HAS_UV5) && !defined(GE_USER_PARTICLE_BUFFER)
    vUV5 = aUV5;
#endif

#if defined(HAS_UV6) && !defined(GE_USER_PARTICLE_BUFFER)
    vUV6 = aUV6;
#endif

#if defined(HAS_UV7) && !defined(GE_USER_PARTICLE_BUFFER)
    vUV7 = aUV7;
#endif
#endif // !HAS_VERTEX_OUTPUT_MODIFIER

    vMaterialIndex = inst.materialIndex;

    gl_Position = geClipPos;
}
