#include "Engine/Rendering/Pipeline/Nodes/WorldRenderNode.h"

#include "Engine/Rendering/DDGIProbeFeature.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/ReflectionsProvider.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/RenderServices.h"

#include <nlohmann/json.hpp>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

Rendering::MaterialKeyword ParseWorldPassKeywords(const std::string& passJson)
{
    Rendering::MaterialKeyword keywords = Rendering::MaterialKeyword::None;
    try
    {
        auto j = nlohmann::json::parse(passJson);
        if (j.is_object() && j.contains("keywords") && j["keywords"].is_array())
        {
            for (const auto& kw : j["keywords"])
            {
                if (!kw.is_string())
                    continue;
                const std::string& s = kw.get_ref<const std::string&>();
                if (s == "ForwardPlus")
                    keywords |= Rendering::MaterialKeyword::ForwardPlus;
                else if (s == "Instanced")
                    keywords |= Rendering::MaterialKeyword::Instanced;
                else if (s == "Shadows")
                    keywords |= Rendering::MaterialKeyword::Shadows;
                else if (s == "IBL")
                    keywords |= Rendering::MaterialKeyword::IBL;
            }
        }
    }
    catch (...)
    {
    }
    return keywords;
}

bool WorldRenderNode::Initialize(std::string nodeId, std::string nodeJson, std::string* /*outError*/)
{
    m_Id = std::move(nodeId);
    m_Json = std::move(nodeJson);

    // colorResolveTarget is not parsed here: it is captured at blueprint-compile
    // time (RenderPipelineBlueprint::worldColorResolveTargetRef) and applied
    // per-view by RenderPipelineInstance::EnsureNodes.
    m_PassKeywords = ParseWorldPassKeywords(m_Json);

    return true;
}

