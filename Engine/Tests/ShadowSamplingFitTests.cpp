// ShadowSamplingFitTests — the sampling-layer selection BuildShadowDataGPU
// uploads per cascade (ShadowMapRenderFeature::SelectCascadeSamplingFit).
// Locks the cascade motion round-robin's flip precondition:
//   * a motion-deferred cascade's receivers transform by the RETAINED layer's
//     own origin-relative fit (CascadeContentFit::LightVPRel) — never the
//     current frame's fit (which would swim off the retained texels), and
//     never a re-derivation from the world-space LightVP (which would
//     re-inherit ULP(|origin|) storage rounding at planetary magnitude — the
//     #660 shadow follow-up);
//   * a sector step during the deferral window re-anchors the retained fit
//     via the exact integer-sector delta (RebaseTranslationColumnBySectorDelta);
//   * a current-content snapshot reproduces frame.LightVPRel bit-for-bit —
//     the cap-0 / always-render dark-ship guarantee;
//   * an absent or !Valid snapshot falls back to the current frame's
//     LightVPRel + metrics (first frames, post-invalidate, standalone calls).
// Pure CPU: no device, no RenderServices — the selection is a static helper
// precisely because BuildShadowDataGPU itself needs an initialized
// RenderServices (Textures()/bindless) for its quality phases.

#include "Engine/Rendering/ShadowMapRenderFeature.h"

#include "Components/Rendering/Light.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>

using namespace GameEngine;
using namespace GameEngine::Mathematics;
namespace Renderer = GameEngine::Engine::Renderer;

using Renderer::CascadeFrameData;
using Renderer::ShadowMapRenderFeature;

namespace
{

// A cascade-shaped fit pair, derived exactly the way ComputeCascadeLightVP
// derives it with the origin active: rotation-only look-at + explicit -R*eye
// translation (never MakeLookAtLH(eye, eye+dir)), reverse-Z ortho, and the
// origin-relative twin through the production ComputeRebasedOrthoLightVP
// primitive (sector 0 => byte-copy).
void MakeCascadeLightVPPair(const Vector3& lightPos, const Vector3& lightDir, float halfExtent,
                            float depthRange, const int32_t sector[3], Matrix4x4& outVP,
                            Matrix4x4& outVPRel)
{
    Vector3 ld = lightDir;
    ld = ld.Normalize();
    const Vector3 up = std::fabs(Vector3::Dot(ld, Vector3{0, 1, 0})) > 0.99f ? Vector3{0, 0, 1}
                                                                             : Vector3{0, 1, 0};
    const Matrix4x4 rotView = MakeLookAtLH(Vector3{0, 0, 0}, ld, up);
    const Matrix4x4 proj = MakeOrthographicLH_ZO_ReverseZ(-halfExtent, halfExtent, -halfExtent,
                                                          halfExtent, 0.0f, depthRange);
    Matrix4x4 view = rotView;
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
    outVP = proj * view;
    outVPRel = outVP;
    Renderer::ComputeRebasedOrthoLightVP(rotView.Data(), proj.Data(), outVP.Data(), lightPos.x,
                                         lightPos.y, lightPos.z, sector, outVPRel.Data());
}

void OriginWorld(int32_t sx, int32_t sy, int32_t sz, float out[3])
{
    Renderer::SectorToWorld(Components::WorldSectorCoord{sx, sy, sz}, out[0], out[1], out[2]);
}

// A frame whose per-cascade fits are all distinct (guards cascade-index
// mixups), with LightVP/LightVPRel derived through the production fit shape.
CascadeFrameData MakeFrame(int32_t sx, int32_t sy, int32_t sz)
{
    CascadeFrameData f{};
    f.NumCascades = Renderer::kMaxShadowCascades;
    f.RenderOriginSector[0] = sx;
    f.RenderOriginSector[1] = sy;
    f.RenderOriginSector[2] = sz;
    float o[3];
    OriginWorld(sx, sy, sz, o);
    for (uint32_t i = 0; i < f.NumCascades; ++i)
    {
        const float half = 20.0f * static_cast<float>(i + 1);
        const float span = 150.0f + 40.0f * static_cast<float>(i);
        const float k = static_cast<float>(i);
        MakeCascadeLightVPPair(Vector3{o[0] + 3.0f * k, o[1] + 100.0f, o[2] - 2.0f * k},
                               Vector3{-0.4f, -1.0f, -0.3f}, half, span, f.RenderOriginSector,
                               f.LightVP[i], f.LightVPRel[i]);
        f.OrthoHalfExtent[i] = half;
        f.DepthSpan[i] = span;
        f.SplitDistances[i] = 10.0f * static_cast<float>(i + 1);
    }
    return f;
}

// A divergent snapshot: the fit of content rendered a few frames ago from a
// different camera — different translation AND different extents — committed
// under `sector`.
ShadowMapRenderFeature::CascadeContentFit MakeDivergentFit(const float originWorld[3],
                                                           const int32_t sector[3])
{
    ShadowMapRenderFeature::CascadeContentFit fit{};
    MakeCascadeLightVPPair(
        Vector3{originWorld[0] + 27.0f, originWorld[1] + 90.0f, originWorld[2] + 11.0f},
        Vector3{-0.5f, -1.0f, -0.2f}, 55.0f, 240.0f, sector, fit.LightVP, fit.LightVPRel);
    fit.Sector[0] = sector[0];
    fit.Sector[1] = sector[1];
    fit.Sector[2] = sector[2];
    fit.OrthoHalfExtent = 55.0f;
    fit.DepthSpan = 240.0f;
    fit.Valid = true;
    return fit;
}

} // namespace

