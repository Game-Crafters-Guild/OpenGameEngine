#include "Assets/HlodMeshMerge.h"
#include "Assets/ModelAsset.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>

using namespace GameEngine;
using namespace GameEngine::Hlod;

namespace {

// A single triangle in the XY plane: normal +Z, tangent +X (handedness +1).
Mesh MakeMergeTriangle() {
    Mesh mesh;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    const std::array<std::array<float, 3>, 3> pos = {{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}};
    for (const auto& p : pos) {
        Vertex v;
        v.Position[0] = p[0]; v.Position[1] = p[1]; v.Position[2] = p[2];
        v.Normal[2] = 1.0f;
        v.Tangent[0] = 1.0f; v.Tangent[3] = 1.0f;
        mesh.Vertices.push_back(v);
    }
    mesh.Indices = {0, 1, 2};
    return mesh;
}

// Column-major translation matrix.
void MakeTranslation(float x, float y, float z, float out[16]) {
    for (int i = 0; i < 16; ++i) out[i] = 0.0f;
    out[0] = out[5] = out[10] = out[15] = 1.0f;
    out[12] = x; out[13] = y; out[14] = z;
}

MergeMember Member(const Mesh& mesh, const GUID& mat, const float xf[16], uint32 lod = 0) {
    MergeMember m;
    m.Source = &mesh;
    m.MaterialGuid = mat;
    m.ChosenLod = lod;
    for (int i = 0; i < 16; ++i) m.Transform[i] = xf[i];
    return m;
}

GUID Mat(const char* s) { return GUID::Derive(GUID::Null(), s); }

} // namespace

TEST(HlodMeshMerge, BakesTranslatedPositions) {
    Mesh tri = MakeMergeTriangle();
    float xf[16];
    MakeTranslation(10.0f, 20.0f, 30.0f, xf);
    MergeMember members[] = {Member(tri, Mat("m/a"), xf)};

    MergedProxy proxy = MergeClusterMembers(members);
    ASSERT_EQ(proxy.Submeshes.size(), 1u);
    const Mesh& out = proxy.Submeshes[0];
    ASSERT_EQ(out.Vertices.size(), 3u);
    EXPECT_FLOAT_EQ(out.Vertices[0].Position[0], 10.0f);
    EXPECT_FLOAT_EQ(out.Vertices[0].Position[1], 20.0f);
    EXPECT_FLOAT_EQ(out.Vertices[0].Position[2], 30.0f);
    EXPECT_FLOAT_EQ(out.Vertices[1].Position[0], 11.0f);
    // Pure translation leaves the normal and tangent untouched.
    EXPECT_FLOAT_EQ(out.Vertices[0].Normal[2], 1.0f);
    EXPECT_FLOAT_EQ(out.Vertices[0].Tangent[3], 1.0f);
    // Winding preserved (positive determinant).
    EXPECT_EQ(out.Indices, (Vector<uint32>{0, 1, 2}));
    EXPECT_EQ(proxy.CoreVertexBytes, 3u * sizeof(Vertex));
}

TEST(HlodMeshMerge, NegativeDeterminantFlipsWindingAndTangentW) {
    Mesh tri = MakeMergeTriangle();
    // Mirror across X (scale x by -1) → det(R) < 0.
    float xf[16] = {0};
    xf[0] = -1.0f; xf[5] = 1.0f; xf[10] = 1.0f; xf[15] = 1.0f;
    MergeMember members[] = {Member(tri, Mat("m/a"), xf)};

    MergedProxy proxy = MergeClusterMembers(members);
    const Mesh& out = proxy.Submeshes[0];
    // Winding reversed: i0, i2, i1.
    EXPECT_EQ(out.Indices, (Vector<uint32>{0, 2, 1}));
    // Bitangent handedness flipped.
    EXPECT_FLOAT_EQ(out.Vertices[0].Tangent[3], -1.0f);
    // Position mirrored on X.
    EXPECT_FLOAT_EQ(out.Vertices[1].Position[0], -1.0f);
}

