#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSceneColor;
layout(set = 0, binding = 1) uniform sampler2D uFeedbackHistory;

layout(push_constant) uniform VhsFrameFeedbackPC {
    float vhsFrameFeedback;
    float vhsFeedbackDecay;
    float vhsFeedbackMotionThreshold;
    float vhsFeedbackTrailLength;
    float vhsFeedbackHistoryValid;
} pc;

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
    float strength = clamp(pc.vhsFrameFeedback, 0.0, 2.0);
    if (strength <= 1e-5 || pc.vhsFeedbackHistoryValid <= 0.5)
    {
        oColor = source;
        return;
    }

    vec2 pixel = 1.0 / vec2(textureSize(uSceneColor, 0));
    float trailPixels = clamp(pc.vhsFeedbackTrailLength, 0.0, 24.0);
    vec2 trail = vec2(pixel.x * trailPixels, 0.0);
    vec3 previousCenter = texture(uFeedbackHistory, vUV).rgb;
    vec3 previousA = texture(
        uFeedbackHistory, clamp(vUV - trail, vec2(0.0), vec2(1.0))).rgb;
    vec3 previousB = texture(
        uFeedbackHistory, clamp(vUV - trail * 0.45, vec2(0.0), vec2(1.0))).rgb;
    vec3 previous = previousCenter * 0.18
                  + previousA * 0.50
                  + previousB * 0.32;

    vec3 currentYiq = RgbToYiq(source.rgb);
    vec3 previousYiq = RgbToYiq(previous);
    float frameDifference = abs(currentYiq.x - RgbToYiq(previousCenter).x)
                          + length(currentYiq.yz
                                 - RgbToYiq(previousCenter).yz) * 0.30;
    float threshold = clamp(pc.vhsFeedbackMotionThreshold, 0.0, 1.0);
    float motion = smoothstep(
        threshold, threshold + 0.12, frameDifference);
    float feedback = clamp(
        strength * clamp(pc.vhsFeedbackDecay, 0.0, 0.98) * motion,
        0.0, 0.86);

    // Luma leaves a restrained bright echo while low-bandwidth chroma decays
    // more slowly. Static areas remain current-frame sharp.
    float echoLuma = max(currentYiq.x, previousYiq.x * 0.94);
    currentYiq.x = mix(currentYiq.x, echoLuma, feedback * 0.62);
    currentYiq.yz = mix(
        currentYiq.yz, previousYiq.yz,
        clamp(feedback * 1.18, 0.0, 0.92));
    oColor = vec4(clamp(YiqToRgb(currentYiq), 0.0, 1.0), source.a);
}
