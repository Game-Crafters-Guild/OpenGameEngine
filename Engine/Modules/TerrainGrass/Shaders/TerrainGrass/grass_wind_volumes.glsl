// CPU snapshot: WindVolumeResolver::ExtractGPU. Positions share WorldTransform's render space.
// GE_SHARED_GRASS_WIND_VOLUMES_BEGIN
struct GrassWindVolume
{
    vec4 CenterShape;
    vec4 AxisXExtent;
    vec4 AxisYExtent;
    vec4 AxisZExtent;
    vec4 VelocityWeight;
    vec4 Gust;
    vec4 Mode;
};

struct GrassResolvedWind
{
    vec3 velocity;
    float turbulence;
    float frequency;
    float scale;
    float weight;
};

float grassVolumeWeight(GrassWindVolume v, vec3 position)
{
    if (v.CenterShape.w == -1.0f) return v.VelocityWeight.w;
    if (v.CenterShape.w == -2.0f || v.VelocityWeight.w <= 0.0f) return 0.0f;
    vec3 delta = position - vec3(v.CenterShape.x, v.CenterShape.y, v.CenterShape.z);
    float x = dot(delta, vec3(v.AxisXExtent.x, v.AxisXExtent.y, v.AxisXExtent.z));
    float y = dot(delta, vec3(v.AxisYExtent.x, v.AxisYExtent.y, v.AxisYExtent.z));
    float z = dot(delta, vec3(v.AxisZExtent.x, v.AxisZExtent.y, v.AxisZExtent.z));
    float hx = max(v.AxisXExtent.w, 0.001f);
    float hy = max(v.AxisYExtent.w, 0.001f);
    float hz = max(v.AxisZExtent.w, 0.001f);
    float dist;
    if (v.CenterShape.w == 1.0f)
    {
        vec3 a = vec3(x / hx, y / hy, z / hz);
        vec3 b = vec3(x / (hx * hx), y / (hy * hy), z / (hz * hz));
        float k1 = sqrt(dot(a, a)), k2 = sqrt(dot(b, b));
        dist = max(0.0f, k1 > 0.0f && k2 > 0.0f ? k1 * (k1 - 1.0f) / k2 : 0.0f);
    }
    else if (v.CenterShape.w == 2.0f || v.CenterShape.w == 3.0f)
    {
        float radius = max(hx, hz);
        float core = max(0.0f, hy - (v.CenterShape.w == 2.0f ? radius : 0.0f));
        float qy = max(abs(y) - core, 0.0f);
        float q = sqrt(x * x + z * z);
        if (v.CenterShape.w == 2.0f)
            dist = max(0.0f, sqrt(q * q + qy * qy) - radius);
        else
        {
            float dx = max(q - radius, 0.0f);
            dist = sqrt(dx * dx + qy * qy);
        }
    }
    else
    {
        vec3 d = vec3(max(abs(x) - hx, 0.0f), max(abs(y) - hy, 0.0f), max(abs(z) - hz, 0.0f));
        dist = sqrt(dot(d, d));
    }
    if (dist <= 0.0f) return v.VelocityWeight.w;
    if (v.Gust.w <= 0.0f || dist >= v.Gust.w) return 0.0f;
    float t = 1.0f - dist / v.Gust.w;
    return v.VelocityWeight.w * t * t * (3.0f - 2.0f * t);
}

GrassResolvedWind grassBlendVolume(GrassResolvedWind wind, GrassWindVolume v, vec3 position)
{
    float w = grassVolumeWeight(v, position);
    if (w <= 0.0f) return wind;
    vec3 target = vec3(v.VelocityWeight.x, v.VelocityWeight.y, v.VelocityWeight.z);
    if (v.Mode.x == 1.0f)
    {
        wind.velocity = wind.velocity + (target - wind.velocity) * w;
        wind.turbulence += (v.Gust.x - wind.turbulence) * w;
    }
    else if (target.x == 0.0f && target.y == 0.0f && target.z == 0.0f)
    {
        wind.velocity = wind.velocity * (1.0f - w);
        wind.turbulence *= 1.0f - w;
    }
    else
    {
        wind.velocity = wind.velocity + target * w;
        wind.turbulence += v.Gust.x * w;
    }
    wind.frequency += (v.Gust.y - wind.frequency) * w;
    wind.scale += (v.Gust.z - wind.scale) * w;
    wind.weight = max(wind.weight, w);
    return wind;
}
// GE_SHARED_GRASS_WIND_VOLUMES_END

layout(std430, set = 2, binding = 8) readonly buffer GrassWindVolumes
{
    GrassWindVolume grassWindVolumes[];
};

// GE_SHARED_GRASS_WIND_APPLY_BEGIN
TerrainParamsEntry grassApplyWindVolumes(TerrainParamsEntry tp, vec3 root)
{
    GrassResolvedWind wind;
    wind.velocity = vec3(cos(tp.GrassWindDirection), 0.0, sin(tp.GrassWindDirection))
        * max(tp.GrassWindStrength, 0.0);
    wind.turbulence = max(tp.GrassWindFlutterAmount, 0.0);
    wind.frequency = max(tp.GrassWindGustSpeed, 0.0);
    // Volumes author wavelength in world units; the grass noise takes its reciprocal.
    wind.scale = 1.0f / max(tp.GrassWindGustScale, 0.0001f);
    wind.weight = 0.0;
    for (int i = 0; i < grassWindVolumes.length(); ++i)
        wind = grassBlendVolume(wind, grassWindVolumes[i], root);
    if (wind.weight <= 0.0) return tp; // Preserve authored settings outside volumes.
    float speed = length(vec2(wind.velocity.x, wind.velocity.z));
    if (speed > 0.00001)
        tp.GrassWindDirection = atan(wind.velocity.z, wind.velocity.x);
    tp.GrassWindStrength = speed;
    tp.GrassWindGustSpeed = max(wind.frequency, 0.0);
    tp.GrassWindGustScale = 1.0f / max(wind.scale, 0.001f);
    tp.GrassWindFlutterAmount = max(wind.turbulence, 0.0);
    return tp;
}
// GE_SHARED_GRASS_WIND_APPLY_END
