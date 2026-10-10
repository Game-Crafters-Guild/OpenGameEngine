// Tests for Engine::Renderer::Camera projection behavior. Specifically guards
// the Perspective/Orthographic toggle: ComputeProjectionMatrix must branch on
// params.Perspective and produce a reverse-Z (LH, depth [1,0]) matrix in both
// modes. Regression coverage for the "Fix game camera projection toggle" change.

#include <gtest/gtest.h>

#include <cmath>

#include "Components/Rendering/Camera.h"
#include "Engine/Rendering/Camera.h"
#include "Mathematics/MatrixOps.h"

using GameEngine::Engine::Renderer::Camera;
using GameEngine::Mathematics::Matrix4x4;
using GameEngine::Mathematics::Vector4;

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kAspect = 16.0f / 9.0f;

float NdcDepthFromViewZ(const Matrix4x4& proj, float viewZ)
{
    Vector4 clip = proj.Transform(Vector4(0.0f, 0.0f, viewZ, 1.0f));
    return clip.z / clip.w;
}

} // namespace

TEST(CameraProjection, PerspectiveBranch_MatchesMakePerspective)
{
    Camera cam{};
    cam.params.Perspective = true;
    cam.params.FovY = 60.0f;
    cam.params.NearZ = 0.1f;
    cam.params.FarZ = 1000.0f;

    const Matrix4x4 actual = cam.ComputeProjectionMatrix(kAspect);
    const Matrix4x4 expected = GameEngine::Mathematics::MakePerspectiveLH_ZO_ReverseZ(
        kPi / 3.0f, kAspect, 0.1f, 1000.0f);

    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            EXPECT_NEAR(actual[col][row], expected[col][row], 1e-5f)
                << "Mismatch at [" << col << "][" << row << "]";
}

TEST(CameraProjection, FocalLengthAndVerticalFovRoundTrip)
{
    constexpr float sensorHeightMm = 24.0f;
    constexpr float focalLengthMm = 300.0f;

    const float fovY = GameEngine::Components::CameraVerticalFovFromFocalLengthMm(
        focalLengthMm, sensorHeightMm);
    EXPECT_NEAR(fovY, 4.5812f, 1e-3f);
    EXPECT_NEAR(GameEngine::Components::CameraFocalLengthMmFromVerticalFov(
                    fovY, sensorHeightMm),
                focalLengthMm, 1e-3f);
}

TEST(CameraProjection, OrthographicBranch_UsesOrthographicSize)
{
    Camera cam{};
    cam.params.Perspective = false;
    cam.params.OrthographicSize = 10.0f;
    cam.params.NearZ = 0.5f;
    cam.params.FarZ = 500.0f;

    const Matrix4x4 proj = cam.ComputeProjectionMatrix(kAspect);

    // Ortho structural markers: w_clip is constant (m[2][3]=0, m[3][3]=1).
    EXPECT_NEAR(proj[2][3], 0.0f, 1e-6f);
    EXPECT_NEAR(proj[3][3], 1.0f, 1e-6f);

    // halfHeight = OrthographicSize / 2 = 5. m[1][1] = 2 / (top-bottom) = 1/halfHeight = 0.2.
    EXPECT_NEAR(proj[1][1], 1.0f / 5.0f, 1e-5f);
    // halfWidth = halfHeight * aspect. m[0][0] = 2 / (right-left) = 1/halfWidth.
    const float halfWidth = 5.0f * kAspect;
    EXPECT_NEAR(proj[0][0], 1.0f / halfWidth, 1e-5f);
}

TEST(CameraProjection, BothBranches_AreReverseZ)
{
    // Reverse-Z contract: viewZ=near -> NDC depth 1.0, viewZ=far -> NDC depth 0.0.
    Camera cam{};
    cam.params.NearZ = 0.5f;
    cam.params.FarZ = 100.0f;

    cam.params.Perspective = true;
    cam.params.FovY = 60.0f;
    const Matrix4x4 persp = cam.ComputeProjectionMatrix(kAspect);
    EXPECT_NEAR(NdcDepthFromViewZ(persp, cam.params.NearZ), 1.0f, 1e-5f);
    EXPECT_NEAR(NdcDepthFromViewZ(persp, cam.params.FarZ), 0.0f, 1e-5f);

    cam.params.Perspective = false;
    cam.params.OrthographicSize = 10.0f;
    const Matrix4x4 ortho = cam.ComputeProjectionMatrix(kAspect);
    EXPECT_NEAR(NdcDepthFromViewZ(ortho, cam.params.NearZ), 1.0f, 1e-5f);
    EXPECT_NEAR(NdcDepthFromViewZ(ortho, cam.params.FarZ), 0.0f, 1e-5f);
}

