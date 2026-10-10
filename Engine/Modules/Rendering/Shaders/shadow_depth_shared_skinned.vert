// Shared skinned depth-only vertex shader. Companion to shadow_depth_shared.vert
// for meshes with GPU skinning.

#version 450

#define GE_INSTANCED
#define SKINNED
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// Bit-exact clip-position contract with adapter_vertex.glsl — see
// shadow_depth_shared.vert. NOTE: 4-influence blend only; SKINNED_8 meshes
// must keep their per-material depth variant (DepthDrawRecorder gates them).
invariant gl_Position;

layout(location = 0) in vec3 aPosition;
layout(location = 6) in uvec4 aJoints;
layout(location = 7) in vec4 aWeights;

layout(set = 0, binding = 5) uniform CameraUBO
{
#include "Includes/camera_ubo_fields.glsl"
} Cam;

#include "Includes/instance_io.glsl"

layout(set = 0, binding = 12) readonly buffer BonePaletteAtlasSSBO
{
    vec4 rows[];
} BonePaletteAtlas;

#define GE_BONE_PALETTE_READABLE
#include "Includes/bone_palette.glsl"
#include "Includes/camera_relative.glsl"

void main()
{
    InstanceData inst = ge_FetchInstanceData();
    vec3 localPos = ge_ApplyBonePalettePosition(
        aPosition, aJoints, aWeights, inst.skinPaletteOffset);
    vec4 sectorLocalWorldPos = inst.modelMatrix * vec4(localPos, 1.0);
    vec3 fullWorldPos;
    vec3 relWorldPos; // unused: depth-only pass writes gl_Position only
    gl_Position = GE_ClipFromSectorLocal(sectorLocalWorldPos.xyz, inst.sector, fullWorldPos, relWorldPos);
}
