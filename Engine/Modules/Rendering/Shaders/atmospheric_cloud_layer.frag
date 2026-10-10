#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSceneColor;
layout(set = 0, binding = 1) uniform sampler2D uDepth;

layout(set = 0, binding = 2, std140) uniform ViewParamsBlock
{
    // 160 B core prefix of the shared ViewParams list.
#define GE_VP_MAT4(name) mat4 name;
#define GE_VP_VEC4(name) vec4 name;
#define GE_VIEWPARAMS_CORE_ONLY 1
#include "Includes/view_params_fields.glsl"
#undef GE_VIEWPARAMS_CORE_ONLY
#undef GE_VP_VEC4
#undef GE_VP_MAT4
} ViewParams;

layout(push_constant) uniform AtmosLayerPC
{
    float atmosSkyFill;
    float atmosVaporMass;
    float atmosCloudColorR;
    float atmosCloudColorG;
    float atmosCloudColorB;
    float atmosOpacity;
    float atmosFloorHeight;
    float atmosLayerDepth;
    float atmosBodyFrequency;
    float atmosEdgeFrequency;
    float atmosEdgeBreakup;
    float atmosDriftAngle;
    float atmosDriftRate;
    float atmosSunFade;
    float atmosSkyBounce;
    float atmosRimBoost;
    float atmosOcclusion;
    float atmosHistoryWeight;
    float atmosPixelScale;
} pc;

float Saturate(float x)
{
    return clamp(x, 0.0, 1.0);
}

vec2 ViewportUvToNdc(vec2 uv)
{
    return vec2(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0);
}

float Hash(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

float ScreenHash(vec2 p)
{
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

float Noise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = Hash(i + vec3(0.0, 0.0, 0.0));
    float n100 = Hash(i + vec3(1.0, 0.0, 0.0));
    float n010 = Hash(i + vec3(0.0, 1.0, 0.0));
    float n110 = Hash(i + vec3(1.0, 1.0, 0.0));
    float n001 = Hash(i + vec3(0.0, 0.0, 1.0));
    float n101 = Hash(i + vec3(1.0, 0.0, 1.0));
    float n011 = Hash(i + vec3(0.0, 1.0, 1.0));
    float n111 = Hash(i + vec3(1.0, 1.0, 1.0));
    float nx00 = mix(n000, n100, f.x);
    float nx10 = mix(n010, n110, f.x);
    float nx01 = mix(n001, n101, f.x);
    float nx11 = mix(n011, n111, f.x);
    float nxy0 = mix(nx00, nx10, f.y);
    float nxy1 = mix(nx01, nx11, f.y);
    return mix(nxy0, nxy1, f.z);
}

float Fbm(vec3 p)
{
    float total = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; ++i)
    {
        total += Noise(p) * amp;
        p = p * 2.03 + vec3(17.0, 31.0, 47.0);
        amp *= 0.5;
    }
    return total;
}

bool IntersectHeightSlab(vec3 origin, vec3 dir, float bottom, float top, float limit, out float startT, out float endT)
{
    if (abs(dir.y) < 1e-5)
    {
        if (origin.y < bottom || origin.y > top)
            return false;
        startT = 0.0;
        endT = limit;
        return endT > startT;
    }

    float t0 = (bottom - origin.y) / dir.y;
    float t1 = (top - origin.y) / dir.y;
    startT = max(min(t0, t1), 0.0);
    endT = min(max(t0, t1), limit);
    return endT > startT;
}

float CloudDensity(vec3 worldPos, vec3 drift)
{
    float bottom = pc.atmosFloorHeight;
    float depth = max(pc.atmosLayerDepth, 1.0);
    float h = Saturate((worldPos.y - bottom) / depth);
    float lower = smoothstep(0.0, 0.20, h);
    float upper = 1.0 - smoothstep(0.62, 1.0, h);
    float verticalShape = lower * upper;

    float bodyScale = max(1800.0 / max(pc.atmosBodyFrequency, 0.01), 40.0);
    float edgeScale = max(520.0 / max(pc.atmosEdgeFrequency, 0.01), 18.0);
    vec3 bodyP = vec3(worldPos.x / bodyScale, h * 1.7, worldPos.z / bodyScale) + drift;
    vec3 edgeP = vec3(worldPos.x / edgeScale, h * 5.0, worldPos.z / edgeScale) + drift * 1.73 + vec3(13.1, 19.7, 5.3);

    float body = Fbm(bodyP);
    float edge = Fbm(edgeP);
    float shaped = body + (edge - 0.5) * clamp(pc.atmosEdgeBreakup, 0.0, 1.0) * 0.55 + verticalShape * 0.18;
    float fill = Saturate(pc.atmosSkyFill);
    float mass = Saturate(pc.atmosVaporMass);
    float threshold = mix(0.72, 0.34, fill);
    float softness = mix(0.035, 0.18, mass);
    float coverage = smoothstep(threshold - softness, threshold + softness, shaped);
    return coverage * verticalShape * mass;
}

