// CascadeReceiverFitTests — the directional cascades fitted to the receivers
// SDSM measured instead of to the camera frustum slice. A pitched RTS camera
// over flat ground (and a tower) is "measured" on the CPU exactly as the
// receiver reduce bins its depth and its rays (ShadowReceiverMeasurement), and
// the tests check what the fit makes of it:
//   * cascade 0's slice starts at the nearest receiver, pulled in by the
//     camera's motion since the measurement, and at the near plane without a
//     usable measurement;
//   * each cascade's box shrinks to the receivers it serves, through a
//     perspective and through an orthographic window;
//   * every receiver the shader can select into a cascade (its depth range
//     plus the blend band ahead of it) still projects inside that cascade's
//     box, with the filter's reach around it, after the camera moved, turned
//     or rolled, or its window changed, since the measurement;
//   * the casters are culled to the receivers' reach, on a narrowed box and
//     on a slice box alike;
//   * the air in front of the receivers, where volumetric fog and transparent
//     surfaces look the cascades up, and the eye, where the sun glare does,
//     stay inside the cascades that select them, at the strategy camera and at
//     eye level, under high and low suns, and under an orthographic strategy
//     camera.
// Pure CPU: ComputeCascades touches no device (the feature is never
// Initialize()d), as in CascadeFitStabilityTests.

#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/ShadowReceiverMeasurement.h"

#include "Mathematics/Geometry.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Common/Frustum.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Mathematics;
namespace Renderer = GameEngine::Engine::Renderer;

using Renderer::CascadeFrameData;
using Renderer::ShadowMapRenderFeature;
using Renderer::ShadowReceiverMeasurement;

namespace
{

constexpr float kPi = 3.14159265358979323846f;
constexpr float kNear = 0.5f;
constexpr float kFar = 1000.0f;
constexpr float kMaxShadowDistance = 150.0f;
constexpr float kFovYDeg = 45.0f;
constexpr float kAspect = 2062.0f / 1396.0f;
// Measurement raster: the reduce samples one pixel in four of a full view;
// this grid stands in for those samples.
constexpr uint32_t kGridW = 160;
constexpr uint32_t kGridH = 108;
const Vector3 kLightDir = Vector3{0.35f, -0.65f, 0.45f}.Normalize();
// A tower on flat ground, as in ShadowReceiverReduceComputeTests.
const AABB kTower{{-3.0f, 0.0f, 8.0f}, {3.0f, 12.0f, 14.0f}};

struct Pose
{
    Vector3 Position;
    Vector3 Forward;
};

// What the camera looks through: its vertical field of view, its aspect, its
// roll about the forward axis and, when positive, the half-height in metres of
// an orthographic window in place of the field of view.
struct Lens
{
    float FovYDeg = kFovYDeg;
    float Aspect = kAspect;
    float RollDeg = 0.0f;
    float OrthographicHalfHeight = 0.0f;
};

// The camera's up vector: world up made perpendicular to the forward, rolled.
Vector3 LensUp(const Pose& pose, const Lens& lens)
{
    const Vector3 right = Vector3::Cross(Vector3{0.0f, 1.0f, 0.0f}, pose.Forward).Normalize();
    const Vector3 up = Vector3::Cross(pose.Forward, right);
    const float roll = lens.RollDeg * kPi / 180.0f;
    return (up * std::cos(roll) - right * std::sin(roll)).Normalize();
}

// The lakeside's game camera: a 40 degree boom 62 m from a pivot on the ground.
Pose RtsPose(const Vector3& pivot, float boom = 62.4f, float pitchDeg = 40.0f)
{
    const float pitch = pitchDeg * kPi / 180.0f;
    const Vector3 forward{0.0f, -std::sin(pitch), std::cos(pitch)};
    return Pose{pivot - forward * boom, forward};
}

Rendering::CameraData MakeCamera(const Pose& pose, const Lens& lens = {})
{
    Rendering::CameraData cam{};
    const Matrix4x4 view = MakeLookAtLH(pose.Position, pose.Position + pose.Forward, LensUp(pose, lens));
    const float halfY = lens.OrthographicHalfHeight;
    const Matrix4x4 proj =
        halfY > 0.0f
            ? MakeOrthographicLH_ZO_ReverseZ(-halfY * lens.Aspect, halfY * lens.Aspect, -halfY, halfY, kNear, kFar)
            : MakePerspectiveLH_ZO_ReverseZ(lens.FovYDeg * kPi / 180.0f, lens.Aspect, kNear, kFar);
    const Matrix4x4 viewProj = proj * view;
    std::memcpy(cam.view, view.Data(), sizeof(cam.view));
    std::memcpy(cam.proj, proj.Data(), sizeof(cam.proj));
    std::memcpy(cam.viewProj, viewProj.Data(), sizeof(cam.viewProj));
    cam.cameraPos[0] = pose.Position.x;
    cam.cameraPos[1] = pose.Position.y;
    cam.cameraPos[2] = pose.Position.z;
    return cam;
}

// A visible receiver: the nearest ground or tower hit along a grid ray.
struct Receiver
{
    Vector3 Position;
    float ViewDepth = 0.0f;
    // Horizontal NDC of the grid ray that found it.
    float ScreenX = 0.0f;
};

// Direction of the grid ray through cell (x, y) of the measurement raster.
Vector3 GridRay(const Pose& pose, const Lens& lens, uint32_t x, uint32_t y)
{
    if (lens.OrthographicHalfHeight > 0.0f)
        return pose.Forward;
    const Vector3 up = LensUp(pose, lens);
    const Vector3 right = Vector3::Cross(up, pose.Forward).Normalize();
    const float tanHalfY = std::tan(lens.FovYDeg * 0.5f * kPi / 180.0f);
    const float ndcX = (static_cast<float>(x) + 0.5f) / kGridW * 2.0f - 1.0f;
    const float ndcY = 1.0f - (static_cast<float>(y) + 0.5f) / kGridH * 2.0f;
    return (pose.Forward + right * (ndcX * tanHalfY * lens.Aspect) + up * (ndcY * tanHalfY)).Normalize();
}

// Origin of the grid ray through cell (x, y): the eye under a perspective lens,
// the cell's point on the window's plane through the eye under an orthographic one.
Vector3 GridOrigin(const Pose& pose, const Lens& lens, uint32_t x, uint32_t y)
{
    const float halfY = lens.OrthographicHalfHeight;
    if (halfY <= 0.0f)
        return pose.Position;
    const Vector3 up = LensUp(pose, lens);
    const Vector3 right = Vector3::Cross(up, pose.Forward).Normalize();
    const float ndcX = (static_cast<float>(x) + 0.5f) / kGridW * 2.0f - 1.0f;
    const float ndcY = 1.0f - (static_cast<float>(y) + 0.5f) / kGridH * 2.0f;
    return pose.Position + right * (ndcX * halfY * lens.Aspect) + up * (ndcY * halfY);
}

// Distance along the unit ray from `origin` to the first ground or tower hit;
// the largest float through the sky.
float FirstSurfaceDistance(const Vector3& origin, const Vector3& dir)
{
    float best = std::numeric_limits<float>::max();
    if (dir.y < 0.0f)
        best = -origin.y / dir.y;
    Ray3D ray;
    ray.origin = origin;
    ray.direction = dir;
    float tMin = 0.0f;
    float tMax = 0.0f;
    if (IntersectRayAABB(ray, kTower, tMin, tMax))
        best = std::min(best, tMin);
    return best;
}

std::vector<Receiver> CastReceivers(const Pose& pose, const Lens& lens = {})
{
    std::vector<Receiver> out;
    for (uint32_t y = 0; y < kGridH; ++y)
    {
        for (uint32_t x = 0; x < kGridW; ++x)
        {
            const Vector3 origin = GridOrigin(pose, lens, x, y);
            const Vector3 dir = GridRay(pose, lens, x, y);
            const float best = FirstSurfaceDistance(origin, dir);
            if (best == std::numeric_limits<float>::max())
                continue;
            const Vector3 p = origin + dir * best;
            const float ndcX = (static_cast<float>(x) + 0.5f) / kGridW * 2.0f - 1.0f;
            out.push_back(Receiver{p, Vector3::Dot(p - pose.Position, pose.Forward), ndcX});
        }
    }
    return out;
}

// The camera half of a measurement context for `pose` through `lens`, as the
// reduce records it and the fit describes the current camera.
ShadowReceiverMeasurement::Context CameraContext(const Pose& pose, const Lens& lens = {})
{
    ShadowReceiverMeasurement::Context c{};
    Renderer::SetShadowReceiverCamera(MakeCamera(pose, lens), lens.OrthographicHalfHeight > 0.0f, c);
    return c;
}

// What the receiver reduce would read back for `receivers` seen from `pose`:
// the same context the ShadowMap node records, the nearest depth, and per
// log-spaced bin the light-space box of the receivers and of their rays'
// parameters (the grid rays that see the sky in the record past every bin):
// the light-space offset from the eye per unit view depth under a perspective
// lens, the offset from the eye's point at that depth under an orthographic one.
ShadowReceiverMeasurement Measure(const Pose& pose, const std::vector<Receiver>& receivers,
                                  const Lens& lens = {}, const Vector3& lightDir = kLightDir)
{
    ShadowReceiverMeasurement m{};
    m.Measured = CameraContext(pose, lens);
    ShadowReceiverMeasurement::Context& c = m.Measured;
    c.NearPlane = kNear;
    c.FarPlane = kFar;
    c.LightDirection = lightDir;
    c.BinNear = kNear;
    c.BinFar = kMaxShadowDistance;
    const Matrix4x4 lightRot = ShadowMapRenderFeature::CascadeLightRotation(lightDir);
    c.CameraLightSpace = lightRot.TransformPoint(pose.Position);
    const Vector4 forward = lightRot.Transform(Vector4{pose.Forward.x, pose.Forward.y, pose.Forward.z, 0.0f});
    c.ForwardLightSpace = Vector3{forward.x, forward.y, forward.z};
    const float binsPerLog = Renderer::ShadowReceiverBinsPerLogUnit(c.BinNear, c.BinFar);
    constexpr uint32_t kPast = ShadowReceiverMeasurement::kDepthBins;
    m.Bins.fill(AABB::Empty());
    m.Rays.fill(AABB::Empty());
    m.NearDepth = std::numeric_limits<float>::max();
    for (const Receiver& r : receivers)
    {
        m.NearDepth = std::min(m.NearDepth, r.ViewDepth);
        const Vector3 ls = lightRot.TransformPoint(r.Position);
        const Vector3 ray = c.Orthographic ? ls - c.CameraLightSpace - c.ForwardLightSpace * r.ViewDepth
                                           : (ls - c.CameraLightSpace) * (1.0f / r.ViewDepth);
        if (r.ViewDepth > c.BinFar)
        {
            m.Rays[kPast].Expand(ray);
            continue;
        }
        const float t = std::log(std::max(r.ViewDepth, c.BinNear) / c.BinNear) * binsPerLog;
        const uint32_t bin = std::min(static_cast<uint32_t>(t), kPast - 1);
        m.Bins[bin].Expand(ls);
        m.Rays[bin].Expand(ray);
    }
    for (uint32_t y = 0; y < kGridH; ++y)
    {
        for (uint32_t x = 0; x < kGridW; ++x)
        {
            const Vector3 origin = GridOrigin(pose, lens, x, y);
            const Vector3 dir = GridRay(pose, lens, x, y);
            if (FirstSurfaceDistance(origin, dir) != std::numeric_limits<float>::max())
                continue;
            // Under an orthographic lens the parameter is the origin's offset at
            // every depth; under a perspective one, the ray at unit depth.
            const Vector3 atParameter =
                c.Orthographic ? origin : pose.Position + dir * (1.0f / Vector3::Dot(dir, pose.Forward));
            m.Rays[kPast].Expand(lightRot.TransformPoint(atParameter) - c.CameraLightSpace);
        }
    }
    m.Valid = !receivers.empty();
    return m;
}

// The lakeside's shadow settings: 150 m, split lambda 0.82.
void Configure(ShadowMapRenderFeature& feature)
{
    feature.ApplyRuntimeShadowSettings(kMaxShadowDistance, 0.82f, feature.GetConfig().DepthBias,
                                       feature.GetConfig().NormalBias);
}

// Largest |light NDC| coordinate of `p` in cascade c's box: inside when <= 1.
float MaxLightNdc(const CascadeFrameData& fd, uint32_t c, const Vector3& p)
{
    const Vector4 clip = fd.LightVPRel[c].Transform(Vector4{p.x, p.y, p.z, 1.0f});
    return std::max(std::abs(clip.x / clip.w), std::abs(clip.y / clip.w));
}

} // namespace

