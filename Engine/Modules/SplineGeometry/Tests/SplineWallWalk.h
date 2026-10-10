#pragma once

// Station walks, wall profiles and mesh probes shared by the SplineGeometry
// tests that build walls through the corner, face-turn and step passes.

#include "SplineGeometry/SplineCorner.h"
#include "SplineGeometry/SplineProfile.h"
#include "SplineGeometry/SplineStation.h"
#include "SplineGeometry/SplineStripBuilder.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace GameEngine::SplineGeometry::Testing
{

using V3 = Mathematics::Vector3;

constexpr float32 kPi = 3.14159265358979323846f;
inline const V3 kUp{0.0f, 1.0f, 0.0f};

inline V3 Normalize(const V3& v)
{
    return v * (1.0f / std::sqrt(V3::Dot(v, v)));
}

inline float32 Length(const V3& v)
{
    return std::sqrt(V3::Dot(v, v));
}

// A level polyline walked at `step` metres the way the extrude samples a Linear
// spline once its authored points are stations: every authored point lands on a
// station, and each station looks along the central difference of its
// neighbours. Right is Cross(up, forward), the right of travel in this LH +Y-up
// engine.
struct Walk
{
    std::vector<SplineStripStation> Stations;
    std::vector<SplineCornerSite> Corners;
};

inline Walk WalkPolyline(const std::vector<V3>& points, float32 step, float32 halfWidth)
{
    Walk walk;
    std::vector<V3> positions;
    for (size_t leg = 0; leg + 1u < points.size(); ++leg)
    {
        const V3 from = points[leg];
        const V3 to = points[leg + 1u];
        const uint32 pieces = std::max(1u, static_cast<uint32>(std::ceil(Length(to - from) / step)));
        if (leg > 0u)
            walk.Corners.push_back({static_cast<uint32>(positions.size()), static_cast<uint32>(leg)});
        for (uint32 i = 0; i < pieces; ++i)
            positions.push_back(from + (to - from) * (static_cast<float32>(i) / static_cast<float32>(pieces)));
    }
    positions.push_back(points.back());

    float32 travelled = 0.0f;
    walk.Stations.resize(positions.size());
    for (size_t i = 0; i < positions.size(); ++i)
    {
        if (i > 0u)
            travelled += Length(positions[i] - positions[i - 1u]);
        const V3 ahead = positions[std::min(i + 1u, positions.size() - 1u)];
        const V3 behind = positions[i == 0u ? 0u : i - 1u];
        SplineStripStation& s = walk.Stations[i];
        s.Position = positions[i];
        s.Forward = Normalize(ahead - behind);
        s.Right = Normalize(V3::Cross(kUp, V3{s.Forward.x, 0.0f, s.Forward.z}));
        s.Up = V3::Cross(s.Forward, s.Right);
        s.HalfWidthLeft = halfWidth;
        s.HalfWidthRight = halfWidth;
        s.Distance = travelled;
    }
    return walk;
}

inline SplineProfile Wall(float32 thickness, float32 height)
{
    SplineProfileParams params;
    params.Shape = SplineProfileShape::Rectangle;
    params.Width = thickness;
    params.Height = height;
    return BuildProfile(params);
}

// Two legs from the origin: `legA` metres along +Z, then `legB` metres along a
// direction turned `turnDegrees` to the right of +Z.
inline std::vector<V3> Elbow(float32 legA, float32 legB, float32 turnDegrees)
{
    const float32 turn = turnDegrees * kPi / 180.0f;
    const V3 corner{0.0f, 0.0f, legA};
    return {V3{0.0f, 0.0f, 0.0f}, corner, corner + V3{std::sin(turn), 0.0f, std::cos(turn)} * legB};
}

// The bottom vertex on each side of ring `r`, by its lateral coordinate in the
// ring's own frame.
struct RingBase
{
    V3 Left;
    V3 Right;
};

inline RingBase BaseOf(const SplineStripMesh& mesh, uint32 r, const V3& origin, const V3& right)
{
    RingBase base{};
    float32 least = 1.0e9f;
    float32 most = -1.0e9f;
    for (uint32 k = 0; k < mesh.RingVertexCount; ++k)
    {
        const V3& p = mesh.Vertices[r * mesh.RingVertexCount + k].Position;
        if (p.y > 0.5f)
            continue;
        const float32 lateral = V3::Dot(p - origin, right);
        if (lateral < least)
        {
            least = lateral;
            base.Left = p;
        }
        if (lateral > most)
        {
            most = lateral;
            base.Right = p;
        }
    }
    return base;
}

// Side-face triangles whose winding disagrees with the normals the builder gave
// them: a face folded back through itself.
inline uint32 InvertedSideTriangles(const SplineStripMesh& mesh)
{
    uint32 inverted = 0;
    const uint32 ringVertices = mesh.RingCount * mesh.RingVertexCount;
    for (size_t t = 0; t + 2u < mesh.Indices.size(); t += 3u)
    {
        if (mesh.Indices[t] >= ringVertices)
            continue;
        const SplineVertex& a = mesh.Vertices[mesh.Indices[t]];
        const SplineVertex& b = mesh.Vertices[mesh.Indices[t + 1u]];
        const SplineVertex& c = mesh.Vertices[mesh.Indices[t + 2u]];
        if (std::abs(a.Normal.y) > 0.5f)
            continue;
        const V3 geometric = V3::Cross(b.Position - a.Position, c.Position - a.Position);
        if (Length(geometric) < 1.0e-8f)
            continue;
        if (V3::Dot(geometric, a.Normal + b.Normal + c.Normal) < 0.0f)
            ++inverted;
    }
    return inverted;
}

// Top-face triangles (a normal pointing up) folded back through themselves.
inline uint32 FoldedTopTriangles(const SplineStripMesh& mesh)
{
    uint32 folded = 0;
    const uint32 ringVertices = mesh.RingCount * mesh.RingVertexCount;
    for (size_t t = 0; t + 2u < mesh.Indices.size(); t += 3u)
    {
        if (mesh.Indices[t] >= ringVertices)
            continue;
        const SplineVertex& a = mesh.Vertices[mesh.Indices[t]];
        const SplineVertex& b = mesh.Vertices[mesh.Indices[t + 1u]];
        const SplineVertex& c = mesh.Vertices[mesh.Indices[t + 2u]];
        if (a.Normal.y < 0.5f)
            continue;
        const V3 geometric = V3::Cross(b.Position - a.Position, c.Position - a.Position);
        if (Length(geometric) < 1.0e-8f)
            continue;
        if (V3::Dot(geometric, a.Normal + b.Normal + c.Normal) < 0.0f)
            ++folded;
    }
    return folded;
}

inline SplineStripMesh Sweep(const SplineProfile& profile, const std::vector<SplineStripStation>& stations)
{
    SplineStripParams params;
    params.UOriginMetres = 0.0f;
    return BuildSplineStrip(profile, stations, params);
}

} // namespace GameEngine::SplineGeometry::Testing
