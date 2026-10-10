#include "Engine/Rendering/ShadowMinMaxPyramid.h"

#include "Engine/Rendering/Pipeline/Nodes/PipelineNodeUtils.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine::Engine::Renderer
{
namespace
{
static_assert(sizeof(ShadowMinMaxPyramid::PushConstants) == 24,
              "PushConstants must match shadow_minmax_reduce.comp's push-constant block");

// pc.mode: which reduction the dispatch runs.
constexpr int32_t kModeDepthToLevel0 = 0; // 4x4 of the D32 cascade depth
constexpr int32_t kModeLevelToLevel = 1;  // 2x2 of the previous (min,max) level

// shadow_minmax_reduce.comp's workgroup tile.
constexpr uint32_t kWorkgroupSize = 8u;

// The base level's full mip chain: floor(log2(baseExtent)) + 1.
uint32_t MipChainLength(uint32_t baseExtent)
{
    uint32_t levels = 1u;
    for (uint32_t extent = baseExtent; extent > 1u; extent >>= 1u)
        ++levels;
    return levels;
}
} // namespace

uint32_t ShadowMinMaxPyramid::LevelsForQuery(uint32_t baseExtent, float queryTexels)
{
    if (baseExtent == 0u)
        return 0u;
    const uint32_t chain = MipChainLength(baseExtent);
    uint32_t levels = 1u;
    float footprint = static_cast<float>(1u << kBaseDownshift);
    // NaN fails the comparison and leaves one level: the shader answers "no
    // bound" for a query no level covers, so this never under-covers silently.
    while (footprint < queryTexels && levels < chain)
    {
        footprint *= 2.0f;
        ++levels;
    }
    return levels;
}

Rendering::RenderGraph::RGTexture ShadowMinMaxPyramid::Declare(
    Rendering::RenderGraph::RGFrame& frame, RenderServices& rs, Rendering::ViewId viewId,
    Rendering::RenderGraph::RGTexture cascadeDepth, uint32_t shadowResolution,
    uint32_t cascadeCount, float widestKernelTexels, bool enabled)
{
    namespace RG = Rendering::RenderGraph;

    // Drop this view's entry up front so EVERY decline path below leaves it at
    // 0 — a stale count outlives a switch away from PCSS and would be published
    // into the UBO as a pyramid that is not there. The frame stamp written on
    // success covers the declines this function never sees at all: a view whose
    // directional light stops casting is not declared here, so it cannot clear
    // itself, and only the stamp makes that entry read as absent.
    m_PyramidByView.erase(viewId);

    // Structural gate: nothing declared, no resource, no dispatch. The A/B this
    // pyramid exists to win is only meaningful if its "off" arm is free.
    if (!enabled || !cascadeDepth.IsValid() || cascadeCount == 0u)
        return {};

    const uint32_t baseExtent = shadowResolution >> static_cast<uint32_t>(kBaseDownshift);
    const uint32_t levels = LevelsForQuery(baseExtent, widestKernelTexels + kQueryMarginTexels);
    if (levels == 0u)
        return {};

    // The depth array bounds the layer count: a dispatch past its last layer
    // would reduce texels that do not exist.
    const auto& depthDesc = frame.Graph().ResourceDesc(cascadeDepth.Id);
    const uint32_t layers = std::min(cascadeCount, depthDesc.ArrayLayers);
    if (layers == 0u)
        return {};

    LoadShader(rs.GetDevice());
    if (!m_PipelineId.IsValid() || !m_Meta || !m_Sampler.IsValid())
        return {};

    // Persistent pool import: the passes bind single-mip views the POOL owns
    // (a pass-side view cache cannot see the realloc that kills the image
    // underneath it), and the import marks the texture an external sink so the
    // chain survives cull in the frames before a consumer samples it.
    Rendering::TextureDesc td{};
    td.width = baseExtent;
    td.height = baseExtent;
    td.depth = 1;
    td.mipLevels = levels;
    td.arrayLayers = layers;
    td.sampleCount = 1;
    td.format = static_cast<uint32_t>(Rendering::TextureFormat::R32G32_FLOAT);
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::UnorderedAccess |
                                     Rendering::TextureUsage::ShaderResource |
                                     Rendering::TextureUsage::TransferSrc);
    // GENERAL for its whole lifetime, both halves of the same fact: each level
    // is storage-written and then sampled by the next level's dispatch (and by
    // the PCSS filter), so the sampled descriptors claim GENERAL rather than
    // ping-ponging the layout mid-chain. initialState makes that true from
    // creation, before the graph's first per-subresource transition, so a
    // whole-level descriptor never spans an untransitioned layer.
    td.sampledInGeneralLayout = true;
    td.initialState = Rendering::ResourceState::UnorderedAccess;
    // The shader binds every level as image2DArray / sampler2DArray, so the
    // views must stay array-shaped even for a single-cascade config — without
    // this a 1-layer pyramid would get 2D views bound to array declarations,
    // which is invalid rather than merely wrong-looking. Same reason
    // ImportShadowMapArrayRG sets it on the cascade depth array.
    td.flags = Rendering::TextureCreateFlags::ForceArrayView;
    const std::string poolName =
        "ShadowMinMaxPyramid.View" + std::to_string(static_cast<uint32_t>(viewId));
    td.debugName = poolName.c_str();
    const RG::RGTexture pyramid = frame.ImportPersistentTexture(poolName.c_str(), td);
    if (!pyramid.IsValid())
        return {};

    // All cascades share the same mip dimensions and descriptors. Dispatch
    // their independent layers in Z so each level needs one pass and one set
    // of barriers/descriptors instead of serializing a pass per cascade.
    for (uint32_t level = 0u; level < levels; ++level)
    {
        const uint32_t dstExtent = std::max(1u, baseExtent >> level);
        DeclareLevelPass(frame, cascadeDepth, pyramid, viewId, layers, level, dstExtent,
                         dstExtent);
    }

    ViewPyramid& entry = m_PyramidByView[viewId];
    entry.Stamp.Stamp(frame);
    entry.State.Physical = frame.PhysicalTexture(pyramid);
    entry.State.Levels = static_cast<int>(levels);
    entry.State.Layers = layers;
    return pyramid;
}

