#include "Engine/Rendering/Pipeline/Nodes/TransmissivePassNode.h"

#include "Engine/Rendering/Pipeline/Nodes/WorldRenderNode.h" // ParseWorldPassKeywords
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"

#include "Logger/Logger.h"

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

bool TransmissivePassNode::Initialize(std::string nodeId, std::string nodeJson, std::string* /*outError*/)
{
    m_Id = std::move(nodeId);
    m_Json = std::move(nodeJson);
    // Same keyword set as the world pass (ForwardPlus / IBL / Shadows / Instanced) so the
    // glass variant compiles with full lighting + the IBL env-cube refraction fallback.
    m_PassKeywords = ParseWorldPassKeywords(m_Json);
    return true;
}

void TransmissivePassNode::DeclareForView(ViewDeclare& d)
{
    const bool hasGlass = d.Services.HasTransmissionInView(d.View.id);
    const bool hasLateForward = d.Services.HasLateForwardCommands(d.View.id);
    // Glass-free and particle-free views pay nothing.
    if (!hasGlass && !hasLateForward)
        return;

    // Transparent contributors such as smoke and ocean spray render after the
    // Ocean node. Keeping them out of World prevents refraction scene-grabs from
    // treating foreground particles as background. They composite into the
    // resolved HDR colour Ocean made authoritative, depth-testing against the
    // opaque depth that produced it so occluded particles are hidden rather than
    // painted over the geometry in front of them.
    if (hasLateForward)
    {
        Engine::Renderer::RenderServices::WorldPassTargetsRG lateTargets{};
        // Ocean has already made the resolved HDR scene colour authoritative.
        // Blend late particles into that target rather than resolving the
        // pre-ocean MSAA colour a second time.
        lateTargets.Color = d.ViewResolve;
        lateTargets.Resolve = {};
        lateTargets.DepthResolved = d.ViewDepthResolved;
        lateTargets.ResolveFromPipeline = false;
        // The view depth is attached when it matches the colour's sample count and extent: a
        // multisampled depth beside a 1-sample colour makes DeriveFormatKey report the whole
        // pass as multisampled and trips the SetPipeline sample-count assert. Under MSAA the
        // colour here is the resolved 1-sample target, so the pass attaches a 1-sample copy of
        // the view depth instead (DeclareLateDepthCopy).
        const uint32_t viewKey = static_cast<uint32_t>(d.View.id);
        if (d.ViewDepth.IsValid() && lateTargets.Color.IsValid())
        {
            const auto colorDesc = d.Frame.Graph().ResourceDesc(lateTargets.Color.Id);
            const auto depthDesc = d.Frame.Graph().ResourceDesc(d.ViewDepth.Id);
            const bool viewDepthFits = depthDesc.SampleCount == colorDesc.SampleCount &&
                                       depthDesc.Width == colorDesc.Width && depthDesc.Height == colorDesc.Height;
            lateTargets.Depth = viewDepthFits ? d.ViewDepth : DeclareLateDepthCopy(d, colorDesc);
            if (lateTargets.Depth.IsValid())
            {
                m_DeclinedLateDepthByView.erase(viewKey);
            }
            else
            {
                // Declining is invisible in the frame — the pass simply loses its z-test — so say
                // so once, naming what the viewer will see.
                const LateDepthSpec spec{colorDesc.SampleCount, depthDesc.SampleCount,
                                         colorDesc.Width,       colorDesc.Height,
                                         depthDesc.Width,       depthDesc.Height};
                const auto it = m_DeclinedLateDepthByView.find(viewKey);
                if (it == m_DeclinedLateDepthByView.end() || !(it->second == spec))
                {
                    m_DeclinedLateDepthByView[viewKey] = spec;
                    Logger::Log::Debug(
                        "TransmissiveRender(view {}): late transparents have NO depth test — the "
                        "view depth ({} samples, {}x{}) cannot share a pass with the colour they "
                        "composite into ({} samples, {}x{}). Smoke and ocean spray will draw over "
                        "geometry that occludes them.",
                        viewKey, depthDesc.SampleCount, depthDesc.Width, depthDesc.Height,
                        colorDesc.SampleCount, colorDesc.Width, colorDesc.Height);
                }
            }
        }
        // Unlit particles divide by the view's exposure (view_exposure.glsl), which needs the
        // metered scale: materialize the view's exposure history now, before the metering node
        // does, so the pass binds it by name and declares its read with the view's other
        // published buffers. A graph that declares no ExposureHistory binds the zero fallback.
        d.ResolveBuffer(Names::Res::ExposureHistory);
        const auto lateCommands = d.Services.GetLateForwardCommands(d.View.id);
        // Individual commands carry their vertex/instancing keywords. Lit
        // particles need the same clustered local lights and shadow receivers
        // as the world pass; IBL alone silently drops point and spot lights.
        const Rendering::MaterialKeyword lateKeywords = m_PassKeywords & kLateTransparentKeywords;
        // Read-only: these contributors test depth and never own it. The producer is
        // registered writesDepth=false (RenderExtractionSystem), and a ReadWrite attach would
        // declare a depth WRITE the graph would then order every later depth reader against.
        d.Services.AddForwardCommandPassForView(
            d.Frame, d.View.id, lateTargets, lateKeywords,
            lateCommands, {}, "LateTransparent", {},
            Rendering::RenderGraph::RGDepthAccess::ReadOnly);
    }

    if (!hasGlass)
        return;

    // Composite over the LIT opaque scene the world node published (the resolved single-sample
    // colour). The shared SceneColor id orders this write after the world pass and before
    // VolumetricFog; the grab (which reads the same colour) is declared between them below.
    const auto effectiveColor = d.ResolveTexture(Names::View::EffectiveColor);
    if (!effectiveColor.IsValid())
        return;

    // Grab the lit-opaque scene colour (post-world) BEFORE the transmissive pass so the glass
    // refracts it screen-space. The grab reads View.EffectiveColor (ordering it after the world
    // pass); the transmissive pass writes the same colour (ordering it after the grab). When the
    // grab declines (e.g. a multisampled source) the SceneColorGrab keyword is dropped and the
    // glass compiles the env-cube refraction fallback instead.
    auto& grab = d.Services.GetOrCreateTransmissionSceneGrab();
    const bool grabReady = grab.DeclareForView(d, static_cast<uint32_t>(d.Frame.FrameIndex()));

    Rendering::MaterialKeyword keywords = m_PassKeywords;
    if (grabReady)
        keywords |= Rendering::MaterialKeyword::SceneColorGrab;

    // Composite into the RESOLVED single-sample colour, depth-tested against the opaque depth that
    // produced it. The attach is read-only (AddWorldPassImpl forces it for this scope), which is
    // also what suppresses depth writes: the recorder issues vkCmdSetDepthWriteEnable(VK_FALSE) on
    // a read-only pass, so the material's own depthWriteEnable cannot leak a write. Glass must not
    // write depth — it is peeled from the prepass, and a write would occlude the transmissive
    // fragments behind it in a pass that does not sort. The material pipeline already carries
    // depthTest=on with GreaterOrEqual (reverse-Z); the attachment is the only thing that turns
    // the test on.
    //
    // Attach only when the depth matches the colour's sample count and extent. A multisampled
    // depth beside the 1-sample resolved colour makes DeriveFormatKey report the whole pass as
    // multisampled and trips the SetPipeline sample-count assert. Under MSAA this pass stays
    // attachment-less and glass falls back to the in-shader compare against ge_sceneDepth
    // (adapter_forward.glsl), View.DepthResolved, which is resolved after the prepass. The late
    // transparents above make their own single-sample copy of the full view depth. DepthResolved is
    // published either way so BuildPassResourcesRG binds ge_sceneDepth for that fallback and for
    // the grab's refraction depth-texel test.
    Engine::Renderer::RenderServices::WorldPassTargetsRG t{};
    t.Color = effectiveColor;
    t.DepthResolved = d.ViewDepthResolved;
    if (d.ViewDepth.IsValid())
    {
        const auto& colorDesc = d.Frame.Graph().ResourceDesc(effectiveColor.Id);
        const auto& depthDesc = d.Frame.Graph().ResourceDesc(d.ViewDepth.Id);
        if (depthDesc.SampleCount == colorDesc.SampleCount &&
            depthDesc.Width == colorDesc.Width && depthDesc.Height == colorDesc.Height)
            t.Depth = d.ViewDepth;
    }
    // Frame-scoped grab id: the pass declares the Read(Sampled) that orders the
    // grab copy before this draw (the contract in SceneColorGrab.h).
    if (grabReady)
        t.SceneGrab = grab.GetGrabTextureRG();
    d.Services.AddWorldPassForView(
        d.Frame, d.View.id, t, keywords,
        Engine::Renderer::RenderServices::WorldPassDrawScope::TransmissiveOnly);
}

