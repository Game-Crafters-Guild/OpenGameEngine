// The procedural fog shape of one particle quad, adapted from MirzaBeig/GPU-Fog-Particles (see
// Engine/Modules/GPUFogParticles/ThirdParty/MirzaBeig-GPU-Fog-Particles/UPSTREAM.md): layered
// value, simplex and Voronoi noise under a radial mask, with a camera-distance fade. The GPU fog
// surface and the particle surface both draw it.
//
// Packed parameters:
//   p0: simpleScale, simplexScale, voronoiScale, combinedRemap
//   p1: simpleAmount, simplexAmount, voronoiAmount, radialMaskPower
//   p2: simpleRemap, simplexRemap, voronoiRemap, edgeSoftness
//   p4: surfaceDepthFade, shapeDistortion, wispyAmount, detailAmount
//   p5: simpleNoiseAnimation.xyzw
//   p6: simplexNoiseAnimation.xyzw
//   p7: voronoiNoiseAnimation.xyzw
//   p8: cameraDepthFadeRange, cameraDepthFadeOffset

#ifndef GE_GPU_FOG_SHAPE_GLSL
#define GE_GPU_FOG_SHAPE_GLSL

// Scene depth bound by RenderServices as the resolved R32F View.DepthResolved.
// Always single-sample regardless of MSAA mode, so a plain sampler2D reads it.
layout(set = 0, binding = 17) uniform sampler2D ge_sceneDepth;

float GPUFogRandom(vec2 uv)
{
    return fract(sin(dot(uv, vec2(12.9898, 78.233))) * 43758.5453);
}

float GPUFogValueNoise(vec2 uv)
{
    vec2 i = floor(uv);
    vec2 f = fract(uv);
    f = f * f * (3.0 - 2.0 * f);

    float r0 = GPUFogRandom(i + vec2(0.0, 0.0));
    float r1 = GPUFogRandom(i + vec2(1.0, 0.0));
    float r2 = GPUFogRandom(i + vec2(0.0, 1.0));
    float r3 = GPUFogRandom(i + vec2(1.0, 1.0));

    return mix(mix(r0, r1, f.x), mix(r2, r3, f.x), f.y);
}

float GPUFogSimpleNoise(vec2 uv)
{
    float total = 0.0;
    total += GPUFogValueNoise(uv) * 0.125;
    total += GPUFogValueNoise(uv * 0.5) * 0.25;
    total += GPUFogValueNoise(uv * 0.25) * 0.5;
    return total;
}

vec3 GPUFogMod289(vec3 x)
{
    return x - floor(x / 289.0) * 289.0;
}

vec4 GPUFogMod289(vec4 x)
{
    return x - floor(x / 289.0) * 289.0;
}

vec4 GPUFogPermute(vec4 x)
{
    return GPUFogMod289((x * 34.0 + 1.0) * x);
}

vec4 GPUFogTaylorInvSqrt(vec4 r)
{
    return 1.79284291400159 - r * 0.85373472095314;
}

float GPUFogSimplexNoise(vec3 v)
{
    const vec2 c = vec2(1.0 / 6.0, 1.0 / 3.0);
    vec3 i = floor(v + dot(v, c.yyy));
    vec3 x0 = v - i + dot(i, c.xxx);
    vec3 g = step(x0.yzx, x0.xyz);
    vec3 l = 1.0 - g;
    vec3 i1 = min(g.xyz, l.zxy);
    vec3 i2 = max(g.xyz, l.zxy);
    vec3 x1 = x0 - i1 + c.xxx;
    vec3 x2 = x0 - i2 + c.yyy;
    vec3 x3 = x0 - 0.5;
    i = GPUFogMod289(i);
    vec4 p = GPUFogPermute(GPUFogPermute(GPUFogPermute(
        i.z + vec4(0.0, i1.z, i2.z, 1.0)) +
        i.y + vec4(0.0, i1.y, i2.y, 1.0)) +
        i.x + vec4(0.0, i1.x, i2.x, 1.0));
    vec4 j = p - 49.0 * floor(p / 49.0);
    vec4 x_ = floor(j / 7.0);
    vec4 y_ = floor(j - 7.0 * x_);
    vec4 x = (x_ * 2.0 + 0.5) / 7.0 - 1.0;
    vec4 y = (y_ * 2.0 + 0.5) / 7.0 - 1.0;
    vec4 h = 1.0 - abs(x) - abs(y);
    vec4 b0 = vec4(x.xy, y.xy);
    vec4 b1 = vec4(x.zw, y.zw);
    vec4 s0 = floor(b0) * 2.0 + 1.0;
    vec4 s1 = floor(b1) * 2.0 + 1.0;
    vec4 sh = -step(h, vec4(0.0));
    vec4 a0 = b0.xzyw + s0.xzyw * sh.xxyy;
    vec4 a1 = b1.xzyw + s1.xzyw * sh.zzww;
    vec3 g0 = vec3(a0.xy, h.x);
    vec3 g1 = vec3(a0.zw, h.y);
    vec3 g2 = vec3(a1.xy, h.z);
    vec3 g3 = vec3(a1.zw, h.w);
    vec4 norm = GPUFogTaylorInvSqrt(vec4(dot(g0, g0), dot(g1, g1), dot(g2, g2), dot(g3, g3)));
    g0 *= norm.x;
    g1 *= norm.y;
    g2 *= norm.z;
    g3 *= norm.w;
    vec4 m = max(0.6 - vec4(dot(x0, x0), dot(x1, x1), dot(x2, x2), dot(x3, x3)), 0.0);
    m = m * m;
    m = m * m;
    vec4 px = vec4(dot(x0, g0), dot(x1, g1), dot(x2, g2), dot(x3, g3));
    return 42.0 * dot(m, px);
}

