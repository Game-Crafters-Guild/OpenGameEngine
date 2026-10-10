#include "SplineGeometry/MeshPlaneCut.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <vector>

using namespace GameEngine;
using SplineGeometry::CutMeshByPlane;
using SplineGeometry::CutPlane;
using SplineGeometry::MeshCutResult;
using SplineGeometry::PieceMesh;
using SplineGeometry::SplineVertex;
using V2 = Mathematics::Vector2;
using V3 = Mathematics::Vector3;
using V4 = Mathematics::Vector4;

namespace
{

// One quad, wound so its geometric normal (b - a) x (c - a) points along
// `normal`, with split vertices of its own the way a modelled hard edge is.
// Its UV is the position projected on two of the axes and its colour is the
// position itself, so an interpolated attribute is predictable anywhere.
void AddQuad(PieceMesh& mesh, const V3& a, const V3& b, const V3& c, const V3& d,
             const V3& normal)
{
    const uint32 base = static_cast<uint32>(mesh.Vertices.size());
    for (const V3& p : {a, b, c, d})
    {
        SplineVertex v;
        v.Position = p;
        v.Normal = normal;
        v.UV = V2(p.x + p.z, p.y);
        v.Tangent = V4(1.0f, 0.0f, 0.0f, 1.0f);
        mesh.Vertices.push_back(v);
        mesh.Colors.push_back(V4(p.x, p.y, p.z, 1.0f));
    }
    const bool agrees = V3::Dot(V3::Cross(b - a, c - a), normal) > 0.0f;
    if (agrees)
        mesh.Indices.insert(mesh.Indices.end(), {base, base + 1u, base + 2u, base, base + 2u, base + 3u});
    else
        mesh.Indices.insert(mesh.Indices.end(), {base, base + 2u, base + 1u, base, base + 3u, base + 2u});
}

// A box whose every face is unwrapped on its own, at one UV unit per metre
// and at an offset of its own, the way a kit piece's faces are laid out in
// separate islands: `normal` picks the face.
void AddUnwrappedQuad(PieceMesh& mesh, const V3& a, const V3& b, const V3& c, const V3& d,
                      const V3& normal, float32 islandOffset)
{
    const V3 u = (b - a).Normalize();
    const V3 v = V3::Cross(normal, u);
    AddQuad(mesh, a, b, c, d, normal);
    for (size_t k = mesh.Vertices.size() - 4u; k < mesh.Vertices.size(); ++k)
    {
        const V3& p = mesh.Vertices[k].Position;
        mesh.Vertices[k].UV = V2(V3::Dot(p, u) + islandOffset, V3::Dot(p, v) - islandOffset);
        mesh.Vertices[k].Tangent = V4(u, 1.0f);
    }
}

// Every vertex of the last quad added takes one palette texel, the way a kit's
// end, top and bottom faces often carry a colour rather than an unwrap.
void MakeLastQuadPalette(PieceMesh& mesh)
{
    for (size_t k = mesh.Vertices.size() - 4u; k < mesh.Vertices.size(); ++k)
        mesh.Vertices[k].UV = V2(0.017f, 0.03f);
}

// How a kit wall's two long faces are unwrapped, each in an island of its own.
enum class LongFaceUnwrap
{
    // U along the wall, V falling as height rises: what an FBX kit carries
    // once the import flips V.
    VFallsWithHeight,
    // U running up the wall, V along it.
    UVertical,
};

V2 LongFaceUV(LongFaceUnwrap unwrap, const V3& p, float32 island)
{
    return unwrap == LongFaceUnwrap::VFallsWithHeight ? V2(p.x + island, island - p.y)
                                                      : V2(p.y + island, p.x - island);
}

// A 1 m long, 2 m tall, 0.5 m thick wall along X: its two long faces (z = 0
// and z = 0.5, islands 3 and 7) carry the unwrap, its ends, top and bottom a
// palette colour, as on the castle kit. `reversed` lists the same triangles in
// the opposite order.
PieceMesh KitWall(LongFaceUnwrap unwrap, bool reversed = false)
{
    PieceMesh mesh;
    const V3 lo(0, 0, 0);
    const V3 hi(1, 2, 0.5f);
    AddQuad(mesh, V3(lo.x, lo.y, lo.z), V3(lo.x, hi.y, lo.z), V3(lo.x, hi.y, hi.z), V3(lo.x, lo.y, hi.z), V3(-1, 0, 0));
    MakeLastQuadPalette(mesh);
    AddQuad(mesh, V3(hi.x, lo.y, lo.z), V3(hi.x, lo.y, hi.z), V3(hi.x, hi.y, hi.z), V3(hi.x, hi.y, lo.z), V3(1, 0, 0));
    MakeLastQuadPalette(mesh);
    AddQuad(mesh, V3(lo.x, lo.y, lo.z), V3(lo.x, lo.y, hi.z), V3(hi.x, lo.y, hi.z), V3(hi.x, lo.y, lo.z), V3(0, -1, 0));
    MakeLastQuadPalette(mesh);
    AddQuad(mesh, V3(lo.x, hi.y, lo.z), V3(hi.x, hi.y, lo.z), V3(hi.x, hi.y, hi.z), V3(lo.x, hi.y, hi.z), V3(0, 1, 0));
    MakeLastQuadPalette(mesh);
    for (const float32 z : {lo.z, hi.z})
    {
        const float32 island = z == lo.z ? 3.0f : 7.0f;
        if (z == lo.z)
            AddQuad(mesh, V3(lo.x, lo.y, z), V3(hi.x, lo.y, z), V3(hi.x, hi.y, z), V3(lo.x, hi.y, z), V3(0, 0, -1));
        else
            AddQuad(mesh, V3(lo.x, lo.y, z), V3(lo.x, hi.y, z), V3(hi.x, hi.y, z), V3(hi.x, lo.y, z), V3(0, 0, 1));
        for (size_t k = mesh.Vertices.size() - 4u; k < mesh.Vertices.size(); ++k)
            mesh.Vertices[k].UV = LongFaceUV(unwrap, mesh.Vertices[k].Position, island);
    }
    if (reversed)
    {
        std::vector<uint32> indices;
        for (size_t t = mesh.Indices.size(); t >= 3u; t -= 3u)
            indices.insert(indices.end(), mesh.Indices.begin() + static_cast<std::ptrdiff_t>(t - 3u),
                           mesh.Indices.begin() + static_cast<std::ptrdiff_t>(t));
        mesh.Indices = std::move(indices);
    }
    return mesh;
}

// The plane a mitre cuts the kit wall on: vertical, oblique to the wall.
CutPlane KitWallMitre()
{
    const V3 normal = V3(-1.0f, 0.0f, -0.4f).Normalize();
    return {normal, V3::Dot(normal, V3(0.5f, 1.0f, 0.25f))};
}

// The cap's vertices: the ones facing along the plane.
std::vector<const SplineVertex*> CapVertices(const PieceMesh& mesh, const V3& normal)
{
    std::vector<const SplineVertex*> cap;
    for (const SplineVertex& v : mesh.Vertices)
    {
        if (V3::Dot(v.Normal, normal) > 0.999f)
            cap.push_back(&v);
    }
    return cap;
}

PieceMesh UnwrappedCube()
{
    PieceMesh mesh;
    AddUnwrappedQuad(mesh, V3(0, 0, 0), V3(0, 1, 0), V3(0, 1, 1), V3(0, 0, 1), V3(-1, 0, 0), 2.0f);
    AddUnwrappedQuad(mesh, V3(1, 0, 0), V3(1, 0, 1), V3(1, 1, 1), V3(1, 1, 0), V3(1, 0, 0), 4.0f);
    AddUnwrappedQuad(mesh, V3(0, 0, 0), V3(0, 0, 1), V3(1, 0, 1), V3(1, 0, 0), V3(0, -1, 0), 6.0f);
    AddUnwrappedQuad(mesh, V3(0, 1, 0), V3(1, 1, 0), V3(1, 1, 1), V3(0, 1, 1), V3(0, 1, 0), 8.0f);
    AddUnwrappedQuad(mesh, V3(0, 0, 0), V3(1, 0, 0), V3(1, 1, 0), V3(0, 1, 0), V3(0, 0, -1), 10.0f);
    AddUnwrappedQuad(mesh, V3(0, 0, 1), V3(0, 1, 1), V3(1, 1, 1), V3(1, 0, 1), V3(0, 0, 1), 12.0f);
    return mesh;
}

// An axis-aligned box from lo to hi, each face with its own four vertices.
void AddBox(PieceMesh& mesh, const V3& lo, const V3& hi, bool inward = false)
{
    const float32 s = inward ? -1.0f : 1.0f;
    const V3 p000(lo.x, lo.y, lo.z), p100(hi.x, lo.y, lo.z), p010(lo.x, hi.y, lo.z),
        p110(hi.x, hi.y, lo.z), p001(lo.x, lo.y, hi.z), p101(hi.x, lo.y, hi.z),
        p011(lo.x, hi.y, hi.z), p111(hi.x, hi.y, hi.z);
    AddQuad(mesh, p000, p010, p011, p001, V3(-s, 0, 0));
    AddQuad(mesh, p100, p101, p111, p110, V3(s, 0, 0));
    AddQuad(mesh, p000, p001, p101, p100, V3(0, -s, 0));
    AddQuad(mesh, p010, p110, p111, p011, V3(0, s, 0));
    AddQuad(mesh, p000, p100, p110, p010, V3(0, 0, -s));
    AddQuad(mesh, p001, p011, p111, p101, V3(0, 0, s));
}

PieceMesh UnitCube()
{
    PieceMesh mesh;
    AddBox(mesh, V3(0, 0, 0), V3(1, 1, 1));
    return mesh;
}

// A square tube along X: the 1 m cube with a 0.5 m square hole through it,
// modelled closed — four outer walls, four inner walls facing into the hole,
// and each end an annulus of four quads.
PieceMesh HollowTube()
{
    PieceMesh mesh;
    const float32 lo = 0.25f;
    const float32 hi = 0.75f;
    // Outer walls.
    AddQuad(mesh, V3(0, 0, 0), V3(0, 0, 1), V3(1, 0, 1), V3(1, 0, 0), V3(0, -1, 0));
    AddQuad(mesh, V3(0, 1, 0), V3(1, 1, 0), V3(1, 1, 1), V3(0, 1, 1), V3(0, 1, 0));
    AddQuad(mesh, V3(0, 0, 0), V3(1, 0, 0), V3(1, 1, 0), V3(0, 1, 0), V3(0, 0, -1));
    AddQuad(mesh, V3(0, 0, 1), V3(0, 1, 1), V3(1, 1, 1), V3(1, 0, 1), V3(0, 0, 1));
    // Inner walls, facing into the hole.
    AddQuad(mesh, V3(0, lo, lo), V3(1, lo, lo), V3(1, lo, hi), V3(0, lo, hi), V3(0, 1, 0));
    AddQuad(mesh, V3(0, hi, lo), V3(0, hi, hi), V3(1, hi, hi), V3(1, hi, lo), V3(0, -1, 0));
    AddQuad(mesh, V3(0, lo, lo), V3(0, hi, lo), V3(1, hi, lo), V3(1, lo, lo), V3(0, 0, 1));
    AddQuad(mesh, V3(0, lo, hi), V3(1, lo, hi), V3(1, hi, hi), V3(0, hi, hi), V3(0, 0, -1));
    // Annular ends.
    for (const float32 x : {0.0f, 1.0f})
    {
        const V3 n(x == 0.0f ? -1.0f : 1.0f, 0, 0);
        AddQuad(mesh, V3(x, 0, 0), V3(x, lo, lo), V3(x, lo, hi), V3(x, 0, 1), n);
        AddQuad(mesh, V3(x, 0, 1), V3(x, lo, hi), V3(x, hi, hi), V3(x, 1, 1), n);
        AddQuad(mesh, V3(x, 1, 1), V3(x, hi, hi), V3(x, hi, lo), V3(x, 1, 0), n);
        AddQuad(mesh, V3(x, 1, 0), V3(x, hi, lo), V3(x, lo, lo), V3(x, 0, 0), n);
    }
    return mesh;
}

V3 Corner(const PieceMesh& mesh, size_t triangle, int k)
{
    return mesh.Vertices[mesh.Indices[triangle * 3u + static_cast<size_t>(k)]].Position;
}

// Signed volume by the divergence theorem; positive for a closed mesh whose
// triangles wind counter-clockwise seen from outside.
float32 Volume(const PieceMesh& mesh)
{
    float32 sum = 0.0f;
    for (size_t t = 0; t * 3u < mesh.Indices.size(); ++t)
        sum += V3::Dot(Corner(mesh, t, 0), V3::Cross(Corner(mesh, t, 1), Corner(mesh, t, 2)));
    return sum / 6.0f;
}

using PointKey = std::tuple<int64, int64, int64>;

PointKey KeyOf(const V3& p)
{
    constexpr float32 kGrid = 1.0e-4f;
    return {std::llround(p.x / kGrid), std::llround(p.y / kGrid), std::llround(p.z / kGrid)};
}

// Closed and consistently wound: every edge, welded by position, is crossed
// once each way.
bool IsWatertight(const PieceMesh& mesh)
{
    std::map<std::pair<PointKey, PointKey>, int> directed;
    for (size_t t = 0; t * 3u < mesh.Indices.size(); ++t)
    {
        for (int k = 0; k < 3; ++k)
        {
            const PointKey a = KeyOf(Corner(mesh, t, k));
            const PointKey b = KeyOf(Corner(mesh, t, (k + 1) % 3));
            if (a != b)
                ++directed[{a, b}];
        }
    }
    for (const auto& [edge, count] : directed)
    {
        const auto reverse = directed.find({edge.second, edge.first});
        if (reverse == directed.end() || reverse->second != count)
            return false;
    }
    return true;
}

// Triangles lying in the plane x = at, and their summed area.
float32 AreaOnPlaneX(const PieceMesh& mesh, float32 at, size_t* outCount = nullptr)
{
    float32 area = 0.0f;
    size_t count = 0;
    for (size_t t = 0; t * 3u < mesh.Indices.size(); ++t)
    {
        const V3 a = Corner(mesh, t, 0);
        const V3 b = Corner(mesh, t, 1);
        const V3 c = Corner(mesh, t, 2);
        if (std::abs(a.x - at) > 1e-5f || std::abs(b.x - at) > 1e-5f || std::abs(c.x - at) > 1e-5f)
            continue;
        area += 0.5f * V3::Cross(b - a, c - a).Length();
        ++count;
    }
    if (outCount)
        *outCount = count;
    return area;
}

// Removes x < at: the normal points to the removed side.
CutPlane KeepAboveX(float32 at) { return {V3(-1, 0, 0), -at}; }

} // namespace