Rendering::RenderGraph::RGTexture TransmissivePassNode::DeclareLateDepthCopy(
    ViewDeclare& d, const Rendering::RenderGraph::RGResourceDesc& colorDesc)
{
    // The source is the multisampled view depth as it stands when this pass runs, so the copy
    // holds everything the opaque passes wrote — the prepass, terrain, grass, the ocean and the
    // world pass — as the single-sample path's direct attach does. Sample 0 of each pixel: the
    // sample every single-sample reader of the view depth takes. View.DepthResolved is not the
    // source: it is resolved right after the prepass. The copy needs the same extent; a view
    // whose depth differs in extent has no copy and its late transparents no depth test.
    const RenderGraph::RGTexture source = d.ViewDepth;
    const auto& sourceDesc = d.Frame.Graph().ResourceDesc(source.Id);
    if (sourceDesc.SampleCount <= 1u || sourceDesc.Width != colorDesc.Width ||
        sourceDesc.Height != colorDesc.Height || colorDesc.SampleCount != 1u)
        return {};
    if (!m_LateDepthCopy.EnsureLoaded(d.Services.GetDevice(), /*multisampledSource=*/true))
        return {};

    TextureDesc desc{};
    desc.width = colorDesc.Width;
    desc.height = colorDesc.Height;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.sampleCount = 1;
    desc.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    desc.usage = static_cast<uint32_t>(TextureUsage::DepthStencil);
    const RenderGraph::RGTexture copy = d.Frame.CreateTexture(d.PassName("LateTransparentDepth").c_str(), desc);
    if (!copy.IsValid())
        return {};
    m_LateDepthCopy.Declare(d.Frame, source, copy, colorDesc.Width, colorDesc.Height,
                            d.PassName("LateTransparentDepthCopy"));
    return copy;
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