TEST(CascadeReceiverFit, Cascade0StartsAtTheNearestMeasuredReceiver)
{
    // At the game camera the nearest visible ground is tens of metres away;
    // a slice from the near plane spends cascade 0 on empty air in front of it.
    const Pose pose = RtsPose(Vector3{0.0f, 0.0f, 20.0f});
    const Rendering::CameraData cam = MakeCamera(pose);
    const std::vector<Receiver> receivers = CastReceivers(pose);
    const ShadowReceiverMeasurement measured = Measure(pose, receivers);
    ASSERT_GT(measured.NearDepth, 25.0f) << "the pose must see nothing for the first tens of metres";

    ShadowMapRenderFeature feature;
    Configure(feature);
    feature.SetFitFreezeEnabled(false);
    ShadowMapRenderFeature::SDSMBounds sdsm{};
    sdsm.nearDepth = measured.NearDepth;
    sdsm.farDepth = kMaxShadowDistance;
    sdsm.valid = true;
    const CascadeFrameData fd = feature.ComputeCascades(cam, kNear, kFar, kLightDir, &sdsm, 1, nullptr, &measured);
    EXPECT_FLOAT_EQ(feature.Cascade0SliceStart(&measured, kNear, fd.SplitDistances[0], CameraContext(pose)),
                    measured.NearDepth);

    ShadowMapRenderFeature unmeasured;
    Configure(unmeasured);
    unmeasured.SetFitFreezeEnabled(false);
    const CascadeFrameData fromNear = unmeasured.ComputeCascades(cam, kNear, kFar, kLightDir, &sdsm, 1);
    EXPECT_LT(fd.OrthoHalfExtent[0], fromNear.OrthoHalfExtent[0])
        << "a slice that starts at the nearest receiver fits a smaller box";

    // Every receiver cascade 0 is selected for still projects inside its box.
    for (const Receiver& r : receivers)
    {
        if (r.ViewDepth < fd.SplitDistances[0])
            EXPECT_LE(MaxLightNdc(fd, 0, r.Position), 1.0f) << "receiver at depth " << r.ViewDepth;
    }
}

TEST(CascadeReceiverFit, Cascade0StartIsPulledInByTheCameraMotionSinceTheMeasurement)
{
    const Pose measuredPose = RtsPose(Vector3{0.0f, 0.0f, 20.0f});
    const ShadowReceiverMeasurement measured = Measure(measuredPose, CastReceivers(measuredPose));
    ShadowMapRenderFeature feature;
    Configure(feature);

    // Five metres closer along the view: receivers can now be five metres nearer.
    const Pose moved{measuredPose.Position + measuredPose.Forward * 5.0f, measuredPose.Forward};
    EXPECT_NEAR(feature.Cascade0SliceStart(&measured, kNear, 80.0f, CameraContext(moved)),
                measured.NearDepth - 5.0f, 1e-3f);

    // A turn sweeps the measured receivers by their distance times the turn.
    const float turn = 2.0f * kPi / 180.0f;
    const Vector3 turned{measuredPose.Forward.x + std::sin(turn), measuredPose.Forward.y,
                         measuredPose.Forward.z};
    const float start = feature.Cascade0SliceStart(
        &measured, kNear, 80.0f, CameraContext(Pose{measuredPose.Position, turned.Normalize()}));
    EXPECT_LT(start, measured.NearDepth);
    EXPECT_GE(start, kNear);

    // A cut far away leaves no usable near bound: the slice starts at the near plane.
    EXPECT_FLOAT_EQ(feature.Cascade0SliceStart(
                        &measured, kNear, 80.0f,
                        CameraContext(Pose{measuredPose.Position + Vector3{500.0f, 0.0f, 0.0f},
                                           measuredPose.Forward})),
                    kNear);
}

