#include "Placement/PlacementGroundGap.h"

#include "Components/HierarchyQueries.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Transform.h"
#include "ECS/World.h"
#include "TerrainECS/PlanarHeightQuery.h"

#include <algorithm>
#include <limits>

namespace GameEngine::Editor
{

namespace
{

using Mathematics::Vector3;

// How far off horizontal the underside may lean before its height above a world
// XZ stops being a measurement: cos(87 degrees). At that lean a centimetre of XZ
// is nineteen centimetres of height, so a gap would report where the probe was
// put rather than how the piece sits.
constexpr float32 kMinUndersideNormalYCosine = 0.05f;

// A world matrix column scaled by the box's half extent along it — the oriented
// box's half-axis, carrying rotation, scale, mirroring and shear alike.
Vector3 HalfAxis(const float32 m[16], uint32 column, float32 halfExtent)
{
    return Vector3{m[column * 4u] * halfExtent, m[column * 4u + 1u] * halfExtent,
                   m[column * 4u + 2u] * halfExtent};
}

Vector3 TransformPoint(const float32 m[16], const Vector3& p)
{
    return Vector3{m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
                   m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                   m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
}

} // namespace

void MeasurePieceGroundGap(const TerrainECS::PlanarHeightQuery& ground,
                           const Mathematics::BoundingBox& localBox,
                           const float32 worldMatrix[16], PieceGroundGap& out)
{
    out.Status = GroundGapStatus::NoGroundSampled;
    out.UndersideCenterY = 0.0f;
    out.Probes = {};
    out.MinGap = 0.0f;
    out.MaxGap = 0.0f;
    out.CenterGap = 0.0f;
    out.ProbesSampled = 0u;
    out.ProbesRequested = 0u;

    const Vector3 center = TransformPoint(worldMatrix, localBox.center);
    const Vector3 halfX = HalfAxis(worldMatrix, 0u, localBox.halfExtents.x);
    const Vector3 halfY = HalfAxis(worldMatrix, 1u, localBox.halfExtents.y);
    const Vector3 halfZ = HalfAxis(worldMatrix, 2u, localBox.halfExtents.z);

    // The underside's plane normal is the cross product of the two half-axes that
    // span it — the actual edge vectors, so a non-uniform scale needs no inverse
    // transpose and a mirrored transform needs no special case.
    const Vector3 normal = Vector3::Cross(halfX, halfZ);
    const float32 normalLengthSq = Vector3::Dot(normal, normal);
    if (normalLengthSq <= 0.0f)
    {
        out.Status = GroundGapStatus::DegenerateFootprint;
        return;
    }
    if (normal.y * normal.y <=
        kMinUndersideNormalYCosine * kMinUndersideNormalYCosine * normalLengthSq)
    {
        out.Status = GroundGapStatus::UndersideNearVertical;
        return;
    }

    // Which local-Y face is the underside: the one that lies below the other at
    // EVERY XZ. Both faces are offsets of the box's mid-plane, so the sign of the
    // vertical offset to the +Y face names the lower one — and it names it
    // correctly for a mirrored transform, where the authored bottom face points
    // up.
    const float32 verticalOffsetToPositiveFace = Vector3::Dot(normal, halfY) / normal.y;
    const Vector3 undersideCenter =
        verticalOffsetToPositiveFace >= 0.0f ? center - halfY : center + halfY;

    // Probe 0 is the underside centre; 1..4 are its corners. Each probe is a
    // VERTEX of the underside quad (or its centre), so its height is the point's
    // own Y — nothing is extrapolated off the quad, and every probe XZ lies
    // inside the piece's true footprint.
    const Vector3 probePoints[kGroundGapProbeCount] = {
        undersideCenter, undersideCenter - halfX - halfZ, undersideCenter + halfX - halfZ,
        undersideCenter - halfX + halfZ, undersideCenter + halfX + halfZ};

    out.UndersideCenterY = undersideCenter.y;
    out.ProbesRequested = kGroundGapProbeCount;

    float32 minGap = std::numeric_limits<float32>::max();
    float32 maxGap = std::numeric_limits<float32>::lowest();
    for (uint32 i = 0u; i < kGroundGapProbeCount; ++i)
    {
        GroundGapProbe& probe = out.Probes[i];
        probe.X = probePoints[i].x;
        probe.Z = probePoints[i].z;
        probe.UndersideY = probePoints[i].y;

        float32 groundY = 0.0f;
        if (!ground.SampleHeight(probe.X, probe.Z, groundY))
            continue;

        probe.GroundY = groundY;
        probe.Gap = probe.UndersideY - groundY;
        probe.Sampled = true;
        minGap = std::min(minGap, probe.Gap);
        maxGap = std::max(maxGap, probe.Gap);
        if (i == 0u)
            out.CenterGap = probe.Gap;
        ++out.ProbesSampled;
    }

    if (out.ProbesSampled == 0u)
        return;

    out.Status = GroundGapStatus::Measured;
    out.MinGap = minGap;
    out.MaxGap = maxGap;
}

void MeasureRouteGroundGaps(ECS::World& world, ECS::EntityHandle route,
                            const TerrainECS::PlanarHeightQuery& ground,
                            std::vector<PieceGroundGap>& out)
{
    out.clear();

    std::vector<ECS::EntityHandle> children;
    Components::ChildrenOf(world, route, children);

    for (const ECS::EntityHandle child : children)
    {
        const auto* bounds = world.GetComponent<Components::LocalBounds>(child);
        const auto* worldTransform = world.GetComponent<Components::WorldTransform>(child);
        if (!bounds || !worldTransform)
            continue;

        PieceGroundGap gap{};
        gap.Piece = child;
        MeasurePieceGroundGap(ground, bounds->Box, worldTransform->matrix, gap);
        out.push_back(gap);
    }
}

} // namespace GameEngine::Editor
