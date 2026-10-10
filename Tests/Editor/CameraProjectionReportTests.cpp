// The projection half of get_camera.
//
// The trap this locks: every projection in this engine is built from the
// VERTICAL field of view (MakePerspectiveLH_ZO_ReverseZ takes fovYRadians), and
// a measurement harness that reads one unqualified "fov" as horizontal converts
// pixels to metres aspect-ratio wide of the truth — silently, with plausible
// numbers. So the report names both angles, and these tests assert they are
// DIFFERENT and each matches its own axis. A regression that reported one angle
// twice, or swapped them, would still produce a well-formed response.
//
// The second trap: aspect is read out of the matrix, with the viewport pixels
// carried alongside as its provenance rather than used as its source. On the
// perspective path the two agree today — ResolveEditorSceneViewCameraAspect
// (SceneViewController.cpp:663) returns nullopt unless the view is orthographic
// — so the perspective case pins the SOURCE, guarding a future "just divide the
// pixels" simplification. The orthographic case pins the divergence itself:
// ortho is the one path where an aspect preset can override the viewport ratio,
// and it is also the path that reports no angles.

#include <gtest/gtest.h>

#include "DebugServer/CameraProjectionReport.h"

#include "Mathematics/MatrixOps.h"
#include "Engine/Rendering/Exposure.h"
#include <limits>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstring>