TEST(CascadeReceiverFit, Cascade0StartsAtTheNearPlaneWithoutAMeasurementOrOutsideClose)
{
    const Pose pose = RtsPose(Vector3{0.0f, 0.0f, 20.0f});
    const ShadowReceiverMeasurement measured = Measure(pose, CastReceivers(pose));
    ShadowMapRenderFeature feature;
    Configure(feature);
    const ShadowReceiverMeasurement::Context current = CameraContext(pose);
    EXPECT_FLOAT_EQ(feature.Cascade0SliceStart(nullptr, kNear, 80.0f, current), kNear);
    ShadowReceiverMeasurement invalid = measured;
    invalid.Valid = false;
    EXPECT_FLOAT_EQ(feature.Cascade0SliceStart(&invalid, kNear, 80.0f, current), kNear);
    for (const Renderer::ShadowProjection projection :
         {Renderer::ShadowProjection::Stable, Renderer::ShadowProjection::WorldTexel})
    {
        feature.SetShadowProjection(projection);
        EXPECT_FLOAT_EQ(feature.Cascade0SliceStart(&measured, kNear, 80.0f, current), kNear);
    }

    // Without a measurement the fit is the one the near-plane slice gives.
    ShadowMapRenderFeature a;
    Configure(a);
    ShadowMapRenderFeature b;
    Configure(b);
    const Rendering::CameraData cam = MakeCamera(pose);
    const CascadeFrameData withNull = a.ComputeCascades(cam, kNear, kFar, kLightDir, nullptr, 1, nullptr, nullptr);
    const CascadeFrameData withInvalid = b.ComputeCascades(cam, kNear, kFar, kLightDir, nullptr, 1, nullptr, &invalid);
    for (uint32_t c = 0; c < withNull.NumCascades; ++c)
        EXPECT_EQ(std::memcmp(withNull.LightVPRel[c].Data(), withInvalid.LightVPRel[c].Data(),
                              16 * sizeof(float)),
                  0)
            << "cascade " << c;
}

namespace
{

// Depth window cascade c is sampled over, as the shader selects it: its own
// range plus the previous cascade's blend band (shadow_sampling.glsl).
void CascadeDepthWindow(const CascadeFrameData& fd, uint32_t c, float& lo, float& hi)
{
    lo = 0.0f;
    if (c > 0)
        lo = fd.SplitDistances[c - 1] - ShadowMapRenderFeature::CascadeBlendBand(fd, c - 1);
    hi = c + 1 == fd.NumCascades ? kMaxShadowDistance : fd.SplitDistances[c];
}

// Every receiver the shader selects into cascade c projects inside its box
// with `reachTexels` of the shadow map to spare on every side, and inside its
// depth window.
void ExpectReceiversInside(const CascadeFrameData& fd, const std::vector<Receiver>& receivers,
                           float reachTexels, uint32_t resolution)
{
    for (uint32_t c = 0; c < fd.NumCascades; ++c)
    {
        float lo = 0.0f;
        float hi = 0.0f;
        CascadeDepthWindow(fd, c, lo, hi);
        const float limit = 1.0f - 2.0f * reachTexels / static_cast<float>(resolution);
        uint32_t checked = 0;
        uint32_t outside = 0;
        for (const Receiver& r : receivers)
        {
            if (r.ViewDepth < lo || r.ViewDepth > hi)
                continue;
            ++checked;
            const Vector4 clip = fd.LightVPRel[c].Transform(
                Vector4{r.Position.x, r.Position.y, r.Position.z, 1.0f});
            const float z = clip.z / clip.w;
            if (MaxLightNdc(fd, c, r.Position) > limit || z < 0.0f || z > 1.0f)
                ++outside;
        }
        EXPECT_GT(checked, 0u) << "cascade " << c << " serves no receiver at this pose";
        EXPECT_EQ(outside, 0u) << "cascade " << c << ": " << outside << " of " << checked
                               << " receivers outside the box";
    }
}

} // namespace

TEST(CascadeReceiverFit, EachCascadeBoxShrinksToTheReceiversItServes)
{
    const Pose pose = RtsPose(Vector3{0.0f, 0.0f, 20.0f});
    const Rendering::CameraData cam = MakeCamera(pose);
    const std::vector<Receiver> receivers = CastReceivers(pose);
    const ShadowReceiverMeasurement measured = Measure(pose, receivers);
    ShadowMapRenderFeature::SDSMBounds sdsm{};
    sdsm.nearDepth = measured.NearDepth;
    sdsm.farDepth = kMaxShadowDistance;
    sdsm.valid = true;

    ShadowMapRenderFeature fitted;
    Configure(fitted);
    fitted.SetFitFreezeEnabled(false);
    const CascadeFrameData fd = fitted.ComputeCascades(cam, kNear, kFar, kLightDir, &sdsm, 1, nullptr, &measured);
    ShadowMapRenderFeature sliced;
    Configure(sliced);
    sliced.SetFitFreezeEnabled(false);
    const CascadeFrameData slice = sliced.ComputeCascades(cam, kNear, kFar, kLightDir, &sdsm, 1);

    for (uint32_t c = 0; c < fd.NumCascades; ++c)
    {
        EXPECT_LE(fd.OrthoHalfExtent[c], slice.OrthoHalfExtent[c]) << "cascade " << c;
        EXPECT_LE(fd.DepthSpan[c], slice.DepthSpan[c]) << "cascade " << c;
    }
    // The far cascades' slices reach under the ground and into the sky: most
    // of their boxes held nothing to receive.
    EXPECT_LT(fd.OrthoHalfExtent[3], 0.8f * slice.OrthoHalfExtent[3]);
    // 32 texels: the PCSS blocker search at 64 taps.
    ExpectReceiversInside(fd, receivers, 32.0f, fitted.GetConfig().Resolution);
}

TEST(CascadeReceiverFit, AnOrthographicCascadeBoxShrinksToTheReceiversItServes)
{
    // Through an orthographic window the rays are parallel: a ray's points
    // are its offset from the eye plus the forward times their depth, not the
    // offset scaled by the depth, which would swell every box back to its slice.
    const Pose pose = RtsPose(Vector3{0.0f, 0.0f, 20.0f});
    const Lens orthographic{kFovYDeg, kAspect, 0.0f, 30.0f};
    const Rendering::CameraData cam = MakeCamera(pose, orthographic);
    const ShadowReceiverMeasurement measured =
        Measure(pose, CastReceivers(pose, orthographic), orthographic);
    ShadowMapRenderFeature::SDSMBounds sdsm{};
    sdsm.nearDepth = measured.NearDepth;
    sdsm.farDepth = kMaxShadowDistance;
    sdsm.valid = true;

    ShadowMapRenderFeature fitted;
    Configure(fitted);
    fitted.SetFitFreezeEnabled(false);
    const CascadeFrameData fd = fitted.ComputeCascades(cam, kNear, kFar, kLightDir, &sdsm, 1, nullptr, &measured);
    ShadowMapRenderFeature sliced;
    Configure(sliced);
    sliced.SetFitFreezeEnabled(false);
    const CascadeFrameData slice = sliced.ComputeCascades(cam, kNear, kFar, kLightDir, &sdsm, 1);
    for (uint32_t c = 0; c < fd.NumCascades; ++c)
        EXPECT_LE(fd.OrthoHalfExtent[c], slice.OrthoHalfExtent[c]) << "cascade " << c;
    EXPECT_LT(fd.OrthoHalfExtent[3], 0.8f * slice.OrthoHalfExtent[3]);
}

