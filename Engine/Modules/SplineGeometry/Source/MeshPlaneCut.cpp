#include "SplineGeometry/MeshPlaneCut.h"

#include "Mathematics/Triangulation.h"
#include "Mathematics/Vector2.h"
#include "Types/FlatMap.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

namespace GameEngine::SplineGeometry
{
namespace
{

using V2 = Mathematics::Vector2;
using V3 = Mathematics::Vector3;
using V4 = Mathematics::Vector4;

// A vertex this close to the plane lies on it, in the piece's local metres.
// Kit pieces are modelled to the millimetre; a micrometre sliver is noise.
constexpr float32 kOnPlaneMetres = 1.0e-6f;
// Cut points this close together are one point of a loop. Wider than the
// on-plane band, so the two ends of a hard edge split for its normals weld
// even when they were interpolated from opposite ends of the edge.
constexpr float32 kWeldMetres = 1.0e-5f;
// Twice the area below which a cap outline or an ear is degenerate.
constexpr float32 kMinDoubledArea = 1.0e-12f;
constexpr Mathematics::EarClipTolerances kEarClipTolerances{kMinDoubledArea, kWeldMetres};
// A face whose corners' UVs all lie within this of each other, in UV units,
// carries a palette colour rather than an unwrap.
constexpr float32 kPaletteUVExtent = 1.0e-5f;

V3 NormalizedOr(const V3& v, const V3& fallback)
{
    const float32 lengthSquared = V3::Dot(v, v);
    if (!(lengthSquared > 1.0e-20f) || !std::isfinite(lengthSquared))
        return fallback;
    return v * (1.0f / std::sqrt(lengthSquared));
}

// A vertex as the cut carries it: the piece's vertex and its colour, which
// every attribute of a new edge point is interpolated from.
struct CutVertex
{
    V3 Position{};
    V3 Normal{};
    Mathematics::Vector2 UV{};
    V4 Tangent{0.0f, 0.0f, 1.0f, 1.0f};
    V4 Color{1.0f, 1.0f, 1.0f, 1.0f};
};

std::vector<CutVertex> ToCutVertices(const PieceMesh& mesh)
{
    std::vector<CutVertex> vertices(mesh.Vertices.size());
    const bool colours = mesh.HasColors();
    for (size_t i = 0; i < vertices.size(); ++i)
    {
        const SplineVertex& v = mesh.Vertices[i];
        vertices[i] = {v.Position, v.Normal, v.UV, v.Tangent,
                       colours ? mesh.Colors[i] : V4(1.0f, 1.0f, 1.0f, 1.0f)};
    }
    return vertices;
}

CutVertex LerpVertex(const CutVertex& a, const CutVertex& b, float32 t)
{
    CutVertex out;
    out.Position = a.Position + (b.Position - a.Position) * t;
    out.Normal = NormalizedOr(a.Normal + (b.Normal - a.Normal) * t, a.Normal);
    out.UV = a.UV + (b.UV - a.UV) * t;
    const V3 tangentA(a.Tangent.x, a.Tangent.y, a.Tangent.z);
    const V3 tangentB(b.Tangent.x, b.Tangent.y, b.Tangent.z);
    out.Tangent = V4(NormalizedOr(tangentA + (tangentB - tangentA) * t, tangentA), a.Tangent.w);
    out.Color = a.Color + (b.Color - a.Color) * t;
    return out;
}

// Cut points welded by position, so the loops close across split vertices.
class PointWeld
{
public:
    uint32 Find(const V3& p)
    {
        const std::array<int64, 3> cell = CellOf(p);
        for (int64 dx = -1; dx <= 1; ++dx)
        {
            for (int64 dy = -1; dy <= 1; ++dy)
            {
                for (int64 dz = -1; dz <= 1; ++dz)
                {
                    const std::vector<uint32>* ids =
                        m_Cells.Find({cell[0] + dx, cell[1] + dy, cell[2] + dz});
                    if (!ids)
                        continue;
                    for (const uint32 id : *ids)
                    {
                        const V3 delta = m_Points[id] - p;
                        if (V3::Dot(delta, delta) <= kWeldMetres * kWeldMetres)
                            return id;
                    }
                }
            }
        }
        const uint32 id = static_cast<uint32>(m_Points.size());
        m_Points.push_back(p);
        m_Cells.GetOrInsert(cell).push_back(id);
        return id;
    }

private:
    static std::array<int64, 3> CellOf(const V3& p)
    {
        return {static_cast<int64>(std::floor(p.x / kWeldMetres)),
                static_cast<int64>(std::floor(p.y / kWeldMetres)),
                static_cast<int64>(std::floor(p.z / kWeldMetres))};
    }

