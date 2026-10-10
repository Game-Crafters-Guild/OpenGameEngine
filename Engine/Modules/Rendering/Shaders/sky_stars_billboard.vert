#version 450

struct StarInstance {
    vec4 dirAndBrightness;
    vec4 properties;
};

layout(std430, set = 0, binding = 0) readonly buffer StarBuf {
    StarInstance stars[];
} sb;

layout(set = 0, binding = 1) uniform SkyUBO
{
    vec3  cameraPositionWS;   float exposureEV;
    vec3  cameraRightWS;      float pad0;
    vec3  cameraUpWS;         float pad1;
    vec3  cameraForwardWS;    float pad2;
    vec3  sunDirectionWS;     float sunAngularRadius;
    vec3  moonDirectionWS;    float moonIntensity;
    float moonAngularRadius;
    float skyRotateAroundZenithRadians;
    float moonUboPad1;
    float moonUboPad2;
    vec3  sunColor;           float sunIntensity;
    float timeOfDayHours;     float skyPad0;
    float viewportAspect;     float tanHalfFovY;
    vec2  viewportResolution; float nightSkyBlend; float skyTimeSeconds;
    vec4  starSizeShape;
    vec4  starTwinkleParams;
    vec4  starTwinkleAmp;
    vec4  starDensityHorizon;
    vec4  starSizeRange;
} uSky;

layout(location = 0) out vec2 vQuadPos;
layout(location = 1) flat out float vBrightness;
layout(location = 2) flat out float vTwinkle;
layout(location = 3) flat out float vRotAngle;
layout(location = 4) flat out float vSizeScale;
layout(location = 5) flat out float vColorTemp;

const float PI = 3.14159265359;

vec2 QuadOffset(uint vid)
{
    if (vid == 0u) return vec2(-1.0, -1.0);
    if (vid == 1u) return vec2( 1.0, -1.0);
    if (vid == 2u) return vec2(-1.0,  1.0);
    if (vid == 3u) return vec2(-1.0,  1.0);
    if (vid == 4u) return vec2( 1.0, -1.0);
    return              vec2( 1.0,  1.0);
}

