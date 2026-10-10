// CascadeFitStabilityTests — measures (and later locks) how often the cascade
// light-VP fit actually changes under representative camera motion.
//
// The static cascade cache (CascadeShadowCache) can only skip a re-render when
// the fit is byte-identical to the last settled render, and the motion
// round-robin can only bound — not eliminate — the re-render cost while the
// fit steps every frame. These tests drive ComputeCascades with synthetic
// orbit / fly / mouse-look trajectories at an overview-scale pose and count
// per-cascade fit changes: the measured design input for the fit-freeze lane
// (a fit that steps ~every frame under all motion classes means per-frame
// texel snapping alone can never produce motion-frame cache hits).
//
// A second group (CascadeFitWorldMagnitude) sweeps the same fit over a render-
// origin magnitude ladder instead of a trajectory, pinning which properties
// survive a planetary camera and which are bounded by the precision of the fit's
// inputs — see the comment block above those tests.
//
// Pure CPU: ComputeCascades touches no device (the feature is default-
// constructed, never Initialize()d), matching the ShadowSamplingFitTests
// precedent.

#include "Engine/Rendering/ShadowMapRenderFeature.h"

#include "Engine/Rendering/RenderOrigin.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Common/Frustum.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>
#include <iterator>
#include <limits>

using namespace GameEngine;
using namespace GameEngine::Mathematics;
namespace Renderer = GameEngine::Engine::Renderer;

using Renderer::CascadeFrameData;
using Renderer::ShadowMapRenderFeature;

namespace
{

constexpr float kPi = 3.14159265358979323846f;
constexpr float kNearPlane = 0.1f;
constexpr float kFarPlane = 1000.0f;
constexpr uint32_t kResolution = 2048;

// Overview-scale editor pose (matches the ElvenRealm gate-scene capture pose:
// position ~[70, 9, 50], yaw 300, pitch -6).
const Vector3 kOverviewPos{70.0f, 9.0f, 50.0f};
constexpr float kOverviewYawDeg = 300.0f;
constexpr float kOverviewPitchDeg = -6.0f;
// Scene sun direction (normalized below; representative outdoor key light).
const Vector3 kLightDir{0.35f, -0.65f, 0.45f};

Vector3 YawPitchForward(float yawDeg, float pitchDeg)
{
    const float yaw = yawDeg * kPi / 180.0f;
    const float pitch = pitchDeg * kPi / 180.0f;
    // LH, +Z forward, +Y up, yaw about +Y.
    return Vector3{std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                   std::cos(yaw) * std::cos(pitch)};
}

Rendering::CameraData MakeCamera(const Vector3& pos, const Vector3& forward)
{
    Rendering::CameraData cam{};
    const Matrix4x4 view = MakeLookAtLH(pos, pos + forward, Vector3{0.0f, 1.0f, 0.0f});
    const Matrix4x4 proj =
        MakePerspectiveLH_ZO_ReverseZ(60.0f * kPi / 180.0f, 16.0f / 9.0f, kNearPlane, kFarPlane);
    const Matrix4x4 viewProj = proj * view;
    std::memcpy(cam.view, view.Data(), sizeof(cam.view));
    std::memcpy(cam.proj, proj.Data(), sizeof(cam.proj));
    std::memcpy(cam.viewProj, viewProj.Data(), sizeof(cam.viewProj));
    cam.cameraPos[0] = pos.x;
    cam.cameraPos[1] = pos.y;
    cam.cameraPos[2] = pos.z;
    cam.cameraPos[3] = 0.0f;
    return cam;
}

struct ChurnStats
{
    uint32_t Frames = 0;
    uint32_t FitChanges[Renderer::kMaxShadowCascades] = {};
    double MeanStepTexels[Renderer::kMaxShadowCascades] = {};
};

// Drives ComputeCascades along a caller-supplied trajectory and counts, per
// cascade, the frames where the fit (LightVP bytes) changed vs the previous
// frame, plus the mean translation step in shadow texels when it did.
template <typename PoseFn>
ChurnStats MeasureChurn(ShadowMapRenderFeature& feature, uint32_t frames, PoseFn&& poseAt)
{
    ChurnStats stats{};
    stats.Frames = frames;
    CascadeFrameData prev{};
    bool hasPrev = false;
    double stepSum[Renderer::kMaxShadowCascades] = {};
    uint32_t stepCount[Renderer::kMaxShadowCascades] = {};
    const Vector3 lightDir = kLightDir.Normalize();

    for (uint32_t f = 0; f < frames; ++f)
    {
        Vector3 pos, fwd;
        poseAt(f, pos, fwd);
        const Rendering::CameraData cam = MakeCamera(pos, fwd);
        const CascadeFrameData fd =
            feature.ComputeCascades(cam, kNearPlane, kFarPlane, lightDir, nullptr, 1);
        if (hasPrev)
        {
            for (uint32_t c = 0; c < fd.NumCascades; ++c)
            {
                if (std::memcmp(fd.LightVP[c].Data(), prev.LightVP[c].Data(),
                                16 * sizeof(float)) != 0)
                {
                    ++stats.FitChanges[c];
                    // Clip-space translation delta in texel units (2/res per texel).
                    const float dx = fd.LightVP[c].Data()[12] - prev.LightVP[c].Data()[12];
                    const float dy = fd.LightVP[c].Data()[13] - prev.LightVP[c].Data()[13];
                    const float clipTexel = 2.0f / static_cast<float>(kResolution);
                    stepSum[c] += std::sqrt(dx * dx + dy * dy) / clipTexel;
                    ++stepCount[c];
                }
            }
        }
        prev = fd;
        hasPrev = true;
    }
    for (uint32_t c = 0; c < Renderer::kMaxShadowCascades; ++c)
        stats.MeanStepTexels[c] = stepCount[c] ? stepSum[c] / stepCount[c] : 0.0;
    return stats;
}

void PrintChurn(const char* label, const ChurnStats& s)
{
    for (uint32_t c = 0; c < Renderer::kMaxShadowCascades; ++c)
    {
        std::printf("[FitChurn] %-18s cascade %u: %4u/%u frames changed (%.1f%%), mean step "
                    "%.2f texels\n",
                    label, c, s.FitChanges[c], s.Frames - 1,
                    100.0f * static_cast<float>(s.FitChanges[c]) /
                        static_cast<float>(s.Frames - 1),
                    s.MeanStepTexels[c]);
    }
}

// The three trajectories, shared by the baseline (freeze OFF) and freeze
// lanes.
void OrbitPose(uint32_t f, Vector3& pos, Vector3& fwd)
{
    const Vector3 fwd0 = YawPitchForward(kOverviewYawDeg, kOverviewPitchDeg);
    const Vector3 pivot = kOverviewPos + fwd0 * 30.0f;
    const float radius = 30.0f;
    const float a =
        (kOverviewYawDeg + 180.0f) * kPi / 180.0f + static_cast<float>(f) * 0.15f * kPi / 180.0f;
    pos = Vector3{pivot.x + std::sin(a) * radius, kOverviewPos.y, pivot.z + std::cos(a) * radius};
    const Vector3 toPivot = pivot - pos;
    const float len =
        std::sqrt(toPivot.x * toPivot.x + toPivot.y * toPivot.y + toPivot.z * toPivot.z);
    fwd = Vector3{toPivot.x / len, toPivot.y / len, toPivot.z / len};
}

void FlyPose(uint32_t f, Vector3& pos, Vector3& fwd)
{
    const Vector3 fwd0 = YawPitchForward(kOverviewYawDeg, kOverviewPitchDeg);
    pos = kOverviewPos + fwd0 * (static_cast<float>(f) * (8.0f / 120.0f));
    fwd = fwd0;
}

void LookPose(uint32_t f, Vector3& pos, Vector3& fwd)
{
    pos = kOverviewPos;
    fwd = YawPitchForward(kOverviewYawDeg + static_cast<float>(f) * 0.25f, kOverviewPitchDeg);
}

// Receiver-coverage invariant: every frame, the overlap-extended slice the
// receivers of cascade c can select must project inside the emitted fit's
// NDC box — whether the fit refit this frame or was re-emitted frozen.
void AssertSliceCovered(const Rendering::CameraData& cam, const CascadeFrameData& fd)
{
    constexpr float kNdcEps = 1e-3f;
    Matrix4x4 camVP;
    std::memcpy(camVP.Data(), cam.viewProj, sizeof(cam.viewProj));
    for (uint32_t c = 0; c < fd.NumCascades; ++c)
    {
        const float sliceNear = (c == 0) ? kNearPlane : fd.SplitDistances[c - 1];
        const float sliceFar = fd.SplitDistances[c];
        Vector3 corners[8];
        ShadowMapRenderFeature::ComputeExtendedSliceCornersWS(camVP, kNearPlane, kFarPlane,
                                                              sliceNear, sliceFar, corners);
        for (int i = 0; i < 8; ++i)
        {
            const Matrix4x4& vp = fd.LightVP[c];
            const float* m = vp.Data();
            const float x = m[0] * corners[i].x + m[4] * corners[i].y + m[8] * corners[i].z + m[12];
            const float y = m[1] * corners[i].x + m[5] * corners[i].y + m[9] * corners[i].z + m[13];
            // Depth half of the coverage inequality: the emitted (possibly
            // frozen) reverse-Z ortho window maps receivers to NDC z in
            // [0 (far bound), 1 (near bound incl. the caster back-extension)].
            const float z =
                m[2] * corners[i].x + m[6] * corners[i].y + m[10] * corners[i].z + m[14];
            ASSERT_LE(std::abs(x), 1.0f + kNdcEps)
                << "cascade " << c << " corner " << i << " outside frozen fit (x)";
            ASSERT_LE(std::abs(y), 1.0f + kNdcEps)
                << "cascade " << c << " corner " << i << " outside frozen fit (y)";
            ASSERT_GE(z, -kNdcEps)
                << "cascade " << c << " corner " << i << " beyond frozen far depth bound";
            ASSERT_LE(z, 1.0f + kNdcEps)
                << "cascade " << c << " corner " << i << " beyond frozen near depth bound";
        }
    }
}

template <typename PoseFn>
ChurnStats MeasureChurnChecked(ShadowMapRenderFeature& feature, uint32_t frames, PoseFn&& poseAt)
{
    // Same loop as MeasureChurn plus the coverage invariant each frame.
    ChurnStats stats{};
    stats.Frames = frames;
    CascadeFrameData prev{};
    bool hasPrev = false;
    const Vector3 lightDir = kLightDir.Normalize();
    for (uint32_t f = 0; f < frames; ++f)
    {
        Vector3 pos, fwd;
        poseAt(f, pos, fwd);
        const Rendering::CameraData cam = MakeCamera(pos, fwd);
        const CascadeFrameData fd =
            feature.ComputeCascades(cam, kNearPlane, kFarPlane, lightDir, nullptr, 1);
        AssertSliceCovered(cam, fd);
        if (hasPrev)
        {
            for (uint32_t c = 0; c < fd.NumCascades; ++c)
            {
                if (std::memcmp(fd.LightVP[c].Data(), prev.LightVP[c].Data(),
                                16 * sizeof(float)) != 0)
                    ++stats.FitChanges[c];
            }
        }
        prev = fd;
        hasPrev = true;
    }
    return stats;
}

} // namespace

// ── Baseline lane (freeze OFF): the per-frame texel-snap fit. These print the
// measured churn that motivated the freeze — near-100% of motion frames step
// the fit under orbit / fly / mouse-look. ──

TEST(CascadeFitStability, MeasureOrbitChurn)
{
    ShadowMapRenderFeature feature;
    feature.SetFitFreezeEnabled(false);
    const ChurnStats s = MeasureChurn(feature, 600, OrbitPose);
    PrintChurn("orbit 18deg/s", s);
    for (uint32_t c = 0; c < 4; ++c)
        EXPECT_LE(s.FitChanges[c], s.Frames - 1);
}

TEST(CascadeFitStability, MeasureFlyChurn)
{
    ShadowMapRenderFeature feature;
    feature.SetFitFreezeEnabled(false);
    const ChurnStats s = MeasureChurn(feature, 600, FlyPose);
    PrintChurn("fly 8m/s", s);
    for (uint32_t c = 0; c < 4; ++c)
        EXPECT_LE(s.FitChanges[c], s.Frames - 1);
}

