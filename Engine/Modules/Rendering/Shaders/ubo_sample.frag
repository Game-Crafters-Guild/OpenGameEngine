#version 450

layout(set = 0, binding = 5) uniform CameraUBO {
    mat4 uV;
    mat4 uP;
    mat4 uVP;
    vec4 uCameraPos;
} cam;

layout(location = 0) out vec4 outColor;

void main() {
    float v = cam.uV[0][0];
    // NaN check
    if (!(v == v)) { outColor = vec4(1.0, 0.0, 1.0, 1.0); return; } // magenta = NaN
    if (v > 0.9 && v < 1.1) { outColor = vec4(0.0, 1.0, 0.0, 1.0); return; } // green = identity view
    outColor = vec4(0.0, 0.0, 1.0, 1.0); // blue = other value
}
