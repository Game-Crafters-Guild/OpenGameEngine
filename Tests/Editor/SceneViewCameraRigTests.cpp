#include <gtest/gtest.h>

#include <cmath>

#include "SceneView/SceneViewCameraRig.h"

#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"

// Planet-scale scene-view camera rig math. The controller's authoritative
// position/pivot are double and the view matrix is built straight from
// yaw/pitch (SceneViewCameraRig.h); these tests lock:
//   1. orbit closure and pivot stability at planetary coordinates,
//   2. the documented fails-before contrast against the legacy fp32 path
//      (MakeLookAtLH(eye, eye + look) direction quantization; fp32 orbit
//      reposition quantization),
//   3. the no-feel-change guard: at ordinary scene scales the new build
//      matches the legacy matrix within float noise.

using GameEngine::Editor::CameraRig::BuildViewMatrixLH;
using GameEngine::Editor::CameraRig::LookDirection;
using GameEngine::Editor::CameraRig::OrbitCameraPosition;
using GameEngine::Editor::CameraRig::OrbitPivotFromCamera;
using GameEngine::Editor::CameraRig::PanBasis;
using GameEngine::Editor::CameraRig::Vec3d;
using GameEngine::Mathematics::MakeLookAtLH;
using GameEngine::Mathematics::Matrix4x4;
using GameEngine::Mathematics::Vector3;

namespace
{

// Legacy fp32 view build: the exact construction the controller used —
// float sin/cos basis, then MakeLookAtLH(eye, eye + look, up), which
// re-derives the direction as (eye + look) - eye in fp32.
Matrix4x4 LegacyViewMatrix(const float eye[3], float yawDeg, float pitchDeg)
{
    const float yawR = yawDeg * 3.1415926535f / 180.0f;
    const float pitR = pitchDeg * 3.1415926535f / 180.0f;
    float look[3] = {std::cos(pitR) * std::cos(yawR), std::sin(pitR),
                     std::cos(pitR) * std::sin(yawR)};
    const float len = std::sqrt(look[0] * look[0] + look[1] * look[1] + look[2] * look[2]);
    look[0] /= len;
    look[1] /= len;
    look[2] /= len;
    const Vector3 eyeV(eye[0], eye[1], eye[2]);
    const Vector3 target(eyeV.x + look[0], eyeV.y + look[1], eyeV.z + look[2]);
    return MakeLookAtLH(eyeV, target, Vector3(0.0f, 1.0f, 0.0f));
}

// Angle between the view matrix's forward row and the exact double direction.
double ForwardAngularError(const float view[16], double yawDeg, double pitchDeg)
{
    const Vec3d f = LookDirection(yawDeg, pitchDeg);
    // Forward row of the LH view matrix (column-major): elements 2, 6, 10.
    const double vx = view[2];
    const double vy = view[6];
    const double vz = view[10];
    const double len = std::sqrt(vx * vx + vy * vy + vz * vz);
    double dot = (vx * f.X + vy * f.Y + vz * f.Z) / len;
    if (dot > 1.0)
        dot = 1.0;
    if (dot < -1.0)
        dot = -1.0;
    return std::acos(dot);
}

} // namespace

// ---- Orbit closure at planetary coordinates ---------------------------------

TEST(SceneViewCameraRig, OrbitClosure360AtPlanetRadius)
{
    // A full 360-degree orbit around a pivot at |pos| = 5e4 (R=50000 planet
    // surface) and at Earth radius must return the camera to its exact start.
    for (const double R : {5.0e4, 6.371e6})
    {
        const Vec3d pivot{1000.0, 25.0, R};
        const double dist = 10.0;
        const double pitch = -20.0;
        const double startYaw = 37.0;

        const Vec3d start = OrbitCameraPosition(pivot, startYaw, pitch, dist);
        Vec3d cam = start;
        for (int step = 1; step <= 360; ++step)
            cam = OrbitCameraPosition(pivot, startYaw + static_cast<double>(step), pitch, dist);
        // 360 one-degree steps land on startYaw + 360: same direction as
        // startYaw up to the sin/cos argument-reduction of 2*pi in double.
        const double err = std::sqrt((cam.X - start.X) * (cam.X - start.X) +
                                     (cam.Y - start.Y) * (cam.Y - start.Y) +
                                     (cam.Z - start.Z) * (cam.Z - start.Z));
        EXPECT_LT(err, 1.0e-6) << "orbit non-closure at |pos| " << R;
    }
}

