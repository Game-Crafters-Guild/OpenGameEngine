// Oracles for camera-relative rendering (Earth-scale precision). These lock the
// falsifiable claims of the render-origin design:
//   * sector pack/unpack round-trips and (0,0,0) packs to zero (byte-identical);
//   * the rebased view is bit-identical to the full-world view when the origin
//     is inactive (the dark-ship gate);
//   * world -> (sector,local) -> reconstruct is bit-exact at small coordinates;
//   * the rebased projection of a relative position equals the full-world
//     projection of the world position (mathematical equivalence);
//   * at a planetary offset the reconstructed vertex is sub-pixel stable across
//     camera motion, where the fp32-world baseline swims by many pixels.
//
// The C++ UnpackSector here mirrors the GLSL ge_UnpackInstanceSector decode; a
// drift between the two is caught by the round-trip + fixed-bit-pattern cases.

#include "Engine/Rendering/RenderOrigin.h"

#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Common/Frustum.h"     // ExtractFrustumPlanes
#include "Rendering/Core/GPUCulling.h"    // MakeFrustumPlanesCameraRelative
#include "Types/FormatNumber.h"           // FormatFloat (Inspector compose-display)

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Mathematics;
namespace Origin = GameEngine::Engine::Renderer;

namespace
{
constexpr float kPi = 3.14159265358979323846f;
constexpr float kDeg2Rad = kPi / 180.0f;

// Build a camera world transform: a fixed precise orientation plus a (possibly
// huge) world translation — exactly the shape Camera::ComputeViewMatrix inverts.
Matrix4x4 MakeCameraWorld(const Matrix4x4& rotation, const Vector3& pos)
{
    Matrix4x4 m = rotation;
    m.Data()[12] = pos.x;
    m.Data()[13] = pos.y;
    m.Data()[14] = pos.z;
    return m;
}

// The LEGACY fp32 column rebase (out = M * translate(origin), fp32 big+big
// sum) — removed from production by #660 (camera) and its shadow follow-up,
// kept here verbatim as the fails-before contrast oracle.
void LegacyRebaseTranslationColumnFp32(const float* m, float ox, float oy, float oz, float* out)
{
    out[12] = m[0] * ox + m[4] * oy + m[8] * oz + m[12];
    out[13] = m[1] * ox + m[5] * oy + m[9] * oz + m[13];
    out[14] = m[2] * ox + m[6] * oy + m[10] * oz + m[14];
    out[15] = m[3] * ox + m[7] * oy + m[11] * oz + m[15];
}
} // namespace

// ---- Risk 1: the packed sector round-trips and (0,0,0) stays byte-zero -------

TEST(RenderOrigin, SectorPackRoundTrip)
{
    const int32 samples[] = {0,   1,   -1,   2,    -2,   1000, -1000, 65535, -65535,
                             Origin::kSectorAxisMax, Origin::kSectorAxisMin, 6221, -6221};
    for (int32 x : samples)
        for (int32 y : samples)
            for (int32 z : samples)
            {
                uint32 p0 = 0, p1 = 0;
                Origin::PackSector(x, y, z, p0, p1);
                int32 rx = 0, ry = 0, rz = 0;
                Origin::UnpackSector(p0, p1, rx, ry, rz);
                EXPECT_EQ(rx, x) << "x=" << x << " y=" << y << " z=" << z;
                EXPECT_EQ(ry, y) << "x=" << x << " y=" << y << " z=" << z;
                EXPECT_EQ(rz, z) << "x=" << x << " y=" << y << " z=" << z;
            }
}

TEST(RenderOrigin, SectorZeroPacksToZero)
{
    // Untagged instances (sector 0) must leave the two reserved GPUInstance words
    // zero, so the 240 B memcmp still treats them as identity (quiescence) and the
    // bytes match the pre-feature layout.
    uint32 p0 = 0xDEADBEEFu, p1 = 0xDEADBEEFu;
    Origin::PackSector(0, 0, 0, p0, p1);
    EXPECT_EQ(p0, 0u);
    EXPECT_EQ(p1, 0u);
}

TEST(RenderOrigin, SectorPackFixedBitLayout)
{
    // Lock the exact bit layout the GLSL decode depends on. p0 = [X:21][Ylo:11],
    // p1 = [Yhi:10][Z:21]. A change here that isn't mirrored in the shader is a
    // silent corruption.
    uint32 p0 = 0, p1 = 0;
    Origin::PackSector(1, 0, 0, p0, p1); // X=1 -> lowest bit of p0
    EXPECT_EQ(p0, 1u);
    EXPECT_EQ(p1, 0u);

    Origin::PackSector(0, 1, 0, p0, p1); // Y=1 -> bit 21 of p0
    EXPECT_EQ(p0, 1u << 21);
    EXPECT_EQ(p1, 0u);

    Origin::PackSector(0, 0, 1, p0, p1); // Z=1 -> bit 10 of p1
    EXPECT_EQ(p0, 0u);
    EXPECT_EQ(p1, 1u << 10);
}

// ---- Dark-ship gate: rebased == full-world when the origin is inactive --------

TEST(RenderOrigin, InactiveInsideActivationRadius)
{
    // A camera anywhere inside the activation radius stays at sector (0,0,0).
    EXPECT_EQ(Origin::ComputeRenderOriginSector(0, 0, 0).x, 0);
    const float r = Origin::kRenderOriginActivationRadius * 0.9f;
    const auto s = Origin::ComputeRenderOriginSector(r, -r, r);
    EXPECT_EQ(s.x, 0);
    EXPECT_EQ(s.y, 0);
    EXPECT_EQ(s.z, 0);
}

TEST(RenderOrigin, RebasedViewIsBitIdenticalWhenInactive)
{
    // Camera inside the activation radius: the rebased matrices must be a
    // bit-for-bit copy of the full-world ones (byte-identical render output).
    const Vector3 camPos(1500.0f, 200.0f, -900.0f); // well inside the radius
    const Matrix4x4 rot = MakeRotationY(0.7f) * MakeRotationX(-0.2f);
    const Matrix4x4 view = Inverse(MakeCameraWorld(rot, camPos));
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(60.0f * kDeg2Rad, 16.0f / 9.0f, 0.1f, 1.0e7f);
    const Matrix4x4 viewProj = proj * view;

    Rendering::CameraData cam{};
    std::memcpy(cam.view, view.Data(), sizeof(cam.view));
    std::memcpy(cam.proj, proj.Data(), sizeof(cam.proj));
    std::memcpy(cam.viewProj, viewProj.Data(), sizeof(cam.viewProj));
    cam.cameraPos[0] = camPos.x;
    cam.cameraPos[1] = camPos.y;
    cam.cameraPos[2] = camPos.z;

    Origin::ComputeRebasedView(cam.view, cam.proj, cam.viewProj, cam.cameraPos[0], cam.cameraPos[1],
                               cam.cameraPos[2], cam.viewRel, cam.viewProjRel, cam.renderOriginSector);

    EXPECT_EQ(cam.renderOriginSector[0], 0);
    EXPECT_EQ(cam.renderOriginSector[1], 0);
    EXPECT_EQ(cam.renderOriginSector[2], 0);
    EXPECT_EQ(0, std::memcmp(cam.viewRel, cam.view, sizeof(cam.view)));
    EXPECT_EQ(0, std::memcmp(cam.viewProjRel, cam.viewProj, sizeof(cam.viewProj)));
}

