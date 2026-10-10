#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSceneColor;
layout(set = 0, binding = 1) uniform sampler2D uDepth;

layout(set = 0, binding = 2, std140) uniform ViewParamsBlock
{
    // 160 B core prefix of the shared ViewParams list. HeightFog lives in its
    // own module, so the field list is reached by relative path into Rendering;
    // glslc resolves #include against the source file's directory.
#define GE_VP_MAT4(name) mat4 name;
#define GE_VP_VEC4(name) vec4 name;
#define GE_VIEWPARAMS_CORE_ONLY 1
#include "../../Rendering/Shaders/Includes/view_params_fields.glsl"
#undef GE_VIEWPARAMS_CORE_ONLY
#undef GE_VP_VEC4
#undef GE_VP_MAT4
} ViewParams;

layout(set = 0, binding = 3, std140) uniform HeightFogParamsBlock
{
    float intensity;
    float density;
    float minDistance;
    float smoothLength;
    float baseHeight;
    float transitionLength;
    float maxDistance;
    float gradientStrength;
    float emissiveR;
    float emissiveG;
    float emissiveB;
    float sunIntensity;
    float sunDirX;
    float sunDirY;
    float sunDirZ;
    float phase;
    float sunColorR;
    float sunColorG;
    float sunColorB;
    float phaseWeight0;
    float phaseWeight1;
    float skyPower;
    float skyFillStart;
    float skyFillEnd;
    float axisX;
    float axisY;
    float axisZ;
    float noiseScale;
    float gradientLowR;
    float gradientLowG;
    float gradientLowB;
    float noiseStrength;
    float gradientHighR;
    float gradientHighG;
    float gradientHighB;
    float noiseContrast;
    float noiseVelX;
    float noiseVelY;
    float noiseVelZ;
    float noiseTime;
    int flags;
    int axisMode;
    int gradientMode;
    float maxOpacity;
    int layerMode;
    float horizonHeightOffset;
    float horizonHeightBlendStart;
    float horizonHeightBlendEnd;
    float noiseMin;
    float noiseMax;
    float noiseFadeStart;
    float noiseFadeEnd;
    float skyHorizonOffset;
    float skyBottomStrength;
    float _pad0;
    float _pad1;
} HeightFogParams;

const float kInvFourPi = 0.07957747154594767;
const int kFlagDistanceEnabled = 1;
const int kFlagHeightEnabled = 2;
const int kFlagNoiseEnabled = 4;
const int kFlagSkyEnabled = 8;

float Saturate(float x)
{
    return clamp(x, 0.0, 1.0);
}

float Unlerp(float a, float b, float x)
{
    return (x - a) / max(b - a, 1e-5);
}

float MaxFogOpticalDepth(float density)
{
    float d = clamp(density, 0.0, 1.0);
    return d * d * 4.0;
}

float FogExtinctionPerWorldUnit(float density)
{
    return MaxFogOpticalDepth(density) / max(HeightFogParams.maxDistance, 1.0);
}

vec2 ViewportUvToNdc(vec2 uv)
{
    return vec2(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0);
}

float HgPhase(float g, float theta)
{
    float gg = g * g;
    float denom = pow(abs(1.0 + gg - 2.0 * g * theta), 1.5);
    return kInvFourPi * ((1.0 - gg) / max(denom, 1e-5));
}

float RdrPhaseM2(float g, float theta, float sigmaExt, float w0, float w1)
{
    float hg = HgPhase(g, theta) * w0;
    float c = (w1 * sigmaExt);
    float c0 = HgPhase((2.0 / 3.0) * g, theta);
    float c1 = HgPhase((4.0 / 9.0) * g, theta);
    return hg + c * (c0 + c1);
}

float FogHash(vec3 p)
{
    p = fract(p * 0.3183099 + vec3(0.1, 0.2, 0.3));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float FogValueNoise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = FogHash(i + vec3(0.0, 0.0, 0.0));
    float n100 = FogHash(i + vec3(1.0, 0.0, 0.0));
    float n010 = FogHash(i + vec3(0.0, 1.0, 0.0));
    float n110 = FogHash(i + vec3(1.0, 1.0, 0.0));
    float n001 = FogHash(i + vec3(0.0, 0.0, 1.0));
    float n101 = FogHash(i + vec3(1.0, 0.0, 1.0));
    float n011 = FogHash(i + vec3(0.0, 1.0, 1.0));
    float n111 = FogHash(i + vec3(1.0, 1.0, 1.0));
    float nx00 = mix(n000, n100, f.x);
    float nx10 = mix(n010, n110, f.x);
    float nx01 = mix(n001, n101, f.x);
    float nx11 = mix(n011, n111, f.x);
    float nxy0 = mix(nx00, nx10, f.y);
    float nxy1 = mix(nx01, nx11, f.y);
    return mix(nxy0, nxy1, f.z);
}

