#include "Engine/Rendering/Pipeline/DeformationMotionProducer.h"

#include "Engine/Rendering/DeformationMotionParams.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewTemporalHistory.h"
#include "Logger/Logger.h"

#include <unordered_set>

namespace GameEngine::Engine::Renderer::Pipeline
{
using SlicePhase = ::GameEngine::Rendering::GPUDrawStreamBuilder::SlicePhase;

namespace
{
// Warn once per arm that has no implementation yet, rather than silently
// producing nothing: a run that enabled a switch and saw no motion must be
// able to tell "the arm is off" from "the arm does not exist".
void WarnArmNotImplemented(DeformationMotionArm arm)
{
    static bool warnedOnce = false;
    if (warnedOnce)
        return;
    warnedOnce = true;
    Logger::Log::Error(
        "Deformation motion: the {} producer arm is enabled but is not implemented in this build, "
        "so no deforming motion is produced. Enable the separate-pass arm "
        "(GE_DEFORMATION_MOTION_SEPARATE_PASS) instead.",
        DeformationMotionArmName(arm));
}
} // namespace

bool DeclareDeformationMotion(ViewDeclare& view, Rendering::RenderGraph::RGTexture target,
                              const ViewTemporalSample* previous, bool previousValid,
                              double currentOrigin)
{
    const DeformationMotionArm arm = view.Services.GetDeformationMotionArm();
    if (arm == DeformationMotionArm::None)
        return false;
    if (arm != DeformationMotionArm::SeparatePass)
    {
        WarnArmNotImplemented(arm);
        return false;
    }
    if (!target.IsValid() || !view.ViewDepth.IsValid())
        return false;

    // A view with no deforming material records nothing and declares no pass.
    // The subset is derived alongside the batch keys, so this costs a lookup.
    auto& drawBuilder = view.Services.GetWorldDrawBuilder();
    if (drawBuilder.GetDeformingBatchKeys(view.View.id).empty())
    {
        // Saying so is the difference between "nothing in this view deforms"
        // and "the lane predicate rejected what does": both produce an empty
        // span, and only the message can tell them apart. Once PER VIEW, not
        // once per process — a frame declares several views and the first one
        // to miss would otherwise swallow the message for the view the reader
        // is actually looking at.
        static std::unordered_set<uint32_t> warnedViews;
        if (warnedViews.insert(static_cast<uint32_t>(view.View.id)).second)
        {
            Logger::Log::Info(
                "Deformation motion: view {} has {} batch keys and none of them is in the "
                "deforming lane, so no motion is produced for it. The lane takes a material "
                "carrying the simple vec3 ModifyVertex form; the extended output form, "
                "procedural geometry and blended materials are refused, and contributor "
                "geometry (terrain, ocean) is not in it at all.",
                static_cast<uint32_t>(view.View.id),
                drawBuilder.GetBatchKeys(view.View.id).size());
        }
        return false;
    }

    // Per-instance validity arrives on the channel the scatter writes, and the
    // motion variant declares that binding unconditionally — so a frame whose
    // history is not allocated must record nothing rather than draw with an
    // unwritten descriptor. The camera slices ask for the history whenever an
    // arm is enabled, so this fires only on the frames before the allocation
    // lands, and it says so.
    auto* streams = view.Services.GetDrawStreamBuilder();
    if (streams == nullptr ||
        !streams->GetRenderedHistoryForRead(static_cast<uint32_t>(view.View.id)).IsValid())
    {
        static bool warnedOnce = false;
        if (!warnedOnce)
        {
            warnedOnce = true;
            Logger::Log::Warning(
                "Deformation motion: view {} has no rendered level and phase history this frame, "
                "so per-instance validity cannot be read and no deforming motion is produced. "
                "This is expected on the frames before the scatter allocates it.",
                static_cast<uint32_t>(view.View.id));
        }
        return false;
    }

    DeformationMotionEndpoint endpoint{};
    endpoint.Previous = previous;
    endpoint.PreviousValid = previousValid;
    endpoint.CurrentOrigin = currentOrigin;

    // Phase A owns the clear. The pass returns invalid when the endpoint pair
    // is not differenceable, which is the discontinuity policy's "no draw".
    const auto phaseA = view.Services.AddDeformationMotionPassForView(
        view.Frame, view.View.id, target, view.ViewDepth, endpoint, /*clearTarget=*/true,
        SlicePhase::A);
    if (!phaseA.IsValid())
        return false;

    // The occlusion-recovery generation, consumed symmetrically with the
    // prepass: a deforming instance revealed in phase B is not left on the
    // sentinel. It loads — it completes phase A's image rather than replacing
    // it — and needs no ordering of its own against phase A's writes, because
    // the depth both test against is already the frame's final depth. Declared
    // only when the view published phase-B ranges at all; every non-HZB view
    // publishes none.
    if (streams->HasPublishedRangesForPhase(
            static_cast<uint32_t>(view.View.id),
            ::GameEngine::Rendering::GPUDrawStreamBuilder::kCascadeIndexNone, SlicePhase::B))
    {
        view.Services.AddDeformationMotionPassForView(view.Frame, view.View.id, target,
                                                      view.ViewDepth, endpoint,
                                                      /*clearTarget=*/false, SlicePhase::B);
    }
    return true;
}

} // namespace GameEngine::Engine::Renderer::Pipeline
