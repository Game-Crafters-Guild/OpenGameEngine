#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSceneColor;

const float PI = 3.14159265358979323846;

layout(push_constant) uniform VhsSignalGlitchesPC {
    float vhsSignalGlitches;
    float vhsGlitchOffsets;
    float vhsSpeed;
    float shaderAnimationTime;
} pc;

float Hash(vec2 p)
{
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453123);
}

float SmoothNoise(float x)
{
    float i = floor(x);
    float f = fract(x);
    float blend = f * f * (3.0 - 2.0 * f);
    return mix(Hash(vec2(i, 17.0)), Hash(vec2(i + 1.0, 17.0)), blend);
}

void main()
{
    float strength = clamp(pc.vhsSignalGlitches, 0.0, 2.0);
    float offsetStrength = clamp(pc.vhsGlitchOffsets, 0.0, 2.0);
    vec2 imageSizePx = vec2(textureSize(uSceneColor, 0));
    vec2 pixelSize = 1.0 / imageSizePx;
    float time = pc.shaderAnimationTime * max(pc.vhsSpeed, 0.0);
    float tick = floor(time * 11.0);

    // Each event is a narrow, softly bounded group of neighboring tape lines.
    // The sampled image slips sideways locally while the rest of the frame
    // remains locked, avoiding digital blocks and full-screen vertical rolls.
    float row = floor(vUV.y * imageSizePx.y / 3.0);
    float eventSeed = Hash(vec2(row, tick));
    float eventGate = step(
        1.0 - min(0.022 * sqrt(strength), 0.055), eventSeed);
    float eventCenter = (row * 3.0 + 1.5) * pixelSize.y;
    float halfHeight = mix(
        pixelSize.y * 1.5, pixelSize.y * 5.0,
        Hash(vec2(row + 29.0, tick)));
    float band = 1.0 - smoothstep(
        halfHeight * 0.38, halfHeight,
        abs(vUV.y - eventCenter));
    float edgeNoise = SmoothNoise(
        vUV.y * imageSizePx.y * 0.31 + tick * 0.73) - 0.5;
    float direction = Hash(vec2(row + 47.0, tick)) < 0.5 ? -1.0 : 1.0;
    float slipPixels = mix(
        2.0, 13.0, Hash(vec2(row + 61.0, tick)));
    float slip = direction * slipPixels * pixelSize.x
               * band * eventGate * clamp(strength, 0.0, 1.6);
    slip += edgeNoise * pixelSize.x * 2.0 * band * eventGate * strength;

    // Broader stretch faults occupy independent horizontal slices. Each slice
    // fades out before choosing a new location, direction and width, while its
    // center oscillates vertically instead of joining a full-frame roll.
    vec2 offsetUv = vec2(0.0);
    float offsetMask = 0.0;
    for (int bandIndex = 0; bandIndex < 3; ++bandIndex)
    {
        float bandId = float(bandIndex);
        float eventRate = mix(
            0.34, 0.72, Hash(vec2(bandId + 3.0, 91.0)));
        float eventClock = time * eventRate
                         + Hash(vec2(bandId + 17.0, 33.0)) * 7.0;
        float eventIndex = floor(eventClock);
        float eventPhase = fract(eventClock);
        float eventSeed = Hash(vec2(eventIndex, bandId + 101.0));
        float eventGate = step(
            0.68 - min(offsetStrength * 0.18, 0.30), eventSeed);
        float lifetime = smoothstep(0.0, 0.10, eventPhase)
                       * (1.0 - smoothstep(0.72, 1.0, eventPhase))
                       * eventGate;

        float baseCenter = mix(
            0.12, 0.88,
            Hash(vec2(eventIndex + 43.0, bandId + 7.0)));
        float verticalDirection =
            Hash(vec2(eventIndex + 59.0, bandId + 19.0)) < 0.5
                ? -1.0 : 1.0;
        float verticalTravel = mix(
            0.025, 0.12,
            Hash(vec2(eventIndex + 71.0, bandId + 29.0)));
        float verticalMotion = sin(
            eventPhase * PI * 2.0
            + Hash(vec2(eventIndex + 83.0, bandId)) * PI);
        float center = clamp(
            baseCenter
            + verticalDirection * verticalMotion * verticalTravel,
            0.06, 0.94);
        float halfHeightPx = mix(
            4.0, 24.0,
            Hash(vec2(eventIndex + 97.0, bandId + 41.0)));
        float halfHeight = halfHeightPx * pixelSize.y;
        float sliceMask = 1.0 - smoothstep(
            halfHeight * 0.34, halfHeight,
            abs(vUV.y - center));
        sliceMask *= lifetime;

        float horizontalDirection =
            Hash(vec2(eventIndex + 109.0, bandId + 53.0)) < 0.5
                ? -1.0 : 1.0;
        float offsetPixels = mix(
            9.0, 72.0,
            Hash(vec2(eventIndex + 127.0, bandId + 67.0)));
        float stretchNoise = SmoothNoise(
            vUV.x * mix(3.0, 8.0,
                        Hash(vec2(eventIndex + 131.0, bandId)))
            + eventIndex * 4.3 + bandId * 13.0) - 0.5;
        float horizontalOffset =
            horizontalDirection
            * (offsetPixels + stretchNoise * offsetPixels * 0.72)
            * pixelSize.x * offsetStrength;
        float verticalOffset =
            verticalDirection
            * mix(1.0, 7.0,
                  Hash(vec2(eventIndex + 149.0, bandId + 79.0)))
            * pixelSize.y * offsetStrength;

        offsetUv += vec2(horizontalOffset, verticalOffset) * sliceMask;
        offsetMask = max(offsetMask, sliceMask);
    }

    vec2 sampleUv = clamp(
        vUV + vec2(slip, 0.0) + offsetUv,
        pixelSize * 0.5, vec2(1.0) - pixelSize * 0.5);
    vec4 shifted = texture(uSceneColor, sampleUv);

    // A faint displaced echo accompanies stronger line slips, as the head
    // briefly reads an adjacent portion of the same track.
    vec4 echo = texture(
        uSceneColor,
        clamp(sampleUv - vec2(direction * pixelSize.x * 2.0, 0.0),
              pixelSize * 0.5, vec2(1.0) - pixelSize * 0.5));
    float echoAmount =
        band * eventGate * clamp(strength * 0.16, 0.0, 0.28)
        + offsetMask * clamp(offsetStrength * 0.055, 0.0, 0.10);
    oColor = mix(shifted, echo, echoAmount);
}
