#include "TerrainECS/PlanetFaceCollider.h"

#include "CBTTerrain/CBTPlanetShading.h" // PlanetRelief
#include "CBTTerrain/CBTSphereFaceMap.h" // FaceUVToWorldDir (sculpt UV -> world dir)
// SampleSphereSculptComposed comes through the header (SphereAnalyticModifiers.h).

#include <algorithm>
#include <cmath>
#include <limits>

namespace GameEngine::TerrainECS
{
namespace
{

std::array<float32, 3> AxisUnit(uint32 axis, float32 sign)
{
    std::array<float32, 3> v{0.0f, 0.0f, 0.0f};
    v[axis] = sign;
    return v;
}

std::array<float32, 3> Cross(const std::array<float32, 3>& a, const std::array<float32, 3>& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

float32 Dot(const std::array<float32, 3>& a, const std::array<float32, 3>& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// Grid cube-coordinate for index i in [0, dim-1]: 2i/(dim-1) - 1 in [-1, 1].
float32 GridCoord(uint32 dim, int32 i)
{
    const float32 denom = dim > 1u ? static_cast<float32>(dim - 1u) : 1.0f;
    return 2.0f * static_cast<float32>(i) / denom - 1.0f;
}

} // namespace

void ComputePlanetFaceFrame(uint32 face, std::array<float32, 3>& outTa, std::array<float32, 3>& outN,
                            std::array<float32, 3>& outTb)
{
    const uint32 axis = (face >> 1u) % 3u;
    const float32 sign = (face & 1u) == 0u ? 1.0f : -1.0f;
    outN = AxisUnit(axis, sign);
    // A tangent axis perpendicular to N (a different world axis), then Tb = Ta x N makes
    // (Ta, N, Tb) a right-handed frame (Ta x N == Tb). Ta and Tb are the two non-N world
    // axes, so the grid covers exactly the cube face [-1,1]^2 in the tangent plane.
    outTa = AxisUnit((axis + 1u) % 3u, 1.0f);
    outTb = Cross(outTa, outN);
}

std::array<float32, 3> PlanetFaceGridDir(uint32 face, uint32 dim, int32 i, int32 j)
{
    std::array<float32, 3> ta, n, tb;
    ComputePlanetFaceFrame(face, ta, n, tb);
    const float32 ca = GridCoord(dim, i);
    const float32 cb = GridCoord(dim, j);
    std::array<float32, 3> d{n[0] + ta[0] * ca + tb[0] * cb, n[1] + ta[1] * ca + tb[1] * cb,
                             n[2] + ta[2] * ca + tb[2] * cb};
    const float32 len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const float32 inv = len > 0.0f ? 1.0f / len : 0.0f;
    return {d[0] * inv, d[1] * inv, d[2] * inv};
}

void GeneratePlanetFacePatch(uint32 face, const PlanetColliderParams& params, uint32 dim,
                             const CBTTerrain::SphereSculptSampler& sculpt,
                             const CBTTerrain::SphereAnalyticModifierSet& analytic, int32 minI,
                             int32 minJ, int32 maxI, int32 maxJ, std::vector<float32>& samples,
                             float32& outMinH, float32& outMaxH)
{
    outMinH = std::numeric_limits<float32>::max();
    outMaxH = std::numeric_limits<float32>::lowest();
    if (dim < 2u || samples.size() < static_cast<size_t>(dim) * dim)
        return;

    const int32 lastIndex = static_cast<int32>(dim) - 1;
    minI = std::clamp(minI, 0, lastIndex);
    minJ = std::clamp(minJ, 0, lastIndex);
    maxI = std::clamp(maxI, 0, lastIndex);
    maxJ = std::clamp(maxJ, 0, lastIndex);

    std::array<float32, 3> ta, n, tb;
    ComputePlanetFaceFrame(face, ta, n, tb);

    for (int32 j = minJ; j <= maxJ; ++j)
    {
        const float32 cb = GridCoord(dim, j);
        for (int32 i = minI; i <= maxI; ++i)
        {
            const float32 ca = GridCoord(dim, i);
            std::array<float32, 3> d{n[0] + ta[0] * ca + tb[0] * cb, n[1] + ta[1] * ca + tb[1] * cb,
                                     n[2] + ta[2] * ca + tb[2] * cb};
            const float32 len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            const float32 inv = len > 0.0f ? 1.0f / len : 0.0f;
            d = {d[0] * inv, d[1] * inv, d[2] * inv};

            // Composed sample (S2): store + the analytic modifier set, with the closed-form relief
            // at this direction handed in so an analytic flatten cancels it — the CPU mirror of
            // VertexEval's composition (parity oracle: PlanetColliderTests).
            const float32 relief = CBTTerrain::PlanetRelief(d[0], d[1], d[2], params.ReliefAmplitude,
                                                            params.ReliefFrequency,
                                                            params.ReliefOctaves);
            const float32 h =
                relief + CBTTerrain::SampleSphereSculptComposed(sculpt, analytic, d[0], d[1], d[2],
                                                                relief);

            // Surface point S = dir * (Radius + h); the heightfield stores its projection
            // onto the face normal (metres). dot(d, n) is the cosine to the face centre.
            const float32 height = (params.Radius + h) * Dot(d, n);
            samples[static_cast<size_t>(j) * dim + i] = height;
            outMinH = std::min(outMinH, height);
            outMaxH = std::max(outMaxH, height);
        }
    }
}

void PlanetFaceUVRectToGridRect(uint32 face, uint32 dim, float32 minU, float32 minV, float32 maxU,
                                float32 maxV, int32& outMinI, int32& outMinJ, int32& outMaxI,
                                int32& outMaxJ)
{
    std::array<float32, 3> ta, n, tb;
    ComputePlanetFaceFrame(face, ta, n, tb);
    const int32 lastIndex = static_cast<int32>(dim) - 1;
    const float32 span = dim > 1u ? static_cast<float32>(dim - 1u) : 1.0f;

    float32 loI = std::numeric_limits<float32>::max();
    float32 loJ = std::numeric_limits<float32>::max();
    float32 hiI = std::numeric_limits<float32>::lowest();
    float32 hiJ = std::numeric_limits<float32>::lowest();

    const float32 us[2] = {minU, maxU};
    const float32 vs[2] = {minV, maxV};
    for (float32 u : us)
        for (float32 v : vs)
        {
            // Sculpt UV -> world direction -> this patch's gnomonic (ca, cb): project the
            // direction onto the cube plane by dividing the tangent components by the
            // (positive, dominant) face-normal component.
            float32 dx, dy, dz;
            CBTTerrain::FaceUVToWorldDir(face, u, v, dx, dy, dz);
            const std::array<float32, 3> d{dx, dy, dz};
            const float32 dn = Dot(d, n);
            if (dn <= 0.0f)
                continue;
            const float32 ca = Dot(d, ta) / dn; // in [-1, 1] on the face
            const float32 cb = Dot(d, tb) / dn;
            const float32 fi = (ca + 1.0f) * 0.5f * span;
            const float32 fj = (cb + 1.0f) * 0.5f * span;
            loI = std::min(loI, fi);
            hiI = std::max(hiI, fi);
            loJ = std::min(loJ, fj);
            hiJ = std::max(hiJ, fj);
        }

    if (loI > hiI || loJ > hiJ) // all corners rejected (degenerate) — empty rect
    {
        outMinI = outMinJ = 0;
        outMaxI = outMaxJ = -1;
        return;
    }

    // Floor the low edge / ceil the high edge so the grid rect conservatively covers the
    // UV rect (an edit must never under-refresh the collider region).
    outMinI = std::clamp(static_cast<int32>(std::floor(loI)), 0, lastIndex);
    outMinJ = std::clamp(static_cast<int32>(std::floor(loJ)), 0, lastIndex);
    outMaxI = std::clamp(static_cast<int32>(std::ceil(hiI)), 0, lastIndex);
    outMaxJ = std::clamp(static_cast<int32>(std::ceil(hiJ)), 0, lastIndex);
}

} // namespace GameEngine::TerrainECS
