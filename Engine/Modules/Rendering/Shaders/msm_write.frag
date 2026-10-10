#version 450

// MSM4 (Peters & Klein 2015) moments-write fragment shader.
//
// Runs as a fullscreen quad after the cascade depth pass: reads the
// cascade's depth array layer (point-sampled to keep moment math correct
// — bilinear-averaged depth produces fictional moment vectors via
// Jensen's inequality) and writes the four raw depth moments through
// the optimized Hamburger quantization matrix into an RGBA16_UNORM
// target. The moments are then pre-blurred separably (msm_blur_h/v) and
// sampled at lighting time via the analytic Hamburger 4MSM bound.
//
// Why fullscreen-quad rather than re-rasterizing the casters with an
// FS attached: re-rasterization doubles the vertex / draw-call cost
// per frame for the cascade; reading the depth produces identical
// moments-per-texel because the moments are pure functions of the
// already-rasterised depth value at that texel. The Gaussian blur
// after this pass provides the receiver-blocker smoothing that MSM's
// analytic bound expects.
//
// References:
//   Peters & Klein, "Moment Shadow Mapping", I3D 2015, supplementary
//   listing 1 (encode matrix) and 4.2 (channel-0 +0.035955884801 bias).
//
// Reverse-Z: the engine stores reverse-Z depth (NDC.z = 1 near, 0 far).
// We un-reverse to forward-Z internal convention here so the read-side
// (GE_SampleCascadeMsm) can do the same un-reverse on its receiver
// depth — Hamburger's CDF semantics work in forward-Z.

layout(location = 0) out vec4 oMoments;

// Cascade depth array (sampler2DArrayShadow can't be used for raw reads;
// we need a linear/point sampler over the depth aspect).
layout(set = 0, binding = 0) uniform sampler2DArray uDepth;

layout(push_constant) uniform PC
{
    int cascadeIdx;       // which depth-array layer to sample
    int momentsResolution; // moments target resolution (for ivec coord scaling)
    int depthResolution;   // source depth resolution (used to scale UV→texel)
} pc;

// HLSL row-major matrix from Peters & Klein supplementary listing 1,
// transposed for GLSL column-major: HLSL `mul(raw, kEncode_HLSL)` becomes
// GLSL `kMomentEncode * raw` where kMomentEncode = transpose(kEncode_HLSL).
// In GLSL's column-stored layout, each GLSL column equals one HLSL row.
//
// Verification: at raw=(0.5, 0.25, 0.125, 0.0625) (z=0.5), the encoded
// output is approximately (0.92, 0.07, 0.33, 0.95) — within the [0,1]
// simplex required for RGBA16_UNORM storage. Pairing with kMomentDecode
// in shadow_sampling.glsl round-trips back to the original moments.
const mat4 kMomentEncode = mat4(
    vec4(-2.07224649,    13.7948857237,   0.105877704,    9.7924062118),
    vec4(32.23703778,   -59.4683975703,  -1.9077466311,  -33.7652110555),
    vec4(-68.571074599,  82.0359750338,   9.3496555107,   47.9456096605),
    vec4(39.3703274134, -35.364903257,   -6.6543490743,  -23.9728048165));

// Scalar bias on channel 0 ONLY (not a vec4 offset). From listing 1;
// centers the .x output near 0.5 inside the simplex to avoid clipping
// on RGBA16_UNORM at the storage step.
const float kMomentBiasChannel0 = 0.035955884801;

void main()
{
    // Sample the depth array at the cascade layer. Point-sample (texelFetch)
    // to preserve per-texel depth values — a bilinear sample would average
    // depths and produce fictional moments. When momentsResolution differs
    // from depthResolution, this produces nearest-neighbor downsampling /
    // upsampling, with the post-blur smoothing the staircase.
    ivec3 texSize = textureSize(uDepth, 0);
    ivec2 momentsXY = ivec2(gl_FragCoord.xy);
    // Map moments-fragment to depth-texel via the explicit res ratio.
    ivec2 depthXY = ivec2((vec2(momentsXY) + 0.5) * vec2(pc.depthResolution) / max(vec2(pc.momentsResolution), vec2(1.0)));
    depthXY = clamp(depthXY, ivec2(0), texSize.xy - ivec2(1));
    float storedZ = texelFetch(uDepth, ivec3(depthXY, pc.cascadeIdx), 0).r;

    // Un-reverse to forward-Z internal convention. Clamp avoids monomial-
    // vector collinearity at z=0 or z=1 which destabilises the analytic
    // bound's Cholesky factorization at sample time.
    float z = clamp(1.0 - storedZ, 0.0001, 0.9999);
    vec4 raw = vec4(z, z * z, z * z * z, z * z * z * z);
    vec4 quantized = kMomentEncode * raw;
    quantized.x += kMomentBiasChannel0;
    oMoments = quantized;
}
