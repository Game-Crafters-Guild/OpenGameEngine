// Tileable layered Worley noise shared by the shape/detail bake kernels.
// Cell feature points are hash-derived from the wrapped cell coordinate, so
// the field tiles seamlessly at every octave without CPU-side point buffers.

float VcHash13(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

vec3 VcHash33(vec3 p)
{
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx);
}

// Distance to the nearest feature point of a jittered grid with numCells cells
// per axis, wrapped for seamless tiling. samplePos is in [0,1); the result is
// normalized by the cell size so it spans roughly [0,1].
float VcWorley(vec3 samplePos, int numCells, float seed)
{
    vec3 scaled = samplePos * float(numCells);
    ivec3 cellId = ivec3(floor(scaled));
    float minSqrDst = 1e10;
    for (int z = -1; z <= 1; ++z)
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
    {
        ivec3 adjId = cellId + ivec3(x, y, z);
        ivec3 wrapped = ivec3(mod(vec3(adjId), float(numCells)));
        vec3 featurePoint = vec3(adjId) + VcHash33(vec3(wrapped) + seed);
        vec3 offset = scaled - featurePoint;
        minSqrDst = min(minSqrDst, dot(offset, offset));
    }
    return sqrt(minSqrDst);
}

// Three inverted Worley octaves folded with persistence 0.5 — the layering the
// upstream NoiseGenCompute.compute performs per channel (divisions A/B/C).
float VcLayeredWorley(vec3 samplePos, int cellsA, int cellsB, int cellsC, float seed)
{
    const float kPersistence = 0.5;
    float layerA = 1.0 - clamp(VcWorley(samplePos, cellsA, seed), 0.0, 1.0);
    float layerB = 1.0 - clamp(VcWorley(samplePos, cellsB, seed + 17.0), 0.0, 1.0);
    float layerC = 1.0 - clamp(VcWorley(samplePos, cellsC, seed + 43.0), 0.0, 1.0);
    float noiseSum = layerA + layerB * kPersistence + layerC * kPersistence * kPersistence;
    float maxVal = 1.0 + kPersistence + kPersistence * kPersistence;
    return noiseSum / maxVal;
}
