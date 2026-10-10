// Thick gizmo lines: camera-facing quads for widths the line pipeline cannot draw.

#include "SceneView/SceneViewGizmos.h"

#include "Mathematics/VectorOps.h"

#include <cmath>

namespace GameEngine::Editor::SceneTools
{

using Mathematics::Vector3;

namespace
{
constexpr float kThickLineWorldHalfWidthScale = 0.01f; // thickness 2 -> 0.02 world half-width
// Constant-screen-size thick lines: world half-width per unit thickness per unit distance.
constexpr float kScreenThicknessWorldScale = 0.0011f;
constexpr float kDegenerateLengthSq = 1.0e-8f;

// Unit tangent and bitangent for a wire circle around `normal`; false for a
// degenerate normal.
bool BuildCircleBasis(const Vector3& normal, Vector3& outTangent, Vector3& outBitangent)
{
    if (normal.LengthSquared() < kDegenerateLengthSq)
        return false;
    BuildPlaneBasis(normal.NormalizeOrZero(), outTangent, outBitangent);
    return true;
}

// Point `index` of a `segments`-segment circle; index `segments` closes it.
Vector3 CirclePoint(const Vector3& center,
                    const Vector3& tangent,
                    const Vector3& bitangent,
                    float radius,
                    int index,
                    int segments)
{
    const float t = static_cast<float>(index) / static_cast<float>(segments);
    const float a = t * 2.0f * Mathematics::Pi;
    const float cs = std::cos(a);
    const float sn = std::sin(a);
    return center + tangent * (radius * cs) + bitangent * (radius * sn);
}
} // namespace

bool BuildThickLineQuad(const GizmoRenderContext& context,
                        const Mathematics::Vector3& a,
                        const Mathematics::Vector3& b,
                        float thickness,
                        bool constantScreenSpaceWidth,
                        bool smartDistanceScaling,
                        std::array<Mathematics::Vector3, 6>& outVertices)
{
    const Vector3* camPos = context.GetCameraWorldPosition();
    if (!camPos)
        return false;

    const Vector3 seg = b - a;
    if (seg.LengthSquared() < kDegenerateLengthSq)
        return false;

    // Compute a screen-aligned perpendicular: cross(segDir, viewDir) gives a
    // vector that lies in the segment plane and is perpendicular to the view
    // direction, which makes the quad face the camera.
    const Vector3 mid = (a + b) * 0.5f;
    const Vector3 view = mid - *camPos;
    Vector3 perp = Vector3::Cross(seg, view);
    const float perpLenSq = perp.LengthSquared();
    if (perpLenSq < kDegenerateLengthSq)
        return false;
    perp = perp * (1.0f / std::sqrt(perpLenSq));

    // In ortho mode (2D view, ortho 3D), camera-to-segment distance no longer
    // controls projected scale — the orthographic visible height does. Use
    // that as the effective "distance" so thickness stays at a constant pixel
    // size across zoom levels (otherwise zooming in inflates the world half-
    // width, producing the staircased / pixelated look on the marquee).
    const float dist = context.HasOrthoHeight()
        ? context.GetOrthoHeight() * kOrthoEffectiveDistanceFactor
        : view.Length();

    float worldHalfWidth;
    if (constantScreenSpaceWidth)
    {
        // Approximate world-space half width for a stable on-screen thickness.
        worldHalfWidth = smartDistanceScaling
            ? thickness * kScreenThicknessWorldScale * std::sqrt(dist * kGizmoSmartDistanceReference)
            : thickness * kScreenThicknessWorldScale * dist;
    }
    else
    {
        worldHalfWidth = thickness * kThickLineWorldHalfWidthScale;
    }

    const Vector3 offset = perp * worldHalfWidth;
    const Vector3 p0 = a - offset;
    const Vector3 p1 = a + offset;
    const Vector3 p2 = b + offset;
    const Vector3 p3 = b - offset;
    outVertices = {p0, p1, p2, p0, p2, p3};
    return true;
}

void DrawThickLine(GizmoRenderContext& context,
                   const Mathematics::Vector3& a,
                   const Mathematics::Vector3& b,
                   const Color& color,
                   float thickness,
                   bool constantScreenSpaceWidth,
                   bool smartDistanceScaling)
{
    // Thin lines: keep the line-pipeline path so single-pixel strokes still
    // share groups with other gizmo line draws.
    if (thickness > kThinLineThickness)
    {
        if ((b - a).LengthSquared() < kDegenerateLengthSq)
            return;
        std::array<Vector3, 6> quad;
        if (BuildThickLineQuad(context, a, b, thickness, constantScreenSpaceWidth, smartDistanceScaling, quad))
        {
            context.DrawTriangles(quad.data(), quad.size(), color);
            return;
        }
    }

    context.DrawColoredLine(a, b, color, thickness);
}

void AppendThickLineTriangles(const GizmoRenderContext& context,
                              const Mathematics::Vector3& a,
                              const Mathematics::Vector3& b,
                              float thickness,
                              bool constantScreenSpaceWidth,
                              bool smartDistanceScaling,
                              std::vector<Mathematics::Vector3>& outVertices)
{
    if (thickness <= kThinLineThickness)
        return;

    std::array<Vector3, 6> quad;
    if (!BuildThickLineQuad(context, a, b, thickness, constantScreenSpaceWidth, smartDistanceScaling, quad))
        return;
    outVertices.insert(outVertices.end(), quad.begin(), quad.end());
}

void DrawThickWireCircle(GizmoRenderContext& context,
                         const Mathematics::Vector3& center,
                         const Mathematics::Vector3& normal,
                         float radius,
                         const Color& color,
                         float thickness,
                         bool constantScreenSpaceLineWidth,
                         bool smartDistanceScaling,
                         int segments)
{
    if (radius <= 0.0f || segments < 3)
        return;
    Vector3 tangent;
    Vector3 bitangent;
    if (!BuildCircleBasis(normal, tangent, bitangent))
        return;

    Vector3 prev = CirclePoint(center, tangent, bitangent, radius, 0, segments);
    for (int i = 1; i <= segments; ++i)
    {
        const Vector3 p = CirclePoint(center, tangent, bitangent, radius, i, segments);
        DrawThickLine(context, prev, p, color, thickness, constantScreenSpaceLineWidth, smartDistanceScaling);
        prev = p;
    }
}

void AppendThickWireCircleTriangles(const GizmoRenderContext& context,
                                    const Mathematics::Vector3& center,
                                    const Mathematics::Vector3& normal,
                                    float radius,
                                    float thickness,
                                    bool constantScreenSpaceLineWidth,
                                    bool smartDistanceScaling,
                                    std::vector<Mathematics::Vector3>& outVertices,
                                    int segments)
{
    if (radius <= 0.0f || segments < 3)
        return;
    Vector3 tangent;
    Vector3 bitangent;
    if (!BuildCircleBasis(normal, tangent, bitangent))
        return;

    Vector3 prev = CirclePoint(center, tangent, bitangent, radius, 0, segments);
    for (int i = 1; i <= segments; ++i)
    {
        const Vector3 p = CirclePoint(center, tangent, bitangent, radius, i, segments);
        AppendThickLineTriangles(context, prev, p, thickness, constantScreenSpaceLineWidth,
                                 smartDistanceScaling, outVertices);
        prev = p;
    }
}

} // namespace GameEngine::Editor::SceneTools