vec2 GPUFogVoronoiHash(vec2 p)
{
    p = vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)));
    return fract(sin(p) * 43758.5453);
}

float GPUFogVoronoi(vec2 v, float time)
{
    vec2 n = floor(v);
    vec2 f = fract(v);
    float f1 = 8.0;
    float f2 = 8.0;

    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 g = vec2(float(x), float(y));
            vec2 o = GPUFogVoronoiHash(n + g);
            o = sin(time + o * 6.2831) * 0.5 + 0.5;
            vec2 r = f - g - o;
            float d = 0.5 * dot(r, r);
            if (d < f1)
            {
                f2 = f1;
                f1 = d;
            }
            else if (d < f2)
            {
                f2 = d;
            }
        }
    }

    return (f1 + f2) * 0.5;
}

float GPUFogRemap01(float value, float low)
{
    return clamp((value - low) / max(1.0 - low, 1e-4), 0.0, 1.0);
}

float GPUFogSceneEyeDepth(float rawDepth, vec2 screenUV)
{
    vec2 ndc = screenUV * 2.0 - 1.0;
    vec4 view = ge_invProj * vec4(ndc, rawDepth, 1.0);
    view.xyz /= max(abs(view.w), 1e-5);
    return abs(view.z);
}

// The radial mask at `uv` (0 to 1 across the quad): the particle's silhouette, 0 outside it.
// `particleStableRandom` decorrelates particles; `timeSeconds` animates the layers. Warps `uv` in
// place and returns the warped quad coordinates in `centeredUv` (-1 to 1): the density layers
// read both, so a caller evaluates the silhouette first and may skip the density where it is 0.
float GPUFogShapeSilhouette(inout vec2 uv, out vec2 centeredUv, float timeSeconds, float particleStableRandom,
                            vec4 p0, vec4 p1, vec4 p2, vec4 p4, vec4 p5, vec4 p6)
{
    float shapeDistortion = clamp(p4.y, 0.0, 1.5);

    centeredUv = uv * 2.0 - 1.0;
    if (shapeDistortion > 1e-4)
    {
        vec3 warpSeed = vec3(
            centeredUv * max(p0.y * 0.75, 0.001) + particleStableRandom * 8.0,
            timeSeconds * (p6.z * 0.35 + 0.025));
        vec2 warp = vec2(
            GPUFogSimplexNoise(warpSeed),
            GPUFogSimplexNoise(warpSeed + vec3(17.13, 29.71, 11.37)));
        centeredUv += warp * shapeDistortion * 0.28;
        uv = centeredUv * 0.5 + 0.5;
    }

    float radial = clamp(1.0 - length(centeredUv), 0.0, 1.0);
    if (shapeDistortion > 1e-4)
    {
        float edgeErosion = GPUFogSimpleNoise(
            (centeredUv + particleStableRandom * 7.0 + timeSeconds * p5.xy * 0.35) * max(p0.x * 0.45, 0.001));
        radial *= mix(1.0, smoothstep(0.05, 0.92, edgeErosion), shapeDistortion * 0.35);
    }
    radial = pow(radial, max(p1.w, 0.001));

    float edgeSoftness = clamp(p2.w, 0.0, 1.0);
    if (edgeSoftness > 0.0)
    {
        radial = smoothstep(0.0, edgeSoftness, radial);
    }
    return radial;
}

