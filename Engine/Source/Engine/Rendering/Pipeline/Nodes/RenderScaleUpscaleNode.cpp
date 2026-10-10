#include "Engine/Rendering/Pipeline/Nodes/RenderScaleUpscaleNode.h"

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"

#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <nlohmann/json.hpp>

#include <utility>
#include <vector>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

bool RenderScaleUpscaleNode::Initialize(std::string nodeId, std::string nodeJson,
                                        std::string* /*outError*/)
{
    m_Id = std::move(nodeId);
    try
    {
        auto j = nlohmann::json::parse(nodeJson);
        if (j.is_object())
        {
            if (j.contains("input") && j["input"].is_string())
                m_InputKey = j["input"].get<std::string>();
            if (j.contains("output") && j["output"].is_string())
                m_OutputKey = j["output"].get<std::string>();
        }
    }
    catch (const std::exception&)
    {
        // Keep defaults on malformed config.
    }
    return true;
}

bool RenderScaleUpscaleNode::LoadShaders(IDevice* device)
{
    if (m_UpscalePipelineId.IsValid())
        return m_DepthUpsample.EnsureLoaded(device);
    if (device == nullptr)
        return false;

    if (!m_LinearClampSampler.IsValid())
        m_LinearClampSampler =
            device->CreateSampler(SamplerDesc::MaterialLinearClamp("SpatialUpscale.LinearClamp"));

    ShaderPackage pkg{};
    std::string loadErr;
    if (!LoadShaderPkg("Shaders/spatial_upscale.shaderpkg", device->PreferredShaderSource(), pkg, &loadErr))
    {
        Logger::Log::Warning("RenderScaleUpscaleNode '{}': failed to load "
                             "spatial_upscale.shaderpkg: {}",
                             m_Id, loadErr);
        return false;
    }
    auto itVs = pkg.stageBytes.find("vs");
    auto itFs = pkg.stageBytes.find("fs");
    if (itVs == pkg.stageBytes.end() || itFs == pkg.stageBytes.end())
    {
        Logger::Log::Warning("RenderScaleUpscaleNode '{}': spatial_upscale.shaderpkg missing vs/fs",
                             m_Id);
        return false;
    }

    m_UpscaleMeta = std::make_unique<ShaderMeta>(std::move(pkg.meta));
    GraphicsPipelineDesc gd{};
    gd.Kind = GraphicsPipelineKind::VertexFragment;
    gd.VertexShader = std::make_shared<const std::vector<uint8_t>>(std::move(itVs->second));
    gd.PixelShader = std::make_shared<const std::vector<uint8_t>>(std::move(itFs->second));
    gd.DebugName = "SpatialUpscale";
    gd.Rasterization.cullMode = CullModeFlagBits::None;
    gd.DepthStencil.depthTestEnable = false;
    gd.DepthStencil.depthWriteEnable = false;
    DynamicStateInfo dyn{};
    dyn.states = {DynamicState::Viewport, DynamicState::Scissor};
    gd.DynamicState = dyn;
    m_UpscaleSet0Layout = DescriptorSetLayoutDesc{};
    auto patchLayout = [this](uint32_t setIndex, DescriptorSetLayoutDesc& dsl)
    {
        if (setIndex == 0)
            m_UpscaleSet0Layout = dsl;
    };
    std::string err;
    MaterialHelper::ApplyShaderMetaToGraphicsDesc(*device, *m_UpscaleMeta, gd,
                                                  MaterialBuilder::MergeMode::Auto, {true, 128},
                                                  patchLayout, &err);
    m_UpscalePipelineId = device->InternGraphicsPipeline(std::move(gd));
    return m_UpscalePipelineId.IsValid() && m_DepthUpsample.EnsureLoaded(device);
}