void WorldRenderNode::DeclareForView(ViewDeclare& d)
{
    if (!d.ViewColor.IsValid() && !d.ViewDepth.IsValid())
        return;

    // Build the sorted transparent (Blend) draw set first so the opaque pass's
    // peel sees the active flag and drops those batches (T2). Empty-set early-out
    // costs nothing for views with no transparents.
    //
    // ONLY when this pipeline has the SortedTransparent node to drain it. The
    // drain relocated out of this node (S1), so a pipeline without that node
    // (debug-overlay / post-stack / preview / builtin-fallback / project-local
    // ForwardPlus copies) would peel Blend here and never draw it → vanish.
    // Skipping the build keeps the set inactive so the peel drops nothing and
    // Blend rides the batched path — same guard as PipelineHasTransmissivePass.
    if (d.PipelineHasSortedTransparentPass)
        d.Services.BuildSortedTransparentForView(d.View.id);

    // The pre-pass already applied the MSAA-off collapse onto ViewColor and
    // resolved the pipeline resolve target into ViewResolve; the arm does the
    // resolve three-way internally from these values.
    Engine::Renderer::RenderServices::WorldPassTargetsRG t{};
    t.Color = d.ViewColor;
    t.Depth = d.ViewDepth;
    t.Resolve = d.ViewResolve;
    t.DepthResolved = d.ViewDepthResolved;
    // The contact shadows march the occluders' depth, which has no grass heads (DepthResolveNode).
    t.OccluderDepthResolved = d.ViewOccluderDepthResolved;
    // Screen-space GTAO, published by the AmbientOcclusion node earlier in the
    // graph; invalid when absent (the binder falls back to the no-occlusion default).
    t.GTAO = d.ResolveTexture(Names::View::GTAO);
    t.ResolveFromPipeline = d.ResolveFromPipeline;
    // Peel transmissive glass into the dedicated post-grab transmissive pass when this view has any
    // AND this pipeline actually has that pass; otherwise glass renders inline here via the env-cube
    // fallback (thumbnails, single-material previews, or any pipeline without the transmissive node)
    // instead of vanishing.
    const auto scope = (d.Services.HasTransmissionInView(d.View.id) && d.PipelineHasTransmissivePass)
                           ? Engine::Renderer::RenderServices::WorldPassDrawScope::OpaqueOnly
                           : Engine::Renderer::RenderServices::WorldPassDrawScope::All;
    // GTAO is consumed only when an AO PostProcessVolume is active for this view; mirror
    // AONode's gate so the StandardPBR variant (and the ge_gtao bind) match the producer.
    Rendering::MaterialKeyword effectiveKeywords = m_PassKeywords;
    if (d.Services.GetEffectivePostProcessSettings(d.View.id, d.View.worldId).IsAOActive())
        effectiveKeywords |= Rendering::MaterialKeyword::GTAO;
    // DDGI is consumed only when a DDGIVolume resolved and converged for
    // THIS view's world — mirrors the GTAO gate above. Requires IBL too
    // (DDGI replaces the IBL diffuse term, see ibl.glsl); a pipeline without
    // the "IBL" pass keyword never grows the DDGI binding regardless of a
    // converged volume, matching ibl.glsl's include guard.
    // The DDGI resolve's shading-normal history for this frame's world pass to
    // write (valid only on frames the resolve pass was declared).
    Rendering::RenderGraph::RGTexture shadingNormal{};
    if (HasKeyword(m_PassKeywords, Rendering::MaterialKeyword::IBL))
    {
        if (auto* ddgi = d.Services.GetFeature<Engine::Renderer::DDGIProbeFeature>();
            ddgi && ddgi->HasConvergedVolume() && ddgi->GetActiveVolume().WorldId == d.View.worldId)
        {
            effectiveKeywords |= Rendering::MaterialKeyword::DDGI;
            // Probe resolve: one compute pass per view, after the depth
            // prepass (it samples the 1-sample view depth), before this world
            // pass (which Reads its outputs). It carries the diffuse
            // irradiance and, with EnableGlossy, the two reflection lobes;
            // the forward consume has no gather of its own. Declares nothing
            // when the view cannot feed it (MSAA depth with no resolve) — the
            // consume then keeps the flat environment sample (the C0 UBO's
            // uParams2.w flag stays in sync, see GlossyResolveConsumeActive).
            // Published so the phase-B recover pass (ScatterBNode) binds the
            // same textures.
            const auto viewParams = d.ResolveBuffer(Names::Res::ViewParams);
            Rendering::RenderGraph::RGTexture resolveRough{};
            Rendering::RenderGraph::RGTexture resolveGlossy{};
            Rendering::RenderGraph::RGTexture resolveIrradiance{};
            // For the DDGIVolume debug overlay ONLY. The overlay writes
            // display-referred [0,1] colours into the scene-linear irradiance
            // target, and the kernel divides them by this scale so they read
            // at about one display white under the view's exposure. Lighting
            // never sees it: with DebugView == None the kernel ignores the
            // value, and the resolved irradiance is not exposed here.
            const float viewExposureScale =
                d.Services.GetEffectivePostProcessSettings(d.View.id, d.View.worldId).Exposure;
            if (ddgi->DeclareGlossyResolveForView(
                    d.Frame, d.Services, viewParams.Buffer, viewParams.Offset, viewParams.Size,
                    d.ViewDepthResolved, d.RenderWidth, d.RenderHeight,
                    static_cast<uint32_t>(d.View.id), viewExposureScale,
                    d.PassName("DDGIGlossyResolve").c_str(), resolveRough, resolveGlossy,
                    resolveIrradiance, shadingNormal))
            {
                t.DDGIResolveRough = resolveRough;
                t.DDGIResolveGlossy = resolveGlossy;
                t.DDGIResolveIrradiance = resolveIrradiance;
                d.PublishTexture(Names::View::DDGIResolveRough, resolveRough);
                d.PublishTexture(Names::View::DDGIResolveGlossy, resolveGlossy);
                d.PublishTexture(Names::View::DDGIResolveIrradiance, resolveIrradiance);
            }
        }
    }
    // Feature-owned MRT slices ride the world pass through the generic
    // extra-attachment list; each provider owns its targets and its keyword.
    // The DDGI resolve's shading-normal history is the normal slice when it
    // exists: the provider attaches the G-buffer for it even without SSSR.
    const Rendering::MaterialKeyword reflectionKeywords =
        ReflectionsProvider::ContributeWorldPassTargets(d, t, shadingNormal);
    effectiveKeywords |= reflectionKeywords;
    // The Parallax steps debug view, per view. The variant cache keeps it off every material that
    // does not march (NarrowColorPassKeywords).
    if (d.Services.Views().GetViewParallaxStepsView(d.View.id))
        effectiveKeywords |= Rendering::MaterialKeyword::ParallaxStepsView;
    if (shadingNormal.IsValid() &&
        HasKeyword(reflectionKeywords, Rendering::MaterialKeyword::SSSRNormalRoughness))
    {
        if (auto* ddgi = d.Services.GetFeature<Engine::Renderer::DDGIProbeFeature>())
            ddgi->MarkGlossyResolveNormalWritten(static_cast<uint32_t>(d.View.id),
                                                 d.Frame.FrameIndex());
        d.Frame.MarkPersistentTextureInitialized(shadingNormal);
    }
    // A forward shader that reads the view's exposure (view_exposure.glsl) needs the metered scale:
    // the metering node declares after this pass, so materialize the view's exposure history now.
    // The pass then binds it by name and declares the read that orders it against the metering
    // write; without it the pass binds the zero fallback, the static scale.
    d.ResolveBuffer(Names::Res::ExposureHistory);
    const auto r = d.Services.AddWorldPassForView(d.Frame, d.View.id, t, effectiveKeywords, scope);
    // The thumbnail/readback contract: what the pass ACTUALLY wrote.
    if (r.EffectiveColor.IsValid())
        d.PublishTexture(Names::View::EffectiveColor, r.EffectiveColor);

    // The sorted transparent (Blend) drain moved to SortedTransparentNode
    // (transparency-scale S1): it must render after HZBBuild so the GPU cull can
    // test transparents against the current-frame opaque HZB. The set is still
    // BUILT above (BuildSortedTransparentForView) so the opaque peel drops those
    // batches; the relocated node reads it back and issues the draw.
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