// A unit cube cut at x = 0.25 keeps 0.75 of its volume and is still a closed
// box of six faces: the five it had, shortened, and the cap. The closed loop is
// capped with one polygon: faces on the plane, facing along its normal,
// covering exactly the cross-section, wound outward like the rest. The outline
// keeps the point where the plane crosses each face's diagonal, so the cap
// shares every edge with the faces around it: eight points, six triangles, the
// ones along a straight run of outline points flat.
TEST(MeshPlaneCut, UnitCubeCutKeepsThreeQuartersAndSixClosedFaces)
{
    const MeshCutResult cut = CutMeshByPlane(UnitCube(), KeepAboveX(0.25f));
    EXPECT_EQ(cut.CappedLoops, 1u);
    EXPECT_EQ(cut.OpenLoops, 0u);
    EXPECT_NEAR(Volume(cut.Mesh), 0.75f, 1e-5f);
    EXPECT_TRUE(IsWatertight(cut.Mesh));

    std::set<std::tuple<int, int, int>> faceNormals;
    for (const SplineVertex& v : cut.Mesh.Vertices)
    {
        EXPECT_GE(v.Position.x, 0.25f - 1e-6f);
        faceNormals.insert({static_cast<int>(std::lround(v.Normal.x)),
                            static_cast<int>(std::lround(v.Normal.y)),
                            static_cast<int>(std::lround(v.Normal.z))});
    }
    EXPECT_EQ(faceNormals.size(), 6u);

    size_t capTriangles = 0;
    EXPECT_NEAR(AreaOnPlaneX(cut.Mesh, 0.25f, &capTriangles), 1.0f, 1e-5f);
    EXPECT_EQ(capTriangles, 6u);
    for (size_t t = 0; t * 3u < cut.Mesh.Indices.size(); ++t)
    {
        const V3 a = Corner(cut.Mesh, t, 0);
        if (std::abs(a.x - 0.25f) > 1e-6f || std::abs(Corner(cut.Mesh, t, 1).x - 0.25f) > 1e-6f ||
            std::abs(Corner(cut.Mesh, t, 2).x - 0.25f) > 1e-6f)
            continue;
        // A flat triangle along a straight run of the outline covers nothing;
        // it is there to keep the outline's edges shared.
        const V3 geometric = V3::Cross(Corner(cut.Mesh, t, 1) - a, Corner(cut.Mesh, t, 2) - a);
        if (geometric.Length() > 1e-9f)
            EXPECT_LT(geometric.x, 0.0f) << "cap wound inward";
        const SplineVertex& v = cut.Mesh.Vertices[cut.Mesh.Indices[t * 3u]];
        EXPECT_NEAR(v.Normal.x, -1.0f, 1e-6f);
        EXPECT_NEAR(std::abs(v.Tangent.x), 0.0f, 1e-6f) << "cap tangent must lie in the plane";
    }
}

