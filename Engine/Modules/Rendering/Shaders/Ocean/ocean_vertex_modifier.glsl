// Ocean vertex output modifier for the adapter pipeline.
// Included by adapter_vertex.glsl via GE_VERTEX_MODIFIER_PATH when
// HAS_VERTEX_OUTPUT_MODIFIER is defined.
//
// Declares its own packed clipmap-ring vertex input, follows/snaps the rings to
// the camera, and displaces them from the FFT cascade stack.

#include "Ocean/ocean_common.glsl"

// xy = clipmap-local XZ position in base-tile units, z = LOD ring index,
// w = cell-footprint multiplier (the pushed outer horizon skirt is larger).
layout(location = 0) in vec4 aGridData;

const float GE_OCEAN_GEOMETRY_MIN_VERTICAL_WAVE_SIZE = 2.0;
const float GE_OCEAN_GEOMETRY_MIN_HORIZONTAL_WAVE_SIZE = 8.0;
const float GE_OCEAN_GEOMETRY_LOD_COUNT = 8.0;
const float GE_OCEAN_TILE_RESOLUTION_DIVISOR = 10.0;
const float GE_OCEAN_MIN_TILE_RESOLUTION = 16.0;

void ModifyVertex(inout VertexOutput v, InstanceData inst)
{
    vec3 camPos = Cam.uCameraPos.xyz;

    // World-space concentric clipmap layout adapted from Crest's proven ocean
    // geometry strategy. The packed CPU mesh supplies one dense center plus
    // power-of-two rings. The vertex stage follows the camera, snaps each ring
    // to twice its cell width, and morphs alternating vertices onto the next
    // coarser grid near ring boundaries so no cracks or popping appear.
    float reachDist = uPatchExtent * 5.0;
    float baseTileScale = reachDist / exp2(GE_OCEAN_GEOMETRY_LOD_COUNT);

    // Raise the entire clipmap by powers of two as the viewer gains altitude.
    // LOD0 morphs out during the fractional transition, then the next scale is
    // geometrically identical when the power-of-two step occurs.
    float viewerHeight = max(abs(camPos.y - uSeaLevel), 1.0);
    float altitudeLevel = max(log2(viewerHeight / max(baseTileScale, 1.0)), 0.0);
    float altitudeScale = exp2(floor(altitudeLevel));
    float meshScaleLerp = fract(altitudeLevel);
    float rootScale = baseTileScale * altitudeScale;

    float lodIndex = clamp(aGridData.z, 0.0, GE_OCEAN_GEOMETRY_LOD_COUNT - 1.0);
    float lodScale = exp2(lodIndex);
    float tileResolution = max(floor(uGeometryGridSize /
                                     GE_OCEAN_TILE_RESOLUTION_DIVISOR),
                               GE_OCEAN_MIN_TILE_RESOLUTION);
    tileResolution = max(floor(tileResolution * 0.5) * 2.0,
                         GE_OCEAN_MIN_TILE_RESOLUTION);
    float gridWidth = rootScale * lodScale / tileResolution;

    vec2 worldXZ = camPos.xz + aGridData.xy * rootScale;
    float snapWidth = 2.0 * gridWidth;
    worldXZ -= fract(camPos.xz / snapWidth) * snapWidth;

    float taxicab = max(abs(worldXZ.x - camPos.x), abs(worldXZ.y - camPos.z));
    float lodAlpha = taxicab / max(rootScale * lodScale, 1.0e-4) - 1.0;
    lodAlpha = clamp((lodAlpha - 0.15) / 0.70, 0.0, 1.0);
    if (lodIndex < 0.5)
        lodAlpha = min(lodAlpha + meshScaleLerp, 1.0);

    float coarseWidth = 4.0 * gridWidth;
    vec2 coarseCell = fract(worldXZ / coarseWidth) - vec2(0.5);
    if (abs(coarseCell.x) < 0.375)
        worldXZ.x += coarseCell.x * lodAlpha * coarseWidth;
    if (abs(coarseCell.y) < 0.375)
        worldXZ.y += coarseCell.y * lodAlpha * coarseWidth;

    float vertexFootprint = mix(gridWidth, 2.0 * gridWidth, lodAlpha) *
                            max(aGridData.w, 1.0);

    float minVerticalWaveSize = max(GE_OCEAN_GEOMETRY_MIN_VERTICAL_WAVE_SIZE,
                                    vertexFootprint * 2.0);
    float minHorizontalWaveSize = max(GE_OCEAN_GEOMETRY_MIN_HORIZONTAL_WAVE_SIZE,
                                      vertexFootprint * 4.0);

    vec3 pos;
    vec3 nrm;
    if (uFFTCascadeCount > 0u)
    {
        // FFT path: sum the displacement cascades and build the surface normal
        // from finite differences of the displaced position (so the choppy
        // horizontal pinch shows up in the lighting, not just the height).
        float e = clamp(minVerticalWaveSize * 0.5, 0.75, 32.0);
        vec3 d0  = OceanShapeDisplacementFilteredAniso(worldXZ, minHorizontalWaveSize, minVerticalWaveSize);
        vec3 dpx = OceanShapeDisplacementFilteredAniso(worldXZ + vec2(e, 0.0),
                                                       minHorizontalWaveSize,
                                                       minVerticalWaveSize);
        vec3 dpz = OceanShapeDisplacementFilteredAniso(worldXZ + vec2(0.0, e),
                                                       minHorizontalWaveSize,
                                                       minVerticalWaveSize);
        vec3 P0 = vec3(worldXZ.x + d0.x,      uSeaLevel + d0.y,  worldXZ.y + d0.z);
        vec3 Px = vec3(worldXZ.x + e + dpx.x, uSeaLevel + dpx.y, worldXZ.y + dpx.z);
        vec3 Pz = vec3(worldXZ.x + dpz.x,     uSeaLevel + dpz.y, worldXZ.y + e + dpz.z);
        nrm = normalize(cross(Pz - P0, Px - P0));
        if (nrm.y < 0.0)
            nrm = -nrm;
        pos = P0;
    }
    else
    {
        float foam;
        EvaluateGerstner(worldXZ, pos, nrm, foam);
    }

    // Dynamic (interactive) waves: add the simulated ripple height on top of the
    // spectrum displacement, and tilt the surface normal by its slope so the
    // ripples both deform the geometry and light correctly. Sampled with textureLod
    // (vertex stage has no derivatives). Zero when the sim is inactive, so the
    // spectrum surface is unchanged. The slope is a central difference of the
    // height field at the undisplaced world XZ (the sim grid is world-aligned).
    if (uDynamicWavesAvailable != 0u)
    {
        float dynScale = OceanSampleWaveMask(worldXZ).x;
        float dynH = clamp(OceanSampleDynWaveHeight(worldXZ) * uDynWavesAmplitude * dynScale,
                           -uDynWavesDisplacementClamp, uDynWavesDisplacementClamp);
        pos.y += dynH;

        float de = 0.5; // world-space slope step (meters)
        float hL = OceanSampleDynWaveHeight(worldXZ - vec2(de, 0.0)) * uDynWavesAmplitude * dynScale;
        float hR = OceanSampleDynWaveHeight(worldXZ + vec2(de, 0.0)) * uDynWavesAmplitude * dynScale;
        float hD = OceanSampleDynWaveHeight(worldXZ - vec2(0.0, de)) * uDynWavesAmplitude * dynScale;
        float hU = OceanSampleDynWaveHeight(worldXZ + vec2(0.0, de)) * uDynWavesAmplitude * dynScale;
        vec2 dynGradient = vec2(hR - hL, hU - hD) / (2.0 * de);
        pos.xz -= dynGradient * uDynWavesHorizontalDisplacement;
        vec3 dynN = normalize(vec3(hL - hR, 2.0 * de, hD - hU));
        // Blend the dynamic-wave normal into the spectrum normal (its tilt rides on
        // top of the spectrum slope rather than replacing it).
        nrm = normalize(nrm + vec3(dynN.x, 0.0, dynN.z));
    }

    // Vertex output modifiers produce WORLD-space position and normal directly.
    v.position = pos;
    v.normal   = nrm;
    v.uv0      = worldXZ;                       // world XZ (meters) for detail UVs
    v.custom0  = vec4(1.0);                     // reserved (flat-interpolated)
}
