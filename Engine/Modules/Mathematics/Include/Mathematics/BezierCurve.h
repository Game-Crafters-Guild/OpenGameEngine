#pragma once

#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"

#include <vector>

namespace GameEngine::Math
{

using Mathematics::Vector2;
using Mathematics::Vector3;

// Evaluate a point on a cubic bezier curve at parameter t in [0,1].
inline Vector2 CubicBezier(Vector2 p0, Vector2 p1, Vector2 p2, Vector2 p3, float t)
{
    float u = 1.0f - t;
    float u2 = u * u;
    float u3 = u2 * u;
    float t2 = t * t;
    float t3 = t2 * t;
    return p0 * u3 + p1 * (3.0f * u2 * t) + p2 * (3.0f * u * t2) + p3 * t3;
}

inline Vector3 CubicBezier(Vector3 p0, Vector3 p1, Vector3 p2, Vector3 p3, float t)
{
    float u = 1.0f - t;
    float u2 = u * u;
    float u3 = u2 * u;
    float t2 = t * t;
    float t3 = t2 * t;
    return p0 * u3 + p1 * (3.0f * u2 * t) + p2 * (3.0f * u * t2) + p3 * t3;
}

inline void SubdivideCubicBezier(Vector2 p0, Vector2 p1, Vector2 p2, Vector2 p3,
                                 std::vector<Vector2>& outPoints, int segments = 32)
{
    outPoints.reserve(outPoints.size() + static_cast<size_t>(segments) + 1);
    for (int i = 0; i <= segments; ++i)
    {
        float t = static_cast<float>(i) / static_cast<float>(segments);
        outPoints.push_back(CubicBezier(p0, p1, p2, p3, t));
    }
}

// Tangent (first derivative) of a cubic Bezier curve at parameter t.
inline Vector3 CubicBezierTangent(Vector3 p0, Vector3 p1, Vector3 p2, Vector3 p3, float t)
{
    float u = 1.0f - t;
    float u2 = u * u;
    float t2 = t * t;
    return (p1 - p0) * (3.0f * u2) + (p2 - p1) * (6.0f * u * t) + (p3 - p2) * (3.0f * t2);
}

// Catmull-Rom spline evaluation between p1 and p2, using p0 and p3 as context points.
// Parameter t in [0,1] interpolates from p1 (t=0) to p2 (t=1).
inline Vector3 CatmullRom(Vector3 p0, Vector3 p1, Vector3 p2, Vector3 p3, float t)
{
    float t2 = t * t;
    float t3 = t2 * t;
    return p0 * (-0.5f * t3 + t2 - 0.5f * t) +
           p1 * (1.5f * t3 - 2.5f * t2 + 1.0f) +
           p2 * (-1.5f * t3 + 2.0f * t2 + 0.5f * t) +
           p3 * (0.5f * t3 - 0.5f * t2);
}

// Tangent of a Catmull-Rom spline at parameter t.
inline Vector3 CatmullRomTangent(Vector3 p0, Vector3 p1, Vector3 p2, Vector3 p3, float t)
{
    float t2 = t * t;
    return p0 * (-1.5f * t2 + 2.0f * t - 0.5f) +
           p1 * (4.5f * t2 - 5.0f * t) +
           p2 * (-4.5f * t2 + 4.0f * t + 0.5f) +
           p3 * (1.5f * t2 - 1.0f * t);
}

} // namespace GameEngine::Math