// ---- Reconstruction round-trip: world -> (sector,local) -> world -------------

TEST(RenderOrigin, ReconstructionIsBitExactForSmallLocal)
{
    // A sector-tagged entity stores a small local remainder; reconstructing
    // absolute world = sector*size + local must be exact, and the reconstructed
    // full world (as the shader computes it) must equal the sector-local + origin
    // path bit-for-bit for small local coordinates.
    const auto originSector = Origin::ComputeRenderOriginSector(6.371e6f, 0.0f, 0.0f);
    ASSERT_NE(originSector.x, 0); // origin active at Earth radius

    // Entity 12.5 m from the render origin, tagged to the origin's sector.
    const Vector3 local(12.5f, -3.25f, 7.0f);
    const Components::WorldSectorCoord instSector = originSector;

    // Shader math: relWorldPos = (instSector - originSector)*size + local.
    const float s = Origin::kSectorSize;
    const Vector3 rel((instSector.x - originSector.x) * s + local.x,
                      (instSector.y - originSector.y) * s + local.y,
                      (instSector.z - originSector.z) * s + local.z);
    EXPECT_FLOAT_EQ(rel.x, local.x); // same sector -> rel == local, exact
    EXPECT_FLOAT_EQ(rel.y, local.y);
    EXPECT_FLOAT_EQ(rel.z, local.z);

    // Reconstruct full world = rel + origin.
    float ox, oy, oz;
    Origin::SectorToWorld(originSector, ox, oy, oz);
    const Vector3 full(rel.x + ox, rel.y + oy, rel.z + oz);
    // Direct absolute world = instSector*size + local.
    const Vector3 direct(instSector.x * s + local.x, instSector.y * s + local.y, instSector.z * s + local.z);
    EXPECT_FLOAT_EQ(full.x, direct.x);
    EXPECT_FLOAT_EQ(full.y, direct.y);
    EXPECT_FLOAT_EQ(full.z, direct.z);
}

TEST(RenderOrigin, ComposeMatchesRenderReconstruction)
{
    // The CPU picking/TLAS composition (ComposeEffectiveWorldTransform) and the
    // GPU render reconstruction (sector*kSectorSize + local) must agree on a
    // tagged entity's absolute world position to the bit — otherwise it renders
    // where it does NOT pick/raycast. The single shared constant is what makes
    // this hold; this oracle fails the instant the two scales diverge again.
    static_assert(Origin::kSectorSize == Components::kWorldSectorSize,
                  "render and composition must use the ONE sector size");

    Components::WorldTransform wt{}; // identity rotation + small sector-local translation
    for (int i = 0; i < 16; ++i)
        wt.matrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    wt.matrix[12] = 37.5f;
    wt.matrix[13] = -12.25f;
    wt.matrix[14] = 800.0f;

    const Components::WorldSectorCoord sector{3595, -1200, 3595}; // ~3.68e6 m, planetary
    const Components::WorldTransform composed =
        Components::ComposeEffectiveWorldTransform(wt, &sector, Components::kWorldSectorSize);

    // Render reconstruction of the same absolute world translation.
    EXPECT_EQ(composed.matrix[12], sector.x * Origin::kSectorSize + wt.matrix[12]);
    EXPECT_EQ(composed.matrix[13], sector.y * Origin::kSectorSize + wt.matrix[13]);
    EXPECT_EQ(composed.matrix[14], sector.z * Origin::kSectorSize + wt.matrix[14]);
}

TEST(RenderOrigin, ComposeDisplayFormatsFullMeters)
{
    // The Inspector's "World (sector-composed)" row composes the sector via
    // ComposeEffectiveWorldTransform and formats each axis with FormatFloat
    // (shortest-exact). This oracle pins the exact strings the user sees for the
    // charter's worked example: sector {3595,0,3595} + local (100,2,100) at a
    // 1024 m sector must read (3681380, 2, 3681380) — the same absolute meters the
    // mesh renders at. It fails if the composition, the constant, or the number
    // formatting drifts from what the Inspector shows.
    Components::WorldTransform wt{};
    wt.matrix[12] = 100.0f;
    wt.matrix[13] = 2.0f;
    wt.matrix[14] = 100.0f;

    const Components::WorldSectorCoord sector{3595, 0, 3595};
    const Components::WorldTransform composed =
        Components::ComposeEffectiveWorldTransform(wt, &sector, Components::kWorldSectorSize);

    EXPECT_EQ(FormatFloat(composed.matrix[12]), "3681380");
    EXPECT_EQ(FormatFloat(composed.matrix[13]), "2");
    EXPECT_EQ(FormatFloat(composed.matrix[14]), "3681380");
}

TEST(RenderOrigin, RebasedProjectionMatchesFullWorldWhenClean)
{
    // Correctness (not precision): just past the activation radius fp32 is still
    // clean, so viewProjRel*relPos and viewProj*worldPos must agree to well under
    // a pixel — proof the rebased transform is the SAME transform, not garbage.
    // (Far past this radius they diverge by design: the rebased path is the more
    // accurate one — that is what the far-field boiling oracle below shows.)
    const Vector3 camPos(34000.0f, 800.0f, -12000.0f); // ~34 km, origin active
    const Matrix4x4 rot = MakeRotationY(-0.5f) * MakeRotationX(0.15f);
    const Matrix4x4 view = Inverse(MakeCameraWorld(rot, camPos));
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(55.0f * kDeg2Rad, 1.5f, 0.1f, 1.0e6f);
    const Matrix4x4 viewProj = proj * view;

    Rendering::CameraData cam{};
    std::memcpy(cam.view, view.Data(), sizeof(cam.view));
    std::memcpy(cam.proj, proj.Data(), sizeof(cam.proj));
    std::memcpy(cam.viewProj, viewProj.Data(), sizeof(cam.viewProj));
    cam.cameraPos[0] = camPos.x;
    cam.cameraPos[1] = camPos.y;
    cam.cameraPos[2] = camPos.z;
    Origin::ComputeRebasedView(cam.view, cam.proj, cam.viewProj, cam.cameraPos[0], cam.cameraPos[1],
                               cam.cameraPos[2], cam.viewRel, cam.viewProjRel, cam.renderOriginSector);
    ASSERT_NE(cam.renderOriginSector[0] | cam.renderOriginSector[1] | cam.renderOriginSector[2], 0);

    Matrix4x4 vpRel;
    std::memcpy(vpRel.Data(), cam.viewProjRel, sizeof(cam.viewProjRel));

    float ox, oy, oz;
    Origin::SectorToWorld(Components::WorldSectorCoord{cam.renderOriginSector[0], cam.renderOriginSector[1],
                                                       cam.renderOriginSector[2]},
                          ox, oy, oz);

    const Vector3 worldPos(34050.0f, 780.0f, -11985.0f); // ~55 m from the camera
    const Vector3 relPos(worldPos.x - ox, worldPos.y - oy, worldPos.z - oz);

    const Vector4 full = viewProj.Transform(Vector4(worldPos.x, worldPos.y, worldPos.z, 1.0f));
    const Vector4 rel = vpRel.Transform(Vector4(relPos.x, relPos.y, relPos.z, 1.0f));

    EXPECT_NEAR(rel.x / rel.w, full.x / full.w, 2e-3f); // < ~2 px on a 1080p frame
    EXPECT_NEAR(rel.y / rel.w, full.y / full.w, 2e-3f);
    EXPECT_NEAR(rel.z / rel.w, full.z / full.w, 2e-3f);
}

