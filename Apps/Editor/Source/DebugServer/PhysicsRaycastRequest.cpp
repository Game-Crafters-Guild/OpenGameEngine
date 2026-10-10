#include "DebugServer/PhysicsRaycastRequest.h"

#include <cmath>

namespace GameEngine::Editor
{

namespace
{
using json = nlohmann::json;

// Read an [x,y,z] number array. Absent -> false with err untouched; present but malformed
// -> false with err set (the caller distinguishes the two via err).
bool TryReadVec3(const json& params, const char* key, Physics::Vector3& out, std::string& err)
{
    if (!params.contains(key))
        return false;
    const json& a = params[key];
    if (!a.is_array() || a.size() != 3 || !a[0].is_number() || !a[1].is_number() || !a[2].is_number())
    {
        err = std::string(key) + " must be a [x,y,z] number array";
        return false;
    }
    const Physics::Vector3 v{a[0].get<float32>(), a[1].get<float32>(), a[2].get<float32>()};
    // A JSON number outside float range parses as a number and narrows to inf. The backend
    // casts against `origin + direction * maxDistance`, so one non-finite component makes
    // the whole segment NaN math that no downstream comparison catches.
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
    {
        err = std::string(key) + " is not finite — it cannot form a ray";
        return false;
    }
    out = v;
    return true;
}

} // namespace

bool ReadRayCastQuery(const json& params, Physics::RayCastQuery& out, std::string& err)
{
    Physics::RayCastQuery query{};
    if (!TryReadVec3(params, "origin", query.ray.origin, err))
    {
        if (err.empty())
            err = "Provide 'origin' as a [x,y,z] array";
        return false;
    }
    if (!TryReadVec3(params, "direction", query.ray.direction, err))
    {
        if (err.empty())
            err = "Provide 'direction' as a [x,y,z] array";
        return false;
    }

    if (params.contains("maxDistance"))
    {
        const json& d = params["maxDistance"];
        const float32 maxDistance = d.is_number() ? d.get<float32>() : 0.0f;
        // Rejected, not clamped: a cap of zero or less is an empty segment whose guaranteed
        // miss would read as a genuine one, and non-finite scales the segment endpoint
        // non-finite in the backend.
        if (!d.is_number() || !std::isfinite(maxDistance) || maxDistance <= 0.0f)
        {
            err = "maxDistance must be a positive finite number";
            return false;
        }
        query.maxDistance = maxDistance;
    }
    else
    {
        query.maxDistance = 1000.0f;
    }

    query.filter.layerMask = params.value("layerMask", 0xFFFFFFFFu);
    out = query;
    return true;
}

} // namespace GameEngine::Editor
