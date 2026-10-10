
#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "Mathematics/Vector3.h"

namespace GameEngine
{
namespace Mathematics
{

// Quaternion wrapper over glm::quat for stable engine API.
struct Quaternion
{
  private:
    glm::quat m_Quat;

  public:
    Quaternion() : m_Quat(1.0f, 0.0f, 0.0f, 0.0f) {} // identity
    Quaternion(float w, float x, float y, float z) : m_Quat(w, x, y, z) {}
    Quaternion(const glm::quat& q) : m_Quat(q) {}

    static Quaternion Identity() { return Quaternion(glm::quat(1.0f, 0.0f, 0.0f, 0.0f)); }

    glm::quat& GetGLM() { return m_Quat; }
    const glm::quat& GetGLM() const { return m_Quat; }

    operator glm::quat&() { return m_Quat; }
    operator const glm::quat&() const { return m_Quat; }

    Quaternion operator*(const Quaternion& rhs) const
    {
        return Quaternion(m_Quat * rhs.m_Quat);
    }

    Quaternion Normalized() const
    {
        return Quaternion(glm::normalize(m_Quat));
    }

    static Quaternion Slerp(const Quaternion& a, const Quaternion& b, float t)
    {
        const float tt = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
        return Quaternion(glm::normalize(glm::slerp(a.m_Quat, b.m_Quat, tt)));
    }

    // Conjugate of the quaternion. For unit quaternions this is also the inverse.
    Quaternion Conjugated() const
    {
        return Quaternion(glm::conjugate(m_Quat));
    }

    static Quaternion FromAxisAngle(const Vector3& axis, float angleRadians)
    {
        glm::vec3 a(axis.x, axis.y, axis.z);
        return Quaternion(glm::angleAxis(angleRadians, a));
    }

    Vector3 Rotate(const Vector3& v) const
    {
        glm::vec3 gv(v.x, v.y, v.z);
        glm::vec3 r = m_Quat * gv;
        return Vector3(r.x, r.y, r.z);
    }
};

} // namespace Mathematics
} // namespace GameEngine