float FogFbm(vec3 p)
{
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 3; ++i)
    {
        sum += amp * FogValueNoise(p);
        p = p * 2.03 + vec3(17.0, 31.0, 47.0);
        amp *= 0.5;
    }
    return sum;
}

vec3 ResolveHeightAxis()
{
    vec3 axis = vec3(HeightFogParams.axisX, HeightFogParams.axisY, HeightFogParams.axisZ);
    float axisLen = length(axis);
    if (axisLen <= 1e-5)
        axis = vec3(0.0, 1.0, 0.0);
    else
        axis /= axisLen;
    return axis;
}

float HeightFogGradientT(vec3 originWS, vec3 viewDirWS, float segmentLength, vec2 uv, vec3 sunDir)
{
    if (HeightFogParams.gradientMode == 1)
        return Saturate(segmentLength / max(HeightFogParams.maxDistance, 0.01));
    if (HeightFogParams.gradientMode == 2)
    {
        vec3 axis = ResolveHeightAxis();
        vec3 samplePos = originWS + viewDirWS * segmentLength * 0.5;
        return Saturate((dot(samplePos, axis) - HeightFogParams.baseHeight) / max(HeightFogParams.transitionLength, 0.01));
    }
    if (HeightFogParams.gradientMode == 3)
        return Saturate(1.0 - uv.y);
    if (HeightFogParams.gradientMode == 4)
        return Saturate(dot(viewDirWS, normalize(sunDir)) * 0.5 + 0.5);
    return 0.5;
}

vec3 ApplyDualColor(vec3 baseColor, vec3 originWS, vec3 viewDirWS, float segmentLength, vec2 uv, vec3 sunDir)
{
    if (HeightFogParams.gradientMode == 0 || HeightFogParams.gradientStrength <= 1e-5)
        return baseColor;

    float t = HeightFogGradientT(originWS, viewDirWS, segmentLength, uv, sunDir);
    vec3 lowColor = vec3(HeightFogParams.gradientLowR, HeightFogParams.gradientLowG, HeightFogParams.gradientLowB);
    vec3 highColor = vec3(HeightFogParams.gradientHighR, HeightFogParams.gradientHighG, HeightFogParams.gradientHighB);
    vec3 gradientColor = mix(lowColor, highColor, t);
    return mix(baseColor, gradientColor, clamp(HeightFogParams.gradientStrength, 0.0, 1.0));
}

float FogNoiseDensityMultiplier(vec3 worldPos, float segmentLength)
{
    vec3 velocity = vec3(HeightFogParams.noiseVelX, HeightFogParams.noiseVelY, HeightFogParams.noiseVelZ);
    vec3 noiseP = worldPos / max(HeightFogParams.noiseScale, 0.01) + velocity * HeightFogParams.noiseTime;
    float raw = pow(clamp(FogFbm(noiseP), 0.0, 1.0), max(HeightFogParams.noiseContrast, 0.05));
    float low = clamp(HeightFogParams.noiseMin, 0.0, 1.0);
    float high = max(clamp(HeightFogParams.noiseMax, 0.0, 1.0), low + 1e-4);
    float remapped = Saturate((raw - low) / (high - low));

    float fade = 1.0;
    if (HeightFogParams.noiseFadeEnd > HeightFogParams.noiseFadeStart + 1e-4)
        fade = 1.0 - smoothstep(HeightFogParams.noiseFadeStart, HeightFogParams.noiseFadeEnd, segmentLength);

    float amount = clamp(HeightFogParams.noiseStrength, 0.0, 1.0) * fade;
    return mix(1.0, remapped * 1.8, amount);
}

float EffectiveHeightFogBase(float segmentLength)
{
    float baseHeight = HeightFogParams.baseHeight;
    if (abs(HeightFogParams.horizonHeightOffset) <= 1e-5)
        return baseHeight;

    if (HeightFogParams.horizonHeightBlendEnd <= HeightFogParams.horizonHeightBlendStart + 1e-4)
        return baseHeight;

    float t = smoothstep(HeightFogParams.horizonHeightBlendStart,
                         HeightFogParams.horizonHeightBlendEnd,
                         segmentLength);
    return baseHeight + HeightFogParams.horizonHeightOffset * t;
}

