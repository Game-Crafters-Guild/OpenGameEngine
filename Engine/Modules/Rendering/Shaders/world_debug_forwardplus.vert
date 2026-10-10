// World debug shader variant that also outputs world position for Forward+ lighting.
//
// Keep push constants identical to world_debug.vert so RenderServices doesn't need
// a different push constant layout.
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

layout(location = 0) out vec3 vColor;
layout(location = 1) out vec3 vPosWS;

layout(push_constant) uniform WorldDebugPC {
    mat4 uViewProj;
    mat4 uModel;
} pc;

void main()
{
    gl_Position = pc.uViewProj * pc.uModel * vec4(aPos, 1.0);
    vColor = normalize(aNormal) * 0.5 + vec3(0.5);
    vPosWS = (pc.uModel * vec4(aPos, 1.0)).xyz;
}