void main()
{
    vec4 scene = texture(uSceneColor, vUV);
    float rawDepth = texture(uDepth, vUV).r;
    float fill = Saturate(pc.atmosSkyFill);
    float mass = Saturate(pc.atmosVaporMass);
    if (fill <= 1e-5 || mass <= 1e-5)
    {
        oColor = scene;
        return;
    }

    float angle = radians(pc.atmosDriftAngle);
    vec2 driftDir = vec2(cos(angle), sin(angle));
    vec3 drift = vec3(driftDir.x, 0.0, driftDir.y) * pc.atmosDriftRate;

    bool sky = rawDepth <= 0.000001;
    float depthForRay = sky ? 0.0 : rawDepth;
    vec2 ndc = ViewportUvToNdc(vUV);
    vec4 pV = ViewParams.ge_invProj * vec4(ndc, depthForRay, 1.0);
    pV.xyz /= max(pV.w, 1e-6);

    float worldLimit = sky
        ? max(max(ViewParams.ge_nearFar.y, pc.atmosFloorHeight + pc.atmosLayerDepth) * 12.0, 1000.0)
        : length(pV.xyz);
    vec3 viewDirV = normalize(pV.xyz);
    vec3 viewDirWS = normalize(transpose(mat3(ViewParams.ge_view)) * viewDirV);
    vec3 originWS = ViewParams.ge_cameraPosWS.xyz;

    float bottom = pc.atmosFloorHeight;
    float top = bottom + max(pc.atmosLayerDepth, 1.0);
    float startT;
    float endT;
    if (!IntersectHeightSlab(originWS, viewDirWS, bottom, top, worldLimit, startT, endT))
    {
        oColor = scene;
        return;
    }

    float marchLength = endT - startT;
    if (marchLength <= 1.0)
    {
        oColor = scene;
        return;
    }

    const int kMaxSteps = 20;
    int stepCount = int(clamp(floor(mix(8.0, 18.0, Saturate(pc.atmosPixelScale)) + 0.5), 6.0, float(kMaxSteps)));
    float stepLength = marchLength / float(stepCount);
    float jitter = ScreenHash(gl_FragCoord.xy + vec2(pc.atmosDriftRate * 31.0, pc.atmosDriftAngle));

    vec3 cloudColor = max(vec3(pc.atmosCloudColorR, pc.atmosCloudColorG, pc.atmosCloudColorB), vec3(0.0));
    vec3 lightDir = normalize(vec3(0.35, 0.62, 0.71));
    float facingLight = Saturate(dot(viewDirWS, lightDir) * 0.5 + 0.5);
    float lightWrap = pow(facingLight, mix(2.0, 6.0, Saturate(pc.atmosSunFade)));
    // Opacity scales optical depth without changing procedural coverage. This
    // lets artists suppress colored background transmission while preserving
    // the authored cloud shape and scattering color.
    float extinctionScale = mix(0.00042, 0.00135, mass) *
                            clamp(pc.atmosOpacity, 0.0, 8.0);
    float transmittance = 1.0;
    vec3 scattering = vec3(0.0);

    for (int i = 0; i < kMaxSteps; ++i)
    {
        if (i >= stepCount)
            break;

        float t = startT + (float(i) + jitter) * stepLength;
        vec3 samplePos = originWS + viewDirWS * t;
        float density = CloudDensity(samplePos, drift);
        if (density <= 1e-4)
            continue;

        float h = Saturate((samplePos.y - bottom) / max(pc.atmosLayerDepth, 1.0));
        float rim = pow(1.0 - Saturate(density), 2.0) * Saturate(pc.atmosRimBoost);
        float selfShade = mix(1.0, 1.0 - density * 0.68, Saturate(pc.atmosOcclusion));
        float cloudLighting = mix(0.58, 1.0, Saturate(lightWrap + rim * 0.55 + h * 0.22));
        float sceneLuminance = dot(max(scene.rgb, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));
        cloudLighting = mix(cloudLighting, sceneLuminance + 0.10,
                            Saturate(pc.atmosSkyBounce) * 0.30);
        vec3 cloudLight = cloudColor * max(cloudLighting, 0.0) * selfShade;

        float opticalDepth = density * stepLength * extinctionScale;
        float stepTransmittance = exp(-opticalDepth);
        float stepAlpha = 1.0 - stepTransmittance;
        scattering += transmittance * stepAlpha * cloudLight;
        transmittance *= stepTransmittance;

        if (transmittance <= 0.025)
            break;
    }

    // Beer-Lambert integration already carries transmittance. Mixing the
    // result by alpha a second time suppresses the authored scattering color
    // and lets the background hue dominate thin and medium-density clouds.
    vec3 rgb = scene.rgb * transmittance + scattering;
    oColor = vec4(max(rgb, vec3(0.0)), scene.a);
}