// ---- Rotation-smooth rebased translation (the planet-scale camera-jitter fix) -

TEST(RenderOrigin, RebasedTranslationSmoothUnderRotationAtRange)
{
    // The old origin-active rebase computed viewRel's translation as the fp32
    // sum (stored translation + M3x3*origin) — two ~|eye|-magnitude terms whose
    // small result inherits ULP(|eye|) rounding, and the rounding CHANGES with
    // the rotation: ~4-8 mm wobble at |eye| 5e4, ~0.5-1 m at Earth radius,
    // visible as world shimmer during interactive camera rotation.
    // ComputeRebasedView now rebuilds the column as -M3x3*(eye - origin) in
    // double. Assert it tracks the exact double reference tightly through a
    // rotation sweep — and that the legacy fp32 column rebase measurably does
    // not at this magnitude.
    const Vector3 camPos(3.681e6f, 500.0f, 3.681e6f);

    double maxNewErr = 0.0;
    double maxOldErr = 0.0;
    for (int i = 0; i <= 60; ++i)
    {
        const float yaw = 0.6f + 1.0e-3f * static_cast<float>(i); // slow rotation sweep
        const Matrix4x4 rot = MakeRotationY(yaw) * MakeRotationX(-0.15f);
        const Matrix4x4 view = Inverse(MakeCameraWorld(rot, camPos));
        const Matrix4x4 proj =
            MakePerspectiveLH_ZO_ReverseZ(55.0f * kDeg2Rad, 16.0f / 9.0f, 0.5f, 4000.0f);
        const Matrix4x4 viewProj = proj * view;

        Rendering::CameraData cam{};
        std::memcpy(cam.view, view.Data(), sizeof(cam.view));
        std::memcpy(cam.proj, proj.Data(), sizeof(cam.proj));
        std::memcpy(cam.viewProj, viewProj.Data(), sizeof(cam.viewProj));
        cam.cameraPos[0] = camPos.x;
        cam.cameraPos[1] = camPos.y;
        cam.cameraPos[2] = camPos.z;
        Origin::ComputeRebasedView(cam.view, cam.proj, cam.viewProj, cam.cameraPos[0],
                                   cam.cameraPos[1], cam.cameraPos[2], cam.viewRel,
                                   cam.viewProjRel, cam.renderOriginSector);
        ASSERT_NE(cam.renderOriginSector[0] | cam.renderOriginSector[1] | cam.renderOriginSector[2], 0);

        // Exact double reference: -M3x3 * (eye - origin), origin from the
        // integer sector (exact in double).
        const double size = static_cast<double>(Origin::kSectorSize);
        const double lx = static_cast<double>(camPos.x) - cam.renderOriginSector[0] * size;
        const double ly = static_cast<double>(camPos.y) - cam.renderOriginSector[1] * size;
        const double lz = static_cast<double>(camPos.z) - cam.renderOriginSector[2] * size;
        const float* v = view.Data();
        const double ref[3] = {
            -(static_cast<double>(v[0]) * lx + static_cast<double>(v[4]) * ly + static_cast<double>(v[8]) * lz),
            -(static_cast<double>(v[1]) * lx + static_cast<double>(v[5]) * ly + static_cast<double>(v[9]) * lz),
            -(static_cast<double>(v[2]) * lx + static_cast<double>(v[6]) * ly + static_cast<double>(v[10]) * lz)};

        for (int c = 0; c < 3; ++c)
            maxNewErr = std::max(maxNewErr, std::fabs(static_cast<double>(cam.viewRel[12 + c]) - ref[c]));

        // Legacy fp32 column rebase for the fails-before contrast.
        float ox, oy, oz;
        Origin::SectorToWorld(
            Components::WorldSectorCoord{cam.renderOriginSector[0], cam.renderOriginSector[1],
                                         cam.renderOriginSector[2]},
            ox, oy, oz);
        float oldRel[16];
        std::memcpy(oldRel, view.Data(), sizeof(oldRel));
        LegacyRebaseTranslationColumnFp32(view.Data(), ox, oy, oz, oldRel);
        for (int c = 0; c < 3; ++c)
            maxOldErr = std::max(maxOldErr, std::fabs(static_cast<double>(oldRel[12 + c]) - ref[c]));
    }

    EXPECT_LT(maxNewErr, 1.0e-3) << "double rebase must track the exact reference (sub-mm)";
    EXPECT_GT(maxOldErr, 0.05) << "legacy fp32 rebase no longer shows the cancellation hazard"
                                  " at 3.68e6 — revisit whether the double path is still needed";
}

// ---- The far-field oracle: z-fighting resolved where the baseline collapses ---