TEST(CascadeFitStability, MeasureLookChurn)
{
    ShadowMapRenderFeature feature;
    feature.SetFitFreezeEnabled(false);
    const ChurnStats s = MeasureChurn(feature, 600, LookPose);
    PrintChurn("look 30deg/s", s);
    for (uint32_t c = 0; c < 4; ++c)
        EXPECT_LE(s.FitChanges[c], s.Frames - 1);
}

// ── Freeze lane (GE_SHADOW_FIT_FREEZE): the fit must hold byte-stable across
// the vast majority of motion frames while never leaving a receiver outside
// the emitted fit (AssertSliceCovered every frame). The 10% bound is ~5x
// slack over the measured rates (~0.2-2% orbit/look, ~1-2% fly) so speed or
// snapping tweaks don't flake it, while still failing hard if the freeze
// regresses toward per-frame refits. ──

TEST(CascadeFitStability, FreezeOrbitStable)
{
    ShadowMapRenderFeature feature;
    feature.SetFitFreezeEnabled(true);
    const ChurnStats s = MeasureChurnChecked(feature, 600, OrbitPose);
    PrintChurn("FREEZE orbit", s);
    for (uint32_t c = 0; c < 4; ++c)
        EXPECT_LE(s.FitChanges[c], (s.Frames - 1) / 10) << "cascade " << c;
}

TEST(CascadeFitStability, FreezeFlyStable)
{
    ShadowMapRenderFeature feature;
    feature.SetFitFreezeEnabled(true);
    const ChurnStats s = MeasureChurnChecked(feature, 600, FlyPose);
    PrintChurn("FREEZE fly", s);
    for (uint32_t c = 0; c < 4; ++c)
        EXPECT_LE(s.FitChanges[c], (s.Frames - 1) / 10) << "cascade " << c;
}

TEST(CascadeFitStability, FreezeLookStable)
{
    ShadowMapRenderFeature feature;
    feature.SetFitFreezeEnabled(true);
    const ChurnStats s = MeasureChurnChecked(feature, 600, LookPose);
    PrintChurn("FREEZE look", s);
    for (uint32_t c = 0; c < 4; ++c)
        EXPECT_LE(s.FitChanges[c], (s.Frames - 1) / 10) << "cascade " << c;
}

TEST(CascadeFitStability, CascadeZeroUsesTighterFitBandsWhileFarCascadesKeepLegacyBands)
{
    ShadowMapRenderFeature feature;
    feature.SetFitFreezeEnabled(true);
    const Vector3 lightDir = kLightDir.Normalize();
    const Rendering::CameraData cam =
        MakeCamera(kOverviewPos, YawPitchForward(kOverviewYawDeg, kOverviewPitchDeg));
    Renderer::ShadowMapRenderFeature::SDSMBounds bounds{};
    bounds.nearDepth = 2.0f;
    bounds.farDepth = 120.0f;
    bounds.valid = true;
    const CascadeFrameData fd =
        feature.ComputeCascades(cam, kNearPlane, kFarPlane, lightDir, &bounds, 71);
    AssertSliceCovered(cam, fd);

    Matrix4x4 camVP;
    std::memcpy(camVP.Data(), cam.viewProj, sizeof(cam.viewProj));
    const Vector3 up = std::abs(Vector3::Dot(lightDir, Vector3{0, 1, 0})) > 0.99f
        ? Vector3{0, 0, 1}
        : Vector3{0, 1, 0};
    const Matrix4x4 lightRot = MakeLookAtLH(Vector3{0, 0, 0}, lightDir, up);

    for (uint32_t c = 0; c < fd.NumCascades; ++c)
    {
        const float sliceNear = c == 0 ? kNearPlane : fd.SplitDistances[c - 1];
        const float sliceFar = fd.SplitDistances[c];
        Vector3 corners[8];
        ShadowMapRenderFeature::ComputeExtendedSliceCornersWS(
            camVP, kNearPlane, kFarPlane, sliceNear, sliceFar, corners);
        // Mirror the DEFAULT (Close) extent source: the light-space AABB of the
        // slice corners. This test pins the BANDS, so it re-derives the raw
        // extent the way the fit does and asserts only the snapping; which
        // source a mode uses is pinned by CascadeShadowProjection instead.
        float minX = std::numeric_limits<float>::max();
        float minY = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float maxY = std::numeric_limits<float>::lowest();
        for (const Vector3& corner : corners)
        {
            const Vector4 ls = lightRot.Transform(Vector4{corner.x, corner.y, corner.z, 1.0f});
            minX = std::min(minX, ls.x);
            minY = std::min(minY, ls.y);
            maxX = std::max(maxX, ls.x);
            maxY = std::max(maxY, ls.y);
        }

        const float guard = c == 0 ? 1.0f / 16.0f : 1.0f / 8.0f;
        const float bandFraction = c == 0 ? 1.0f / 16.0f : 1.0f / 8.0f;
        const float rawHalf = 0.5f * std::max(maxX - minX, maxY - minY) * (1.0f + guard);
        const float magnitude = std::pow(2.0f, std::ceil(std::log2(std::max(rawHalf, 1e-3f))));
        const float expected = std::ceil(rawHalf / (magnitude * bandFraction)) *
                               (magnitude * bandFraction);
        EXPECT_FLOAT_EQ(fd.OrthoHalfExtent[c], expected) << "cascade " << c;
    }
}

// Mid-freeze render-origin sector step: the frozen VP bakes the origin
// rebase, so crossing a sector boundary must refit immediately — recapturing
// the cull snapshot from THIS frame's camera and rebasing LightVPRel against
// the new sector — even when the travel itself (0.1 m) sits far inside every
// freeze budget (i.e. only the sector gate can force the refit).
TEST(CascadeFitStability, FreezeOriginSectorStepRefits)
{
    ShadowMapRenderFeature feature;
    feature.SetFitFreezeEnabled(true);
    const Vector3 fwd = YawPitchForward(kOverviewYawDeg, kOverviewPitchDeg);
    const Vector3 lightDir = kLightDir.Normalize();
    // Beyond the render-origin activation radius (32768 m), straddling the
    // sector-32/33 boundary at 32.5 * 1024 = 33280 m.
    const Rendering::CameraData camA = MakeCamera(Vector3{33279.95f, 9.0f, 50.0f}, fwd);
    const Rendering::CameraData camB = MakeCamera(Vector3{33280.05f, 9.0f, 50.0f}, fwd);

    const CascadeFrameData a =
        feature.ComputeCascades(camA, kNearPlane, kFarPlane, lightDir, nullptr, 21);
    ASSERT_EQ(a.RenderOriginSector[0], 32);
    AssertSliceCovered(camA, a);
    const CascadeFrameData a2 =
        feature.ComputeCascades(camA, kNearPlane, kFarPlane, lightDir, nullptr, 21);
    for (uint32_t c = 0; c < a.NumCascades; ++c)
        ASSERT_EQ(0, std::memcmp(a.LightVP[c].Data(), a2.LightVP[c].Data(), 64)) << "cascade " << c;

    const CascadeFrameData b =
        feature.ComputeCascades(camB, kNearPlane, kFarPlane, lightDir, nullptr, 21);
    EXPECT_EQ(b.RenderOriginSector[0], 33);
    AssertSliceCovered(camB, b);
    for (uint32_t c = 0; c < b.NumCascades; ++c)
    {
        // A wrong reuse would keep camA's frozen cull snapshot and a VPRel
        // rebased against sector 32 (receivers sample origin-relative — a
        // stale rebase misses every cascade lookup).
        EXPECT_EQ(0, std::memcmp(b.CullCameraViewProj[c].Data(), camB.viewProj, 64))
            << "cascade " << c << " kept the pre-step frozen cull snapshot";
        EXPECT_NE(0, std::memcmp(b.LightVPRel[c].Data(), a.LightVPRel[c].Data(), 64))
            << "cascade " << c << " kept the pre-step origin rebase";
    }
}

// Animated-sun degeneration (documented cost): the freeze compares the light
// direction EXACTLY, so a continuously slewing sun (time-of-day) refits every
// cascade every frame — the freeze banks nothing while the guard band still
// costs its worldPerTexel. Locked here so the tradeoff stays visible; a
// slew-aware lane (light-direction quantization) is the tracked follow-up.
TEST(CascadeFitStability, FreezeLightSlewRefitsEveryFrame)
{
    ShadowMapRenderFeature feature;
    feature.SetFitFreezeEnabled(true);
    const Vector3 fwd = YawPitchForward(kOverviewYawDeg, kOverviewPitchDeg);
    const Rendering::CameraData cam = MakeCamera(kOverviewPos, fwd);

    CascadeFrameData prev{};
    bool hasPrev = false;
    for (uint32_t f = 0; f < 60; ++f)
    {
        // ~0.02 deg/frame yaw about +Y — a slow time-of-day sun.
        const float a = static_cast<float>(f) * 0.02f * kPi / 180.0f;
        const Vector3 lightDir = Vector3{0.35f * std::cos(a) + 0.45f * std::sin(a), -0.65f,
                                         0.45f * std::cos(a) - 0.35f * std::sin(a)}
                                     .Normalize();
        const CascadeFrameData fd =
            feature.ComputeCascades(cam, kNearPlane, kFarPlane, lightDir, nullptr, 31);
        AssertSliceCovered(cam, fd);
        if (hasPrev)
        {
            for (uint32_t c = 0; c < fd.NumCascades; ++c)
                EXPECT_NE(0, std::memcmp(fd.LightVP[c].Data(), prev.LightVP[c].Data(), 64))
                    << "cascade " << c << " frame " << f;
        }
        prev = fd;
        hasPrev = true;
    }
}

// Shrink refit (rev-2 F2): SDSM is active in the shipped config, and its
// contraction shrinks the slices while receiver containment keeps passing —
// without the shrink test in CanReuseFrozenCascadeFit the frozen box would
// pass forever and worldPerTexel stays pinned at capture size. A deep
// contraction must refit smaller; band-scale breathing must not oscillate.
TEST(CascadeFitStability, FreezeShrinkRefitsDeepContractionWithoutOscillation)
{
    ShadowMapRenderFeature feature;
    feature.SetFitFreezeEnabled(true);
    // Close projection: this pins how the freeze reacts to SDSM breathing and to
    // a deep SDSM contraction. Under Stable the splits ignore SDSM entirely, so
    // there is nothing for the shrink path to react to and the assertions below
    // are meaningless rather than wrong.
    feature.SetShadowProjection(Renderer::ShadowProjection::Close);
    const Vector3 fwd = YawPitchForward(kOverviewYawDeg, kOverviewPitchDeg);
    const Rendering::CameraData cam = MakeCamera(kOverviewPos, fwd);
    const Vector3 lightDir = kLightDir.Normalize();

    ShadowMapRenderFeature::SDSMBounds wide{};
    wide.nearDepth = 0.5f;
    wide.farDepth = 90.0f;
    wide.valid = true;
    ShadowMapRenderFeature::SDSMBounds slight = wide;
    slight.farDepth = 89.0f;

    const CascadeFrameData a =
        feature.ComputeCascades(cam, kNearPlane, kFarPlane, lightDir, &wide, 11);
    AssertSliceCovered(cam, a);

    // Band-scale SDSM breathing (90 <-> 89, ~1%): at most one alignment refit
    // total (a raw extent straddling a power-of-two rebuckets once), never a
    // per-frame ping-pong — the band + hysteresis deadband absorb it.
    CascadeFrameData prev = a;
    uint32_t breathChanges[Renderer::kMaxShadowCascades] = {};
    for (uint32_t f = 0; f < 12; ++f)
    {
        const CascadeFrameData fd = feature.ComputeCascades(
            cam, kNearPlane, kFarPlane, lightDir, (f & 1) ? &slight : &wide, 11);
        AssertSliceCovered(cam, fd);
        for (uint32_t c = 0; c < fd.NumCascades; ++c)
        {
            if (std::memcmp(fd.LightVP[c].Data(), prev.LightVP[c].Data(), 64) != 0)
                ++breathChanges[c];
        }
        prev = fd;
    }
    for (uint32_t c = 0; c < prev.NumCascades; ++c)
        EXPECT_LE(breathChanges[c], 1u) << "cascade " << c << " oscillates on SDSM breathing";

    // Deep contraction (~3x): cascade 0's shrunk slice stays NESTED in the
    // frozen one (both anchor at the camera near plane), so receiver
    // containment passes and ONLY the shrink test can force the refit — the
    // half-extent must come down instead of staying pinned.
    ShadowMapRenderFeature::SDSMBounds shrunk = wide;
    shrunk.farDepth = 30.0f;
    const CascadeFrameData d =
        feature.ComputeCascades(cam, kNearPlane, kFarPlane, lightDir, &shrunk, 11);
    AssertSliceCovered(cam, d);
    EXPECT_NE(0, std::memcmp(prev.LightVP[0].Data(), d.LightVP[0].Data(), 64))
        << "cascade 0 kept the oversize frozen fit across a deep SDSM contraction";
    EXPECT_LT(d.OrthoHalfExtent[0], prev.OrthoHalfExtent[0]);

    // The shrunk fit re-freezes: identical follow-up frames are byte-stable.
    const CascadeFrameData e =
        feature.ComputeCascades(cam, kNearPlane, kFarPlane, lightDir, &shrunk, 11);
    for (uint32_t c = 0; c < e.NumCascades; ++c)
        EXPECT_EQ(0, std::memcmp(d.LightVP[c].Data(), e.LightVP[c].Data(), 64)) << "cascade " << c;
}

