// Tests for the TAA jitter primitives (Engine/Rendering/AntiAliasing.h) and
// the two-domain camera plumbing in ViewRegistry: Halton sequence values, the
// exact-NDC-offset property of ApplyNdcJitter under the LH reverse-Z
// projection (depth rows untouched, composition-distributive), and the
// per-frame rotation/freeze discipline of ResolveJitteredCameraData (one
// jitter sample per frame regardless of call count; the registry's stored
// CameraData stays byte-identical — the CascadeShadowCache key contract).

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>

#include "Engine/Rendering/AntiAliasing.h"
#include "Engine/Rendering/ViewRegistry.h"
#include "Mathematics/MatrixOps.h"

using GameEngine::Engine::Renderer::AntiAliasingMode;
using GameEngine::Engine::Renderer::ApplyNdcJitter;
using GameEngine::Engine::Renderer::HaltonSequence;
using GameEngine::Engine::Renderer::kFxaaJitterSequenceLength;
using GameEngine::Engine::Renderer::TemporalFxaaJitterOffset;
using GameEngine::Engine::Renderer::TemporalJitterOffset;
using GameEngine::Engine::Renderer::ViewRegistry;
using GameEngine::Mathematics::Matrix4x4;
using GameEngine::Rendering::CameraData;

namespace
{

constexpr float kPi = 3.14159265358979323846f;

Matrix4x4 TestProj()
{
    return GameEngine::Mathematics::MakePerspectiveLH_ZO_ReverseZ(kPi / 3.0f, 16.0f / 9.0f, 0.1f,
                                                                  1000.0f);
}

Matrix4x4 TestView()
{
    // An arbitrary rigid view: rotation about Y plus translation. Hand-rolled
    // to avoid depending on camera helpers.
    const float c = std::cos(0.37f);
    const float s = std::sin(0.37f);
    glm::mat4 m(1.0f);
    m[0][0] = c;
    m[0][2] = -s;
    m[2][0] = s;
    m[2][2] = c;
    m[3][0] = 1.5f;
    m[3][1] = -2.0f;
    m[3][2] = 7.25f;
    return Matrix4x4(m);
}

CameraData MakeCameraData(float tag);

} // namespace

TEST(TemporalAAJitter, HaltonSequenceKnownValues)
{
    // Radical inverse base 2: 1 -> 0.5, 2 -> 0.25, 3 -> 0.75, 4 -> 0.125.
    EXPECT_NEAR(HaltonSequence(1, 2), 0.5f, 1e-6f);
    EXPECT_NEAR(HaltonSequence(2, 2), 0.25f, 1e-6f);
    EXPECT_NEAR(HaltonSequence(3, 2), 0.75f, 1e-6f);
    EXPECT_NEAR(HaltonSequence(4, 2), 0.125f, 1e-6f);
    // Base 3: 1 -> 1/3, 2 -> 2/3, 3 -> 1/9, 4 -> 4/9.
    EXPECT_NEAR(HaltonSequence(1, 3), 1.0f / 3.0f, 1e-6f);
    EXPECT_NEAR(HaltonSequence(2, 3), 2.0f / 3.0f, 1e-6f);
    EXPECT_NEAR(HaltonSequence(3, 3), 1.0f / 9.0f, 1e-6f);
    EXPECT_NEAR(HaltonSequence(4, 3), 4.0f / 9.0f, 1e-6f);
}

TEST(TemporalAAJitter, OffsetsCenteredAndCyclic)
{
    // All offsets in [-0.5, 0.5), cycle repeats at sequenceLength, and the
    // 8-sample mean is near zero (low-discrepancy recentering).
    float sumX = 0.0f, sumY = 0.0f;
    for (uint32_t phase = 0; phase < 8; ++phase)
    {
        float jx = 0.0f, jy = 0.0f;
        TemporalJitterOffset(phase, 8, jx, jy);
        EXPECT_GE(jx, -0.5f);
        EXPECT_LT(jx, 0.5f);
        EXPECT_GE(jy, -0.5f);
        EXPECT_LT(jy, 0.5f);
        float jx2 = 0.0f, jy2 = 0.0f;
        TemporalJitterOffset(phase + 8, 8, jx2, jy2);
        EXPECT_EQ(jx, jx2);
        EXPECT_EQ(jy, jy2);
        sumX += jx;
        sumY += jy;
    }
    EXPECT_NEAR(sumX / 8.0f, 0.0f, 0.08f);
    EXPECT_NEAR(sumY / 8.0f, 0.0f, 0.08f);
}