    FlatMap<std::array<int64, 3>, std::vector<uint32>> m_Cells;
    std::vector<V3> m_Points;
};

float32 Cross2(const V2& a, const V2& b) { return a.x * b.y - a.y * b.x; }

float32 DoubledArea(const std::vector<V2>& points, const std::vector<uint32>& loop)
{
    float32 sum = 0.0f;
    for (size_t i = 0; i < loop.size(); ++i)
        sum += Cross2(points[loop[i]], points[loop[(i + 1u) % loop.size()]]);
    return sum;
}

bool InsidePolygon(const std::vector<V2>& points, const std::vector<uint32>& loop, const V2& p)
{
    bool inside = false;
    for (size_t i = 0, j = loop.size() - 1u; i < loop.size(); j = i++)
    {
        const V2& a = points[loop[i]];
        const V2& b = points[loop[j]];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            inside = !inside;
    }
    return inside;
}

// How a cap is unwrapped: the unwrap of one face the plane cuts, continued
// across the cut edge onto the plane as if the face were folded flat onto it,
// at the piece's texel density. Along the edge the cap's UV runs as the face's
// does; into the cap it runs as the face's does toward the edge. So a face's
// V that falls as height rises, a U laid vertically or a mirrored island all
// carry across without a jog in the texture's phase. Without an anchor (no cut
// face carries an unwrap) the cap takes its first outline point's UV
// everywhere.
struct CapUnwrap
{
    float32 TexelDensity = 0.0f;
    bool HasAnchor = false;
    V3 AnchorPosition{};
    V2 AnchorUV{};
    // Unit direction of the anchor edge, and the unit direction in the plane
    // perpendicular to it that points into the cap.
    V3 Along{};
    V3 IntoCap{};
    // The anchor face's UV change per metre along the edge, and per metre
    // moving across its face toward the edge.
    V2 UVAlong{};
    V2 UVTowardEdge{};
};

// The cut's loops, each a closed chain of welded point ids in the direction
// the cap is wound.
struct CutLoops
{
    std::vector<std::vector<uint32>> Closed;
    uint32 Open = 0;
};

CutLoops ChainLoops(const std::vector<std::pair<uint32, uint32>>& edges)
{
    // An edge both kept triangles either side of it contribute, one each way,
    // is interior to the kept surface: the two cancel.
    FlatMap<std::pair<uint32, uint32>, int32> count;
    for (const auto& [a, b] : edges)
    {
        if (a == b)
            continue;
        int32* reverse = count.Find({b, a});
        if (reverse && *reverse > 0)
            --*reverse;
        else
            ++count.GetOrInsert({a, b});
    }
    // The cap runs each cut edge the other way round from the surface
    // triangle that owns it, which is what keeps the closed surface's winding
    // consistent across the seam.
    FlatMap<uint32, std::vector<uint32>> outgoing;
    FlatMap<uint32, int32> surplus; // outgoing minus incoming
    size_t remaining = 0;
    for (const auto& [edge, n] : count)
    {
        for (int32 k = 0; k < n; ++k)
        {
            outgoing.GetOrInsert(edge.second).push_back(edge.first);
            ++surplus.GetOrInsert(edge.second);
            --surplus.GetOrInsert(edge.first);
            ++remaining;
        }
    }

    CutLoops loops;
    while (remaining > 0u)
    {
        // An open chain is walked from its head, so it counts once; what is
        // left once no head remains is closed loops.
        uint32 start = UINT32_MAX;
        for (const auto& [from, to] : outgoing)
        {
            if (!to.empty() && surplus.GetOrInsert(from) > 0)
            {
                start = from;
                break;
            }
        }
        for (auto it = outgoing.begin(); start == UINT32_MAX && it != outgoing.end(); ++it)
        {
            if (!it->second.empty())
                start = it->first;
        }
        std::vector<uint32> loop{start};
        uint32 at = start;
        bool closed = false;
        while (true)
        {
            std::vector<uint32>* next = outgoing.Find(at);
            if (!next || next->empty())
                break;
            const uint32 to = next->back();
            next->pop_back();
            --remaining;
            --surplus.GetOrInsert(at);
            ++surplus.GetOrInsert(to);
            if (to == start)
            {
                closed = true;
                break;
            }
            loop.push_back(to);
            at = to;
        }
        if (closed && loop.size() >= 3u)
            loops.Closed.push_back(std::move(loop));
        else if (!closed)
            ++loops.Open;
    }
    return loops;
}

// The plane's side of every vertex, and what the clip adds: the kept
// triangles, the points on cut edges and the cut edges themselves as welded
// point ids in the kept triangles' winding.
class TriangleClipper
{
public:
    TriangleClipper(const PieceMesh& mesh, std::vector<float32> distance, const V3& normal)
        : m_Normal(normal), m_Distance(std::move(distance)), m_Vertices(ToCutVertices(mesh)),
          m_WeldOf(m_Vertices.size(), UINT32_MAX)
    {
        m_Indices.reserve(mesh.Indices.size());
    }