// A new vertex on a cut edge carries every attribute interpolated along that
// edge: here UV and colour are linear in position, so each must equal what the
// position says it should. The cap's own vertices, which face along the plane,
// take the cut edge's colour too, but the cap's unwrap for their UV
// (ACapMeetsItsAnchorFaceInPhase and its neighbours below).
TEST(MeshPlaneCut, CutEdgesInterpolateEveryAttribute)
{
    const MeshCutResult cut = CutMeshByPlane(UnitCube(), KeepAboveX(0.25f));
    size_t onCut = 0;
    size_t onCap = 0;
    ASSERT_TRUE(cut.Mesh.HasColors());
    for (size_t i = 0; i < cut.Mesh.Vertices.size(); ++i)
    {
        const SplineVertex& v = cut.Mesh.Vertices[i];
        if (std::abs(v.Position.x - 0.25f) > 1e-6f)
            continue;
        if (v.Normal.x < -0.5f)
        {
            ++onCap;
            EXPECT_NEAR(cut.Mesh.Colors[i].x, v.Position.x, 1e-5f);
            EXPECT_NEAR(cut.Mesh.Colors[i].y, v.Position.y, 1e-5f);
            EXPECT_NEAR(cut.Mesh.Colors[i].z, v.Position.z, 1e-5f);
            continue;
        }
        ++onCut;
        EXPECT_NEAR(v.UV.x, v.Position.x + v.Position.z, 1e-5f);
        EXPECT_NEAR(v.UV.y, v.Position.y, 1e-5f);
        EXPECT_NEAR(cut.Mesh.Colors[i].x, v.Position.x, 1e-5f);
        EXPECT_NEAR(cut.Mesh.Colors[i].y, v.Position.y, 1e-5f);
        EXPECT_NEAR(cut.Mesh.Colors[i].z, v.Position.z, 1e-5f);
    }
    // Each of the four side faces is two triangles, and the plane crosses its
    // two outer edges and its diagonal: three points a face; the cap has its
    // own copy of the eight outline points.
    EXPECT_EQ(onCut, 12u);
    EXPECT_EQ(onCap, 8u);
}

