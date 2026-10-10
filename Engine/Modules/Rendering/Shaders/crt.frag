// Timothy Lottes — public-domain CRT / scanline pixel-art style (Vulkan port).
// Based on https://www.shadertoy.com/view/XsjSzR (split-screen demo removed; full framebuffer).
//
// Shader parameters are driven by PushConstants wired from PostProcessSettings:
// crtIntensity — mix with unprocessed sample (0 = bypass)
// crtCurvature — scales display warp toward Lottes defaults (warp vec2 ~ 1/32, 1/24)
// crtScanlines — blends hardScan between soft (-8) and sharper (-17.5 typical)
// crtVignette  — optional extra radial edge darkening after CRT combine
// crtAberration — red/blue UV split amount
// crtSoftness — blends horizontal phosphor taps from sharp (-4.25) to soft (-2.5)
// crtExposureCompensation — enables auto gain derived from crtScanlines to offset darkening
// crtEmulatedResolutionDiv — framebuffer / divisor gives emulated res (Lottes uses 6;
// Scene View 2D + pixel-perfect sets this to the pixel-perfect scale so scan rows align.)

#version 450

// Post passes sample single-mip render targets: lod-0 taps are exact and
// satisfy WGSL uniformity inside the effect branches.
#include "Includes/compat_profile.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSceneColor;

layout(push_constant) uniform CrtPC {
    float crtIntensity;
    float crtCurvature;
    float crtScanlines;
    float crtVignette;
    float crtAberration;
    float crtSoftness;
    float crtExposureCompensation;
    // Lottes: emulated framebuffer = texture_size / divisor (canonical 6).
    // Match pixel-perfect 2D scale so each logical artwork row spans CRT emulated texels cleanly.
    float crtEmulatedResolutionDiv;
} pc;

//------------------------------------------------------------------------

vec2 EmulatedResolution()
{
    vec2 full = vec2(textureSize(uSceneColor, 0));
    float div = pc.crtEmulatedResolutionDiv;
    if (!(div >= 1.0))
        div = 6.0;
    float span = max(min(full.x, full.y), 1.0);
    div = clamp(div, 1.0, span);
    return max(full / div, vec2(1.0));
}

float HardScanStrength()
{
    // Lottes: -8 softer, -16 more pronounced between lines (see exp2 Gaussian).
    float t = clamp(pc.crtScanlines, 0.0, 1.0);
    return mix(-8.0, -17.5, t);
}

float ScanlineExposureCompensation()
{
    // Approximate compensation for CRT scan darkening. Gain is driven only by
    // scanline intensity so users get an automatic "checkbox only" workflow.
    float enabled = clamp(pc.crtExposureCompensation, 0.0, 1.0);
    float scanT = clamp(pc.crtScanlines, 0.0, 1.0);
    float gain = mix(1.0, 1.35, scanT);
    return mix(1.0, gain, enabled);
}

float HardPixStrength()
{
    // Lottes: -4 hard, -2 soft. Dedicated softness control for horizontal tap shaping.
    float t = clamp(pc.crtSoftness, 0.0, 1.0);
    return mix(-4.25, -2.5, t);
}

vec2 WarpScale()
{
    // Lottes default vec2(1/32, 1/24). Authoring default crtCurvature 0.12 ≈ full warp; 0 = none.
    const vec2 kLottes = vec2(1.0 / 32.0, 1.0 / 24.0);
    float g = clamp(pc.crtCurvature / 0.12, 0.0, 8.0);
    return kLottes * g;
}

float MaskDarkAmt()
{
    return 0.5;
}
float MaskLightAmt()
{
    return 1.5;
}

//------------------------------------------------------------------------

// Nearest emulated sample given floating point position and texel offset.
// The Lottes original decoded sRGB here because it sampled an encoded
// framebuffer; this chain feeds the pass display-referred LINEAR
// (LDRBeforeCRT — the single terminal OETF runs in FinalSRGBEncode), so
// samples are already in the linear light the mask/scanline math wants.
vec3 Fetch(vec2 pos, vec2 off)
{
    vec2 res = EmulatedResolution();
    pos = floor(pos * res + off) / res;
    if (max(abs(pos.x - 0.5), abs(pos.y - 0.5)) > 0.5)
        return vec3(0.0);
#if defined(GE_COMPAT_PROFILE)
    // The -16 bias forces base mip on desktop; lod 0 is the same thing said
    // explicitly, and it is what WGSL uniformity allows here.
    return textureLod(uSceneColor, pos.xy, 0.0).rgb;
#else
    return texture(uSceneColor, pos.xy, -16.0).rgb;
#endif
}