// A static camera must re-emit the frozen fit byte-for-byte, and a light
// direction change must refit immediately (the frozen VP bakes the light
// rotation — reusing it under a moved sun would freeze stale shadows).
TEST(CascadeFitStability, FreezeStaticCameraBytesStable_LightChangeRefits)
{
    ShadowMapRenderFeature feature;
    feature.SetFitFreezeEnabled(true);
    const Vector3 fwd = YawPitchForward(kOverviewYawDeg, kOverviewPitchDeg);
    const Rendering::CameraData cam = MakeCamera(kOverviewPos, fwd);
    const Vector3 lightA = kLightDir.Normalize();
    const Vector3 lightB = Vector3{-0.2f, -0.8f, 0.3f}.Normalize();

    const CascadeFrameData a =
        feature.ComputeCascades(cam, kNearPlane, kFarPlane, lightA, nullptr, 7);
    const CascadeFrameData b =
        feature.ComputeCascades(cam, kNearPlane, kFarPlane, lightA, nullptr, 7);
    for (uint32_t c = 0; c < a.NumCascades; ++c)
    {
        EXPECT_EQ(0, std::memcmp(a.LightVP[c].Data(), b.LightVP[c].Data(), 64));
        EXPECT_EQ(0, std::memcmp(a.LightVPRel[c].Data(), b.LightVPRel[c].Data(), 64));
        EXPECT_EQ(0, std::memcmp(a.CullCameraViewProj[c].Data(), b.CullCameraViewProj[c].Data(),
                                 64));
    }

    const CascadeFrameData d =
        feature.ComputeCascades(cam, kNearPlane, kFarPlane, lightB, nullptr, 7);
    for (uint32_t c = 0; c < d.NumCascades; ++c)
        EXPECT_NE(0, std::memcmp(a.LightVP[c].Data(), d.LightVP[c].Data(), 64))
            << "cascade " << c << " kept a frozen fit across a light direction change";
    AssertSliceCovered(cam, d);
}

// ── Render-origin magnitude sweep (|eye| ladder instead of a trajectory) ──
// The cascade fit is built in the render-origin-relative frame, while
// ComputeExtendedSliceCornersWS (the cull path) still works in world space.
// These tests pin which properties hold at a planetary camera and which are
// bounded by the precision of the fit's own inputs, so the numbers stay attached
// to the code instead of to prose:
//
//   * emitted OrthoHalfExtent is translation-invariant to within the snap band —
//     the fit does NOT inflate extents at planetary distance (an ortho box is
//     large at a planetary pose only because MaxShadowDistance is large, and it
//     is then equally large at the origin);
//   * the WORLD-space slice corners (the cull path's) lose ~ULP(|eye|), i.e.
//     sub-metre at Earth radius, and their raw half-extent well under 0.5% —
//     harmless for culling, which carries metres of slack, but 19-22 shadow
//     texels of cascade 0, which is why the FIT is not built from them;
//   * the emitted fit contains the TRUE slice at every magnitude (fitting the
//     world corners instead let the slice fall tens of texels outside the box);
//   * a static receiver's shadow texel PHASE is translation-invariant — the
//     invariant that decides whether a fit step is visible at all;
//   * the fit's step granularity tracks, and does not amplify, the fp32 world
//     camera position it is handed (0.5 m at Earth radius).

namespace
{

// Planetary camera the way the engine builds one (#660 double rig): the SAME
// rotation 3x3 as the near-origin pose, translation column rebuilt as -R*eye in
// double. Never MakeLookAtLH(eye, eye + fwd) — that re-derives the direction as
// fl(eye + fwd) - eye and quantizes the camera BASIS at ULP(|eye|) (degrees at
// Earth radius), which would rotate the frustum and confound a translation-only
// comparison.
Rendering::CameraData MakeCameraAt(const Matrix4x4& rot3x3, double ex, double ey, double ez,
                                  float nearP, float farP)
{
    Rendering::CameraData cam{};
    Matrix4x4 view = rot3x3;
    float* v = view.Data();
    v[12] = static_cast<float>(-(static_cast<double>(v[0]) * ex + static_cast<double>(v[4]) * ey +
                                 static_cast<double>(v[8]) * ez));
    v[13] = static_cast<float>(-(static_cast<double>(v[1]) * ex + static_cast<double>(v[5]) * ey +
                                 static_cast<double>(v[9]) * ez));
    v[14] = static_cast<float>(-(static_cast<double>(v[2]) * ex + static_cast<double>(v[6]) * ey +
                                 static_cast<double>(v[10]) * ez));
    v[15] = 1.0f;
    const Matrix4x4 proj =
        MakePerspectiveLH_ZO_ReverseZ(60.0f * kPi / 180.0f, 16.0f / 9.0f, nearP, farP);
    const Matrix4x4 viewProj = proj * view;
    std::memcpy(cam.view, view.Data(), sizeof(cam.view));
    std::memcpy(cam.proj, proj.Data(), sizeof(cam.proj));
    std::memcpy(cam.viewProj, viewProj.Data(), sizeof(cam.viewProj));
    cam.cameraPos[0] = static_cast<float>(ex);
    cam.cameraPos[1] = static_cast<float>(ey);
    cam.cameraPos[2] = static_cast<float>(ez);
    return cam;
}

// Exact (double) extended-slice corners: the reference the fp32 production path
// is measured against. Mirrors ComputeExtendedSliceCornersWS' fraction
// arithmetic (same overlap fraction, same normalization against the full camera
// range) with the camera basis and eye carried in double.
void ExactSliceCorners(const Matrix4x4& rot3x3, double ex, double ey, double ez, float nearP,
                       float farP, float sliceNear, float sliceFar, double out[8][3])
{
    // View rows are the camera basis: right=(m0,m4,m8), up=(m1,m5,m9), fwd=(m2,m6,m10).
    const float* m = rot3x3.Data();
    const double rx = m[0], ry = m[4], rz = m[8];
    const double ux = m[1], uy = m[5], uz = m[9];
    const double fx = m[2], fy = m[6], fz = m[10];
    const double tanHalfFovY = std::tan(30.0 * 3.14159265358979323846 / 180.0);
    const double aspect = 16.0 / 9.0;
    const double span = static_cast<double>(farP) - nearP;
    const double range = span > 1e-4 ? span : 1e-4;
    double cascadeNear = (static_cast<double>(sliceNear) - nearP) / range;
    double cascadeFar = (static_cast<double>(sliceFar) - nearP) / range;
    const double overlap = (cascadeFar - cascadeNear) * 0.2; // kCascadeOverlapFraction
    cascadeNear = std::max(0.0, cascadeNear - overlap);
    cascadeFar = std::min(1.0, cascadeFar + overlap);
    const double depthNear = nearP + span * cascadeNear;
    const double depthFar = nearP + span * cascadeFar;
    const int signX[4] = {-1, 1, 1, -1};
    const int signY[4] = {-1, -1, 1, 1};
    for (int i = 0; i < 4; ++i)
    {
        for (int half = 0; half < 2; ++half)
        {
            const double d = half ? depthFar : depthNear;
            const double h = d * tanHalfFovY;
            const double w = h * aspect;
            out[i + half * 4][0] = ex + fx * d + rx * w * signX[i] + ux * h * signY[i];
            out[i + half * 4][1] = ey + fy * d + ry * w * signX[i] + uy * h * signY[i];
            out[i + half * 4][2] = ez + fz * d + rz * w * signX[i] + uz * h * signY[i];
        }
    }
}

// Raw (unsnapped) square half-extent of a corner set in light space, reduced in
// double about `origin` so the reduction itself contributes no error.
double RawHalfExtent(const Matrix4x4& lightRot, const double corners[8][3], const double origin[3])
{
    const float* m = lightRot.Data();
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    for (int i = 0; i < 8; ++i)
    {
        const double px = corners[i][0] - origin[0];
        const double py = corners[i][1] - origin[1];
        const double pz = corners[i][2] - origin[2];
        const double lx = static_cast<double>(m[0]) * px + static_cast<double>(m[4]) * py +
                          static_cast<double>(m[8]) * pz;
        const double ly = static_cast<double>(m[1]) * px + static_cast<double>(m[5]) * py +
                          static_cast<double>(m[9]) * pz;
        minX = std::min(minX, lx); maxX = std::max(maxX, lx);
        minY = std::min(minY, ly); maxY = std::max(maxY, ly);
    }
    return std::max(maxX - minX, maxY - minY) * 0.5;
}

const Matrix4x4& OverviewRotation()
{
    static const Matrix4x4 rot = MakeLookAtLH(
        Vector3{0, 0, 0}, YawPitchForward(kOverviewYawDeg, kOverviewPitchDeg), Vector3{0, 1, 0});
    return rot;
}

// |eye| ladder: inside the activation radius (origin inactive, sector 0) through
// planetary. Same local offset at every rung so the frustum is congruent.
struct OriginMagnitudePose
{
    const char* Name;
    double X, Y, Z;
};
constexpr OriginMagnitudePose kOriginLadder[] = {
    {"origin", 70.0, 9.0, 50.0},
    {"5e4", 50070.0, 9.0, 50.0},
    {"5e5", 500070.0, 9.0, 50.0},
    {"Earth", 6371070.0, 9.0, 50.0},
};

} // namespace

// Extents are exactly translation-invariant in real arithmetic (a rigid shift of
// the frustum shifts its light-space AABB without resizing it), so the emitted
// half-extent at a planetary pose must match the near-origin control. The bound
// is one 12.5% snap band with margin — tight enough that any inflation of the
// kind the fit is suspected of (orders of magnitude) fails hard, loose enough
// that a pose sitting on a band edge cannot flake it.
//
// This is also the invariant a rel-space fit must PRESERVE: it may not change
// which extent band a given pose lands in.
TEST(CascadeFitWorldMagnitude, EmittedExtentsAreTranslationInvariant)
{
    const Vector3 lightDir = kLightDir.Normalize();
    // MaxShadowDistance dominates the extents; sweep the shipped value
    // (ForwardPlus.rendergraph = 200) through planetary shadow ranges, and the
    // camera far plane independently (the slice interpolation normalizes against
    // the FULL camera range, so a planetary far clip changes the fractions).
    const float maxDistances[] = {200.0f, 2000.0f, 20000.0f, 200000.0f};
    const float farPlanes[] = {1000.0f, 100000.0f, 10000000.0f};
    constexpr float kBandTolerance = 1.30f; // one 12.5% band + margin

    uint32_t viewId = 3100;
    for (float maxDist : maxDistances)
    {
        for (float farPlane : farPlanes)
        {
            if (farPlane < maxDist)
                continue; // the shadow range would be clamped by the far clip
            float control[Renderer::kMaxShadowCascades] = {};
            for (size_t p = 0; p < std::size(kOriginLadder); ++p)
            {
                const OriginMagnitudePose& pose = kOriginLadder[p];
                ShadowMapRenderFeature feature;
                feature.SetFitFreezeEnabled(false);
                feature.ApplyRuntimeShadowSettings(maxDist, 0.5f, 0.0001f, 0.02f);
                const Rendering::CameraData cam =
                    MakeCameraAt(OverviewRotation(), pose.X, pose.Y, pose.Z, kNearPlane, farPlane);
                const CascadeFrameData fd = feature.ComputeCascades(cam, kNearPlane, farPlane,
                                                                    lightDir, nullptr, ++viewId);
                for (uint32_t c = 0; c < fd.NumCascades; ++c)
                {
                    if (p == 0)
                    {
                        control[c] = fd.OrthoHalfExtent[c];
                        ASSERT_GT(control[c], 0.0f);
                        continue;
                    }
                    EXPECT_LE(fd.OrthoHalfExtent[c], control[c] * kBandTolerance)
                        << "maxDist " << maxDist << " far " << farPlane << " cascade " << c
                        << " pose " << pose.Name << ": extent inflated vs the near-origin control ("
                        << fd.OrthoHalfExtent[c] << " vs " << control[c] << ")";
                    EXPECT_GE(fd.OrthoHalfExtent[c], control[c] / kBandTolerance)
                        << "maxDist " << maxDist << " far " << farPlane << " cascade " << c
                        << " pose " << pose.Name << ": extent collapsed vs the near-origin control ("
                        << fd.OrthoHalfExtent[c] << " vs " << control[c] << ")";
                }
            }
        }
    }
}