// An oblique plane through two opposite edges halves the cube; the cap is the
// diagonal rectangle, ear-clipped, and the result is still closed.
TEST(MeshPlaneCut, AnObliquePlaneLeavesAClosedWedge)
{
    const MeshCutResult cut = CutMeshByPlane(UnitCube(), {V3(1, 1, 0), 1.0f});
    EXPECT_EQ(cut.CappedLoops, 1u);
    EXPECT_EQ(cut.OpenLoops, 0u);
    EXPECT_NEAR(Volume(cut.Mesh), 0.5f, 1e-5f);
    EXPECT_TRUE(IsWatertight(cut.Mesh));
}

// A piece with a hole through it, cut across the hole: the plane crosses two
// loops, the outline and the hole's rim, and the cap is the ring between them.
TEST(MeshPlaneCut, AHollowPieceIsCappedAroundItsHole)
{
    const PieceMesh tube = HollowTube();
    ASSERT_NEAR(Volume(tube), 0.75f, 1e-5f);
    ASSERT_TRUE(IsWatertight(tube));

    const MeshCutResult cut = CutMeshByPlane(tube, KeepAboveX(0.25f));
    EXPECT_EQ(cut.CappedLoops, 2u);
    EXPECT_EQ(cut.OpenLoops, 0u);
    EXPECT_NEAR(Volume(cut.Mesh), 0.75f * 0.75f, 1e-5f);
    EXPECT_NEAR(AreaOnPlaneX(cut.Mesh, 0.25f), 0.75f, 1e-5f) << "the hole must stay open";
    EXPECT_TRUE(IsWatertight(cut.Mesh));
}

