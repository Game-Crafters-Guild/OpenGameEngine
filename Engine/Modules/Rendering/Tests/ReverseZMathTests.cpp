#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "Engine/Rendering/CameraUtils.h"
#include "Mathematics/MatrixOps.h"

using namespace GameEngine::Mathematics;

using GameEngine::Engine::Renderer::ExtractNearFarLH_ZO;
using GameEngine::Engine::Renderer::LinearizeReverseZDepthLH_ZO;

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kFovY = kPi / 3.0f; // 60 degrees
constexpr float kAspect = 16.0f / 9.0f;

float NdcDepthFromViewZ(const Matrix4x4& proj, float viewZ)
{
    Vector4 clip = proj.Transform(Vector4(0.0f, 0.0f, viewZ, 1.0f));
    return clip.z / clip.w;
}

float ProjectAndUnprojectViewZ(const Matrix4x4& proj, const Matrix4x4& invProj, float viewZ)
{
    Vector4 clip = proj.Transform(Vector4(0.0f, 0.0f, viewZ, 1.0f));
    Vector4 ndc(clip.x / clip.w, clip.y / clip.w, clip.z / clip.w, 1.0f);
    Vector4 view = invProj.Transform(ndc);
    return view.z / view.w;
}

}  // namespace

// === Endpoint tests: zNear must map to NDC.z = 1.0, zFar must map to 0.0. ===

TEST(ReverseZMath, PerspectiveEndpointsExact)
{
    const float zNear = 0.1f;
    const float zFar = 1000.0f;
    auto proj = MakePerspectiveLH_ZO_ReverseZ(kFovY, kAspect, zNear, zFar);

    EXPECT_NEAR(NdcDepthFromViewZ(proj, zNear), 1.0f, 1e-5f);
    EXPECT_NEAR(NdcDepthFromViewZ(proj, zFar), 0.0f, 1e-5f);
}

TEST(ReverseZMath, OrthographicEndpointsExact)
{
    const float zNear = 0.5f;
    const float zFar = 500.0f;
    auto proj = MakeOrthographicLH_ZO_ReverseZ(-10.0f, 10.0f, -8.0f, 8.0f, zNear, zFar);

    EXPECT_NEAR(NdcDepthFromViewZ(proj, zNear), 1.0f, 1e-5f);
    EXPECT_NEAR(NdcDepthFromViewZ(proj, zFar), 0.0f, 1e-5f);
}

TEST(ReverseZMath, PerspectiveMidFrustumIsNonLinear)
{
    // Reverse-Z + perspective is hyperbolic in 1/z: the geometric mid-Z view-space
    // value maps to a small NDC depth (close to far), because near gets the lion's
    // share of the [0,1] depth budget. This is the entire point of reverse-Z + float —
    // float density near 0 cancels the 1/z non-linearity, so depth precision is
    // strongly biased toward the camera in view space.
    const float zNear = 0.1f;
    const float zFar = 1000.0f;
    auto proj = MakePerspectiveLH_ZO_ReverseZ(kFovY, kAspect, zNear, zFar);

    const float midZ = (zNear + zFar) * 0.5f;
    const float midDepth = NdcDepthFromViewZ(proj, midZ);
    EXPECT_LT(midDepth, 0.05f) << "Geometric mid-Z should map to a small NDC depth "
                               << "(reverse-Z hyperbolic). Got " << midDepth;

    // A 1mm step beyond near should still leave NDC depth near 1.0. For near=0.1m
    // and z=0.101m, the formula gives depth ≈ 0.99. This is the precision win.
    const float justPastNear = zNear + 0.001f;
    const float justPastDepth = NdcDepthFromViewZ(proj, justPastNear);
    EXPECT_GT(justPastDepth, 0.99f) << "Just past the near plane (1mm step), depth "
                                    << "should still be near 1.0. Got " << justPastDepth;
}

// === Round-trip: project then unproject must recover view-Z. ===

