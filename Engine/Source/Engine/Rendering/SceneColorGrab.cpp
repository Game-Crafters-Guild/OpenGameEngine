#include "Engine/Rendering/SceneColorGrab.h"

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h" // Pipeline::ViewDeclare
#include "Engine/Rendering/RenderServices.h"

#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineDescTranslator.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGFullscreen.h"

#include <utility>

namespace GameEngine::Engine::Renderer
{

using namespace ::GameEngine::Rendering;

SceneColorGrab::SceneColorGrab(std::string sourceResourceName, std::string debugName)
    : m_SourceName(std::move(sourceResourceName)), m_DebugName(std::move(debugName))
{
    m_CopyLayoutName = m_DebugName + ".Copy.Set0";
    m_CopyPipelineName = m_DebugName + ".Copy";
    m_SamplerName = m_DebugName + "_Sampler";
    m_SettleName = m_DebugName + "Settle";
}

SceneColorGrab::~SceneColorGrab()
{
    if (!m_Device)
        return;
    if (m_GrabTexture.IsValid())
        m_Device->DestroyTexture(m_GrabTexture);
    if (m_Sampler.IsValid())
        m_Device->DestroySampler(m_Sampler);
}

void SceneColorGrab::OnDeviceRebuilt()
{
    m_GrabTexture = {};
    m_Sampler = {};
    m_Width = 0;
    m_Height = 0;
    m_ReadyFrameIndex = UINT32_MAX;
}

bool SceneColorGrab::EnsurePipeline(IDevice& device)
{
    if (!m_PipelineLoadAttempted)
    {
        m_PipelineLoadAttempted = true; // one attempt: a missing pkg declines the grab
        if (!RenderGraph::LoadCopyPipelineDesc(device.PreferredShaderSource(),
                                               m_CopyPipelineName.c_str(), m_CopyLayoutName.c_str(),
                                               m_CopyLayout, m_CopyPipeline))
            Logger::Log::Warning(
                "SceneColorGrab({}): copy.shaderpkg unavailable; refraction declines (env-cube fallback).",
                m_DebugName);
    }
    if (!m_CopyPipelineId.IsValid() && !m_CopyPipeline.vertexShader.empty())
        m_CopyPipelineId = PipelineDescTranslator::InternGraphics(device, m_CopyPipeline);
    return m_CopyPipelineId.IsValid();
}

bool SceneColorGrab::EnsureTexture(IDevice& device, uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return false;
    if (m_GrabTexture.IsValid() && width == m_Width && height == m_Height)
        return true;

    if (m_GrabTexture.IsValid())
        device.DestroyTexture(m_GrabTexture);

    TextureDesc td{};
    td.width = width;
    td.height = height;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.sampleCount = 1;
    td.format = m_Format;
    td.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    // Resting state ShaderResource: the grab settles here so the transmissive pass's
    // descriptor-direct sample sees a stable layout and next frame's import is well-defined.
    td.initialState = ResourceState::ShaderResource;
    td.debugName = m_DebugName.c_str();
    m_GrabTexture = device.CreateTexture(td);
    if (!m_GrabTexture.IsValid())
        return false;
    m_Width = width;
    m_Height = height;

    if (!m_Sampler.IsValid())
        m_Sampler = device.CreateSampler(SamplerDesc::MaterialLinearClamp(m_SamplerName.c_str()));
    return m_Sampler.IsValid();
}

bool SceneColorGrab::DeclareForView(Pipeline::ViewDeclare& d, uint32_t frameIndex)
{
    m_ReadyFrameIndex = UINT32_MAX;
    m_GrabRG = {}; // frame-scoped: last frame's id must never leak into this frame

    auto* device = d.Services.GetDevice();
    if (!device)
        return false;
    m_Device = device;

    // Snapshot the named scene-colour resource. For the transmission lobe this is the
    // post-world resolved "SceneColor" (lit opaque) — the read-after-write edge against
    // the world pass's colour write orders this grab after the world pass regardless of
    // the phase tag.
    const RenderGraph::RGTexture sourceColor = d.ResolveTexture(m_SourceName.c_str());
    if (!sourceColor.IsValid())
        return false;

    // Phase-1 scope: a plain sampler2D copy. If the source is multisampled (the MSAA
    // format-mismatch fallback path leaves SceneColor as the MSAA target), decline rather
    // than resolve — the consumer then falls back to the env cube.
    const RenderGraph::RGResourceDesc scd = d.Frame.Graph().ResourceDesc(sourceColor.Id);
    if (scd.SampleCount > 1)
        return false;
    m_Format = scd.Format;

    if (!EnsureTexture(*device, d.RenderWidth, d.RenderHeight))
        return false;
    if (!EnsurePipeline(*device))
        return false;

    // Import the feature-owned grab as external (resting ShaderResource) so the render
    // graph chains barriers against the copy + settle passes and the import is excluded
    // from transient aliasing.
    const RenderGraph::RGTexture grab = d.Frame.ImportExternalTexture(
        m_DebugName.c_str(), m_GrabTexture, ResourceState::ShaderResource,
        static_cast<TextureFormat>(m_Format));

    // Copy: sample the lit-opaque scene into the grab (read source, write grab → the RG
    // inserts the RenderTarget->Sampled edge). MSAA sources declined above, so the
    // sampler2DMS resolve fork is never taken — pass empty handles for it.
    RenderGraph::AddCopyPass(d.Frame, sourceColor, grab, m_DebugName.c_str(),
                             Rendering::PassPhase::kWorldRender, m_CopyPipelineId,
                             GraphicsPipelineId{}, m_CopyLayout, DescriptorSetLayoutDesc{},
                             m_Sampler, d.RenderWidth, d.RenderHeight);

    // Settle: a Sampled read with no writer transitions the grab back to ShaderResource
    // so it rests in a well-defined layout for next frame's import. PreventCulling keeps it.
    d.Frame.AddPass(
        m_SettleName.c_str(), Rendering::PassPhase::kWorldRender,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(grab, RenderGraph::RGTextureRead::Sampled);
            p.PreventCulling();
        },
        [](RenderGraph::RGContext&) {});

    m_ReadyFrameIndex = frameIndex;
    m_GrabRG = grab;
    return true;
}

} // namespace GameEngine::Engine::Renderer
