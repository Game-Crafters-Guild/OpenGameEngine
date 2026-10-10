#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

// Camera UBO at set0,binding5 (matches pbr_forward.vert)
layout(set = 0, binding = 5) uniform CameraUBO {
    mat4 uV;
    mat4 uP;
    mat4 uVP;
    vec4 uCameraPos;
} cam;

void main()
{
    // Minimal: map model position to clip space and apply a small UBO-derived x offset
    vec4 pos = vec4(aPos, 1.0);
    float k = cam.uP[0][0];
    pos.x += 0.1 * sign(k == 0.0 ? 1.0 : k);
    gl_Position = pos;
}