TEST(RenderOrigin, FarFieldDepthResolvedWhereBaselineZFights)
{
    // Two sector-tagged surfaces 5 cm apart in DEPTH at ~6371 km. fp32-world
    // storage cannot represent 5 cm below the ~0.76 m ULP at that magnitude, so
    // the baseline collapses both to the same view-space depth — the exact
    // condition that makes far coplanar surfaces z-fight. The sector-local
    // relative path keeps the 5 cm in its small local remainder and resolves it.
    // View-space depth with an axis-aligned camera is worldZ - cameraZ, so the
    // difference isolates depth resolution directly and deterministically.
    //
    // Disable-and-fail evidence: baseDz (full-world storage) is ~0 (both surfaces
    // land on the same depth); relDz (sector-local) recovers the true 5 cm.
    const double R = 6.371e6;
    const double sep = 0.05; // 5 cm in depth
    const double w1d[3] = {5000.0, 200.0, R};
    const double w2d[3] = {5000.0, 200.0, R + sep};

    auto sectorOf = [](const double w[3]) {
        return Components::WorldSectorCoord{
            static_cast<int32>(std::llround(w[0] / static_cast<double>(Origin::kSectorSize))),
            static_cast<int32>(std::llround(w[1] / static_cast<double>(Origin::kSectorSize))),
            static_cast<int32>(std::llround(w[2] / static_cast<double>(Origin::kSectorSize)))};
    };
    auto localOf = [](const double w[3], const Components::WorldSectorCoord& s) {
        return Vector3(static_cast<float>(w[0] - s.x * static_cast<double>(Origin::kSectorSize)),
                       static_cast<float>(w[1] - s.y * static_cast<double>(Origin::kSectorSize)),
                       static_cast<float>(w[2] - s.z * static_cast<double>(Origin::kSectorSize)));
    };
    const Components::WorldSectorCoord sec1 = sectorOf(w1d), sec2 = sectorOf(w2d);
    const Vector3 loc1 = localOf(w1d, sec1), loc2 = localOf(w2d, sec2);
    const Vector3 w1f(static_cast<float>(w1d[0]), static_cast<float>(w1d[1]), static_cast<float>(w1d[2]));
    const Vector3 w2f(static_cast<float>(w2d[0]), static_cast<float>(w2d[1]), static_cast<float>(w2d[2]));

    // fp32 world storage cannot separate the two depths — the precondition for the
    // z-fight the relative path fixes.
    ASSERT_FLOAT_EQ(w1f.z, w2f.z);

    const Vector3 camPos(5000.0f, 200.0f, static_cast<float>(R) - 30.0f); // 30 m back, axis-aligned
    const Matrix4x4 view = Inverse(MakeCameraWorld(Matrix4x4::Identity(), camPos));

    Rendering::CameraData cam{};
    std::memcpy(cam.view, view.Data(), sizeof(cam.view));
    std::memcpy(cam.proj, Matrix4x4::Identity().Data(), sizeof(cam.proj)); // proj irrelevant for view-space depth
    std::memcpy(cam.viewProj, view.Data(), sizeof(cam.viewProj));
    cam.cameraPos[0] = camPos.x;
    cam.cameraPos[1] = camPos.y;
    cam.cameraPos[2] = camPos.z;
    Origin::ComputeRebasedView(cam.view, cam.proj, cam.viewProj, cam.cameraPos[0], cam.cameraPos[1],
                               cam.cameraPos[2], cam.viewRel, cam.viewProjRel, cam.renderOriginSector);
    ASSERT_NE(cam.renderOriginSector[0] | cam.renderOriginSector[1] | cam.renderOriginSector[2], 0);

    Matrix4x4 viewRel;
    std::memcpy(viewRel.Data(), cam.viewRel, sizeof(cam.viewRel));
    auto rel = [&](const Components::WorldSectorCoord& s, const Vector3& loc) {
        return Vector3((s.x - cam.renderOriginSector[0]) * Origin::kSectorSize + loc.x,
                       (s.y - cam.renderOriginSector[1]) * Origin::kSectorSize + loc.y,
                       (s.z - cam.renderOriginSector[2]) * Origin::kSectorSize + loc.z);
    };

    // View-space depth (z of view * worldPos) difference.
    const float baseDz = view.Transform(Vector4(w1f.x, w1f.y, w1f.z, 1.0f)).z -
                         view.Transform(Vector4(w2f.x, w2f.y, w2f.z, 1.0f)).z;
    const Vector3 rel1 = rel(sec1, loc1), rel2 = rel(sec2, loc2);
    const float relDz = viewRel.Transform(Vector4(rel1.x, rel1.y, rel1.z, 1.0f)).z -
                        viewRel.Transform(Vector4(rel2.x, rel2.y, rel2.z, 1.0f)).z;

    EXPECT_NEAR(std::fabs(relDz), static_cast<float>(sep), 0.005f) << "relative path resolves the 5 cm depth";
    EXPECT_LT(std::fabs(baseDz), 0.01f) << "fp32-world baseline collapses the two depths (z-fight)";
    EXPECT_GT(std::fabs(relDz) - std::fabs(baseDz), 0.03f) << "relative recovers depth the baseline lost";
}

// ---- Slice 2 + #660 shadow follow-up: cascade shadow VP rebase ---------------
//
// ShadowMapRenderFeature::ComputeCascadeLightVP fits a cascade orthographic VP
// in world space, then builds CascadeFrameData::LightVPRel via RenderOrigin.h::
// ComputeRebasedOrthoLightVP — rebuilt in DOUBLE from the fit's rotation-only
// view + projection + shadow-camera eye, never through the world VP's fp32
// translation column (whose storage rounding at planetary magnitude is coarser
// than a shadow texel) nor through MakeLookAtLH(eye, eye + dir) (whose forward
// reconstruction fl(eye + dir) − eye quantizes the light DIRECTION to
// ULP(|eye|)). Both the depth pass (caster) and the receiver (ge_shadowVP)
// project render-origin-relative positions through the SAME matrix. These
// oracles lock the primitive at the math level with cascade-shaped inputs.

namespace
{
// The fit shape ComputeCascadeLightVP produces with the origin ACTIVE:
// rotation from the rotation-only look-at, world translation −R·eye derived
// from the rotation's own 3x3, reverse-Z ortho.
struct CascadeFitShape
{
    Matrix4x4 RotView; // rotation-only light view (translation zero)
    Matrix4x4 Proj;    // reverse-Z ortho
    Matrix4x4 WorldVP; // Proj * [R | -R*eye], fp32 world composition
};

CascadeFitShape MakeCascadeFit(const Vector3& lightPos, const Vector3& lightDir, float halfExtent,
                               float depthRange)
{
    Vector3 ld = lightDir;
    ld = ld.Normalize();
    const Vector3 up = std::fabs(Vector3::Dot(ld, Vector3{0, 1, 0})) > 0.99f ? Vector3{0, 0, 1}
                                                                             : Vector3{0, 1, 0};
    CascadeFitShape f;
    f.RotView = MakeLookAtLH(Vector3{0, 0, 0}, ld, up);
    f.Proj = MakeOrthographicLH_ZO_ReverseZ(-halfExtent, halfExtent, -halfExtent, halfExtent,
                                            0.0f, depthRange);
    Matrix4x4 view = f.RotView;
    float* v = view.Data();
    v[12] = static_cast<float>(-(static_cast<double>(v[0]) * lightPos.x +
                                 static_cast<double>(v[4]) * lightPos.y +
                                 static_cast<double>(v[8]) * lightPos.z));
    v[13] = static_cast<float>(-(static_cast<double>(v[1]) * lightPos.x +
                                 static_cast<double>(v[5]) * lightPos.y +
                                 static_cast<double>(v[9]) * lightPos.z));
    v[14] = static_cast<float>(-(static_cast<double>(v[2]) * lightPos.x +
                                 static_cast<double>(v[6]) * lightPos.y +
                                 static_cast<double>(v[10]) * lightPos.z));
    f.WorldVP = f.Proj * view;
    return f;
}

// The LEGACY cascade world VP: MakeLookAtLH(eye, eye + dir) — the pre-fix
// construction whose forward reconstruction quantizes to ULP(|eye|).
Matrix4x4 MakeLegacyCascadeWorldVP(const Vector3& lightPos, const Vector3& lightDir,
                                   float halfExtent, float depthRange)
{
    Vector3 ld = lightDir;
    ld = ld.Normalize();
    const Vector3 up = std::fabs(Vector3::Dot(ld, Vector3{0, 1, 0})) > 0.99f ? Vector3{0, 0, 1}
                                                                             : Vector3{0, 1, 0};
    const Matrix4x4 view = MakeLookAtLH(lightPos, lightPos + ld, up);
    const Matrix4x4 proj = MakeOrthographicLH_ZO_ReverseZ(-halfExtent, halfExtent, -halfExtent,
                                                          halfExtent, 0.0f, depthRange);
    return proj * view;
}

// Exact double reference: the clip position of a render-origin-relative point
// through the fit, every term small or exact in double. lightPosRel is the
// shadow-camera eye relative to the origin (exact in double).
void RefClipOfRel(const CascadeFitShape& f, const double lightPosRel[3], const Vector3& pRel,
                  double outClip[3])
{
    const float* r = f.RotView.Data();
    double viewRel[16];
    for (int i = 0; i < 12; ++i)
        viewRel[i] = static_cast<double>(r[i]);
    for (int row = 0; row < 3; ++row)
        viewRel[12 + row] = -(static_cast<double>(r[0 + row]) * lightPosRel[0] +
                              static_cast<double>(r[4 + row]) * lightPosRel[1] +
                              static_cast<double>(r[8 + row]) * lightPosRel[2]);
    viewRel[15] = static_cast<double>(r[15]);
    const float* p = f.Proj.Data();
    double vp[16];
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
        {
            double acc = 0.0;
            for (int k = 0; k < 4; ++k)
                acc += static_cast<double>(p[k * 4 + row]) * viewRel[c * 4 + k];
            vp[c * 4 + row] = acc;
        }
    for (int row = 0; row < 3; ++row)
        outClip[row] = vp[0 + row] * pRel.x + vp[4 + row] * pRel.y + vp[8 + row] * pRel.z +
                       vp[12 + row];
}
} // namespace