// A mesh with a face missing where the plane crosses it cannot be capped: the
// cut edges form a chain that does not close, which is reported, and nothing
// is laid on the plane.
TEST(MeshPlaneCut, AnOpenMeshReportsAndDoesNotCap)
{
    PieceMesh open = UnitCube();
    // Drop the +Y face: its two triangles are the fourth quad's.
    open.Indices.erase(open.Indices.begin() + 18, open.Indices.begin() + 24);

    const MeshCutResult cut = CutMeshByPlane(open, KeepAboveX(0.25f));
    EXPECT_EQ(cut.OpenLoops, 1u);
    EXPECT_EQ(cut.CappedLoops, 0u);
    size_t capTriangles = 0;
    EXPECT_EQ(AreaOnPlaneX(cut.Mesh, 0.25f, &capTriangles), 0.0f);
    EXPECT_EQ(capTriangles, 0u);
}

// A plane that removes nothing hands the mesh back bit for bit, so a piece a
// join never reaches is not re-emitted with drifted vertices.
TEST(MeshPlaneCut, APlaneThatMissesReturnsTheMeshBitIdentical)
{
    const PieceMesh cube = UnitCube();
    const MeshCutResult cut = CutMeshByPlane(cube, KeepAboveX(-1.0f));
    ASSERT_EQ(cut.Mesh.Vertices.size(), cube.Vertices.size());
    ASSERT_EQ(cut.Mesh.Indices, cube.Indices);
    EXPECT_EQ(std::memcmp(cut.Mesh.Vertices.data(), cube.Vertices.data(),
                          cube.Vertices.size() * sizeof(SplineVertex)),
              0);
    EXPECT_EQ(cut.CappedLoops, 0u);
    EXPECT_EQ(cut.OpenLoops, 0u);
}