TEST(ShadowSamplingFit, DeferredCascadeSamplesRetainedRelFitNotCurrentFit)
{
    // The uploaded VP must be the snapshot's OWN rel fit — the matrix that
    // rasterized the retained layer — bit-for-bit while the sector is
    // unchanged, and NOT the current frame's fit.
    const int32_t sx = 5, sy = 0, sz = -2;
    const CascadeFrameData frame = MakeFrame(sx, sy, sz);
    float o[3];
    OriginWorld(sx, sy, sz, o);

    const uint32_t c = 2; // per-cascade values are distinct; pick a middle slot
    const ShadowMapRenderFeature::CascadeContentFit fit =
        MakeDivergentFit(o, frame.RenderOriginSector);

    const float resolution = 2048.0f;
    float outVP[16];
    float worldPerTexel = -1.0f, depthSpan = -1.0f;
    ShadowMapRenderFeature::SelectCascadeSamplingFit(&fit, frame, c, resolution, outVP,
                                                     worldPerTexel, depthSpan);

    EXPECT_EQ(0, std::memcmp(outVP, fit.LightVPRel.Data(), 16 * sizeof(float)));
    // ...and NOT the current frame's fit: receivers must key into the retained
    // layer's texels instead of swimming with the fresh fit.
    EXPECT_NE(0, std::memcmp(outVP, frame.LightVPRel[c].Data(), 16 * sizeof(float)));

    // PCSS metrics ride the snapshot for the same reason the matrix does.
    EXPECT_EQ(worldPerTexel, (2.0f * fit.OrthoHalfExtent) / resolution);
    EXPECT_EQ(depthSpan, fit.DepthSpan);
}

TEST(ShadowSamplingFit, DeferredCascadeReanchorsAcrossSectorStep)
{
    // A render-origin sector step DURING the deferral window: the retained
    // layer's rel fit was committed under the OLD sector; receivers now carry
    // positions relative to the NEW one. The selection re-anchors the snapshot
    // via RebaseTranslationColumnBySectorDelta — same primitive, same inputs
    // => same bits — never re-deriving from the world-space LightVP.
    const int32_t sx = 6222, sy = 0, sz = 6222; // Earth-radius shell sector
    const CascadeFrameData frame = MakeFrame(sx, sy, sz);

    // Content committed one sector step ago.
    const int32_t oldSector[3] = {sx - 1, sy, sz + 2};
    float oldOrigin[3];
    OriginWorld(oldSector[0], oldSector[1], oldSector[2], oldOrigin);
    const ShadowMapRenderFeature::CascadeContentFit fit = MakeDivergentFit(oldOrigin, oldSector);

    const uint32_t c = 1;
    const float resolution = 2048.0f;
    float outVP[16];
    float worldPerTexel = -1.0f, depthSpan = -1.0f;
    ShadowMapRenderFeature::SelectCascadeSamplingFit(&fit, frame, c, resolution, outVP,
                                                     worldPerTexel, depthSpan);

    Matrix4x4 expected = fit.LightVPRel;
    Renderer::RebaseTranslationColumnBySectorDelta(fit.LightVPRel.Data(), fit.Sector,
                                                   frame.RenderOriginSector, expected.Data());
    EXPECT_EQ(0, std::memcmp(outVP, expected.Data(), 16 * sizeof(float)));
    // The re-anchor is not a no-op (the sectors differ) and still isn't the
    // current frame's fit.
    EXPECT_NE(0, std::memcmp(outVP, fit.LightVPRel.Data(), 16 * sizeof(float)));
    EXPECT_NE(0, std::memcmp(outVP, frame.LightVPRel[c].Data(), 16 * sizeof(float)));
}

