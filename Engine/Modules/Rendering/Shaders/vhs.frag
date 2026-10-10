#version 450

// Adapted from punikonta's MIT-licensed Godot VHS shader:
// https://github.com/punikonta/godot-shader-crt-vhs
// Copyright (c) Marcel Jovic <punikonta@protonmail.com>
// See ThirdParty/PunikontaVhs/LICENSE.md.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSceneColor;
layout(set = 0, binding = 1) uniform sampler2D uFieldHistory;

layout(push_constant) uniform VhsPC {
    float vhsIntensity;
    float vhsWobble;
    float vhsTracking;
    float vhsColorBleed;
    float vhsColorBleedOffset;
    float vhsTapeNoise;
    float vhsChromaStreaks;
    float vhsDropouts;
    float vhsScanlines;
    float vhsSpeed;
    float vhsOverlayEnabled;
    float vhsOverlayColorR;
    float vhsOverlayColorG;
    float vhsOverlayColorB;
    float vhsOverlayOpacity;
    float vhsOverlaySize;
    float vhsOverlayFont;
    float vhsOverlayPositionX;
    float vhsOverlayPositionY;
    float vhsOverlayTextLength;
    float vhsOverlayText0;
    float vhsOverlayText1;
    float vhsOverlayText2;
    float vhsOverlayText3;
    float vhsOverlayText4;
    float vhsOverlayText5;
    float vhsOverlayText6;
    float vhsOverlayText7;
    float vhsTransportMode;
    float vhsTransportStrength;
    float vhsFieldHistoryValid;
    float shaderAnimationTime;
} pc;

const float PI = 3.14159265359;

float Hash(vec2 p)
{
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453123);
}

float SmoothNoise(float x)
{
    float i = floor(x);
    float f = fract(x);
    float u = f * f * (3.0 - 2.0 * f);
    return mix(Hash(vec2(i)), Hash(vec2(i + 1.0)), u);
}

float ValueNoise(vec2 p)
{
    vec2 cell = floor(p);
    vec2 fraction = fract(p);
    vec2 blend = fraction * fraction * (3.0 - 2.0 * fraction);
    float bottom = mix(Hash(cell), Hash(cell + vec2(1.0, 0.0)), blend.x);
    float top = mix(Hash(cell + vec2(0.0, 1.0)),
                    Hash(cell + vec2(1.0, 1.0)), blend.x);
    return mix(bottom, top, blend.y);
}