// Decima's pattern: two phases, one axis each, half a pixel, repeating. The
// property that matters is the last block: only two distinct positions exist,
// so the 50/50 two-frame blend sees the same pair every frame and a static
// scene is bit-stable — a four-position rotation cycled through four blends.
TEST(TemporalAAJitter, FxaaOffsetsAlternateAxesAtHalfAPixel)
{
    constexpr float kAmp = GameEngine::Engine::Renderer::kFxaaEdgeJitterTexels;
    static_assert(kFxaaJitterSequenceLength == 2u,
                  "a two-frame 50/50 blend is only stable with exactly two positions");
    for (uint32_t phase = 0; phase < 8; ++phase)
    {
        float x = 0.0f;
        float y = 0.0f;
        TemporalFxaaJitterOffset(phase, x, y);
        const bool odd = (phase & 1u) != 0u;
        EXPECT_FLOAT_EQ(x, odd ? kAmp : 0.0f) << "phase " << phase;
        EXPECT_FLOAT_EQ(y, odd ? 0.0f : kAmp) << "phase " << phase;

        float repeatedX = 0.0f;
        float repeatedY = 0.0f;
        TemporalFxaaJitterOffset(phase + kFxaaJitterSequenceLength, repeatedX, repeatedY);
        EXPECT_FLOAT_EQ(repeatedX, x);
        EXPECT_FLOAT_EQ(repeatedY, y);
    }
}

TEST(TemporalAAJitter, RegistrySelectsTemporalFxaaModeAndTwoPhaseCycle)
{
    ViewRegistry reg;
    const auto cameraId = reg.AllocateCamera("tfxaa-camera");
    const auto viewId = reg.AllocateView("tfxaa-view", cameraId);
    reg.SetCameraData(cameraId, MakeCameraData(1.0f));
    reg.SetViewAntiAliasing(viewId, true, AntiAliasingMode::TemporalFXAA, 16u);

    const auto* state = reg.AdvanceViewAntiAliasing(viewId, 1u);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->Mode, AntiAliasingMode::TemporalFXAA);
    EXPECT_EQ(state->SequenceLength, kFxaaJitterSequenceLength);
}

// The single-frame FXAA registers view state (its node gates on the mode) but
// must never move the raster: like SMAA it is a spatial filter, and a jittered
// input would ship the oscillation the two-frame resolve exists to cancel.
TEST(TemporalAAJitter, SpatialFxaaRegistersStateButNeverJitters)
{
    ViewRegistry reg;
    const auto cameraId = reg.AllocateCamera("fxaa-camera");
    const auto viewId = reg.AllocateView("fxaa-view", cameraId);
    reg.SetCameraData(cameraId, MakeCameraData(1.0f));
    reg.SetViewAntiAliasing(viewId, true, AntiAliasingMode::FXAA, 16u);

    for (uint64_t frame = 1; frame <= 4; ++frame)
    {
        const auto* state = reg.AdvanceViewAntiAliasing(viewId, frame);
        ASSERT_NE(state, nullptr) << "frame " << frame;
        EXPECT_EQ(state->Mode, AntiAliasingMode::FXAA);
        EXPECT_EQ(state->SequenceLength, 1u);
        EXPECT_EQ(state->JitterX, 0.0f) << "frame " << frame;
        EXPECT_EQ(state->JitterY, 0.0f) << "frame " << frame;
    }
    EXPECT_FALSE(IsJitteredAntiAliasingMode(AntiAliasingMode::FXAA));
    EXPECT_TRUE(IsJitteredAntiAliasingMode(AntiAliasingMode::TemporalFXAA));
}

TEST(TemporalAAJitter, ApplyNdcJitterWritesTheFreeZeros)
{
    // On the bare reverse-Z projection the row op reduces to writing the free
    // zeros m[2][0]/m[2][1]; every other element is bit-identical.
    const Matrix4x4 proj = TestProj();
    float jittered[16];
    std::memcpy(jittered, proj.Data(), sizeof(jittered));
    const float dx = 0.375f / 1920.0f;
    const float dy = -0.25f / 1080.0f;
    ApplyNdcJitter(jittered, dx, dy);

    for (int i = 0; i < 16; ++i)
    {
        if (i == 8) // m[2][0]
            EXPECT_NEAR(jittered[i], proj.Data()[i] + dx, 1e-9f);
        else if (i == 9) // m[2][1]
            EXPECT_NEAR(jittered[i], proj.Data()[i] + dy, 1e-9f);
        else
            EXPECT_EQ(jittered[i], proj.Data()[i]) << "element " << i << " must be untouched";
    }
}