    // Keeps a triangle on the kept side whole, drops one on the removed side
    // or lying in the plane (the cap replaces it), and clips one the plane
    // crosses to the polygon on its kept side.
    void Clip(const uint32 (&corner)[3])
    {
        const float32 d[3] = {m_Distance[corner[0]], m_Distance[corner[1]], m_Distance[corner[2]]};
        if (d[0] >= 0.0f && d[1] >= 0.0f && d[2] >= 0.0f)
            return;

        m_Polygon.clear();
        for (int k = 0; k < 3; ++k)
        {
            const int next = (k + 1) % 3;
            if (d[k] <= 0.0f)
                m_Polygon.push_back({corner[k], d[k] == 0.0f});
            if ((d[k] < 0.0f && d[next] > 0.0f) || (d[k] > 0.0f && d[next] < 0.0f))
                m_Polygon.push_back({EdgePoint(corner[k], corner[next]), true});
        }
        for (size_t k = 1; k + 1u < m_Polygon.size(); ++k)
        {
            m_Indices.insert(m_Indices.end(),
                             {m_Polygon[0].Vertex, m_Polygon[k].Vertex, m_Polygon[k + 1u].Vertex});
        }
        for (size_t k = 0; k < m_Polygon.size(); ++k)
        {
            const PolygonCorner& a = m_Polygon[k];
            const PolygonCorner& b = m_Polygon[(k + 1u) % m_Polygon.size()];
            if (!a.OnPlane || !b.OnPlane)
                continue;
            m_CutEdges.emplace_back(WeldedId(a.Vertex), WeldedId(b.Vertex));
            SampleUnwrap(corner, a.Vertex, b.Vertex);
        }
    }

    // UV units per metre along the cut edges of the faces that carry an
    // unwrap: the median weighted by length, so a seam, a stretched face or a
    // run of short edges does not set the scale. Anchored on the longest such
    // edge, the lowest start point breaking a tie, so the anchor is a property
    // of the piece rather than of the order its triangles are listed in.
    CapUnwrap Unwrap() const
    {
        CapUnwrap unwrap;
        if (m_TexelSamples.empty())
            return unwrap;
        const TexelSample& first = *std::min_element(
            m_TexelSamples.begin(), m_TexelSamples.end(),
            [this](const TexelSample& a, const TexelSample& b) { return AnchorsBefore(a, b); });
        unwrap.HasAnchor = true;
        unwrap.AnchorPosition = m_Vertices[first.Vertex].Position;
        unwrap.AnchorUV = m_Vertices[first.Vertex].UV;
        unwrap.Along = first.Along;
        unwrap.IntoCap = first.IntoCap;
        unwrap.UVAlong = first.UVAlong;
        unwrap.UVTowardEdge = first.UVTowardEdge;
        std::vector<TexelSample> sorted = m_TexelSamples;
        std::sort(sorted.begin(), sorted.end(),
                  [](const TexelSample& a, const TexelSample& b) { return a.Density < b.Density; });
        float32 total = 0.0f;
        for (const TexelSample& sample : sorted)
            total += sample.Metres;
        float32 walked = 0.0f;
        for (const TexelSample& sample : sorted)
        {
            walked += sample.Metres;
            unwrap.TexelDensity = sample.Density;
            if (walked >= 0.5f * total)
                break;
        }
        return unwrap;
    }