TEST(HlodMeshMerge, MultiMaterialProducesOneSubmeshPerMaterial) {
    Mesh a = MakeMergeTriangle();
    Mesh b = MakeMergeTriangle();
    Mesh c = MakeMergeTriangle();
    float id[16]; MakeTranslation(0, 0, 0, id);
    // Two members share material "m/a", one uses "m/b".
    MergeMember members[] = {
        Member(a, Mat("m/a"), id),
        Member(b, Mat("m/b"), id),
        Member(c, Mat("m/a"), id),
    };

    MergedProxy proxy = MergeClusterMembers(members);
    ASSERT_EQ(proxy.Submeshes.size(), 2u);
    ASSERT_EQ(proxy.SubmeshMaterials.size(), 2u);
    // Ascending GUID order; each submesh's MaterialIndex is its ordinal.
    EXPECT_EQ(proxy.Submeshes[0].MaterialIndex, 0u);
    EXPECT_EQ(proxy.Submeshes[1].MaterialIndex, 1u);
    // The "m/a" submesh merged two triangles (6 verts), "m/b" one (3 verts).
    const Mesh* matA = nullptr;
    const Mesh* matB = nullptr;
    for (size_t i = 0; i < proxy.Submeshes.size(); ++i) {
        if (proxy.SubmeshMaterials[i] == Mat("m/a")) matA = &proxy.Submeshes[i];
        if (proxy.SubmeshMaterials[i] == Mat("m/b")) matB = &proxy.Submeshes[i];
    }
    ASSERT_NE(matA, nullptr);
    ASSERT_NE(matB, nullptr);
    EXPECT_EQ(matA->Vertices.size(), 6u);
    EXPECT_EQ(matA->Indices.size(), 6u);
    EXPECT_EQ(matB->Vertices.size(), 3u);
    // Second member's indices rebased by the first member's vertex count.
    EXPECT_EQ(matA->Indices, (Vector<uint32>{0, 1, 2, 3, 4, 5}));
}

TEST(HlodMeshMerge, UsesCoarsestLodIndices) {
    Mesh mesh = MakeMergeTriangle();
    // Add a coarser LOD1 index buffer (a degenerate 1-tri subset for the test).
    mesh.ExtraLODs.push_back(Vector<uint32>{0, 1, 2});
    mesh.ExtraLODErrors.push_back(0.1f);
    mesh.ExtraLODSloppy.push_back(0);
    // Make LOD1 distinguishable: reverse index order in the source LOD.
    mesh.ExtraLODs[0] = Vector<uint32>{2, 1, 0};

    float id[16]; MakeTranslation(0, 0, 0, id);
    MergeMember lod0[] = {Member(mesh, Mat("m/a"), id, 0)};
    MergeMember lod1[] = {Member(mesh, Mat("m/a"), id, 1)};

    EXPECT_EQ(MergeClusterMembers(lod0).Submeshes[0].Indices, (Vector<uint32>{0, 1, 2}));
    EXPECT_EQ(MergeClusterMembers(lod1).Submeshes[0].Indices, (Vector<uint32>{2, 1, 0}));
}

// An own-vertex chosen level (authored LOD or generated attribute-honest shell)
// carries LOD-LOCAL indices into its own block: the bake must copy THAT block,
// not LOD0's vertices, or the local indices would address the wrong geometry.
TEST(HlodMeshMerge, OwnVertexChosenLodBakesItsBlock) {
    Mesh mesh = MakeMergeTriangle(); // LOD0: 3 verts at x = 0/1/0
    Vector<Vertex> shell(3);
    for (size_t i = 0; i < 3; ++i) {
        shell[i] = mesh.Vertices[i];
        shell[i].Position[0] += 100.0f; // distinctive block
    }
    mesh.ExtraLODVertices.push_back(shell);
    mesh.ExtraLODs.push_back({0u, 1u, 2u}); // LOD-local
    mesh.ExtraLODErrors = {0.5f};
    mesh.ExtraLODSloppy = {1u};

    float id[16]; MakeTranslation(0, 0, 0, id);
    MergeMember members[] = {Member(mesh, Mat("m/shell"), id, /*lod=*/1u)};

    MergedProxy proxy = MergeClusterMembers(members);
    ASSERT_EQ(proxy.Submeshes.size(), 1u);
    const Mesh& out = proxy.Submeshes[0];
    ASSERT_EQ(out.Vertices.size(), 3u) << "bake copies the shell block, not LOD0";
    EXPECT_FLOAT_EQ(out.Vertices[0].Position[0], 100.0f);
    EXPECT_FLOAT_EQ(out.Vertices[1].Position[0], 101.0f);
    EXPECT_FLOAT_EQ(out.Vertices[2].Position[0], 100.0f);
    EXPECT_EQ(out.Indices, (Vector<uint32>{0, 1, 2}));
}

TEST(HlodMeshMerge, NullSourceMembersSkipped) {
    Mesh tri = MakeMergeTriangle();
    float id[16]; MakeTranslation(0, 0, 0, id);
    MergeMember members[2];
    members[0] = Member(tri, Mat("m/a"), id);
    members[1].Source = nullptr; // skipped
    members[1].MaterialGuid = Mat("m/b");

    MergedProxy proxy = MergeClusterMembers(members);
    ASSERT_EQ(proxy.Submeshes.size(), 1u);
    EXPECT_EQ(proxy.SubmeshMaterials[0], Mat("m/a"));
}
