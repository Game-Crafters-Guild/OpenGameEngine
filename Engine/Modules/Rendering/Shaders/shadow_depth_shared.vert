// Shared depth-only vertex shader (Shape B). Outputs only gl_Position — no
// varyings, no fragment shader. Used by depth-class-eligible materials
// (opaque, no vertex modifier) for the camera DepthPrepass and the shadow
// depth families, replacing the per-material depth pipeline that would
// otherwise compile one variant per surface (prepass-collapse-2026-07-24).

#version 450

#define GE_INSTANCED
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// The color pass re-shades through a GreaterOrEqual test (re-writing equal
// depth values) against
// prepass depth written by this shader — the clip position must be bit-exact
// with adapter_vertex.glsl. Same expression chain (camera_relative.glsl) +
// Invariant on both sides is that guarantee.
invariant gl_Position;

layout(location = 0) in vec3 aPosition;

layout(set = 0, binding = 5) uniform CameraUBO
{
#include "Includes/camera_ubo_fields.glsl"
} Cam;

#include "Includes/instance_io.glsl"
#include "Includes/camera_relative.glsl"

void main()
{
    InstanceData inst = ge_FetchInstanceData();
    // Depth prepass shares the main view's CameraUBO, so the rebased clip position
    // matches adapter_vertex.glsl bit-for-bit. Shadow cascades bind their own
    // CameraUBO carrying the SAME render origin (viewProjRel = rebased light VP,
    // renderOriginSector = camera sector) so casters rasterize at fp32-of-small-
    // magnitude at planetary distance. Origin inactive (sector 0) => full-world
    // path, byte-identical to the pre-feature build.
    vec4 sectorLocalWorldPos = inst.modelMatrix * vec4(aPosition, 1.0);
    vec3 fullWorldPos;
    vec3 relWorldPos; // unused: depth-only pass writes gl_Position only
    gl_Position = GE_ClipFromSectorLocal(sectorLocalWorldPos.xyz, inst.sector, fullWorldPos, relWorldPos);
}