// A cap is unwrapped planar in its own plane at the texel density of the faces
// it closes, whatever islands those faces sit in: any two cap vertices are as
// far apart in UV as in metres, and the tangent frame follows the UV.
TEST(MeshPlaneCut, ACapIsUnwrappedInItsPlaneAtThePiecesTexelDensity)
{
    const V3 normal = V3(-1.0f, -0.3f, -0.5f).Normalize();
    const MeshCutResult cut =
        CutMeshByPlane(UnwrappedCube(), {normal, V3::Dot(normal, V3(0.5f, 0.5f, 0.5f))});
    ASSERT_EQ(cut.CappedLoops, 1u);
    std::vector<const SplineVertex*> cap;
    for (const SplineVertex& v : cut.Mesh.Vertices)
    {
        if (V3::Dot(v.Normal, normal) > 0.999f)
            cap.push_back(&v);
    }
    ASSERT_GE(cap.size(), 3u);
    for (size_t i = 0; i < cap.size(); ++i)
    {
        const V3 tangent(cap[i]->Tangent.x, cap[i]->Tangent.y, cap[i]->Tangent.z);
        EXPECT_NEAR(V3::Dot(tangent, normal), 0.0f, 1e-5f);
        const V3 bitangent = V3::Cross(normal, tangent) * cap[i]->Tangent.w;
        for (size_t j = i + 1u; j < cap.size(); ++j)
        {
            const V3 dp = cap[j]->Position - cap[i]->Position;
            const V2 duv = cap[j]->UV - cap[i]->UV;
            EXPECT_NEAR(duv.x, V3::Dot(dp, tangent), 1e-4f) << i << "," << j;
            EXPECT_NEAR(duv.y, V3::Dot(dp, bitangent), 1e-4f) << i << "," << j;
        }
    }
}

