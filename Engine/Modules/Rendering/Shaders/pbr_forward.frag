#version 450

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec3 vN;
layout(location = 2) in vec3 vPosView;
layout(location = 3) in vec3 vPosWorld;
layout(location = 4) in vec4 vPosLightClip;
layout(location = 0) out vec4 oColor;

// Combined image samplers for simplicity in metadata/descriptor setup
layout(set = 0, binding = 0)  uniform sampler2D    uAlbedoTex;        // sRGB texture (runtime)
layout(set = 0, binding = 1)  uniform sampler2D    uNormalTex;        // linear normal map (unused for now)
layout(set = 0, binding = 2)  uniform sampler2D    uMetallicRoughTex; // linear (R=metallic, G=roughness)
layout(set = 0, binding = 3)  uniform sampler2D    uAOTex;            // linear AO
layout(set = 0, binding = 4)  uniform sampler2D    uBRDFLUT;          // split-sum BRDF LUT (rg)
layout(set = 0, binding = 11) uniform sampler2D    uEnvMap;           // Optional lat-long environment (fallback)
layout(set = 0, binding = 12) uniform samplerCube  uDiffuseEnvMap;    // Pre-baked diffuse irradiance cube
layout(set = 0, binding = 13) uniform samplerCube  uSpecularEnvMap;   // Pre-baked specular IBL cube

// Light uniforms: light VP for shadow projection (set0,binding6)
layout(set = 0, binding = 6) uniform LightUBO {
    mat4 uLightVP;
    vec4 uLightDirWorld; // optional, not required for view-space lighting here
    vec4 uLightParams0;  // x=worldPerTexelX, y=worldPerTexelY, z=tan(angularRadius), w=depthSpan
} lightUBO;

// Shadow map samplers
layout(set = 0, binding = 7) uniform sampler2DShadow uShadowMap;   // compare sampler
layout(set = 0, binding = 9) uniform sampler2D       uShadowMapRaw; // non-compare raw depth for PCSS
layout(set = 0, binding = 10) uniform sampler2D      uShadowEVSM;   // EVSM blurred moments (rgba)
// Shadow sampling settings (std140-friendly packing)
layout(set = 0, binding = 8) uniform ShadowSettings {
    ivec4 i0; // x=taps, y=pcssEnable (also reused as EVSM depth debug flag in evsm_write), z=blueNoise, w=evsmEnable
    vec4  f0; // x=pcfRadiusTexels, y=pcssSearchRadiusTexels, z=pcssFilterMaxTexels, w=evsmK
    vec4  f1; // x=evsmLBR (light bleeding reduction [0..0.99]), y=minVar, z=receiverBiasBase, w=receiverBiasSlope
};

// Shared push constants across VS/FS (match pbr_forward.vert)
layout(push_constant) uniform PC {
    mat4 uM;       // model matrix (matches VS)
    vec4 uN0;
    vec4 uN1;
    vec4 uN2;

    vec4 lightDir;
} pc;

const float PI = 3.14159265359;
const float kIblDiffuseStrength = 0.5;
const float kIblSpecularStrength = 0.3;

const vec2 kPoisson16[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870), vec2(0.34495938, 0.29387760),
    vec2(-0.91588581, 0.45771432),  vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543, 0.27676845),  vec2(0.97484398, 0.75648379),
    vec2(0.44323325, -0.97511554),  vec2(0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
    vec2(-0.24188840, 0.99706507),  vec2(-0.81409955, 0.91437590),
    vec2(0.19984126, 0.78641367),   vec2(0.14383161, -0.14100790)
);

vec2 DirectionToLatLong(vec3 dir)
{
    dir = normalize(dir);
    float phi = atan(dir.z, dir.x);
    float theta = acos(clamp(dir.y, -1.0, 1.0));
    float u = phi / (2.0 * PI) + 0.5;
    float v = theta / PI;
    return vec2(u, v);
}

vec3 SchlickFresnel(vec3 F0, float cosTheta)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

