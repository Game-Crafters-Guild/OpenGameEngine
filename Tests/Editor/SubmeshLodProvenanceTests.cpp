// The Model Inspector's per-submesh LOD badge reports whether the renderer actually draws the
// chain the importer read. It is the only place an author sees that divergence, so the rule it
// reports is pinned here rather than only in a running editor.

#include "Inspectors/SubmeshLodProvenance.h"

#include "Assets/ModelAsset.h"

#include <gtest/gtest.h>

#include <limits>

using GameEngine::Mesh;
using GameEngine::Editor::ClassifySubmeshLod;
using GameEngine::Editor::SubmeshLodProvenance;

namespace
{

// A drawable triangle with a valid LOD0 box, the shape ResolveMeshLODGeometry needs before it
// will look at any lower level.
Mesh BaseMesh()
{
    Mesh mesh;
    mesh.Vertices.resize(3);
    mesh.Vertices[1].Position[0] = 2.0f;
    mesh.Vertices[2].Position[1] = 2.0f;
    mesh.Indices = {0, 1, 2};
    mesh.MinBounds[0] = 0.0f; mesh.MaxBounds[0] = 2.0f;
    mesh.MinBounds[1] = 0.0f; mesh.MaxBounds[1] = 2.0f;
    mesh.MinBounds[2] = 0.0f; mesh.MaxBounds[2] = 0.0f;
    return mesh;
}

// A level that owns its own vertex block, which is what the upload has to concatenate and is
// therefore the only kind of level the stream rules can refuse.
void AddOwnVertexLevel(Mesh& mesh)
{
    mesh.ExtraLODs.push_back({0, 1, 2});
    mesh.ExtraLODVertices.push_back(mesh.Vertices);
    mesh.AuthoredLODs = true;
}

// A level that reuses LOD0's vertices. The upload never has to merge streams for one of these.
void AddIndexOnlyLevel(Mesh& mesh)
{
    mesh.ExtraLODs.push_back({0, 1, 2});
    mesh.ExtraLODVertices.emplace_back();
    mesh.AuthoredLODs = true;
}

} // namespace

TEST(SubmeshLodProvenance, AMeshWithNoLowerLevelsIsLod0Only)
{
    EXPECT_EQ(ClassifySubmeshLod(BaseMesh()), SubmeshLodProvenance::Lod0Only);
}

TEST(SubmeshLodProvenance, AChainWithoutAuthoredProvenanceIsGenerated)
{
    Mesh mesh = BaseMesh();
    AddOwnVertexLevel(mesh);
    mesh.AuthoredLODs = false; // a generated attribute-honest shell owns vertices too
    EXPECT_EQ(ClassifySubmeshLod(mesh), SubmeshLodProvenance::Generated);
}

TEST(SubmeshLodProvenance, AnAdmittedAuthoredChainIsAuthored)
{
    Mesh mesh = BaseMesh();
    AddOwnVertexLevel(mesh);
    EXPECT_EQ(ClassifySubmeshLod(mesh), SubmeshLodProvenance::Authored);
}

// The divergence the badge exists for, and the one the inspector's own copy of the rule missed:
// no importer refuses an own-vertex chain over morph targets, but the upload does, so this mesh
// carries two CPU levels and draws one. A classifier keyed on vertex colour / UV1 / skinning /
// extra UV sets reports "authored" here and is wrong.
TEST(SubmeshLodProvenance, MorphTargetsDropAnOwnVertexChainAndTheBadgeSaysSo)
{
    Mesh mesh = BaseMesh();
    AddOwnVertexLevel(mesh);
    mesh.MorphTargets.resize(1);
    ASSERT_EQ(mesh.LODCount(), 2u) << "the CPU chain is intact";
    EXPECT_FALSE(mesh.HasColor0());
    EXPECT_FALSE(mesh.HasTexCoords1());
    EXPECT_FALSE(mesh.IsSkinned());
    EXPECT_TRUE(mesh.ExtraTexCoords.empty());
    EXPECT_EQ(ClassifySubmeshLod(mesh), SubmeshLodProvenance::AuthoredDropped);
}

// Index-only levels address LOD0's block, so no stream has to be merged and the chain survives
// streams that would refuse an own-vertex level. Over-refusing here would cost every authored
// index-only chain on a blend-shaped mesh its LODs.
TEST(SubmeshLodProvenance, MorphTargetsLeaveAnIndexOnlyChainAuthored)
{
    Mesh mesh = BaseMesh();
    AddIndexOnlyLevel(mesh);
    mesh.MorphTargets.resize(1);
    EXPECT_EQ(ClassifySubmeshLod(mesh), SubmeshLodProvenance::Authored);
}

TEST(SubmeshLodProvenance, ALowerLevelIndexingPastItsBlockDropsTheChain)
{
    Mesh mesh = BaseMesh();
    AddOwnVertexLevel(mesh);
    mesh.ExtraLODs[0][2] = 3; // one past the level's own three vertices
    EXPECT_EQ(ClassifySubmeshLod(mesh), SubmeshLodProvenance::AuthoredDropped);
}

TEST(SubmeshLodProvenance, ANonFiniteLowerPositionDropsTheChain)
{
    Mesh mesh = BaseMesh();
    AddOwnVertexLevel(mesh);
    mesh.ExtraLODVertices[0][0].Position[0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(ClassifySubmeshLod(mesh), SubmeshLodProvenance::AuthoredDropped);
}

// An empty level ends the admitted prefix. When it is the first one, nothing below LOD0 renders
// and the badge has to say so even though the CPU list shows two levels.
TEST(SubmeshLodProvenance, AnEmptyFirstLevelDropsTheChain)
{
    Mesh mesh = BaseMesh();
    mesh.ExtraLODs.emplace_back();
    mesh.ExtraLODVertices.emplace_back();
    AddOwnVertexLevel(mesh);
    ASSERT_EQ(mesh.LODCount(), 3u);
    EXPECT_EQ(ClassifySubmeshLod(mesh), SubmeshLodProvenance::AuthoredDropped);
}
