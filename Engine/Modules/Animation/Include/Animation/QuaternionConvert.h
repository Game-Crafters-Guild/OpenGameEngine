#pragma once

#include "Mathematics/Quaternion.h"

#include <glm/gtc/quaternion.hpp>

namespace GameEngine
{
namespace Animation
{

// Explicit conversions between glm::quat and Mathematics::Quaternion.
// All retargeting code uses Mathematics::Quaternion exclusively per the
// Appendix A convention; conversions happen only at module boundaries
// (e.g., when GPU constants are uploaded as glm types).
//
// Both libraries use Hamilton multiplication and (x, y, z, w) storage,
// so the conversion is a memberwise copy.

inline glm::quat GlmFromMathematics(const Mathematics::Quaternion& q)
{
    return glm::quat(q.W, q.X, q.Y, q.Z); // glm constructor takes (w, x, y, z)
}

inline Mathematics::Quaternion MathematicsFromGlm(const glm::quat& q)
{
    return Mathematics::Quaternion(q.x, q.y, q.z, q.w);
}

} // namespace Animation
} // namespace GameEngine