float OpticalDepthHeightFog(float extinction, float baseHeight, float heightScale, float cosZenith, float startHeight, float intervalLength)
{
    float H = max(heightScale, 0.01);
    float rcpH = 1.0 / H;
    float z = cosZenith;
    float absZ = max(abs(z), 0.001);
    float rcpAbsZ = 1.0 / absZ;

    float endHeight = startHeight + intervalLength * z;
    float minHeight = min(startHeight, endHeight);
    float h = max(minHeight - baseHeight, 0.0);

    float homFogDist = clamp((baseHeight - minHeight) * rcpAbsZ, 0.0, intervalLength);
    float expFogDist = intervalLength - homFogDist;
    float expFogMult = exp(-h * rcpH) * (1.0 - exp(-expFogDist * absZ * rcpH)) * (rcpAbsZ * H);
    return extinction * (homFogDist + expFogMult);
}

vec3 SampleHeightFogLighting(vec3 viewDir, float extinction, vec3 emissive)
{
    vec3 sunDir = normalize(vec3(HeightFogParams.sunDirX, HeightFogParams.sunDirY, HeightFogParams.sunDirZ));
    vec3 sunColor = vec3(HeightFogParams.sunColorR, HeightFogParams.sunColorG, HeightFogParams.sunColorB) * max(HeightFogParams.sunIntensity, 0.0);
    float theta = dot(sunDir, viewDir);
    float phase = RdrPhaseM2(clamp(HeightFogParams.phase, -0.95, 0.95), theta, extinction,
                             max(HeightFogParams.phaseWeight0, 0.0), max(HeightFogParams.phaseWeight1, 0.0));
    return emissive + sunColor * Saturate(phase);
}

void ComputeHeightFog(vec3 originWS, vec3 viewDirWS, float segmentLength, vec2 uv, out vec3 scattering, out float transmittance)
{
    float density = clamp(HeightFogParams.density, 0.0, 1.0);
    float extinction = FogExtinctionPerWorldUnit(density);
    if (extinction <= 1e-7)
    {
        scattering = vec3(0.0);
        transmittance = 1.0;
        return;
    }

    if ((HeightFogParams.flags & kFlagNoiseEnabled) != 0 && HeightFogParams.noiseStrength > 1e-5)
    {
        vec3 samplePos = originWS + viewDirWS * segmentLength * 0.5;
        extinction *= FogNoiseDensityMultiplier(samplePos, segmentLength);
    }

    segmentLength = min(segmentLength, max(HeightFogParams.maxDistance, 0.01));

    float distanceOpticalDepth = 0.0;
    if ((HeightFogParams.flags & kFlagDistanceEnabled) != 0)
    {
        float smoothLength = max(HeightFogParams.smoothLength, 0.0001);
        float x = max(0.0, segmentLength - max(HeightFogParams.minDistance, 0.0));
        if (x < smoothLength)
        {
            float invS = 1.0 / smoothLength;
            distanceOpticalDepth = invS * invS * x * x * x - 0.5 * invS * invS * invS * x * x * x * x;
        }
        else
        {
            distanceOpticalDepth = (x - smoothLength) + 0.5 * smoothLength;
        }
        distanceOpticalDepth *= extinction;
    }

    float heightOpticalDepth = 0.0;
    if ((HeightFogParams.flags & kFlagHeightEnabled) != 0)
    {
        vec3 axis = ResolveHeightAxis();
        float heightScale = max(HeightFogParams.transitionLength, 0.01) * 0.144765;
        float baseHeight = EffectiveHeightFogBase(segmentLength);
        heightOpticalDepth = OpticalDepthHeightFog(extinction, baseHeight, heightScale,
                                                   dot(viewDirWS, axis), dot(originWS, axis), segmentLength);
    }

    float opticalDepth = 0.0;
    if ((HeightFogParams.flags & kFlagDistanceEnabled) != 0 && (HeightFogParams.flags & kFlagHeightEnabled) != 0)
        opticalDepth = (HeightFogParams.layerMode == 1)
            ? (distanceOpticalDepth + heightOpticalDepth)
            : max(distanceOpticalDepth, heightOpticalDepth);
    else if ((HeightFogParams.flags & kFlagDistanceEnabled) != 0)
        opticalDepth = distanceOpticalDepth;
    else if ((HeightFogParams.flags & kFlagHeightEnabled) != 0)
        opticalDepth = heightOpticalDepth;

    float fogOpacity = clamp(1.0 - exp(-opticalDepth), 0.0, clamp(HeightFogParams.maxOpacity, 0.0, 1.0));
    transmittance = 1.0 - fogOpacity;

    vec3 sunDir = vec3(HeightFogParams.sunDirX, HeightFogParams.sunDirY, HeightFogParams.sunDirZ);
    vec3 emissive = vec3(HeightFogParams.emissiveR, HeightFogParams.emissiveG, HeightFogParams.emissiveB);
    emissive = ApplyDualColor(emissive, originWS, viewDirWS, segmentLength, uv, sunDir);
    vec3 fogColor = SampleHeightFogLighting(viewDirWS, extinction, emissive);
    vec3 albedo = vec3(extinction);
    scattering = (fogColor * albedo - fogColor * albedo * transmittance) / max(extinction, 0.0001);
}