TEST(TemporalAAJitter, ExactNdcOffsetAtEveryDepth)
{
    // Because w_clip = z_view, the jitter is an exact constant NDC translation
    // at every depth, and NDC depth is bit-identical (reverse-Z untouched).
    const Matrix4x4 proj = TestProj();
    Matrix4x4 jittered = proj;
    const float dx = 2.0f * 0.4f / 1920.0f;
    const float dy = 2.0f * -0.3f / 1080.0f;
    ApplyNdcJitter(jittered.Data(), dx, dy);

    const GameEngine::Mathematics::Vector4 points[] = {
        {0.13f, -0.62f, 0.11f, 1.0f}, // just past near
        {3.7f, 2.1f, 42.0f, 1.0f},    // mid
        {-40.0f, 13.0f, 950.0f, 1.0f} // near far
    };
    for (const auto& p : points)
    {
        const auto clipBase = proj.Transform(p);
        const auto clipJit = jittered.Transform(p);
        ASSERT_GT(clipBase.w, 0.0f);
        EXPECT_NEAR(clipJit.x / clipJit.w - clipBase.x / clipBase.w, dx, 1e-6f);
        EXPECT_NEAR(clipJit.y / clipJit.w - clipBase.y / clipBase.w, dy, 1e-6f);
        EXPECT_EQ(clipJit.z, clipBase.z); // depth row untouched -> bit-identical
        EXPECT_EQ(clipJit.w, clipBase.w);
    }
}

TEST(TemporalAAJitter, RowOpDistributesOverComposition)
{
    // Applying the row op to viewProj must equal composing the jittered proj
    // with the view — the property that lets the seams jitter composed
    // matrices without re-multiplying.
    const Matrix4x4 proj = TestProj();
    const Matrix4x4 view = TestView();
    const float dx = 0.0007f, dy = -0.0004f;

    Matrix4x4 viewProj = proj * view;
    ApplyNdcJitter(viewProj.Data(), dx, dy);

    Matrix4x4 projJ = proj;
    ApplyNdcJitter(projJ.Data(), dx, dy);
    const Matrix4x4 expected = projJ * view;

    for (int i = 0; i < 16; ++i)
        EXPECT_NEAR(viewProj.Data()[i], expected.Data()[i], 1e-5f) << "element " << i;
}

namespace
{

CameraData MakeCameraData(float tag)
{
    CameraData cam{};
    const Matrix4x4 view = TestView();
    const Matrix4x4 proj = TestProj();
    const Matrix4x4 viewProj = proj * view;
    std::memcpy(cam.view, view.Data(), sizeof(cam.view));
    std::memcpy(cam.proj, proj.Data(), sizeof(cam.proj));
    std::memcpy(cam.viewProj, viewProj.Data(), sizeof(cam.viewProj));
    cam.cameraPos[0] = tag; // distinguishes frames in the snapshot assertions
    return cam;
}

} // namespace

TEST(TemporalAAJitter, RegistryRotatesOncePerFrameAndKeepsLogicDomainClean)
{
    ViewRegistry reg;
    const auto camId = reg.AllocateCamera("cam");
    const auto viewId = reg.AllocateView("view", camId);
    reg.SetCameraData(camId, MakeCameraData(1.0f));
    reg.SetViewAntiAliasing(viewId, true, AntiAliasingMode::TAA, 8);

    // Two resolves in the same frame must produce identical matrices (one
    // jitter sample per frame, not per call).
    const CameraData a = reg.ResolveJitteredCameraData(viewId, 10, 1920, 1080);
    const CameraData b = reg.ResolveJitteredCameraData(viewId, 10, 1920, 1080);
    EXPECT_EQ(0, std::memcmp(&a, &b, sizeof(CameraData)));

    // The jittered resolve differs from the logic domain...
    const CameraData logic = reg.ResolveCameraData(viewId);
    EXPECT_NE(0, std::memcmp(a.proj, logic.proj, sizeof(a.proj)));
    // ...but the logic domain itself is untouched by jittered resolves — the
    // stored viewProj is what CascadeShadowCache memcmp-keys on.
    const CameraData logicAgain = reg.ResolveCameraData(viewId);
    EXPECT_EQ(0, std::memcmp(&logic, &logicAgain, sizeof(CameraData)));

    // Next frame rotates to a different jitter phase.
    const CameraData c = reg.ResolveJitteredCameraData(viewId, 11, 1920, 1080);
    EXPECT_NE(0, std::memcmp(a.proj, c.proj, sizeof(a.proj)));

    // Depth rows never change: proj cols 2/3 rows z/w equal the logic domain.
    EXPECT_EQ(a.proj[10], logic.proj[10]);
    EXPECT_EQ(a.proj[11], logic.proj[11]);
    EXPECT_EQ(a.proj[14], logic.proj[14]);
    EXPECT_EQ(a.proj[15], logic.proj[15]);
}

