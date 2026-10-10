#version 450

// A region mark-up's display for the glow (MarkupRenderFeature's mesh path): its walls and lid
// from a vertex buffer of MarkupECS::MarkupRegionVertex, a world position and the rim coordinate
// (the distance to the top edge, or on the lid to the outline, over the rim's width). Shares
// markup_glow.frag, which takes the rim coordinate as a box takes its edge facing.

layout(set = 0, binding = 5) uniform CameraUBO {
#include "Includes/camera_ubo_fields.glsl"
} cam;

layout(push_constant) uniform PC {
    mat4 uModel; // the mesh's world placement (the identity: its positions are world)
    vec4 uColor; // rgb = the mark-up's color (linear), a = body alpha
    vec4 uRim;   // x = rim power, y = rim gain, z = rim budget, w = shape (2 mesh)
} pc;

layout(location = 0) in vec4 aPositionRim; // xyz world position, w rim coordinate

layout(location = 0) out vec3 vNormalWS;
layout(location = 1) out vec3 vPositionWS;
layout(location = 2) out vec2 vFacePoint;            // x = the rim coordinate
layout(location = 3) flat out vec2 vFaceHalfExtents; // unused by the mesh path

void main()
{
    vec4 world = pc.uModel * vec4(aPositionRim.xyz, 1.0);
    vPositionWS = world.xyz;
    vNormalWS = vec3(0.0, 1.0, 0.0);
    vFacePoint = vec2(aPositionRim.w, 0.0);
    vFaceHalfExtents = vec2(0.0);
    gl_Position = cam.uVP * world;
}