// How much precision the world-magnitude fit actually loses: the production
// world-space slice corners vs an exact double reference, and the raw
// (pre-snap) half-extent that follows from them. The near-origin rung is the
// positive control — it must read ~zero, so a run where BOTH rungs read zero
// (a broken reference) cannot pass as a clean result.
TEST(CascadeFitWorldMagnitude, SliceCornerErrorStaysSubMetreAtEarthRadius)
{
    const Vector3 lightDir = kLightDir.Normalize();
    const Matrix4x4 lightRot = MakeLookAtLH(Vector3{0, 0, 0}, lightDir, Vector3{0, 1, 0});
    constexpr float kPlanetFar = 10000000.0f;
    // Bounds with ~5x margin over measured (Earth radius: corner error 0.23-0.86 m,
    // raw half-extent error <= 0.073%). The point of the assertion is the ORDER of
    // magnitude: metres, not kilometres.
    constexpr double kMaxCornerErrorEarth = 4.0;
    constexpr double kMaxRawExtentErrorFraction = 0.005;
    constexpr double kMaxCornerErrorControl = 0.01;

    uint32_t viewId = 3300;
    for (const OriginMagnitudePose& pose : kOriginLadder)
    {
        const bool isControl = std::fabs(pose.X) < 1000.0;
        ShadowMapRenderFeature feature;
        feature.SetFitFreezeEnabled(false);
        feature.ApplyRuntimeShadowSettings(200.0f, 0.5f, 0.0001f, 0.02f);
        const Rendering::CameraData cam =
            MakeCameraAt(OverviewRotation(), pose.X, pose.Y, pose.Z, kNearPlane, kPlanetFar);
        const CascadeFrameData fd =
            feature.ComputeCascades(cam, kNearPlane, kPlanetFar, lightDir, nullptr, ++viewId);
        Matrix4x4 cameraViewProj;
        std::memcpy(cameraViewProj.Data(), cam.viewProj, 64);
        const double originWorld[3] = {
            static_cast<double>(fd.RenderOriginSector[0]) * Renderer::kSectorSize,
            static_cast<double>(fd.RenderOriginSector[1]) * Renderer::kSectorSize,
            static_cast<double>(fd.RenderOriginSector[2]) * Renderer::kSectorSize};

        for (uint32_t c = 0; c < fd.NumCascades; ++c)
        {
            const float sliceNear = (c == 0) ? kNearPlane : fd.SplitDistances[c - 1];
            const float sliceFar = fd.SplitDistances[c];
            Vector3 produced[8];
            ShadowMapRenderFeature::ComputeExtendedSliceCornersWS(cameraViewProj, kNearPlane,
                                                                  kPlanetFar, sliceNear, sliceFar,
                                                                  produced);
            double exact[8][3];
            ExactSliceCorners(OverviewRotation(), pose.X, pose.Y, pose.Z, kNearPlane, kPlanetFar,
                              sliceNear, sliceFar, exact);
            double producedD[8][3];
            double maxCornerError = 0.0;
            for (int i = 0; i < 8; ++i)
            {
                producedD[i][0] = produced[i].x;
                producedD[i][1] = produced[i].y;
                producedD[i][2] = produced[i].z;
                for (int k = 0; k < 3; ++k)
                    maxCornerError =
                        std::max(maxCornerError, std::fabs(producedD[i][k] - exact[i][k]));
            }
            const double rawProduced = RawHalfExtent(lightRot, producedD, originWorld);
            const double rawExact = RawHalfExtent(lightRot, exact, originWorld);
            ASSERT_GT(rawExact, 0.0);
            const double extentErrorFraction = std::fabs(rawProduced - rawExact) / rawExact;
            std::printf("[FitMagnitude] %-6s c%u cornerErr=%9.4f m  rawHalf=%11.4f (err %8.5f%%)\n",
                        pose.Name, c, maxCornerError, rawExact, 100.0 * extentErrorFraction);
            if (isControl)
            {
                EXPECT_LE(maxCornerError, kMaxCornerErrorControl)
                    << "cascade " << c << ": the near-origin control must be exact — a nonzero "
                       "reading here means the double reference, not the fit, is wrong";
            }
            else
            {
                EXPECT_LE(maxCornerError, kMaxCornerErrorEarth)
                    << "pose " << pose.Name << " cascade " << c;
            }
            EXPECT_LE(extentErrorFraction, kMaxRawExtentErrorFraction)
                << "pose " << pose.Name << " cascade " << c;
        }
    }
}

// The fit may not AMPLIFY the quantization of its own input. CameraData carries
// the camera position (and the view matrix's translation column) as fp32 WORLD
// values, so at Earth radius the smallest camera motion the fit can even observe
// is ULP(6.371e6) = 0.5 m — 12.8 texels of cascade 0, and only 5 distinct camera
// positions exist along this 2 m ladder. A perfect fit therefore steps
// ULP(|eye|)/texelWorldSize texels at planetary magnitude, not one; asking for
// one-texel steps there asks the fit to resolve motion its input cannot express.
// (Finer camera motion needs a sector-relative camera position in CameraData —
// upstream of this file.)
//
// The bound is that input floor — computed per rung from ULP(|eye|) and the
// cascade's own texel size — times the sqrt(2) two-axis diagonal, plus a rounding
// texel per axis. One formula covers every rung: at the origin the floor
// collapses to zero and the bound becomes the 2.5-texel near-origin control, so
// no magnitude branch is needed.
//
// What this pin is NOT: a visual-artifact bound. An integral-texel step of the
// emitted translation at unchanged extent is a pure window slide — a static
// receiver keeps sampling the identical sub-texel phase, so nothing moves on
// screen (StaticReceiverTexelPhaseIsTranslationInvariant measures exactly that).
// Amplified stepping matters because it comes from a mis-centred, under-sized
// box, which is what EmittedFitContainsTrueSliceAtPlanetaryMagnitude pins.
TEST(CascadeFitWorldMagnitude, RelFitTexelStepStaysInputLimitedAtPlanetaryMagnitude)
{
    const Vector3 lightDir = kLightDir.Normalize();
    constexpr float kClipTexel = 2.0f / static_cast<float>(kResolution);
    constexpr float kPlanetFar = 10000000.0f;
    constexpr double kLadderStepMetres = 0.01;
    constexpr int kLadderSteps = 200;
    // Slack over the input floor: the sqrt(2) diagonal of a simultaneous X+Y
    // step, plus one rounding texel on each axis.
    constexpr double kDiagonal = 1.4142135623730951;
    constexpr double kRoundingSlackTexels = 2.5;

    uint32_t viewId = 3500;
    for (const OriginMagnitudePose& pose : kOriginLadder)
    {
        // The granularity of the camera position the fit is handed.
        const float eyeMag =
            static_cast<float>(std::max({std::fabs(pose.X), std::fabs(pose.Y), std::fabs(pose.Z)}));
        const double eyeUlp = static_cast<double>(
            std::nextafterf(eyeMag, std::numeric_limits<float>::infinity()) - eyeMag);
        ShadowMapRenderFeature feature;
        feature.SetFitFreezeEnabled(false);
        feature.ApplyRuntimeShadowSettings(200.0f, 0.5f, 0.0001f, 0.02f);
        const uint32_t vid = ++viewId;
        double maxStep[Renderer::kMaxShadowCascades] = {};
        uint32_t nonIntegralSteps[Renderer::kMaxShadowCascades] = {};
        uint32_t movedFrames[Renderer::kMaxShadowCascades] = {};
        CascadeFrameData prev{};
        bool hasPrev = false;
        for (int s = 0; s < kLadderSteps; ++s)
        {
            const Rendering::CameraData cam =
                MakeCameraAt(OverviewRotation(), pose.X + kLadderStepMetres * s, pose.Y, pose.Z,
                             kNearPlane, kPlanetFar);
            const CascadeFrameData fd =
                feature.ComputeCascades(cam, kNearPlane, kPlanetFar, lightDir, nullptr, vid);
            if (hasPrev)
            {
                for (uint32_t c = 0; c < fd.NumCascades; ++c)
                {
                    const float* before = prev.LightVPRel[c].Data();
                    const float* after = fd.LightVPRel[c].Data();
                    const double dx = (static_cast<double>(after[12]) - before[12]) / kClipTexel;
                    const double dy = (static_cast<double>(after[13]) - before[13]) / kClipTexel;
                    const double stepTexels = std::sqrt(dx * dx + dy * dy);
                    if (stepTexels <= 1e-4)
                        continue;
                    ++movedFrames[c];
                    maxStep[c] = std::max(maxStep[c], stepTexels);
                    if (std::fabs(dx - std::round(dx)) > 0.05 ||
                        std::fabs(dy - std::round(dy)) > 0.05)
                        ++nonIntegralSteps[c];
                }
            }
            prev = fd;
            hasPrev = true;
        }
        for (uint32_t c = 0; c < prev.NumCascades; ++c)
        {
            const double texelWorld = 2.0 * static_cast<double>(prev.OrthoHalfExtent[c]) /
                                      static_cast<double>(kResolution);
            ASSERT_GT(texelWorld, 0.0);
            const double inputFloorTexels = eyeUlp / texelWorld;
            const double bound = inputFloorTexels * kDiagonal + kRoundingSlackTexels;
            std::printf("[FitTexelStep] %-6s c%u moved=%3u/%d  maxStep=%7.3f texels  "
                        "inputFloor=%6.2f  bound=%6.2f  nonIntegral=%u  halfExtent=%.1f\n",
                        pose.Name, c, movedFrames[c], kLadderSteps - 1, maxStep[c],
                        inputFloorTexels, bound, nonIntegralSteps[c], prev.OrthoHalfExtent[c]);
            // The clip-space rel snap must keep every step on the texel grid at
            // every magnitude — a FRACTIONAL step would be visible (swim).
            EXPECT_EQ(0u, nonIntegralSteps[c]) << "pose " << pose.Name << " cascade " << c;
            EXPECT_LE(maxStep[c], bound)
                << "pose " << pose.Name << " cascade " << c
                << ": the fit amplifies its input's own fp32 granularity (" << eyeUlp << " m = "
                << inputFloorTexels << " texels) instead of tracking it";
        }
    }
}

