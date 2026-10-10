#pragma once

#include "Mathematics/Types.h"
#include "Types/Types.h"

#include <array>

namespace GameEngine::Ocean
{

/// A view frustum built once from a CameraData view-projection (16 floats), for
/// testing water footprints against it: the passes that light only near water
/// (the underwater portal composite, reflected caustics) declare themselves only
/// when some footprint can be on screen.
class OceanFootprintFrustum
{
public:
    explicit OceanFootprintFrustum(const float* viewProj);

    /// False only when the box lies entirely outside one frustum plane.
    bool Intersects(float minX, float minY, float minZ, float maxX, float maxY, float maxZ) const;

    /// Intersects() for the XZ bounding box of a polygon's first `count` points,
    /// spanning [minY, maxY]; false for an empty polygon.
    bool IntersectsPolygon(const float* xs, const float* zs, uint32 count, float minY,
                           float maxY, float horizontalMargin) const;

    /// True when the band [minY, maxY], unbounded in X and Z, reaches into the
    /// frustum: exact, since the frustum spans the heights of its eight corners.
    /// Always true for a frustum without a finite far plane.
    bool IntersectsHorizontalBand(float minY, float maxY) const;

private:
    std::array<Mathematics::Vector4, 6> m_Planes{};
    float m_MinY = 0.0f;
    float m_MaxY = 0.0f;
    bool m_VerticallyBounded = false;
};

} // namespace GameEngine::Ocean
