#include <cmath>
#include <cstring>
#include <gtest/gtest.h>

#include "Components/Hierarchy.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Placement/PieceEntity.h"
#include "Placement/TileLayout.h"

using GameEngine::Components::Parent;
using GameEngine::Components::Transform;
using GameEngine::Components::WorldTransform;
using GameEngine::ECS::Entity;
using GameEngine::ECS::World;
using GameEngine::Engine::Renderer::TransformHierarchySystem;
using GameEngine::Mathematics::Matrix4x4;
using GameEngine::Mathematics::Vector3;

using GameEngine::Editor::InvertPlacerWorld;
using GameEngine::Editor::MakePieceLabel;
using GameEngine::Editor::PieceAxis;
using GameEngine::Editor::TilePose;
using GameEngine::Editor::WriteParentLocalPose;

namespace
{

// The world-space pose write both controllers performed while pieces were root
// entities, for a piece laid along its local +Z. It is the invariant under
// test, not a copy of production code: the child's world pose after parenting
// must equal what this produced before it.
void WriteWorldPoseAsRoot(Transform& t, const TilePose& pose, float forwardScale)
{
    const Vector3 forward = pose.Forward * forwardScale;
    t.matrix[0] = pose.Right.x;     t.matrix[1] = pose.Right.y;     t.matrix[2] = pose.Right.z;     t.matrix[3] = 0.0f;
    t.matrix[4] = pose.Up.x;        t.matrix[5] = pose.Up.y;        t.matrix[6] = pose.Up.z;        t.matrix[7] = 0.0f;
    t.matrix[8] = forward.x;        t.matrix[9] = forward.y;        t.matrix[10] = forward.z;       t.matrix[11] = 0.0f;
    t.matrix[12] = pose.Position.x; t.matrix[13] = pose.Position.y; t.matrix[14] = pose.Position.z; t.matrix[15] = 1.0f;
}

// A pose with a genuinely oblique basis and an off-axis position, so a dropped
// or transposed multiply cannot pass by symmetry.
TilePose FixturePose()
{
    TilePose pose{};
    pose.Position = Vector3(3.25f, -1.5f, 7.75f);
    pose.Right = Vector3(0.8f, 0.0f, -0.6f);
    pose.Up = Vector3(0.12f, 0.98f, 0.16f);
    pose.Forward = Vector3(0.6f, -0.1f, 0.79f);
    pose.Base = Vector3(3.25f, -2.0f, 7.75f);
    return pose;
}

// Translation, a yaw, and a non-uniform scale: the three things a placer can
// carry that a naive "just subtract the origin" local-space fix gets wrong.
Matrix4x4 FixturePlacerWorld()
{
    const float yaw = 0.7f;
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    Matrix4x4 m;
    float* v = m.Data();
    v[0] = c * 2.0f;  v[1] = 0.0f;      v[2] = -s * 2.0f; v[3] = 0.0f;
    v[4] = 0.0f;      v[5] = 1.0f * 3.0f; v[6] = 0.0f;    v[7] = 0.0f;
    v[8] = s * 0.5f;  v[9] = 0.0f;      v[10] = c * 0.5f; v[11] = 0.0f;
    v[12] = -12.0f;   v[13] = 4.5f;     v[14] = 30.0f;    v[15] = 1.0f;
    return m;
}

void ExpectMatrixNear(const float* actual, const float* expected, float tolerance, const char* what)
{
    for (int i = 0; i < 16; ++i)
        EXPECT_NEAR(actual[i], expected[i], tolerance) << what << " element " << i;
}

} // namespace

// An identity placer is the degenerate case of the composition, so the
// parent-local write must reproduce the old root-entity write exactly — not
// merely to a tolerance. This is what pins "the pieces did not move".
TEST(PlacementPieceParenting, IdentityPlacerWritesTheSameBytesAsTheRootPose)
{
    const TilePose pose = FixturePose();

    Transform asRoot{};
    WriteWorldPoseAsRoot(asRoot, pose, 1.0f);

    float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    Transform asChild{};
    WriteParentLocalPose(asChild, InvertPlacerWorld(identity), pose, PieceAxis::Z, 1.0f);

    EXPECT_EQ(std::memcmp(asChild.matrix, asRoot.matrix, sizeof(asRoot.matrix)), 0);
}