// The defect a render-origin-relative fit is FOR. The cascade box is fitted to
// frustum corners that come out of an fp32 inverse view-projection: at world
// magnitude each corner carries ~ULP(|eye|) — measured 0.73-0.86 m at Earth
// radius by SliceCornerErrorStaysSubMetreAtEarthRadius, i.e. 19-22 texels of
// cascade 0. That error both mis-centres the box (the light-space centre snap
// then rounds a ~1.6e8 quotient whose fp32 ULP is 16 texels) and sizes it against
// the wrong corners, so the TRUE slice can fall OUTSIDE the emitted box — and
// receivers in that band sample past the cascade.
//
// The box is the slice's own AABB rounded UP to a 12.5% band, so how much margin
// a pose has depends on where its raw extent sits inside that band: a pose just
// above a band edge has none. Sweeping MaxShadowDistance walks every cascade
// through its band repeatedly, which is what turns a centring error into an
// out-of-box slice. Coverage geometry is the EXACT (double) slice, because the
// receivers that can select this cascade sit at true world positions.
TEST(CascadeFitWorldMagnitude, EmittedFitContainsTrueSliceAtPlanetaryMagnitude)
{
    const Vector3 lightDir = kLightDir.Normalize();
    constexpr float kPlanetFar = 10000000.0f;
    constexpr int kBandSweepSteps = 400;
    // Containment tolerance. The band snap can land flush against the raw extent,
    // so a sub-texel overhang is the fit being tight, not wrong; magnitude-driven
    // failure is tens of texels (measured 52 at Earth radius pre-fix).
    constexpr double kMaxOutsideTexels = 1.5;
    // Positive control: the exact slice must actually FILL most of the box. A
    // broken reference (zeros, or the wrong frame) would project near the centre
    // and read a huge margin, which must not pass as containment.
    constexpr double kMinSliceFillNdc = 0.5;

    uint32_t viewId = 3700;
    for (const OriginMagnitudePose& pose : kOriginLadder)
    {
        double worstNdc[Renderer::kMaxShadowCascades] = {};
        float worstAt[Renderer::kMaxShadowCascades] = {};
        for (int step = 0; step < kBandSweepSteps; ++step)
        {
            const float maxDist = 50.0f + static_cast<float>(step);
            ShadowMapRenderFeature feature;
            feature.SetFitFreezeEnabled(false);
            feature.ApplyRuntimeShadowSettings(maxDist, 0.5f, 0.0001f, 0.02f);
            const Rendering::CameraData cam =
                MakeCameraAt(OverviewRotation(), pose.X, pose.Y, pose.Z, kNearPlane, kPlanetFar);
            const CascadeFrameData fd =
                feature.ComputeCascades(cam, kNearPlane, kPlanetFar, lightDir, nullptr, ++viewId);
            const double originWorld[3] = {
                static_cast<double>(fd.RenderOriginSector[0]) * Renderer::kSectorSize,
                static_cast<double>(fd.RenderOriginSector[1]) * Renderer::kSectorSize,
                static_cast<double>(fd.RenderOriginSector[2]) * Renderer::kSectorSize};

            for (uint32_t c = 0; c < fd.NumCascades; ++c)
            {
                const float sliceNear = (c == 0) ? kNearPlane : fd.SplitDistances[c - 1];
                const float sliceFar = fd.SplitDistances[c];
                double exact[8][3];
                ExactSliceCorners(OverviewRotation(), pose.X, pose.Y, pose.Z, kNearPlane,
                                  kPlanetFar, sliceNear, sliceFar, exact);
                const float* vp = fd.LightVPRel[c].Data();
                double worst = 0.0;
                for (int i = 0; i < 8; ++i)
                {
                    // The receiver position the vertex stage reconstructs: exact
                    // world minus the integer render origin.
                    const float px = static_cast<float>(exact[i][0] - originWorld[0]);
                    const float py = static_cast<float>(exact[i][1] - originWorld[1]);
                    const float pz = static_cast<float>(exact[i][2] - originWorld[2]);
                    const double cx = static_cast<double>(vp[0]) * px +
                                      static_cast<double>(vp[4]) * py +
                                      static_cast<double>(vp[8]) * pz + vp[12];
                    const double cy = static_cast<double>(vp[1]) * px +
                                      static_cast<double>(vp[5]) * py +
                                      static_cast<double>(vp[9]) * pz + vp[13];
                    worst = std::max(worst, std::max(std::fabs(cx), std::fabs(cy)));
                }
                if (worst > worstNdc[c])
                {
                    worstNdc[c] = worst;
                    worstAt[c] = maxDist;
                }
            }
        }
        for (uint32_t c = 0; c < Renderer::kMaxShadowCascades; ++c)
        {
            const double outsideTexels =
                (worstNdc[c] - 1.0) * 0.5 * static_cast<double>(kResolution);
            std::printf("[FitCoverage] %-6s c%u worst|ndc|=%.6f (%+8.2f texels outside) at "
                        "maxDist=%6.0f\n",
                        pose.Name, c, worstNdc[c], outsideTexels, worstAt[c]);
            EXPECT_GE(worstNdc[c], kMinSliceFillNdc)
                << "pose " << pose.Name << " cascade " << c
                << ": the exact slice barely registers inside the emitted box — the reference "
                   "geometry, not the fit, is wrong";
            EXPECT_LE(outsideTexels, kMaxOutsideTexels)
                << "pose " << pose.Name << " cascade " << c << " (maxShadowDistance " << worstAt[c]
                << "): the true slice falls outside the emitted cascade box, so receivers in that "
                   "band sample past the cascade";
        }
    }
}

// The invariant that actually decides whether a fit step is visible: a receiver
// standing still in the world must keep sampling the SAME sub-texel phase of the
// shadow map while the camera translates. Phase drift is swim/shimmer; a change in
// the integer texel index alone is an invisible window slide, because the depth
// pass rasterized its casters through the same matrix and so shifted by the same
// whole number of texels.
//
// This is what the clip-space re-snap on the RELATIVE matrix buys: at
// render-origin-relative magnitude fp32 represents the texel grid exactly, so the
// grid stays anchored to the world even where a world-magnitude snap cannot
// resolve a texel.
//
// The perturbed arm is a self-check, not decoration: it offsets the projected
// position by half a texel on alternate frames and must read a ~0.5 phase drift.
// Without it, a measurement that silently saw nothing would report a perfect
// result. The offset is applied in double AFTER the projection, so the probe's
// sensitivity never depends on the magnitude of the matrix under test — folded
// into an fp32 world-magnitude translation instead (GE_ES_FORCE_WORLD_SPACE=1) it
// rounds away entirely, and the arm would then fail for a reason that has nothing
// to do with the invariant.
TEST(CascadeFitWorldMagnitude, StaticReceiverTexelPhaseIsTranslationInvariant)
{
    const Vector3 lightDir = kLightDir.Normalize();
    constexpr float kPlanetFar = 10000000.0f;
    constexpr float kClipTexel = 2.0f / static_cast<float>(kResolution);
    constexpr double kLadderStepMetres = 0.01;
    constexpr int kLadderSteps = 200;
    constexpr int kNumReceivers = 6;
    // fp32 slack in the projection itself; a real misalignment is O(0.1-0.5).
    constexpr double kMaxPhaseDrift = 0.02;
    constexpr double kMinPerturbedDrift = 0.4;

    uint32_t viewId = 3900;
    for (int arm = 0; arm < 2; ++arm)
    {
        const bool perturb = (arm == 1);
        for (const OriginMagnitudePose& pose : kOriginLadder)
        {
            ShadowMapRenderFeature feature;
            feature.SetFitFreezeEnabled(false);
            feature.ApplyRuntimeShadowSettings(200.0f, 0.5f, 0.0001f, 0.02f);
            const uint32_t vid = ++viewId;

            // Receivers fixed in WORLD space, taken from the ladder's first pose
            // and spread across the near cascades' slices.
            const Matrix4x4& rot = OverviewRotation();
            const float* rm = rot.Data();
            const double fwd[3] = {rm[2], rm[6], rm[10]};
            const double right[3] = {rm[0], rm[4], rm[8]};
            double receivers[kNumReceivers][3];
            const double depths[3] = {5.0, 12.0, 25.0};
            const double offsets[2] = {-6.0, 6.0};
            int n = 0;
            for (int d = 0; d < 3; ++d)
                for (int o = 0; o < 2; ++o)
                {
                    receivers[n][0] = pose.X + fwd[0] * depths[d] + right[0] * offsets[o];
                    receivers[n][1] = pose.Y + fwd[1] * depths[d] + right[1] * offsets[o];
                    receivers[n][2] = pose.Z + fwd[2] * depths[d] + right[2] * offsets[o];
                    ++n;
                }

            double maxDrift[Renderer::kMaxShadowCascades] = {};
            double basePhase[Renderer::kMaxShadowCascades][kNumReceivers][2] = {};
            bool haveBase = false;
            for (int s = 0; s < kLadderSteps; ++s)
            {
                const Rendering::CameraData cam =
                    MakeCameraAt(rot, pose.X + kLadderStepMetres * s, pose.Y, pose.Z, kNearPlane,
                                 kPlanetFar);
                const CascadeFrameData fd =
                    feature.ComputeCascades(cam, kNearPlane, kPlanetFar, lightDir, nullptr, vid);
                const double originWorld[3] = {
                    static_cast<double>(fd.RenderOriginSector[0]) * Renderer::kSectorSize,
                    static_cast<double>(fd.RenderOriginSector[1]) * Renderer::kSectorSize,
                    static_cast<double>(fd.RenderOriginSector[2]) * Renderer::kSectorSize};

                for (uint32_t c = 0; c < fd.NumCascades; ++c)
                {
                    const float* vp = fd.LightVPRel[c].Data();
                    const double offset =
                        (perturb && (s & 1)) ? 0.5 * static_cast<double>(kClipTexel) : 0.0;
                    for (int p = 0; p < n; ++p)
                    {
                        const float px = static_cast<float>(receivers[p][0] - originWorld[0]);
                        const float py = static_cast<float>(receivers[p][1] - originWorld[1]);
                        const float pz = static_cast<float>(receivers[p][2] - originWorld[2]);
                        const double cx = static_cast<double>(vp[0]) * px +
                                          static_cast<double>(vp[4]) * py +
                                          static_cast<double>(vp[8]) * pz + vp[12] + offset;
                        const double cy = static_cast<double>(vp[1]) * px +
                                          static_cast<double>(vp[5]) * py +
                                          static_cast<double>(vp[9]) * pz + vp[13];
                        const double u = (cx * 0.5 + 0.5) * static_cast<double>(kResolution);
                        const double v = (cy * 0.5 + 0.5) * static_cast<double>(kResolution);
                        const double phU = u - std::floor(u);
                        const double phV = v - std::floor(v);
                        if (!haveBase)
                        {
                            basePhase[c][p][0] = phU;
                            basePhase[c][p][1] = phV;
                            continue;
                        }
                        // Circular distance: phase 0.99 vs 0.01 is 0.02 apart.
                        double dU = std::fabs(phU - basePhase[c][p][0]);
                        dU = std::min(dU, 1.0 - dU);
                        double dV = std::fabs(phV - basePhase[c][p][1]);
                        dV = std::min(dV, 1.0 - dV);
                        maxDrift[c] = std::max(maxDrift[c], std::max(dU, dV));
                    }
                }
                haveBase = true;
            }
            for (uint32_t c = 0; c < Renderer::kMaxShadowCascades; ++c)
            {
                std::printf("[FitPhase] %s %-6s c%u maxPhaseDrift=%9.6f texel\n",
                            perturb ? "PERTURBED" : "         ", pose.Name, c, maxDrift[c]);
                if (perturb)
                {
                    EXPECT_GE(maxDrift[c], kMinPerturbedDrift)
                        << "pose " << pose.Name << " cascade " << c
                        << ": a half-texel translation offset did not register — this test cannot "
                           "see misalignment, so its clean arm proves nothing";
                }
                else
                {
                    EXPECT_LE(maxDrift[c], kMaxPhaseDrift)
                        << "pose " << pose.Name << " cascade " << c
                        << ": a static receiver's shadow texel phase drifts under camera "
                           "translation (shadow swim), not just its texel index";
                }
            }
        }
    }
}

// Cascade fit scene clamp. A low camera pitched down puts most of cascade 0's
// frustum slice BELOW the ground plane, so fitting the box to those slice
// corners centres it on empty space and the casters fall outside — cascades 0/1
// end up holding the ground and no occluders, and shadows cut along a hard
// horizontal line. ComputeClampedLightSpaceBounds clamps each slice corner into
// the scene's world bounds before the light-space accumulation.
//
// The clamp is world-space on purpose. Projected into light space the
// constraint vanishes: a 140 m ground spans ~134 m along the light direction
// while the slice spans ~20 m, so the slice sits wholly inside and a
// light-space intersection is a no-op. Verified numerically before this was
// written.
TEST(CascadeSceneClamp, ClampsSliceCornersIntoSceneBounds)
{
    // Identity rotation keeps light space == world space, so the expected
    // values are readable by inspection. Rotation is exercised separately below.
    Matrix4x4 identity = Matrix4x4::Identity();
    ShadowMapRenderFeature::SceneBoundsRel scene{Vector3{-10.0f, 0.0f, -10.0f},
                                                 Vector3{10.0f, 6.0f, 10.0f}};

    // A slice reaching well below the scene floor — the low-camera defect shape.
    Vector3 corners[8];
    for (int i = 0; i < 8; ++i)
        corners[i] = Vector3{(i & 1) ? 4.0f : -4.0f,
                             (i & 2) ? 2.0f : -30.0f,
                             (i & 4) ? 4.0f : -4.0f};

    Vector3 minLS{}, maxLS{};
    ShadowMapRenderFeature::ComputeClampedLightSpaceBounds(identity, corners, &scene, minLS, maxLS);

    // The floor rises to the scene floor; everything already inside is untouched.
    EXPECT_FLOAT_EQ(minLS.y, 0.0f);
    EXPECT_FLOAT_EQ(maxLS.y, 2.0f);
    EXPECT_FLOAT_EQ(minLS.x, -4.0f);
    EXPECT_FLOAT_EQ(maxLS.x, 4.0f);
}