vec2 Dist(vec2 pos)
{
    vec2 res = EmulatedResolution();
    pos *= res;
    return -((pos - floor(pos)) - vec2(0.5));
}

float Gaus(float pos, float scale)
{
    return exp2(scale * pos * pos);
}

vec3 Horz3(vec2 pos, float off)
{
    float hardPix = HardPixStrength();
    vec3 b = Fetch(pos, vec2(-1.0, off));
    vec3 c = Fetch(pos, vec2(0.0, off));
    vec3 d = Fetch(pos, vec2(1.0, off));
    float dst = Dist(pos).x;
    float wb = Gaus(dst - 1.0, hardPix);
    float wc = Gaus(dst + 0.0, hardPix);
    float wd = Gaus(dst + 1.0, hardPix);
    return (b * wb + c * wc + d * wd) / (wb + wc + wd);
}

vec3 Horz5(vec2 pos, float off)
{
    float hardPix = HardPixStrength();
    vec3 a = Fetch(pos, vec2(-2.0, off));
    vec3 b = Fetch(pos, vec2(-1.0, off));
    vec3 c = Fetch(pos, vec2(0.0, off));
    vec3 d = Fetch(pos, vec2(1.0, off));
    vec3 e = Fetch(pos, vec2(2.0, off));
    float dst = Dist(pos).x;
    float wa = Gaus(dst - 2.0, hardPix);
    float wb = Gaus(dst - 1.0, hardPix);
    float wc = Gaus(dst + 0.0, hardPix);
    float wd = Gaus(dst + 1.0, hardPix);
    float we = Gaus(dst + 2.0, hardPix);
    return (a * wa + b * wb + c * wc + d * wd + e * we) / (wa + wb + wc + wd + we);
}

float Scan(vec2 pos, float off)
{
    float hardScanVal = HardScanStrength();
    float dst = Dist(pos).y;
    return Gaus(dst + off, hardScanVal);
}

vec3 Tri(vec2 pos)
{
    vec3 a = Horz3(pos, -1.0);
    vec3 b = Horz5(pos, 0.0);
    vec3 c = Horz3(pos, 1.0);
    float wa = Scan(pos, -1.0);
    float wb = Scan(pos, 0.0);
    float wc = Scan(pos, 1.0);
    return a * wa + b * wb + c * wc;
}

vec2 WarpUv(vec2 pos)
{
    vec2 warp = WarpScale();
    pos = pos * 2.0 - 1.0;
    pos *= vec2(1.0 + (pos.y * pos.y) * warp.x, 1.0 + (pos.x * pos.x) * warp.y);
    return pos * 0.5 + 0.5;
}

vec3 ShadowMask(vec2 fragCoordPx)
{
    vec2 pos = fragCoordPx;
    pos.x += pos.y * 3.0;
    float maskDark = MaskDarkAmt();
    float maskLight = MaskLightAmt();
    vec3 mask = vec3(maskDark);
    pos.x = fract(pos.x / 6.0);
    if (pos.x < 0.333)
        mask.r = maskLight;
    else if (pos.x < 0.666)
        mask.g = maskLight;
    else
        mask.b = maskLight;
    return mask;
}

// Extra vignette — not in original CRT stack; gated by crtVignette.
float EdgeVignette(vec2 uv)
{
    uv = uv * 2.0 - 1.0;
    float r = clamp(dot(uv, uv), 0.0, 1.0);
    return mix(1.0, 1.0 - r, clamp(pc.crtVignette, 0.0, 3.0) * 0.35);
}

//------------------------------------------------------------------------

void main()
{
    float w = clamp(pc.crtIntensity, 0.0, 1.0);
    vec4 scene = GE_TAP_LOD0(uSceneColor, vUV);
    if (w <= 1e-5)
    {
        oColor = scene;
        return;
    }

    // Input and output are both display-referred linear; Tri/Mask accumulate
    // in that same linear light (see Fetch) and the terminal OETF happens
    // downstream in FinalSRGBEncode.
    vec3 bypassLin = scene.rgb;

    vec3 crtLin = Tri(WarpUv(vUV)) * ShadowMask(gl_FragCoord.xy);
    crtLin *= EdgeVignette(vUV);
    crtLin *= ScanlineExposureCompensation();

    vec3 blendedLin = mix(bypassLin, crtLin, w);
    oColor = vec4(blendedLin, scene.a);
}
