#include "Engine/Rendering/ShadowReceiverMeasurement.h"

#include "Engine/Rendering/CameraUtils.h"
#include "Mathematics/MatrixOps.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace GameEngine
{
namespace Engine::Renderer
{

using Mathematics::AABB;
using Mathematics::Vector3;

namespace
{

// The sentinels the reduce pass fills its result with before the dispatch: an
// untouched word means no sample reached it.
constexpr uint32_t kMinNeutral = 0xFFFFFFFFu;
constexpr uint32_t kMaxNeutral = 0u;

// Inverse of the reduce's EncodeOrdered: the float whose order-preserving
// encoding is `word`.
float DecodeOrdered(uint32_t word)
{
    const uint32_t bits = (word & 0x80000000u) != 0u ? (word & 0x7FFFFFFFu) : ~word;
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

float FloatFromBits(uint32_t word)
{
    float value = 0.0f;
    std::memcpy(&value, &word, sizeof(value));
    return value;
}

// The box x, y, z of the three words at `base` on both sides; Empty() when no
// sample reached them.
AABB DecodeBox(const ShadowReceiverReadback& readback, uint32_t base)
{
    if (readback.MinWords[base] == kMinNeutral)
        return AABB::Empty();
    return AABB{Vector3{DecodeOrdered(readback.MinWords[base]), DecodeOrdered(readback.MinWords[base + 1]),
                        DecodeOrdered(readback.MinWords[base + 2])},
                Vector3{DecodeOrdered(readback.MaxWords[base]), DecodeOrdered(readback.MaxWords[base + 1]),
                        DecodeOrdered(readback.MaxWords[base + 2])}};
}

} // namespace

float ShadowReceiverMeasurement::Context::RayLength(float depth) const
{
    const float halfDiagonal = std::sqrt(WindowHalfX * WindowHalfX + WindowHalfY * WindowHalfY);
    const float z = std::max(depth, 0.0f);
    return Orthographic ? z + halfDiagonal : z * std::sqrt(1.0f + halfDiagonal * halfDiagonal);
}

float ShadowReceiverMeasurement::BinNearDepth(uint32_t bin) const
{
    const float t = static_cast<float>(bin) / static_cast<float>(kDepthBins);
    return Measured.BinNear * std::pow(Measured.BinFar / Measured.BinNear, t);
}

float ShadowReceiverMeasurement::BinFarDepth(uint32_t bin) const
{
    return BinNearDepth(bin + 1);
}

AABB ShadowReceiverMeasurement::RayPointsAt(const AABB& rays, float depth) const
{
    if (rays.IsEmpty())
        return rays;
    const Vector3& camera = Measured.CameraLightSpace;
    if (Measured.Orthographic)
    {
        const Vector3 along = Measured.ForwardLightSpace * depth;
        return AABB{camera + along + rays.min, camera + along + rays.max};
    }
    // depth >= 0, so the box scales without swapping its bounds.
    return AABB{camera + rays.min * depth, camera + rays.max * depth};
}

AABB ShadowReceiverMeasurement::AirInBin(uint32_t bin, const AABB& reaching, const AABB& passing) const
{
    // Along each ray the points are affine in view depth, so the air of the bin
    // lies between its near face over the rays that reach the bin and its far
    // face over the rays that pass it; a ray that ends in the bin ends on a
    // surface Bins[bin] already holds.
    const float nearDepth = bin == 0 ? 0.0f : BinNearDepth(bin);
    AABB air = RayPointsAt(reaching, nearDepth);
    air.Expand(RayPointsAt(passing, BinFarDepth(bin)));
    return air;
}

float ShadowReceiverBinsPerLogUnit(float binNear, float binFar)
{
    return static_cast<float>(ShadowReceiverMeasurement::kDepthBins) /
           std::log(std::max(binFar / binNear, 1.0f + 1e-6f));
}

bool DecodeShadowReceiverResult(const ShadowReceiverReadback& readback,
                                ShadowReceiverMeasurement& out)
{
    out = ShadowReceiverMeasurement{};
    const ShadowReceiverMeasurement::Context& context = readback.Measured;
    if (readback.MinWords[0] == kMinNeutral || readback.MaxWords[0] == kMaxNeutral)
        return false;
    if (!(context.NearPlane > 0.0f) || !(context.FarPlane > context.NearPlane) ||
        !(context.BinNear > 0.0f) || !(context.BinFar > context.BinNear))
        return false;

    out.Measured = context;
    // Reverse-Z: the largest depth word is the NEAREST receiver.
    out.NearDepth = LinearizeReverseZDepthLH_ZO(FloatFromBits(readback.MaxWords[0]),
                                                context.NearPlane, context.FarPlane,
                                                context.Orthographic);
    if (!std::isfinite(out.NearDepth))
        return false;
    constexpr uint32_t kRaysBase = 1 + 3 * ShadowReceiverMeasurement::kDepthBins;
    for (uint32_t bin = 0; bin < ShadowReceiverMeasurement::kDepthBins; ++bin)
        out.Bins[bin] = DecodeBox(readback, 1 + 3 * bin);
    for (uint32_t record = 0; record < ShadowReceiverMeasurement::kRayRecords; ++record)
        out.Rays[record] = DecodeBox(readback, kRaysBase + 3 * record);
    out.Valid = true;
    return true;
}

void SetShadowReceiverCamera(const Rendering::CameraData& camera, bool orthographic,
                             ShadowReceiverMeasurement::Context& context)
{
    // The view basis is the inverse view's rotation (left-handed, +Z forward,
    // as DeriveCameraData reads it).
    Mathematics::Matrix4x4 view;
    std::memcpy(view.Data(), camera.view, sizeof(camera.view));
    const Mathematics::Matrix4x4 worldFromCamera = Mathematics::Inverse(view);
    auto axis = [&worldFromCamera](float x, float y, float z)
    {
        const Mathematics::Vector4 a = worldFromCamera.Transform(Mathematics::Vector4{x, y, z, 0.0f});
        return Vector3{a.x, a.y, a.z}.Normalize();
    };
    context.CameraPosition = worldFromCamera.TransformPoint(Vector3{0.0f, 0.0f, 0.0f});
    context.CameraRight = axis(1.0f, 0.0f, 0.0f);
    context.CameraUp = axis(0.0f, 1.0f, 0.0f);
    context.CameraForward = axis(0.0f, 0.0f, 1.0f);
    // proj[0] and proj[5] are the reciprocal half-extents of the window: per
    // unit depth under a perspective projection, absolute under an orthographic one.
    context.WindowHalfX = 1.0f / std::max(std::abs(camera.proj[0]), 1e-6f);
    context.WindowHalfY = 1.0f / std::max(std::abs(camera.proj[5]), 1e-6f);
    context.Orthographic = orthographic;
}

bool ShadowReceiverSameWindow(const ShadowReceiverMeasurement::Context& measured,
                              const ShadowReceiverMeasurement::Context& current)
{
    return measured.Orthographic == current.Orthographic &&
           std::memcmp(&measured.WindowHalfX, &current.WindowHalfX, sizeof(float)) == 0 &&
           std::memcmp(&measured.WindowHalfY, &current.WindowHalfY, sizeof(float)) == 0;
}

float ShadowReceiverMotionBound(const ShadowReceiverMeasurement& measurement, float depth,
                                const ShadowReceiverMeasurement::Context& current)
{
    // A receiver p at view depth z lies within RayLength(z) of the measuring
    // camera c. Through the same window, a point of the current frustum
    // c' + R' w and the measured frustum's point at the same window position
    // and depth, c + R w, differ by |c' - c| + |(R' - R) w| with |w| <=
    // RayLength(z); the same bound holds a measured receiver's change in view
    // depth, dot(p - c', f') - dot(p - c, f). |(R' - R) w| <= |R' - R|_2 |w|,
    // and for two rotations |R' - R|_2 = |R' - R|_F / sqrt(2) = 2 sin(angle / 2)
    // of the rotation between them: the forward alone misses a roll. A
    // receiver revealed since lies that close to the surface the measured
    // frustum saw.
    const ShadowReceiverMeasurement::Context& measured = measurement.Measured;
    const float travel = (current.CameraPosition - measured.CameraPosition).Length();
    const Vector3 dRight = current.CameraRight - measured.CameraRight;
    const Vector3 dUp = current.CameraUp - measured.CameraUp;
    const Vector3 dForward = current.CameraForward - measured.CameraForward;
    const float rotation = std::sqrt(0.5f * (Vector3::Dot(dRight, dRight) + Vector3::Dot(dUp, dUp) +
                                             Vector3::Dot(dForward, dForward)));
    return travel + measured.RayLength(depth) * rotation;
}

AABB GatherShadowReceivers(const ShadowReceiverMeasurement& measurement, float depthLo,
                           float depthHi, const ShadowReceiverMeasurement::Context& current,
                           const Mathematics::Matrix4x4* measuredToCurrent)
{
    AABB receivers = AABB::Empty();
    if (!measurement.Valid)
        return receivers;
    // From the last bin to the first, so `passing` accumulates the rays that
    // reach past the current bin.
    AABB passing = measurement.Rays[ShadowReceiverMeasurement::kDepthBins];
    for (uint32_t bin = ShadowReceiverMeasurement::kDepthBins; bin-- > 0;)
    {
        AABB reaching = passing;
        reaching.Expand(measurement.Rays[bin]);
        AABB measured = measurement.Bins[bin];
        measured.Expand(measurement.AirInBin(bin, reaching, passing));
        passing = reaching;
        if (measured.IsEmpty())
            continue;
        // The reduce folds depths in front of the first bin into it.
        const float binNear = bin == 0 ? 0.0f : measurement.BinNearDepth(bin);
        const float binFar = measurement.BinFarDepth(bin);
        const float motion = ShadowReceiverMotionBound(measurement, binFar, current);
        if (binFar + motion < depthLo || binNear - motion > depthHi)
            continue;
        AABB box = measuredToCurrent
                       ? Mathematics::BoundingBox::FromMinMax(measured.min, measured.max)
                             .TransformToAABB(measuredToCurrent->Data())
                       : measured;
        // The motion bound carries the air too: a point on the segment from the
        // current camera to a current receiver lies within it of the measured
        // segment's point at the same fraction of its length.
        box.Inflate(Vector3{motion, motion, motion});
        receivers.Expand(box);
    }
    return receivers;
}

} // namespace Engine::Renderer
} // namespace GameEngine