// A kit wall whose top, bottom and ends are palette-mapped and whose two long
// faces carry the unwrap, cut across its length as a mitre cuts it: most cut
// edges lie on palette faces and change no UV (eight of twelve here, as on the
// castle kit's wall), and the cap still reads at the
// long faces' density, continuing their texture, rather than collapsing onto
// the palette texel.
TEST(MeshPlaneCut, APaletteFaceDoesNotSetTheCapsTexelDensity)
{
    PieceMesh wall;
    AddUnwrappedQuad(wall, V3(0, 0, 0), V3(0, 1, 0), V3(0, 1, 1), V3(0, 0, 1), V3(-1, 0, 0), 2.0f);
    MakeLastQuadPalette(wall);
    AddUnwrappedQuad(wall, V3(1, 0, 0), V3(1, 0, 1), V3(1, 1, 1), V3(1, 1, 0), V3(1, 0, 0), 4.0f);
    MakeLastQuadPalette(wall);
    // Top and bottom in two strips each, as a kit's bevelled top is, so the
    // palette faces hold most of the cut edges.
    for (const float32 z : {0.0f, 0.5f})
    {
        AddUnwrappedQuad(wall, V3(0, 0, z), V3(0, 0, z + 0.5f), V3(1, 0, z + 0.5f), V3(1, 0, z),
                         V3(0, -1, 0), 6.0f);
        MakeLastQuadPalette(wall);
        AddUnwrappedQuad(wall, V3(0, 1, z), V3(1, 1, z), V3(1, 1, z + 0.5f), V3(0, 1, z + 0.5f),
                         V3(0, 1, 0), 8.0f);
        MakeLastQuadPalette(wall);
    }
    AddUnwrappedQuad(wall, V3(0, 0, 0), V3(1, 0, 0), V3(1, 1, 0), V3(0, 1, 0), V3(0, 0, -1), 10.0f);
    AddUnwrappedQuad(wall, V3(0, 0, 1), V3(0, 1, 1), V3(1, 1, 1), V3(1, 0, 1), V3(0, 0, 1), 12.0f);

    const V3 normal = V3(-1.0f, 0.0f, -0.3f).Normalize();
    const MeshCutResult cut =
        CutMeshByPlane(wall, {normal, V3::Dot(normal, V3(0.5f, 0.5f, 0.5f))});
    ASSERT_EQ(cut.CappedLoops, 1u);
    std::vector<const SplineVertex*> cap;
    for (const SplineVertex& v : cut.Mesh.Vertices)
    {
        if (V3::Dot(v.Normal, normal) > 0.999f)
            cap.push_back(&v);
    }
    ASSERT_GE(cap.size(), 3u);
    float32 widest = 0.0f;
    for (size_t i = 0; i < cap.size(); ++i)
    {
        for (size_t j = i + 1u; j < cap.size(); ++j)
        {
            const float32 metres = (cap[j]->Position - cap[i]->Position).Length();
            EXPECT_NEAR((cap[j]->UV - cap[i]->UV).Length(), metres, 1e-4f) << i << "," << j;
            widest = std::max(widest, metres);
        }
    }
    EXPECT_GT(widest, 0.5f) << "the fixture must span the cap";
}

