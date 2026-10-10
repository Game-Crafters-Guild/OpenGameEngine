// The producer side of the composed motion variant's per-view block.
//
// The block itself is declared once in GLSL
// (Shaders/Includes/deformation_motion.glsl) and read by both stages of the
// motion variant. This header is its processor mirror plus the two decisions a
// producer has to make before it can fill it: which arm is writing, and whether
// the view's history can be differenced at all this frame.
#pragma once

#include "Mathematics/Rect.h"

#include <cstddef>
#include <cstdint>

namespace GameEngine::Engine::Renderer
{
struct ViewTemporalSample;

/// std140 mirror of the `DeformationMotionParams` block. The layout is pinned
/// on the shader side by
/// `DeformationMotionVariant.TheMotionBlockLayoutIsTheProducersUploadContract`
/// (112 bytes, members at 0 / 64 / 80 / 96); the static assertions below are
/// this side of that contract.
struct DeformationMotionParamsGPU
{
    /// The UNJITTERED view-projection of the frame this view last rendered.
    /// Jitter on this matrix would ride into every motion vector as
    /// jitter(n) - jitter(n-1), which is the one term the temporal resolve
    /// must not reproject by.
    float PrevViewProj[16]{};
    /// x = previous animation lane, y = previous scroll lane, both as
    /// `float(cumulative - origin)` against the one deformation origin every
    /// view shares (ViewTemporalHistory.h). zw unused.
    float PrevTimeParams[4]{};
    /// (x, y, width, height) exactly as handed to SetViewport: the viewport's
    /// top-left corner and size in framebuffer pixels, BEFORE the backend's
    /// negative-height flip. The fragment measures gl_FragCoord from that
    /// corner.
    float ViewportRect[4]{};
    /// xy = this frame's temporal jitter as a viewport-UV offset. zw unused.
    float JitterUv[4]{};
};
static_assert(sizeof(DeformationMotionParamsGPU) == 112,
              "must match Shaders/Includes/deformation_motion.glsl");
static_assert(offsetof(DeformationMotionParamsGPU, PrevTimeParams) == 64);
static_assert(offsetof(DeformationMotionParamsGPU, ViewportRect) == 80);
static_assert(offsetof(DeformationMotionParamsGPU, JitterUv) == 96);

/// Reflection names a uniform block by its INSTANCE name, and the world pass's
/// set-0 resolution keys on that name exactly as it does for `Cam` and `Light`.
/// A provider registered under the type name never binds.
inline constexpr const char* kDeformationMotionParamsName = "MotionParams";

/// Which producer writes the deforming surfaces into the shared motion target.
/// One value per option in the design's producer comparison; the arms exist
/// simultaneously so the comparison can measure them against each other.
enum class DeformationMotionArm : uint8_t
{
    /// Nothing records deforming motion. The target carries what the mover
    /// lane writes and the clear sentinel everywhere else — today's frame.
    None = 0,
    /// (a) An extra colour target on the opaque world pass.
    ColorPassTarget,
    /// (b) The same, on the depth prepass.
    PrepassTarget,
    /// (c) A pass of its own that re-rasterizes the deforming ranges.
    SeparatePass,
};

const char* DeformationMotionArmName(DeformationMotionArm arm);

/// Resolve the three independent switches to the one arm that runs.
///
/// Two producers writing one target would write every deforming pixel twice
/// and the later write would win non-deterministically, so more than one
/// enabled switch is refused by name and resolves to `None` — the frame keeps
/// today's behaviour rather than a wrong vector. The refusal is logged once.
DeformationMotionArm ResolveDeformationMotionArm(bool colorPassTarget, bool prepassTarget,
                                                 bool separatePass);

/// The history half of the block: what the view last rendered, and whether
/// this frame can be differenced against it. Resolved by the target's owner,
/// which is the one that rotated the history.
struct DeformationMotionEndpoint
{
    /// The frame this view last rendered, or null when it has none.
    const ViewTemporalSample* Previous = nullptr;
    /// Whether `Previous` is a real earlier rendered frame rather than a
    /// stand-in for this one (ViewTemporalHistory::Advance's outPrevValid).
    bool PreviousValid = false;
    /// The origin this frame's current endpoint was formed against. A previous
    /// sample carrying a different one is not differenceable against it.
    double CurrentOrigin = 0.0;
};

/// The raster half: what the producer's pass is about to rasterize with.
/// Built by that pass from the very expressions it hands SetViewport and
/// ResolveJitteredCameraData, so the fragment's reconstruction cannot disagree
/// with the raster that produced its gl_FragCoord.
struct DeformationMotionRaster
{
    /// The rect handed to SetViewport: top-left corner and size in framebuffer
    /// pixels, before the backend's negative-height flip.
    Mathematics::Rect Viewport{};
    /// The view's frozen NDC jitter for this frame; zero when no jittered
    /// anti-aliasing drives the view.
    float NdcJitterX = 0.0f;
    float NdcJitterY = 0.0f;
};

/// Fill `out`, or report that this view has no differenceable previous
/// endpoint this frame.
///
/// False means the discontinuity policy applies: the producer records no
/// deforming draw and the surface keeps the clear sentinel, which both
/// consumers already read as "no exact motion here". It never means "write a
/// zero vector" — a zero delta is a claim that the surface did not move.
bool BuildDeformationMotionParams(const DeformationMotionEndpoint& endpoint,
                                  const DeformationMotionRaster& raster,
                                  DeformationMotionParamsGPU& out);

} // namespace GameEngine::Engine::Renderer
