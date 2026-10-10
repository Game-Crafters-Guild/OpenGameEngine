#version 450

// Selection mask draw (static meshes).
//
// Reads only the position attribute from the standard interleaved core
// vertex buffer (Position + Normal + UV0, stride 32). Normal/UV streams
// exist on the buffer but are ignored — the layout binds only location 0.
//
// The push-constant layout is shared with the alpha and skinned mask shaders
// so a single C++ MaskPC struct and push-constant range drives every variant.
// uVPM is viewProj * model combined on the CPU to keep the range inside the
// engine's 128-byte push-constant limit.

layout(location = 0) in vec3 aPos;

layout(push_constant) uniform PC
{
    mat4 uVPM;
    vec4 uMaskParams;   // x alpha cutoff, yz UV scale, w skin palette offset
    vec4 uMaskExtra;    // xy UV offset, z time seconds, w instance seed
    vec4 uWindStrength; // xyz local wind, w enabled
    vec4 uWindParams;   // x frequency, y spatial scale, z height, w base Y
} pc;

float SelectionMaskWindHash(vec3 p)
{
    return fract(sin(dot(p, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
}

vec3 ApplySelectionMaskTreeWind(vec3 position)
{
    if (pc.uWindStrength.w < 0.5)
        return position;

    float heightScale = max(pc.uWindParams.z, 0.001);
    float height01 = clamp((position.y - pc.uWindParams.w) / heightScale, 0.0, 1.0);
    float trunkWeight = height01 * height01 * (3.0 - 2.0 * height01);
    float branchWeight = pow(height01, 1.35);

    float time = pc.uMaskExtra.z * max(pc.uWindParams.x, 0.0);
    float spatialScale = max(pc.uWindParams.y, 0.001);
    float spatial = (position.x * 0.21 + position.y * 0.13 + position.z * 0.17) / spatialScale;
    float seed = SelectionMaskWindHash(floor(position * 0.173 + vec3(pc.uMaskExtra.w * 0.031)));
    float edgeFlutter = 0.35;

    vec3 wind = pc.uWindStrength.xyz;
    float windMagnitude = length(wind);
    if (windMagnitude <= 0.0001)
        return position;

    vec3 windDir = wind / windMagnitude;
    vec3 lateralDir = normalize(vec3(-windDir.z, 0.0, windDir.x) + vec3(0.0001, 0.0, 0.0));

    float slowGust = sin(time * 0.37 + spatial * 0.55 + seed * 6.2831);
    float mainWave = sin(time + spatial + slowGust * 0.45);
    float secondaryWave = sin(time * 1.91 + position.x * 0.19 - position.z * 0.23 + seed * 12.566);
    float ripple = sin(time * 5.2 + position.y * 0.61 + seed * 18.849);

    float trunkSway = (mainWave * 0.72 + secondaryWave * 0.28) * trunkWeight * 0.32;
    float branchSway = (secondaryWave * 0.55 + slowGust * 0.45) * branchWeight * 0.35;
    float leafFlutter = ripple * edgeFlutter * branchWeight * 0.34;

    vec3 displacement = wind * trunkSway;
    displacement += lateralDir * windMagnitude * branchSway;
    displacement += (windDir + lateralDir * 0.45) * windMagnitude * leafFlutter;
    displacement.y += windMagnitude * leafFlutter * 0.12;
    return position + displacement;
}

void main()
{
    gl_Position = pc.uVPM * vec4(ApplySelectionMaskTreeWind(aPos), 1.0);
}