float D_GGX(float NdotH, float a)
{
    float a2 = a*a;

    float d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float G_SchlickGGX(float NdotV, float k)
{
    return NdotV / (NdotV * (1.0 - k) + k);
}

float G_Smith(float NdotV, float NdotL, float roughness)
{
    float r = roughness + 1.0;
    float k = (r*r) / 8.0; // direct lighting (not IBL) remapping
    return G_SchlickGGX(NdotV, k) * G_SchlickGGX(NdotL, k);
}

void main()
{
    vec3 N = normalize(vN);
    vec3 V = normalize(-vPosView);
    // Light vector in shader is from surface point to the light
    vec3 L = normalize(pc.lightDir.xyz);
    vec3 H = normalize(V + L);

    // Shadow mapping: explicit divide by w, then map x/y to [0,1] (Vulkan z already 0..1)
    // Use interpolated light clip from VS to ensure consistency with shadow pass
    vec4 clipL = vPosLightClip;
    vec3 ndc = clipL.xyz / max(clipL.w, 1e-5);
    vec2 shadowUV = ndc.xy * 0.5 + 0.5;
    // Vulkan textures are addressed with (0,0) at top-left: flip Y by default for shadow map sampling
    shadowUV.y = 1.0 - shadowUV.y;
    // Optional extra flips for diagnostics
    int flipFlags = int(pc.uN0.w + 0.5);
    if ((flipFlags & 2) != 0) { shadowUV.x = 1.0 - shadowUV.x; }
    float shadowDepth = clamp(ndc.z, 0.0, 1.0);
    if (pc.uN1.w < -0.5) { shadowDepth = 1.0 - shadowDepth; }

    // Debug: invert sampled shadow depth (gives forward-Z semantics under reverse-Z; useful for A/B).
    bool invertSampleZ = (pc.uN1.w < -0.5);

    // PCF without edge clamping: skip OOB taps and treat them as lit (no occlusion)
    bool inBounds = (shadowUV.x >= 0.0 && shadowUV.x <= 1.0 && shadowUV.y >= 0.0 && shadowUV.y <= 1.0) && (shadowDepth >= 0.0 && shadowDepth <= 1.0);
    float ndotl = clamp(dot(N, L), 0.0, 1.0);
    // Receiver bias from UBO (f1.z = base, f1.w = slope), defaulted in CPU to sensible values.
    // Reverse-Z: ADD bias to push the receiver away from the light (toward 0.0).
    float receiverBias = f1.z + f1.w * (1.0 - ndotl);
    float ref = shadowDepth + receiverBias;
    // Hardware compare (with linear filtering = 2x2 PCF) + optional Poisson PCF taps or PCSS
    float shadow = 1.0;
    int tapsClamped = max(1, min(i0.x, 16));
    if (i0.w != 0) {
        // EVSM mode: treat OOB as lit (match PCF) and clamp UVs to half-texel inside.
        // Reverse-Z: evsm_write.frag stores moments computed from (1.0 - z), keeping
        // the EVSM math forward-Z internally. Un-reverse the receiver depth here so
        // the Chebyshev bound retains its original semantics (occluders at small z).
        float ref_evsm = (1.0 - shadowDepth) - receiverBias;
        if (!inBounds) {
            shadow = 1.0;
        } else {
            vec2 texSize = vec2(textureSize(uShadowEVSM, 0));
            vec2 eps = 0.5 / max(texSize, vec2(1.0));
            // Use a stricter safe-rect check; treat near-edge samples as lit to avoid border artifacts
            bool inSafe = (shadowUV.x >= eps.x && shadowUV.x <= 1.0 - eps.x && shadowUV.y >= eps.y && shadowUV.y <= 1.0 - eps.y);
            if (!inSafe) { shadow = 1.0; }
            else {
                vec2 uv  = clamp(shadowUV, eps, vec2(1.0) - eps);
                vec4 m = texture(uShadowEVSM, uv);
                // With R32 moments we can support a much larger k (helps show differences)
                float k = clamp(f0.w, 1.0, 80.0);
                float lbr = clamp(f1.x, 0.0, 0.99);
                // Positive warp — left-tail bound (only evaluate Chebyshev bound when warped depth is above the mean)
                float tp = exp(k * ref_evsm);
                float mu1p = m.r;
                float mu2p = m.g;
                float varp = max(mu2p - mu1p * mu1p, 0.0);
                float minVar = max(1e-6, f1.y);
                varp = max(varp, minVar);
                float pP = 1.0;
                if (tp > mu1p)
                {
                    float dp = tp - mu1p;
                    float p = varp / (varp + dp * dp + 1e-6);
                    pP = clamp((p - lbr) / max(1.0 - lbr, 1e-3), 0.0, 1.0);
                }
                // Negative warp — right-tail bound
                float tn = exp(-k * ref_evsm);
                float mu1n = m.b;
                float mu2n = m.a;
                float varn = max(mu2n - mu1n * mu1n, 0.0);
                varn = max(varn, minVar);
                float pN = 1.0;
                if (tn > mu1n)
                {
                    float dn = tn - mu1n;
                    float pn = varn / (varn + dn * dn + 1e-6);
                    pN = clamp((pn - lbr) / max(1.0 - lbr, 1e-3), 0.0, 1.0);
                }
                shadow = clamp(min(pP, pN), 0.0, 1.0);
            }
        }
    } else if (i0.y != 0) {
        // PCSS: blocker search (raw depth), then variable-radius PCF using compare sampler
        ivec2 ts = textureSize(uShadowMap, 0);
        vec2 texel = 1.0 / vec2(max(1, ts.x), max(1, ts.y));
        float searchRadius = max(0.0, f0.y);
        float filterMax = max(0.0, f0.z);
        // Optional blue-noise-like per-texel rotation (stable in light texel space)
        mat2 R = mat2(1.0, 0.0, 0.0, 1.0);
        if (i0.z != 0) {
            vec2 pix = floor(shadowUV * vec2(ts));
            float h = fract(sin(dot(pix, vec2(12.9898, 78.233))) * 43758.5453);
            float ang = 6.2831853 * h; float c = cos(ang), s = sin(ang);
            R = mat2(c, -s, s, c);
        }
        // Blocker search.
        // Reverse-Z: a stored value LARGER than the receiver means "closer to light" → blocker.
        float blockerSum = 0.0; int blockerCount = 0;
        for (int i = 0; i < 16; ++i) {
            vec2 duv = (R * kPoisson16[i]) * searchRadius * texel;
            float d = texture(uShadowMapRaw, shadowUV + duv).r;
            if (d > ref + 1e-5) { blockerSum += d; blockerCount++; }
        }
        if (blockerCount == 0) {
            shadow = 1.0; // fully lit
        } else {
            float avgBlocker = blockerSum / float(blockerCount);
            // Physical penumbra for directional light: (DeltaZ_world * tan(theta)) converted to texels
            float invWptAvg = 2.0 / max(1e-6, (lightUBO.uLightParams0.x + lightUBO.uLightParams0.y));
            // Reverse-Z: blocker depth > receiver, so swap the subtraction to keep penumbra non-negative.
            float penTexels = max(0.0, (avgBlocker - ref)) * lightUBO.uLightParams0.w * lightUBO.uLightParams0.z * invWptAvg;
            float pen = clamp(penTexels, 0.0, filterMax);
            int fltTaps = 16; float sum = 0.0;
            for (int i = 0; i < fltTaps; ++i) {
                vec2 duv = (R * kPoisson16[i]) * pen * texel;
                sum += texture(uShadowMap, vec3(shadowUV + duv, ref));
            }
            shadow = sum / float(fltTaps);
        }
    } else {
        // PCF path using hardware compare sampler (GreaterOrEqual under reverse-Z)
        ivec2 ts = textureSize(uShadowMap, 0);
        vec2 texSize = vec2(max(1, ts.x), max(1, ts.y));
        vec2 eps = 0.5 / texSize;
        bool inSafe = (shadowUV.x >= eps.x && shadowUV.x <= 1.0 - eps.x && shadowUV.y >= eps.y && shadowUV.y <= 1.0 - eps.y);
        if (!inSafe) {
            shadow = 1.0; // treat near-edge/out-of-bounds as lit to avoid wedge artifacts
        } else if (tapsClamped == 1 || f0.x <= 0.0) {
            vec2 uv = clamp(shadowUV, eps, vec2(1.0) - eps);
            shadow = texture(uShadowMap, vec3(uv, ref));
        } else {
            vec2 texel = 1.0 / texSize;
            // Optional blue-noise-like per-texel rotation
            mat2 R = mat2(1.0, 0.0, 0.0, 1.0);
            if (i0.z != 0) {
                vec2 pix = floor(shadowUV * texSize);
                float h = fract(sin(dot(pix, vec2(12.9898, 78.233))) * 43758.5453);
                float ang = 6.2831853 * h; float c = cos(ang), s = sin(ang);
                R = mat2(c, -s, s, c);
            }
            float sum = 0.0;
            vec2 base = clamp(shadowUV, eps, vec2(1.0) - eps);
            for (int i = 0; i < tapsClamped; ++i) {
                vec2 duv = (R * kPoisson16[i]) * f0.x * texel;
                sum += texture(uShadowMap, vec3(base + duv, ref));
            }
            shadow = sum / float(tapsClamped);
        }
    }

    // If debug flag is set in push constants, output the shadow diagnostics directly
    if (pc.lightDir.w > 0.5) {
        if (pc.uN1.w > 0.5) {
            // UV/depth visualization mode (post-bias)
            vec3 ndcVis = vec3(shadowUV, shadowDepth);
            oColor = vec4(clamp(ndcVis, 0.0, 1.0), 1.0);
            return;
        }
        // EVSM/PCF visibility in overlay when requested via negative uN2.w (matches normal-mode visualize)
        if (pc.uN2.w < -0.5) {
            oColor = vec4(shadow, shadow, shadow, 1.0);
            return;
        }
        if (pc.uN2.w > 1.5) {
            // NdotL / shadow / product visualization
            float ndotl_dbg = clamp(dot(N, L), 0.0, 1.0);
            oColor = vec4(ndotl_dbg, shadow, ndotl_dbg * shadow, 1.0);
            return;
        }
        // Compare visualization (raw depth path to disambiguate compare-op):
        // R = ref depth (with bias/inversion), G = raw compare (1 if rawD <= ref under reverse-Z), B = inBounds flag
        float rawD = texture(uShadowMapRaw, shadowUV).r;
        if (invertSampleZ) { rawD = 1.0 - rawD; }
        float vis = rawD <= ref ? 1.0 : 0.0;
        oColor = vec4(clamp(ref,0.0,1.0), vis, inBounds ? 1.0 : 0.0, 1.0);
        return;
    }

    // Material inputs (with reasonable defaults if textures are flat)
    vec3 baseColor = texture(uAlbedoTex, vUV).rgb;
    vec2 mr = texture(uMetallicRoughTex, vUV).rg;
    float metallic = clamp(mr.r, 0.0, 1.0);
    float roughness = clamp(mr.g, 0.04, 1.0);
    float ao = texture(uAOTex, vUV).r;

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    // Fresnel at normal incidence: lerp between dielectric and metal
    vec3 F0 = mix(vec3(0.04), baseColor, metallic);
    vec3  F = SchlickFresnel(F0, VdotH);
    float D = D_GGX(NdotH, roughness);
    float G = G_Smith(NdotV, NdotL, roughness);

    vec3 numerator = D * G * F;
    float denom = max(4.0 * NdotV * NdotL, 1e-4);
    vec3 specular = numerator / denom;

    vec3 kS = F;
    vec3 kD = (1.0 - kS) * (1.0 - metallic);
    vec3 diffuse = (baseColor / PI) * kD;

    // Optional visualization in normal mode (not in debug overlay): pc.uN2.w < -0.5
    if (pc.lightDir.w <= 0.5 && pc.uN2.w < -0.5) {
        oColor = vec4(shadow, shadow, shadow, 1.0);
        return;
    }

    vec3 color = (diffuse + specular) * (NdotL * shadow) * ao;

    // Image-based lighting using pre-baked cubemaps when available.
    // When only 1x1 fallback cubemaps are bound (demo neutral IBL), first try
    // a dynamic lat-long environment (uEnvMap) and otherwise fall back to a
    // lightweight approximation so scenes are still reasonably lit.
    vec3 R = reflect(-V, N);

    // Detect whether we have a real prefiltered specular cubemap (more than 1 mip or >1 texel).
    int specLevels = textureQueryLevels(uSpecularEnvMap);
    ivec2 specSize = textureSize(uSpecularEnvMap, 0);
    bool hasEnvCubemaps = (specLevels > 1) || (specSize.x > 1 || specSize.y > 1);

    // Detect whether we have a real lat-long environment (more than 1 texel).
    ivec2 envSize = textureSize(uEnvMap, 0);
    bool hasLatLongEnv = (envSize.x > 1 || envSize.y > 1);

    if (hasEnvCubemaps) {
        // Diffuse irradiance from a pre-integrated cubemap.
        vec3 envDiffuse = texture(uDiffuseEnvMap, N).rgb;

        // Specular from a prefiltered environment cubemap, selecting mip by roughness.
        float maxSpecularMip = float(specLevels - 1);
        float lod = roughness * maxSpecularMip;
        vec3 envSpec = textureLod(uSpecularEnvMap, R, lod).rgb;

        // BRDF split-sum lookup: NdotV and roughness.
        vec3 brdf = texture(uBRDFLUT, vec2(NdotV, roughness)).rgb;

        vec3 iblDiffuse = envDiffuse * (baseColor / PI) * (1.0 - metallic) * ao * kIblDiffuseStrength;
        vec3 iblSpec = envSpec * (F0 * brdf.x + brdf.y) * kIblSpecularStrength;
        color += iblDiffuse + iblSpec;
    } else if (hasLatLongEnv) {
        // Dynamic IBL from a lat-long environment rendered from the analytic sky.
        vec2 uvN = DirectionToLatLong(N);
        vec2 uvR = DirectionToLatLong(R);
        vec3 envDiffuse = texture(uEnvMap, uvN).rgb;
        vec3 envSpec = texture(uEnvMap, uvR).rgb;

        vec3 brdf = texture(uBRDFLUT, vec2(NdotV, roughness)).rgb;

        vec3 iblDiffuse = envDiffuse * (baseColor / PI) * (1.0 - metallic) * ao * kIblDiffuseStrength;
        vec3 iblSpec = envSpec * (F0 * brdf.x + brdf.y) * kIblSpecularStrength;
        color += iblDiffuse + iblSpec;
    } else {
        // Fallback: lightweight IBL approximation similar to the original demo.
        vec3 envColor = vec3(1.0);
        // Diffuse ambient scaled modestly, only for dielectric portion.
        vec3 iblDiffuse = envColor * (baseColor / PI) * (1.0 - metallic) * ao * 0.006;
        // Specular IBL uses BRDF LUT; keep conservative to preserve contrast.
        vec3 brdf = texture(uBRDFLUT, vec2(NdotV, 1.0 - roughness)).rgb;
        vec3 iblSpec = envColor * (F0 * brdf.x + brdf.y) * 0.12;
        color += iblDiffuse + iblSpec;
    }

    // Optional stronger visibility application for diagnosis: enabled when pc.uN2.w > 0.5
    if (pc.lightDir.w <= 0.5 && pc.uN2.w > 0.5) {
        // Mix towards a darker base in occluded regions to amplify visibility
        color = mix(color * 0.25, color, shadow);
    }

    // Simple exposure control via pc.uN2.w in [-0.4, 0.4] range (mapped to exp2)
    float exposure = exp2(clamp(pc.uN2.w, -5.0, 5.0));
    color *= exposure;

    oColor = vec4(color, 1.0);
}