TEST(ReverseZMath, PerspectiveRoundTripPreservesViewZ)
{
    const float zNear = 0.1f;
    const float zFar = 1000.0f;
    auto proj = MakePerspectiveLH_ZO_ReverseZ(kFovY, kAspect, zNear, zFar);
    auto invProj = Inverse(proj);

    const float samples[] = {
        zNear,
        zNear * 2.0f,
        zNear * 10.0f,
        zFar * 0.25f,
        zFar * 0.5f,
        zFar * 0.9f,
        zFar - 0.01f,
        zFar,
    };

    for (float viewZ : samples)
    {
        const float recovered = ProjectAndUnprojectViewZ(proj, invProj, viewZ);
        EXPECT_NEAR(recovered, viewZ, std::max(viewZ * 1e-3f, 1e-3f))
            << "Reverse-Z perspective round-trip failed at view-Z=" << viewZ;
    }
}

TEST(ReverseZMath, OrthographicRoundTripPreservesViewZ)
{
    const float zNear = 0.5f;
    const float zFar = 500.0f;
    auto proj = MakeOrthographicLH_ZO_ReverseZ(-10.0f, 10.0f, -8.0f, 8.0f, zNear, zFar);
    auto invProj = Inverse(proj);

    const float samples[] = {
        zNear,
        zNear + 1.0f,
        zFar * 0.5f,
        zFar - 1.0f,
        zFar,
    };

    for (float viewZ : samples)
    {
        const float recovered = ProjectAndUnprojectViewZ(proj, invProj, viewZ);
        EXPECT_NEAR(recovered, viewZ, 1e-3f)
            << "Reverse-Z ortho round-trip failed at view-Z=" << viewZ;
    }
}

// === Precision regression: D32 reverse-Z must distinguish far - 1cm vs far - 10cm. ===
// Forward-Z D24 fails this comparison by ~6 orders of magnitude in the same range.

TEST(ReverseZMath, FarPlanePrecisionWin)
{
    const float zNear = 0.1f;
    const float zFar = 1000.0f;
    auto proj = MakePerspectiveLH_ZO_ReverseZ(kFovY, kAspect, zNear, zFar);

    const float depth1 = NdcDepthFromViewZ(proj, zFar - 0.01f); // far - 1cm
    const float depth2 = NdcDepthFromViewZ(proj, zFar - 0.10f); // far - 10cm

    const float delta = std::fabs(depth1 - depth2);
    EXPECT_GT(delta, 1e-9f) << "Reverse-Z must distinguish 1cm vs 10cm at far plane "
                            << "(depth1=" << depth1 << " depth2=" << depth2 << ")";
}

// === Sanity: matrix structure (column-major, LH w=z). ===

TEST(ReverseZMath, PerspectiveMatrixStructure)
{
    const float zNear = 0.1f;
    const float zFar = 100.0f;
    auto proj = MakePerspectiveLH_ZO_ReverseZ(kFovY, kAspect, zNear, zFar);

    // proj[col][row] convention. LH means column 2 row 3 = 1 (w_clip = z_view).
    EXPECT_NEAR(proj[2][3], 1.0f, 1e-6f);
    // Translation only in column 3, row 2 (z_clip from constant).
    EXPECT_NEAR(proj[3][3], 0.0f, 1e-6f);
    // m22 should be -near/(far-near).
    EXPECT_NEAR(proj[2][2], -zNear / (zFar - zNear), 1e-5f);
    // m32 should be near*far/(far-near).
    EXPECT_NEAR(proj[3][2], zNear * zFar / (zFar - zNear), 1e-5f);
}

TEST(ReverseZMath, OrthographicMatrixStructure)
{
    const float zNear = 1.0f;
    const float zFar = 100.0f;
    auto proj = MakeOrthographicLH_ZO_ReverseZ(-10.0f, 10.0f, -8.0f, 8.0f, zNear, zFar);

    // Ortho w_clip is constant 1.
    EXPECT_NEAR(proj[2][3], 0.0f, 1e-6f);
    EXPECT_NEAR(proj[3][3], 1.0f, 1e-6f);
    // m22 = -1/(far-near), m32 = far/(far-near).
    EXPECT_NEAR(proj[2][2], -1.0f / (zFar - zNear), 1e-5f);
    EXPECT_NEAR(proj[3][2], zFar / (zFar - zNear), 1e-5f);
}

// === Direction-aware tests: near must be NDC-greater than far. ===