TEST(ShadowSamplingFit, CurrentContentReproducesFrameLightVPRelBitForBit)
{
    // Rendered-this-frame / cache-skipped: the snapshot IS the current fit
    // (same LightVPRel, same sector), so the selection uploads it bit-for-bit
    // — the cap-0 always-render path is unchanged (dark-ship). Checked with
    // the origin inactive (sector 0) and active.
    const int32_t sectors[2][3] = {{0, 0, 0}, {5, 0, -2}};
    for (const auto& s : sectors)
    {
        const CascadeFrameData frame = MakeFrame(s[0], s[1], s[2]);
        const float resolution = 2048.0f;
        for (uint32_t c = 0; c < frame.NumCascades; ++c)
        {
            ShadowMapRenderFeature::CascadeContentFit fit{};
            fit.LightVP = frame.LightVP[c];
            fit.LightVPRel = frame.LightVPRel[c];
            fit.Sector[0] = frame.RenderOriginSector[0];
            fit.Sector[1] = frame.RenderOriginSector[1];
            fit.Sector[2] = frame.RenderOriginSector[2];
            fit.OrthoHalfExtent = frame.OrthoHalfExtent[c];
            fit.DepthSpan = frame.DepthSpan[c];
            fit.Valid = true;

            float outVP[16];
            float worldPerTexel = -1.0f, depthSpan = -1.0f;
            ShadowMapRenderFeature::SelectCascadeSamplingFit(&fit, frame, c, resolution, outVP,
                                                             worldPerTexel, depthSpan);
            EXPECT_EQ(0, std::memcmp(outVP, frame.LightVPRel[c].Data(), 16 * sizeof(float)))
                << "sector (" << s[0] << "," << s[1] << "," << s[2] << ") cascade " << c;
            EXPECT_EQ(worldPerTexel, (2.0f * frame.OrthoHalfExtent[c]) / resolution);
            EXPECT_EQ(depthSpan, frame.DepthSpan[c]);
        }
    }
}

TEST(ShadowSamplingFit, NoSnapshotFallsBackToCurrentFrame)
{
    // Absent (null) or !Valid snapshot: the current frame's LightVPRel +
    // metrics upload unchanged. The !Valid case carries junk fit fields
    // (including a junk sector) to prove Valid gates the selection, not the
    // field contents.
    const int32_t sx = 5, sy = 0, sz = -2;
    const CascadeFrameData frame = MakeFrame(sx, sy, sz);
    float o[3];
    OriginWorld(sx, sy, sz, o);

    ShadowMapRenderFeature::CascadeContentFit invalid{};
    const int32_t junkSector[3] = {sx + 40, sy - 7, sz + 13};
    MakeCascadeLightVPPair(Vector3{o[0] + 500.0f, o[1] + 40.0f, o[2]},
                           Vector3{-0.1f, -1.0f, -0.7f}, 999.0f, 5.0f, junkSector,
                           invalid.LightVP, invalid.LightVPRel);
    invalid.Sector[0] = junkSector[0];
    invalid.Sector[1] = junkSector[1];
    invalid.Sector[2] = junkSector[2];
    invalid.OrthoHalfExtent = 999.0f;
    invalid.DepthSpan = 5.0f;
    invalid.Valid = false;

    const ShadowMapRenderFeature::CascadeContentFit* fits[2] = {nullptr, &invalid};
    const float resolution = 2048.0f;
    for (const auto* fit : fits)
    {
        for (uint32_t c = 0; c < frame.NumCascades; ++c)
        {
            float outVP[16];
            float worldPerTexel = -1.0f, depthSpan = -1.0f;
            ShadowMapRenderFeature::SelectCascadeSamplingFit(fit, frame, c, resolution, outVP,
                                                             worldPerTexel, depthSpan);
            EXPECT_EQ(0, std::memcmp(outVP, frame.LightVPRel[c].Data(), 16 * sizeof(float)))
                << (fit ? "!Valid snapshot" : "null snapshot") << " cascade " << c;
            EXPECT_EQ(worldPerTexel, (2.0f * frame.OrthoHalfExtent[c]) / resolution);
            EXPECT_EQ(depthSpan, frame.DepthSpan[c]);
        }
    }
}