TEST(TemporalAAJitter, RegistrySnapshotsPrevCameraAcrossFrames)
{
    ViewRegistry reg;
    const auto camId = reg.AllocateCamera("cam");
    const auto viewId = reg.AllocateView("view", camId);
    reg.SetViewAntiAliasing(viewId, true, AntiAliasingMode::TAA, 8);

    reg.SetCameraData(camId, MakeCameraData(1.0f));
    (void)reg.ResolveJitteredCameraData(viewId, 100, 1920, 1080);
    const auto* s1 = reg.FindViewAntiAliasing(viewId);
    ASSERT_NE(s1, nullptr);
    EXPECT_FALSE(s1->PrevValid); // first rendered frame: no previous camera

    reg.SetCameraData(camId, MakeCameraData(2.0f));
    (void)reg.ResolveJitteredCameraData(viewId, 101, 1920, 1080);
    const auto* s2 = reg.FindViewAntiAliasing(viewId);
    ASSERT_NE(s2, nullptr);
    EXPECT_TRUE(s2->PrevValid); // consecutive frames
    EXPECT_EQ(s2->PrevCamera.cameraPos[0], 1.0f);
    EXPECT_EQ(s2->CurrCamera.cameraPos[0], 2.0f);

    // A skipped frame (OnDemand lapse, hidden panel) does NOT invalidate the
    // pair: Prev* keep the last rendered frame's camera — exactly what
    // reprojecting the surviving history needs. Whether the history physical
    // survived the gap is the render-graph pool's freshness arm, not this
    // pair's concern.
    reg.SetCameraData(camId, MakeCameraData(3.0f));
    (void)reg.ResolveJitteredCameraData(viewId, 103, 1920, 1080);
    const auto* s3 = reg.FindViewAntiAliasing(viewId);
    ASSERT_NE(s3, nullptr);
    EXPECT_TRUE(s3->PrevValid);
    EXPECT_EQ(s3->PrevCamera.cameraPos[0], 2.0f) << "last rendered frame's camera";

    // Disabling drops the state; resolve falls back to the logic domain.
    reg.SetViewAntiAliasing(viewId, false, AntiAliasingMode::TAA, 8);
    EXPECT_EQ(reg.FindViewAntiAliasing(viewId), nullptr);
    const CameraData off = reg.ResolveJitteredCameraData(viewId, 104, 1920, 1080);
    const CameraData logic = reg.ResolveCameraData(viewId);
    EXPECT_EQ(0, std::memcmp(&off, &logic, sizeof(CameraData)));
}

TEST(TemporalAAJitter, NdcOffsetsFreezeAtFirstResolveOfFrame)
{
    // Seams can disagree about extent sources (letterbox sub-rect vs texture
    // size); the first resolve of a frame freezes the NDC offsets and later
    // seams must reuse them verbatim — matching clip positions across the
    // depth prepass and world pass is what makes GreaterOrEqual depth testing
    // survive jitter.
    ViewRegistry reg;
    const auto camId = reg.AllocateCamera("cam");
    const auto viewId = reg.AllocateView("view", camId);
    reg.SetCameraData(camId, MakeCameraData(1.0f));
    reg.SetViewAntiAliasing(viewId, true, AntiAliasingMode::TAA, 8);

    const CameraData a = reg.ResolveJitteredCameraData(viewId, 7, 1920, 1080);
    const CameraData b = reg.ResolveJitteredCameraData(viewId, 7, 1280, 720);
    EXPECT_EQ(0, std::memcmp(&a, &b, sizeof(CameraData)));
}