TEST(SceneViewCameraRig, OrbitPivotRoundTripIsExact)
{
    // Pivot placed ahead of the camera, then the camera recomputed from that
    // pivot, must land back on the camera (the orbit-enter/leave identity).
    const Vec3d cam{-3.2e4, 850.0, 3.9e4};
    const double yaw = 122.5, pitch = -31.0, dist = 42.0;
    const Vec3d pivot = OrbitPivotFromCamera(cam, yaw, pitch, dist);
    const Vec3d back = OrbitCameraPosition(pivot, yaw, pitch, dist);
    EXPECT_NEAR(back.X, cam.X, 1.0e-9);
    EXPECT_NEAR(back.Y, cam.Y, 1.0e-9);
    EXPECT_NEAR(back.Z, cam.Z, 1.0e-9);
}

TEST(SceneViewCameraRig, PivotStableAcrossManyOrbitSteps)
{
    // The recurrence camera = pivot - look*dist never rewrites the pivot, so N
    // orbit steps followed by re-deriving the pivot from the final camera pose
    // must reproduce the original pivot to double precision at Earth radius.
    const Vec3d pivot{5.0e3, -120.0, 6.371e6};
    const double dist = 25.0;
    double yaw = 10.0;
    const double pitch = -45.0;
    Vec3d cam{};
    for (int step = 0; step < 3000; ++step)
    {
        yaw += 0.37;
        cam = OrbitCameraPosition(pivot, yaw, pitch, dist);
    }
    const Vec3d rederived = OrbitPivotFromCamera(cam, yaw, pitch, dist);
    EXPECT_NEAR(rederived.X, pivot.X, 1.0e-6);
    EXPECT_NEAR(rederived.Y, pivot.Y, 1.0e-6);
    EXPECT_NEAR(rederived.Z, pivot.Z, 1.0e-6);
}

// ---- Fails-before: the fp32 hazards this rig replaces -----------------------

TEST(SceneViewCameraRig, ViewDirectionQuantizationFailsBeforeAt5e4)
{
    // Hazard A (the reported rotation jitter): the legacy
    // MakeLookAtLH(eye, eye + look) build quantizes the view DIRECTION to
    // ULP(|eye|)-sized angular steps. The error is worst when a large eye
    // coordinate is TRANSVERSE to the look direction (its ~3.9 mm ULP at 5e4
    // lands across the unit direction, up to ~4e-3 rad ~ 0.22 deg ~ 4 px at
    // 60 deg FOV / 1080 px), so sweep the full yaw circle at a position that
    // is large on both horizontal axes. The double build stays sub-microradian.
    const float eyeF[3] = {35355.0f, 300.0f, 35355.0f}; // |eye| ~ 5e4
    const Vec3d eyeD{35355.0, 300.0, 35355.0};

    double maxLegacyErr = 0.0;
    double maxNewErr = 0.0;
    for (int i = 0; i < 1440; ++i)
    {
        const double yaw = 0.25 * static_cast<double>(i); // full circle
        const double pitch = -15.0;

        const Matrix4x4 legacy =
            LegacyViewMatrix(eyeF, static_cast<float>(yaw), static_cast<float>(pitch));
        maxLegacyErr = std::max(maxLegacyErr, ForwardAngularError(legacy.Data(), yaw, pitch));

        float fresh[16];
        BuildViewMatrixLH(eyeD, yaw, pitch, fresh);
        maxNewErr = std::max(maxNewErr, ForwardAngularError(fresh, yaw, pitch));
    }

    EXPECT_GT(maxLegacyErr, 5.0e-4) << "legacy path no longer shows the hazard this rig fixes"
                                       " — revisit whether the rig is still needed";
    EXPECT_LT(maxNewErr, 1.0e-6) << "double view build must not quantize the direction";
}