namespace
{
using GameEngine::Editor::DescribeCameraProjection;
using GameEngine::Mathematics::MakeOrthographicLH_ZO_ReverseZ;
using GameEngine::Mathematics::MakePerspectiveLH_ZO_ReverseZ;
using json = nlohmann::json;

constexpr float kDegToRad = 0.01745329251994329577f;
constexpr float kNear = 0.1f;
constexpr float kFar = 1000.0f;

json Perspective(float fovVerticalDeg, float aspect, unsigned w = 1920, unsigned h = 1080)
{
    const auto proj = MakePerspectiveLH_ZO_ReverseZ(fovVerticalDeg * kDegToRad, aspect, kNear, kFar);
    return DescribeCameraProjection(proj.Data(), w, h);
}

// tan(fovH/2) = tan(fovV/2) * aspect — the engine's own derivation
// (SceneViewController's screen-ray build).
float ExpectedHorizontalDeg(float fovVerticalDeg, float aspect)
{
    const float tanHalfY = std::tan(fovVerticalDeg * kDegToRad * 0.5f);
    return 2.0f * std::atan(tanHalfY * aspect) / kDegToRad;
}

TEST(CameraProjectionReport, PerspectiveReportsTheVerticalAngleItWasBuiltFrom)
{
    const json out = Perspective(60.0f, 16.0f / 9.0f);

    EXPECT_EQ(out["type"], "perspective");
    EXPECT_NEAR(out["fovVerticalDeg"].get<float>(), 60.0f, 1e-3f);
    EXPECT_NEAR(out["aspect"].get<float>(), 16.0f / 9.0f, 1e-4f);
}

TEST(CameraProjectionReport, HorizontalIsDerivedAtTheMatrixAspectAndDiffersFromVertical)
{
    const json out = Perspective(60.0f, 16.0f / 9.0f);

    const float vertical = out["fovVerticalDeg"].get<float>();
    const float horizontal = out["fovHorizontalDeg"].get<float>();

    EXPECT_NEAR(horizontal, ExpectedHorizontalDeg(60.0f, 16.0f / 9.0f), 1e-3f);
    // ~91.5 vs 60: the gap is the whole reason both are reported. A report that
    // echoed one angle into both fields would pass every equality check above.
    EXPECT_GT(horizontal, vertical + 25.0f);
}

TEST(CameraProjectionReport, SameVerticalAtADifferentAspectGivesADifferentHorizontal)
{
    const float wide = Perspective(60.0f, 16.0f / 9.0f)["fovHorizontalDeg"].get<float>();
    const float classic = Perspective(60.0f, 4.0f / 3.0f)["fovHorizontalDeg"].get<float>();
    const float square = Perspective(60.0f, 1.0f)["fovHorizontalDeg"].get<float>();

    EXPECT_GT(wide, classic);
    EXPECT_GT(classic, square);
    // Square is the one aspect where the two angles coincide — the case that
    // makes a swapped-axis bug invisible, hence the non-square cases above.
    EXPECT_NEAR(square, 60.0f, 1e-3f);
}

TEST(CameraProjectionReport, AspectIsReadFromTheMatrixAndPixelsAreOnlyItsProvenance)
{
    // A 4:3 matrix handed a 1920x1080 viewport. The Scene View cannot produce
    // this pairing on the perspective path today (the aspect override is
    // ortho-only), so what this pins is the SOURCE: the report divides the
    // matrix, never the pixels, and the pixels ride along unmodified.
    const json out = Perspective(50.0f, 4.0f / 3.0f, 1920, 1080);

    EXPECT_NEAR(out["aspect"].get<float>(), 4.0f / 3.0f, 1e-4f);
    EXPECT_EQ(out["viewportWidthPx"].get<unsigned>(), 1920u);
    EXPECT_EQ(out["viewportHeightPx"].get<unsigned>(), 1080u);
    EXPECT_NEAR(out["fovHorizontalDeg"].get<float>(), ExpectedHorizontalDeg(50.0f, 4.0f / 3.0f), 1e-3f);
}

TEST(CameraProjectionReport, ClampedFovRangeEndsRoundTrip)
{
    // SceneViewSettings clamps the setting to [20, 120]; both ends must survive
    // the matrix round-trip, since a wide-angle pose is where a misread FOV
    // costs the most.
    EXPECT_NEAR(Perspective(20.0f, 16.0f / 9.0f)["fovVerticalDeg"].get<float>(), 20.0f, 1e-3f);
    EXPECT_NEAR(Perspective(120.0f, 16.0f / 9.0f)["fovVerticalDeg"].get<float>(), 120.0f, 1e-3f);
}

TEST(CameraProjectionReport, OrthographicReportsHeightAndNoAngles)
{
    // 2D mode: halfH = distance * 0.5, halfW = halfH * aspect.
    constexpr float kDistance = 12.0f;
    constexpr float kAspect = 16.0f / 9.0f;
    const float halfH = kDistance * 0.5f;
    const float halfW = halfH * kAspect;
    const auto proj = MakeOrthographicLH_ZO_ReverseZ(-halfW, halfW, -halfH, halfH, kNear, kFar);

    const json out = DescribeCameraProjection(proj.Data(), 1920, 1080);

    EXPECT_EQ(out["type"], "orthographic");
    EXPECT_NEAR(out["orthoHeight"].get<float>(), kDistance, 1e-3f);
    EXPECT_NEAR(out["aspect"].get<float>(), kAspect, 1e-4f);
    // Absence, not zero: a 0-degree FOV would read as data and convert every
    // pixel measurement to nonsense.
    EXPECT_FALSE(out.contains("fovVerticalDeg"));
    EXPECT_FALSE(out.contains("fovHorizontalDeg"));
}

TEST(CameraProjectionReport, OrthographicAspectPresetDivergesFromTheViewportRatio)
{
    // The reachable divergence: an ortho view under a 4:3 aspect preset on a
    // 16:9 viewport. ResolveEditorSceneViewCameraAspect fires only for
    // orthographic views, so this is the one shape where reporting the pixel
    // ratio as `aspect` would be a wrong answer rather than a coincidence.
    constexpr float kDistance = 10.0f;
    constexpr float kPresetAspect = 4.0f / 3.0f;
    const float halfH = kDistance * 0.5f;
    const float halfW = halfH * kPresetAspect;
    const auto proj = MakeOrthographicLH_ZO_ReverseZ(-halfW, halfW, -halfH, halfH, kNear, kFar);

    const json out = DescribeCameraProjection(proj.Data(), 1920, 1080);

    EXPECT_NEAR(out["aspect"].get<float>(), kPresetAspect, 1e-4f);
    const float pixelRatio = 1920.0f / 1080.0f;
    EXPECT_GT(std::abs(out["aspect"].get<float>() - pixelRatio), 0.4f);
    EXPECT_EQ(out["viewportWidthPx"].get<unsigned>(), 1920u);
    EXPECT_EQ(out["viewportHeightPx"].get<unsigned>(), 1080u);
}

TEST(CameraProjectionReport, DegenerateMatrixReportsNothingRatherThanAGuess)
{
    float zeroed[16] = {};
    EXPECT_TRUE(DescribeCameraProjection(zeroed, 1920, 1080).is_null());
    EXPECT_TRUE(DescribeCameraProjection(nullptr, 1920, 1080).is_null());
}

TEST(CameraProjectionReport, NegativeDiagonalScalesReportNothingRatherThanANegativeAngle)
{
    // A mirrored or otherwise inverted projection is not one this code can
    // describe. atan(1/-x) is a perfectly well-formed negative angle, which is
    // the worst possible answer: it reads as data.
    auto proj = MakePerspectiveLH_ZO_ReverseZ(60.0f * kDegToRad, 16.0f / 9.0f, kNear, kFar);
    float flippedY[16];
    std::memcpy(flippedY, proj.Data(), sizeof(flippedY));
    flippedY[5] = -flippedY[5];
    EXPECT_TRUE(DescribeCameraProjection(flippedY, 1920, 1080).is_null());

    float flippedX[16];
    std::memcpy(flippedX, proj.Data(), sizeof(flippedX));
    flippedX[0] = -flippedX[0];
    EXPECT_TRUE(DescribeCameraProjection(flippedX, 1920, 1080).is_null());
}

TEST(CameraProjectionReport, GameCameraReportsDeclaredTranslationRollProjectionAndExposureMode)
{
    using namespace GameEngine;
    Rendering::CameraData camera{};
    // A 90-degree roll and translation; inversion must preserve both.
    const float worldData[16] = {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 12, 3, -4, 1};
    const auto world = Mathematics::Matrix4x4::FromColumnMajor(worldData);
    const auto view = Mathematics::Inverse(world);
    const auto projection = MakePerspectiveLH_ZO_ReverseZ(48.0f * kDegToRad, 16.0f / 9.0f, kNear, kFar);
    std::memcpy(camera.view, view.Data(), sizeof(camera.view));
    std::memcpy(camera.proj, projection.Data(), sizeof(camera.proj));
    camera.cameraPos[0] = 12;
    camera.cameraPos[1] = 3;
    camera.cameraPos[2] = -4;
    auto out = Editor::DescribeGameViewCamera(camera, Components::ExposureMode::Auto, 160, 90);
    EXPECT_EQ(out["position"], (json{12.0f, 3.0f, -4.0f}));
    ASSERT_EQ(out["worldTransform"].size(), 16u);
    for (int index = 0; index < 16; ++index)
        EXPECT_NEAR(out["worldTransform"][index].get<float>(), worldData[index], 1e-5f);
    EXPECT_NEAR(out["projection"]["fovVerticalDeg"].get<float>(), 48.0f, 1e-3f);
    EXPECT_EQ(out["projection"]["viewportWidthPx"], 160);
    EXPECT_EQ(out["projection"]["viewportHeightPx"], 90);
    EXPECT_EQ(out["exposure"]["mode"], "auto");
    EXPECT_TRUE(out["exposure"]["metered"].is_null()) << "settings are not a meter sample";
    for (const auto [mode, name] : {std::pair{Components::ExposureMode::Fixed, "fixed"},
                                  std::pair{Components::ExposureMode::Manual, "manual"},
                                  std::pair{Components::ExposureMode::Physical, "physical"}})
    {
        out = Editor::DescribeGameViewCamera(camera, mode, 160, 90);
        EXPECT_EQ(out["exposure"]["mode"], name);
        EXPECT_TRUE(out["exposure"]["metered"].is_null());
    }
}

TEST(CameraProjectionReport, MeteredExposureReportsItsOwnCompletedFrameAndRejectsInvalidSamples)
{
    using namespace GameEngine;
    const auto sample = Editor::DescribeMeteredExposure(0.25f, 37);
    EXPECT_FLOAT_EQ(sample["linearScale"].get<float>(), 0.25f);
    EXPECT_NEAR(sample["ev100"].get<float>(), Rendering::kNeutralExposureEV + 2.0f, 1e-5f);
    EXPECT_EQ(sample["frameIndex"], 37);
    for (float invalid : {0.0f, -1.0f, std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN()})
        EXPECT_TRUE(Editor::DescribeMeteredExposure(invalid, 38).is_null());
}
} // namespace
