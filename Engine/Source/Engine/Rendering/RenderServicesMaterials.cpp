// RenderServicesMaterials.cpp
// Part of the RenderServices implementation — split by concern from the
// former single RenderServices.cpp. The material stack (registry, compiler,
// shader/variant caches, prewarm, build context, SSBO packing, classification,
// hot-reload, binder) moved to MaterialSystem (A1.3) and is reached via
// rs.Materials(). What stays here reads RenderServices-owned state: the
// world-pass-keyword resolver (blueprint + per-view state, both RS concerns)
// and the one register+prewarm entry that depends on it.
#include "Engine/Rendering/RenderServices.h"

#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/Pipeline/Nodes/WorldRenderNode.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include <optional>

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

MaterialKeyword RenderServices::ResolveWorldPassKeywordsForPrewarm() const
{
    // The active blueprint's WorldRender pass is authoritative: its keywords are the
    // exact set the world draw requests at draw time (WorldRenderNode parses the SAME
    // JSON into PerViewResources::WorldPassKeywords). Prewarming that set means the variant the
    // first draw needs is already cached. This naturally yields IBL for the editor's
    // ForwardPlus.rendergraph and non-IBL for WASDDemo.
    constexpr const char* kWorldRenderPassType = "WorldRender";
    if (const auto* bp = m_FrameOrchestrator.ActiveBlueprint())
    {
        for (const auto& pass : bp->passes)
        {
            if (pass.type == kWorldRenderPassType)
                return Pipeline::Nodes::ParseWorldPassKeywords(pass.passJson);
        }
    }

    // Blueprint not built yet (prewarm raced first-frame declaration): use the keywords a
    // real world pass already recorded for any view this frame. AddWorldPassForView writes
    // this from the SAME blueprint JSON, so once a frame has rendered it is draw-proven
    // ground truth — covering early-startup registration before EnsureActiveRenderPipelineBlueprint.
    std::optional<MaterialKeyword> anyRecorded;
    m_ViewRegistry.ForEachPerView(
        [&](const ViewRegistry::PerViewResources& pv)
        {
            if (!anyRecorded && pv.WorldPassKeywords && *pv.WorldPassKeywords != MaterialKeyword::None)
                anyRecorded = *pv.WorldPassKeywords;
        });
    if (anyRecorded)
        return *anyRecorded;

    // Nothing rendered yet either: fall back to the structural base every ForwardPlus color
    // pipeline declares so the work isn't wasted. If the loaded blueprint later adds keywords
    // (e.g. IBL), the GetOrCompileColorVariantImpl self-heal compiles the real variant on first draw.
    return MaterialKeyword::ForwardPlus | MaterialKeyword::Instanced | MaterialKeyword::Shadows;
}

// The register+prewarm entry stays on RenderServices because it resolves the
// world-pass keyword set from RS-owned blueprint + per-view state
// (ResolveWorldPassKeywordsForPrewarm) and passes it into the facade, which
// never touches that state. Every other material entry point — register,
// recompile, base-shader/variant prewarm, FinalizeFrameBuffers, build context —
// is called directly on rs.Materials().
Material* RenderServices::RegisterAndPrewarmMaterial(const GUID& guid, const MaterialDocument& doc,
                                                     Rendering::MaterialKeyword additionalKeywords)
{
    return m_MaterialSystem.RegisterAndPrewarmMaterial(guid, doc, additionalKeywords,
                                                       ResolveWorldPassKeywordsForPrewarm());
}

} // namespace Engine::Renderer
} // namespace GameEngine
