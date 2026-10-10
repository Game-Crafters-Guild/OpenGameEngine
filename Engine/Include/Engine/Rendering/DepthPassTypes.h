#pragma once

// Shared depth/shadow pass declaration types. Promoted out of the RenderServices
// private nested scope so ShadowMapRenderFeature can build the params for a
// shadow pass and hand them to the free RecordDepthOnlyPass (DepthDrawRecorder)
// executor. Kept in its own header (not FeatureDeclareContext.h) so the ctx
// header stays light — this one drags in ResolvedPassResources + SlicePhase.

#include "Engine/Rendering/DrawCommandProducer.h" // DepthPassType
#include "Engine/Rendering/PassBindingContext.h"  // ResolvedPassResources
#include "Rendering/CameraTypes.h"                 // ViewId
#include "Rendering/Core/GPUDrawStreamBuilder.h"    // GPUDrawStreamBuilder::SlicePhase

#include <cstdint>

namespace GameEngine
{
namespace Engine::Renderer
{

// Which forward heads a camera prepass (DepthPassType::Prepass, phase A) draws.
enum class PrepassHeads : uint8_t
{
    // The camera prepass: the entity batches and the ForwardDrawDepth::Prepass heads, plus the
    // PrepassNonOccluding heads when no non-occluding prepass was declared for the view.
    Occluding,
    // The non-occluding prepass after DepthResolve: the PrepassNonOccluding heads alone.
    NonOccluding,
};

// RenderGraph depth-only pass params. All declaration-time:
// Cam is an upload-ring {buffer, offset} written at declaration; the
// resolved table arrives PRE-BUILT (Cam-patched + contributor overrides
// applied — the old path patched pass.PassResources after BeginPass).
// Keyword narrowing stays at EXEC (old-arm parity): the world arm writes
// PerViewResources::WorldPassKeywords during declaration, AFTER the depth
// arms ran in recording order, and the recorded value is reset on every
// pipeline build — narrowing at declaration would miss Instanced for one
// frame per pipeline (re)load.
struct DepthOnlyPassParamsRG
{
    Rendering::ViewId ViewId{};
    uint32_t ViewportX = 0;
    uint32_t ViewportY = 0;
    uint32_t ViewportWidth = 0;
    uint32_t ViewportHeight = 0;
    bool DepthBiasEnable = false;
    // Pancaking. Casters nearer the light than the shadow camera's near plane
    // rasterize CLAMPED to the near depth instead of being clipped away, so the
    // near plane bounds the depth encode without deciding which occluders exist.
    // Directional cascades only: a directional light's caster ray is infinite,
    // so its near plane is arbitrary, while a spot/point light's near plane is a
    // real bound (nothing can sit behind the light).
    bool DepthClampEnable = false;
    // 0 = inherit from render pass, 1 = single-sample (shadows).
    uint32_t RasterizationSamples = 0;
    DepthPassType PassType = DepthPassType::Prepass;
    uint32_t CascadeIndex = 0;
    // Culling generation this pass consumes ranges from. Phase A is the
    // frame's first generation (the only one for frustum-only views); the
    // HZB recover prepass (design §5-A4) consumes phase B. Selects the
    // SlicePhase at the FindBatchDrawRange lookup so the recover pass reads
    // its own scatter generation, never phase A's.
    Rendering::GPUDrawStreamBuilder::SlicePhase Phase =
        Rendering::GPUDrawStreamBuilder::SlicePhase::A;
    // Camera prepass only (PassType Prepass, phase A); every other pass ignores it.
    PrepassHeads Heads = PrepassHeads::Occluding;
    ResolvedPassResources Resources;
};

} // namespace Engine::Renderer
} // namespace GameEngine
