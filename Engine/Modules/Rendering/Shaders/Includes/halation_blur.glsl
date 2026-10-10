// Dense, normalized separable Gaussian. Radius is in final-image pixels,
// independent of bloom radius/octaves. No sparse ring taps at large radii.
vec3 HalationBlur(sampler2D source, vec2 uv, vec2 axis, float radius)
{
    float support = clamp(radius, 0.0, 64.0) * 0.5;
    float sigma = max(support / 3.0, 0.5);
    int taps = int(ceil(support));
    vec3 sum = texture(source, uv).rgb;
    float weight = 1.0;
    vec2 texel = axis / vec2(textureSize(source, 0));
    for (int i = 1; i <= taps; ++i)
    {
        float w = exp(-0.5 * float(i * i) / (sigma * sigma));
        // Fade the boundary tap to avoid radius-dependent steps.
        w *= clamp(support - float(i - 1), 0.0, 1.0);
        sum += (texture(source, uv + texel * float(i)).rgb
              + texture(source, uv - texel * float(i)).rgb) * w;
        weight += 2.0 * w;
    }
    return sum / weight;
}