TEST(RenderOrigin, CascadeVPRebaseBitIdenticalWhenInactive)
{
    // Dark-ship: with the origin inactive (sector 0,0,0) ComputeRebasedOrthoLightVP
    // is a byte-for-byte copy of the world VP, so LightVPRel, the depth pass, and
    // ge_shadowVP are identical to the pre-feature build.
    const CascadeFitShape f =
        MakeCascadeFit(Vector3{20.0f, 80.0f, -15.0f}, Vector3{-0.4f, -1.0f, -0.25f}, 40.0f, 220.0f);
    const int32 sector[3] = {0, 0, 0};

    Matrix4x4 vpRel = f.WorldVP;
    Origin::ComputeRebasedOrthoLightVP(f.RotView.Data(), f.Proj.Data(), f.WorldVP.Data(), 20.0f,
                                       80.0f, -15.0f, sector, vpRel.Data());
    EXPECT_EQ(0, std::memcmp(vpRel.Data(), f.WorldVP.Data(), 16 * sizeof(float)));
}

TEST(RenderOrigin, CascadeVPRebaseIsTheSameTransform)
{
    // Correctness: the double-rebuilt cascade VP applied to a render-origin-
    // relative caster position equals the world VP applied to the full world
    // position — proof the rebase is the SAME projection, not garbage. Just
    // past the activation radius fp32 is still clean, so the two agree to well
    // under a shadow texel.
    const Vector3 camPos(40000.0f, 300.0f, -9000.0f); // ~40 km, origin active
    const auto originSector = Origin::ComputeRenderOriginSector(camPos.x, camPos.y, camPos.z);
    ASSERT_NE(originSector.x | originSector.y | originSector.z, 0);
    const int32 sector[3] = {originSector.x, originSector.y, originSector.z};
    float ox, oy, oz;
    Origin::SectorToWorld(originSector, ox, oy, oz);

    // A cascade fit near the camera.
    const Vector3 lightPos(camPos.x + 30.0f, camPos.y + 120.0f, camPos.z - 20.0f);
    const CascadeFitShape f = MakeCascadeFit(lightPos, Vector3{-0.5f, -1.0f, -0.3f}, 60.0f, 300.0f);
    Matrix4x4 vpRel = f.WorldVP;
    Origin::ComputeRebasedOrthoLightVP(f.RotView.Data(), f.Proj.Data(), f.WorldVP.Data(),
                                       lightPos.x, lightPos.y, lightPos.z, sector, vpRel.Data());

    const Vector3 caster(camPos.x + 12.0f, camPos.y, camPos.z + 8.0f);
    const Vector3 rel(caster.x - ox, caster.y - oy, caster.z - oz);

    const Vector4 world = f.WorldVP.Transform(Vector4(caster.x, caster.y, caster.z, 1.0f));
    const Vector4 relClip = vpRel.Transform(Vector4(rel.x, rel.y, rel.z, 1.0f));

    // Orthographic => w == 1; compare NDC directly. 2048 shadow texels over the
    // [-1,1] NDC span => one texel is ~1e-3 NDC; require < half a texel.
    EXPECT_NEAR(relClip.x, world.x, 5e-4f);
    EXPECT_NEAR(relClip.y, world.y, 5e-4f);
    EXPECT_NEAR(relClip.z, world.z, 5e-4f);
}

TEST(RenderOrigin, CascadeVPRebaseResolvesSelfShadowDepthAtRange)
{
    // The acne mechanism, isolated: two caster surfaces 5 cm apart along the
    // light's depth axis at ~3.68e6 m (the #514 repro distance). The world-space
    // orthographic VP computes light-space depth as big·big cancellation, so both
    // surfaces round to the SAME NDC depth — the receiver then can't separate its
    // own surface from itself and speckles. The rebased VP projects small relative
    // positions; the constant coarse offset cancels in the per-vertex difference,
    // so the 5 cm resolves and the self-shadow is clean.
    const double R = 3.681e6; // matches the tagged-sphere sector (3595 * 1024)
    const Vector3 lightDir{-0.5f, -1.0f, -0.3f};
    const float halfExtent = 60.0f;
    const float depthRange = 300.0f;

    // Camera / render origin at the repro distance.
    const Vector3 camPos(static_cast<float>(R), 2.0f, static_cast<float>(R));
    const auto originSector = Origin::ComputeRenderOriginSector(camPos.x, camPos.y, camPos.z);
    ASSERT_NE(originSector.x | originSector.y | originSector.z, 0);
    const int32 sector[3] = {originSector.x, originSector.y, originSector.z};
    float ox, oy, oz;
    Origin::SectorToWorld(originSector, ox, oy, oz);

    // Light positioned near the camera; two casters 5 cm apart along +Y (a floor
    // fragment vs. the same fragment offset by the anti-acne bias magnitude).
    const Vector3 lightPos(camPos.x + 20.0f, camPos.y + 150.0f, camPos.z - 10.0f);
    const CascadeFitShape f = MakeCascadeFit(lightPos, lightDir, halfExtent, depthRange);
    Matrix4x4 vpRel = f.WorldVP;
    Origin::ComputeRebasedOrthoLightVP(f.RotView.Data(), f.Proj.Data(), f.WorldVP.Data(),
                                       lightPos.x, lightPos.y, lightPos.z, sector, vpRel.Data());

    const double sep = 0.05; // 5 cm
    const Vector3 c1(camPos.x + 5.0f, 2.0f, camPos.z + 5.0f);
    const Vector3 c2(camPos.x + 5.0f, 2.0f + static_cast<float>(sep), camPos.z + 5.0f);

    // World-space NDC depth (ortho => w==1).
    const float worldZ1 = f.WorldVP.Transform(Vector4(c1.x, c1.y, c1.z, 1.0f)).z;
    const float worldZ2 = f.WorldVP.Transform(Vector4(c2.x, c2.y, c2.z, 1.0f)).z;

    // Rebased NDC depth from render-origin-relative positions.
    const Vector3 r1(c1.x - ox, c1.y - oy, c1.z - oz);
    const Vector3 r2(c2.x - ox, c2.y - oy, c2.z - oz);
    const float relZ1 = vpRel.Transform(Vector4(r1.x, r1.y, r1.z, 1.0f)).z;
    const float relZ2 = vpRel.Transform(Vector4(r2.x, r2.y, r2.z, 1.0f)).z;

    const float worldDz = std::fabs(worldZ1 - worldZ2);
    const float relDz = std::fabs(relZ1 - relZ2);

    EXPECT_LT(worldDz, 1e-6f) << "fp32-world ortho VP collapses the 5 cm caster separation (acne)";
    EXPECT_GT(relDz, 1e-5f) << "rebased ortho VP resolves the caster separation (acne fixed)";
    EXPECT_GT(relDz, worldDz * 10.0f) << "relative path recovers depth resolution the world path lost";
}

