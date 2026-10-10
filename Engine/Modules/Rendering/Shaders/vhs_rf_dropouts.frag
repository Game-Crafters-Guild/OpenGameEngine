#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSceneColor;

layout(push_constant) uniform VhsRfDropoutsPC {
    float vhsRfDropouts;
    float vhsSpeed;
    float shaderAnimationTime;
} pc;

float Hash(vec2 p)
{
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453123);
}

void main()
{
    vec4 source = texture(uSceneColor, vUV);
    float strength = clamp(pc.vhsRfDropouts, 0.0, 2.0);
    if (strength <= 1e-5)
    {
        oColor = source;
        return;
    }

    vec2 imageSizePx = vec2(textureSize(uSceneColor, 0));
    vec2 fragmentPx = vUV * imageSizePx;
    float time = pc.shaderAnimationTime * max(pc.vhsSpeed, 0.0);
    float frameTick = floor(time * 29.97);

    // Sparse short RF-loss comets are held for one video frame, then reseeded.
    // This produces changing tape dropouts without continuously sliding shapes.
    vec2 cellSize = vec2(38.0, 7.0);
    vec2 cell = floor(fragmentPx / cellSize);
    vec2 within = mod(fragmentPx, cellSize);
    // Independent frame offsets prevent the hash lattice from appearing to
    // drift vertically. A dropout is replaced rather than translated.
    vec2 frameSeed = vec2(
        Hash(vec2(frameTick, 19.0)) * 8192.0,
        Hash(vec2(frameTick, 47.0)) * 8192.0);
    float seed = Hash(cell + frameSeed);
    float densityScale = mix(
        0.32, 1.72, pow(Hash(vec2(frameTick, 113.0)), 1.35));
    float burst = step(0.955, Hash(vec2(frameTick, 211.0)));
    densityScale *= mix(1.0, 1.75, burst);
    float rowCluster = mix(
        0.58, 1.42,
        Hash(vec2(floor(cell.y / 7.0), frameTick * 3.0 + 71.0)));

    // Oxide damage and marginal head contact commonly affect a short run of
    // neighboring tracks. Concentrate many frames into one narrow horizontal
    // region; some events are deliberately biased toward the lower picture,
    // adjacent to the head-switching area, without making every frame bottom-heavy.
    float clusterActive = step(0.42, Hash(vec2(frameTick, 263.0)));
    float bottomCluster = step(0.70, Hash(vec2(frameTick, 307.0)));
    float randomCenter = mix(
        0.08, 0.92, Hash(vec2(frameTick, 331.0)));
    float clusterCenter = mix(
        randomCenter,
        mix(0.78, 0.975, Hash(vec2(frameTick, 353.0))),
        bottomCluster);
    float clusterHalfHeight = mix(
        5.0, 34.0, pow(Hash(vec2(frameTick, 379.0)), 1.8));
    float clusterBand = 1.0 - smoothstep(
        clusterHalfHeight * 0.42, clusterHalfHeight,
        abs(fragmentPx.y - clusterCenter * imageSizePx.y));
    float clusterDensity = mix(
        0.48, 3.8, clusterBand * clusterActive);
    float density = min(
        (0.012 + strength * 0.048)
        * densityScale * rowCluster * clusterDensity,
        0.24);
    float present = step(1.0 - density, seed);
    float centerX =
        3.0 + Hash(cell + frameSeed + 17.0) * (cellSize.x - 6.0);
    float centerY =
        1.0 + Hash(cell + frameSeed + 31.0) * (cellSize.y - 2.0);
    float halfLength = mix(
        0.7, 7.5, pow(Hash(cell + frameSeed + 47.0), 2.2));
    float halfHeight = mix(
        0.45, 1.15, Hash(cell + frameSeed + 59.0));
    vec2 distancePx = abs(within - vec2(centerX, centerY));
    float dash = (1.0 - smoothstep(halfLength * 0.72, halfLength + 1.2,
                                   distancePx.x))
               * (1.0 - smoothstep(halfHeight * 0.45, halfHeight + 0.9,
                                   distancePx.y));
    float damage = present * dash * clamp(0.34 + strength * 0.34, 0.0, 0.92);

    // RF loss clips toward low-saturation luma with a tiny color-under fringe.
    float luma = dot(source.rgb, vec3(0.299, 0.587, 0.114));
    vec3 rfLoss = vec3(mix(luma, 0.94, 0.78));
    float fringe = present * (1.0 - smoothstep(
        halfLength + 0.5, halfLength + 2.4, distancePx.x))
        * (1.0 - smoothstep(halfHeight, halfHeight + 1.4, distancePx.y));
    rfLoss += vec3(-0.025, 0.008, 0.032) * fringe;
    float dropoutGain = mix(
        0.58, 1.0, Hash(cell + frameSeed + 83.0));
    oColor = vec4(
        mix(source.rgb, rfLoss, damage * dropoutGain), source.a);
}