ShadowMinMaxPyramid::FrameState ShadowMinMaxPyramid::StateFor(
    Rendering::ViewId viewId, const Rendering::RenderGraph::RGFrame& frame) const
{
    const auto it = m_PyramidByView.find(viewId);
    if (it == m_PyramidByView.end() || !it->second.Stamp.IsFor(frame))
        return {};
    return it->second.State;
}

void ShadowMinMaxPyramid::OnDeviceRebuilt()
{
    m_PyramidByView.clear();
    // The load is one-shot, so clearing the attempt flag is what lets the next
    // Declare rebuild the pipeline, the reflected layout and the sampler against
    // the new device. Nothing is destroyed here: the rebuild already freed them
    // and these handles only look valid.
    m_LoadAttempted = false;
    m_PipelineId = {};
    m_Meta.reset();
    m_Set0Layout = {};
    m_DstBinding = 0;
    m_Sampler = {};
}

void ShadowMinMaxPyramid::DeclareLevelPass(Rendering::RenderGraph::RGFrame& frame,
                                           Rendering::RenderGraph::RGTexture cascadeDepth,
                                           Rendering::RenderGraph::RGTexture pyramid,
                                           Rendering::ViewId viewId, uint32_t layers,
                                           uint32_t level, uint32_t dstWidth, uint32_t dstHeight)
{
    namespace RG = Rendering::RenderGraph;

    const bool fromDepth = level == 0u;
    const uint32_t srcLevel = fromDepth ? 0u : level - 1u;

    PushConstants pc{};
    pc.DstSize[0] = static_cast<int32_t>(dstWidth);
    pc.DstSize[1] = static_cast<int32_t>(dstHeight);
    pc.LayerCount = static_cast<int32_t>(layers);
    pc.SrcLevel = static_cast<int32_t>(srcLevel);
    pc.Mode = fromDepth ? kModeDepthToLevel0 : kModeLevelToLevel;

    const uint32_t gx = (dstWidth + kWorkgroupSize - 1u) / kWorkgroupSize;
    const uint32_t gy = (dstHeight + kWorkgroupSize - 1u) / kWorkgroupSize;

    const std::string passName = "ShadowMinMaxPyramid.Level" + std::to_string(level) + "[View#" +
                                 std::to_string(static_cast<uint32_t>(viewId)) + "]";

    frame.AddComputePass(
        passName.c_str(), Rendering::PassPhase::kDefault,
        [&](RG::RGPassBuilder& p)
        {
            // Declared ranges match what the pass's DESCRIPTORS reach, not just
            // the texels one dispatch stores: uSrcDepth binds the whole cascade
            // array and uDst a whole mip level. Z workgroups reduce all active
            // layers together, so the array-wide barriers order only the real
            // level-to-level dependency.
            if (fromDepth)
                p.Read(cascadeDepth, RG::RGTextureRead::Sampled);
            else
                p.Read(pyramid, RG::RGTextureRead::Sampled,
                       RG::RGRange{.BaseMip = srcLevel, .MipCount = 1u});
            p.Write(pyramid, RG::RGTextureWrite::Storage,
                    RG::RGRange{.BaseMip = level, .MipCount = 1u});
        },
        [this, cascadeDepth, pyramid, level, layers, fromDepth, pc, gx, gy](RG::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl)
                return;

            // uDst addresses ONE level as an image2DArray, so it needs the
            // pool's per-mip view; the sampled sources bind whole textures
            // (the shader selects its level through pc.srcLevel).
            const Rendering::TextureViewHandle dstView = ctx.GetOrCreatePooledMipView(pyramid, level);
            const Rendering::TextureHandle srcTex =
                fromDepth ? ctx.GetTexture(cascadeDepth) : ctx.GetTexture(pyramid);
            if (!dstView.IsValid() || !srcTex.IsValid())
                return;

            Rendering::DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_Set0Layout;
            dsDesc.transient = true;
            dsDesc.debugName = "ShadowMinMaxPyramid.Set0";
            auto ds = dev->CreateDescriptorSet(dsDesc);
            if (!ds.IsValid())
                return;

            // Both sampled bindings exist in the layout whichever mode runs, and
            // an unwritten descriptor is undefined behaviour on a device without
            // nullDescriptor — so the idle one binds the same texture as the
            // live one rather than being left empty. texelFetch ignores sampler
            // state; the point-clamp sampler is only there to complete the
            // combined-image-sampler descriptors. LoadShader already proved both
            // names resolve, so a placement failure here means the set is not
            // the one the shader was reflected from: dispatching would be the UB
            // the paragraph above exists to avoid.
            Rendering::NamedDescriptorWriter wd(dev, ds, *m_Meta, 0);
            if (!wd.TryAddCombinedImageSampler("uSrcDepth", srcTex, m_Sampler) ||
                !wd.TryAddCombinedImageSampler("uSrcLevel", srcTex, m_Sampler))
                return;
            wd.Flush();
            dev->UpdateStorageImageBinding(ds, m_DstBinding, dstView);

            const Rendering::PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_PipelineId);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->SetPushConstants(pc);
            cl->Dispatch(gx, gy, layers);
        });
}

