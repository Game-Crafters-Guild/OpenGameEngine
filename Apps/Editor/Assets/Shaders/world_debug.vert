// Minimal world-space debug shader for ECS default world cubes and thumbnails.
//
// Transforms positions by a view-projection and model matrix provided via
// push constants so that the same simple pipeline can be reused across
// views without a dedicated camera UBO.
//
// Vertex layout: GameEngine::Vertex (position + normal + uv + tangent + bitangent).
// Normal is converted to color for visualization.
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

layout(location = 0) out vec3 vColor;

layout(push_constant) uniform WorldDebugPC {
    mat4 uViewProj;
    mat4 uModel;
} pc;

void main()
{
    gl_Position = pc.uViewProj * pc.uModel * vec4(aPos, 1.0);
    // Convert normal to color: remap [-1,1] to [0,1]
    vColor = normalize(aNormal) * 0.5 + vec3(0.5);
}

