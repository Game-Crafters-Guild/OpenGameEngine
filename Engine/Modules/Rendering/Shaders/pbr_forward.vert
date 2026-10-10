#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec3 vN;
layout(location = 2) out vec3 vPosView;
layout(location = 3) out vec3 vPosWorld;
layout(location = 4) out vec4 vPosLightClip;

// Camera UBO (set0,binding5) carries V/P/VP
layout(set = 0, binding = 5) uniform CameraUBO {
#include "Includes/camera_ubo_fields.glsl"
} cam;

// Light UBO for projecting to light clip (match FS binding)
layout(set = 0, binding = 6) uniform LightUBO {
    mat4 uLightVP;
    vec4 uLightDirWorld;
} lightUBO;

// Push constants: per-draw model and normal matrix + light in view space (total 128 bytes)
layout(push_constant) uniform PC
{
    mat4 uM;       // model matrix (replaces previous MVP)
    vec4 uN0;      // normal matrix column 0 (xyz), padded
    vec4 uN1;      // normal matrix column 1 (xyz), padded
    vec4 uN2;      // normal matrix column 2 (xyz), padded
    vec4 lightDir; // xyz = light dir in view space, w padding
} pc;

void main()
{
    vec4 posWorld = pc.uM * vec4(aPos, 1.0);
    vPosWorld = posWorld.xyz;
    vec4 posView  = cam.uV * posWorld;
    vPosView = posView.xyz;
    gl_Position = cam.uP * posView;
    vUV = aUV;
    mat3 Nmat = mat3(pc.uN0.xyz, pc.uN1.xyz, pc.uN2.xyz);
    // Transform normal to view space so lighting in FS (which uses view-space V and L) is consistent
    vN = normalize(mat3(cam.uV) * (Nmat * aNormal));
    // Also compute light clip-space position for shadow UV; keeps math consistent with depth pass
    vPosLightClip = lightUBO.uLightVP * posWorld;
}
