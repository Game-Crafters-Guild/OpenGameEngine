#pragma once

#include "Mathematics/Quaternion.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace GameEngine
{
namespace Animation
{

// Header-only quaternion helpers used across the retarget pipeline.
// Per Appendix A of the Humanoid Retargeting plan: every slerp / lerp call
// in the pipeline must apply the antipode flip (negate one operand when the
// dot product is negative) so that the interpolation always travels the
// short way on the unit hypersphere. Calling `glm::slerp` or
// `Mathematics::Quaternion::Slerp` directly is forbidden inside Retarget*
// code; route through `SlerpShort` here so any future fix lands once.

inline float Dot(const Mathematics::Quaternion& a, const Mathematics::Quaternion& b)
{
    return glm::dot(a.GetGLM(), b.GetGLM());
}

inline Mathematics::Quaternion Negate(const Mathematics::Quaternion& q)
{
    const glm::quat& g = q.GetGLM();
    return Mathematics::Quaternion(glm::quat(-g.w, -g.x, -g.y, -g.z));
}

// Antipode-flipped slerp. If dot(a,b) < 0, flips b to its hemisphere
// equivalent before slerping, so the path is always the short rotation.
// `t` is clamped to [0, 1]. Result is renormalized.
inline Mathematics::Quaternion SlerpShort(const Mathematics::Quaternion& a,
                                          const Mathematics::Quaternion& b,
                                          float t)
{
    const float tt = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
    glm::quat ga = a.GetGLM();
    glm::quat gb = b.GetGLM();
    if (glm::dot(ga, gb) < 0.0f)
        gb = glm::quat(-gb.w, -gb.x, -gb.y, -gb.z);
    return Mathematics::Quaternion(glm::normalize(glm::slerp(ga, gb, tt)));
}

// Antipode-flipped normalized lerp. Use for tight inner loops where slerp's
// trig cost is wasted; result direction matches slerp within ~0.5° for
// |angle| < 60°.
inline Mathematics::Quaternion LerpShortNormalized(const Mathematics::Quaternion& a,
                                                   const Mathematics::Quaternion& b,
                                                   float t)
{
    const float tt = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
    glm::quat ga = a.GetGLM();
    glm::quat gb = b.GetGLM();
    if (glm::dot(ga, gb) < 0.0f)
        gb = glm::quat(-gb.w, -gb.x, -gb.y, -gb.z);
    glm::quat r = glm::lerp(ga, gb, tt);
    return Mathematics::Quaternion(glm::normalize(r));
}

// Inverse of a unit quaternion (== conjugate). Wrapper used for clarity at
// retarget sites where the math reads "inverse(qParent) * ...".
inline Mathematics::Quaternion Inverse(const Mathematics::Quaternion& q)
{
    return q.Conjugated();
}

// Angle between two unit quaternions in radians (always non-negative).
// Uses the antipode-folded `2 * atan2(|cross|, |dot|)` formulation, which is
// numerically stable near zero angle (acos(1-eps) loses precision dramatically).
inline float AngleBetweenRadians(const Mathematics::Quaternion& a,
                                 const Mathematics::Quaternion& b)
{
    const glm::quat& ga = a.GetGLM();
    const glm::quat& gb = b.GetGLM();
    // Difference quaternion d = b * conj(a). The rotation angle of d is the
    // angle between a and b. Antipode-fold by taking |d.w|.
    const glm::quat d = gb * glm::conjugate(ga);
    const float vmag = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    const float w    = std::fabs(d.w);
    return 2.0f * std::atan2(vmag, w);
}

inline float AngleBetweenDegrees(const Mathematics::Quaternion& a,
                                 const Mathematics::Quaternion& b)
{
    constexpr float kRadToDeg = 57.29577951308232f;
    return AngleBetweenRadians(a, b) * kRadToDeg;
}

} // namespace Animation
} // namespace GameEngine