TEST(SceneViewCameraRig, OrbitQuantizationFailsBeforeAt5e4)
{
    // Hazard C: recomputing camera = pivot - look*dist in fp32 lands the
    // position on the ~3.9 mm ULP grid of |pos| = 5e4 every frame of an orbit
    // drag; the double recurrence tracks the exact circle.
    const Vec3d pivot{0.0, 0.0, 5.0e4};
    const double dist = 10.0;
    const double pitch = -10.0;

    double maxF32Err = 0.0;
    for (int i = 0; i <= 500; ++i)
    {
        const double yaw = 45.0 + 0.02 * static_cast<double>(i);
        const Vec3d exact = OrbitCameraPosition(pivot, yaw, pitch, dist);

        // fp32 emulation of the old recurrence.
        const float yawR = static_cast<float>(yaw) * 3.1415926535f / 180.0f;
        const float pitR = static_cast<float>(pitch) * 3.1415926535f / 180.0f;
        const float lx = std::cos(pitR) * std::cos(yawR);
        const float ly = std::sin(pitR);
        const float lz = std::cos(pitR) * std::sin(yawR);
        const float cx = static_cast<float>(pivot.X) - lx * static_cast<float>(dist);
        const float cy = static_cast<float>(pivot.Y) - ly * static_cast<float>(dist);
        const float cz = static_cast<float>(pivot.Z) - lz * static_cast<float>(dist);

        const double err = std::sqrt((cx - exact.X) * (cx - exact.X) +
                                     (cy - exact.Y) * (cy - exact.Y) +
                                     (cz - exact.Z) * (cz - exact.Z));
        maxF32Err = std::max(maxF32Err, err);
    }
    EXPECT_GT(maxF32Err, 5.0e-4) << "fp32 orbit recurrence no longer shows the hazard";
}

// ---- No-feel-change guard: byte-comparable at ordinary scales ---------------

TEST(SceneViewCameraRig, SmallSceneViewMatrixMatchesLegacyWithinFloatNoise)
{
    // No-feel-change guard, stated honestly: even at ordinary scene scales the
    // LEGACY build already carries its own fp32 direction quantization of
    // ~ULP(|eye|) (measured ~2.8e-5 rad at |eye| ~ 660 — a hundredth of a
    // pixel, invisible). So the new build must (a) match the legacy matrix
    // within that legacy noise band, scaled by the eye magnitude, and (b) be
    // the more exact of the two — its basis must track the exact double
    // direction to sub-microradian everywhere.
    const float eyes[][3] = {
        {0.0f, 3.0f, -10.0f}, // the seeded default camera
        {12.5f, 1.7f, 42.0f},
        {-250.0f, 80.0f, 610.0f},
        {950.0f, -40.0f, -1000.0f},
    };
    const float poses[][2] = {{90.0f, 0.0f}, {35.0f, -25.0f}, {200.0f, 45.0f}, {-80.0f, 80.0f}};

    for (const auto& eye : eyes)
    {
        const float maxAbsEye =
            std::max({std::fabs(eye[0]), std::fabs(eye[1]), std::fabs(eye[2])});
        // Legacy fp32 noise: the legacy basis quantizes by ~ULP(|eye|)
        // (~|eye| * 1.2e-7 per component), and the translation runs that basis
        // error through dot(basis, eye) — so its error grows ~quadratically
        // with |eye| (measured 1.5e-2 at |eye| ~ 660: angularly ~2e-5 rad,
        // a hundredth of a pixel). The new build is double-exact; the bound
        // here is the legacy path's own noise envelope.
        const float rotTol = 1.0e-5f + maxAbsEye * 3.0e-7f;
        const float transTol = 1.0e-3f + maxAbsEye * 5.0e-5f;
        for (const auto& pose : poses)
        {
            const Matrix4x4 legacy = LegacyViewMatrix(eye, pose[0], pose[1]);
            float fresh[16];
            BuildViewMatrixLH(Vec3d{eye[0], eye[1], eye[2]}, pose[0], pose[1], fresh);

            for (int i = 0; i < 16; ++i)
            {
                const bool translation = (i >= 12 && i <= 14);
                const float tol = translation ? transTol : rotTol;
                EXPECT_NEAR(fresh[i], legacy.Data()[i], tol)
                    << "element " << i << " diverges beyond legacy fp32 noise (eye " << eye[0]
                    << "," << eye[1] << "," << eye[2] << " yaw " << pose[0] << " pitch "
                    << pose[1] << ")";
            }
            // (b): the divergence above is legacy noise, not new-path error.
            EXPECT_LT(ForwardAngularError(fresh, pose[0], pose[1]), 1.0e-6);
        }
    }
}

