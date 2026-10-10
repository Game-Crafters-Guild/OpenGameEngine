// The deforming-motion producer's declaration policy.
//
// Three arms are possible and exactly one may run: an extra colour target on
// the opaque world pass, the same on the depth prepass, or a pass of its own
// that re-rasterizes the deforming ranges. They write one shared target, so
// the decision of WHICH runs belongs to one place and the decision of WHO
// CLEARS follows from it — the arm that runs owns the clear and the target
// owner's own writes load.
//
// This is the policy half: which arm, whether this view can produce anything
// at all this frame, and which culling generations to record. The passes
// themselves are declared by the arms.
#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

namespace GameEngine::Engine::Renderer
{
struct ViewTemporalSample;
}

namespace GameEngine::Engine::Renderer::Pipeline
{

/// Declare the deforming-motion production for `view` into `target`.
///
/// Returns true when a producer arm recorded into the target and therefore
/// owns its clear; false when nothing was recorded, in which case the caller's
/// own writes keep the clear and the frame is unchanged from the no-producer
/// one. False is the answer for: no arm enabled, no deforming material in the
/// view, and no differenceable previous endpoint (a view's first frame, a
/// device rebuild, an origin re-anchor) — the last of which is the
/// discontinuity policy, and it leaves the sentinel rather than a zero vector.
///
/// `previous` and `previousValid` are the rotated history the caller already
/// resolved; `currentOrigin` is the deformation origin this frame's current
/// endpoint was formed against.
bool DeclareDeformationMotion(ViewDeclare& view,
                              Rendering::RenderGraph::RGTexture target,
                              const ViewTemporalSample* previous, bool previousValid,
                              double currentOrigin);

} // namespace GameEngine::Engine::Renderer::Pipeline