// Null bounds must reproduce the plain corner accumulation this replaced, so
// callers without bounds are provably unaffected.
TEST(CascadeSceneClamp, NullBoundsMatchesPlainCornerAccumulation)
{
    Matrix4x4 identity = Matrix4x4::Identity();
    Vector3 corners[8];
    for (int i = 0; i < 8; ++i)
        corners[i] = Vector3{(i & 1) ? 4.0f : -4.0f,
                             (i & 2) ? 2.0f : -30.0f,
                             (i & 4) ? 4.0f : -4.0f};

    Vector3 minLS{}, maxLS{};
    ShadowMapRenderFeature::ComputeClampedLightSpaceBounds(identity, corners, nullptr, minLS, maxLS);

    // Unclamped: the -30 floor survives.
    EXPECT_FLOAT_EQ(minLS.y, -30.0f);
    EXPECT_FLOAT_EQ(maxLS.y, 2.0f);
}

// A degenerate scene box would clamp every corner onto a point. Treat it as
// "no bounds" rather than over-tightening.
TEST(CascadeSceneClamp, DegenerateSceneBoxSkipsTheClamp)
{
    Matrix4x4 identity = Matrix4x4::Identity();
    ShadowMapRenderFeature::SceneBoundsRel inverted{Vector3{10.0f, 10.0f, 10.0f},
                                                    Vector3{-10.0f, -10.0f, -10.0f}};

    Vector3 corners[8];
    for (int i = 0; i < 8; ++i)
        corners[i] = Vector3{(i & 1) ? 4.0f : -4.0f,
                             (i & 2) ? 2.0f : -30.0f,
                             (i & 4) ? 4.0f : -4.0f};

    Vector3 minLS{}, maxLS{};
    ShadowMapRenderFeature::ComputeClampedLightSpaceBounds(identity, corners, &inverted, minLS,
                                                          maxLS);

    EXPECT_FLOAT_EQ(minLS.y, -30.0f);
    EXPECT_FLOAT_EQ(maxLS.y, 2.0f);
}

// The clamp happens in world space, but the ACCUMULATION is in light space. A
// 90-degree Y rotation maps world +X onto light space Z, so a slice that is
// wide in X must produce a wide light-space Z. An implementation that forgot
// the rotation would report the extents on the wrong axes.
TEST(CascadeSceneClamp, BoundsAreAccumulatedInLightSpace)
{
    constexpr float kHalfPi = 1.5707963f;
    Matrix4x4 rotY = MakeRotationY(kHalfPi);

    Vector3 corners[8];
    for (int i = 0; i < 8; ++i)
        corners[i] = Vector3{(i & 1) ? 8.0f : -8.0f,
                             (i & 2) ? 1.0f : -1.0f,
                             (i & 4) ? 1.0f : -1.0f};

    Vector3 minLS{}, maxLS{};
    ShadowMapRenderFeature::ComputeClampedLightSpaceBounds(rotY, corners, nullptr, minLS, maxLS);

    EXPECT_NEAR(maxLS.z - minLS.z, 16.0f, 1e-3f);
    EXPECT_NEAR(maxLS.x - minLS.x, 2.0f, 1e-3f);
}

// The defect itself: a low camera pitched down puts most of cascade 0's slice
// below the ground, so the unclamped fit centres on empty space and the casters
// fall outside it. With scene bounds supplied the fit must track the geometry.
TEST(CascadeSceneClamp, LowCameraFitStaysWithinSceneBounds)
{
    ShadowMapRenderFeature feature;
    const Vector3 lightDir = kLightDir.Normalize();

    // Camera 0.5 m up, pitched down ~36 degrees — the reported repro shape.
    const Vector3 pos{-2.0f, 0.5f, -2.0f};
    const Vector3 fwd = Vector3{0.45f, -0.58f, 0.68f}.Normalize();
    const Rendering::CameraData cam = MakeCamera(pos, fwd);

    ShadowMapRenderFeature::SceneBoundsRel bounds{Vector3{-70.0f, 0.0f, -70.0f},
                                                  Vector3{70.0f, 6.0f, 70.0f}};

    // Distinct viewIds: extent hysteresis and frozen-fit state are per view, so
    // sharing one would let the first call's snapped extent leak into the second.
    const CascadeFrameData unclamped =
        feature.ComputeCascades(cam, kNearPlane, kFarPlane, lightDir, nullptr, 1, nullptr);
    const CascadeFrameData clamped =
        feature.ComputeCascades(cam, kNearPlane, kFarPlane, lightDir, nullptr, 2, &bounds);

    // The clamp must actually change cascade 0's fit at this pose. If it does
    // not, the test is not exercising the defect and proves nothing.
    EXPECT_NE(clamped.DepthSpan[0], unclamped.DepthSpan[0]);

    // And it must not collapse the cascade.
    EXPECT_GT(clamped.DepthSpan[0], 0.0f);
    EXPECT_GT(clamped.OrthoHalfExtent[0], 0.0f);
}

// The never-over-tighten guarantee at the ComputeCascades level: scene bounds so
// large they cannot bind any cascade must reproduce the unclamped fit
// bit-for-bit. This is the differential the clamp actually has to satisfy —
// comparing the null-bounds call against itself would pass no matter what the
// clamp does, since the pre-clamp fit no longer exists to compare against.
TEST(CascadeSceneClamp, NonBindingSceneBoundsReproduceTheUnclampedFit)
{
    ShadowMapRenderFeature feature;
    const Vector3 lightDir = kLightDir.Normalize();
    const Rendering::CameraData cam =
        MakeCamera(Vector3{0.0f, 2.0f, 0.0f}, Vector3{0.0f, 0.0f, 1.0f});

    // Far outside any cascade this camera produces, so every clamp is a no-op.
    constexpr float kNonBinding = 1.0e6f;
    ShadowMapRenderFeature::SceneBoundsRel nonBinding{};
    nonBinding.Min = Vector3{-kNonBinding, -kNonBinding, -kNonBinding};
    nonBinding.Max = Vector3{kNonBinding, kNonBinding, kNonBinding};

    const CascadeFrameData unclamped =
        feature.ComputeCascades(cam, kNearPlane, kFarPlane, lightDir, nullptr, 11, nullptr);
    const CascadeFrameData clamped =
        feature.ComputeCascades(cam, kNearPlane, kFarPlane, lightDir, nullptr, 11, &nonBinding);

    ASSERT_GT(unclamped.NumCascades, 0u);
    ASSERT_EQ(unclamped.NumCascades, clamped.NumCascades);
    for (uint32_t c = 0; c < unclamped.NumCascades; ++c)
    {
        EXPECT_EQ(std::memcmp(unclamped.LightVP[c].Data(), clamped.LightVP[c].Data(),
                              16 * sizeof(float)),
                  0)
            << "cascade " << c;
        EXPECT_FLOAT_EQ(unclamped.DepthSpan[c], clamped.DepthSpan[c]) << "cascade " << c;
        EXPECT_FLOAT_EQ(unclamped.OrthoHalfExtent[c], clamped.OrthoHalfExtent[c])
            << "cascade " << c;
    }
}

// The freeze trap. CanReuseFrozenCascadeFit recomputes the same light-space
// bounds and tests them against the frozen record, which was fitted from
// CLAMPED bounds. If the fit clamps and the reuse test does not, the test sees
// bounds outside the frozen box every frame and refits continuously — a
// coverage fix turned into a shimmer source, invisible in a screenshot.
//
// Two static calls CANNOT detect this: a refit with identical inputs produces an
// identical matrix, so reuse and refit look the same. The freeze only becomes
// observable under small camera motion, which is precisely what it exists to
// absorb — so drift the camera by well under a texel and require the fit to hold
// still. A reuse test that rejects every frame refits on each new pose and the
// matrices diverge.
//
// Fit freeze defaults ON (m_FitFreezeEnabled), so this is the default path.
TEST(CascadeSceneClamp, ClampedFitHoldsStillUnderSubTexelCameraDrift)
{
    ShadowMapRenderFeature feature;
    feature.SetFitFreezeEnabled(true);

    const Vector3 lightDir = kLightDir.Normalize();
    ShadowMapRenderFeature::SceneBoundsRel bounds{Vector3{-70.0f, 0.0f, -70.0f},
                                                  Vector3{70.0f, 6.0f, 70.0f}};
    const Vector3 fwd = Vector3{0.45f, -0.58f, 0.68f}.Normalize();

    // Seed the frozen record.
    const CascadeFrameData seed = feature.ComputeCascades(
        MakeCamera(Vector3{-2.0f, 0.5f, -2.0f}, fwd), kNearPlane, kFarPlane, lightDir, nullptr, 41,
        &bounds);

    // Drift a few centimetres per step — far inside the freeze's travel budget,
    // so a healthy freeze re-emits the seed fit byte-for-byte every time.
    uint32_t changes = 0;
    for (int step = 1; step <= 8; ++step)
    {
        const Vector3 pos{-2.0f + 0.01f * static_cast<float>(step), 0.5f,
                          -2.0f + 0.01f * static_cast<float>(step)};
        const CascadeFrameData fd = feature.ComputeCascades(MakeCamera(pos, fwd), kNearPlane,
                                                            kFarPlane, lightDir, nullptr, 41,
                                                            &bounds);
        if (std::memcmp(fd.LightVP[0].Data(), seed.LightVP[0].Data(), 16 * sizeof(float)) != 0)
            ++changes;
    }

    EXPECT_EQ(changes, 0u)
        << "cascade 0 refit on " << changes << " of 8 sub-texel camera steps — the reuse test is "
           "not applying the same clamp as the fit, so it rejects the frozen record every frame";
}

// ── Shimmer attribution ─────────────────────────────────────────────────────
//
// "The fit changed" is not a diagnosis. A whole-texel XY translation is HARMLESS
// — the shadow map lands on the same world texel lattice, so every receiver
// re-quantizes identically and edges hold still. What actually crawls is:
//
//   * a change in orthoHalfExtent, because worldPerTexel changes with it and
//     EVERY sample then reads a different world point (the classic
//     rotation-induced shimmer that a bounding-SPHERE fit exists to prevent);
//   * a sub-texel XY translation, because the lattice slides under the geometry.
//
// This decomposes the churn into those causes so a fix targets the dominant one
// rather than the aggregate. Reports numbers; asserts only the invariant that
// makes the numbers meaningful.

namespace
{

struct ChurnCauses
{
    uint32_t Changed = 0;
    uint32_t ExtentChanged = 0;
    uint32_t DepthChanged = 0;
    uint32_t TranslationOnly = 0;
    uint32_t SubTexelTranslation = 0; // translation-only frames that did NOT land on the lattice
    double MaxExtentJumpPct = 0.0;
};

template <typename PoseFn>
ChurnCauses MeasureChurnCauses(ShadowMapRenderFeature& feature, uint32_t cascade, uint32_t frames,
                               PoseFn&& poseAt)
{
    ChurnCauses out{};
    CascadeFrameData prev{};
    bool hasPrev = false;
    const Vector3 lightDir = kLightDir.Normalize();
    const float clipTexel = 2.0f / static_cast<float>(kResolution);

    for (uint32_t f = 0; f < frames; ++f)
    {
        Vector3 pos, fwd;
        poseAt(f, pos, fwd);
        const CascadeFrameData fd = feature.ComputeCascades(MakeCamera(pos, fwd), kNearPlane,
                                                           kFarPlane, lightDir, nullptr, 1);
        if (hasPrev && cascade < fd.NumCascades)
        {
            const bool matrixMoved = std::memcmp(fd.LightVP[cascade].Data(),
                                                 prev.LightVP[cascade].Data(),
                                                 16 * sizeof(float)) != 0;
            if (matrixMoved)
            {
                ++out.Changed;
                const bool extentMoved =
                    fd.OrthoHalfExtent[cascade] != prev.OrthoHalfExtent[cascade];
                const bool depthMoved = fd.DepthSpan[cascade] != prev.DepthSpan[cascade];
                if (extentMoved)
                {
                    ++out.ExtentChanged;
                    const double jump =
                        std::abs(fd.OrthoHalfExtent[cascade] - prev.OrthoHalfExtent[cascade])
                        / std::max(prev.OrthoHalfExtent[cascade], 1e-6f) * 100.0;
                    out.MaxExtentJumpPct = std::max(out.MaxExtentJumpPct, jump);
                }
                if (depthMoved)
                    ++out.DepthChanged;
                if (!extentMoved && !depthMoved)
                {
                    ++out.TranslationOnly;
                    // Did the clip translation land back on the texel lattice?
                    const float dx =
                        fd.LightVP[cascade].Data()[12] - prev.LightVP[cascade].Data()[12];
                    const float dy =
                        fd.LightVP[cascade].Data()[13] - prev.LightVP[cascade].Data()[13];
                    const double nx = static_cast<double>(dx) / clipTexel;
                    const double ny = static_cast<double>(dy) / clipTexel;
                    const double ex = std::abs(nx - std::round(nx));
                    const double ey = std::abs(ny - std::round(ny));
                    if (ex > 0.01 || ey > 0.01)
                        ++out.SubTexelTranslation;
                }
            }
        }
        prev = fd;
        hasPrev = true;
    }
    return out;
}

} // namespace

