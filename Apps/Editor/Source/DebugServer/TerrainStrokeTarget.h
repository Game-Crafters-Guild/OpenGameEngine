#pragma once

#include "Mathematics/Vector3.h"

#include <nlohmann/json.hpp>

#include <string>

namespace GameEngine::Editor
{

// Stroke-endpoint resolution for the `terrain_sculpt_dab` IPC handler: turns the request's
// `pos`/`dir` (and `end_pos`/`end_dir`) parameters into world-space targets, and interpolates
// between two of them along the domain's own path.
//
// It lives apart from the handler because every rejection here is a guard the sculpt path
// downstream cannot make for itself: a non-finite target passes every `length < epsilon`
// check further down (a NaN compares false against all of them), so the stroke reports
// success while writing nothing. Resolution is where a bad endpoint has to die.

// Unit-sphere slerp between two surface directions. Both inputs must be unit length and
// finite; a zero-length input normalizes to NaN and propagates.
Mathematics::Vector3 SlerpDir(const Mathematics::Vector3& a, const Mathematics::Vector3& b, float t);

// Resolve a stroke endpoint from `posKey` ([x,y,z] world position) or `dirKey` ([x,y,z]
// surface direction from the planet centre, spherical only). `spherical` selects which keys
// are meaningful, so a planar caller reaching for `dir` is told what to pass instead of
// silently sculpting at the world origin, and so the planet centre is rejected as a position
// while a planar terrain keeps the origin as an ordinary one. A non-finite component is
// rejected on either domain and for either key, since no terrain shape has such a point.
// Returns false with err empty when neither key is present.
bool ResolveStrokeTarget(const nlohmann::json& params, const char* posKey, const char* dirKey,
                         bool spherical, Mathematics::Vector3& out, std::string& err);

} // namespace GameEngine::Editor
