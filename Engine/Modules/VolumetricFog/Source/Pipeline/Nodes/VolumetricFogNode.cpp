#include "Engine/Rendering/Pipeline/Nodes/VolumetricFogNode.h"

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/VolumetricFogRenderer.h"
#include "Logger/Logger.h"

#include <nlohmann/json.hpp>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

bool VolumetricFogNode::Initialize(std::string nodeId, std::string nodeJson, std::string* outError)
{
    m_Id = std::move(nodeId);
    m_Json = std::move(nodeJson);

    try
    {
        auto j = nlohmann::json::parse(m_Json);
        if (j.is_object() && j.contains("output") && j["output"].is_string())
            m_OutputRef = j["output"].get<std::string>();
    }
    catch (const std::exception& e)
    {
        if (outError)
            *outError = std::string("VolumetricFogNode JSON parse error: ") + e.what();
        return false;
    }

    m_PostWorldDepthResolve.Initialize(
        "VolumetricFog.PostWorldDepthResolve",
        std::string(R"({"output":")") + Names::View::DepthResolvedPostOcean +
            R"(","poolName":"Pipeline.DepthResolvedPostOcean.View"})",
        nullptr);
    return true;
}

void VolumetricFogNode::Declare(RenderPipelineInstance& instance,
                                const PipelineDeclareContext& ctx)
{
    auto& feature = instance.GetRenderServices().EnsureFeature<VolumetricFogRenderer>();
    auto* device = instance.GetRenderServices().GetDevice();
    if (!feature.IsInitialized() && !feature.InitializeFailed() && device)
    {
        if (!feature.Initialize(device))
            LOG_WARNING("VolumetricFogNode: failed to initialize VolumetricFogRenderer; "
                        "volumetric fog stays inactive this session");
    }
    m_PostWorldDepthResolve.Declare(instance, ctx);
}

void VolumetricFogNode::DeclareForView(ViewDeclare& d)
{
    // Gates verbatim at declaration: depth valid, output resolves, feature
    // initialized, settings derived ONCE. A disabled view resets history and
    // declares nothing (the pool ages out).
    if (!d.ViewDepth.IsValid())
        return;

    RenderGraph::RGTexture sceneColor = d.ResolveTexture(m_OutputRef);
    if (!sceneColor.IsValid())
        sceneColor = d.ViewResolve.IsValid() ? d.ViewResolve : d.ViewColor;
    if (!sceneColor.IsValid())
        return;

    auto* feature = d.Services.GetFeature<VolumetricFogRenderer>();
    if (!feature || !feature->IsInitialized())
        return;

    const auto& pp = d.Services.GetEffectivePostProcessSettings(d.View.id, d.View.worldId);
    VolumetricFogSettings settings = ToVolumetricFogSettings(pp);
    const auto localVolumes = d.Services.GetEffectiveVolumetricFogVolumes(d.View.id, d.View.worldId);
    settings.localVolumes.assign(localVolumes.begin(), localVolumes.end());
    if (!settings.localVolumes.empty() && !settings.isGlobal)
    {
        settings.density = 0.0f;
        settings.isGlobal = true;
        settings.localVolumeValid = false;
    }
    settings.enabled = settings.enabled || !settings.localVolumes.empty();
    feature->SetSettings(d.View.id, settings);
    if (!settings.enabled)
    {
        feature->ResetHistory(d.View.id);
        return;
    }

    // EFFECTIVE depth: the view depth after the world and the water. The ocean surface step
    // publishes it when the water draws (the fog then uses the water's own distance, not the sky
    // behind the transparent ocean); otherwise resolve it here, after every world depth writer, so
    // fragments written after the prepass copy are not fogged as the far plane. Fall back to the
    // early resolved depth, then the raw view depth.
    RenderGraph::RGTexture depthForFog = d.ResolveTexture(Names::View::DepthResolvedPostOcean);
    if (!depthForFog.IsValid())
    {
        m_PostWorldDepthResolve.DeclareForView(d);
        depthForFog = d.ResolveTexture(Names::View::DepthResolvedPostOcean);
    }
    if (!depthForFog.IsValid())
        depthForFog = d.ViewDepthResolved.IsValid() ? d.ViewDepthResolved : d.ViewDepth;
    feature->DeclareForView(d, sceneColor, depthForFog);
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