// Attribution, not a threshold. Prints the cause split for cascade 0 under each
// motion class with the fit freeze OFF (the raw fit) so the dominant mechanism is
// visible, and pins the one property that must hold regardless of which mechanism
// dominates: a translation-only step lands on the shadow texel lattice.
TEST(CascadeShimmerAttribution, ChurnIsDominatedByExtentChangesNotSubTexelTranslation)
{
    struct Motion
    {
        const char* Name;
        std::function<void(uint32_t, Vector3&, Vector3&)> Pose;
    };
    const Motion motions[] = {
        {"orbit 18deg/s",
         [](uint32_t f, Vector3& p, Vector3& d) {
             const float a = static_cast<float>(f) * 18.0f / 60.0f * kPi / 180.0f;
             p = Vector3{kOverviewPos.x + 30.0f * std::sin(a), kOverviewPos.y,
                         kOverviewPos.z + 30.0f * std::cos(a)};
             d = YawPitchForward(kOverviewYawDeg + static_cast<float>(f) * 0.3f, kOverviewPitchDeg);
         }},
        {"fly 8m/s",
         [](uint32_t f, Vector3& p, Vector3& d) {
             p = Vector3{kOverviewPos.x, kOverviewPos.y,
                         kOverviewPos.z + static_cast<float>(f) * 8.0f / 60.0f};
             d = YawPitchForward(kOverviewYawDeg, kOverviewPitchDeg);
         }},
        {"look 30deg/s",
         [](uint32_t f, Vector3& p, Vector3& d) {
             p = kOverviewPos;
             d = YawPitchForward(kOverviewYawDeg + static_cast<float>(f) * 30.0f / 60.0f,
                                 kOverviewPitchDeg);
         }},
    };

    for (bool freeze : {false, true})
    {
        for (const Motion& m : motions)
        {
            ShadowMapRenderFeature feature;
            feature.SetFitFreezeEnabled(freeze);
            const ChurnCauses c = MeasureChurnCauses(feature, 0, 600, m.Pose);
            std::printf("[shimmer cause] freeze %-3s %-14s cascade 0: changed %3u  extent %3u "
                        "(max jump %.1f%%)  depth %3u  translation-only %3u  of which sub-texel "
                        "%3u\n",
                        freeze ? "ON" : "OFF", m.Name, c.Changed, c.ExtentChanged,
                        c.MaxExtentJumpPct, c.DepthChanged, c.TranslationOnly,
                        c.SubTexelTranslation);

            EXPECT_EQ(c.SubTexelTranslation, 0u)
                << m.Name
                << ": a translation-only fit step left the shadow texel lattice. The "
                   "clip-translation snap is the one thing that must hold exactly — if it does "
                   "not, every receiver re-quantizes on a pure pan and no extent fix can help";
        }
    }
}

// Costs the classic fix before anyone pays for it. A bounding-SPHERE fit is
// rotation-invariant by construction, so worldPerTexel cannot move when the
// camera turns — but it covers more area than a tight AABB, which is why this
// engine replaced it. The comparison that decides the trade is NOT sphere vs raw
// AABB: it is sphere vs the AABB **as actually emitted**, i.e. after the
// power-of-two band snap rounds it UP. Band-snapping already pays part of the
// sphere's overhead while still not being invariant.
TEST(CascadeShimmerAttribution, SphereFitOverheadVersusTheEmittedAabbFit)
{
    ShadowMapRenderFeature feature;
    feature.SetFitFreezeEnabled(false);
    const Vector3 lightDir = kLightDir.Normalize();

    float minEmitted = std::numeric_limits<float>::max();
    float maxEmitted = 0.0f;
    double sumEmitted = 0.0;
    float minSphere = std::numeric_limits<float>::max();
    float maxSphere = 0.0f;
    double sumSphere = 0.0;
    uint32_t n = 0;

    for (uint32_t f = 0; f < 600; ++f)
    {
        const Vector3 pos = kOverviewPos;
        const Vector3 fwd =
            YawPitchForward(kOverviewYawDeg + static_cast<float>(f) * 30.0f / 60.0f,
                            kOverviewPitchDeg);
        const Rendering::CameraData cam = MakeCamera(pos, fwd);
        const CascadeFrameData fd =
            feature.ComputeCascades(cam, kNearPlane, kFarPlane, lightDir, nullptr, 1);
        ASSERT_GT(fd.NumCascades, 0u);

        // Cascade 0's slice corners, exactly as the cull path re-derives them.
        Matrix4x4 camVP;
        std::memcpy(camVP.Data(), cam.viewProj, 16 * sizeof(float));
        Vector3 corners[8];
        ShadowMapRenderFeature::ComputeExtendedSliceCornersWS(camVP, kNearPlane, kFarPlane,
                                                             kNearPlane, fd.SplitDistances[0],
                                                             corners);
        // Bounding sphere of the slice: centroid + max radius. Rotation-invariant
        // because the corner SET rotates rigidly with the camera.
        Vector3 c{0, 0, 0};
        for (const Vector3& p : corners)
            c = Vector3{c.x + p.x, c.y + p.y, c.z + p.z};
        c = Vector3{c.x / 8.0f, c.y / 8.0f, c.z / 8.0f};
        float r2 = 0.0f;
        for (const Vector3& p : corners)
        {
            const Vector3 d{p.x - c.x, p.y - c.y, p.z - c.z};
            r2 = std::max(r2, d.x * d.x + d.y * d.y + d.z * d.z);
        }
        const float sphere = std::sqrt(r2);
        const float emitted = fd.OrthoHalfExtent[0];

        minEmitted = std::min(minEmitted, emitted);
        maxEmitted = std::max(maxEmitted, emitted);
        sumEmitted += emitted;
        minSphere = std::min(minSphere, sphere);
        maxSphere = std::max(maxSphere, sphere);
        sumSphere += sphere;
        ++n;
    }

    const double meanEmitted = sumEmitted / n;
    const double meanSphere = sumSphere / n;
    std::printf("[sphere vs aabb] emitted halfExtent  min %.2f  mean %.2f  max %.2f\n", minEmitted,
                meanEmitted, maxEmitted);
    std::printf("[sphere vs aabb] slice sphere radius min %.2f  mean %.2f  max %.2f  "
                "(spread %.2f%%)\n",
                minSphere, meanSphere, maxSphere,
                (maxSphere - minSphere) / std::max(minSphere, 1e-6f) * 100.0);
    std::printf("[sphere vs aabb] sphere / emitted = %.3f  (>1 means the sphere costs "
                "resolution)\n",
                meanSphere / std::max(meanEmitted, 1e-6));

    // The load-bearing property: under PURE rotation the slice sphere is a rigid
    // body, so its radius must be constant. If this ever fails the sphere fit is
    // not the invariant it is advertised to be and the whole plan changes.
    EXPECT_LT((maxSphere - minSphere) / std::max(minSphere, 1e-6f), 0.01f)
        << "the slice bounding sphere is NOT rotation-invariant (min " << minSphere << ", max "
        << maxSphere << ") — a sphere fit would shimmer too";
}

// What actually sets the on-screen size of a shadow texel near the camera, at a
// close-up ground-level pose. worldPerTexel = 2*halfExtent/resolution, so this is
// the number that decides whether shadow edges show visible stair-steps — a
// separate axis from fit STABILITY, and the one that dominates when the fit is
// already stable. SplitLambda is the free knob: at 0.5 the splits are half
// uniform, so cascade 0 must cover most of the near field.
TEST(CascadeShimmerAttribution, SplitLambdaSetsTheNearFieldTexelSize)
{
    const Vector3 lightDir = kLightDir.Normalize();
    const Vector3 pos{-1.24f, 0.96f, -1.65f};
    const Vector3 fwd = YawPitchForward(87.8f, -29.6f);

    std::printf("[texel size] lambda  split0      halfExtent  worldPerTexel@2048  vs lambda 0.5\n");
    float baseline = 0.0f;
    for (float lambda : {0.5f, 0.7f, 0.85f, 0.95f})
    {
        ShadowMapRenderFeature feature;
        // Mirrors what a ShadowSettingsEffect volume would push at runtime.
        feature.ApplyRuntimeShadowSettings(100.0f, lambda, 0.0001f, 0.02f);
        const CascadeFrameData fd = feature.ComputeCascades(MakeCamera(pos, fwd), kNearPlane,
                                                           kFarPlane, lightDir, nullptr, 1);
        ASSERT_GT(fd.NumCascades, 0u);
        const float wpt = 2.0f * fd.OrthoHalfExtent[0] / 2048.0f;
        if (baseline == 0.0f)
            baseline = wpt;
        std::printf("[texel size] %5.2f   %7.3f m   %7.2f m   %8.2f mm         %.2fx finer\n",
                    lambda, fd.SplitDistances[0], fd.OrthoHalfExtent[0], wpt * 1000.0f,
                    baseline / wpt);
    }
}

// The contract of the two projection modes, stated as the difference that
// motivates having both. SDSM re-measures the visible depth range every frame,
// so if the splits follow it the cascade resizes under camera motion and
// worldPerTexel steps — the shimmer. Stable must be immune; Close must not be,
// or it is not doing the tightening it costs stability for.
TEST(CascadeShadowProjection, StableIgnoresSdsmWhileCloseFollowsIt)
{
    const Vector3 fwd = YawPitchForward(kOverviewYawDeg, kOverviewPitchDeg);
    const Rendering::CameraData cam = MakeCamera(kOverviewPos, fwd);
    const Vector3 lightDir = kLightDir.Normalize();

    ShadowMapRenderFeature::SDSMBounds wide{};
    wide.nearDepth = 0.5f;
    wide.farDepth = 90.0f;
    wide.valid = true;
    ShadowMapRenderFeature::SDSMBounds tight = wide;
    tight.farDepth = 20.0f; // the camera approaches a wall: a 4.5x contraction

    for (auto mode : {Renderer::ShadowProjection::Stable, Renderer::ShadowProjection::Close})
    {
        const bool stable = mode == Renderer::ShadowProjection::Stable;
        ShadowMapRenderFeature feature;
        feature.SetFitFreezeEnabled(false); // isolate the fit from the freeze
        feature.SetShadowProjection(mode);

        const CascadeFrameData a =
            feature.ComputeCascades(cam, kNearPlane, kFarPlane, lightDir, &wide, 61);
        const CascadeFrameData b =
            feature.ComputeCascades(cam, kNearPlane, kFarPlane, lightDir, &tight, 61);

        std::printf("[projection] %-6s  split0 %7.3f -> %7.3f   halfExtent %6.2f -> %6.2f\n",
                    stable ? "STABLE" : "CLOSE", a.SplitDistances[0], b.SplitDistances[0],
                    a.OrthoHalfExtent[0], b.OrthoHalfExtent[0]);

        if (stable)
        {
            EXPECT_FLOAT_EQ(a.SplitDistances[0], b.SplitDistances[0])
                << "Stable let SDSM move the split — the splits must depend only on "
                   "MaxShadowDistance and SplitLambda, or the cascade resizes as the camera moves";
            EXPECT_FLOAT_EQ(a.OrthoHalfExtent[0], b.OrthoHalfExtent[0])
                << "Stable let SDSM move the ortho extent, so worldPerTexel steps and every "
                   "shadow texel re-quantizes — that is the shimmer this mode exists to remove";
            EXPECT_EQ(std::memcmp(a.LightVP[0].Data(), b.LightVP[0].Data(), 16 * sizeof(float)), 0)
                << "Stable emitted a different cascade-0 matrix for the same camera";
        }
        else
        {
            EXPECT_LT(b.SplitDistances[0], a.SplitDistances[0])
                << "Close ignored an SDSM contraction — it pays stability for tightening it is "
                   "then not doing";
        }
    }
}

