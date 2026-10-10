#include "SceneView/LocalVolumeGizmo.h"

#include <algorithm>
#include <cmath>

#include "Components/Transform.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector3.h"
#include "SceneView/SceneViewGizmos.h"
#include "Types/Color.h"

namespace GameEngine::Editor::SceneTools
{

using GameEngine::Components::WorldTransform;
using GameEngine::Mathematics::Vector3;

namespace
{

constexpr int kEllipseSegments = 48;

void DrawWireCapsuleOrCylinder(GizmoRenderContext& ctx,
                               const Vector3& center,
                               const Vector3& ux,
                               const Vector3& uy,
                               const Vector3& uz,
                               float radius,
                               float halfHeight,
                               bool roundedCaps,
                               const Color& color,
                               float thickness)
{
    const float axisCore = std::max(0.0f, halfHeight - (roundedCaps ? radius : 0.0f));
    const Vector3 top = center + uy * axisCore;
    const Vector3 bottom = center - uy * axisCore;

    GizmoLineBatch batch(ctx, color, thickness);
    DrawCircleInto(batch, top, ux, radius, uz, radius, kEllipseSegments);
    DrawCircleInto(batch, bottom, ux, radius, uz, radius, kEllipseSegments);

    const Vector3 px = ux * radius;
    const Vector3 pz = uz * radius;
    batch.AddLine(top + px, bottom + px);
    batch.AddLine(top - px, bottom - px);
    batch.AddLine(top + pz, bottom + pz);
    batch.AddLine(top - pz, bottom - pz);

    if (roundedCaps)
    {
        DrawCircleInto(batch, top, ux, radius, uy, radius, kEllipseSegments);
        DrawCircleInto(batch, top, uz, radius, uy, radius, kEllipseSegments);
        DrawCircleInto(batch, bottom, ux, radius, uy, radius, kEllipseSegments);
        DrawCircleInto(batch, bottom, uz, radius, uy, radius, kEllipseSegments);
    }
}

constexpr float kThickness = 1.5f;

} // namespace

void DrawLocalVolume(GizmoRenderContext& context,
                     const WorldTransform& xf,
                     VolumeShape shape,
                     float blendDistance,
                     const Color& innerColor,
                     const Color& blendColor)
{
    // Per-axis scale = WorldTransform column lengths.
    const float* m = xf.matrix;
    const Vector3 ax(m[0], m[1], m[2]);
    const Vector3 ay(m[4], m[5], m[6]);
    const Vector3 az(m[8], m[9], m[10]);
    const float sx = std::sqrt(ax.LengthSquared());
    const float sy = std::sqrt(ay.LengthSquared());
    const float sz = std::sqrt(az.LengthSquared());
    if (sx <= 0.0f || sy <= 0.0f || sz <= 0.0f)
        return;

    const float blend = blendDistance > 0.0f ? blendDistance : 0.0f;

    // Orthonormal basis (axes / scale) shared by every shape.
    const Vector3 ux = ax / sx;
    const Vector3 uy = ay / sy;
    const Vector3 uz = az / sz;
    const float hx = sx * 0.5f;
    const float hy = sy * 0.5f;
    const float hz = sz * 0.5f;
    const Vector3 center(m[12], m[13], m[14]);

    if (shape == VolumeShape::Sphere)
    {
        DrawWireEllipsoid(context, center, ux, uy, uz, Vector3(hx, hy, hz), innerColor, kThickness,
                          kEllipseSegments);
        if (blend > 0.0f)
            DrawWireEllipsoid(context, center, ux, uy, uz, Vector3(hx + blend, hy + blend, hz + blend), blendColor,
                              kThickness, kEllipseSegments);
        return;
    }
    if (shape == VolumeShape::Capsule || shape == VolumeShape::Cylinder)
    {
        const float radius = std::max(hx, hz);
        const bool roundedCaps = shape == VolumeShape::Capsule;
        DrawWireCapsuleOrCylinder(context, center, ux, uy, uz, radius, hy, roundedCaps, innerColor, kThickness);
        if (blend > 0.0f)
            DrawWireCapsuleOrCylinder(context, center, ux, uy, uz, radius + blend, hy + blend, roundedCaps, blendColor, kThickness);
        return;
    }

    Mathematics::Matrix4x4 rotOnly;
    rotOnly[0] = {ux.x, ux.y, ux.z, 0.0f};
    rotOnly[1] = {uy.x, uy.y, uy.z, 0.0f};
    rotOnly[2] = {uz.x, uz.y, uz.z, 0.0f};
    rotOnly[3] = {center.x, center.y, center.z, 1.0f};
    context.DrawTransformedWireBox(Vector3(-hx, -hy, -hz), Vector3(hx, hy, hz), rotOnly, innerColor, kThickness);

    if (blend > 0.0f)
    {
        const Vector3 outerMin(-(hx + blend), -(hy + blend), -(hz + blend));
        const Vector3 outerMax(hx + blend, hy + blend, hz + blend);
        context.DrawTransformedWireBox(outerMin, outerMax, rotOnly, blendColor, kThickness);
    }
}

} // namespace GameEngine::Editor::SceneTools
