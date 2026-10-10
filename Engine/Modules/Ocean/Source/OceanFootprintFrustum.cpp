#include "Ocean/OceanFootprintFrustum.h"

#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Common/Frustum.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace GameEngine::Ocean
{

OceanFootprintFrustum::OceanFootprintFrustum(const float* viewProj)
{
    Mathematics::Matrix4x4 vp{};
    std::memcpy(vp.Data(), viewProj, sizeof(float) * 16);
    Rendering::ExtractFrustumPlanes(vp, m_Planes.data());

    // The corners are the clip-space cube's, depth 0 to 1, mapped back to world
    // space. A corner at infinity (w = 0, an infinite far plane) leaves the
    // frustum's height unbounded.
    const Mathematics::Matrix4x4 inverse = Mathematics::Inverse(vp);
    constexpr float kMinCornerW = 1e-6f;
    m_MinY = std::numeric_limits<float>::max();
    m_MaxY = std::numeric_limits<float>::lowest();
    m_VerticallyBounded = true;
    for (const float x : {-1.0f, 1.0f})
        for (const float y : {-1.0f, 1.0f})
            for (const float depth : {0.0f, 1.0f})
            {
                const Mathematics::Vector4 corner = inverse.Transform(Mathematics::Vector4(x, y, depth, 1.0f));
                if (std::abs(corner.w) < kMinCornerW)
                {
                    m_VerticallyBounded = false;
                    return;
                }
                const float worldY = corner.y / corner.w;
                m_MinY = std::min(m_MinY, worldY);
                m_MaxY = std::max(m_MaxY, worldY);
            }
}

bool OceanFootprintFrustum::IntersectsHorizontalBand(float minY, float maxY) const
{
    return !m_VerticallyBounded || (maxY >= m_MinY && minY <= m_MaxY);
}

bool OceanFootprintFrustum::Intersects(float minX, float minY, float minZ, float maxX, float maxY,
                                       float maxZ) const
{
    return Rendering::TestAabbFrustum(Mathematics::Vector3(minX, minY, minZ),
                                      Mathematics::Vector3(maxX, maxY, maxZ), m_Planes.data());
}

bool OceanFootprintFrustum::IntersectsPolygon(const float* xs, const float* zs, uint32 count,
                                              float minY, float maxY,
                                              float horizontalMargin) const
{
    if (count == 0u)
        return false;
    float minX = xs[0], maxX = xs[0], minZ = zs[0], maxZ = zs[0];
    for (uint32 i = 1u; i < count; ++i)
    {
        minX = std::min(minX, xs[i]);
        maxX = std::max(maxX, xs[i]);
        minZ = std::min(minZ, zs[i]);
        maxZ = std::max(maxZ, zs[i]);
    }
    return Intersects(minX - horizontalMargin, minY, minZ - horizontalMargin,
                      maxX + horizontalMargin, maxY, maxZ + horizontalMargin);
}

} // namespace GameEngine::Ocean