float WrappedDistance(float a, float b)
{
    float distance = abs(a - b);
    return min(distance, 1.0 - distance);
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

uvec2 DigitBits(int digit)
{
    if (digit == 0) return uvec2(2738546222u, 3u);
    if (digit == 1) return uvec2(2286031044u, 3u);
    if (digit == 2) return uvec2(3292807726u, 7u);
    if (digit == 3) return uvec2(3775349263u, 3u);
    if (digit == 4) return uvec2(301246856u, 2u);
    if (digit == 5) return uvec2(3775366207u, 3u);
    if (digit == 6) return uvec2(2736227374u, 3u);
    if (digit == 7) return uvec2(2216829471u, 0u);
    if (digit == 8) return uvec2(2736211502u, 3u);
    return uvec2(2702132782u, 3u);
}

uvec2 GlyphBits(int code)
{
    if (code >= 48 && code <= 57) return DigitBits(code - 48);
    if (code == 65) return uvec2(1663026734u, 4u);
    if (code == 66) return uvec2(3809986095u, 3u);
    if (code == 67) return uvec2(2182120510u, 7u);
    if (code == 68) return uvec2(3810051631u, 3u);
    if (code == 69) return uvec2(3256321087u, 7u);
    if (code == 70) return uvec2(1108837439u, 0u);
    if (code == 71) return uvec2(2736686142u, 7u);
    if (code == 72) return uvec2(1663026737u, 4u);
    if (code == 73) return uvec2(3359772831u, 7u);
    if (code == 74) return uvec2(2459181340u, 1u);
    if (code == 75) return uvec2(1381078321u, 4u);
    if (code == 76) return uvec2(3255862305u, 7u);
    if (code == 77) return uvec2(1662703473u, 4u);
    if (code == 78) return uvec2(1662834289u, 4u);
    if (code == 79) return uvec2(2736309806u, 3u);
    if (code == 80) return uvec2(1108854319u, 0u);
    if (code == 81) return uvec2(2472068654u, 5u);
    if (code == 82) return uvec2(1381484079u, 4u);
    if (code == 83) return uvec2(3775333438u, 3u);
    if (code == 84) return uvec2(138547359u, 1u);
    if (code == 85) return uvec2(2736309809u, 3u);
    if (code == 86) return uvec2(353945137u, 1u);
    if (code == 87) return uvec2(2874852913u, 2u);
    if (code == 88) return uvec2(1654794801u, 4u);
    if (code == 89) return uvec2(138553905u, 1u);
    if (code == 90) return uvec2(3257016863u, 7u);
    if (code == 58) return uvec2(138416256u, 0u);
    if (code == 45) return uvec2(1015808u, 0u);
    if (code == 46) return uvec2(402653184u, 3u);
    if (code == 47) return uvec2(1143087376u, 0u);
    return uvec2(0u);
}

int OverlayTextCode(int index)
{
    if (index == 0) return int(pc.vhsOverlayText0 + 0.5);
    if (index == 1) return int(pc.vhsOverlayText1 + 0.5);
    if (index == 2) return int(pc.vhsOverlayText2 + 0.5);
    if (index == 3) return int(pc.vhsOverlayText3 + 0.5);
    if (index == 4) return int(pc.vhsOverlayText4 + 0.5);
    if (index == 5) return int(pc.vhsOverlayText5 + 0.5);
    if (index == 6) return int(pc.vhsOverlayText6 + 0.5);
    return int(pc.vhsOverlayText7 + 0.5);
}

float GlyphMask(vec2 fragmentPx, vec2 originPx, float cellPx, uvec2 bits,
                float fontStyle)
{
    vec2 local = (fragmentPx - originPx) / cellPx;
    if (local.x < 0.0 || local.y < 0.0 || local.x >= 5.0 || local.y >= 7.0)
        return 0.0;

    ivec2 cell = ivec2(floor(local));
    int bitIndex = cell.y * 5 + cell.x;
    float enabled = bitIndex < 32
        ? float((bits.x >> uint(bitIndex)) & 1u)
        : float((bits.y >> uint(bitIndex - 32)) & 1u);
    vec2 within = abs(fract(local) - 0.5);
    float roundedCell = 1.0 - smoothstep(0.32, 0.5, length(within));
    float blockCell = 1.0 - smoothstep(0.45, 0.5, max(within.x, within.y));
    float lcdCell = 1.0 - smoothstep(0.22, 0.34, max(within.x, within.y));
    float cellMask = fontStyle < 0.5
        ? roundedCell
        : (fontStyle < 1.5
            ? blockCell
            : (fontStyle < 2.5 ? lcdCell : 1.0));
    return enabled * cellMask;
}

float DrawDigit(vec2 fragmentPx, vec2 originPx, float cellPx, int digit,
                float fontStyle)
{
    return GlyphMask(fragmentPx, originPx, cellPx, DigitBits(digit), fontStyle);
}

vec2 DrawVhsOverlay(vec2 fragmentPx, vec2 imageSizePx, float time,
                    float sizeScale, float fontStyle, vec2 position)
{
    float cellPx = clamp(floor(imageSizePx.y / 360.0), 2.0, 4.0)
                 * clamp(sizeScale, 0.5, 4.0);
    float advance = cellPx * 6.0;
    float mask = 0.0;
    float counterWidth = advance * 8.0 - cellPx;
    vec2 groupSize = vec2(counterWidth, cellPx * 15.0);
    vec2 margin = vec2(cellPx * 6.0);
    vec2 available = max(imageSizePx - groupSize - margin * 2.0, vec2(0.0));
    vec2 groupOrigin = margin + available * clamp(position, vec2(0.0), vec2(1.0));

    // User-defined status text at the upper-left. REC retains the familiar
    // blinking red indicator on its left; all other labels render without it.
    int textLength = clamp(int(pc.vhsOverlayTextLength + 0.5), 0, 8);
    bool isRecord = textLength == 3
                 && OverlayTextCode(0) == 82
                 && OverlayTextCode(1) == 69
                 && OverlayTextCode(2) == 67;
    vec2 statusOrigin = groupOrigin
                      + vec2(isRecord ? advance : 0.0, 0.0);
    for (int index = 0; index < 8; ++index)
    {
        if (index < textLength)
        {
            mask = max(mask, GlyphMask(
                fragmentPx, statusOrigin + vec2(advance * float(index), 0.0),
                cellPx, GlyphBits(OverlayTextCode(index)), fontStyle));
        }
    }
    float recordDotMask = 0.0;
    if (isRecord)
    {
        vec2 recordDot = groupOrigin + vec2(cellPx * 2.0, cellPx * 3.5);
        float dotMask = 1.0 - smoothstep(
            cellPx * 1.30, cellPx * 2.10,
            length(fragmentPx - recordDot));
        recordDotMask = dotMask * step(0.25, fract(time));
    }

    // Running HH:MM:SS counter below REC; the group can be placed anywhere
    // inside the safe screen area.
    int elapsed = int(floor(max(time, 0.0)));
    int hours = (elapsed / 3600) % 100;
    int minutes = (elapsed / 60) % 60;
    int seconds = elapsed % 60;
    vec2 counterOrigin = groupOrigin + vec2(0.0, cellPx * 8.0);
    const uvec2 colonBits = uvec2(138416256u, 0u);
    mask = max(mask, DrawDigit(fragmentPx, counterOrigin, cellPx, hours / 10, fontStyle));
    mask = max(mask, DrawDigit(fragmentPx, counterOrigin + vec2(advance, 0.0), cellPx, hours % 10, fontStyle));
    mask = max(mask, GlyphMask(fragmentPx, counterOrigin + vec2(advance * 2.0, 0.0), cellPx, colonBits, fontStyle));
    mask = max(mask, DrawDigit(fragmentPx, counterOrigin + vec2(advance * 3.0, 0.0), cellPx, minutes / 10, fontStyle));
    mask = max(mask, DrawDigit(fragmentPx, counterOrigin + vec2(advance * 4.0, 0.0), cellPx, minutes % 10, fontStyle));
    mask = max(mask, GlyphMask(fragmentPx, counterOrigin + vec2(advance * 5.0, 0.0), cellPx, colonBits, fontStyle));
    mask = max(mask, DrawDigit(fragmentPx, counterOrigin + vec2(advance * 6.0, 0.0), cellPx, seconds / 10, fontStyle));
    mask = max(mask, DrawDigit(fragmentPx, counterOrigin + vec2(advance * 7.0, 0.0), cellPx, seconds % 10, fontStyle));
    return vec2(mask, recordDotMask);
}

vec4 SampleRecordedSource(vec2 uv, vec2 imageSizePx, float time)
{
    // The prior date-burn pass is recorded into the source image. VCR-generated
    // transport OSD is composited later, after all tape-signal degradation.
    return texture(uSceneColor, uv);
}

void main()
{
    vec4 source = texture(uSceneColor, vUV);
    float intensity = clamp(pc.vhsIntensity, 0.0, 1.0);
    if (intensity <= 1e-5)
    {
        oColor = source;
        return;
    }
    vec2 textureSizePx = vec2(textureSize(uSceneColor, 0));
    vec2 pixelSize = 1.0 / textureSizePx;
    float time = pc.shaderAnimationTime * max(pc.vhsSpeed, 0.0);
    // Use video-rate sample-and-hold timing instead of continuously changing
    // random values. The extra field cadence only introduces a sub-pixel weave.
    float frameTime = floor(time * 29.97);
    float fieldTime = floor(time * 59.94);
    float fieldParity = mod(fieldTime, 2.0);
    float wobbleStrength = clamp(pc.vhsWobble, 0.0, 2.0);
    float trackingStrength = clamp(pc.vhsTracking, 0.0, 2.0);
    float bleedStrength = clamp(pc.vhsColorBleed, 0.0, 2.0);
    float noiseStrength = clamp(pc.vhsTapeNoise, 0.0, 2.0);
    float streakStrength = clamp(pc.vhsChromaStreaks, 0.0, 2.0);
    float dropoutStrength = clamp(pc.vhsDropouts, 0.0, 2.0);
    float scanlineStrength = clamp(pc.vhsScanlines, 0.0, 2.0);
    float transportMode = floor(pc.vhsTransportMode + 0.5);
    float transportActive = step(0.5, transportMode);
    float transportDirection = transportMode < 1.5 ? 1.0 : -1.0;
    float transportStrength = clamp(pc.vhsTransportStrength, 0.0, 2.0)
                            * transportActive;

    // Independent low- and high-frequency tape motion produces analog wow and flutter.
    float wow = SmoothNoise(vUV.y * 4.0 + time * 2.0) * 2.0 - 1.0;
    float flutter = SmoothNoise(vUV.y * 12.0 - time * 4.0) * 2.0 - 1.0;
    float horizontalWobble = (wow + flutter * 0.4) * 0.002 * wobbleStrength;

    // Tracking combines a slow time-base bend with a soft rolling correction
    // band and a much narrower line tear. The broad fault stays coherent across
    // neighboring rows; only the thin tear receives frame-held jitter.
    float trackingRow = floor(vUV.y * textureSizePx.y * 0.25);
    float trackingTick = floor(time * 12.0);
    float rowJitter = Hash(vec2(trackingRow, trackingTick)) * 2.0 - 1.0;
    float broadJitter = SmoothNoise(vUV.y * 7.0 - time * 1.3) * 2.0 - 1.0;

    float bandPositionA = fract(0.84 - time * 0.071);
    float bandPositionB = fract(0.31 + time * 0.043);
    float bandA = smoothstep(0.026, 0.0, WrappedDistance(vUV.y, bandPositionA));
    float bandB = smoothstep(0.0045, 0.0, WrappedDistance(vUV.y, bandPositionB));
    float bandActivityA = smoothstep(0.38, 0.72, SmoothNoise(time * 0.41 + 31.0));
    float bandActivityB = smoothstep(0.50, 0.80, SmoothNoise(time * 0.29 + 47.0));
    float transportBend = (SmoothNoise(vUV.y * 2.8 - time * 0.37) * 2.0 - 1.0)
                        * 0.0007 * trackingStrength;
    float bandShift = broadJitter * bandA * bandActivityA * 0.0015
                    + rowJitter * bandB * bandActivityB * 0.0030;
    bandShift *= trackingStrength;

    // Head switching belongs at the bottom of the displayed image in this
    // renderer's top-left UV convention. Its displacement changes in short,
    // frame-held steps while its boundary remains softly uneven.
    float headEdge = 0.987 + (SmoothNoise(vUV.x * 5.0 + trackingTick * 0.11) - 0.5) * 0.004;
    float headSwitchMask = smoothstep(headEdge, min(headEdge + 0.010, 0.999), vUV.y);
    float headNoise = SmoothNoise(vUV.y * 31.0 + floor(time * 9.0) * 0.37);
    float headSkew = Hash(vec2(floor(time * 2.5), 19.0)) - 0.5;
    float headShift = ((headNoise - 0.5) * 0.65 + headSkew * 0.35)
                    * 0.028 * headSwitchMask * trackingStrength;

    vec2 sampleUv = vUV + vec2(horizontalWobble + transportBend + bandShift + headShift, 0.0);
    sampleUv.y += (fieldParity - 0.5) * pixelSize.y * 0.18 * trackingStrength;
    sampleUv.y -= headSwitchMask * (0.001 + headNoise * 0.003) * trackingStrength;

    // Cue/review crosses several diagonal tape tracks per head sweep. The
    // recognizable picture therefore stays vertically locked while bands of
    // mistracking travel in the tape direction. Only the affected rows slip a
    // few lines; the entire frame must not continuously roll.
    float transportSweep = vUV.y * 8.5
                         - time * 2.15 * transportDirection;
    float transportEdgeNoise = SmoothNoise(
        vUV.y * 37.0 + time * 4.1 * transportDirection) - 0.5;
    float transportCarrier = sin(
        (transportSweep + transportEdgeNoise * 0.055) * PI * 2.0);
    float transportStripe = floor(transportSweep);
    float transportStripeGain = mix(
        0.58, 1.0,
        Hash(vec2(transportStripe, floor(time * 0.55))));
    float transportBandA = smoothstep(0.72, 0.96, transportCarrier)
                         * transportStripeGain;
    float transportBandB = smoothstep(
        0.012, 0.0,
        WrappedDistance(vUV.y, fract(0.41 + time * 0.27 * transportDirection)));
    float transportBand = clamp(
        transportBandA * 0.82 + transportBandB * 0.55, 0.0, 1.0)
        * transportStrength;
    float transportLocalSlip = transportDirection
                             * (transportBandA * 1.5 - transportBandB * 0.8)
                             * pixelSize.y * transportStrength;
    sampleUv.y += transportLocalSlip;
    sampleUv.x += transportDirection
                * (transportBandA * 0.006 - transportBandB * 0.004)
                * transportStrength;
    sampleUv = clamp(sampleUv, pixelSize * 0.5, vec2(1.0) - pixelSize * 0.5);

    // A narrow mechanical crease travels through the tape independently of the
    // bottom head-switch region. Its irregular edge displaces and attenuates
    // neighboring scan rows instead of behaving like a clean digital glitch.
    float creaseCenter = fract(0.18 + time * 0.11);
    float creaseDistance = abs(vUV.y - creaseCenter);
    float creaseEnvelope = smoothstep(0.011, 0.0, creaseDistance);
    float creaseEdge = Hash(vec2(floor(vUV.y * textureSizePx.y * 0.35),
                                 floor(time * 7.0)));
    float creaseActivity = smoothstep(0.42, 0.74, SmoothNoise(time * 0.23 + 8.0));
    float creaseAmount = creaseEnvelope * mix(0.35, 1.0, creaseEdge)
                       * creaseActivity * trackingStrength;
    sampleUv.x += (creaseEdge - 0.5) * pixelSize.x * 7.0 * creaseAmount;

    // Five taps emulate the low chroma bandwidth of an analog YIQ recording.
    // Strength controls symmetric softness and blend amount. Offset is an
    // independent pixel displacement of the blurred chroma centre, so it
    // creates a directional trail instead of acting like a second blur knob.
    float chromaBlurPixels = mix(
        0.75, 4.5, clamp(bleedStrength * 0.5, 0.0, 1.0));
    float chromaOffsetPixels = clamp(pc.vhsColorBleedOffset, 0.0, 16.0);
    vec2 bleedStep = vec2(pixelSize.x * chromaBlurPixels, 0.0);
    vec2 bleedOffset = vec2(pixelSize.x * chromaOffsetPixels, 0.0);
    vec3 center = SampleRecordedSource(
        sampleUv, textureSizePx, pc.shaderAnimationTime).rgb;
    vec3 positive1 = SampleRecordedSource(
        sampleUv + bleedOffset + bleedStep,
        textureSizePx, pc.shaderAnimationTime).rgb;
    vec3 negative1 = SampleRecordedSource(
        sampleUv + bleedOffset - bleedStep,
        textureSizePx, pc.shaderAnimationTime).rgb;
    vec3 positive2 = SampleRecordedSource(
        sampleUv + bleedOffset + bleedStep * 2.0,
        textureSizePx, pc.shaderAnimationTime).rgb;
    vec3 negative2 = SampleRecordedSource(
        sampleUv + bleedOffset - bleedStep * 2.0,
        textureSizePx, pc.shaderAnimationTime).rgb;
    vec3 verticalPositive = SampleRecordedSource(
        sampleUv + vec2(0.0, pixelSize.y * 1.5),
        textureSizePx, pc.shaderAnimationTime).rgb;
    vec3 verticalNegative = SampleRecordedSource(
        sampleUv - vec2(0.0, pixelSize.y * 1.5),
        textureSizePx, pc.shaderAnimationTime).rgb;

    vec3 yiqCenter = RgbToYiq(center);
    vec3 yiqPositive1 = RgbToYiq(positive1);
    vec3 yiqNegative1 = RgbToYiq(negative1);
    vec3 yiqPositive2 = RgbToYiq(positive2);
    vec3 yiqNegative2 = RgbToYiq(negative2);
    vec3 yiqVerticalPositive = RgbToYiq(verticalPositive);
    vec3 yiqVerticalNegative = RgbToYiq(verticalNegative);

    float blurredI = yiqCenter.y * 0.28
                   + (yiqPositive1.y + yiqNegative1.y) * 0.24
                   + (yiqPositive2.y + yiqNegative2.y) * 0.12;
    float blurredQ = yiqCenter.z * 0.20
                   + (yiqPositive1.z + yiqNegative1.z) * 0.22
                   + (yiqPositive2.z + yiqNegative2.z) * 0.18;
    float trailingI = yiqCenter.y * 0.18
                    + yiqPositive1.y * 0.42
                    + yiqPositive2.y * 0.28
                    + yiqNegative1.y * 0.08
                    + yiqNegative2.y * 0.04;
    float trailingQ = yiqCenter.z * 0.15
                    + yiqPositive1.z * 0.36
                    + yiqPositive2.z * 0.34
                    + yiqNegative1.z * 0.10
                    + yiqNegative2.z * 0.05;
    vec2 verticalChroma = (yiqVerticalPositive.yz + yiqVerticalNegative.yz) * 0.5;
    blurredI = mix(blurredI, verticalChroma.x, 0.18);
    blurredQ = mix(blurredQ, verticalChroma.y, 0.24);
    float delayedI = mix(blurredI, trailingI, 0.32);
    float delayedQ = mix(blurredQ, trailingQ, 0.38);

    float chromaFiltering = clamp(bleedStrength * 0.92, 0.0, 1.0);
    vec3 filteredYiq = vec3(
        yiqCenter.x,
        mix(yiqCenter.y, delayedI, chromaFiltering),
        mix(yiqCenter.z, delayedQ, chromaFiltering));

    // Colour-under phase recovery is never perfectly still. Apply a restrained
    // field-held hue and saturation wander across the whole picture, not only
    // inside explicit damage bands.
    float chromaPhase = (Hash(vec2(fieldTime, 73.0)) - 0.5)
                      * (0.055 * bleedStrength + 0.025 * noiseStrength);
    float chromaCos = cos(chromaPhase);
    float chromaSin = sin(chromaPhase);
    filteredYiq.yz = vec2(
        filteredYiq.y * chromaCos - filteredYiq.z * chromaSin,
        filteredYiq.y * chromaSin + filteredYiq.z * chromaCos);
    float chromaBreathing = 1.0
        + (Hash(vec2(fieldTime, 91.0)) - 0.5)
        * (0.045 * bleedStrength + 0.025 * noiseStrength);
    filteredYiq.yz *= chromaBreathing;
    vec3 color = YiqToRgb(filteredYiq);
    color *= 1.0 - clamp(creaseAmount * 0.065, 0.0, 0.14);

    // Head switching degrades the recorded picture rather than introducing a
    // synthetic pixel field. The lower edge loses chroma and gain while the
    // displaced source remains recognizable.
    vec3 headYiq = RgbToYiq(color);
    headYiq.yz *= 1.0 - headSwitchMask * 0.42 * trackingStrength;
    headYiq.x *= 1.0 - headSwitchMask * 0.075 * trackingStrength;
    color = YiqToRgb(headYiq);

    vec2 frameOffset = vec2(Hash(vec2(frameTime, 0.0)), Hash(vec2(frameTime, 1.0)));

    // Search bands retain recognizable luma while losing chroma lock and
    // gaining coarse RF grain, matching analog shuttle rather than a white
    // synthetic glitch stripe.
    vec3 transportYiq = RgbToYiq(color);
    vec2 transportNoiseCell = floor(
        vUV * textureSizePx * vec2(0.72, 1.0));
    float transportGrain = Hash(
        transportNoiseCell
        + vec2(frameTime * 17.0, frameTime * 5.0));
    float noisyTransportLuma = clamp(
        transportYiq.x + (transportGrain - 0.5) * 0.14 * transportStrength,
        0.0, 1.0);
    transportYiq.x = mix(
        transportYiq.x, noisyTransportLuma,
        clamp(transportBand * 0.78, 0.0, 0.9));
    transportYiq.yz *= 1.0 - clamp(transportBand * 0.55, 0.0, 0.82);
    color = YiqToRgb(transportYiq);

    // Narrow bands occasionally lose chroma lock. Work in YIQ so the luminance
    // structure remains stable instead of digitally shuffling RGB channels.
    float streakBand = floor(vUV.y / 0.018);
    float streakGate = step(1.0 - min(0.0125 * streakStrength, 0.2),
                            Hash(vec2(streakBand, frameOffset.x * 50.0)));
    vec3 streakYiq = RgbToYiq(color);
    float phaseError = (Hash(vec2(streakBand, frameTime)) * 2.0 - 1.0)
                     * 0.42 * streakStrength;
    float phaseCos = cos(phaseError);
    float phaseSin = sin(phaseError);
    vec2 rotatedChroma = vec2(
        streakYiq.y * phaseCos - streakYiq.z * phaseSin,
        streakYiq.y * phaseSin + streakYiq.z * phaseCos);
    streakYiq.yz = mix(streakYiq.yz, rotatedChroma,
                       streakGate * clamp(streakStrength * 0.55, 0.0, 0.85));
    color = YiqToRgb(streakYiq);

    // Missing tape RF triggers the deck's line concealment. Each event is held
    // for part of a video frame, occupies only a few scan rows, and covers an
    // irregular horizontal segment rather than a rectangular screen band.
    float dropoutTick = floor(time * 15.0);
    float dropoutBandHeight = 0.018;
    float dropoutBand = floor(vUV.y / dropoutBandHeight);
    float dropoutSeed = Hash(vec2(dropoutBand, dropoutTick));
    float dropoutGate = step(1.0 - min(0.018 * dropoutStrength, 0.20),
                             dropoutSeed);
    float dropoutRowPhase = fract(vUV.y / dropoutBandHeight);
    float dropoutCenter = mix(0.28, 0.72,
                              Hash(vec2(dropoutBand + 17.0, dropoutTick)));
    float dropoutHalfWidth = mix(0.035, 0.095,
                                 Hash(vec2(dropoutBand + 29.0, dropoutTick)));
    float dropoutEnvelope = 1.0 - smoothstep(
        dropoutHalfWidth * 0.42, dropoutHalfWidth,
        abs(dropoutRowPhase - dropoutCenter));
    float dropoutEcho = (1.0 - smoothstep(
        dropoutHalfWidth * 0.24, dropoutHalfWidth * 0.62,
        abs(dropoutRowPhase - dropoutCenter - dropoutHalfWidth * 1.8))) * 0.24;

    float dropoutStart = Hash(vec2(dropoutBand + 43.0, dropoutTick)) * 0.72;
    float dropoutLength = mix(0.08, 0.28,
                              Hash(vec2(dropoutBand + 61.0, dropoutTick)));
    float dropoutEnd = min(dropoutStart + dropoutLength, 0.985);
    float dropoutSegment = smoothstep(dropoutStart - 0.012,
                                      dropoutStart + 0.012, vUV.x)
                         * (1.0 - smoothstep(dropoutEnd - 0.018,
                                            dropoutEnd + 0.018, vUV.x));
    float dropoutFray = ValueNoise(vec2(
        vUV.x * 19.0 + dropoutTick * 0.73,
        dropoutBand * 0.37));
    dropoutSegment *= mix(0.45, 1.0, smoothstep(0.18, 0.76, dropoutFray));

    float dropoutAmount = dropoutGate * (dropoutEnvelope + dropoutEcho)
                        * dropoutSegment
                        * clamp(dropoutStrength * 0.62, 0.0, 0.88);
    // Consumer decks commonly concealed a missing line with a nearby good
    // line. Repeating the preceding scan row reads as tape concealment without
    // reintroducing the synthetic bright streaks avoided by this effect.
    vec2 concealedUv = clamp(
        sampleUv - vec2(0.0, pixelSize.y * 2.0),
        pixelSize * 0.5, vec2(1.0) - pixelSize * 0.5);
    vec3 concealed = SampleRecordedSource(
        concealedUv, textureSizePx, pc.shaderAnimationTime).rgb;
    vec3 dropoutYiq = RgbToYiq(color);
    vec3 concealedYiq = RgbToYiq(concealed);
    concealedYiq.yz *= 0.72;
    dropoutYiq = mix(dropoutYiq, concealedYiq, dropoutAmount * 0.86);
    dropoutYiq.x *= 1.0 - dropoutAmount * 0.08;
    color = YiqToRgb(dropoutYiq);

    // Tape noise has much less horizontal bandwidth than a digital pixel-noise
    // overlay. Correlated luma variation carries most of the noise while the
    // chroma component changes more slowly and remains subtle.
    vec2 lumaNoiseCoord = vec2(vUV.x * textureSizePx.x * 0.09 + frameOffset.x * 37.0,
                               vUV.y * textureSizePx.y * 0.62 + frameOffset.y * 53.0);
    float lumaNoise = ValueNoise(lumaNoiseCoord);
    float lineNoise = SmoothNoise(vUV.y * textureSizePx.y * 0.42 + frameTime * 1.31);
    vec2 chromaNoiseCoord = vec2(vUV.x * textureSizePx.x * 0.028 + frameOffset.x * 11.0,
                                 vUV.y * textureSizePx.y * 0.24 + frameOffset.y * 17.0);
    vec2 chromaNoise = vec2(
        ValueNoise(chromaNoiseCoord),
        ValueNoise(chromaNoiseCoord + vec2(23.7, 9.1)));
    float luminance = dot(color, vec3(0.299, 0.587, 0.114));
    float wearLine = SmoothNoise(vUV.y * 128.0 - time * 3.1);
    float linePresence = mix(0.08, 0.62, smoothstep(0.72, 0.91, wearLine));
    float noiseVolume = mix(0.038, 0.013, luminance) * noiseStrength * linePresence;
    vec3 noisyYiq = RgbToYiq(color);
    noisyYiq.x += (lumaNoise - 0.5) * noiseVolume;
    noisyYiq.x += (lineNoise - 0.5) * noiseVolume * 0.22;
    noisyYiq.yz += (chromaNoise - 0.5) * noiseVolume * 0.34;
    color = YiqToRgb(noisyYiq);

    // Slow luminance pumping comes from tape transport and gain instability;
    // it remains spatially broad so it reads as analog rather than pixel noise.
    float gainDrift = SmoothNoise(time * 0.48 + vUV.y * 1.35) - 0.5;
    float agcRecovery = SmoothNoise(time * 1.9 - vUV.y * 0.7) - 0.5;
    float powerFlutter = sin(time * 1.37 + vUV.y * 3.1) * 0.5;
    color *= 1.0 + (gainDrift + agcRecovery * 0.25 + powerFlutter * 0.18)
                   * 0.055 * noiseStrength;

    vec4 finalColor = vec4(
        mix(source.rgb, clamp(color, 0.0, 1.0), intensity), source.a);

    // Reconstruct alternating VHS fields from the previous rendered output.
    // Each field refreshes its own raster rows while the opposite rows retain
    // part of the preceding field, producing motion combing and field judder
    // without a generic whole-frame ghost.
    float rasterParity = mod(floor(gl_FragCoord.y), 2.0);
    float currentFieldLine = 1.0 - abs(rasterParity - fieldParity);
    float retainedFieldLine = 1.0 - currentFieldLine;
    if (pc.vhsFieldHistoryValid > 0.5)
    {
        vec3 previousField = texture(uFieldHistory, vUV).rgb;
        finalColor.rgb = mix(
            finalColor.rgb, previousField, retainedFieldLine * 0.72);
    }

    // REC, the counter, rewind, and fast-forward are generated by the playback
    // deck after decoding the tape signal. Keep the OSD stable above wobble,
    // bleed, dropouts, noise, and field reconstruction.
    if (pc.vhsOverlayEnabled > 0.5)
    {
        vec2 overlayMasks = DrawVhsOverlay(
            vUV * textureSizePx, textureSizePx, pc.shaderAnimationTime,
            pc.vhsOverlaySize, pc.vhsOverlayFont,
            vec2(pc.vhsOverlayPositionX, pc.vhsOverlayPositionY));
        vec3 overlayColor = clamp(
            vec3(pc.vhsOverlayColorR, pc.vhsOverlayColorG, pc.vhsOverlayColorB),
            0.0, 1.0);
        float overlayOpacity =
            clamp(pc.vhsOverlayOpacity, 0.0, 1.0) * intensity;
        finalColor.rgb = mix(
            finalColor.rgb, overlayColor, overlayMasks.x * overlayOpacity);
        finalColor.rgb = mix(
            finalColor.rgb, vec3(1.0, 0.055, 0.035),
            overlayMasks.y * overlayOpacity);
    }

    // VHS itself does not create a coarse pixel grid. This control represents
    // the final 480-line display response, above both tape and deck OSD.
    float scanlinePhase = vUV.y * 480.0 + fieldParity * 0.5 + lineNoise * 0.08;
    float scanline = sin(scanlinePhase * PI * 2.0) * 0.5 + 0.5;
    finalColor.rgb *= mix(
        1.0 - 0.042 * scanlineStrength * intensity, 1.0, scanline);

    oColor = finalColor;
}