// Rotation invariance is the other half of Stable, and it is the half a bounding
// sphere buys: a rigid corner set has a fixed bounding radius, while its
// axis-aligned bounds breathe as the camera turns.
TEST(CascadeShadowProjection, StableHoldsExtentUnderPureRotationAndCloseDoesNot)
{
    const Vector3 lightDir = kLightDir.Normalize();
    for (auto mode : {Renderer::ShadowProjection::Stable, Renderer::ShadowProjection::Close})
    {
        const bool stable = mode == Renderer::ShadowProjection::Stable;
        ShadowMapRenderFeature feature;
        feature.SetFitFreezeEnabled(false);
        feature.SetShadowProjection(mode);

        std::set<float> extents;
        for (uint32_t f = 0; f < 360; ++f)
        {
            const Vector3 fwd = YawPitchForward(kOverviewYawDeg + static_cast<float>(f),
                                                kOverviewPitchDeg);
            const CascadeFrameData fd = feature.ComputeCascades(
                MakeCamera(kOverviewPos, fwd), kNearPlane, kFarPlane, lightDir, nullptr, 62);
            extents.insert(fd.OrthoHalfExtent[0]);
        }
        std::printf("[projection] %-6s  distinct cascade-0 half-extents over a 360 deg pan: %zu\n",
                    stable ? "STABLE" : "CLOSE", extents.size());
        if (stable)
        {
            EXPECT_EQ(extents.size(), 1u)
                << "Stable's extent moved under pure camera rotation — the bounding sphere of a "
                   "rigid corner set cannot change, so the extent is not coming from it";
        }
    }
}

// ── Caster inclusion ────────────────────────────────────────────────────────
//
// Under a directional light the caster of a receiver at P lies exactly on the
// ray P - t*L (t >= 0), so it shares P's light-space XY exactly. Two things
// follow, and the cascade cull must respect both:
//
//   * the four side planes can never legitimately reject a caster whose
//     receiver is inside the cascade — no lateral margin is ever needed;
//   * the near plane is therefore the only thing deciding which casters exist,
//     and it must not decide at all.
//
// ComputeCascadeLightVP sets nearZLS = minZSnapped - orthoHalfExtent * 1.5.
// That back-pull is measured in light-space Z, so the world height it reaches
// above a receiver is backExtension * sin(elevation), while a caster H above
// its receiver needs H / sin(elevation) of light-space Z. The requirement
// diverges as 1/sin(elevation) while the extension is constant, so the reach
// collapses as the sun drops. These tests pin the invariant rather than the
// constant, so they stay valid if the constant moves.

namespace
{

Vector3 LightDirFromElevationDeg(float elevationDeg)
{
    const float e = elevationDeg * kPi / 180.0f;
    return Vector3{0.0f, -std::sin(e), std::cos(e)}.Normalize();
}

// Mirrors Rendering::TestSphereFrustum including its 1.5x radius inflation, but
// reports WHICH plane rejected, so a failure names the mechanism instead of just
// saying "culled". Plane order is ExtractFrustumPlanes': 0..3 sides, 4 near,
// 5 far.
int FirstRejectingPlane(const Vector3& center, float radius, const Vector4* planes)
{
    const float inflatedRadius = radius * 1.5f;
    for (int i = 0; i < 6; ++i)
    {
        const Vector4& p = planes[i];
        const float dist = center.x * p.x + center.y * p.y + center.z * p.z + p.w;
        if (dist < -inflatedRadius)
            return i;
    }
    return -1;
}

struct CasterReach
{
    float Height = std::numeric_limits<float>::infinity(); // world metres above the receiver
    int Plane = -1;                                        // which plane evicted it
};

// Walks a caster sphere up the light ray through `receiver` and reports the
// first height at which the cascade's cull planes reject it. Infinite height
// with plane -1 means the caster survived the whole sweep.
CasterReach FindCasterReach(const Vector4* planes, const Vector3& receiver,
                            const Vector3& lightDir, float radius, float maxHeight)
{
    // Stepping t along -lightDir raises the caster by t * |lightDir.y|; a
    // horizontal light never gains height, so there is nothing to sweep.
    const float rise = std::abs(lightDir.y);
    if (rise < 1e-4f)
        return {};

    for (float h = 0.0f; h <= maxHeight; h += 0.05f)
    {
        const Vector3 caster = receiver - lightDir * (h / rise);
        const int rejected = FirstRejectingPlane(caster, radius, planes);
        if (rejected >= 0)
            return {h, rejected};
    }
    return {};
}

// Ground (y = 0) hit of the camera's forward ray — a receiver guaranteed to sit
// in the near cascade for a camera looking down at the floor.
Vector3 GroundHit(const Vector3& pos, const Vector3& forward)
{
    return pos + forward * (-pos.y / forward.y);
}

// The user's repro: a camera close to the ground looking down at a shadow.
const Vector3 kLowCameraPos{-2.0f, 0.5f, -2.0f};
const Vector3 kLowCameraFwd = Vector3{0.45f, -0.58f, 0.68f}.Normalize();
constexpr float kCasterRadius = 0.5f; // a ShadowStress sphere
constexpr float kSweepMaxHeight = 200.0f;

} // namespace

// The lateral half of the invariant, measured against the RAW cascade frustum —
// deliberately not the production plane set, because this is the claim that
// justifies the production set retiring plane 4 and nothing else. A caster
// directly up-light of an in-cascade receiver shares its light-space XY exactly,
// so no SIDE plane may reject it at any height or any sun elevation, and the
// near plane is therefore the sole gate. If this ever fails, retiring the near
// plane is not sufficient and the fix is incomplete.
TEST(CascadeCasterInclusion, OnlyTheNearPlaneEvictsCastersFromTheRawCascadeFrustum)
{
    for (float elevationDeg : {5.0f, 15.0f, 30.0f, 45.0f, 62.0f, 85.0f})
    {
        ShadowMapRenderFeature feature;
        const Vector3 lightDir = LightDirFromElevationDeg(elevationDeg);
        const CascadeFrameData fd = feature.ComputeCascades(
            MakeCamera(kLowCameraPos, kLowCameraFwd), kNearPlane, kFarPlane, lightDir, nullptr, 91);
        ASSERT_GT(fd.NumCascades, 0u);

        const Vector3 receiver = GroundHit(kLowCameraPos, kLowCameraFwd);
        Vector4 rawPlanes[6]{};
        Rendering::ExtractFrustumPlanes(fd.LightVP[0], rawPlanes);
        ASSERT_EQ(FirstRejectingPlane(receiver, kCasterRadius, rawPlanes), -1)
            << "receiver is not inside cascade 0 at elevation " << elevationDeg
            << " deg — the sweep below would be meaningless";

        const CasterReach reach =
            FindCasterReach(rawPlanes, receiver, lightDir, kCasterRadius, kSweepMaxHeight);
        ASSERT_GE(reach.Plane, 0)
            << "at elevation " << elevationDeg
            << " deg the raw frustum evicted nothing up to " << kSweepMaxHeight
            << " m — the sweep is not reaching the near plane, so this test proves nothing";
        EXPECT_EQ(reach.Plane, 4)
            << "at elevation " << elevationDeg << " deg a caster " << reach.Height
            << " m above its receiver was evicted by plane " << reach.Plane
            << " (0..3 = sides, 4 = near, 5 = far). A side plane rejecting a caster that shares "
               "its receiver's light-space XY contradicts the directional-light invariant, and "
               "means retiring the near plane alone cannot fix caster inclusion";
    }
}

// The defect. A caster standing on the light ray through a receiver that IS
// inside cascade 0 must remain a caster at any height — the near plane exists
// to bound the depth encode, not to decide which occluders exist. Today the
// near plane evicts it, and the height at which it does so collapses as the sun
// drops, because the back-pull is a constant multiple of the cascade's LATERAL
// size and blind to light elevation.
TEST(CascadeCasterInclusion, CastersAboveTheirReceiverSurviveAtEverySunElevation)
{
    for (float elevationDeg : {5.0f, 15.0f, 30.0f, 45.0f, 62.0f, 85.0f})
    {
        ShadowMapRenderFeature feature;
        const Vector3 lightDir = LightDirFromElevationDeg(elevationDeg);
        const CascadeFrameData fd = feature.ComputeCascades(
            MakeCamera(kLowCameraPos, kLowCameraFwd), kNearPlane, kFarPlane, lightDir, nullptr, 92);
        ASSERT_GT(fd.NumCascades, 0u);

        const Vector3 receiver = GroundHit(kLowCameraPos, kLowCameraFwd);
        // The production cull plane set — OnScheduleCulling builds its planes
        // through this and nothing else, so extracting them here instead would
        // measure a code path the engine does not run.
        Vector4 planes[6]{};
        ShadowMapRenderFeature::BuildCascadeCasterCullPlanes(fd.LightVP[0], planes);
        const CasterReach reach =
            FindCasterReach(planes, receiver, lightDir, kCasterRadius, kSweepMaxHeight);

        // Printed whether it passes or fails: the measured reach against the
        // reach the retired near plane would have imposed is the measurement this
        // test exists to produce.
        const float wouldHaveBeen =
            fd.OrthoHalfExtent[0] * 1.5f * std::sin(elevationDeg * kPi / 180.0f);
        std::printf("[caster reach] elevation %5.1f deg  halfExtent %7.2f m  reach %8.2f m  "
                    "(near plane would have capped it near %7.2f m)  evicted by plane %d\n",
                    elevationDeg, fd.OrthoHalfExtent[0], reach.Height, wouldHaveBeen, reach.Plane);

        EXPECT_EQ(reach.Plane, -1)
            << "at elevation " << elevationDeg << " deg the cascade-0 cull evicted a caster only "
            << reach.Height << " m above its receiver (plane " << reach.Plane
            << "); a directional light's caster may sit at any distance toward the light";
    }
}

// The cost side of retiring the near plane: it admits casters by construction,
// so measure how many rather than describing it. Sweeps the ShadowStress caster
// layout (14x14 sphere grid, 3.4 m spacing, the authored height cycle) against
// cascade 0's raw frustum and its production plane set, and prints both counts
// per sun elevation. Not a threshold — a recorded number, so a future change to
// the cull can be compared against it.
TEST(CascadeCasterInclusion, NearPlaneRetirementAdmitsAMeasuredNumberOfExtraCasters)
{
    static constexpr float kGridStep = 3.4f;
    static constexpr float kHeights[] = {0.6f, 3.15f, 5.7f, 2.3f, 4.85f, 1.45f, 4.0f};

    for (float elevationDeg : {5.0f, 15.0f, 62.0f})
    {
        ShadowMapRenderFeature feature;
        const Vector3 lightDir = LightDirFromElevationDeg(elevationDeg);
        const CascadeFrameData fd = feature.ComputeCascades(
            MakeCamera(kLowCameraPos, kLowCameraFwd), kNearPlane, kFarPlane, lightDir, nullptr, 93);
        ASSERT_GT(fd.NumCascades, 0u);

        Vector4 rawPlanes[6]{};
        Rendering::ExtractFrustumPlanes(fd.LightVP[0], rawPlanes);
        Vector4 cullPlanes[6]{};
        ShadowMapRenderFeature::BuildCascadeCasterCullPlanes(fd.LightVP[0], cullPlanes);

        uint32_t rawPass = 0;
        uint32_t cullPass = 0;
        uint32_t h = 0;
        for (int ix = 0; ix < 14; ++ix)
        {
            for (int iz = 0; iz < 14; ++iz)
            {
                const Vector3 c{-22.1f + kGridStep * static_cast<float>(ix),
                                kHeights[h % std::size(kHeights)],
                                -16.1f + kGridStep * static_cast<float>(iz)};
                ++h;
                if (FirstRejectingPlane(c, kCasterRadius, rawPlanes) < 0)
                    ++rawPass;
                if (FirstRejectingPlane(c, kCasterRadius, cullPlanes) < 0)
                    ++cullPass;
            }
        }

        std::printf("[cull growth] elevation %5.1f deg  cascade 0 casters: raw frustum %3u -> "
                    "near retired %3u  (+%u of 196)\n",
                    elevationDeg, rawPass, cullPass, cullPass - rawPass);

        // Retiring a plane can only ever admit more, never fewer.
        EXPECT_GE(cullPass, rawPass)
            << "retiring the near plane reduced the caster set at elevation " << elevationDeg
            << " deg — a plane that cannot reject must not remove anything";
    }
}