TEST(ReverseZMath, PerspectiveNearIsGreaterThanFar)
{
    const float zNear = 0.1f;
    const float zFar = 1000.0f;
    auto proj = MakePerspectiveLH_ZO_ReverseZ(kPi / 3.0f, 16.0f / 9.0f, zNear, zFar);

    const float dNear = NdcDepthFromViewZ(proj, zNear + 0.01f);
    const float dFar = NdcDepthFromViewZ(proj, zFar - 0.01f);
    EXPECT_GT(dNear, dFar) << "Reverse-Z: a closer fragment must produce a larger NDC depth.";
}

TEST(ReverseZMath, OrthographicNearIsGreaterThanFar)
{
    const float zNear = 0.5f;
    const float zFar = 500.0f;
    auto proj = MakeOrthographicLH_ZO_ReverseZ(-10.0f, 10.0f, -8.0f, 8.0f, zNear, zFar);

    const float dNear = NdcDepthFromViewZ(proj, zNear + 1.0f);
    const float dFar = NdcDepthFromViewZ(proj, zFar - 1.0f);
    EXPECT_GT(dNear, dFar) << "Reverse-Z ortho: closer fragment must be NDC-greater.";
}

// === ExtractNearFarLH_ZO must round-trip the projection's near/far. ===

// Round-trip error budget. The builder stores m22 = -near * invRange and
// m32 = (near * far) * invRange; the extraction divides those two entries, so
// the shared invRange cancels exactly and each recovered plane carries at most
// four float roundings (the stored products, the 1 - m22 subtraction for near,
// and the division). Compilers MAY contract 1 - m22 into an fma (Clang does);
// the contracted path, modelled explicitly over 66,000 frusta, stays within
// the same bound (worst 0.42x of tolerance), so the budget holds with or
// without contraction. Four roundings bound the relative error at
// 4 * FLT_EPSILON/2; the factor below carries 2x margin.
constexpr float kExtractRelTol = 4.0f * std::numeric_limits<float>::epsilon();

TEST(ReverseZMath, ExtractNearFarRoundTrips)
{
    // These must differ from ExtractNearFarLH_ZO's degenerate-input fallbacks
    // (0.1 / 1000, and 0.1 / 1000.1 for the second-stage clamp). With the
    // fallback values the test passes even when the perspective extraction is
    // never reached, validating the constants instead of the math.
    const float zNear = 0.25f;
    const float zFar = 750.0f;
    auto proj = MakePerspectiveLH_ZO_ReverseZ(kPi / 3.0f, 16.0f / 9.0f, zNear, zFar);

    float recoveredNear = 0.0f, recoveredFar = 0.0f;
    ExtractNearFarLH_ZO(proj.Data(), recoveredNear, recoveredFar);

    EXPECT_NEAR(recoveredNear, zNear, zNear * kExtractRelTol);
    EXPECT_NEAR(recoveredFar, zFar, zFar * kExtractRelTol);
}

TEST(ReverseZMath, ExtractNearFarRoundTripsOrthographic)
{
    const float zNear = 0.5f;
    const float zFar = 500.0f;
    auto proj = MakeOrthographicLH_ZO_ReverseZ(-10.0f, 10.0f, -8.0f, 8.0f, zNear, zFar);

    float recoveredNear = 0.0f, recoveredFar = 0.0f;
    ExtractNearFarLH_ZO(proj.Data(), recoveredNear, recoveredFar);

    EXPECT_NEAR(recoveredNear, zNear, 1e-3f);
    EXPECT_NEAR(recoveredFar, zFar, 1e-3f);
}

TEST(ReverseZMath, LinearizeReverseZDepthRoundTripsOrthographic)
{
    const float zNear = 0.5f;
    const float zFar = 500.0f;
    auto proj = MakeOrthographicLH_ZO_ReverseZ(-10.0f, 10.0f, -8.0f, 8.0f, zNear, zFar);

    const float samples[] = {zNear, zNear + 2.0f, 42.0f, zFar * 0.5f, zFar - 1.0f};
    for (float viewZ : samples)
    {
        const float ndcDepth = NdcDepthFromViewZ(proj, viewZ);
        const float recovered = LinearizeReverseZDepthLH_ZO(ndcDepth, zNear, zFar, true);
        EXPECT_NEAR(recovered, viewZ, 1e-3f)
            << "Reverse-Z ortho linearization failed at view-Z=" << viewZ;
    }
}