TEST(CascadeReceiverFit, BoxesHoldTheReceiversRevealedByCameraMotionSinceTheMeasurement)
{
    // The readback lands frames after the depth it measured: the camera has
    // since panned and turned. What the current camera sees must still fit.
    const Pose measuredPose = RtsPose(Vector3{0.0f, 0.0f, 20.0f});
    const ShadowReceiverMeasurement measured = Measure(measuredPose, CastReceivers(measuredPose));
    for (const float panMetres : {1.0f, 4.0f, 12.0f})
    {
        SCOPED_TRACE(::testing::Message() << "pan " << panMetres << " m");
        const float yaw = 1.5f * kPi / 180.0f;
        Pose pose = RtsPose(Vector3{panMetres, 0.0f, 20.0f + 0.5f * panMetres});
        pose.Forward = Vector3{pose.Forward.x * std::cos(yaw) + pose.Forward.z * std::sin(yaw),
                               pose.Forward.y,
                               pose.Forward.z * std::cos(yaw) - pose.Forward.x * std::sin(yaw)};
        const std::vector<Receiver> receivers = CastReceivers(pose);
        ShadowMapRenderFeature feature;
        Configure(feature);
        feature.SetFitFreezeEnabled(false);
        ShadowMapRenderFeature::SDSMBounds sdsm{};
        sdsm.nearDepth = measured.NearDepth;
        sdsm.farDepth = kMaxShadowDistance;
        sdsm.valid = true;
        const CascadeFrameData fd =
            feature.ComputeCascades(MakeCamera(pose), kNear, kFar, kLightDir, &sdsm, 1, nullptr, &measured);
        ExpectReceiversInside(fd, receivers, 32.0f, feature.GetConfig().Resolution);
    }
}

TEST(CascadeReceiverFit, AMeasurementUnderAnotherLightDirectionIsCarriedIntoTheCurrentLightSpace)
{
    // A moving sun: the measurement's bins are in the light basis of a few
    // frames ago.
    const Pose pose = RtsPose(Vector3{0.0f, 0.0f, 20.0f});
    const std::vector<Receiver> receivers = CastReceivers(pose);
    const ShadowReceiverMeasurement measured = Measure(pose, receivers);
    const float turn = 3.0f * kPi / 180.0f;
    const Vector3 currentLight = Vector3{kLightDir.x * std::cos(turn) - kLightDir.z * std::sin(turn),
                                         kLightDir.y,
                                         kLightDir.x * std::sin(turn) + kLightDir.z * std::cos(turn)}
                                     .Normalize();
    ShadowMapRenderFeature feature;
    Configure(feature);
    feature.SetFitFreezeEnabled(false);
    ShadowMapRenderFeature::SDSMBounds sdsm{};
    sdsm.nearDepth = measured.NearDepth;
    sdsm.farDepth = kMaxShadowDistance;
    sdsm.valid = true;
    const CascadeFrameData fd =
        feature.ComputeCascades(MakeCamera(pose), kNear, kFar, currentLight, &sdsm, 1, nullptr, &measured);
    ExpectReceiversInside(fd, receivers, 32.0f, feature.GetConfig().Resolution);

    ShadowMapRenderFeature sliced;
    Configure(sliced);
    sliced.SetFitFreezeEnabled(false);
    const CascadeFrameData slice =
        sliced.ComputeCascades(MakeCamera(pose), kNear, kFar, currentLight, &sdsm, 1);
    EXPECT_LT(fd.OrthoHalfExtent[3], slice.OrthoHalfExtent[3])
        << "the carried measurement still narrows the box";
}

TEST(CascadeReceiverFit, ACascadeNoMeasuredRayReachesKeepsItsSliceBox)
{
    // A wall across the whole view 30 m ahead: every measured ray ends on it,
    // so nothing the measurement saw, surface or air, reaches the last
    // cascade past it, which keeps its slice box; the cascade the wall stands
    // in narrows to the wall and the air in front of it.
    constexpr float kWallDepth = 30.0f;
    const Pose pose = RtsPose(Vector3{0.0f, 0.0f, 20.0f});
    std::vector<Receiver> wall;
    for (uint32_t y = 0; y < kGridH; ++y)
    {
        for (uint32_t x = 0; x < kGridW; ++x)
        {
            const Vector3 dir = GridRay(pose, Lens{}, x, y);
            const float ndcX = (static_cast<float>(x) + 0.5f) / kGridW * 2.0f - 1.0f;
            wall.push_back(Receiver{pose.Position + dir * (kWallDepth / Vector3::Dot(dir, pose.Forward)), kWallDepth, ndcX});
        }
    }
    const ShadowReceiverMeasurement measured = Measure(pose, wall);
    ShadowMapRenderFeature fitted;
    Configure(fitted);
    fitted.SetFitFreezeEnabled(false);
    const CascadeFrameData fd =
        fitted.ComputeCascades(MakeCamera(pose), kNear, kFar, kLightDir, nullptr, 1, nullptr, &measured);
    ShadowMapRenderFeature sliced;
    Configure(sliced);
    sliced.SetFitFreezeEnabled(false);
    const CascadeFrameData slice = sliced.ComputeCascades(MakeCamera(pose), kNear, kFar, kLightDir, nullptr, 1);

    float lo = 0.0f;
    float hi = 0.0f;
    CascadeDepthWindow(fd, 3, lo, hi);
    ASSERT_GT(lo, kWallDepth) << "the last cascade must start past the wall";
    EXPECT_EQ(std::memcmp(fd.LightVPRel[3].Data(), slice.LightVPRel[3].Data(), 16 * sizeof(float)), 0);
    CascadeDepthWindow(fd, 2, lo, hi);
    ASSERT_TRUE(lo < kWallDepth && kWallDepth < hi) << "cascade 2 must hold the wall";
    EXPECT_LT(fd.OrthoHalfExtent[2], slice.OrthoHalfExtent[2]);
}

