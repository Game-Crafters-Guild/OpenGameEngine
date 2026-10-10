#include "Assets/MeshLODGeometry.h"
#include "Assets/MeshLODGenerator.h"
#include "Assets/MeshLODCache.h"
#include "Assets/ModelAsset.h"
#include <gtest/gtest.h>
#include <limits>

using namespace GameEngine;
namespace {
Mesh BoundsMesh() {
    Mesh m;
    m.Vertices.resize(3);
    m.Vertices[0].Position[0] = 1.0f;
    m.Vertices[1].Position[0] = 3.0f;
    m.Vertices[2].Position[1] = 2.0f;
    m.Indices = {0, 1, 2};
    m.MinBounds[0] = 0.0f; m.MaxBounds[0] = 4.0f;
    m.MinBounds[1] = 0.0f; m.MaxBounds[1] = 2.0f;
    m.MinBounds[2] = -1.0f; m.MaxBounds[2] = 1.0f;
    return m;
}
void AddOwn(Mesh& m, float maxX) {
    m.ExtraLODs.push_back({0, 1, 2});
    m.ExtraLODVertices.push_back(m.Vertices);
    m.ExtraLODVertices.back()[1].Position[0] = maxX;
}
}

TEST(MeshLODGeometry, KeepsReferenceCenterAndShrinksWhenLowerGeometryChanges) {
    Mesh m = BoundsMesh();
    AddOwn(m, 9.0f);
    const auto grown = ResolveMeshLODGeometry(m);
    ASSERT_EQ(grown.LevelCount, 2u);
    EXPECT_EQ(grown.Issue, MeshLODGeometryIssue::None);
    EXPECT_FLOAT_EQ(grown.Bounds.center.x, 2.0f);
    EXPECT_FLOAT_EQ(grown.Bounds.halfExtents.x, 7.0f);
    EXPECT_FLOAT_EQ(grown.ReferenceBounds.halfExtents.x, 2.0f);
    EXPECT_FLOAT_EQ(m.MaxBounds[0], 4.0f);
    m.ExtraLODVertices[0][1].Position[0] = 5.0f;
    EXPECT_FLOAT_EQ(ResolveMeshLODGeometry(m).Bounds.halfExtents.x, 3.0f);
    m.ExtraLODs.clear();
    EXPECT_FLOAT_EQ(ResolveMeshLODGeometry(m).Bounds.halfExtents.x, 2.0f);
}

TEST(MeshLODGeometry, IndexOnlyAndBeyondCapacityBlocksDoNotGrowBounds) {
    Mesh m = BoundsMesh();
    for (uint32 i = 1; i < MeshLODConfig::kMaxLODs; ++i) {
        m.ExtraLODs.push_back({0, 1, 2});
        m.ExtraLODVertices.emplace_back();
    }
    AddOwn(m, 900.0f);
    m.ExtraLODVertices.back()[0].Position[0] = std::numeric_limits<float>::quiet_NaN();
    const auto g = ResolveMeshLODGeometry(m);
    EXPECT_EQ(g.LevelCount, MeshLODConfig::kMaxLODs);
    EXPECT_EQ(g.Issue, MeshLODGeometryIssue::None);
    EXPECT_FLOAT_EQ(g.Bounds.halfExtents.x, 2.0f);
}

TEST(MeshLODGeometry, EmptyLevelEndsPrefixWithoutRenumbering) {
    Mesh m = BoundsMesh();
    AddOwn(m, 5.0f);
    m.ExtraLODs.emplace_back();
    m.ExtraLODVertices.emplace_back();
    AddOwn(m, 99.0f);
    const auto g = ResolveMeshLODGeometry(m);
    EXPECT_EQ(g.LevelCount, 2u);
    EXPECT_EQ(g.Issue, MeshLODGeometryIssue::EmptyLevel);
    EXPECT_FLOAT_EQ(g.Bounds.halfExtents.x, 3.0f);
}

TEST(MeshLODGeometry, UnsupportedStreamsUseUnexpandedBase) {
    for (int stream = 0; stream < 3; ++stream) {
        Mesh m = BoundsMesh();
        AddOwn(m, 90.0f);
        if (stream == 0) m.Color0.resize(m.Vertices.size() * 4, 1.0f);
        if (stream == 1) m.TexCoords1.resize(m.Vertices.size() * 2, 0.0f);
        if (stream == 2) m.MorphTargets.resize(1);
        const auto g = ResolveMeshLODGeometry(m);
        EXPECT_EQ(g.LevelCount, 1u);
        EXPECT_EQ(g.Issue, MeshLODGeometryIssue::UnsupportedStreams);
        EXPECT_FLOAT_EQ(g.Bounds.halfExtents.x, 2.0f);
    }
}

