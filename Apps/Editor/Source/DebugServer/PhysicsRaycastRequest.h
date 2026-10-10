#pragma once

#include "Physics/PhysicsQuery.h"

#include <nlohmann/json.hpp>

#include <string>

namespace GameEngine::Editor
{

// Parameter resolution for the `physics_raycast` IPC handler: turns the request's `origin`,
// `direction`, `maxDistance` and `layerMask` parameters into a Physics::RayCastQuery.
//
// It lives apart from the handler because every rejection here is a guard the query path
// downstream cannot make for itself: a JSON number outside float range parses as a number
// and narrows to inf, and the backend casts against `origin + direction * maxDistance`, so
// one non-finite input turns the whole segment into NaN math that no later check catches.
//
// Rules: `origin` and `direction` are required [x,y,z] arrays with finite components.
// `maxDistance` defaults to 1000 and must be a positive finite number — a cap of zero or
// less describes an empty segment whose guaranteed miss would be indistinguishable from a
// genuine one, so it is rejected rather than clamped or defaulted. Returns false with err
// stating the fix; writes `out` only on success.
bool ReadRayCastQuery(const nlohmann::json& params, Physics::RayCastQuery& out, std::string& err);

} // namespace GameEngine::Editor