TEST(CascadeReceiverFit, ACascadeBoxHoldsTheReceiversInTheBlendBandBeforeIt)
{
    // A fragment in the blend band before a split also samples the next
    // cascade. On open ground the receivers just past the split sit beside
    // the band's in light space and would cover them anyway, so the band is
    // measured on the left of the screen and the cascade's own receivers on
    // the right. Only the band receivers in a depth bin wholly before the
    // split's bin count: the split's own bin brings the rest in regardless.
    const Pose pose = RtsPose(Vector3{0.0f, 0.0f, 20.0f});
    const Rendering::CameraData cam = MakeCamera(pose);
    const std::vector<Receiver> all = CastReceivers(pose);
    const ShadowReceiverMeasurement full = Measure(pose, all);
    ShadowMapRenderFeature::SDSMBounds sdsm{};
    sdsm.nearDepth = full.NearDepth;
    sdsm.farDepth = kMaxShadowDistance;
    sdsm.valid = true;

    ShadowMapRenderFeature probe;
    Configure(probe);
    probe.SetFitFreezeEnabled(false);
    const CascadeFrameData splits = probe.ComputeCascades(cam, kNear, kFar, kLightDir, &sdsm, 1, nullptr, &full);
    const float binsPerLog =
        Renderer::ShadowReceiverBinsPerLogUnit(full.Measured.BinNear, full.Measured.BinFar);
    auto binOf = [&](float depth)
    { return static_cast<int>(std::log(std::max(depth, full.Measured.BinNear) / full.Measured.BinNear) * binsPerLog); };

    // The cascade whose band holds the most receivers in earlier bins.
    uint32_t c = 0;
    std::vector<Receiver> band;
    std::vector<Receiver> own;
    for (uint32_t candidate = 1; candidate < splits.NumCascades; ++candidate)
    {
        float lo = 0.0f;
        float hi = 0.0f;
        CascadeDepthWindow(splits, candidate, lo, hi);
        const float split = splits.SplitDistances[candidate - 1];
        std::vector<Receiver> candidateBand;
        std::vector<Receiver> candidateOwn;
        for (const Receiver& r : all)
        {
            if (r.ViewDepth >= lo && binOf(r.ViewDepth) < binOf(split) && r.ScreenX < -0.5f)
                candidateBand.push_back(r);
            else if (r.ViewDepth >= split && r.ViewDepth <= hi && r.ScreenX > 0.5f)
                candidateOwn.push_back(r);
        }
        if (candidateBand.size() > band.size() && !candidateOwn.empty())
        {
            c = candidate;
            band = std::move(candidateBand);
            own = std::move(candidateOwn);
        }
    }
    ASSERT_GT(c, 0u) << "no cascade's band reaches a bin before its split at this pose";
    SCOPED_TRACE(::testing::Message() << "cascade " << c << ", " << band.size() << " band receivers");

    auto outsideCascade = [&](const std::vector<Receiver>& measuredSet)
    {
        ShadowMapRenderFeature feature;
        Configure(feature);
        feature.SetFitFreezeEnabled(false);
        const ShadowReceiverMeasurement measured = Measure(pose, measuredSet);
        const CascadeFrameData fd = feature.ComputeCascades(cam, kNear, kFar, kLightDir, &sdsm, 1, nullptr, &measured);
        EXPECT_EQ(fd.SplitDistances[c - 1], splits.SplitDistances[c - 1]);
        uint32_t outside = 0;
        for (const Receiver& r : band)
            outside += MaxLightNdc(fd, c, r.Position) > 1.0f ? 1u : 0u;
        return outside;
    };
    // The test has teeth: a box fitted to the cascade's own receivers alone
    // leaves the band receivers out.
    std::vector<Receiver> both = own;
    both.insert(both.end(), band.begin(), band.end());
    EXPECT_GT(outsideCascade(own), 0u);
    EXPECT_EQ(outsideCascade(both), 0u);
}

TEST(CascadeReceiverFit, TheCasterFootprintHoldsTheFiltersTexelReachAroundItsReceivers)
{
    // The caster cull keeps only the casters inside each cascade's footprint,
    // so the footprint must hold everything a lookup reads around a receiver:
    // the normal offset and penumbra ceiling in world units, and a reach in
    // texels of the cascade. 20 texels: a Poisson kernel at softness 13, or
    // the PCSS search's 2-texel floor past its cap with room to spare. The
    // receivers are the middle half of the screen, so the footprint is wide
    // enough (tens of metres) for the texel reach to exceed the world margin,
    // and the reach around them stays inside each cascade's slice.
    const Pose pose = RtsPose(Vector3{0.0f, 0.0f, 20.0f});
    const Rendering::CameraData cam = MakeCamera(pose);
    std::vector<Receiver> middle;
    for (const Receiver& r : CastReceivers(pose))
    {
        if (std::abs(r.ScreenX) < 0.5f)
            middle.push_back(r);
    }
    const ShadowReceiverMeasurement measured = Measure(pose, middle);
    ShadowMapRenderFeature::SDSMBounds sdsm{};
    sdsm.nearDepth = measured.NearDepth;
    sdsm.farDepth = kMaxShadowDistance;
    sdsm.valid = true;

    ShadowMapRenderFeature feature;
    Configure(feature);
    feature.SetFitFreezeEnabled(false);
    const CascadeFrameData fd = feature.ComputeCascades(cam, kNear, kFar, kLightDir, &sdsm, 1, nullptr, &measured);
    ShadowMapRenderFeature sliced;
    Configure(sliced);
    sliced.SetFitFreezeEnabled(false);
    const CascadeFrameData slice = sliced.ComputeCascades(cam, kNear, kFar, kLightDir, &sdsm, 1);

    const Matrix4x4 toWorld = Transpose(ShadowMapRenderFeature::CascadeLightRotation(kLightDir));
    const Vector4 ax = toWorld.Transform(Vector4{1.0f, 0.0f, 0.0f, 0.0f});
    const Vector4 ay = toWorld.Transform(Vector4{0.0f, 1.0f, 0.0f, 0.0f});
    const Vector3 lateral[4] = {Vector3{ax.x, ax.y, ax.z}, Vector3{-ax.x, -ax.y, -ax.z},
                                Vector3{ay.x, ay.y, ay.z}, Vector3{-ay.x, -ay.y, -ay.z}};
    const float resolution = static_cast<float>(feature.GetConfig().Resolution);
    const float worldReach = std::max(feature.GetConfig().NormalBias, 0.0f) + feature.GetPcssMaxPenumbra();
    uint32_t marginDominated = 0;
    // Cascade 0 keeps its slice (Cascade0KeepsItsSliceFromTheNearestMeasuredReceiver).
    for (uint32_t c = 1; c < fd.NumCascades; ++c)
    {
        SCOPED_TRACE(::testing::Message() << "cascade " << c);
        ASSERT_TRUE(fd.CasterFootprintApplies[c]);
        const float texel = 2.0f * fd.OrthoHalfExtent[c] / resolution;
        const float reach = worldReach + 20.0f * texel;
        marginDominated += 20.0f * texel > 0.6f ? 1u : 0u;
        const float* rect = fd.CasterFootprint[c];
        const float tolerance = 2.0f / resolution;
        float lo = 0.0f;
        float hi = 0.0f;
        CascadeDepthWindow(fd, c, lo, hi);
        uint32_t checked = 0;
        uint32_t outside = 0;
        for (const Receiver& r : middle)
        {
            if (r.ViewDepth < lo || r.ViewDepth > hi)
                continue;
            for (const Vector3& dir : lateral)
            {
                const Vector3 p = r.Position + dir * reach;
                // Inside the slice with room for its own rounding: the fit
                // never reaches past the slice.
                if (MaxLightNdc(slice, c, p) > 0.65f)
                    continue;
                ++checked;
                const Vector4 clip = fd.LightVP[c].Transform(Vector4{p.x, p.y, p.z, 1.0f});
                const float x = clip.x / clip.w;
                const float y = clip.y / clip.w;
                if (x < rect[0] - tolerance || y < rect[1] - tolerance || x > rect[2] + tolerance ||
                    y > rect[3] + tolerance)
                    ++outside;
            }
        }
        EXPECT_GT(checked, 0u);
        EXPECT_EQ(outside, 0u) << "of " << checked << " points within the reach of a receiver";
    }
    // The cascades where the texel reach exceeds the half-metre motion
    // allowance are the ones that show it.
    EXPECT_GT(marginDominated, 0u);
}

namespace
{

// The caster cull planes OnScheduleCulling builds for cascade c of `fd`.
void CasterCullPlanes(const CascadeFrameData& fd, uint32_t c, Vector4 planes[6])
{
    ShadowMapRenderFeature::BuildCascadeCasterCullPlanes(fd.LightVP[c], planes);
    if (fd.CasterFootprintApplies[c])
        ShadowMapRenderFeature::TightenCascadeSidePlanes(fd.LightVP[c], fd.CasterFootprint[c],
                                                         fd.OrthoHalfExtent[c], 0.0f, planes);
}

} // namespace