TEST(ShadowSamplingFit, ProjectResolutionOverrideIsOwnedAndNormalizedByShadowFeature)
{
    ShadowMapRenderFeature feature;

    EXPECT_FALSE(feature.GetProjectResolutionOverride().has_value());
    feature.SetProjectResolutionOverride(5000u);

    ASSERT_TRUE(feature.GetProjectResolutionOverride().has_value());
    EXPECT_EQ(*feature.GetProjectResolutionOverride(), 4096u);
    EXPECT_EQ(feature.GetConfig().Resolution, 4096u);
}

// Penumbra width is driven by the light's angular size, not by a world-space
// scalar: a directional light sits at infinity, so its penumbra is
// depthDelta * tan(halfAngle) with no divide by blocker distance. These pin the
// degrees -> tangent resolution that makes the authored unit physical.
TEST(PhysicalPenumbra, AngularDiameterResolvesToTangent)
{
    // Solar disc: 0.53 degrees FULL diameter -> tan of the HALF angle.
    const float expectedSun = std::tan(0.53f * 0.5f * 3.14159265358979f / 180.0f);
    EXPECT_NEAR(Renderer::ResolveShadowTanHalfAngle(0.53f), expectedSun, 1e-7f);

    // A perfectly collimated light is legal and must resolve to exactly zero —
    // the shader floors the kernel at one texel, it never divides by this.
    EXPECT_FLOAT_EQ(Renderer::ResolveShadowTanHalfAngle(0.0f), 0.0f);

    // A negative authored diameter is meaningless and must clamp, not reflect
    // into a positive tangent.
    EXPECT_FLOAT_EQ(Renderer::ResolveShadowTanHalfAngle(-3.0f), 0.0f);

    // Doubling the angle more than doubles the tangent (tan is convex here).
    // Pins that an ANGLE is being resolved, not a linear scale factor.
    const float t1 = Renderer::ResolveShadowTanHalfAngle(10.0f);
    const float t2 = Renderer::ResolveShadowTanHalfAngle(20.0f);
    EXPECT_GT(t2, 2.0f * t1);
}

// Penumbra is a per-LIGHT property. Before this slice the inspector rendered its
// PCSS controls inside a specific light's panel but wrote a single global on
// ShadowMapRenderFeature, so editing light A silently changed light B's shadows.
// A regression guard, not a red-green driver: the component data is already
// per-light, and this exists to fail loudly if a global is reintroduced.
TEST(PhysicalPenumbra, PerLightDiametersAreIndependent)
{
    Components::Light sun{};
    sun.Type = Components::LightType::Directional;
    sun.ShadowAngularDiameter = 0.53f;

    Components::Light broad{};
    broad.Type = Components::LightType::Directional;
    broad.ShadowAngularDiameter = 10.0f;

    EXPECT_GT(Renderer::ResolveShadowTanHalfAngle(broad.ShadowAngularDiameter),
              Renderer::ResolveShadowTanHalfAngle(sun.ShadowAngularDiameter));

    // Authoring one must not move the other.
    broad.ShadowAngularDiameter = 20.0f;
    EXPECT_FLOAT_EQ(sun.ShadowAngularDiameter, 0.53f);

    // And the resolved tangent tracks each light's own value, not a shared one.
    EXPECT_FLOAT_EQ(Renderer::ResolveShadowTanHalfAngle(sun.ShadowAngularDiameter),
                    Renderer::ResolveShadowTanHalfAngle(0.53f));
    EXPECT_FLOAT_EQ(Renderer::ResolveShadowTanHalfAngle(broad.ShadowAngularDiameter),
                    Renderer::ResolveShadowTanHalfAngle(20.0f));
}
