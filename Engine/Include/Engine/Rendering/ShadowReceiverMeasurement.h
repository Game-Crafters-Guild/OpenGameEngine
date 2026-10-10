#pragma once

#include "Mathematics/Geometry.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector3.h"
#include "Rendering/CameraTypes.h"
#include "Types/Types.h"

#include <array>
#include <cstdint>

namespace GameEngine
{
namespace Engine::Renderer
{

/// What the SDSM receiver reduce (Shaders/Includes/shadow_receiver_reduce.glsl)
/// measured for one view over its final depth: the nearest visible surface, per
/// view-depth bin the light-space box of the surfaces in that bin, and per bin
/// the view rays that reach it, which bound the visible air in front of the
/// surfaces. A cascade lookup samples both: the surfaces that write depth, and
/// the air in front of them, where volumetric fog's froxels, transparent
/// surfaces and the sun glare's eye look the shadow up. The directional
/// cascades are fitted and caster-culled to these receivers rather than to the
/// camera frustum slice, much of which lies under the ground or behind the
/// visible surfaces at a pitched camera.
///
/// The result reaches the CPU a few frames after the depth it measured, so it
/// carries the camera and light it was measured with (Context); every use
/// widens it by how far the camera has moved and turned since
/// (ShadowReceiverMotionBound), and none uses it once the camera's window
/// changed (ShadowReceiverSameWindow).
struct ShadowReceiverMeasurement
{
    /// Log-spaced view-depth bins; must equal GE_RECEIVER_DEPTH_BINS.
    static constexpr uint32_t kDepthBins = 64;
    /// Ray records: one per bin, then one for the rays that reach past every
    /// bin (the sky, or a surface beyond BinFar).
    static constexpr uint32_t kRayRecords = kDepthBins + 1;
    /// 32-bit words per side of the GPU result: the depth word, x, y, z of each
    /// bin's surface box, then x, y, z of each ray record.
    static constexpr uint32_t kWordsPerSide = 1 + 3 * kDepthBins + 3 * kRayRecords;
    static constexpr uint32_t kResultBytes = 2 * kWordsPerSide * sizeof(uint32_t);

    /// Recorded when the reduce is declared; travels with its readback slot.
    struct Context
    {
        /// World-space position and unit view basis of the measuring camera.
        Mathematics::Vector3 CameraPosition{};
        Mathematics::Vector3 CameraRight{};
        Mathematics::Vector3 CameraUp{};
        Mathematics::Vector3 CameraForward{};
        /// Half-extents of the view window: per unit view depth under a
        /// perspective projection, in world units under an orthographic one.
        float WindowHalfX = 0.0f;
        float WindowHalfY = 0.0f;
        /// The measuring projection the depth word linearizes against.
        float NearPlane = 0.0f;
        float FarPlane = 0.0f;
        bool Orthographic = false;
        /// Unit light direction whose rotation-only basis
        /// (ShadowMapRenderFeature::CascadeLightRotation) the bins are
        /// expressed in, applied to positions relative to this render origin.
        Mathematics::Vector3 LightDirection{};
        int32 RenderOriginSector[3]{};
        /// The camera's position (relative to that render origin) and forward
        /// in the bins' light space: the frame the ray records are rooted in.
        Mathematics::Vector3 CameraLightSpace{};
        Mathematics::Vector3 ForwardLightSpace{};
        /// Bin layout: log-spaced view depth over [BinNear, BinFar].
        float BinNear = 0.0f;
        float BinFar = 0.0f;

        /// Upper bound on a visible receiver's distance from the camera at
        /// view depth `depth`: the window corner ray's length to that depth.
        float RayLength(float depth) const;
    };

    Context Measured{};
    /// View depth of the nearest surface.
    float NearDepth = 0.0f;
    /// Light-space box of the surfaces in each bin; Empty() for an empty bin.
    std::array<Mathematics::AABB, kDepthBins> Bins{};
    /// The view rays of the samples whose surface lies in each bin, and in the
    /// last record of those that reach past every bin, as the light-space box
    /// of their ray parameter (RayPointsAt); Empty() where no sample landed.
    std::array<Mathematics::AABB, kRayRecords> Rays{};
    bool Valid = false;