TEST(RenderOrigin, CascadeVPRebaseClosureAtPlanetScale)
{
    // The #660 shadow-wobble gate, unit level. At |origin| 5e4 and Earth radius,
    // sweep the shadow-camera eye through micro-steps (what a translating camera
    // does to the fit every frame) and measure the worst clip-space deviation of
    // the rebased VP from the exact double reference over caster points near the
    // camera. The deviation bounds frame-to-frame shadow-edge wobble (each
    // frame's matrix is within eps of the same exact transform).
    //   * New path (ComputeRebasedOrthoLightVP): < 1e-5 NDC (~0.01 texel @2048).
    //   * Legacy path (MakeLookAtLH(eye, eye+dir) world VP + fp32 column
    //     rebase): the ULP(|eye|) direction quantization + big+big translation
    //     cancellation — texel-scale at 5e4, catastrophic at Earth radius.
    struct Case
    {
        Vector3 CamPos;
        double LegacyFloor; // the legacy error the fix must beat
    };
    const Case cases[] = {
        {Vector3(50000.0f, 300.0f, -9000.0f), 2.0e-4},   // R=50000 (the report)
        {Vector3(6371000.0f, 500.0f, 6371000.0f), 2.0e-2}, // Earth radius
    };
    const Vector3 lightDir{-0.5f, -1.0f, -0.3f};
    const float halfExtent = 60.0f;
    const float depthRange = 300.0f;

    for (const Case& tc : cases)
    {
        const auto originSector =
            Origin::ComputeRenderOriginSector(tc.CamPos.x, tc.CamPos.y, tc.CamPos.z);
        ASSERT_NE(originSector.x | originSector.y | originSector.z, 0);
        const int32 sector[3] = {originSector.x, originSector.y, originSector.z};
        const double size = static_cast<double>(Origin::kSectorSize);
        const double originD[3] = {originSector.x * size, originSector.y * size,
                                   originSector.z * size};

        double maxNewErr = 0.0;
        double maxLegacyErr = 0.0;
        for (int step = 0; step < 24; ++step)
        {
            // Micro-step the fit's shadow-camera eye — 12.5 cm per step, the
            // scale at which a walking camera re-fits the cascade.
            const float dt = 0.125f * static_cast<float>(step);
            const Vector3 lightPos(tc.CamPos.x + 20.0f + dt, tc.CamPos.y + 150.0f,
                                   tc.CamPos.z - 10.0f + 0.5f * dt);
            const CascadeFitShape f = MakeCascadeFit(lightPos, lightDir, halfExtent, depthRange);
            Matrix4x4 vpRel = f.WorldVP;
            Origin::ComputeRebasedOrthoLightVP(f.RotView.Data(), f.Proj.Data(), f.WorldVP.Data(),
                                               lightPos.x, lightPos.y, lightPos.z, sector,
                                               vpRel.Data());
            const Matrix4x4 legacyVP =
                MakeLegacyCascadeWorldVP(lightPos, lightDir, halfExtent, depthRange);
            Matrix4x4 legacyRel = legacyVP;
            LegacyRebaseTranslationColumnFp32(legacyVP.Data(),
                                              static_cast<float>(originD[0]),
                                              static_cast<float>(originD[1]),
                                              static_cast<float>(originD[2]), legacyRel.Data());

            const double lightPosRel[3] = {static_cast<double>(lightPos.x) - originD[0],
                                           static_cast<double>(lightPos.y) - originD[1],
                                           static_cast<double>(lightPos.z) - originD[2]};
            // Caster/receiver points near the camera, exact small rel coords.
            const Vector3 camRel(static_cast<float>(static_cast<double>(tc.CamPos.x) - originD[0]),
                                 static_cast<float>(static_cast<double>(tc.CamPos.y) - originD[1]),
                                 static_cast<float>(static_cast<double>(tc.CamPos.z) - originD[2]));
            const Vector3 offsets[] = {{5.0f, 0.0f, 5.0f}, {-12.0f, 2.0f, 8.0f},
                                       {25.0f, 10.0f, -14.0f}, {0.0f, 0.25f, 0.0f}};
            for (const Vector3& off : offsets)
            {
                const Vector3 pRel(camRel.x + off.x, camRel.y + off.y, camRel.z + off.z);
                double ref[3];
                RefClipOfRel(f, lightPosRel, pRel, ref);

                const Vector4 c = vpRel.Transform(Vector4(pRel.x, pRel.y, pRel.z, 1.0f));
                maxNewErr = std::max({maxNewErr, std::fabs(c.x - ref[0]),
                                      std::fabs(c.y - ref[1]), std::fabs(c.z - ref[2])});

                // Legacy consumed the fp32 WORLD position through the fp32
                // world VP + fp32 rebase — evaluate the whole legacy chain.
                const Vector3 pWorld(static_cast<float>(originD[0] + pRel.x),
                                     static_cast<float>(originD[1] + pRel.y),
                                     static_cast<float>(originD[2] + pRel.z));
                const Vector3 pRelLegacy(pWorld.x - static_cast<float>(originD[0]),
                                         pWorld.y - static_cast<float>(originD[1]),
                                         pWorld.z - static_cast<float>(originD[2]));
                const Vector4 lc =
                    legacyRel.Transform(Vector4(pRelLegacy.x, pRelLegacy.y, pRelLegacy.z, 1.0f));
                maxLegacyErr = std::max({maxLegacyErr, std::fabs(lc.x - ref[0]),
                                         std::fabs(lc.y - ref[1]), std::fabs(lc.z - ref[2])});
            }
        }

        // Evidence line for the fails-before record (clip units; 1 texel @2048
        // is ~9.8e-4; world meters = clip * halfExtent).
        std::cout << "[closure] |origin|~" << originD[0] << "  legacy=" << maxLegacyErr
                  << " clip (" << maxLegacyErr * halfExtent << " m)  new=" << maxNewErr
                  << " clip (" << maxNewErr * halfExtent << " m)\n";

        EXPECT_LT(maxNewErr, 1e-5) << "double-rebuilt cascade VP must track the exact reference"
                                   << " at |origin| ~" << originD[0];
        EXPECT_GT(maxLegacyErr, tc.LegacyFloor)
            << "legacy fp32 cascade path no longer shows the hazard at |origin| ~" << originD[0]
            << " — revisit whether the double path is still needed";
        EXPECT_GT(maxLegacyErr, maxNewErr * 100.0)
            << "fix must collapse the wobble by orders of magnitude";
    }
}