TEST(MeshLODGeometry, InvalidLowerGeometryCannotPublishNonfiniteBounds) {
    for (int invalid = 0; invalid < 4; ++invalid) {
        Mesh m = BoundsMesh();
        AddOwn(m, 90.0f);
        if (invalid == 0) m.ExtraLODVertices[0][0].Position[0] = std::numeric_limits<float>::quiet_NaN();
        if (invalid == 1) m.ExtraLODVertices[0][0].Position[1] = std::numeric_limits<float>::infinity();
        if (invalid == 2) m.ExtraLODVertices[0][0].Position[2] = std::numeric_limits<float>::max();
        if (invalid == 3) m.ExtraLODs[0][2] = 3;
        const auto g = ResolveMeshLODGeometry(m);
        EXPECT_EQ(g.LevelCount, 1u);
        EXPECT_EQ(g.Issue, MeshLODGeometryIssue::InvalidLowerGeometry);
        EXPECT_FLOAT_EQ(g.Bounds.halfExtents.x, 2.0f);
    }
}

TEST(MeshLODGeometry, InvalidBaseBoundsRefuseAndDegenerateReferenceStaysValid) {
    EXPECT_EQ(ResolveMeshLODGeometry(Mesh{}).LevelCount, 0u);
    Mesh m = BoundsMesh();
    m.MinBounds[0] = 8.0f;
    EXPECT_EQ(ResolveMeshLODGeometry(m).LevelCount, 0u);
    m.MinBounds[0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(ResolveMeshLODGeometry(m).Issue, MeshLODGeometryIssue::InvalidBaseBounds);
    for (int axis = 0; axis < 3; ++axis) m.MinBounds[axis] = m.MaxBounds[axis] = 0;
    AddOwn(m, 8.0f);
    const auto g = ResolveMeshLODGeometry(m);
    EXPECT_EQ(g.LevelCount, 2u);
    EXPECT_FLOAT_EQ(g.ReferenceBounds.Radius(), 0.0f);
    EXPECT_GT(g.Bounds.Radius(), 8.0f);
}

TEST(MeshLODGeometry, EmptyBaseIndicesCannotAdmitLowerOnlyGeometry) {
    Mesh m = BoundsMesh();
    AddOwn(m, 90.0f);
    m.Indices.clear();
    EXPECT_EQ(ResolveMeshLODGeometry(m).LevelCount, 0u);
    ModelAsset model(GUID::Generate(), "empty-base.fbx");
    m.AuthoredLODs = true;
    model.SetMeshesForTest({m});
    model.GenerateLODs(MeshLODConfig{}, false);
    float minimum[3], maximum[3]; model.GetBoundingBox(minimum, maximum);
    EXPECT_FLOAT_EQ(minimum[0], 0.0f);
    EXPECT_FLOAT_EQ(maximum[0], 0.0f);
}

TEST(MeshLODGeometry, PostLoadReconcilesTransformedAggregateAndLaterRemoval) {
    const auto previous = GetLODImportSettings();
    struct Restore { LODImportSettings Value; ~Restore() { SetLODImportSettings(Value); } } restore{previous};
    auto settings = previous; settings.AutoGenerateOnImport = false; SetLODImportSettings(settings);
    Mesh m = BoundsMesh();
    AddOwn(m, 9.0f);
    m.SourceNodeIndex = 0;
    m.SourceNodeTransform[0] = 2.0f;
    m.SourceNodeTransform[12] = 10.0f;
    ModelAsset asset(GUID::Generate(), "bounds.obj");
    asset.SetMeshesForTest({m});
    asset.PostLoad();
    float lo[3], hi[3]; asset.GetBoundingBox(lo, hi);
    EXPECT_FLOAT_EQ(lo[0], 0.0f);
    EXPECT_FLOAT_EQ(hi[0], 28.0f);
    m.ExtraLODs.clear(); m.ExtraLODVertices.clear();
    asset.SetMeshesForTest({m}); asset.PostLoad(); asset.GetBoundingBox(lo, hi);
    EXPECT_FLOAT_EQ(lo[0], 10.0f); EXPECT_FLOAT_EQ(hi[0], 18.0f);
    asset.SetMeshesForTest({}); asset.PostLoad(); asset.GetBoundingBox(lo, hi);
    EXPECT_FLOAT_EQ(lo[0], 0.0f); EXPECT_FLOAT_EQ(hi[0], 0.0f);
}
