#version 450
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 oColor;
layout(set=0,binding=0) uniform sampler2D uSrc;

// Dual-filter pyramid upsample (Bjorge, SIGGRAPH 2015): a 3x3 tent over the
// coarser level built from eight bilinear taps (corner weight 1, edge weight
// 2, sum 12) — a smooth widening filter with no separable-axis asymmetry.
//
// Energy: the pass does NOT add the coarse level on top of the finer one
// (which would compound brightness with each level and pop). It writes the
// tent in .rgb and the upsample blend in .a, and the node runs it with Alpha
// blending, so the hardware computes finer = coarse*a + finer*(1-a) — a convex
// combination. The blend (BloomEffect.Scatter, default 0.5) shapes the PSF tail
// width: at 0.5 the finest/quarter/eighth levels compose to weights
// 0.5/0.25/0.25 (sum 1.0), matching the old fixed blend; higher widens the halo.
// Energy stays convex/conserving for any blend in [0,1] by construction.
layout(push_constant) uniform BloomUpPC {
    float bloomScatter;
} pc;

void main(){
    vec2 hp = 0.5 / vec2(textureSize(uSrc, 0));
    vec3 s = texture(uSrc, vUV + vec2(-hp.x * 2.0, 0.0)).rgb;
    s += texture(uSrc, vUV + vec2(-hp.x,  hp.y)).rgb * 2.0;
    s += texture(uSrc, vUV + vec2( 0.0,   hp.y * 2.0)).rgb;
    s += texture(uSrc, vUV + vec2( hp.x,  hp.y)).rgb * 2.0;
    s += texture(uSrc, vUV + vec2( hp.x * 2.0, 0.0)).rgb;
    s += texture(uSrc, vUV + vec2( hp.x, -hp.y)).rgb * 2.0;
    s += texture(uSrc, vUV + vec2( 0.0,  -hp.y * 2.0)).rgb;
    s += texture(uSrc, vUV + vec2(-hp.x, -hp.y)).rgb * 2.0;
    oColor = vec4(s * (1.0 / 12.0), clamp(pc.bloomScatter, 0.0, 1.0));
}