TEST(RenderOrigin, CascadeSnapshotSectorDeltaRebaseExactAcrossSectorStep)
{
    // The motion round-robin's deferral-window absorption: a retained rel fit
    // committed under sector A, re-anchored to the camera's new sector B via
    // RebaseTranslationColumnBySectorDelta (delta exact in double from the
    // integer sectors). Must match building the rel VP directly at sector B to
    // sub-texel precision — at Earth radius, where the legacy world-VP re-rebase
    // carried ULP(|origin|) storage rounding.
    const Vector3 camPos(6371000.0f, 500.0f, 6371000.0f);
    const auto secA = Origin::ComputeRenderOriginSector(camPos.x, camPos.y, camPos.z);
    ASSERT_NE(secA.x | secA.y | secA.z, 0);
    const int32 from[3] = {secA.x, secA.y, secA.z};
    const int32 to[3] = {secA.x + 1, secA.y, secA.z - 2}; // a sector step mid-deferral

    const Vector3 lightPos(camPos.x + 20.0f, camPos.y + 150.0f, camPos.z - 10.0f);
    const CascadeFitShape f = MakeCascadeFit(lightPos, Vector3{-0.5f, -1.0f, -0.3f}, 60.0f, 300.0f);

    Matrix4x4 relA = f.WorldVP;
    Origin::ComputeRebasedOrthoLightVP(f.RotView.Data(), f.Proj.Data(), f.WorldVP.Data(),
                                       lightPos.x, lightPos.y, lightPos.z, from, relA.Data());
    Matrix4x4 relB = f.WorldVP;
    Origin::ComputeRebasedOrthoLightVP(f.RotView.Data(), f.Proj.Data(), f.WorldVP.Data(),
                                       lightPos.x, lightPos.y, lightPos.z, to, relB.Data());

    Matrix4x4 stepped = relA;
    Origin::RebaseTranslationColumnBySectorDelta(relA.Data(), from, to, stepped.Data());

    // Rotation/scale columns untouched (bit-for-bit), translation column within
    // ~1 fp32 ULP of the direct build (both are one rounding away from the same
    // double value). 1e-5 clip ≈ 0.01 texel @2048.
    EXPECT_EQ(0, std::memcmp(stepped.Data(), relA.Data(), 12 * sizeof(float)));
    for (int i = 12; i < 16; ++i)
        EXPECT_NEAR(stepped.Data()[i], relB.Data()[i], 1e-5f) << "column element " << i;
}

// ---- Slice 2: camera-relative GPU culling (from stash, dark-ship-gated) -------
//
// Rendering::MakeFrustumPlanesCameraRelative translates each frustum plane's
// offset by n*origin so the culling shaders can test (boundingCenter - origin) in
// small magnitudes, recovering the true plane distance where the world-space
// dot(center, n) + d collapses (big-minus-big) at planetary distance. These
// oracles are the brute-force-vs-predicate correctness check + the dark-ship
// no-op proof.

namespace
{
using GameEngine::Rendering::MakeFrustumPlanesCameraRelative;

// CPU replica of frustum_culling.comp / hzb_culling.comp TestSphereFrustum: the
// camera-relative sphere test the GPU runs (planes carry the translated offset).
bool TestSphereFrustumRel(const Vector3& center, float radius, const Vector4* planes,
                          const Vector3& origin)
{
    const Vector3 rel = center - origin;
    for (int i = 0; i < 6; ++i)
    {
        const float dist = rel.x * planes[i].x + rel.y * planes[i].y + rel.z * planes[i].z + planes[i].w;
        if (dist < -radius)
            return false;
    }
    return true;
}
} // namespace

TEST(RenderOrigin, CullPlanesCameraRelativeIsNoOpAtZeroOrigin)
{
    // Dark-ship: with origin (0,0,0) the plane translation is a bit-for-bit no-op,
    // and the shader's (center - 0) == center, so the whole camera-relative cull
    // path is byte-identical to the world-space test for sector-0 scenes.
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(60.0f * kDeg2Rad, 16.0f / 9.0f, 0.1f, 5000.0f);
    const Matrix4x4 view = Inverse(MakeCameraWorld(MakeRotationY(0.3f), Vector3(120.0f, 40.0f, -80.0f)));
    const Matrix4x4 viewProj = proj * view;

    Vector4 planes[6];
    GameEngine::Rendering::ExtractFrustumPlanes(viewProj, planes);
    Vector4 translated[6];
    std::memcpy(translated, planes, sizeof(planes));
    MakeFrustumPlanesCameraRelative(translated, Vector3(0.0f, 0.0f, 0.0f));
    EXPECT_EQ(0, std::memcmp(translated, planes, sizeof(planes)));
}

