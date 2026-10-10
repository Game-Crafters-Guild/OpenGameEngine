// Shared body of the TAA movers motion-vector vertex stage. Included by
// taa_motion_vectors.vert (rigid) and taa_motion_vectors_skinned.vert
// (dual-skinned); both call ge_TaaMVEmit with the two LOCAL-space endpoints
// of the vertex and everything downstream — the raster position chain and the
// unjittered motion payload — happens here exactly once.
//
// One body on purpose: the raster position must stay bit-exact with
// adapter_vertex.glsl (read-only GreaterOrEqual against prepass depth), so the
// two variants must not be able to drift in that expression chain.
//
// The caller declares `invariant gl_Position;` and the position input before
// including this file.

layout(set = 0, binding = 5) uniform CameraUBO
{
#include "camera_ubo_fields.glsl"
} Cam;

#include "camera_relative.glsl"

struct GPUInstance
{
#include "gpu_instance_fields.glsl"
};

layout(std430, set = 0, binding = 0) readonly buffer GPUInstances
{
    GPUInstance ge_instances[];
};

layout(std140, set = 0, binding = 1) uniform TaaMVParams
{
    mat4 uCurrViewProj; // UNJITTERED current view-proj (full world)
    mat4 uPrevViewProj; // UNJITTERED previous view-proj (full world)
} MV;

// uPrevPaletteOffset value meaning "this instance had no pose last frame".
// Steady-state atlas offsets are frame-STABLE, so "previous == current" is the
// NORMAL case and cannot double as the no-history encoding. Must match
// RenderServices::kNoPreviousSkinPalette.
const uint GE_NO_PREV_SKIN_PALETTE = 0xFFFFFFFFu;

layout(push_constant) uniform TaaMVPush
{
    uint uInstanceSlot;
    // Atlas bone-slot offset of this instance's palette in the PREVIOUS
    // frame's atlas, or GE_NO_PREV_SKIN_PALETTE when there is no previous pose
    // (spawn frame, animation just started, no previous ring). Unused by the
    // rigid variant.
    uint uPrevPaletteOffset;
} pc;

layout(location = 0) out vec4 vCurrClip;
layout(location = 1) out vec4 vPrevClip;

// Mirror of instance_io.glsl::ge_UnpackInstanceSector (this pass binds the
// instance buffer directly instead of pulling in the adapter's BDA machinery).
ivec3 TaaUnpackInstanceSector(uint p0, uint p1)
{
    uint ux = p0 & 0x1FFFFFu;
    uint uy = ((p0 >> 21) & 0x7FFu) | ((p1 & 0x3FFu) << 11);
    uint uz = (p1 >> 10) & 0x1FFFFFu;
    return ivec3(bitfieldExtract(int(ux), 0, 21),
                 bitfieldExtract(int(uy), 0, 21),
                 bitfieldExtract(int(uz), 0, 21));
}

// currLocal / prevLocal are OBJECT-space positions for this frame's and last
// frame's pose. The rigid variant passes aPosition for both; the skinned
// variant passes the two skinned results. Raster uses the current pose only —
// the depth test must match what the world pass actually rasterized.
void ge_TaaMVEmit(vec3 currLocal, vec3 prevLocal)
{
    GPUInstance inst = ge_instances[pc.uInstanceSlot];
    ivec3 sector = TaaUnpackInstanceSector(inst.sectorPacked0, inst.sectorPacked1);

    // Raster position: identical expression chain to adapter_vertex.glsl's
    // instanced path (jittered Cam).
    vec4 sectorLocalWorldPos = inst.transform * vec4(currLocal, 1.0);
    vec3 fullWorld;
    vec3 relWorld;
    gl_Position = GE_ClipFromSectorLocal(sectorLocalWorldPos.xyz, sector, fullWorld, relWorld);

    // Motion payload: unjittered current/previous clip positions. Each
    // endpoint pairs its own pose with its own transform, so object motion and
    // pose deformation compose in one vector.
    vec3 sectorOffset = vec3(sector) * GE_SECTOR_SIZE;
    vCurrClip = MV.uCurrViewProj * vec4(sectorLocalWorldPos.xyz + sectorOffset, 1.0);
    vec4 prevSectorLocal = inst.prevTransform * vec4(prevLocal, 1.0);
    vPrevClip = MV.uPrevViewProj * vec4(prevSectorLocal.xyz + sectorOffset, 1.0);
}