TEST(CameraProjection, PerspectiveToggle_ProducesDifferentMatrices)
{
    // Guards against the original bug: the projection toggle was a no-op because
    // ComputeProjectionMatrix ignored params.Perspective. Flipping the toggle
    // must visibly change m[1][1] (FOV slope vs ortho half-height slope) and the
    // homogeneous-w row (perspective divide vs constant w).
    Camera cam{};
    cam.params.FovY = 60.0f;
    cam.params.OrthographicSize = 10.0f;
    cam.params.NearZ = 0.1f;
    cam.params.FarZ = 100.0f;

    cam.params.Perspective = true;
    const Matrix4x4 persp = cam.ComputeProjectionMatrix(kAspect);

    cam.params.Perspective = false;
    const Matrix4x4 ortho = cam.ComputeProjectionMatrix(kAspect);

    EXPECT_NE(persp[1][1], ortho[1][1]);
    EXPECT_NEAR(persp[2][3], 1.0f, 1e-6f); // perspective: w_clip = z_view
    EXPECT_NEAR(ortho[2][3], 0.0f, 1e-6f); // ortho:       w_clip = 1
    EXPECT_NEAR(persp[3][3], 0.0f, 1e-6f);
    EXPECT_NEAR(ortho[3][3], 1.0f, 1e-6f);
}

TEST(CameraProjection, AnamorphicSqueezeWidensOnlyPerspectiveHorizontalField)
{
    Camera cam{};
    cam.params.Perspective = true;
    cam.params.FovY = 60.0f;
    cam.params.NearZ = 0.1f;
    cam.params.FarZ = 100.0f;

    cam.params.AnamorphicSqueeze = 1.0f;
    const Matrix4x4 spherical = cam.ComputeProjectionMatrix(kAspect);
    cam.params.AnamorphicSqueeze = 2.0f;
    const Matrix4x4 anamorphic = cam.ComputeProjectionMatrix(kAspect);

    // Doubling the horizontal optical squeeze doubles tan(FovX/2), so the
    // horizontal projection scale halves. Vertical FOV and depth stay fixed.
    EXPECT_NEAR(anamorphic[0][0], spherical[0][0] * 0.5f, 1e-5f);
    EXPECT_NEAR(anamorphic[1][1], spherical[1][1], 1e-5f);
    EXPECT_NEAR(anamorphic[2][2], spherical[2][2], 1e-5f);

    cam.params.Perspective = false;
    cam.params.OrthographicSize = 10.0f;
    cam.params.AnamorphicSqueeze = 1.0f;
    const Matrix4x4 orthoSpherical = cam.ComputeProjectionMatrix(kAspect);
    cam.params.AnamorphicSqueeze = 2.0f;
    const Matrix4x4 orthoAnamorphic = cam.ComputeProjectionMatrix(kAspect);
    EXPECT_NEAR(orthoAnamorphic[0][0], orthoSpherical[0][0], 1e-6f);
    EXPECT_NEAR(orthoAnamorphic[1][1], orthoSpherical[1][1], 1e-6f);
}

TEST(CameraProjection, ZeroAspect_Clamped)
{
    // ComputeProjectionMatrix clamps aspect to a small positive epsilon so a
    // zero/negative aspect (degenerate viewport) doesn't produce NaN/Inf entries.
    Camera cam{};
    cam.params.Perspective = true;
    cam.params.FovY = 60.0f;
    cam.params.NearZ = 0.1f;
    cam.params.FarZ = 100.0f;

    const Matrix4x4 proj = cam.ComputeProjectionMatrix(0.0f);
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            EXPECT_TRUE(std::isfinite(proj[col][row]))
                << "Non-finite at [" << col << "][" << row << "]";
}
