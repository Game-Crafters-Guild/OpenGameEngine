#pragma once

#include <cmath>

namespace GameEngine
{
namespace Engine::Renderer
{

inline bool IsOrthographicProjectionLH_ZO(const float proj[16])
{
    return std::abs(proj[15] - 1.0f) < 1e-4f && std::abs(proj[11]) < 1e-4f;
}

// Extract near and far planes from a left-handed [1,0] reverse-Z projection
// matrix. `proj` is column-major 4x4. Supports the engine's finite perspective
// and orthographic projection builders, and falls back to sensible defaults on
// degenerate input.
//
// Reverse-Z LH ZO finite (MakePerspectiveLH_ZO_ReverseZ):
//   m22 = -near/(far-near)        → col 2, row 2 → index 10
//   m32 = (near*far)/(far-near)   → col 3, row 2 → index 14
// =>
//   1 - m22 = (far-near)/(far-near) + near/(far-near) = far/(far-near)
//   m32/(1 - m22) = near
//   -m32/m22 = far
inline void ExtractNearFarLH_ZO(const float proj[16], float& outNear, float& outFar)
{
    const float m22 = proj[10];
    const float m32 = proj[14];

    float zn = 0.1f;
    float zf = 1000.0f;

    if (IsOrthographicProjectionLH_ZO(proj))
    {
        // Reverse-Z orthographic:
        //   m22 = -1/(far-near), m32 = far/(far-near)
        const float range = -1.0f / m22;
        zf = m32 * range;
        zn = zf - range;
    }
    else if (std::abs(m22) > 1e-6f && std::abs(1.0f - m22) > 1e-6f)
    {
        // Reverse-Z perspective:
        //   m22 = -near/(far-near), m32 = near*far/(far-near)
        zn = m32 / (1.0f - m22);
        zf = -m32 / m22;
    }

    if (!(zn > 0.0f) || !std::isfinite(zn))
        zn = 0.1f;
    if (!(zf > zn) || !std::isfinite(zf))
        zf = zn + 1000.0f;

    outNear = zn;
    outFar = zf;
}

inline float LinearizeReverseZDepthLH_ZO(float ndcDepth, float nearPlane, float farPlane, bool orthographic)
{
    if (orthographic)
        return farPlane - ndcDepth * (farPlane - nearPlane);

    return (nearPlane * farPlane) / (ndcDepth * (farPlane - nearPlane) + nearPlane);
}

} // namespace Engine::Renderer
} // namespace GameEngine