void ShadowMinMaxPyramid::LoadShader(IDevice* device)
{
    if (m_LoadAttempted || !device)
        return;
    m_LoadAttempted = true;

    Rendering::ShaderPackage pkg{};
    std::string loadErr;
    if (!Rendering::LoadShaderPkg("Shaders/shadow_minmax_reduce.shaderpkg",
                                 device->PreferredShaderSource(), pkg, &loadErr))
    {
        Logger::Log::Warning(
            "ShadowMinMaxPyramid: failed to load shadow_minmax_reduce.shaderpkg: {}", loadErr);
        return;
    }
    auto itCs = pkg.stageBytes.find("cs");
    if (itCs == pkg.stageBytes.end() || itCs->second.empty())
    {
        Logger::Log::Warning("ShadowMinMaxPyramid: shadow_minmax_reduce.shaderpkg missing cs stage");
        return;
    }

    m_Meta = std::make_unique<ShaderMeta>(std::move(pkg.meta));

    // Every binding the dispatch writes must resolve, and it must resolve NOW:
    // a name that goes missing here is a shader-side layout change, and the
    // per-dispatch alternative is either an arbitrary index corrupting a live
    // descriptor (uDst, which is bound by view and so has no
    // NamedDescriptorWriter path) or an unwritten descriptor sampled as UB on a
    // device without nullDescriptor (the two sampled sources).
    Rendering::DescriptorType reflectedType{};
    uint32_t sourceBinding = 0;
    const bool bindingsResolved =
        Pipeline::Nodes::Detail::TryGetSet0BindingByName(*m_Meta, "uDst", m_DstBinding,
                                                         reflectedType) &&
        Pipeline::Nodes::Detail::TryGetSet0BindingByName(*m_Meta, "uSrcDepth", sourceBinding,
                                                         reflectedType) &&
        Pipeline::Nodes::Detail::TryGetSet0BindingByName(*m_Meta, "uSrcLevel", sourceBinding,
                                                         reflectedType);
    if (!bindingsResolved)
    {
        Logger::Log::Warning(
            "ShadowMinMaxPyramid: shadow_minmax_reduce.comp does not declare all of set-0 "
            "'uSrcDepth' / 'uSrcLevel' / 'uDst' — the pyramid cannot be built");
        m_Meta.reset();
        return;
    }

    // Set-0 layout and the push-constant range come straight from reflection, so
    // the descriptor layout is the shader's own.
    Rendering::ComputePipelineDesc cd{};
    cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(itCs->second));
    cd.DebugName = "ShadowMinMaxPyramid";

    m_Set0Layout = Rendering::DescriptorSetLayoutDesc{};
    auto patchLayout = [&](uint32_t setIndex, Rendering::DescriptorSetLayoutDesc& dsl)
    {
        if (setIndex == 0)
            m_Set0Layout = dsl;
    };
    std::string err;
    if (!Rendering::MaterialHelper::ApplyShaderMetaToComputeDesc(
            *device, *m_Meta, cd, Rendering::MaterialBuilder::MergeMode::Auto, {true, 128},
            patchLayout, &err))
    {
        // The set-0 layout and the push-constant range both come from this call.
        // Interning the pipeline anyway would build it against an empty layout —
        // every descriptor the dispatch writes would land on a set that does not
        // describe them.
        Logger::Log::Warning("ShadowMinMaxPyramid: could not apply shadow_minmax_reduce "
                             "reflection to the compute pipeline desc: {}",
                             err);
        m_Meta.reset();
        return;
    }

    m_PipelineId = device->InternComputePipeline(std::move(cd));
    if (!m_Sampler.IsValid())
        m_Sampler = device->CreateSampler(
            Rendering::SamplerDesc::PointClamp("ShadowMinMaxPyramid.Sampler"));
}

} // namespace GameEngine::Engine::Renderer