void RenderScaleUpscaleNode::DeclareForView(ViewDeclare& d)
{
    const RenderGraph::RGTexture input = d.ResolveTexture(m_InputKey);
    // Stitch-through: the output name must resolve for downstream consumers
    // whether the crossing runs or not. It is published on every path that
    // declines below, and NEVER before the output ref is resolved — publishing
    // first would shadow the blueprint resource under its own name, so the
    // output resolve would hand back the internal-extent input and the crossing
    // would decline itself.
    auto stitchThrough = [&]()
    {
        if (input.IsValid())
            d.PublishTexture(m_OutputKey, input);
    };

    // The split is active exactly when the pre-pass preserved the caller's
    // display-res targets (see RenderPipelineInstance::Declare).
    if (!d.ViewOutputColor.IsValid() || !d.ViewOutputDepth.IsValid() || !input.IsValid() ||
        d.OutputWidth == 0 || d.OutputHeight == 0 || d.RenderWidth == 0 || d.RenderHeight == 0 ||
        (d.OutputWidth == d.RenderWidth && d.OutputHeight == d.RenderHeight))
    {
        stitchThrough();
        return;
    }

    // Already crossed: a TAA view's temporal resolve upscaled and republished
    // View.Resolve, so its output sits at the display extent and a second
    // resample would only soften it.
    const auto& inDesc = d.Frame.Graph().ResourceDesc(input.Id);
    if (inDesc.Width == d.OutputWidth && inDesc.Height == d.OutputHeight)
    {
        stitchThrough();
        return;
    }

    if (!LoadShaders(d.Services.GetDevice()))
    {
        // Stitched through: the internal-res image reaches the display-res
        // consumers, which magnify it bilinearly — soft but on screen. Retries
        // next frame.
        if (!m_WarnedShadersUnstaged)
        {
            m_WarnedShadersUnstaged = true;
            Logger::Log::Warning("RenderScaleUpscaleNode '{}': upscale shaders not staged yet; "
                                 "the internal-resolution crossing is bilinear passthrough until "
                                 "they load",
                                 m_Id);
        }
        stitchThrough();
        return;
    }

    // The output resolves through the blueprint, whose declaration carries
    // extent.basis "output" — that is what places this node on the display side
    // of the boundary. A render-basis output would make the node a no-op copy.
    const RenderGraph::RGTexture outColor = d.ResolveTexture(m_OutputKey);
    if (!outColor.IsValid())
    {
        stitchThrough();
        return;
    }
    const auto& outDesc = d.Frame.Graph().ResourceDesc(outColor.Id);
    if (outDesc.Width != d.OutputWidth || outDesc.Height != d.OutputHeight)
    {
        if (!m_WarnedOutputBasis)
        {
            m_WarnedOutputBasis = true;
            Logger::Log::Warning(
                "RenderScaleUpscaleNode '{}': output '{}' materialized {}x{} but the view's display "
                "extent is {}x{} — the blueprint resource needs extent.basis \"output\"; crossing "
                "skipped",
                m_Id, m_OutputKey, outDesc.Width, outDesc.Height, d.OutputWidth, d.OutputHeight);
        }
        stitchThrough();
        return;
    }

    const uint32_t outW = d.OutputWidth;
    const uint32_t outH = d.OutputHeight;
    d.Frame.AddPass(
        d.PassName("SpatialUpscale").c_str(), Rendering::PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            RenderGraph::RGAttachmentOps ops{};
            // Full-surface overwrite: the fullscreen triangle writes every pixel.
            ops.Load = RenderGraph::RGLoadOp::DontCare;
            ops.Store = RenderGraph::RGStoreOp::Store;
            p.AttachColor(0, outColor, ops);
            p.Read(input, RenderGraph::RGTextureRead::Sampled);
        },
        [this, input, outW, outH](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl || !m_UpscaleMeta)
                return;
            const auto srcTex = ctx.GetTexture(input);
            if (!srcTex.IsValid() || !m_LinearClampSampler.IsValid())
                return;
            cl->SetViewport(0.0f, 0.0f, static_cast<float>(outW), static_cast<float>(outH));
            cl->SetScissor(0, 0, outW, outH);

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_UpscaleSet0Layout;
            dsDesc.transient = true;
            dsDesc.debugName = "SpatialUpscale.Set0";
            auto ds = dev->CreateDescriptorSet(dsDesc);
            {
                NamedDescriptorWriter wd(dev, ds, *m_UpscaleMeta, 0);
                if (!wd.TryAddCombinedImageSampler("uSource", srcTex, m_LinearClampSampler))
                {
                    if (!m_WarnedBindingMismatch)
                    {
                        m_WarnedBindingMismatch = true;
                        Logger::Log::Error("RenderScaleUpscaleNode: descriptor name mismatch vs "
                                           "spatial_upscale reflection — pass skipped");
                    }
                    return;
                }
                wd.Flush();
            }
            const PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_UpscalePipelineId);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            const struct { float outputExtent[4]; } params{{float(outW), float(outH), 0.0f, 0.0f}};
            cl->SetPushConstants(params);
            cl->Draw(3, 1);
        });

    d.PublishTexture(m_OutputKey, outColor);

    // Redirect the final-output slot to the caller's display-res colour:
    // View.Resolve served the world half as the internal SceneColor up to here;
    // from this point on (bloom -> ... -> FinalCopy, and the blueprint's
    // outputs.FinalColor) it must be the display-res target. Without this the
    // terminal stage would resample the finished image DOWN into the internal
    // target.
    d.PublishTexture(Names::View::Resolve, d.ViewOutputColor);

    // Reconstitute the caller's display-res depth: the editor's overlay and
    // gizmo passes attach it for depth testing at the display extent.
    m_DepthUpsample.Declare(d.Frame, d.ViewDepth, d.ViewOutputDepth, outW, outH,
                            d.PassName("RenderScaleDepthUpsample"));
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