TEST(SceneViewCameraRig, PanBasisMatchesLegacyBasis)
{
    // Pan feel guard: the double pan basis must match the old fp32 basis.
    const float poses[][2] = {{90.0f, 0.0f}, {10.0f, -60.0f}, {245.0f, 35.0f}, {-30.0f, 88.0f}};
    for (const auto& pose : poses)
    {
        const float yawR = pose[0] * 3.1415926535f / 180.0f;
        const float pitR = pose[1] * 3.1415926535f / 180.0f;
        float look[3] = {std::cos(pitR) * std::cos(yawR), std::sin(pitR),
                         std::cos(pitR) * std::sin(yawR)};
        float lookFlat[3] = {look[0], 0.0f, look[2]};
        const float lenFlat = lookFlat[0] * lookFlat[0] + lookFlat[2] * lookFlat[2];
        if (lenFlat > 1e-6f)
        {
            const float invLen = 1.0f / std::sqrt(lenFlat);
            lookFlat[0] *= invLen;
            lookFlat[2] *= invLen;
        }
        else
        {
            lookFlat[0] = 0.0f;
            lookFlat[2] = 1.0f;
        }
        const float right[3] = {lookFlat[2], 0.0f, -lookFlat[0]};
        float up[3] = {right[1] * look[2] - right[2] * look[1],
                       right[2] * look[0] - right[0] * look[2],
                       right[0] * look[1] - right[1] * look[0]};
        const float upLen = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
        up[0] /= upLen;
        up[1] /= upLen;
        up[2] /= upLen;

        Vec3d rightD{};
        Vec3d upD{};
        PanBasis(pose[0], pose[1], rightD, upD);
        EXPECT_NEAR(rightD.X, right[0], 1.0e-5);
        EXPECT_NEAR(rightD.Y, right[1], 1.0e-5);
        EXPECT_NEAR(rightD.Z, right[2], 1.0e-5);
        EXPECT_NEAR(upD.X, up[0], 1.0e-5);
        EXPECT_NEAR(upD.Y, up[1], 1.0e-5);
        EXPECT_NEAR(upD.Z, up[2], 1.0e-5);
    }
}

// ---- Earth-radius direction sanity ------------------------------------------

TEST(SceneViewCameraRig, ViewDirectionStaysExactAtEarthRadius)
{
    // At 6.4e6 the fp32 ULP is 0.5 m: the legacy build's direction error grows
    // to tens of degrees (unusable). The double build must stay exact.
    const Vec3d eyeD{2000.0, 100.0, 6.371e6};
    double maxErr = 0.0;
    for (int i = 0; i <= 100; ++i)
    {
        const double yaw = 0.5 * static_cast<double>(i);
        const double pitch = -30.0 + 0.25 * static_cast<double>(i);
        float view[16];
        BuildViewMatrixLH(eyeD, yaw, pitch, view);
        maxErr = std::max(maxErr, ForwardAngularError(view, yaw, pitch));
    }
    EXPECT_LT(maxErr, 1.0e-6);
}

TEST(SceneViewCameraRig, OrthographicWheelZoomUsesIntegerPixelLevels)
{
    using GameEngine::Editor::CameraRig::StepOrthographicPixelZoom;
    constexpr double raster = 259;
    EXPECT_DOUBLE_EQ(StepOrthographicPixelZoom(raster / 4, raster, 1), raster / 5);
    EXPECT_DOUBLE_EQ(StepOrthographicPixelZoom(raster / 5, raster, -1), raster / 4);
    EXPECT_DOUBLE_EQ(StepOrthographicPixelZoom(raster / 4.3, raster, 1), raster / 5);
    EXPECT_DOUBLE_EQ(StepOrthographicPixelZoom(raster / 4.3, raster, -1), raster / 4);
    EXPECT_DOUBLE_EQ(StepOrthographicPixelZoom(raster, raster, -1), raster * 2);
    EXPECT_DOUBLE_EQ(StepOrthographicPixelZoom(raster * 2, raster, 1), raster);
    EXPECT_DOUBLE_EQ(StepOrthographicPixelZoom(raster * 3, raster, 1), raster * 2);
    EXPECT_DOUBLE_EQ(StepOrthographicPixelZoom(raster * 3, raster, -1), raster * 4);
    EXPECT_DOUBLE_EQ(StepOrthographicPixelZoom(47, 0, 1), 47);
    EXPECT_DOUBLE_EQ(StepOrthographicPixelZoom(47, raster, 0), 47);
}
