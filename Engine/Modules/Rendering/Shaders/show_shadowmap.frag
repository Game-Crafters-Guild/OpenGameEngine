#version 450

layout(location = 0) in vec2 vUV;           // from fullscreen_noinput.vert
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2D uTex; // shadow map sampled as regular 2D

// 0 = gradient (draw path sanity), 1 = sample depth
const int kDebugMode = 1;

void main()
{
    if (kDebugMode == 0) {
        // Show a clear gradient to prove this pass writes to the backbuffer
        oColor = vec4(vUV, 0.0, 1.0);
        return;
    }

    // Depth view: vUV spans [0,1]; sample the entire shadow map
    float d = texture(uTex, vUV).r;
    oColor = vec4(d, d, d, 1.0);
}