    // The first vertex at each welded point: its UV and colour are the cut
    // edge's, interpolated when the point was made.
    std::vector<uint32> Representatives() const
    {
        std::vector<uint32> representative(m_WeldCount, UINT32_MAX);
        for (uint32 v = 0; v < m_WeldOf.size(); ++v)
        {
            const uint32 id = m_WeldOf[v];
            if (id != UINT32_MAX && representative[id] == UINT32_MAX)
                representative[id] = v;
        }
        return representative;
    }

    const std::vector<std::pair<uint32, uint32>>& CutEdges() const { return m_CutEdges; }
    std::vector<CutVertex>& Vertices() { return m_Vertices; }
    std::vector<uint32>& Indices() { return m_Indices; }

private:
    struct PolygonCorner
    {
        uint32 Vertex;
        bool OnPlane;
    };

    struct TexelSample
    {
        float32 Density;
        float32 Metres;
        uint32 Vertex;
        V3 Along;
        V3 IntoCap;
        V2 UVAlong;
        V2 UVTowardEdge;
    };

    // Longer first; between equal lengths the start point lowest in x, then y,
    // then z, then the lower UV. Lengths compare exactly: each is computed from
    // the same interpolated points whatever order the triangles come in, so
    // the choice is a function of the mesh.
    bool AnchorsBefore(const TexelSample& a, const TexelSample& b) const
    {
        if (a.Metres != b.Metres)
            return a.Metres > b.Metres;
        const CutVertex& p = m_Vertices[a.Vertex];
        const CutVertex& q = m_Vertices[b.Vertex];
        return std::tie(p.Position.x, p.Position.y, p.Position.z, p.UV.x, p.UV.y) <
               std::tie(q.Position.x, q.Position.y, q.Position.z, q.UV.x, q.UV.y);
    }

    // The UV change a face's unwrap gives a step `step` in its plane.
    V2 FaceUVChange(const uint32 (&corner)[3], const V3& step) const
    {
        const CutVertex& p0 = m_Vertices[corner[0]];
        const V3 d1 = m_Vertices[corner[1]].Position - p0.Position;
        const V3 d2 = m_Vertices[corner[2]].Position - p0.Position;
        const float32 a11 = V3::Dot(d1, d1);
        const float32 a12 = V3::Dot(d1, d2);
        const float32 a22 = V3::Dot(d2, d2);
        const float32 det = a11 * a22 - a12 * a12;
        if (!(det > kMinDoubledArea))
            return V2(0.0f, 0.0f);
        const float32 b1 = V3::Dot(d1, step);
        const float32 b2 = V3::Dot(d2, step);
        const float32 alpha = (b1 * a22 - b2 * a12) / det;
        const float32 beta = (b2 * a11 - b1 * a12) / det;
        return (m_Vertices[corner[1]].UV - p0.UV) * alpha + (m_Vertices[corner[2]].UV - p0.UV) * beta;
    }

    // Records the cut edge from `a` to `b` as a candidate for the cap's
    // unwrap, when the face it lies on carries one.
    void SampleUnwrap(const uint32 (&corner)[3], uint32 a, uint32 b)
    {
        const CutVertex& from = m_Vertices[a];
        const CutVertex& to = m_Vertices[b];
        const float32 metres = (to.Position - from.Position).Length();
        if (!(metres > kWeldMetres))
            return;
        const V2 uv0 = m_Vertices[corner[0]].UV;
        const float32 extent = std::max((m_Vertices[corner[1]].UV - uv0).Length(),
                                        (m_Vertices[corner[2]].UV - uv0).Length());
        if (!(extent > kPaletteUVExtent))
            return;
        const V3 along = (to.Position - from.Position) * (1.0f / metres);
        const V3 faceNormal =
            NormalizedOr(V3::Cross(m_Vertices[corner[1]].Position - m_Vertices[corner[0]].Position,
                                   m_Vertices[corner[2]].Position - m_Vertices[corner[0]].Position),
                         V3(0.0f, 0.0f, 0.0f));
        // Across the face toward the plane: the kept face lies on the plane's
        // negative side.
        V3 towardEdge = V3::Cross(faceNormal, along);
        if (V3::Dot(towardEdge, m_Normal) < 0.0f)
            towardEdge = towardEdge * -1.0f;
        // Into the cap: the solid lies behind the face's outward normal.
        V3 intoCap = V3::Cross(m_Normal, along);
        if (V3::Dot(intoCap, faceNormal) > 0.0f)
            intoCap = intoCap * -1.0f;
        const V2 uvAlong = (to.UV - from.UV) * (1.0f / metres);
        m_TexelSamples.push_back({uvAlong.Length(), metres, a, along, intoCap, uvAlong,
                                  FaceUVChange(corner, towardEdge)});
    }