// The noise density, 0 to 1, at the `uv` and `centeredUv` GPUFogShapeSilhouette warped.
float GPUFogShapeDensity(vec2 uv, vec2 centeredUv, float timeSeconds, float particleStableRandom, vec4 p0, vec4 p1,
                         vec4 p2, vec4 p4, vec4 p5, vec4 p6, vec4 p7)
{
    float wispyAmount = clamp(p4.z, 0.0, 1.0);
    float detailAmount = clamp(p4.w, 0.0, 1.0);

    vec2 simpleUv = uv + p5.xy * timeSeconds + particleStableRandom * 10.0;
    float simple = GPUFogSimpleNoise(simpleUv * max(p0.x, 0.001));
    simple = GPUFogRemap01(simple, clamp(p2.x, 0.0, 0.999));
    float simpleLayer = mix(1.0, simple, clamp(p1.x, 0.0, 1.0));

    vec3 simplexCoord = vec3(uv, 0.0) + p6.xyz * timeSeconds + particleStableRandom * 20.0;
    float simplex = GPUFogSimplexNoise(simplexCoord * max(p0.y, 0.001)) * 0.5 + 0.5;
    simplex = GPUFogRemap01(simplex, clamp(p2.y, 0.0, 0.999));
    float simplexLayer = mix(1.0, simplex, clamp(p1.y, 0.0, 1.0));

    float voronoi = GPUFogVoronoi(uv * max(p0.z, 0.001) + p7.xy * timeSeconds, timeSeconds * p7.z);
    voronoi = GPUFogRemap01(voronoi, clamp(p2.z, 0.0, 0.999));
    float voronoiLayer = mix(1.0, voronoi, clamp(p1.z, 0.0, 1.0));

    float combined = simpleLayer * simplexLayer * voronoiLayer;
    if (wispyAmount > 1e-4)
    {
        vec2 windDir = normalize(p5.xy + vec2(0.37, 0.91));
        vec2 windSide = vec2(-windDir.y, windDir.x);
        vec2 wispUv = vec2(dot(centeredUv, windSide) * 0.85,
                           dot(centeredUv, windDir) * 3.75);
        wispUv += vec2(particleStableRandom * 12.0 + timeSeconds * p5.x,
                       particleStableRandom * 5.0 + timeSeconds * (abs(p6.z) + 0.035));
        float wisp = GPUFogSimplexNoise(vec3(wispUv * max(p0.y, 0.001), timeSeconds * 0.08)) * 0.5 + 0.5;
        wisp = smoothstep(0.25, 0.95, wisp);
        combined *= mix(1.0, wisp, wispyAmount);
    }
    if (detailAmount > 1e-4)
    {
        float detail = GPUFogSimpleNoise((uv + particleStableRandom * 19.0) * max(p0.x * 3.7, 0.001));
        float ridged = 1.0 - abs(detail * 2.0 - 1.0);
        ridged = smoothstep(0.1, 0.9, ridged);
        combined *= mix(1.0, ridged, detailAmount);
    }
    return GPUFogRemap01(combined, clamp(p0.w, 0.0, 0.999));
}

// The noise density and the radial mask at `uv` (0 to 1 across the quad).
void GPUFogEvaluateShape(vec2 uv, float timeSeconds, float particleStableRandom, vec4 p0, vec4 p1, vec4 p2,
                         vec4 p4, vec4 p5, vec4 p6, vec4 p7, out float density, out float radial)
{
    vec2 centeredUv;
    radial = GPUFogShapeSilhouette(uv, centeredUv, timeSeconds, particleStableRandom, p0, p1, p2, p4, p5, p6);
    density = GPUFogShapeDensity(uv, centeredUv, timeSeconds, particleStableRandom, p0, p1, p2, p4, p5, p6, p7);
}

// Fades particles in over p8.x metres past the near plane plus the p8.y offset.
float GPUFogCameraFade(float particleEyeDepth, vec4 p8)
{
    float cameraFade = 1.0;
    float cameraDepthFadeRange = max(p8.x, 0.0);
    if (cameraDepthFadeRange > 1e-4)
    {
        float cameraDepthFadeOffset = p8.y;
        float nearPlane = ge_nearFar.x;
        float cameraDepth = (particleEyeDepth - nearPlane - cameraDepthFadeOffset) / cameraDepthFadeRange;
        cameraFade = clamp(cameraDepth, 0.0, 1.0);
    }
    return cameraFade;
}

#endif // GE_GPU_FOG_SHAPE_GLSL
