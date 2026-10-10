#pragma once

// CBTUpdateNode — the render-graph seam for the CBT update loop. Declares ONE
// compute pass ("CBT.Update") at kEarlySetup whose exec records the whole
// hand-barriered kernel sequence on ctx.Cmd (the Ocean OceanFFTGenerate pattern,
// plan §5: a single pass owns its internal barriers; only its externally-visible
// output is declared to the graph). The persistent CBT buffers a C3 indexed
// indirect draw consumes (indirect args + the ALL index stream + the per-bisector
// vertex buffer) are imported and declared as Storage writes + MarkOutput so the
// cross-pass barrier forms for the later forward draw. ImportOutputs exposes the
// same RGBuffers (idempotent per frame) so the forward pass can declare its reads
// against them — that declared read is what orders the draw after this pass.

#include <cstdint>

#include "CBTTerrain/CBTActivityReadback.h" // CBTActivitySlot (a defaulted argument)
#include "Rendering/Core/Handle.h" // TextureHandle
#include "Rendering/Core/RenderGraph/RGFrame.h"

namespace GameEngine::CBTTerrain
{

class CBTInstance;
struct CBTClassifyDesc;
struct CBTFrameParams;

// The CBT buffers imported into the current frame's graph. Import is idempotent
// per (name, handle) within a frame, so the update pass and the later forward
// draw obtain the SAME RGBuffers and the compute->graphics edge forms.
struct CBTUpdateOutputs
{
    Rendering::RenderGraph::RGBuffer IndirectDraw{};
    Rendering::RenderGraph::RGBuffer IndicesAll{};
    Rendering::RenderGraph::RGBuffer IndicesVisible{};
    Rendering::RenderGraph::RGBuffer CurrentVertex{};
    // The declared update pass, so the caller can order it against producers this module has no
    // business knowing about (the terrain texture upload). Invalid when nothing was declared.
    Rendering::RenderGraph::RGPass Update{};
};

class CBTUpdateNode
{
  public:
    // Import the persistent CBT draw-consumed buffers into `frame`. No side
    // effects beyond the imports; safe to call once per view for the forward
    // reads. Returns empty outputs if the instance is not ready.
    static CBTUpdateOutputs ImportOutputs(Rendering::RenderGraph::RGFrame& frame,
                                          CBTInstance& instance);

    // Declares the "CBT.Update" pass into `frame` and returns the imported
    // outputs. No-op (empty return) if the instance is not ready. `params` (camera +
    // terrain) drives the screen-space Classify metric and the height displacement;
    // the caller binds the height source on the instance before this. `heightSource`
    // is the terrain heightmap the VertexEval kernel samples (invalid = the flat
    // default): when valid it is imported and declared as a graph read (SampledCompute)
    // so a same-frame region re-upload (E2) is a visible dependency, and the pass is
    // scheduled just after the kEarlySetup heightmap upload so VertexEval never
    // samples a half-uploaded region (plan §8 C5). `activity`, when its Buffer is valid
    // (CBTActivityReadback::Begin), receives this update's work-queue counters, copied
    // and stamped in the same pass right after the kernels. The caller owns BeginFrame/Execute
    // on the frame, and should record this ONCE per frame.
    static CBTUpdateOutputs DeclareUpdatePass(Rendering::RenderGraph::RGFrame& frame,
                                              CBTInstance& instance,
                                              const CBTClassifyDesc& classify,
                                              const CBTFrameParams& params, uint32_t frameCounter,
                                              Rendering::TextureHandle heightSource,
                                              const CBTActivitySlot& activity = {});
};

} // namespace GameEngine::CBTTerrain
