#version 450

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec3 vN;
layout(location = 2) in vec3 vPosView;
layout(location = 3) in vec3 vPosWorld;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uAlbedoTex;
layout(set = 0, binding = 1) uniform sampler2D uNormalTex;
layout(set = 0, binding = 2) uniform sampler2D uMetallicRoughTex;
layout(set = 0, binding = 3) uniform sampler2D uAOTex;
layout(set = 0, binding = 4) uniform sampler2D uBRDFLUT;

layout(set = 0, binding = 6) uniform LightUBO {
    mat4 uLightVP;
    vec4 uLightDirWorld;
} lightUBO;

layout(set = 0, binding = 7) uniform sampler2D uShadowMap;

layout(push_constant) uniform PC {
    mat4 uM;
    vec4 uN0;
    vec4 uN1;
    vec4 uN2;
    vec4 lightDir;
} pc;

void main()
{
    vec3 N = normalize(vN);
    vec3 V = normalize(-vPosView);
    vec3 L = normalize(-pc.lightDir.xyz);

    // Shadow mapping: project world position into light clip, then to UV
    vec4 posLight = lightUBO.uLightVP * vec4(vPosWorld, 1.0);
    vec3 ndc = posLight.xyz / max(posLight.w, 1e-5);
    vec2 shadowUV = ndc.xy * 0.5 + 0.5;
    float shadowDepth = clamp(ndc.z, 0.0, 1.0);

    float shadow = 1.0;
    if (shadowUV.x >= 0.0 && shadowUV.x <= 1.0 && shadowUV.y >= 0.0 && shadowUV.y <= 1.0) {
        vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
        float ndotl = clamp(dot(N, L), 0.0, 1.0);
        float bias = max(0.0015, 0.01 * (1.0 - ndotl));
        float sum = 0.0;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                float d = texture(uShadowMap, shadowUV + vec2(dx,dy) * texel).r;
                sum += (shadowDepth + bias) >= d ? 1.0 : 0.0;  // reverse-Z
            }
        }
        shadow = sum / 9.0;
    }

    // Output grayscale of the shadow factor directly
    oColor = vec4(vec3(shadow), 1.0);
}