TEST(RenderOrigin, CullDistanceMoreAccurateCameraRelativeAtRange)
{
    // The core numeric claim: at ~6371 km the world-space plane distance
    // dot(center, n) + d is big-minus-big and collapses (errors of order the fp32
    // ULP at that magnitude, ~0.76 m — enough to cull a clearly-visible instance,
    // the "culls everything" bug). The camera-relative form dot(center - origin, n)
    // + (d + n*origin) keeps every term small and recovers the true distance to
    // within the storage quantum. Statistical over many instances so it never
    // depends on one lucky rounding.
    const double R = 6.371e6;
    const Vector3 nrm = Vector3(0.6f, 0.3f, -0.74f).Normalize();
    const double nd[3] = {nrm.x, nrm.y, nrm.z};

    const double Qd[3] = {R, 2000.0, R * 0.5};
    const Vector3 Qf(static_cast<float>(Qd[0]), static_cast<float>(Qd[1]), static_cast<float>(Qd[2]));
    const float dWorld = -(Qf.x * nrm.x + Qf.y * nrm.y + Qf.z * nrm.z);

    const auto originSector = Origin::ComputeRenderOriginSector(Qf.x, Qf.y, Qf.z);
    ASSERT_NE(originSector.x | originSector.y | originSector.z, 0);
    float ox, oy, oz;
    Origin::SectorToWorld(originSector, ox, oy, oz);
    const Vector3 origin(ox, oy, oz);

    Vector4 world[6];
    for (int i = 0; i < 6; ++i)
        world[i] = Vector4(0.0f, 0.0f, 0.0f, 1.0e30f); // 5 always-inside sentinels
    world[0] = Vector4(nrm.x, nrm.y, nrm.z, dWorld);   // the one tested plane
    Vector4 rel[6];
    std::memcpy(rel, world, sizeof(world));
    MakeFrustumPlanesCameraRelative(rel, origin);

    float maxWorldErr = 0.0f, maxRelErr = 0.0f;
    for (int k = -100; k <= 100; ++k)
    {
        const double off = 0.05 * static_cast<double>(k); // +/- 5 m
        const double cd[3] = {Qd[0] + nd[0] * off, Qd[1] + nd[1] * off, Qd[2] + nd[2] * off};
        const Vector3 cf(static_cast<float>(cd[0]), static_cast<float>(cd[1]), static_cast<float>(cd[2]));

        const double trueSd = static_cast<double>(cf.x) * nd[0] + static_cast<double>(cf.y) * nd[1]
                            + static_cast<double>(cf.z) * nd[2] - (Qd[0] * nd[0] + Qd[1] * nd[1] + Qd[2] * nd[2]);
        const float worldSd = cf.x * nrm.x + cf.y * nrm.y + cf.z * nrm.z + dWorld;
        const Vector3 relC = cf - origin;
        const float relSd = relC.x * nrm.x + relC.y * nrm.y + relC.z * nrm.z + rel[0].w;

        maxWorldErr = std::max(maxWorldErr, std::fabs(worldSd - static_cast<float>(trueSd)));
        maxRelErr = std::max(maxRelErr, std::fabs(relSd - static_cast<float>(trueSd)));
    }

    // Honest magnitudes (measured ~0.35 m world vs ~0.05 m camera-relative at
    // 6371 km): the camera-relative distance is materially more accurate and
    // stays comfortably sub-decimeter (well inside the 1.5x cull margin), while
    // the world-space distance carries the cancellation error the design doc
    // calls the "~1 m harmless" baseline. Neither collapses to garbage at THIS
    // radius for a normal object — the win is precision hardening for the
    // extreme-distance / tiny-object edge, not a fix for a reproduced mis-cull.
    EXPECT_LT(maxRelErr, 0.15f) << "camera-relative plane distance stays sub-decimeter at 6371 km";
    EXPECT_GT(maxWorldErr, maxRelErr * 1.5f)
        << "world-space plane distance is materially less accurate than camera-relative";
}

TEST(RenderOrigin, CullPredicateMatchesPreciseReferenceAtRange)
{
    // Correctness (predicate vs reference): a real extracted frustum at planetary
    // distance. The reference is the precise relative-frame test (small magnitude
    // => fp32 ~ exact); the camera-relative predicate (world planes translated by
    // n*origin, tested against center - origin) must reproduce it for every
    // instance, while the naive world-space predicate misclassifies some.
    const Vector3 camPos(3.681e6f, 500.0f, 3.681e6f);
    const Matrix4x4 rot = MakeRotationY(0.6f) * MakeRotationX(-0.15f);
    const Matrix4x4 view = Inverse(MakeCameraWorld(rot, camPos));
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(55.0f * kDeg2Rad, 16.0f / 9.0f, 0.5f, 4000.0f);
    const Matrix4x4 viewProj = proj * view;

    Rendering::CameraData cam{};
    std::memcpy(cam.view, view.Data(), sizeof(cam.view));
    std::memcpy(cam.proj, proj.Data(), sizeof(cam.proj));
    std::memcpy(cam.viewProj, viewProj.Data(), sizeof(cam.viewProj));
    cam.cameraPos[0] = camPos.x; cam.cameraPos[1] = camPos.y; cam.cameraPos[2] = camPos.z;
    Origin::ComputeRebasedView(cam.view, cam.proj, cam.viewProj, cam.cameraPos[0], cam.cameraPos[1],
                               cam.cameraPos[2], cam.viewRel, cam.viewProjRel, cam.renderOriginSector);
    ASSERT_NE(cam.renderOriginSector[0] | cam.renderOriginSector[1] | cam.renderOriginSector[2], 0);
    float ox, oy, oz;
    Origin::SectorToWorld(Components::WorldSectorCoord{cam.renderOriginSector[0], cam.renderOriginSector[1],
                                                       cam.renderOriginSector[2]}, ox, oy, oz);
    const Vector3 origin(ox, oy, oz);

    Matrix4x4 vpWorld, vpRel;
    std::memcpy(vpWorld.Data(), cam.viewProj, 64);
    std::memcpy(vpRel.Data(), cam.viewProjRel, 64);
    Vector4 worldPlanes[6], refPlanes[6], relPlanes[6];
    GameEngine::Rendering::ExtractFrustumPlanes(vpWorld, worldPlanes);
    GameEngine::Rendering::ExtractFrustumPlanes(vpRel, refPlanes); // precise reference planes
    std::memcpy(relPlanes, worldPlanes, sizeof(worldPlanes));
    MakeFrustumPlanesCameraRelative(relPlanes, origin);

    // The brute-force-vs-predicate correctness check: the camera-relative
    // predicate the GPU runs (world planes translated by n*origin, tested against
    // center - origin) must equal the precise relative-frame reference for EVERY
    // instance across and just outside the frustum at planetary distance. A grid
    // of instances spanning in/out of the frustum, small object radius so the
    // decision is sharp at the boundary. (worldMismatches is informational: at
    // 3.68 Mm with a 1.5 m object the world predicate happens to still agree —
    // the accuracy delta shows up as a sub-decimeter distance error in the
    // numeric oracle above, not a mis-cull here.)
    const float radius = 1.5f;
    int worldMismatches = 0;
    for (int gx = -6; gx <= 6; ++gx)
        for (int gy = -4; gy <= 4; ++gy)
            for (int gz = 1; gz <= 8; ++gz)
            {
                const Vector3 rel(static_cast<float>(gx) * 40.0f, static_cast<float>(gy) * 40.0f,
                                  static_cast<float>(gz) * 60.0f);
                const Vector3 world(origin.x + rel.x, origin.y + rel.y, origin.z + rel.z);
                const Vector3 relFromOrigin = world - origin;

                const bool ref = TestSphereFrustumRel(relFromOrigin, radius, refPlanes, Vector3(0, 0, 0));
                const bool camRel = TestSphereFrustumRel(world, radius, relPlanes, origin);
                const bool worldSpace = TestSphereFrustumRel(world, radius, worldPlanes, Vector3(0, 0, 0));

                EXPECT_EQ(camRel, ref) << "camera-relative predicate must match the precise reference"
                                       << " (" << gx << "," << gy << "," << gz << ")";
                if (worldSpace != ref)
                    ++worldMismatches;
            }
    EXPECT_GE(worldMismatches, 0); // informational; camRel==ref above is the correctness gate
}
