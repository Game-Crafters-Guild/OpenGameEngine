#version 450

// Debug visualisation overlay for the clustered light cull output.
// Reads the new V2 ClusterBuffer schema (uvec4 header + uvec2 per cluster).
// Per-pixel: looks up the (tileX, tileY) and visualises per-tile light density
// by summing over Z slices.

layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0, std430) readonly buffer ClusterBufferBlock
{
    uvec4 header;       // (clustersX, clustersY, depthSlices, maxLightsPerCluster)
    uvec2 clusters[];   // (firstIndex, count) per cluster, indexed by 3D clusterId
} ClusterBuffer;

layout(set = 0, binding = 1, std430) readonly buffer LightIndexBufferBlock
{
    uint indices[];
} LightIndexBuffer;

layout(set = 0, binding = 2, std140) uniform ClusterParamsBlock
{
    uvec4 params0; // tileX, tileY, depthSlices, maxLightsPerCluster
} ClusterParams;

void main()
{
    const uvec2 tileSize = max(uvec2(ClusterParams.params0.xy), uvec2(1u, 1u));
    const uint clustersX = max(ClusterBuffer.header.x, 1u);
    const uint clustersY = max(ClusterBuffer.header.y, 1u);
    const uint depthSlices = max(ClusterBuffer.header.z, 1u);
    const uint maxLightsPerCluster = max(ClusterBuffer.header.w, 1u);

    const uvec2 tile = uvec2(gl_FragCoord.xy) / tileSize;
    if (tile.x >= clustersX || tile.y >= clustersY)
    {
        oColor = vec4(0.0);
        return;
    }

    const uint clusterXY = tile.x + tile.y * clustersX;
    const uint stride = clustersX * clustersY;
    uint sumCount = 0u;
    uint maxCount = 0u;
    uint firstFoundIndex = 0u;
    bool foundAny = false;
    for (uint s = 0u; s < depthSlices; ++s)
    {
        // 3D clusterId in the V2 schema: tx + ty*cX + sli*cX*cY.
        const uint cid = clusterXY + s * stride;
        const uvec2 c = ClusterBuffer.clusters[cid];
        sumCount += c.y;
        maxCount = max(maxCount, c.y);
        if (!foundAny && c.y > 0u)
        {
            firstFoundIndex = LightIndexBuffer.indices[c.x];
            foundAny = true;
        }
    }

    const float lc = clamp(float(maxCount) / float(maxLightsPerCluster), 0.0, 1.0);
    const float ssum = clamp(float(sumCount) / max(1.0, float(depthSlices * maxLightsPerCluster)), 0.0, 1.0);
    const float f = fract(sin(float(clusterXY) * 12.9898) * 43758.5453);
    const vec3 base = mix(vec3(0.1), vec3(f, 1.0 - f, fract(float(firstFoundIndex) * 0.07)), 0.25);
    vec3 col = mix(base, vec3(1.0, 0.3 + 0.5 * ssum, 0.1), lc);
    float alpha = 0.20;

    // Cap-saturation flag: bright red where ANY Z slice in this tile hit the
    // per-cluster cap (lights got DROPPED). atomicAdd ordering is non-
    // deterministic at saturation, so these tiles are where flicker / quad-
    // boundary artifacts live. Use this overlay to confirm cap-saturation
    // hypothesis before paying perf cost to fix it.
    if (maxCount >= maxLightsPerCluster)
    {
        col = vec3(1.0, 0.0, 0.0);  // bright red = saturated
        alpha = 0.65;
    }
    else if (maxCount * 4u >= maxLightsPerCluster * 3u)  // >= 75% of cap
    {
        col = mix(col, vec3(1.0, 0.85, 0.0), 0.7);  // yellow warning band
        alpha = 0.45;
    }

    oColor = vec4(col, alpha);
}

