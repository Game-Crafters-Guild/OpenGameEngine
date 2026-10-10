#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSceneColor;

layout(push_constant) uniform VhsInterferencePC {
    float vhsInterference;
    float vhsSpeed;
    float shaderAnimationTime;
} pc;

float Hash(vec2 p)
{
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453123);
}

void main()
{
    float strength = clamp(pc.vhsInterference, 0.0, 2.0);
    vec4 source = texture(uSceneColor, vUV);
    if (strength <= 1e-5)
    {
        oColor = source;
        return;
    }

    vec2 sizePx = vec2(textureSize(uSceneColor, 0));
    vec2 pixel = 1.0 / sizePx;
    float time = pc.shaderAnimationTime * max(pc.vhsSpeed, 0.0);
    float frame = floor(time * 29.97);

    float lineA = fract(0.13 + time * 0.083);
    float lineB = fract(0.71 - time * 0.047);
    float thinA = 1.0 - smoothstep(
        pixel.y * 0.8, pixel.y * 2.4, abs(vUV.y - lineA));
    float thinB = 1.0 - smoothstep(
        pixel.y * 1.2, pixel.y * 3.5, abs(vUV.y - lineB));
    float lineGateA = step(0.42, Hash(vec2(floor(frame / 3.0), 11.0)));
    float lineGateB = step(0.62, Hash(vec2(floor(frame / 5.0), 23.0)));
    float lineNoise = thinA * lineGateA + thinB * lineGateB * 0.65;

    // Slow mains-related gain ripple: broad and low amplitude, unlike a crisp
    // digital band.
    float humPhase = vUV.y * 6.2831853
                   - time * 0.34 * 6.2831853;
    float humBar = sin(humPhase) * 0.5 + 0.5;
    humBar = smoothstep(0.22, 0.78, humBar) - 0.5;

    float slip = (Hash(vec2(frame, floor(vUV.y * sizePx.y))) - 0.5)
               * pixel.x * 2.5 * lineNoise * strength;
    vec3 color = texture(
        uSceneColor,
        clamp(vUV + vec2(slip, 0.0), pixel * 0.5,
              vec2(1.0) - pixel * 0.5)).rgb;
    color *= 1.0 + humBar * 0.025 * strength;
    color += vec3(lineNoise * 0.035 * strength);
    oColor = vec4(clamp(color, 0.0, 1.0), source.a);
}
