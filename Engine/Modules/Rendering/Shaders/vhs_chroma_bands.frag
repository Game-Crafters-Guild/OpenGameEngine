#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSceneColor;

layout(push_constant) uniform VhsChromaBandsPC {
    float vhsPreFilterChromaStreaks;
    float vhsSpeed;
    float shaderAnimationTime;
} pc;

float Hash(vec2 p)
{
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453123);
}

vec3 RgbToYiq(vec3 rgb)
{
    return vec3(
        dot(rgb, vec3(0.299, 0.587, 0.114)),
        dot(rgb, vec3(0.596, -0.274, -0.322)),
        dot(rgb, vec3(0.211, -0.523, 0.312)));
}

vec3 YiqToRgb(vec3 yiq)
{
    return vec3(
        yiq.x + yiq.y * 0.956 + yiq.z * 0.621,
        yiq.x - yiq.y * 0.272 - yiq.z * 0.647,
        yiq.x - yiq.y * 1.106 + yiq.z * 1.703);
}

void main()
{
    vec4 source = texture(uSceneColor, vUV);
    float strength = clamp(pc.vhsPreFilterChromaStreaks, 0.0, 2.0);
    if (strength <= 1e-5)
    {
        oColor = source;
        return;
    }

    vec2 sizePx = vec2(textureSize(uSceneColor, 0));
    vec2 pixel = 1.0 / sizePx;
    float time = pc.shaderAnimationTime * max(pc.vhsSpeed, 0.0);
    float field = floor(time * 29.97);
    float bandCell = floor(vUV.y * 54.0);
    float seed = Hash(vec2(bandCell, floor(field / 5.0)));
    float gate = step(1.0 - min(0.025 + strength * 0.04, 0.11), seed);
    float localY = fract(vUV.y * 54.0);
    float center = mix(0.22, 0.78, Hash(vec2(bandCell + 19.0, field)));
    float width = mix(0.07, 0.24, Hash(vec2(bandCell + 37.0, field)));
    float band = (1.0 - smoothstep(width * 0.35, width,
                                   abs(localY - center))) * gate;

    // Form the chroma fault from already softened neighboring samples. This
    // pass runs before the main VHS YIQ filter, so the resulting band receives
    // that blur as well instead of appearing as a crisp digital stripe.
    vec3 blur = texture(uSceneColor, vUV).rgb * 0.24;
    blur += texture(uSceneColor, vUV + vec2(pixel.x * 3.0, 0.0)).rgb * 0.19;
    blur += texture(uSceneColor, vUV - vec2(pixel.x * 3.0, 0.0)).rgb * 0.19;
    blur += texture(uSceneColor, vUV + vec2(pixel.x * 7.0, pixel.y * 1.5)).rgb * 0.12;
    blur += texture(uSceneColor, vUV - vec2(pixel.x * 7.0, pixel.y * 1.5)).rgb * 0.12;
    blur += texture(uSceneColor, vUV + vec2(0.0, pixel.y * 2.5)).rgb * 0.07;
    blur += texture(uSceneColor, vUV - vec2(0.0, pixel.y * 2.5)).rgb * 0.07;

    vec3 yiq = RgbToYiq(source.rgb);
    vec3 blurredYiq = RgbToYiq(blur);
    float phase = (Hash(vec2(bandCell + 73.0, field)) * 2.0 - 1.0)
                * 0.65 * strength;
    mat2 rotation = mat2(cos(phase), -sin(phase),
                         sin(phase),  cos(phase));
    vec2 damagedChroma = rotation * blurredYiq.yz;
    damagedChroma *= mix(0.45, 1.18, Hash(vec2(bandCell + 91.0, field)));
    yiq.yz = mix(yiq.yz, damagedChroma,
                 band * clamp(0.28 + strength * 0.34, 0.0, 0.88));
    oColor = vec4(clamp(YiqToRgb(yiq), 0.0, 1.0), source.a);
}