    // Interpolated from the lower index, so both triangles sharing the edge
    // get the same bits.
    uint32 EdgePoint(uint32 a, uint32 b)
    {
        const uint32 lo = std::min(a, b);
        const uint32 hi = std::max(a, b);
        if (const uint32* found = m_EdgePoints.Find({lo, hi}))
            return *found;
        const float32 t = m_Distance[lo] / (m_Distance[lo] - m_Distance[hi]);
        m_Vertices.push_back(LerpVertex(m_Vertices[lo], m_Vertices[hi], t));
        const uint32 index = static_cast<uint32>(m_Vertices.size() - 1u);
        m_EdgePoints.InsertOrAssign({lo, hi}, index);
        return index;
    }

    uint32 WeldedId(uint32 vertex)
    {
        if (vertex >= m_WeldOf.size())
            m_WeldOf.resize(vertex + 1u, UINT32_MAX);
        if (m_WeldOf[vertex] == UINT32_MAX)
        {
            m_WeldOf[vertex] = m_Weld.Find(m_Vertices[vertex].Position);
            m_WeldCount = std::max(m_WeldCount, m_WeldOf[vertex] + 1u);
        }
        return m_WeldOf[vertex];
    }

    V3 m_Normal;
    std::vector<float32> m_Distance;
    std::vector<CutVertex> m_Vertices;
    std::vector<uint32> m_Indices;
    FlatMap<std::pair<uint32, uint32>, uint32> m_EdgePoints;
    PointWeld m_Weld;
    std::vector<uint32> m_WeldOf;
    uint32 m_WeldCount = 0;
    std::vector<std::pair<uint32, uint32>> m_CutEdges;
    std::vector<TexelSample> m_TexelSamples;
    std::vector<PolygonCorner> m_Polygon;
};

// The closed loops projected into the plane.
struct CapOutlines
{
    std::vector<V2> Points;                 // one per welded point on a loop
    std::vector<uint32> PointIds;           // the welded id of each point
    std::vector<std::vector<uint32>> Loops; // point indices, in cap winding
    std::vector<float32> DoubledAreas;
};

float32 LargestPointX(const CapOutlines& outlines, size_t loop)
{
    float32 largest = -std::numeric_limits<float32>::max();
    for (const uint32 p : outlines.Loops[loop])
        largest = std::max(largest, outlines.Points[p].x);
    return largest;
}

// Triangulates every outline with the holes inside it, as point indices.
// Every outline wound one way is an outer boundary and every one wound the
// other way a hole; the largest is certainly an outer one. The plane is
// mirrored so the outer ones run counter-clockwise, which keeps each
// triangle wound as its outline was.
std::vector<uint32> TriangulateCap(CapOutlines& outlines)
{
    std::vector<uint32> triangles;
    if (outlines.Loops.empty())
        return triangles;
    size_t largest = 0;
    for (size_t i = 1; i < outlines.DoubledAreas.size(); ++i)
    {
        if (std::abs(outlines.DoubledAreas[i]) > std::abs(outlines.DoubledAreas[largest]))
            largest = i;
    }
    if (outlines.DoubledAreas[largest] < 0.0f)
    {
        for (V2& p : outlines.Points)
            p.y = -p.y;
        for (float32& area : outlines.DoubledAreas)
            area = -area;
    }

    std::vector<size_t> outers;
    std::vector<size_t> holes;
    for (size_t i = 0; i < outlines.Loops.size(); ++i)
    {
        if (std::abs(outlines.DoubledAreas[i]) <= kMinDoubledArea)
            continue;
        (outlines.DoubledAreas[i] > 0.0f ? outers : holes).push_back(i);
    }

    // Each hole belongs to the smallest outline around it, and is bridged in
    // from its right, rightmost hole first.
    std::vector<std::vector<size_t>> holesOf(outlines.Loops.size());
    for (const size_t hole : holes)
    {
        size_t owner = outlines.Loops.size();
        const V2& probe = outlines.Points[outlines.Loops[hole].front()];
        for (const size_t outer : outers)
        {
            if (!InsidePolygon(outlines.Points, outlines.Loops[outer], probe))
                continue;
            if (owner == outlines.Loops.size() ||
                outlines.DoubledAreas[outer] < outlines.DoubledAreas[owner])
                owner = outer;
        }
        if (owner != outlines.Loops.size())
            holesOf[owner].push_back(hole);
    }
    for (const size_t outer : outers)
    {
        std::vector<uint32> polygon = outlines.Loops[outer];
        std::vector<size_t>& inner = holesOf[outer];
        std::sort(inner.begin(), inner.end(), [&outlines](size_t a, size_t b)
                  { return LargestPointX(outlines, a) > LargestPointX(outlines, b); });
        for (const size_t hole : inner)
            Mathematics::BridgeHole(outlines.Points, polygon, outlines.Loops[hole], kEarClipTolerances);
        Mathematics::EarClip(outlines.Points, std::move(polygon), triangles, kEarClipTolerances);
    }
    return triangles;
}

// The cap's UV as a linear map of position on the plane, and the tangent frame
// that map implies.
struct CapMapping
{
    V3 Origin{};
    V2 OriginUV{};
    V3 Along{};
    V3 Into{};
    V2 UVAlong{};
    V2 UVInto{};
    V4 Tangent{};
};

// The unwrap's two UV gradients, scaled together to the texel density so the
// anchor face's shape is kept at the piece's scale. A face that gives no
// usable gradient toward the edge takes the perpendicular one.
CapMapping MapCap(const CapUnwrap& unwrap, const V3& normal, const V3& axisU,
                  const CutVertex& fallback)
{
    CapMapping mapping;
    if (!unwrap.HasAnchor)
    {
        mapping.Origin = fallback.Position;
        mapping.OriginUV = fallback.UV;
        mapping.Along = axisU;
        mapping.Into = V3::Cross(normal, axisU);
        mapping.Tangent = V4(axisU, 1.0f);
        return mapping;
    }
    mapping.Origin = unwrap.AnchorPosition;
    mapping.OriginUV = unwrap.AnchorUV;
    mapping.Along = unwrap.Along;
    mapping.Into = unwrap.IntoCap;
    const float32 scale = unwrap.TexelDensity / unwrap.UVAlong.Length();
    mapping.UVAlong = unwrap.UVAlong * scale;
    mapping.UVInto = unwrap.UVTowardEdge * scale;
    float32 det = mapping.UVAlong.x * mapping.UVInto.y - mapping.UVInto.x * mapping.UVAlong.y;
    if (std::abs(det) <= kMinDoubledArea)
    {
        mapping.UVInto = V2(-mapping.UVAlong.y, mapping.UVAlong.x);
        det = mapping.UVAlong.x * mapping.UVInto.y - mapping.UVInto.x * mapping.UVAlong.y;
    }
    // The position change per unit of U and of V: the inverse of the map.
    const V3 perU = mapping.Along * (mapping.UVInto.y / det) + mapping.Into * (-mapping.UVAlong.y / det);
    const V3 perV = mapping.Along * (-mapping.UVInto.x / det) + mapping.Into * (mapping.UVAlong.x / det);
    const V3 tangent = NormalizedOr(perU, axisU);
    // B = cross(N, T) * w must run the way V does.
    const float32 handedness = V3::Dot(V3::Cross(normal, tangent), perV) >= 0.0f ? 1.0f : -1.0f;
    mapping.Tangent = V4(tangent, handedness);
    return mapping;
}

// Caps each closed loop with vertices of its own that face along the plane.
// The cap continues the unwrap's anchor face across the cut edge (CapUnwrap),
// with the tangent frame that unwrap implies.
// Returns how many loops it capped.
uint32 CapLoops(const CutLoops& loops, const std::vector<uint32>& representative, const V3& normal,
                const CapUnwrap& unwrap, std::vector<CutVertex>& vertices,
                std::vector<uint32>& indices)
{
    // The cap's own frame in the plane.
    const V3 helper = std::abs(normal.y) < 0.9f ? V3(0.0f, 1.0f, 0.0f) : V3(1.0f, 0.0f, 0.0f);
    const V3 axisU = NormalizedOr(V3::Cross(helper, normal), V3(1.0f, 0.0f, 0.0f));
    const V3 axisV = V3::Cross(normal, axisU);

    CapOutlines outlines;
    FlatMap<uint32, uint32> pointOf; // welded id -> point index
    for (const std::vector<uint32>& loop : loops.Closed)
    {
        std::vector<uint32> outline;
        outline.reserve(loop.size());
        for (const uint32 id : loop)
        {
            const uint32* known = pointOf.Find(id);
            if (!known)
            {
                known = &pointOf.InsertOrAssign(id, static_cast<uint32>(outlines.Points.size()));
                const V3& p = vertices[representative[id]].Position;
                outlines.Points.emplace_back(V3::Dot(p, axisU), V3::Dot(p, axisV));
                outlines.PointIds.push_back(id);
            }
            outline.push_back(*known);
        }
        outlines.DoubledAreas.push_back(DoubledArea(outlines.Points, outline));
        outlines.Loops.push_back(std::move(outline));
    }
    const std::vector<uint32> triangles = TriangulateCap(outlines);

    const uint32 firstCapVertex = static_cast<uint32>(vertices.size());
    if (outlines.PointIds.empty())
        return 0u;
    const CapMapping mapping = MapCap(unwrap, normal, axisU,
                                      vertices[representative[outlines.PointIds[0]]]);
    for (const uint32 id : outlines.PointIds)
    {
        CutVertex cap = vertices[representative[id]];
        cap.Normal = normal;
        cap.Tangent = mapping.Tangent;
        const V3 offset = cap.Position - mapping.Origin;
        cap.UV = mapping.OriginUV + mapping.UVAlong * V3::Dot(offset, mapping.Along) +
                 mapping.UVInto * V3::Dot(offset, mapping.Into);
        vertices.push_back(cap);
    }
    for (const uint32 point : triangles)
        indices.push_back(firstCapVertex + point);
    return static_cast<uint32>(outlines.Loops.size());
}

// The referenced vertices in their original order, reindexed.
// Colours are carried only when the piece had them.
PieceMesh Compact(const std::vector<CutVertex>& vertices, const std::vector<uint32>& indices,
                  bool colours)
{
    std::vector<uint32> remap(vertices.size(), UINT32_MAX);
    for (const uint32 index : indices)
        remap[index] = 0u;
    PieceMesh mesh;
    mesh.Vertices.reserve(vertices.size());
    for (size_t v = 0; v < vertices.size(); ++v)
    {
        if (remap[v] == UINT32_MAX)
            continue;
        remap[v] = static_cast<uint32>(mesh.Vertices.size());
        const CutVertex& cut = vertices[v];
        SplineVertex out;
        out.Position = cut.Position;
        out.Normal = cut.Normal;
        out.UV = cut.UV;
        out.Tangent = cut.Tangent;
        mesh.Vertices.push_back(out);
        if (colours)
            mesh.Colors.push_back(cut.Color);
    }
    mesh.Indices.reserve(indices.size());
    for (const uint32 index : indices)
        mesh.Indices.push_back(remap[index]);
    return mesh;
}

} // namespace

MeshCutResult CutMeshByPlane(const PieceMesh& mesh, const CutPlane& plane)
{
    MeshCutResult result;
    const float32 normalLength = std::sqrt(V3::Dot(plane.Normal, plane.Normal));
    if (!(normalLength > 0.0f) || !std::isfinite(normalLength))
    {
        result.Mesh = mesh;
        return result;
    }
    const V3 normal = plane.Normal * (1.0f / normalLength);
    const float32 offset = plane.Offset / normalLength;

    std::vector<float32> distance(mesh.Vertices.size());
    bool anyRemoved = false;
    for (size_t i = 0; i < mesh.Vertices.size(); ++i)
    {
        float32 d = V3::Dot(normal, mesh.Vertices[i].Position) - offset;
        if (std::abs(d) <= kOnPlaneMetres)
            d = 0.0f;
        distance[i] = d;
        anyRemoved |= d > 0.0f;
    }
    if (!anyRemoved)
    {
        result.Mesh = mesh;
        return result;
    }

    TriangleClipper clipper(mesh, std::move(distance), normal);
    for (size_t t = 0; t + 2u < mesh.Indices.size(); t += 3u)
        clipper.Clip({mesh.Indices[t], mesh.Indices[t + 1u], mesh.Indices[t + 2u]});

    const CutLoops loops = ChainLoops(clipper.CutEdges());
    result.OpenLoops = loops.Open;
    result.CappedLoops = CapLoops(loops, clipper.Representatives(), normal, clipper.Unwrap(),
                                  clipper.Vertices(), clipper.Indices());
    result.Mesh = Compact(clipper.Vertices(), clipper.Indices(), mesh.HasColors());
    return result;
}

} // namespace GameEngine::SplineGeometry