void main()
{
    vec4 scene = texture(uSceneColor, vUV);
    float fogStrength = clamp(HeightFogParams.intensity, 0.0, 1.0);
    if (fogStrength <= 1e-5 || HeightFogParams.density <= 1e-5)
    {
        oColor = scene;
        return;
    }

    ivec2 depthSize = textureSize(uDepth, 0);
    ivec2 pix = ivec2(clamp(gl_FragCoord.xy, vec2(0.0), vec2(depthSize) - vec2(1.0)));
    float rawDepth = texelFetch(uDepth, pix, 0).r;
    bool sky = rawDepth <= 0.000001;

    float depthForRay = sky ? 0.0 : rawDepth;
    vec2 ndc = ViewportUvToNdc(vUV);
    vec4 pV = ViewParams.ge_invProj * vec4(ndc, depthForRay, 1.0);
    pV.xyz /= max(pV.w, 1e-6);
    float viewLength = sky ? max(ViewParams.ge_nearFar.y, HeightFogParams.maxDistance) : length(pV.xyz);
    vec3 viewDirV = normalize(pV.xyz);
    vec3 viewDirWS = normalize(transpose(mat3(ViewParams.ge_view)) * viewDirV);
    vec3 originWS = ViewParams.ge_cameraPosWS.xyz;

    vec3 scattering;
    float transmittance;
    ComputeHeightFog(originWS, viewDirWS, viewLength, vUV, scattering, transmittance);

    if (sky && (HeightFogParams.flags & kFlagSkyEnabled) != 0)
    {
        vec3 axis = ResolveHeightAxis();
        float axisDot = dot(viewDirWS, axis);
        float s = max(0.0, axisDot + HeightFogParams.skyHorizonOffset);
        s = Saturate(Unlerp(HeightFogParams.skyFillStart, HeightFogParams.skyFillEnd, s));
        s = smoothstep(1.0, 0.0, pow(s, max(HeightFogParams.skyPower, 0.001)));
        float bottom = pow(Saturate(-axisDot), max(HeightFogParams.skyPower, 0.001)) *
                       clamp(HeightFogParams.skyBottomStrength, 0.0, 1.0);
        s = max(s, bottom);
        vec3 sunDir = vec3(HeightFogParams.sunDirX, HeightFogParams.sunDirY, HeightFogParams.sunDirZ);
        vec3 emissive = ApplyDualColor(vec3(HeightFogParams.emissiveR, HeightFogParams.emissiveG, HeightFogParams.emissiveB), originWS, viewDirWS, viewLength, vUV, sunDir);
        vec3 fogColor = SampleHeightFogLighting(viewDirWS, FogExtinctionPerWorldUnit(HeightFogParams.density), emissive);
        float skyAmount = s * (1.0 - exp(-MaxFogOpticalDepth(HeightFogParams.density)));
        skyAmount = min(skyAmount, clamp(HeightFogParams.maxOpacity, 0.0, 1.0));
        scattering = fogColor * skyAmount;
        transmittance = 1.0 - skyAmount;
    }
    else if (sky)
    {
        oColor = scene;
        return;
    }

    // The output alpha (scene.a * effectiveTransmittance) is load-bearing, not a
    // leftover. The shared fog-glow pyramid samples this pass (HDRHeightFog) as its
    // uFogged input and reconstructs per-pixel fog transmittance as
    // fogged.a / scene.a (see fog_glow_fast.frag / fog_glow_prefilter.frag), which
    // isolates the additive fog radiance for the glow veil. Gating this write on
    // "fog glow off" would break that reconstruction, so it stays unconditional.
    //
    // It is safe for the presented image only because the fog-glow passes run
    // immediately after this pass and BEFORE the alpha is overwritten to 1.0
    // downstream (taa_resolve.comp writes vec4(color, 1.0); tonemap.frag forces
    // outA = 1.0 under the default preserveAlpha == 0). If TAA/Tonemap are ever
    // reordered ahead of the fog-glow pyramid, or a preserveAlpha tonemap path is
    // placed downstream of height fog, foggy regions would present semi-transparent
    // — revisit this alpha then.
    float effectiveTransmittance = mix(1.0, clamp(transmittance, 0.0, 1.0), fogStrength);
    vec3 fogged = scene.rgb * clamp(transmittance, 0.0, 1.0) + scattering;
    oColor = vec4(mix(scene.rgb, fogged, fogStrength), scene.a * effectiveTransmittance);
}