TEST(CascadeReceiverFit, CastersAreCulledToTheReceiversEachCascadeServes)
{
    const Pose pose = RtsPose(Vector3{0.0f, 0.0f, 20.0f});
    const Rendering::CameraData cam = MakeCamera(pose);
    const std::vector<Receiver> receivers = CastReceivers(pose);
    const ShadowReceiverMeasurement measured = Measure(pose, receivers);
    ShadowMapRenderFeature::SDSMBounds sdsm{};
    sdsm.nearDepth = measured.NearDepth;
    sdsm.farDepth = kMaxShadowDistance;
    sdsm.valid = true;

    ShadowMapRenderFeature fitted;
    Configure(fitted);
    fitted.SetFitFreezeEnabled(false);
    const CascadeFrameData fd = fitted.ComputeCascades(cam, kNear, kFar, kLightDir, &sdsm, 1, nullptr, &measured);
    ShadowMapRenderFeature sliced;
    Configure(sliced);
    sliced.SetFitFreezeEnabled(false);
    const CascadeFrameData slice = sliced.ComputeCascades(cam, kNear, kFar, kLightDir, &sdsm, 1);

    for (uint32_t c = 0; c < fd.NumCascades; ++c)
    {
        SCOPED_TRACE(::testing::Message() << "cascade " << c);
        // Cascade 0 keeps its slice and its full set of casters.
        EXPECT_EQ(fd.CasterFootprintApplies[c], c > 0) << "a cascade fitted to receivers culls to them";
        Vector4 planes[6]{};
        CasterCullPlanes(fd, c, planes);

        // Every caster that can shadow a receiver the cascade serves sits in
        // that receiver's light column, at any height toward the light.
        float lo = 0.0f;
        float hi = 0.0f;
        CascadeDepthWindow(fd, c, lo, hi);
        uint32_t culledShadowing = 0;
        for (const Receiver& r : receivers)
        {
            if (r.ViewDepth < lo || r.ViewDepth > hi)
                continue;
            for (const float lift : {0.0f, 6.0f, 25.0f})
            {
                if (!Rendering::TestSphereFrustum(r.Position - kLightDir * lift, 0.25f, planes))
                    ++culledShadowing;
            }
        }
        EXPECT_EQ(culledShadowing, 0u);
    }

    // The slice's box reaches far under the ground; casters there shadow
    // nothing on screen. The slice planes keep them, the receiver planes do not.
    Vector4 receiverPlanes[6]{};
    Vector4 slicePlanes[6]{};
    CasterCullPlanes(fd, 3, receiverPlanes);
    CasterCullPlanes(slice, 3, slicePlanes);
    uint32_t keptBySlice = 0;
    uint32_t culledByReceivers = 0;
    for (float x = -200.0f; x <= 200.0f; x += 10.0f)
    {
        for (float z = -100.0f; z <= 300.0f; z += 10.0f)
        {
            const Vector3 buried{x, -40.0f, z};
            if (!Rendering::TestSphereFrustum(buried, 0.25f, slicePlanes))
                continue;
            ++keptBySlice;
            culledByReceivers += Rendering::TestSphereFrustum(buried, 0.25f, receiverPlanes) ? 0u : 1u;
        }
    }
    ASSERT_GT(keptBySlice, 0u);
    EXPECT_GT(culledByReceivers, keptBySlice / 2) << "of " << keptBySlice << " buried casters";
}

TEST(CascadeReceiverFit, Cascade0KeepsItsSliceFromTheNearestMeasuredReceiver)
{
    // Cascade 0 starts at the nearest measured surface but keeps the box of
    // that slice, plus the measured air in front of it: narrowed to the
    // surfaces, the smallest box would change its texel size with the lagging
    // measurement as the camera moves. Its fit is the same with the surface
    // bins as without them, the nearest depth and the rays alone.
    const Pose pose = RtsPose(Vector3{0.0f, 0.0f, 20.0f});
    const Rendering::CameraData cam = MakeCamera(pose);
    const ShadowReceiverMeasurement measured = Measure(pose, CastReceivers(pose));
    ShadowReceiverMeasurement nearOnly = measured;
    nearOnly.Bins.fill(AABB::Empty());
    ShadowMapRenderFeature::SDSMBounds sdsm{};
    sdsm.nearDepth = measured.NearDepth;
    sdsm.farDepth = kMaxShadowDistance;
    sdsm.valid = true;
    ShadowMapRenderFeature binned;
    Configure(binned);
    binned.SetFitFreezeEnabled(false);
    const CascadeFrameData fd = binned.ComputeCascades(cam, kNear, kFar, kLightDir, &sdsm, 1, nullptr, &measured);
    ShadowMapRenderFeature unbinned;
    Configure(unbinned);
    unbinned.SetFitFreezeEnabled(false);
    const CascadeFrameData slice = unbinned.ComputeCascades(cam, kNear, kFar, kLightDir, &sdsm, 1, nullptr, &nearOnly);
    EXPECT_EQ(std::memcmp(fd.LightVPRel[0].Data(), slice.LightVPRel[0].Data(), 16 * sizeof(float)), 0);
    EXPECT_FALSE(fd.CasterFootprintApplies[0]);
    // The other cascades do narrow, against a fit with no measurement.
    ShadowMapRenderFeature unmeasured;
    Configure(unmeasured);
    unmeasured.SetFitFreezeEnabled(false);
    EXPECT_LT(fd.OrthoHalfExtent[3],
              unmeasured.ComputeCascades(cam, kNear, kFar, kLightDir, &sdsm, 1).OrthoHalfExtent[3]);
}

TEST(CascadeReceiverFit, Cascade0OnTheSliceKeepsItsFullBox)
{
    // Without a measurement cascade 0 cannot tell where its receivers are.
    const Pose pose = RtsPose(Vector3{0.0f, 0.0f, 20.0f});
    ShadowMapRenderFeature feature;
    Configure(feature);
    const CascadeFrameData fd = feature.ComputeCascades(MakeCamera(pose), kNear, kFar, kLightDir, nullptr, 1);
    EXPECT_FALSE(fd.CasterFootprintApplies[0]);
    for (uint32_t c = 1; c < fd.NumCascades; ++c)
        EXPECT_TRUE(fd.CasterFootprintApplies[c]) << "cascade " << c;
}

namespace
{

// Receivers the shader selects into some cascade of `fd` that project outside
// that cascade's box.
uint32_t CountReceiversOutside(const CascadeFrameData& fd, const std::vector<Receiver>& receivers)
{
    uint32_t outside = 0;
    for (uint32_t c = 0; c < fd.NumCascades; ++c)
    {
        float lo = 0.0f;
        float hi = 0.0f;
        CascadeDepthWindow(fd, c, lo, hi);
        for (const Receiver& r : receivers)
        {
            if (r.ViewDepth >= lo && r.ViewDepth <= hi && MaxLightNdc(fd, c, r.Position) > 1.0f)
                ++outside;
        }
    }
    return outside;
}

} // namespace

TEST(CascadeReceiverFit, AMeasurementThroughAnotherWindowBoundsNoReceiver)
{
    // A wider field of view or a resized viewport, at the same pose, reveals
    // receivers past the measured frustum's edges: at the pitched camera the
    // top edge reaches tens of metres further over the ground, nowhere near a
    // measured receiver. Until a measurement through the new window lands,
    // the cascades fit their slices, as with no measurement.
    const Pose pose = RtsPose(Vector3{0.0f, 0.0f, 20.0f});
    const ShadowReceiverMeasurement measured = Measure(pose, CastReceivers(pose));
    ShadowMapRenderFeature::SDSMBounds sdsm{};
    sdsm.nearDepth = measured.NearDepth;
    sdsm.farDepth = kMaxShadowDistance;
    sdsm.valid = true;
    for (const Lens& lens : {Lens{53.0f, kAspect, 0.0f}, Lens{kFovYDeg, 16.0f / 9.0f, 0.0f}})
    {
        SCOPED_TRACE(::testing::Message() << "field of view " << lens.FovYDeg << ", aspect " << lens.Aspect);
        const Rendering::CameraData cam = MakeCamera(pose, lens);
        const std::vector<Receiver> receivers = CastReceivers(pose, lens);
        auto fit = [&](const ShadowReceiverMeasurement* m)
        {
            ShadowMapRenderFeature feature;
            Configure(feature);
            feature.SetFitFreezeEnabled(false);
            return feature.ComputeCascades(cam, kNear, kFar, kLightDir, &sdsm, 1, nullptr, m);
        };
        const CascadeFrameData fd = fit(&measured);
        const CascadeFrameData slice = fit(nullptr);
        for (uint32_t c = 0; c < fd.NumCascades; ++c)
            EXPECT_EQ(std::memcmp(fd.LightVPRel[c].Data(), slice.LightVPRel[c].Data(), 16 * sizeof(float)), 0)
                << "cascade " << c;
        EXPECT_EQ(CountReceiversOutside(fd, receivers), 0u);

        // The test has teeth: the same surface bins, taken as if measured
        // through this window, leave the revealed receivers out. Without the
        // rays: the air they would add can hold the revealed receivers by
        // chance, which is no measurement of them either.
        ShadowReceiverMeasurement sameWindow = measured;
        sameWindow.Rays.fill(AABB::Empty());
        const ShadowReceiverMeasurement::Context now = CameraContext(pose, lens);
        sameWindow.Measured.WindowHalfX = now.WindowHalfX;
        sameWindow.Measured.WindowHalfY = now.WindowHalfY;
        EXPECT_GT(CountReceiversOutside(fit(&sameWindow), receivers), 0u);
    }
}

