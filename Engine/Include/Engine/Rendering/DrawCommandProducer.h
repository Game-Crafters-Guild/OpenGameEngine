#pragma once

#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Handle.h"

#include <cstdint>

namespace GameEngine::Rendering
{
class IDevice;
namespace RenderGraph
{
class RGFrame;
} // namespace RenderGraph
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{
class RenderServices;

enum class ForwardEmitPurpose : uint8_t
{
    World,
    ReflectionProbeCapture,
};

// Identifies the type of depth pass invoking a registered depth emit callback.
// Callbacks can use this to select pipeline variants (e.g. depth bias for
// shadows) or skip passes they don't participate in.
enum class DepthPassType : uint8_t
{
    Prepass,       // Main camera depth prepass
    ShadowCascade, // Directional light shadow cascade
    AreaShadow,    // Local area light shadow map
    SpotShadow,    // Local spot light shadow map
    PointShadow,   // Local point light shadow map face
    TransmittanceCascade, // Glass-only light-space tint pass (translucent shadows)
    DeformationMotion, // Deforming-motion producer: the deforming subset of the camera's batch keys, re-rasterized into the shared motion target under read-only camera depth
    Count          // Sentinel: number of depth pass types (sizes per-pass-type arrays). Keep last.
};

// How a forward contributor draw's depth reaches its view's depth target. A view's world pass
// attaches the depth the camera prepass wrote read-only only while none of its forward draws
// writes depth itself. GTAO and the screen-space contact shadows read the occluders' copy
// DepthResolve takes of that depth (Names::View::OccluderDepthResolved); a head drawn before the
// copy occludes them, a head drawn after it (PrepassNonOccluding) does not. Every other reader of
// the resolved depth (View.DepthResolved) sees both kinds.
enum class ForwardDrawDepth : uint8_t
{
    // The producer emitted the draw's depth-only head into the camera prepass with it this frame
    // (EmitForwardCommand's `prepassHead`): the same geometry through the same vertex stage, with the
    // coverage the colour draw keeps. The colour draw only tests against it.
    Prepass,
    // As Prepass, but the head draws in the non-occluding prepass, after DepthResolve took the
    // occluders' depth: the colour draw still tests against it, while GTAO and the
    // contact shadows neither see nor are darkened by it. For thin, dense cover that receives
    // neither pass (grass). A view whose pipeline takes no such copy draws the head in the camera
    // prepass instead.
    PrepassNonOccluding,
    // The colour draw writes its own depth: it has no prepass head this frame (its producer draws
    // none, or the device has not built the head's pipeline yet).
    ColourPass,
    // The colour draw writes no depth (blended).
    None,
};

// The passes that declare a read of a buffer a producer's forward draws read
// (RenderServices::EmitForwardSampledBufferRead).
enum class ForwardBufferReaders : uint8_t
{
    // The world pass alone: the draws have no prepass heads this frame.
    WorldPass,
    // The world pass and the camera prepasses (the camera prepass and the non-occluding prepass),
    // whose heads of those draws read the same buffer.
    WorldPassAndPrepass,
};

// Parameter bag passed to a registered forward-emit callback. Producers push
// DrawCommand records into the per-view command stream owned by RenderServices
// via EmitForwardCommand(); the world pass execute lambda later iterates that
// stream and records the draws through MaterialBinder.
//
// No CommandList here — emit runs before pass execution.
struct ForwardEmitContext
{
    ::GameEngine::Rendering::IDevice* Device = nullptr;
    RenderServices* Services = nullptr;

    // The frame being declared. Producers use its upload ring for per-frame
    // UBO/SSBO data bound on the emitted draws ({buffer, offset} DrawBindings
    // entries) instead of hand-rolled per-frame buffer rings. Null only when
    // no frame stream is active; producers must skip such uploads then.
    ::GameEngine::Rendering::RenderGraph::RGFrame* Frame = nullptr;

    ::GameEngine::Rendering::ViewId ViewId{};
    uint32_t FrameIndex = 0;
    ForwardEmitPurpose Purpose = ForwardEmitPurpose::World;
};

// Parameter bag passed to a registered depth-emit callback. Producers push
// DrawCommand records into the per-(view, passType) depth command stream via
// EmitDepthCommand(); the depth-pass execute lambda later iterates that
// stream and records draws through MaterialBinder. Registered depth producers
// serve the light-space passes; the camera prepass draws the heads forward
// producers emit beside their colour draws (ForwardDrawDepth::Prepass and PrepassNonOccluding).
//
// No CommandList here — emit runs before pass execution. The per-cascade
// camera/light UBO referenced by callback bindings (e.g. terrain's
// ViewParams) is overridden by the consumer per cascade after BeginPass,
// matching the entity-loop pattern that overrides "Cam" inside
// RecordDepthOnlyPass.
struct DepthEmitContext
{
    ::GameEngine::Rendering::IDevice* Device = nullptr;
    RenderServices* Services = nullptr;

    ::GameEngine::Rendering::ViewId ViewId{};
    uint32_t FrameIndex = 0;
};

} // namespace GameEngine::Engine::Renderer