// The general case: composing the placer's world matrix back onto the stored
// local must return the world pose layout produced before parenting.
TEST(PlacementPieceParenting, PlacerWorldTimesLocalRecoversTheWorldPose)
{
    const TilePose pose = FixturePose();
    const Matrix4x4 placerWorld = FixturePlacerWorld();

    Transform expected{};
    WriteWorldPoseAsRoot(expected, pose, 1.0f);

    Transform local{};
    WriteParentLocalPose(local, InvertPlacerWorld(placerWorld.Data()), pose, PieceAxis::Z, 1.0f);

    const Matrix4x4 recovered = placerWorld * Matrix4x4::FromColumnMajor(local.matrix);
    ExpectMatrixNear(recovered.Data(), expected.matrix, 1e-3f, "recovered world pose");

    // The local pose is genuinely different from the world one, so the test
    // above is not passing because the composition was skipped.
    EXPECT_NE(std::memcmp(local.matrix, expected.matrix, sizeof(expected.matrix)), 0);
}

// Fence spans stretch along the path; the stretch belongs in the pose, not in
// the parent composition, so it must survive the round trip identically.
TEST(PlacementPieceParenting, LengthScaleSurvivesTheParentComposition)
{
    const TilePose pose = FixturePose();
    const Matrix4x4 placerWorld = FixturePlacerWorld();
    constexpr float kLengthScale = 1.37f;

    Transform expected{};
    WriteWorldPoseAsRoot(expected, pose, kLengthScale);

    Transform local{};
    WriteParentLocalPose(local, InvertPlacerWorld(placerWorld.Data()), pose, PieceAxis::Z,
                         kLengthScale);

    const Matrix4x4 recovered = placerWorld * Matrix4x4::FromColumnMajor(local.matrix);
    ExpectMatrixNear(recovered.Data(), expected.matrix, 1e-3f, "recovered stretched pose");
}

// A piece whose long axis is local X is yawed onto the path, and the placer's
// own transform must not undo that: recomposing the child gives a world basis
// whose local +X is on travel carrying the stretch, and whose local +Z is
// across it. Reading the recovered COLUMNS rather than re-deriving the write
// keeps this a statement about the piece's axes, not a copy of the formula.
TEST(PlacementPieceParenting, PieceAxisSurvivesTheParentComposition)
{
    const TilePose pose = FixturePose();
    const Matrix4x4 placerWorld = FixturePlacerWorld();
    constexpr float kLengthScale = 1.37f;

    Transform local{};
    WriteParentLocalPose(local, InvertPlacerWorld(placerWorld.Data()), pose, PieceAxis::X,
                         kLengthScale);

    const Matrix4x4 recovered = placerWorld * Matrix4x4::FromColumnMajor(local.matrix);
    const float* r = recovered.Data();

    const Vector3 alongTravel = pose.Forward * kLengthScale;
    EXPECT_NEAR(r[0], alongTravel.x, 1e-3f);
    EXPECT_NEAR(r[1], alongTravel.y, 1e-3f);
    EXPECT_NEAR(r[2], alongTravel.z, 1e-3f);

    // +Z points LEFT of travel, unscaled: a yaw, not a mirror.
    const Vector3 acrossTravel = pose.Right * -1.0f;
    EXPECT_NEAR(r[8], acrossTravel.x, 1e-3f);
    EXPECT_NEAR(r[9], acrossTravel.y, 1e-3f);
    EXPECT_NEAR(r[10], acrossTravel.z, 1e-3f);

    // Up and the origin are the pose's, whichever axis the piece runs on.
    EXPECT_NEAR(r[5], pose.Up.y, 1e-3f);
    EXPECT_NEAR(r[12], pose.Position.x, 1e-3f);
    EXPECT_NEAR(r[14], pose.Position.z, 1e-3f);
}

