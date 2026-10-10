#include "Ocean/OceanSceneGrab.h"
#include "Ocean/OceanShaderProgram.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "OceanShaderDirectory.h"

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineDescTranslator.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGFullscreen.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Logger/Logger.h"

#include <array>
#include <filesystem>
#include <string>

namespace GameEngine::Ocean
{

using namespace ::GameEngine::Rendering;
namespace fs = std::filesystem;

namespace
{
constexpr uint32_t kSceneGrabFragmentStage = kShaderStageFragment;

} // namespace

OceanSceneGrab::~OceanSceneGrab()
{
    if (!m_Device)
        return;
    for (auto& [_, grab] : m_GrabsByView)
    {
        if (grab.Texture.IsValid())
            m_Device->DestroyTexture(grab.Texture);
    }
    if (m_Sampler.IsValid())
        m_Device->DestroySampler(m_Sampler);
}

bool OceanSceneGrab::EnsurePipeline(IDevice& device)
{
    if (!m_PipelineLoadAttempted)
    {
        m_PipelineLoadAttempted = true; // one attempt: a missing pkg declines the grab
        const bool loaded = RenderGraph::LoadCopyPipelineDesc(
            device.PreferredShaderSource(), "Ocean.SceneGrab.Copy", "Ocean.SceneGrab.Copy.Set0",
            m_CopyLayout, m_CopyPipeline);
        if (!loaded && !m_WarnedLoadFailed)
        {
            m_WarnedLoadFailed = true;
            Logger::Log::Warning(
                "OceanSceneGrab: copy.shaderpkg unavailable; refraction declines (opaque ocean).");
        }
    }
    if (!m_CopyPipelineId.IsValid() && !m_CopyPipeline.vertexShader.empty())
        m_CopyPipelineId = PipelineDescTranslator::InternGraphics(device, m_CopyPipeline);
    return m_CopyPipelineId.IsValid();
}

bool OceanSceneGrab::EnsureResolvePipeline(IDevice& device)
{
    if (!m_ResolveLoadAttempted)
    {
        m_ResolveLoadAttempted = true;
        const fs::path shaderDir = OceanShaderDirectory("ocean_scene_resolve.frag");
        if (!shaderDir.empty())
        {
            ShaderProgramCompileRequest req{};
            req.debugName = "ocean_scenegrab_resolve";
            req.baseDirectory = shaderDir;
            req.cacheRoot = fs::path(".Cache") / "Shaders";
            req.includeDirs = {shaderDir.parent_path()};
            req.stages = {{"vs", "ocean_fullscreen.vert", "main", {}},
                          {"fs", "ocean_scene_resolve.frag", "main", {}}};
            ShaderProgramCompileResult result{};
            std::string err;
            if (LoadOceanShaderProgram(req, device.PreferredShaderSource(), result, &err))
            {
                auto vs = result.stageBytes.find("vs");
                auto fsIt = result.stageBytes.find("fs");
                if (vs != result.stageBytes.end() && fsIt != result.stageBytes.end() &&
                    !vs->second.empty() && !fsIt->second.empty())
                {
                    m_ResolveLayout.debugName = "Ocean.SceneGrab.Resolve.Set0";
                    m_ResolveLayout.bindings = {
                        {0, DescriptorType::CombinedImageSampler, 1, kSceneGrabFragmentStage},
                        {1, DescriptorType::CombinedImageSampler, 1, kSceneGrabFragmentStage},
                        {2, DescriptorType::UniformBuffer, 1, kSceneGrabFragmentStage}};
                    m_ResolvePipeline = {};
                    m_ResolvePipeline.type = PipelineType::Graphics;
                    m_ResolvePipeline.vertexShader = vs->second;
                    m_ResolvePipeline.pixelShader = fsIt->second;
                    m_ResolvePipeline.rasterizationSamples = 1;
                    m_ResolvePipeline.EnableDepthTest(false);
                    m_ResolvePipeline.SetCullingMode(CullModeFlagBits::None);
                    m_ResolvePipeline.AddDynamicState(DynamicState::Viewport);
                    m_ResolvePipeline.AddDynamicState(DynamicState::Scissor);
                    Rendering::ApplyMetaImageShapeToLayout(result.meta, m_ResolveLayout);
                    m_ResolvePipeline.descriptorSetLayouts.push_back(m_ResolveLayout);
                    m_ResolvePipeline.debugName = "Ocean.SceneGrab.Resolve";
                }
            }
            else
            {
                Logger::Log::Warning("OceanSceneGrab: MSAA resolve compile failed: {}", err);
            }
        }
    }
    if (!m_ResolvePipelineId.IsValid() && !m_ResolvePipeline.vertexShader.empty())
        m_ResolvePipelineId = PipelineDescTranslator::InternGraphics(device, m_ResolvePipeline);
    return m_ResolvePipelineId.IsValid();
}

bool OceanSceneGrab::EnsureTexture(IDevice& device, ViewGrab& grab,
                                   uint32_t width, uint32_t height, uint32_t format)
{
    if (width == 0 || height == 0)
        return false;
    if (grab.Texture.IsValid() && width == grab.Width && height == grab.Height &&
        format == grab.Format)
        return true;

    if (grab.Texture.IsValid())
        device.DestroyTexture(grab.Texture);

    TextureDesc td{};
    td.width = width;
    td.height = height;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.sampleCount = 1;
    td.format = format;
    td.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    // Resting state ShaderResource: the grab settles here so the world pass's
    // descriptor-direct sample sees a stable layout, and next frame's grab
    // imports from a well-defined state.
    td.initialState = ResourceState::ShaderResource;
    td.debugName = "Ocean_SceneGrab";
    grab.Texture = device.CreateTexture(td);
    if (!grab.Texture.IsValid())
        return false;
    grab.Width = width;
    grab.Height = height;
    grab.Format = format;

    if (!m_Sampler.IsValid())
        m_Sampler = device.CreateSampler(SamplerDesc::MaterialLinearClamp("Ocean_SceneGrab_Sampler"));
    return m_Sampler.IsValid();
}

bool OceanSceneGrab::DeclareForView(Engine::Renderer::Pipeline::ViewDeclare& d, uint32_t frameIndex,
                                    RenderGraph::RGTexture* outGraphTexture)
{
    auto* device = d.Services.GetDevice();
    if (!device)
        return false;
    m_Device = device;
    ViewGrab& viewGrab = m_GrabsByView[static_cast<uint32_t>(d.View.id)];
    viewGrab.ReadyFrameIndex = UINT32_MAX;
    viewGrab.DepthMatched = false;

    // Snapshot the opaque scene colour accumulated so far. The post-world ocean
    // node sees View.EffectiveColor from the World node: under MSAA this is the
    // resolved SceneColor, while no-MSAA pipelines may write SceneColor directly.
    // Fallback to View.Color for simpler pipelines that do not publish it.
    RenderGraph::RGTexture sourceColor = d.ResolveTexture(Engine::Renderer::Pipeline::Names::View::EffectiveColor);
    if (!sourceColor.IsValid())
        sourceColor = d.ResolveTexture(Engine::Renderer::Pipeline::Names::View::Color);
    if (!sourceColor.IsValid())
        return false;

    // Keep the opaque MSAA samples until this water-specific resolve. The normal
    // presentation resolve already mixes sky into partially covered silhouettes,
    // while the depth resolve retains sample zero. Refracting that pair
    // produces a bright outline around submerged objects.
    const auto rawColor = d.ResolveTexture(Engine::Renderer::Pipeline::Names::View::Color);
    if (rawColor.IsValid() && d.Frame.Graph().ResourceDesc(rawColor.Id).SampleCount > 1)
        sourceColor = rawColor;

    // When the pre-world colour is multisampled, a sampler2D copy can't read it.
    // Resolve the MSAA samples in a sampler2DMS copy instead of declining, so
    // refraction works with MSAA on. The grab texture itself is always
    // single-sample.
    const RenderGraph::RGResourceDesc scd = d.Frame.Graph().ResourceDesc(sourceColor.Id);
    const bool msaa = scd.SampleCount > 1;

    if (!EnsureTexture(*device, viewGrab, d.RenderWidth, d.RenderHeight, scd.Format))
        return false;
    const bool pipeReady = msaa ? EnsureResolvePipeline(*device) : EnsurePipeline(*device);
    if (!pipeReady)
        return false;

    // Import the feature-owned grab as external (resting in ShaderResource) so
    // the render graph chains barriers against the copy + settle passes.
    const std::string grabName =
        "Ocean_SceneGrab_View" + std::to_string(static_cast<uint32_t>(d.View.id));
    const RenderGraph::RGTexture grab = d.Frame.ImportExternalTexture(
        grabName.c_str(), viewGrab.Texture, ResourceState::ShaderResource,
        static_cast<TextureFormat>(viewGrab.Format));
    if (outGraphTexture)
        *outGraphTexture = grab;

    // Grab: sample the scene-as-built into the grab texture (read View.Color,
    // write grab → the RG inserts the RenderTarget->Sampled edge). The helper
    // picks the sampler2DMS resolve fork off the source's SampleCount.
    if (msaa)
    {
        const auto sourceDepth = d.ResolveTexture(Engine::Renderer::Pipeline::Names::View::Depth);
        const auto depthDesc = sourceDepth.IsValid()
            ? d.Frame.Graph().ResourceDesc(sourceDepth.Id) : RenderGraph::RGResourceDesc{};
        const bool depthMatches = sourceDepth.IsValid() && depthDesc.SampleCount == scd.SampleCount &&
                                  depthDesc.Width == scd.Width && depthDesc.Height == scd.Height;
        struct alignas(16) ResolveParams
        {
            uint32_t DepthAware, Padding[3];
        };
        static_assert(sizeof(ResolveParams) == 16);
        const auto params = d.Frame.AllocUpload<ResolveParams>();
        if (!params.Ptr)
            return false;
        *params.Ptr = {depthMatches ? 1u : 0u, {0u, 0u, 0u}};
        viewGrab.DepthMatched = depthMatches;
        std::array<RenderGraph::RGFullscreenTextureInput, 2> inputs{};
        inputs[0].Binding = 0;
        inputs[0].Texture = sourceColor;
        inputs[0].Sampler = m_Sampler;
        inputs[0].Required = true;
        inputs[1].Binding = 1;
        inputs[1].Texture = depthMatches ? sourceDepth : sourceColor;
        inputs[1].Sampler = m_Sampler;
        inputs[1].Required = true;
        RenderGraph::RGFullscreenBufferInput uniform{};
        uniform.Binding = 2;
        uniform.Buffer = params.Buffer;
        uniform.Offset = params.Offset;
        uniform.Size = sizeof(ResolveParams);
        RenderGraph::RGFullscreenDesc pass{};
        pass.Name = "OceanSceneGrabMSAA";
        pass.Phase = Rendering::PassPhase::kWorldRender;
        pass.Pipeline = m_ResolvePipelineId;
        pass.Layout = m_ResolveLayout;
        pass.Textures = inputs;
        pass.Buffers = {&uniform, 1};
        pass.Target = grab;
        pass.Ops.Load = RenderGraph::RGLoadOp::DontCare;
        pass.Ops.Store = RenderGraph::RGStoreOp::Store;
        pass.Width = d.RenderWidth;
        pass.Height = d.RenderHeight;
        RenderGraph::AddFullscreenPass(d.Frame, pass);
    }
    else
    {
        RenderGraph::AddCopyPass(d.Frame, sourceColor, grab, "OceanSceneGrab",
                                 Rendering::PassPhase::kWorldRender, m_CopyPipelineId,
                                 m_ResolvePipelineId, m_CopyLayout, m_ResolveLayout, m_Sampler,
                                 d.RenderWidth, d.RenderHeight);
    }

    // The same-frame consumer (OceanSurface) declares its Sampled read through
    // AddForwardCommandPassForView's sampledTextures span; MarkOutput covers the
    // frames where the surface declines — it sink-anchors the copy against
    // culling and restores ShaderReadOnly so next frame's import claim holds.
    d.Frame.MarkOutput(grab, RenderGraph::RGImageLayout::ShaderReadOnly);

    viewGrab.ReadyFrameIndex = frameIndex;
    return true;
}

RenderGraph::RGTexture OceanSceneGrab::ImportForSampling(RenderGraph::RGFrame& frame,
                                                         uint32_t viewId,
                                                         uint32_t frameIndex) const
{
    if (!IsReady(viewId, frameIndex))
        return {};
    const auto it = m_GrabsByView.find(viewId);
    const std::string grabName = "Ocean_SceneGrab_View" + std::to_string(viewId);
    return frame.ImportExternalTexture(grabName.c_str(), it->second.Texture,
                                       ResourceState::ShaderResource,
                                       static_cast<TextureFormat>(it->second.Format));
}

} // namespace GameEngine::Ocean
