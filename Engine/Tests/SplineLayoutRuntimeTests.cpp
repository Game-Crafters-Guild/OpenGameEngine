#include "SplineLayout/CenterlineSampling.h"
#include "SplineLayout/FenceLayout.h"
#include "SplineLayout/PieceEntity.h"
#include "SplineLayout/SeamShear.h"
#include "Components/Transform.h"

#include <gtest/gtest.h>

using namespace GameEngine;
namespace Layout = GameEngine::SplineLayout;
using V3 = Mathematics::Vector3;

TEST(SplineLayoutRuntime, FenceLayoutCrossesRuntimeLibraryBoundary)
{
    const std::vector<Layout::CenterSample> center = {
        {V3(0, 0, 0), V3(0, 1, 0)},
        {V3(0, 0, 6), V3(0, 1, 0)},
        {V3(6, 0, 6), V3(0, 1, 0)}};
    const float32 boundaries[] = {0, 1, 2};
    const Layout::FencePieceBounds pieces[] = {{V3(0, 1, 0), V3(0.1f, 1, 1.5f)}};
    Layout::FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = pieces;
    params.PostPitch = 3;
    const auto result = Layout::BuildFenceLayout(center, params);
    ASSERT_EQ(result.Spans.size(), 4u);
    ASSERT_EQ(result.Stations.size(), 5u);
    EXPECT_TRUE(result.Validation.empty());
    EXPECT_FLOAT_EQ(result.Stations.back().Pose.Base.x, 6);
    EXPECT_FLOAT_EQ(result.Stations.back().Pose.Base.z, 6);
    EXPECT_EQ(result.Spans.front().Run, 0u);
    EXPECT_EQ(result.Spans.back().Run, 1u);
}

TEST(SplineLayoutRuntime, SurfaceCallbackAndPoseHelpersNeedNoEditor)
{
    const std::vector<Layout::CenterSample> center = {
        {V3(0, 4, 0), V3(0, 1, 0)}, {V3(0, 4, 6), V3(0, 1, 0)}};
    Layout::TileLayoutParams params;
    params.Spacing = 2;
    int probes = 0;
    params.Probe = [&](const V3&, float32& altitude, V3& normal) {
        ++probes;
        altitude = 4;
        normal = V3(0, 1, 0);
        return true;
    };
    auto poses = Layout::BuildTilePoses(center, params);
    ASSERT_EQ(poses.size(), 4u);
    EXPECT_EQ(probes, 4);
    std::vector<V3> forwards;
    for (const auto& pose : poses)
    {
        EXPECT_FLOAT_EQ(pose.Base.y, 4);
        forwards.push_back(pose.Forward);
    }
    const auto shear = Layout::ComputeSeamShearFactors(forwards, 8);
    Layout::ApplySeamShear(poses, shear, V3(0, 0, 0), Layout::PieceAxis::Z);
    const auto identity = Mathematics::Matrix4x4::Identity();
    const auto inverse = Layout::InvertPlacerWorld(identity.Data());
    Components::Transform transform;
    Layout::WriteParentLocalPose(transform, inverse, poses.front(), Layout::PieceAxis::Z, 1);
    EXPECT_FLOAT_EQ(transform.GetPosition().y, 4);
    EXPECT_GT(Layout::CenterlineSampleCount(6), 2u);
}
