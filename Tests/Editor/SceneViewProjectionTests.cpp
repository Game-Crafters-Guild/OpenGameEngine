#include <gtest/gtest.h>

#include <cmath>

#include "SceneView/SceneViewEvents.h"
#include "SceneView/SceneViewProjection.h"

#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"

using GameEngine::Editor::SceneTools::ProjectWorldToView;
using GameEngine::Editor::SceneTools::ScenePointerEvent;

using GameEngine::Mathematics::Vector2;
using GameEngine::Mathematics::Vector3;

namespace
{

constexpr float kPixelEpsilon = 1.0e-3f;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kViewWidth = 1600.0f;
constexpr float kViewHeight = 900.0f;

// The camera state SceneViewController::PopulatePointerCameraState writes:
// forward from yaw and pitch, right = up x forward, up = forward x right.
ScenePointerEvent MakeView(const Vector3& position, float yawDeg, float pitchDeg)
{
    const float yawR = yawDeg * kPi / 180.0f;
    const float pitR = pitchDeg * kPi / 180.0f;

    ScenePointerEvent view{};
    view.viewW = kViewWidth;
    view.viewH = kViewHeight;
    view.cameraPos = position;
    view.cameraForward = Vector3(std::cos(pitR) * std::cos(yawR),
                                 std::sin(pitR),
                                 std::cos(pitR) * std::sin(yawR)).NormalizeOrZero();
    view.cameraRight = Vector3::Cross(Vector3(0.0f, 1.0f, 0.0f), view.cameraForward).NormalizeOrZero();
    view.cameraUp = Vector3::Cross(view.cameraForward, view.cameraRight).NormalizeOrZero();
    return view;
}

ScenePointerEvent MakePerspectiveView(const Vector3& position, float yawDeg, float pitchDeg, float fovYDeg)
{
    ScenePointerEvent view = MakeView(position, yawDeg, pitchDeg);
    view.tanHalfFovY = std::tan(fovYDeg * kPi / 360.0f);
    return view;
}

} // namespace

// Yaw 90 looks down +Z with +X to the right: the forward axis lands on the
// view center and the frustum's top-right edge ray on the top-right corner.
TEST(SceneViewProjectionTests, PerspectiveMapsFrustumEdgesToViewCorners)
{
    const ScenePointerEvent view = MakePerspectiveView(Vector3(0.0f, 0.0f, 0.0f), 90.0f, 0.0f, 60.0f);
    const float depth = 25.0f;
    const float tanHalfX = view.tanHalfFovY * kViewWidth / kViewHeight;

    Vector2 center;
    ASSERT_TRUE(ProjectWorldToView(view, Vector3(0.0f, 0.0f, depth), center));
    EXPECT_NEAR(center.x, kViewWidth * 0.5f, kPixelEpsilon);
    EXPECT_NEAR(center.y, kViewHeight * 0.5f, kPixelEpsilon);

    Vector2 topRight;
    ASSERT_TRUE(ProjectWorldToView(view, Vector3(tanHalfX * depth, view.tanHalfFovY * depth, depth), topRight));
    EXPECT_NEAR(topRight.x, kViewWidth, kPixelEpsilon);
    EXPECT_NEAR(topRight.y, 0.0f, kPixelEpsilon);
}

// A pixel unprojected along its view ray, at any depth, projects back onto
// itself under a camera that is translated, yawed and pitched.
TEST(SceneViewProjectionTests, PerspectiveRoundTripsAnUnprojectedPixel)
{
    const ScenePointerEvent view = MakePerspectiveView(Vector3(120.0f, 35.0f, -40.0f), 33.0f, -27.0f, 75.0f);
    const float tanHalfX = view.tanHalfFovY * kViewWidth / kViewHeight;
    const float pixelX = 311.0f;
    const float pixelY = 702.0f;
    const float ndcX = 2.0f * pixelX / kViewWidth - 1.0f;
    const float ndcY = 1.0f - 2.0f * pixelY / kViewHeight;

    for (const float depth : {0.5f, 12.0f, 400.0f})
    {
        const Vector3 world = view.cameraPos + view.cameraRight * (ndcX * tanHalfX * depth) +
                              view.cameraUp * (ndcY * view.tanHalfFovY * depth) + view.cameraForward * depth;
        Vector2 pixel;
        ASSERT_TRUE(ProjectWorldToView(view, world, pixel)) << "depth " << depth;
        EXPECT_NEAR(pixel.x, pixelX, 0.01f) << "depth " << depth;
        EXPECT_NEAR(pixel.y, pixelY, 0.01f) << "depth " << depth;
    }
}

// A point behind a perspective camera, or nearer than 1 cm, does not project
// and leaves the output untouched.
TEST(SceneViewProjectionTests, PerspectiveRefusesPointsBehindOrAtTheCamera)
{
    const ScenePointerEvent view = MakePerspectiveView(Vector3(0.0f, 0.0f, 0.0f), 90.0f, 0.0f, 60.0f);
    Vector2 pixel(-7.0f, -7.0f);

    EXPECT_FALSE(ProjectWorldToView(view, Vector3(0.0f, 0.0f, -5.0f), pixel));
    EXPECT_FALSE(ProjectWorldToView(view, Vector3(0.0f, 0.0f, 0.005f), pixel));
    EXPECT_EQ(pixel.x, -7.0f);
    EXPECT_EQ(pixel.y, -7.0f);
    EXPECT_TRUE(ProjectWorldToView(view, Vector3(0.0f, 0.0f, 0.02f), pixel));
}

// Orthographic: orthoHeight is the full vertical extent, the depth along the
// view is ignored (a point behind the camera plane still projects), and a
// non-positive height refuses.
TEST(SceneViewProjectionTests, OrthographicMapsTheExtentToTheViewEdges)
{
    ScenePointerEvent view = MakeView(Vector3(0.0f, 0.0f, 0.0f), 90.0f, 0.0f);
    view.orthoHeight = 10.0f;
    const float halfW = 5.0f * kViewWidth / kViewHeight;

    for (const float depth : {-3.0f, 50.0f})
    {
        Vector2 bottomLeft;
        ASSERT_TRUE(ProjectWorldToView(view, Vector3(-halfW, -5.0f, depth), bottomLeft));
        EXPECT_NEAR(bottomLeft.x, 0.0f, kPixelEpsilon);
        EXPECT_NEAR(bottomLeft.y, kViewHeight, kPixelEpsilon);
    }

    view.orthoHeight = 0.0f;
    Vector2 pixel;
    EXPECT_FALSE(ProjectWorldToView(view, Vector3(0.0f, 0.0f, 10.0f), pixel));
}

TEST(SceneViewProjectionTests, ViewWithoutAreaRefuses)
{
    ScenePointerEvent view = MakePerspectiveView(Vector3(0.0f, 0.0f, 0.0f), 90.0f, 0.0f, 60.0f);
    view.viewH = 0.0f;
    Vector2 pixel;
    EXPECT_FALSE(ProjectWorldToView(view, Vector3(0.0f, 0.0f, 10.0f), pixel));
}
