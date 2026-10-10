#version 450

layout(location = 0) in vec3 aPos;

// Camera UBO at set0,binding5 (same as other demos)
layout(set = 0, binding = 5) uniform CameraUBO {
#include "Includes/camera_ubo_fields.glsl"
} cam;

layout(push_constant) uniform PC {
    mat4 uM;      // model matrix
    vec4 uColor;  // rgb=color, a=blend weight
} pc;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = cam.uVP * pc.uM * vec4(aPos, 1.0);
    vColor = pc.uColor;
}