TEST(CascadeReceiverFit, TheWindowCheckSeesTheFieldOfViewAspectOrthographicSizeAndProjectionKind)
{
    const Pose pose = RtsPose(Vector3{0.0f, 0.0f, 20.0f});
    const ShadowReceiverMeasurement::Context base = CameraContext(pose);
    // Moving, turning and rolling keep the window; the motion bound covers them.
    EXPECT_TRUE(Renderer::ShadowReceiverSameWindow(
        base, CameraContext(Pose{pose.Position + Vector3{3.0f, 0.0f, 1.0f}, pose.Forward})));
    EXPECT_TRUE(Renderer::ShadowReceiverSameWindow(base, CameraContext(pose, Lens{kFovYDeg, kAspect, 8.0f})));
    EXPECT_FALSE(Renderer::ShadowReceiverSameWindow(base, CameraContext(pose, Lens{46.0f, kAspect, 0.0f})));
    EXPECT_FALSE(
        Renderer::ShadowReceiverSameWindow(base, CameraContext(pose, Lens{kFovYDeg, 16.0f / 9.0f, 0.0f})));

    auto orthographic = [&](float halfHeight)
    {
        Rendering::CameraData cam = MakeCamera(pose);
        const Matrix4x4 proj = MakeOrthographicLH_ZO_ReverseZ(-halfHeight * kAspect, halfHeight * kAspect,
                                                              -halfHeight, halfHeight, kNear, kFar);
        std::memcpy(cam.proj, proj.Data(), sizeof(cam.proj));
        ShadowReceiverMeasurement::Context c{};
        Renderer::SetShadowReceiverCamera(cam, true, c);
        return c;
    };
    EXPECT_TRUE(Renderer::ShadowReceiverSameWindow(orthographic(30.0f), orthographic(30.0f)));
    EXPECT_FALSE(Renderer::ShadowReceiverSameWindow(orthographic(30.0f), orthographic(32.0f)));
    ShadowReceiverMeasurement::Context orthographicBase = orthographic(30.0f);
    orthographicBase.WindowHalfX = base.WindowHalfX;
    orthographicBase.WindowHalfY = base.WindowHalfY;
    EXPECT_FALSE(Renderer::ShadowReceiverSameWindow(base, orthographicBase))
        << "the same half-extents mean different windows per unit depth and in world units";
}

TEST(CascadeReceiverFit, BoxesHoldTheReceiversRevealedByCameraRollSinceTheMeasurement)
{
    // A roll about the view axis leaves the forward where it was and swings
    // the window's corners through the ray length times the roll.
    const Pose pose = RtsPose(Vector3{0.0f, 0.0f, 20.0f});
    const ShadowReceiverMeasurement measured = Measure(pose, CastReceivers(pose));
    ShadowMapRenderFeature::SDSMBounds sdsm{};
    sdsm.nearDepth = measured.NearDepth;
    sdsm.farDepth = kMaxShadowDistance;
    sdsm.valid = true;
    for (const float rollDeg : {3.0f, 8.0f})
    {
        SCOPED_TRACE(::testing::Message() << "roll " << rollDeg << " degrees");
        const Lens rolled{kFovYDeg, kAspect, rollDeg};
        ShadowMapRenderFeature feature;
        Configure(feature);
        feature.SetFitFreezeEnabled(false);
        const CascadeFrameData fd =
            feature.ComputeCascades(MakeCamera(pose, rolled), kNear, kFar, kLightDir, &sdsm, 1, nullptr, &measured);
        ExpectReceiversInside(fd, CastReceivers(pose, rolled), 32.0f, feature.GetConfig().Resolution);
    }
}

TEST(CascadeReceiverFit, EveryCascadeCullsCastersWithinAReceiversReachOfItsSliceEdge)
{
    // Receivers can stand on a cascade's slice box edge, and their lookups
    // read past it. A cascade that keeps its slice box (nothing measured, or
    // the Stable projection) culls its casters to that box widened by the
    // reach; one narrowed to measured receivers culls them to those receivers
    // widened by the reach, not clipped to the slice. The sun stands in the
    // vertical plane of the screen's lower-left corner ray, so every point on
    // that ray (a wall or a unit can stand there) lies on the slice box's edge
    // in light space.
    const Pose pose = RtsPose(Vector3{0.0f, 0.0f, 20.0f});
    const Rendering::CameraData cam = MakeCamera(pose);
    const Vector3 up = LensUp(pose, Lens{});
    const Vector3 right = Vector3::Cross(up, pose.Forward).Normalize();
    const float tanHalfY = std::tan(kFovYDeg * 0.5f * kPi / 180.0f);
    const float tanHalfX = tanHalfY * kAspect;
    const Vector3 lowerLeft = pose.Forward - right * tanHalfX - up * tanHalfY;
    const Vector3 lightDir =
        (Vector3{lowerLeft.x, 0.0f, lowerLeft.z}.Normalize() * 0.6f + Vector3{0.0f, -0.8f, 0.0f}).Normalize();
    const Matrix4x4 toWorld = Transpose(ShadowMapRenderFeature::CascadeLightRotation(lightDir));
    const Vector4 ax = toWorld.Transform(Vector4{1.0f, 0.0f, 0.0f, 0.0f});
    const Vector4 ay = toWorld.Transform(Vector4{0.0f, 1.0f, 0.0f, 0.0f});
    const Vector3 lateral[4] = {Vector3{ax.x, ax.y, ax.z}, Vector3{-ax.x, -ax.y, -ax.z},
                                Vector3{ay.x, ay.y, ay.z}, Vector3{-ay.x, -ay.y, -ay.z}};

    // The measured arm sees the ground and points all along the corner ray.
    std::vector<Receiver> seen = CastReceivers(pose);
    for (int i = 1; i < 64; ++i)
    {
        const float depth = kMaxShadowDistance * static_cast<float>(i) / 64.0f;
        seen.push_back(Receiver{pose.Position + lowerLeft * depth, depth, -1.0f});
    }
    const ShadowReceiverMeasurement measured = Measure(pose, seen, Lens{}, lightDir);

    struct Arm
    {
        Renderer::ShadowProjection Projection;
        const ShadowReceiverMeasurement* Measured;
    };
    for (const Arm arm : {Arm{Renderer::ShadowProjection::Close, &measured},
                          Arm{Renderer::ShadowProjection::Close, nullptr},
                          Arm{Renderer::ShadowProjection::Stable, nullptr}})
    {
        SCOPED_TRACE(::testing::Message() << "projection " << static_cast<int>(arm.Projection)
                                          << (arm.Measured ? ", measured" : ", nothing measured"));
        ShadowMapRenderFeature feature;
        Configure(feature);
        feature.SetShadowProjection(arm.Projection);
        feature.SetFitFreezeEnabled(false);
        const CascadeFrameData fd =
            feature.ComputeCascades(cam, kNear, kFar, lightDir, nullptr, 1, nullptr, arm.Measured);
        const float resolution = static_cast<float>(feature.GetConfig().Resolution);
        const float worldReach = std::max(feature.GetConfig().NormalBias, 0.0f) + feature.GetPcssMaxPenumbra();
        for (uint32_t c = 1; c < fd.NumCascades; ++c)
        {
            SCOPED_TRACE(::testing::Message() << "cascade " << c);
            ASSERT_TRUE(fd.CasterFootprintApplies[c]);
            const float reach = worldReach + 20.0f * 2.0f * fd.OrthoHalfExtent[c] / resolution;
            const float* rect = fd.CasterFootprint[c];
            const float tolerance = 2.0f / resolution;
            uint32_t outside = 0;
            // The cascade's own depth range along the corner ray.
            const float lo = fd.SplitDistances[c - 1];
            const float hi = c + 1 == fd.NumCascades ? kMaxShadowDistance : fd.SplitDistances[c];
            for (int step = 0; step <= 8; ++step)
            {
                const float depth = lo + (hi - lo) * static_cast<float>(step) / 8.0f;
                const Vector3 receiver = pose.Position + lowerLeft * depth;
                for (const Vector3& dir : lateral)
                {
                    const Vector3 p = receiver + dir * reach;
                    const Vector4 clip = fd.LightVP[c].Transform(Vector4{p.x, p.y, p.z, 1.0f});
                    const float x = clip.x / clip.w;
                    const float y = clip.y / clip.w;
                    if (x < rect[0] - tolerance || y < rect[1] - tolerance || x > rect[2] + tolerance ||
                        y > rect[3] + tolerance)
                        ++outside;
                }
            }
            EXPECT_EQ(outside, 0u) << "of 36 points within the reach of a receiver on the corner ray";
        }
    }
}