// The cap continues one long face's unwrap across the edge they share, so the
// texture's courses meet that edge in phase: every cap vertex on the edge has
// the UV the face has there. The castle kit's imported V falls as height rises
// and a kit may lay U vertically; a cap laid on a fixed in-plane frame instead
// mirrors or swaps them, a constant jog along the whole edge on a periodic
// brick texture.
TEST(MeshPlaneCut, ACapMeetsItsAnchorFaceInPhase)
{
    for (const LongFaceUnwrap unwrap : {LongFaceUnwrap::VFallsWithHeight, LongFaceUnwrap::UVertical})
    {
        const CutPlane plane = KitWallMitre();
        const MeshCutResult cut = CutMeshByPlane(KitWall(unwrap), plane);
        ASSERT_EQ(cut.CappedLoops, 1u);
        const std::vector<const SplineVertex*> cap = CapVertices(cut.Mesh, plane.Normal);
        ASSERT_GE(cap.size(), 4u);
        bool inPhase = false;
        for (const float32 z : {0.0f, 0.5f})
        {
            const float32 island = z == 0.0f ? 3.0f : 7.0f;
            size_t onEdge = 0;
            bool matches = true;
            for (const SplineVertex* v : cap)
            {
                if (std::abs(v->Position.z - z) > 1e-5f)
                    continue;
                ++onEdge;
                matches = matches &&
                          (v->UV - LongFaceUV(unwrap, v->Position, island)).Length() < 1e-4f;
            }
            inPhase = inPhase || (onEdge >= 2u && matches);
        }
        EXPECT_TRUE(inPhase) << static_cast<int>(unwrap);
        // And the cap is still an isometric unwrap at the faces' density.
        for (size_t i = 0; i < cap.size(); ++i)
        {
            for (size_t j = i + 1u; j < cap.size(); ++j)
                EXPECT_NEAR((cap[j]->UV - cap[i]->UV).Length(),
                            (cap[j]->Position - cap[i]->Position).Length(), 1e-4f)
                    << static_cast<int>(unwrap) << " " << i << "," << j;
        }
    }
}

// Which face anchors the cap is a property of the piece, not of the order its
// triangles are listed in: the same wall listed back to front gets the same
// cap, vertex for vertex.
TEST(MeshPlaneCut, TheCapsAnchorDoesNotDependOnTriangleOrder)
{
    const CutPlane plane = KitWallMitre();
    const MeshCutResult forward = CutMeshByPlane(KitWall(LongFaceUnwrap::VFallsWithHeight), plane);
    const MeshCutResult backward =
        CutMeshByPlane(KitWall(LongFaceUnwrap::VFallsWithHeight, true), plane);
    const std::vector<const SplineVertex*> a = CapVertices(forward.Mesh, plane.Normal);
    const std::vector<const SplineVertex*> b = CapVertices(backward.Mesh, plane.Normal);
    ASSERT_EQ(a.size(), b.size());
    ASSERT_GE(a.size(), 4u);
    for (const SplineVertex* v : a)
    {
        const auto same = std::find_if(b.begin(), b.end(), [v](const SplineVertex* w)
                                       { return (w->Position - v->Position).Length() < 1e-5f; });
        ASSERT_NE(same, b.end());
        EXPECT_LT(((*same)->UV - v->UV).Length(), 1e-5f);
        EXPECT_LT(V3(((*same)->Tangent.x - v->Tangent.x), ((*same)->Tangent.y - v->Tangent.y),
                     ((*same)->Tangent.z - v->Tangent.z)).Length(), 1e-5f);
        EXPECT_EQ((*same)->Tangent.w, v->Tangent.w);
    }
}