    /// View-depth edges of bin `bin`.
    float BinNearDepth(uint32_t bin) const;
    float BinFarDepth(uint32_t bin) const;

    /// Light-space box of the points at view depth `depth` on the rays whose
    /// parameters `rays` bounds. A ray parameter is the ray's direction per
    /// unit view depth under a perspective camera (the point at depth z is
    /// CameraLightSpace + z * parameter) and its offset from the camera under
    /// an orthographic one (CameraLightSpace + parameter + z * ForwardLightSpace).
    Mathematics::AABB RayPointsAt(const Mathematics::AABB& rays, float depth) const;

    /// Light-space box of the visible air in bin `bin`: the points of the bin's
    /// depth range on every ray that reaches the bin (its surface lies in this
    /// bin or beyond, or it sees the sky), up to the ray's surface. `reaching`
    /// bounds the parameters of the rays that reach the bin, `passing` of those
    /// that reach past it. Bin 0's range starts at the eye.
    Mathematics::AABB AirInBin(uint32_t bin, const Mathematics::AABB& reaching,
                               const Mathematics::AABB& passing) const;
};

/// One readback slot as the CPU maps it: the reduce's two result sides plus the
/// context the reduce was declared with.
struct ShadowReceiverReadback
{
    uint32_t MinWords[ShadowReceiverMeasurement::kWordsPerSide];
    uint32_t MaxWords[ShadowReceiverMeasurement::kWordsPerSide];
    ShadowReceiverMeasurement::Context Measured{};
};

/// Bins per natural-log unit of view depth for bins spread over [binNear, binFar].
float ShadowReceiverBinsPerLogUnit(float binNear, float binFar);

/// Decodes a readback. False, leaving `out` invalid, when nothing wrote depth
/// (the sentinels the pass fills before its dispatch are untouched) or the
/// context is degenerate.
bool DecodeShadowReceiverResult(const ShadowReceiverReadback& readback,
                                ShadowReceiverMeasurement& out);

/// Fills the camera half of `context` (position, view basis, window and
/// projection kind) from `camera`. The reduce records it with each
/// measurement; the cascade fit describes the current camera with it.
void SetShadowReceiverCamera(const Rendering::CameraData& camera, bool orthographic,
                             ShadowReceiverMeasurement::Context& context);

/// True when `current` looks through the same window as `measured`: the same
/// projection kind and bit-identical half-extents. A wider field of view, a
/// resized viewport or a larger orthographic size reveals receivers beyond the
/// measured frustum's edges, at any distance from the receivers it measured
/// (at a pitched camera the top edge reaches tens of metres further over the
/// ground), so a measurement under another window bounds nothing.
bool ShadowReceiverSameWindow(const ShadowReceiverMeasurement::Context& measured,
                              const ShadowReceiverMeasurement::Context& current);

/// How far the receivers a measurement saw at view depth `depth` may have moved
/// in view depth since, and how far the receivers revealed since may lie from
/// the measured ones, once the camera moved from the measured pose to
/// `current`'s through the same window: the camera's travel plus its rotation
/// (turn, pitch and roll) swept at the ray length to that depth.
float ShadowReceiverMotionBound(const ShadowReceiverMeasurement& measurement, float depth,
                                const ShadowReceiverMeasurement::Context& current);

/// The light-space box of the measured receivers that can be selected into
/// view depths [depthLo, depthHi] from the `current` camera, the visible
/// surfaces and the visible air in front of them alike: the union of the bins
/// whose depth range, widened by the motion bound, overlaps that range, each
/// widened by the same bound. `measuredToCurrent` (null when the
/// measurement shares the current light basis and render origin) carries a bin
/// from the measurement's light space into the current one. Empty when no
/// measured receiver can reach the range. The caller checks
/// ShadowReceiverSameWindow first.
Mathematics::AABB GatherShadowReceivers(const ShadowReceiverMeasurement& measurement, float depthLo,
                                        float depthHi,
                                        const ShadowReceiverMeasurement::Context& current,
                                        const Mathematics::Matrix4x4* measuredToCurrent);

} // namespace Engine::Renderer
} // namespace GameEngine
