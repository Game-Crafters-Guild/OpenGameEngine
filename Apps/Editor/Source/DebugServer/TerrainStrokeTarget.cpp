#include "DebugServer/TerrainStrokeTarget.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Editor
{

namespace
{
using json = nlohmann::json;

// Shortest vector that still names a direction. Below it a normalize is NaN, and NaN passes
// every downstream comparison guard.
constexpr float kMinDirectionLength = 1e-6f;

// Parse a [x,y,z] number array parameter. Absent -> false with err untouched; present but
// malformed -> false with err set (the caller distinguishes the two via err).
bool TryReadVec3(const json& params, const char* key, Mathematics::Vector3& out, std::string& err)
{
    if (!params.contains(key))
        return false;
    const json& a = params[key];
    if (!a.is_array() || a.size() != 3 || !a[0].is_number() || !a[1].is_number() || !a[2].is_number())
    {
        err = std::string(key) + " must be a [x,y,z] number array";
        return false;
    }
    const Mathematics::Vector3 v{a[0].get<float>(), a[1].get<float>(), a[2].get<float>()};
    // A JSON number outside float range parses as a number and narrows to inf, and inf clears a
    // `length < epsilon` test as easily as zero fails it — so a non-finite component would reach
    // the sculpt as a NaN target. Rejecting it here covers every key the resolver reads, and it
    // is ungated: no terrain of either shape has a non-finite point.
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
    {
        err = std::string(key) + " is not finite — it names no surface point";
        return false;
    }
    out = v;
    return true;
}

} // namespace

Mathematics::Vector3 SlerpDir(const Mathematics::Vector3& a, const Mathematics::Vector3& b, float t)
{
    const float dot = std::clamp(Mathematics::Vector3::Dot(a, b), -1.0f, 1.0f);
    const float angle = std::acos(dot);
    const float s = std::sin(angle);
    if (s < kMinDirectionLength) // coincident or antipodal — nothing sensible to interpolate
        return t < 0.5f ? a : b;
    return (a * (std::sin((1.0f - t) * angle) / s) + b * (std::sin(t * angle) / s)).Normalize();
}

bool ResolveStrokeTarget(const json& params, const char* posKey, const char* dirKey,
                         bool spherical, Mathematics::Vector3& out, std::string& err)
{
    Mathematics::Vector3 v{};
    if (TryReadVec3(params, posKey, v, err))
    {
        // A planet is centred at the world origin, so its centre names no surface point and
        // normalizes to NaN. Reject it here: downstream every guard is a `length < epsilon`
        // test, all of which a NaN passes, so the stroke would report dabs it never wrote.
        // A planar terrain has no such singularity — the origin is an ordinary position there.
        if (spherical && v.Length() < kMinDirectionLength)
        {
            err = std::string(posKey) + " is at the planet centre — it names no surface point";
            return false;
        }
        out = v;
        return true;
    }
    if (!err.empty())
        return false;
    if (params.contains(dirKey) && !spherical)
    {
        err = std::string(dirKey) + " is a planet surface direction and means nothing on a planar "
              "terrain — pass " + posKey + " as a world position over the terrain instead";
        return false;
    }
    if (TryReadVec3(params, dirKey, v, err))
    {
        if (v.Length() < kMinDirectionLength)
        {
            err = std::string(dirKey) + " is the zero vector — it names no surface direction";
            return false;
        }
        out = v;
        return true;
    }
    return false;
}

} // namespace GameEngine::Editor