namespace
{

// A point a cascade lookup samples off the depth buffer: the air in front of
// the visible surfaces, where volumetric fog's froxels and transparent
// surfaces look the shadow up.
struct AirPoint
{
    Vector3 Position;
    float ViewDepth = 0.0f;
};

// Points every `spacing` metres along every other grid ray, from the near
// plane to the ray's first surface or, through the sky, to the shadow distance.
std::vector<AirPoint> CastAir(const Pose& pose, const Lens& lens = {}, float spacing = 2.0f)
{
    std::vector<AirPoint> out;
    for (uint32_t y = 0; y < kGridH; y += 2)
    {
        for (uint32_t x = 0; x < kGridW; x += 2)
        {
            const Vector3 origin = GridOrigin(pose, lens, x, y);
            const Vector3 dir = GridRay(pose, lens, x, y);
            const float perDepth = 1.0f / Vector3::Dot(dir, pose.Forward);
            const float end = std::min(FirstSurfaceDistance(origin, dir), kMaxShadowDistance * perDepth);
            for (float t = kNear * perDepth; t < end; t += spacing)
                out.push_back(AirPoint{origin + dir * t, t / perDepth});
        }
    }
    return out;
}

// True when `p` lies inside cascade c's box and depth range: a lookup there
// reads the map; outside it reads lit.
bool CascadeHolds(const CascadeFrameData& fd, uint32_t c, const Vector3& p)
{
    const Vector4 clip = fd.LightVPRel[c].Transform(Vector4{p.x, p.y, p.z, 1.0f});
    const float z = clip.z / clip.w;
    return MaxLightNdc(fd, c, p) <= 1.0f && z >= 0.0f && z <= 1.0f;
}

// Air points the shader selects into a cascade (its depth window, the blend
// band before it included) that the cascade does not hold; `checked` counts
// the selections.
uint32_t CountAirOutside(const CascadeFrameData& fd, const std::vector<AirPoint>& air, uint32_t& checked)
{
    checked = 0;
    uint32_t outside = 0;
    for (uint32_t c = 0; c < fd.NumCascades; ++c)
    {
        float lo = 0.0f;
        float hi = 0.0f;
        CascadeDepthWindow(fd, c, lo, hi);
        for (const AirPoint& a : air)
        {
            if (a.ViewDepth < lo || a.ViewDepth > hi)
                continue;
            ++checked;
            outside += CascadeHolds(fd, c, a.Position) ? 0u : 1u;
        }
    }
    return outside;
}

} // namespace

TEST(CascadeReceiverFit, BoxesHoldTheAirInFrontOfTheReceivers)
{
    // Volumetric fog and transparent surfaces look the cascades up in the air
    // in front of the depth buffer, the sun glare at the eye, and a lookup
    // outside its cascade's box reads lit. At the strategy camera and at eye
    // level, the camera a first- or third-person game uses, under the test's
    // sun and a low sun ahead of and behind the camera; after the camera
    // walked and turned since the measurement; and through an orthographic
    // window, where the rays are parallel: at the strategy camera, and from a
    // shallow boom whose upper rows reach ground past the shadow distance, so
    // their air runs out to it with no surface to bound it.
    const Vector3 lowAhead = Vector3{0.15f, -0.25f, -0.95f}.Normalize();
    const Vector3 lowBehind = Vector3{0.15f, -0.25f, 0.95f}.Normalize();
    const Pose rts = RtsPose(Vector3{0.0f, 0.0f, 20.0f});
    const Pose eye{Vector3{0.0f, 1.7f, -10.0f}, Vector3{0.0f, 0.0f, 1.0f}};
    const float turn = 3.0f * kPi / 180.0f;
    const Pose walked{Vector3{0.3f, 1.7f, -9.0f}, Vector3{std::sin(turn), 0.0f, std::cos(turn)}};
    const Lens orthographic{kFovYDeg, kAspect, 0.0f, 30.0f};
    const Pose shallow = RtsPose(Vector3{0.0f, 0.0f, 20.0f}, 200.0f, 10.0f);
    const Lens shallowOrthographic{kFovYDeg, kAspect, 0.0f, 20.0f};
    struct Case
    {
        const char* Name;
        Pose Measured;
        Pose View;
        Vector3 Light;
        Lens ViewLens{};
    };
    for (const Case& test :
         {Case{"strategy, test sun", rts, rts, kLightDir}, Case{"strategy, low sun ahead", rts, rts, lowAhead},
          Case{"eye level, test sun", eye, eye, kLightDir}, Case{"eye level, low sun ahead", eye, eye, lowAhead},
          Case{"eye level, low sun behind", eye, eye, lowBehind},
          Case{"eye level, low sun ahead, walked and turned", eye, walked, lowAhead},
          Case{"strategy, orthographic, test sun", rts, rts, kLightDir, orthographic},
          Case{"strategy, orthographic, low sun ahead", rts, rts, lowAhead, orthographic},
          Case{"shallow, orthographic, test sun", shallow, shallow, kLightDir, shallowOrthographic}})
    {
        SCOPED_TRACE(test.Name);
        const ShadowReceiverMeasurement measured =
            Measure(test.Measured, CastReceivers(test.Measured, test.ViewLens), test.ViewLens, test.Light);
        ShadowMapRenderFeature::SDSMBounds sdsm{};
        sdsm.nearDepth = measured.NearDepth;
        sdsm.farDepth = kMaxShadowDistance;
        sdsm.valid = true;
        ShadowMapRenderFeature feature;
        Configure(feature);
        feature.SetFitFreezeEnabled(false);
        const CascadeFrameData fd = feature.ComputeCascades(MakeCamera(test.View, test.ViewLens), kNear, kFar,
                                                            test.Light, &sdsm, 1, nullptr, &measured);
        uint32_t checked = 0;
        const uint32_t outside = CountAirOutside(fd, CastAir(test.View, test.ViewLens), checked);
        ASSERT_GT(checked, 0u);
        EXPECT_EQ(outside, 0u) << outside << " of " << checked << " air lookups outside their cascade";
        bool eyeHeld = false;
        for (uint32_t c = 0; c < fd.NumCascades; ++c)
            eyeHeld = eyeHeld || CascadeHolds(fd, c, test.View.Position);
        EXPECT_TRUE(eyeHeld) << "no cascade holds the eye";
    }
}