// A zero-scaled placer has no inverse. The pieces are allowed to collapse with
// it; what they must never do is carry NaN or infinity into WorldTransform,
// where it would reach bounds and culling.
TEST(PlacementPieceParenting, DegeneratePlacerYieldsFiniteIdentityInverse)
{
    float zeroScale[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 6, 7, 1};
    const Matrix4x4 inverse = InvertPlacerWorld(zeroScale);

    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    ExpectMatrixNear(inverse.Data(), identity, 0.0f, "degenerate inverse");

    Transform local{};
    WriteParentLocalPose(local, inverse, FixturePose(), PieceAxis::Z, 1.0f);
    for (int i = 0; i < 16; ++i)
        EXPECT_TRUE(std::isfinite(local.matrix[i])) << "local element " << i;
}

TEST(PlacementPieceParenting, PieceLabelIsRoleThenOrdinal)
{
    EXPECT_STREQ(MakePieceLabel("Tile", 0u).value, "Tile 0");
    EXPECT_STREQ(MakePieceLabel("Span", 41u).value, "Span 41");
    // The buffer is fixed-size; a label must truncate rather than overrun.
    const auto longLabel = MakePieceLabel("AVeryLongRoleNameThatGoesOnAndOnAndOnPastSixtyFourBytesEasily", 7u);
    EXPECT_EQ(std::strlen(longLabel.value), sizeof(longLabel.value) - 1u);
}

// The two claims that only the real hierarchy system can settle: a parented
// piece lands on the same world pose it had as a root, and it FOLLOWS a placer
// that moves — without any controller rebuild in between.
TEST(PlacementPieceParenting, ChildFollowsPlacerThroughTheHierarchySystem)
{
    World world;

    const Matrix4x4 placerWorld = FixturePlacerWorld();
    Entity placer = world.Create();
    Transform placerLocal{};
    std::memcpy(placerLocal.matrix, placerWorld.Data(), sizeof(placerLocal.matrix));
    placer.Set(placerLocal);

    const TilePose pose = FixturePose();
    Transform expectedWorld{};
    WriteWorldPoseAsRoot(expectedWorld, pose, 1.0f);

    Entity piece = world.Create();
    Transform pieceLocal{};
    WriteParentLocalPose(pieceLocal, InvertPlacerWorld(placerWorld.Data()), pose, PieceAxis::Z,
                         1.0f);
    piece.Set(pieceLocal);
    piece.Set(Parent{placer.GetHandle()});
    world.ProcessCommands();

    TransformHierarchySystem hierarchy;
    hierarchy.Update(world, 0.0f);

    const auto* pieceWorld = world.GetComponent<WorldTransform>(piece.GetHandle());
    ASSERT_NE(pieceWorld, nullptr);
    ExpectMatrixNear(pieceWorld->matrix, expectedWorld.matrix, 1e-3f, "child world pose");

    // Move the placer. No rebuild runs; propagation alone must carry the piece.
    Matrix4x4 movedPlacer = placerWorld;
    movedPlacer.Data()[12] += 25.0f;
    movedPlacer.Data()[13] -= 4.0f;
    movedPlacer.Data()[14] += 9.5f;
    auto* placerTransform = world.GetComponentForWrite<Transform>(placer.GetHandle());
    ASSERT_NE(placerTransform, nullptr);
    std::memcpy(placerTransform->matrix, movedPlacer.Data(), sizeof(placerTransform->matrix));

    hierarchy.Update(world, 0.0f);

    const Matrix4x4 expectedMoved = movedPlacer * Matrix4x4::FromColumnMajor(pieceLocal.matrix);
    const auto* movedPieceWorld = world.GetComponent<WorldTransform>(piece.GetHandle());
    ASSERT_NE(movedPieceWorld, nullptr);
    ExpectMatrixNear(movedPieceWorld->matrix, expectedMoved.Data(), 1e-3f, "child world pose after placer move");

    // The piece really did move with the placer, rather than simply not being
    // re-evaluated: its world translation shifted by the placer's delta.
    EXPECT_NEAR(movedPieceWorld->matrix[12] - expectedWorld.matrix[12], 25.0f, 1e-3f);
    EXPECT_NEAR(movedPieceWorld->matrix[13] - expectedWorld.matrix[13], -4.0f, 1e-3f);
    EXPECT_NEAR(movedPieceWorld->matrix[14] - expectedWorld.matrix[14], 9.5f, 1e-3f);
}
