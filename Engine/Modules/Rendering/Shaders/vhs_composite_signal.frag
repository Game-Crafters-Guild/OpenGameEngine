#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSceneColor;

layout(push_constant) uniform VhsCompositeSignalPC {
    float vhsCompositeSignalMode;
    float vhsDotCrawl;
    float shaderAnimationTime;
} pc;

const float PI = 3.14159265359;

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
    int mode = clamp(int(pc.vhsCompositeSignalMode + 0.5), 0, 3);
    if (mode == 0)
    {
        oColor = source;
        return;
    }

    vec2 sizePx = vec2(textureSize(uSceneColor, 0));
    vec2 pixel = 1.0 / sizePx;
    float line = floor(gl_FragCoord.y);
    float phaseStep = mode == 1 ? PI : (PI * 2.0 / 3.0);
    float phaseCycle = mode == 1 ? mod(line, 2.0) : mod(line, 3.0);
    float linePhase = phaseCycle * phaseStep;
    float oldSoft = mode == 3 ? 1.0 : 0.0;

    vec3 center = RgbToYiq(source.rgb);
    vec3 left1 = RgbToYiq(texture(
        uSceneColor, clamp(vUV - vec2(pixel.x * 2.0, 0.0),
                           vec2(0.0), vec2(1.0))).rgb);
    vec3 right1 = RgbToYiq(texture(
        uSceneColor, clamp(vUV + vec2(pixel.x * 2.0, 0.0),
                           vec2(0.0), vec2(1.0))).rgb);
    vec3 left2 = RgbToYiq(texture(
        uSceneColor, clamp(vUV - vec2(pixel.x * 5.0, 0.0),
                           vec2(0.0), vec2(1.0))).rgb);
    vec3 right2 = RgbToYiq(texture(
        uSceneColor, clamp(vUV + vec2(pixel.x * 5.0, 0.0),
                           vec2(0.0), vec2(1.0))).rgb);

    float softenedY = center.x * 0.56
                    + (left1.x + right1.x) * 0.18
                    + (left2.x + right2.x) * 0.04;
    vec2 delayedChroma = center.yz * 0.25
                       + right1.yz * 0.34
                       + right2.yz * 0.25
                       + left1.yz * 0.11
                       + left2.yz * 0.05;
    mat2 phaseRotation = mat2(
        cos(linePhase), -sin(linePhase),
        sin(linePhase),  cos(linePhase));
    vec2 phasedChroma = phaseRotation * delayedChroma;
    // Keep the phase-cycle hue shift faint; the line cycle should appear as
    // edge rainbows, not three solid tinted rows.
    center.yz = mix(delayedChroma, phasedChroma, mode == 1 ? 0.045 : 0.065);
    center.x = mix(center.x, softenedY, oldSoft * 0.62);

    float highFrequencyY = center.x - (left1.x + right1.x) * 0.5;
    float chromaEdge = length(center.yz - (left1.yz + right1.yz) * 0.5);
    float edge = smoothstep(0.015, 0.16, abs(highFrequencyY) + chromaEdge);
    float carrier = sin(
        gl_FragCoord.x * PI + linePhase
        + floor(pc.shaderAnimationTime * 29.97) * PI);
    float crawl = carrier * edge * clamp(pc.vhsDotCrawl, 0.0, 2.0);
    center.y += crawl * highFrequencyY * 0.22;
    center.z -= crawl * highFrequencyY * 0.16;
    center.x += crawl * chromaEdge * 0.035;

    oColor = vec4(clamp(YiqToRgb(center), 0.0, 1.0), source.a);
}
