#version 450

layout(location = 0) out vec4 oColor;

// Reuse ShadowSettings binding for EVSM k (f0.w) and optional depth debug flag (i0.y).
layout(set = 0, binding = 8) uniform ShadowSettings {
    ivec4 i0;
    vec4  f0;
} u;

void main()
{
    // R32 moments: allow larger k to exaggerate EVSM bound when desired.
    // Reverse-Z: gl_FragCoord.z has occluders near 1.0; un-reverse so the EVSM moments
    // are stored in forward-Z form. The read side (pbr_forward.frag EVSM block) must
    // un-reverse the receiver depth (1.0 - shadowDepth) to compute its reference.
    float k = clamp(u.f0.w, 1.0, 80.0);
    float z = clamp(1.0 - gl_FragCoord.z, 0.0001, 0.9999);
    // Debug: when i0.y != 0, write linear depth to visualize shadow content (shared packing with PBR ShadowSettings)
    if (u.i0.y != 0) { oColor = vec4(z, z, z, 1.0); return; }
    float eP = exp(k * z);
    float eP2 = exp(2.0 * k * z);
    float eN = exp(-k * z);
    float eN2 = exp(-2.0 * k * z);
    // 4-channel EVSM moments: (e^{+kz}, e^{+2kz}, e^{-kz}, e^{-2kz})
    oColor = vec4(eP, eP2, eN, eN2);
}

