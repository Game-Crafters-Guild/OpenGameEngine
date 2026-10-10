#version 450
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 oColor;
layout(set=0,binding=0) uniform sampler2D uSrc;
layout(push_constant) uniform PC { float reserved; } pc;

// Normalized Gaussian, paired bilinear taps. The identical linear filter is
// used for highlight and threshold-free scattering pyramids.
void main()
{
    const float offsets[6] = float[6](-4.59777260, -2.70860853, -0.89068246,
                                      0.89068246,  2.70860853,  4.59777260);
    const float weights[6] = float[6](0.00327490, 0.08380325, 0.41292185,
                                     0.41292185, 0.08380325, 0.00327490);
    vec2 texel = 1.0 / vec2(textureSize(uSrc, 0));
    vec4 sum = vec4(0.0);
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 6; ++x)
            sum += texture(uSrc, vUV + vec2(offsets[x], offsets[y]) * texel)
                   * (weights[x] * weights[y]);
    oColor = sum;
}
