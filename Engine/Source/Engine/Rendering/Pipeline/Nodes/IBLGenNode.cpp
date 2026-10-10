#include "Engine/Rendering/Pipeline/Nodes/IBLGenNode.h"

#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/IEnvironmentSource.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyEnvironmentSource.h"
#include "Engine/Rendering/SkyRenderFeature.h"

#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace GameEngine::Rendering;

namespace
{
// 16x16 workgroup for brdf_lut.comp; must match its local_size_{x,y}.
constexpr uint32_t kBrdfGroup = 16;

// set 0, binding 0 = the BRDF LUT storage image. Built once so the intern-time
// pipeline layout and the execute-time transient descriptor cannot drift apart.
DescriptorSetLayoutDesc BrdfLutSet0(bool rg16Storage)
{
    DescriptorSetLayoutDesc set0{};
    set0.bindings = {{0, DescriptorType::StorageImage, 1, kShaderStageCompute}};
    // Must name the LUT's actual format: rg16f where the backend can store to
    // it, the widened rgba16f otherwise (see CreateBrdfLut). A layout left at
    // the RGBA8 default is rejected against either.
    set0.bindings[0].storageTexelFormat = static_cast<uint32_t>(
        rg16Storage ? TextureFormat::R16G16_FLOAT : TextureFormat::R16G16B16A16_FLOAT);
    set0.debugName = "IBL_BrdfLut_Set0";
    return set0;
}
} // namespace

bool IBLGenNode::Initialize(std::string nodeId, std::string /*nodeJson*/, std::string* /*outError*/)
{
    m_Id = std::move(nodeId);
    return true;
}

void IBLGenNode::ScheduleBrdfLutBake(RenderGraph::RGFrame& frame, IDevice& device,
                                     Engine::Renderer::ImageBasedLightingFeature& feature)
{
    // Intern the BRDF LUT compute pipeline once (set 0, binding 0 = storage image).
    if (!m_BrdfPipelineId.IsValid() && !m_BrdfShaderTried)
    {
        m_BrdfShaderTried = true;
        std::vector<uint8_t> cs = Utils::LoadShaderFile("Shaders/brdf_lut.comp.spv");
        if (!cs.empty())
        {
            ComputePipelineDesc cd{};
            cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(cs));
            cd.DescriptorSetLayouts.push_back(device.InternDescriptorSetLayout(
                BrdfLutSet0(device.GetCapabilities().supportsRG16FloatStorage)));
            cd.DebugName = "IBL_BrdfLut";
            m_BrdfPipelineId = device.InternComputePipeline(cd);
        }
    }
    if (!m_BrdfPipelineId.IsValid())
        return;
    // Environment-independent: bakes exactly once. The dirty gate AT DECLARATION
    // replaces the old activation predicate — not declaring is not running.
    if (feature.IsBrdfLutBaked())
        return;

    Engine::Renderer::ImageBasedLightingFeature* feat = &feature;
    const ComputePipelineId pipeId = m_BrdfPipelineId;

    // 1-layer import; Common is honest for the first/only bake (never-computed
    // content is discardable). Dedup-by-handle fuses with the world-pass import.
    const RenderGraph::RGTexture lut = frame.ImportExternalTexture(
        "IBL_BrdfLut", feature.GetBrdfLut(), ResourceState::Common, TextureFormat{}, 1, 1);
    frame.AddComputePass(
        "IBLGen.BrdfLut", PassPhase::kEarlySetup,
        [lut](RenderGraph::RGPassBuilder& p) {
            p.Write(lut, RenderGraph::RGTextureWrite::Storage);
            p.PreventCulling();
        },
        [feat, pipeId](RenderGraph::RGContext& c) {
            auto* cl = c.Cmd;
            auto* dev = c.GetDevice();
            if (!cl || !dev)
                return;
            PipelineHandle pipe = c.GetOrCreatePipelineVariant(pipeId);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);

            DescriptorSetDesc dsd{};
            dsd.layout = BrdfLutSet0(dev->GetCapabilities().supportsRG16FloatStorage);
            dsd.transient = true;
            dsd.debugName = "IBL_BrdfLut_DS0";
            DescriptorSetHandle ds = dev->CreateDescriptorSet(dsd);
            dev->UpdateStorageImageBinding(ds, 0, feat->GetBrdfLut());
            cl->BindDescriptorSet(0, ds, pipe);

            const uint32_t g =
                (ImageBasedLightingFeature::kBrdfLutSize + kBrdfGroup - 1) / kBrdfGroup;
            cl->Dispatch(g, g, 1);
            feat->MarkBrdfLutBaked();
        });
    // Resting-state contract: the bake leaves the LUT GENERAL on the compute
    // queue; the export contract restores ShaderReadOnly at frame end (the
    // sentinel transition records on the queue that last touched the LUT, with
    // derived submission edges), so the resting state every later import
    // declares (ResourceState::ShaderResource, RenderServicesWorldPass) is
    // TRUE. The graph-authoritative oldLayout rule records import claims
    // verbatim: a false claim parks the image in GENERAL under sampling
    // (VUID-09600 startup storm).
    frame.MarkOutput(lut, RenderGraph::RGImageLayout::ShaderReadOnly);
}

void IBLGenNode::Declare(RenderPipelineInstance& /*instance*/, const PipelineDeclareContext& ctx)
{
    GameEngine::Rendering::IDevice* device = ctx.Services.GetDevice();
    if (!device)
        return;

    auto& feature = ctx.Services.EnsureFeature<ImageBasedLightingFeature>();
    if (!feature.IsInitialized())
        feature.Initialize(device);
    if (!feature.IsInitialized())
        return;

    // Wire the analytic-sky source once. The feature owns it (engine-shared
    // lifetime) so the digest-gated bake passes never reference a dead source. The
    // feature keeps the ambient-fallback cubes until the source first bakes.
    if (!feature.GetEnvironmentSource())
        feature.SetEnvironmentSource(std::make_unique<Engine::Renderer::SkyEnvironmentSource>(ctx.Services));

    // Push the active source's dynamic IBL intensity into the feature, then refresh
    // this frame's EnvData UBO element. The UBO is ringed one element per device frame,
    // so a small rewrite every frame can't tear an in-flight GPU read. Nothing here
    // binds it — the world pass and the ocean/grass contributors each refresh and bind
    // the frame's element themselves — so the returned handle is not needed.
    if (auto* iblSource = feature.GetEnvironmentSource())
        feature.SetIblIntensity(std::max(0.0f, iblSource->IblIntensity()));
    // When the IBL source has no active content (the sky is disabled or absent) zero
    // the intensity so surfaces stop being lit by the stale last-baked cube. The bake
    // itself already skips on the same condition, and a zero intensity makes the leftover
    // cube contribute nothing until a real sky returns and re-bakes. HasActiveContent() is
    // the cheap predicate (no per-frame InputDigest hash); an active HDRI/Skybox stays lit.
    if (auto* iblSource = feature.GetEnvironmentSource(); !iblSource || !iblSource->HasActiveContent())
        feature.SetIblIntensity(0.0f);
    feature.UploadEnvData(device);

    ScheduleBrdfLutBake(ctx.Frame, *device, feature);

    if (auto* source = feature.GetEnvironmentSource())
    {
        Engine::Renderer::EnvironmentBakeContext bakeCtx{};
        bakeCtx.Frame = &ctx.Frame;
        bakeCtx.Device = device;
        bakeCtx.Feature = &feature;
        bakeCtx.DeltaTimeSeconds = ctx.DeltaTimeSeconds;
        source->ScheduleBake(bakeCtx);
    }
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