void main()
{
    uint starIdx = gl_InstanceIndex;
    StarInstance star = sb.stars[starIdx];

    vec3 starDir = star.dirAndBrightness.xyz;
    float brightness = star.dirAndBrightness.w;
    float sizeScale = star.properties.x;
    float rotAngle = star.properties.y;
    float twinkleSeed = star.properties.z;
    float colorTemp = star.properties.w;

    vec3 fwd = normalize(uSky.cameraForwardWS);
    vec3 right = normalize(uSky.cameraRightWS);
    vec3 camUp = normalize(uSky.cameraUpWS);

    float viewDot = dot(starDir, fwd);
    float nightAmount = clamp(uSky.nightSkyBlend, 0.0, 1.0);

    float horizonFade = smoothstep(-0.02, 0.05, starDir.y);

    // tanHalfFovY <= 0 signals orthographic projection.
    bool isOrtho = uSky.tanHalfFovY <= 0.0;

    // Perspective: cull behind-camera and below-horizon stars. Ortho 2D
    // backdrop: show stars across the full NDC rectangle with no horizon
    // cull or fade, so the lower half of the screen also fills with stars.
    bool cull = nightAmount <= 0.0;
    if (!isOrtho)
        cull = cull || viewDot <= -0.1 || horizonFade <= 0.0;
    else
        cull = cull || viewDot <= 0.0;

    if (cull)
    {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        vQuadPos = vec2(0.0);
        vBrightness = 0.0;
        vTwinkle = 0.0;
        vRotAngle = 0.0;
        vSizeScale = 0.0;
        vColorTemp = 0.0;
        return;
    }

    float starSize = clamp(uSky.starSizeShape.y, 0.001, 2.0);
    float diamondRadius = clamp(uSky.starSizeShape.z, 0.001, 2.0);
    float maxExtent = max(starSize, diamondRadius) * sizeScale;

    float angularRadius = maxExtent * 0.015;

    vec2 centerNDC;
    vec2 halfSizeNDC;
    if (isOrtho)
    {
        // Project stars through a virtual sky dome using the camera basis,
        // matching the orthographic sun disk in sky_render.frag.
        vec3 viewDir = vec3(dot(starDir, right), dot(starDir, camUp), dot(starDir, fwd));
        const float virtualTanHalfFovY = 1.0;
        float aspect = max(uSky.viewportAspect, 0.001);
        float invZ = 1.0 / max(abs(viewDir.z), 1e-4);
        centerNDC = vec2(viewDir.x * invZ / (aspect * virtualTanHalfFovY),
                         viewDir.y * invZ / virtualTanHalfFovY);
        halfSizeNDC = vec2(angularRadius / aspect, angularRadius);
    }
    else
    {
        float projScaleY = 1.0 / uSky.tanHalfFovY;
        float projScaleX = projScaleY / max(uSky.viewportAspect, 0.001);
        vec3 viewDir = vec3(dot(starDir, right), dot(starDir, camUp), dot(starDir, fwd));
        centerNDC = vec2(viewDir.x * projScaleX, viewDir.y * projScaleY) / viewDir.z;
        halfSizeNDC = vec2(angularRadius * projScaleX, angularRadius * projScaleY);
    }

    vec2 quadCorner = QuadOffset(gl_VertexIndex % 6u);
    vQuadPos = quadCorner;

    vec2 offsetNDC = quadCorner * halfSizeNDC;

    gl_Position = vec4(centerNDC + offsetNDC, 0.5, 1.0);

    float moonVisibility = 1.0;
    if (uSky.moonIntensity > 0.0)
    {
        vec3 moonDir = normalize(uSky.moonDirectionWS);
        if (isOrtho)
        {
            float aspect = max(uSky.viewportAspect, 0.001);
            vec2 moonScreen = vec2(dot(moonDir, right), dot(moonDir, camUp));
            if (uSky.skyRotateAroundZenithRadians > 0.5)
            {
                const float maxMoonScreenRadius = 0.82;
                float moonScreenLen = length(moonScreen);
                if (moonScreenLen > maxMoonScreenRadius)
                    moonScreen *= maxMoonScreenRadius / moonScreenLen;
            }

            vec2 moonNDC = vec2(moonScreen.x / aspect, moonScreen.y);
            float moonRadius = max(uSky.moonAngularRadius * 6.0 * max(uSky.pad1, 0.1), 0.05);
            vec2 moonDelta = vec2((centerNDC.x - moonNDC.x) * aspect, centerNDC.y - moonNDC.y);
            moonVisibility = smoothstep(moonRadius * 0.98, moonRadius * 1.08, length(moonDelta));
        }
        else
        {
            float moonRadius = max(uSky.moonAngularRadius, 0.0);
            float moonDot = dot(starDir, moonDir);
            moonVisibility = 1.0 - smoothstep(cos(moonRadius * 1.25), cos(moonRadius * 0.95), moonDot);
        }
    }

    float t = uSky.skyTimeSeconds;
    float minFreq = max(uSky.starTwinkleParams.x, 0.0);
    float maxFreq = max(uSky.starTwinkleParams.y, minFreq);
    float freqHz = mix(minFreq, maxFreq, twinkleSeed);
    float phase = twinkleSeed * 6.28 * uSky.starTwinkleParams.z;
    float s = 0.5 + 0.5 * sin(t * freqHz * 2.0 * PI + phase);
    float tw = pow(s, 1.5);
    float minTwinkle = clamp(uSky.starTwinkleAmp.x, 0.0, 2.0);
    float maxTwinkle = clamp(uSky.starTwinkleAmp.y, minTwinkle, 4.0);
    vTwinkle = mix(minTwinkle, maxTwinkle, tw);

    vBrightness = brightness * (isOrtho ? 1.0 : horizonFade) * nightAmount * moonVisibility;
    vRotAngle = rotAngle;
    vSizeScale = sizeScale;
    vColorTemp = colorTemp;
}
