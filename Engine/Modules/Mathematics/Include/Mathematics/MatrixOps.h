#pragma once

#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Mathematics/Types.h"

namespace GameEngine {
namespace Mathematics {

// Common matrix construction helpers built on top of GLM, unified here so the
// engine can share a single set of conventions (left-handed, Z+ forward).

inline Matrix4x4 MakeTranslation(const Vector3& translation)
{
    glm::vec3 t(translation.x, translation.y, translation.z);
    glm::mat4 m(1.0f);
    m = glm::translate(m, t);
    return Matrix4x4(m);
}

inline Matrix4x4 MakeScale(const Vector3& scale)
{
    glm::vec3 s(scale.x, scale.y, scale.z);
    glm::mat4 m(1.0f);
    m = glm::scale(m, s);
    return Matrix4x4(m);
}

// Rotation around principal axes (angles in radians).
inline Matrix4x4 MakeRotationX(float angle)
{
    return Matrix4x4(glm::rotate(glm::mat4(1.0f), angle, glm::vec3(1.0f, 0.0f, 0.0f)));
}

inline Matrix4x4 MakeRotationY(float angle)
{
    return Matrix4x4(glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0.0f, 1.0f, 0.0f)));
}

inline Matrix4x4 MakeRotationZ(float angle)
{
    return Matrix4x4(glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0.0f, 0.0f, 1.0f)));
}

// Left-handed, Z+ forward view matrix.
inline Matrix4x4 MakeLookAtLH(const Vector3& eye,
                              const Vector3& target,
                              const Vector3& up)
{
    glm::vec3 e(eye.x, eye.y, eye.z);
    glm::vec3 c(target.x, target.y, target.z);
    glm::vec3 u(up.x, up.y, up.z);
    return Matrix4x4(glm::lookAtLH(e, c, u));
}

// Left-handed, depth [0,1] forward-Z perspective projection.
// NOTE: the engine is reverse-Z by default. Forward-Z combined with the default
// CompareOp::GreaterOrEqual depth state will reject every fragment. Use
// MakePerspectiveLH_ZO_ReverseZ unless you have a deliberate reason to deviate.
[[deprecated("Engine defaults to reverse-Z; use MakePerspectiveLH_ZO_ReverseZ.")]]
inline Matrix4x4 MakePerspectiveLH_ZO(float fovYRadians,
                                      float aspect,
                                      float zNear,
                                      float zFar)
{
    return Matrix4x4(glm::perspectiveLH_ZO(fovYRadians, aspect, zNear, zFar));
}

// Left-handed, depth [1,0] reverse-Z perspective projection.
// Maps zNear -> NDC depth 1.0, zFar -> NDC depth 0.0. Combined with a float
// depth buffer this gives near-uniform depth precision across the frustum.
inline Matrix4x4 MakePerspectiveLH_ZO_ReverseZ(float fovYRadians,
                                               float aspect,
                                               float zNear,
                                               float zFar)
{
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);
    const float invRange = 1.0f / (zFar - zNear);
    glm::mat4 m(0.0f);
    m[0][0] = f / aspect;
    m[1][1] = f;
    m[2][2] = -zNear * invRange;        // -near/(far-near)
    m[2][3] = 1.0f;                     // LH: w_clip = z_view
    m[3][2] = (zNear * zFar) * invRange;
    return Matrix4x4(m);
}

// Left-handed, depth [0,1] forward-Z orthographic projection.
// NOTE: the engine is reverse-Z by default. Use MakeOrthographicLH_ZO_ReverseZ
// unless you have a deliberate reason to deviate.
[[deprecated("Engine defaults to reverse-Z; use MakeOrthographicLH_ZO_ReverseZ.")]]
inline Matrix4x4 MakeOrthographicLH_ZO(float left,
                                       float right,
                                       float bottom,
                                       float top,
                                       float zNear,
                                       float zFar)
{
    return Matrix4x4(glm::orthoLH_ZO(left, right, bottom, top, zNear, zFar));
}

// Left-handed, depth [1,0] reverse-Z orthographic projection.
// Maps zNear -> NDC depth 1.0, zFar -> NDC depth 0.0. Hand-built because
// glm::orthoLH_ZO does not produce a reverse-Z matrix when its near/far args
// are swapped (the resulting matrix is mathematically wrong).
inline Matrix4x4 MakeOrthographicLH_ZO_ReverseZ(float left,
                                                float right,
                                                float bottom,
                                                float top,
                                                float zNear,
                                                float zFar)
{
    glm::mat4 m(1.0f);
    m[0][0] = 2.0f / (right - left);
    m[1][1] = 2.0f / (top - bottom);
    m[2][2] = -1.0f / (zFar - zNear);
    m[3][0] = -(right + left) / (right - left);
    m[3][1] = -(top + bottom) / (top - bottom);
    m[3][2] = zFar / (zFar - zNear);
    m[3][3] = 1.0f;
    return Matrix4x4(m);
}

// Basic linear algebra helpers.
inline Matrix4x4 Transpose(const Matrix4x4& m)
{
    return Matrix4x4(glm::transpose(m.GetGLM()));
}

inline Matrix4x4 Inverse(const Matrix4x4& m)
{
    return Matrix4x4(glm::inverse(m.GetGLM()));
}

} // namespace Mathematics
} // namespace GameEngine

