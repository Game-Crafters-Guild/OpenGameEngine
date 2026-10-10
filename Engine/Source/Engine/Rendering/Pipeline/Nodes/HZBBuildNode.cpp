#include "Engine/Rendering/Pipeline/Nodes/HZBBuildNode.h"

#include "Engine/Rendering/Pipeline/Nodes/PipelineNodeUtils.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/RenderServices.h"

#include "Assets/ShaderProgramAsset.h"
#include "Logger/Logger.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUCulling.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>

namespace GameEngine::Engine::Renderer { using namespace ::GameEngine::Rendering; }

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

namespace
{
// Field order MUST match hzb_build.comp's PushConstants block.
struct HzbBuildPC
{
    uint32_t dstW;
    uint32_t dstH;
    uint32_t srcW;
    uint32_t srcH;
    uint32_t mode; // 0 = copy depth into mip 0, 1 = 2x2 MIN reduce, 2 = 2x2 MAX reduce
};

// Field order MUST match the PushConstants block shared by hzb_spd.comp and
// hzb_spd_finalize.comp.
struct HzbSpdPC
{
    uint32_t width;
    uint32_t height;
    uint32_t mipCount;
};

// hzb_spd.comp's workgroup tile (64x64 depth texels per workgroup).
constexpr uint32_t kSpdTileSize = 64u;
// The tile pass writes mips 1..6 (uMips[6]); the finalize pass addresses mips
// 1..12 (uMips[12], chain length <= 13).
constexpr uint32_t kSpdTileMipArrayCount = 6u;
constexpr uint32_t kSpdFinalizeMipArrayCount = 12u;
// The finalize pass keeps whole levels >= 7 in fixed-size LDS tiles: mip 7
// must fit 32x32, capping supported depth extents at 4223 per axis (which
// also caps the chain at 13 levels). Larger extents fall back to the chain.
constexpr uint32_t kSpdMaxMip7Extent = 32u;
// hzb_spd_finalize.comp's static shared-memory footprint: the 2-slot edge
// overlays (eCol/eRow: 2 x 2 x 1056 floats = 16896 B) + the tail tiles
// (32^2 + 16^2 floats = 5120 B). That exceeds Vulkan's 16 KiB guaranteed
// minimum for maxComputeSharedMemorySize, so the SPD path is gated on the
// reported device limit — an undersized (or unreporting) device falls back
// to the chain instead of failing finalize pipeline creation every frame,
// which would ship unrepaired fold edges (non-conservative: a too-near HZB
// texel can cull visible instances).
constexpr uint32_t kSpdFinalizeSharedMemoryBytes = 22016u;

// Kill-switch for the two-dispatch SPD build: default ON, GE_HZB_SPD=0
// restores the per-mip chain (A/B parity + escape hatch).
bool SpdEnabled()
{
    static const bool enabled = [] {
        const char* v = std::getenv("GE_HZB_SPD");
        return !(v && std::string_view(v) == "0");
    }();
    return enabled;
}

// floor(log2(max(w,h))) + 1 — the full mip chain for the exact depth extent
// (NOT next-pow2-down: keeping the true extent is what preserves culling
// strength, design §5-A5).
uint32_t MipChainLength(uint32_t w, uint32_t h)
{
    uint32_t levels = 1u;
    uint32_t s = std::max(w, h);
    while (s > 1u)
    {
        s >>= 1u;
        ++levels;
    }
    return levels;
}
} // namespace

bool HZBBuildNode::Initialize(std::string nodeId, std::string nodeJson, std::string* /*outError*/)
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
            if (j.contains("poolName") && j["poolName"].is_string())
                m_PoolPrefix = j["poolName"].get<std::string>();
        }
    }
    catch (const std::exception&)
    {
        // Keep defaults on malformed config.
    }
    return true;
}

void HZBBuildNode::Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& /*ctx*/)
{
    if (auto* dev = instance.GetRenderServices().GetDevice())
        LoadShaders(dev);
}

void HZBBuildNode::DeclareForView(ViewDeclare& d)
{
    if (!m_PipelineId.IsValid())
        return;

    // Reservation guard: each pyramid is built (and published) only for a view
    // that actually consumes it. The occlusion HZB needs a phase-B slice — i.e.
    // an HZB view (HzbCullingStrategy opted it in). Scene panes and the Game
    // View both opt in, so an editor frame routinely carries SEVERAL HZB views
    // through this one node object; probes, thumbnails and any view before its
    // strategy is set have no slicePhase==1 range. Mirrors the no-op paths in
    // OcclusionCullP2Node + ScheduleWorldOcclusionRecoverScatter. The SSR Hi-Z
    // needs an active SSSR volume. A view with neither pays zero cost with the
    // node still enabled in the blueprint.
    GPUCullingPipeline* cull = d.Services.GetGPUCullingPipeline();
    bool hasPhaseB = false;
    if (cull)
    {
        for (const auto& r : cull->GetViewVisibilityRanges())
        {
            if (r.viewId == d.View.id && r.cascadeIndex == kCullingCascadeIndexNone &&
                r.slicePhase == 1u)
            {
                hasPhaseB = true;
                break;
            }
        }
    }
    const bool ssrActive =
        d.Services.GetEffectivePostProcessSettings(d.View.id, d.View.worldId).IsSSSRActive();
    if (!hasPhaseB && !ssrActive)
        return;

    // Source depth: the resolved single-sample R32F (View.DepthResolved) — it
    // carries UnorderedAccess so it can be read as a storage image, matching
    // the pyramid's storage-only binding layout. Falls back to the effective
    // resolved depth value on the ViewDeclare when the key is unpublished.
    RenderGraph::RGTexture depth = d.ResolveTexture(m_InputKey);
    if (!depth.IsValid())
        depth = d.ViewDepthResolved;
    if (!depth.IsValid())
        return;

    const auto& dd = d.Frame.Graph().ResourceDesc(depth.Id);
    const uint32_t w = dd.Width;
    const uint32_t h = dd.Height;
    if (w == 0u || h == 0u)
        return;

    const uint32_t levels = MipChainLength(w, h);

    // POOL import (design's "hi-Z" persistence case): full mip chain at the
    // EXACT depth extent, R32F, storage read+write.
    Rendering::TextureDesc td{};
    td.width = w;
    td.height = h;
    td.depth = 1u;
    td.mipLevels = levels;
    td.arrayLayers = 1u;
    td.sampleCount = 1u;
    td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource);
    // Import and publish whichever pyramids this view consumes, then pick the
    // cheapest build that covers them. SSR needs the OPPOSITE bound to
    // occlusion: a MAX-reduced (nearest-surface) pyramid, so the ray march
    // registers a crossing as soon as it could hit near geometry (the MIN
    // occlusion pyramid over-steps it). Each pyramid keeps its own pool entry,
    // named per view — which is also what scopes its pool-owned mip views.
    const std::string poolName = m_PoolPrefix + std::to_string(static_cast<uint32_t>(d.View.id));
    const std::string ssrPool =
        m_SsrPoolPrefix + std::to_string(static_cast<uint32_t>(d.View.id));

    RenderGraph::RGTexture hzbMin{};
    RenderGraph::RGTexture hzbMax{};
    if (hasPhaseB)
    {
        td.debugName = poolName.c_str();
        hzbMin = d.Frame.ImportPersistentTexture(poolName.c_str(), td);
        if (hzbMin.IsValid())
            d.PublishTexture(m_OutputKey, hzbMin);
    }
    if (ssrActive)
    {
        td.debugName = ssrPool.c_str();
        hzbMax = d.Frame.ImportPersistentTexture(ssrPool.c_str(), td);
        if (hzbMax.IsValid())
            d.PublishTexture(m_SsrOutputKey, hzbMax);
    }

    // Two-dispatch SPD build unless killed, over-large, or unloadable — the
    // chain fallback keeps every pyramid on screen in all of those cases.
    const bool spdCapable = (w >> 7u) <= kSpdMaxMip7Extent && (h >> 7u) <= kSpdMaxMip7Extent;
    const bool spdUsable = SpdEnabled() && spdCapable;

    // Both pyramids wanted: one fused pair of dispatches reads depth once and
    // emits both reductions, replacing the MAX pyramid's entire per-mip chain.
    if (hzbMin.IsValid() && hzbMax.IsValid() && spdUsable && m_SpdMinMaxPipelineId.IsValid() &&
        m_SpdMinMaxFinalizePipelineId.IsValid())
    {
        DeclareSpdMinMaxBuild(d, depth, hzbMin, hzbMax, w, h, levels);
        return;
    }

    if (hzbMin.IsValid())
    {
        if (spdUsable && m_SpdPipelineId.IsValid() && m_SpdFinalizePipelineId.IsValid())
            DeclareSpdBuild(d, depth, hzbMin, w, h, levels);
        else
            DeclareChainBuild(d, depth, hzbMin, w, h, levels, kReduceMin);
    }
    if (hzbMax.IsValid())
        DeclareChainBuild(d, depth, hzbMax, w, h, levels, kReduceMax);
}

void HZBBuildNode::DeclareChainBuild(ViewDeclare& d, RenderGraph::RGTexture depth,
                                     RenderGraph::RGTexture hzb, uint32_t w, uint32_t h,
                                     uint32_t levels, uint32_t reduceMode)
{
    // Mip 0: copy the phase-A depth into the pyramid base (full res).
    d.Frame.AddComputePass(
        d.PassName("HZBBuild.Mip0").c_str(), Rendering::PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(depth, RenderGraph::RGTextureRead::Storage);
            p.Write(hzb, RenderGraph::RGTextureWrite::Storage,
                    RenderGraph::RGRange{.BaseMip = 0u, .MipCount = 1u});
        },
        [this, depth, hzb, w, h](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl)
                return;
            const TextureHandle depthTex = ctx.GetTexture(depth);
            if (!depthTex.IsValid())
                return;
            const TextureViewHandle dstView = ctx.GetOrCreatePooledMipView(hzb, 0u);
            if (!dstView.IsValid())
                return;

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_Set0Layout;
            dsDesc.transient = true;
            dsDesc.debugName = "HZBBuild.Mip0.Set0";
            DescriptorSetHandle ds = dev->CreateDescriptorSet(dsDesc);
            if (!ds.IsValid())
                return;
            dev->UpdateStorageImageBinding(ds, m_SrcBinding, depthTex);
            dev->UpdateStorageImageBinding(ds, m_DstBinding, dstView);

            PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_PipelineId);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            HzbBuildPC pc{w, h, w, h, 0u};
            cl->SetPushConstants(pc);
            cl->Dispatch((w + 7u) / 8u, (h + 7u) / 8u, 1u);
        });

    // Mip m (>=1): conservative 2x2 reduce of mip m-1 under reduceMode (MIN =
    // farthest for occlusion, MAX = nearest for the SSR march). Per-mip
    // Read/Write subresource ranges give the render graph the RAW edge between
    // successive dispatches (RGTypes' Hi-Z case) without self-hazarding.
    for (uint32_t m = 1u; m < levels; ++m)
    {
        const uint32_t srcW = std::max(1u, w >> (m - 1u));
        const uint32_t srcH = std::max(1u, h >> (m - 1u));
        const uint32_t dstW = std::max(1u, w >> m);
        const uint32_t dstH = std::max(1u, h >> m);

        d.Frame.AddComputePass(
            d.PassName(("HZBBuild.Mip" + std::to_string(m)).c_str()).c_str(),
            Rendering::PassPhase::kDefault,
            [&, m](RenderGraph::RGPassBuilder& p)
            {
                p.Read(hzb, RenderGraph::RGTextureRead::Storage,
                       RenderGraph::RGRange{.BaseMip = m - 1u, .MipCount = 1u});
                p.Write(hzb, RenderGraph::RGTextureWrite::Storage,
                        RenderGraph::RGRange{.BaseMip = m, .MipCount = 1u});
            },
            [this, hzb, srcW, srcH, dstW, dstH, m, reduceMode](RenderGraph::RGContext& ctx)
            {
                auto* dev = ctx.GetDevice();
                auto* cl = ctx.Cmd;
                if (!dev || !cl)
                    return;
                const TextureViewHandle srcView = ctx.GetOrCreatePooledMipView(hzb, m - 1u);
                const TextureViewHandle dstView = ctx.GetOrCreatePooledMipView(hzb, m);
                if (!srcView.IsValid() || !dstView.IsValid())
                    return;

                DescriptorSetDesc dsDesc{};
                dsDesc.layout = m_Set0Layout;
                dsDesc.transient = true;
                dsDesc.debugName = "HZBBuild.Reduce.Set0";
                DescriptorSetHandle ds = dev->CreateDescriptorSet(dsDesc);
                if (!ds.IsValid())
                    return;
                dev->UpdateStorageImageBinding(ds, m_SrcBinding, srcView);
                dev->UpdateStorageImageBinding(ds, m_DstBinding, dstView);

                PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_PipelineId);
                if (!pipe.IsValid())
                    return;
                cl->SetPipeline(pipe);
                cl->BindDescriptorSet(0, ds, pipe);
                HzbBuildPC pc{dstW, dstH, srcW, srcH, reduceMode};
                cl->SetPushConstants(pc);
                cl->Dispatch((dstW + 7u) / 8u, (dstH + 7u) / 8u, 1u);
            });
    }
}

void HZBBuildNode::DeclareSpdBuild(ViewDeclare& d, RenderGraph::RGTexture depth,
                                   RenderGraph::RGTexture hzb, uint32_t w, uint32_t h,
                                   uint32_t levels)
{
    const uint32_t wgX = (w + kSpdTileSize - 1u) / kSpdTileSize;
    const uint32_t wgY = (h + kSpdTileSize - 1u) / kSpdTileSize;
    const uint32_t tileMips = std::min(levels, 7u);

    // Tile pass: mips 0..6 per 64x64 tile (LDS-internal, no image re-reads).
    d.Frame.AddComputePass(
        d.PassName("HZBBuild.SPD").c_str(), Rendering::PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(depth, RenderGraph::RGTextureRead::Storage);
            p.Write(hzb, RenderGraph::RGTextureWrite::Storage,
                    RenderGraph::RGRange{.BaseMip = 0u, .MipCount = tileMips});
        },
        [this, depth, hzb, w, h, levels, wgX, wgY](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl)
                return;
            const TextureHandle depthTex = ctx.GetTexture(depth);
            const TextureViewHandle mip0View = ctx.GetOrCreatePooledMipView(hzb, 0u);
            if (!depthTex.IsValid() || !mip0View.IsValid())
                return;

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_SpdSet0Layout;
            dsDesc.transient = true;
            dsDesc.debugName = "HZBBuild.SPD.Set0";
            DescriptorSetHandle ds = dev->CreateDescriptorSet(dsDesc);
            if (!ds.IsValid())
                return;
            dev->UpdateStorageImageBinding(ds, m_SpdDepthBinding, depthTex);
            dev->UpdateStorageImageBinding(ds, m_SpdMip0Binding, mip0View);
            for (uint32_t i = 0; i < kSpdTileMipArrayCount; ++i)
            {
                // Slots past the real chain re-bind the deepest mip: a valid
                // descriptor the shader never addresses (mipCount clamps).
                const uint32_t mip = std::min(i + 1u, levels - 1u);
                dev->UpdateStorageImageBinding(ds, m_SpdMipsBinding,
                                               ctx.GetOrCreatePooledMipView(hzb, mip), i);
            }

            PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_SpdPipelineId);
            if (!pipe.IsValid())
            {
                // Fail visible: a dead tile pipeline leaves the whole pyramid
                // stale, and a stale pyramid under camera motion can cull
                // instances the fresh depth would have kept.
                if (!m_WarnedSpdTilePipeline)
                {
                    m_WarnedSpdTilePipeline = true;
                    LOG_WARNING("HZBBuildNode: SPD tile pipeline creation failed — HZB pyramid "
                                "is not being rebuilt (stale occlusion source)");
                }
                return;
            }
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            HzbSpdPC pc{w, h, levels};
            cl->SetPushConstants(pc);
            cl->Dispatch(wgX, wgY, 1u);
        });

    // Finalize pass: one workgroup repairs the deferred odd-extent folds and
    // builds mips 7+. Chains of 1-2 levels have nothing to repair (mip 1 is
    // exact from the tile pass).
    if (levels < 3u)
        return;

    d.Frame.AddComputePass(
        d.PassName("HZBBuild.SPDFinalize").c_str(), Rendering::PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            // Reads mips 1..6 interior (tile-pass output), writes mips 2..
            // edges + mips 7.. whole. Declared as one identical-range
            // Read+Write pair so the graph merges them into a single storage
            // access (General, ShaderRead|ShaderWrite) — overlapping but
            // DIFFERENT ranges with a write would self-hazard.
            const RenderGraph::RGRange range{.BaseMip = 1u, .MipCount = levels - 1u};
            p.Read(hzb, RenderGraph::RGTextureRead::Storage, range);
            p.Write(hzb, RenderGraph::RGTextureWrite::Storage, range);
        },
        [this, hzb, w, h, levels](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl)
                return;
            // Mip 1 is the shallowest level this pass addresses (levels >= 3
            // here), so an invalid view for it means the pyramid is gone.
            if (!ctx.GetOrCreatePooledMipView(hzb, 1u).IsValid())
                return;

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_SpdFinalizeSet0Layout;
            dsDesc.transient = true;
            dsDesc.debugName = "HZBBuild.SPDFinalize.Set0";
            DescriptorSetHandle ds = dev->CreateDescriptorSet(dsDesc);
            if (!ds.IsValid())
                return;
            for (uint32_t i = 0; i < kSpdFinalizeMipArrayCount; ++i)
            {
                const uint32_t mip = std::min(i + 1u, levels - 1u);
                dev->UpdateStorageImageBinding(ds, m_SpdFinalizeMipsBinding,
                                               ctx.GetOrCreatePooledMipView(hzb, mip), i);
            }

            PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_SpdFinalizePipelineId);
            if (!pipe.IsValid())
            {
                // Fail visible: skipping the finalize ships unrepaired fold
                // edges and stale mips 7+ — NON-conservative (a too-near HZB
                // texel culls visible instances), unlike the benign-stale
                // chain fallback.
                if (!m_WarnedSpdFinalizePipeline)
                {
                    m_WarnedSpdFinalizePipeline = true;
                    LOG_WARNING("HZBBuildNode: SPD finalize pipeline creation failed — pyramid "
                                "fold edges are unrepaired (occlusion may over-cull); set "
                                "GE_HZB_SPD=0 to restore the per-mip chain");
                }
                return;
            }
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            HzbSpdPC pc{w, h, levels};
            cl->SetPushConstants(pc);
            cl->Dispatch(1u, 1u, 1u);
        });
}

bool HZBBuildNode::TryCompileSlangChainShader(ShaderSourceKind kind, ShaderPackage& outPkg)
{
    std::string src;
    try
    {
        src = Rendering::Utils::ResolveShaderPath("Shaders/Slang/hzb_build.slang");
    }
    catch (const std::exception&)
    {
        // No resolver configured (headless tests) — nothing staged to compile.
    }
    if (src.empty())
    {
        LOG_WARNING("HZBBuildNode: Slang lane is on but Shaders/Slang/hzb_build.slang did not "
                    "resolve; falling back to the prebuilt GLSL package");
        return false;
    }
    const std::filesystem::path& cacheRoot = ShaderProgramAsset::GetShaderCacheRoot();
    if (cacheRoot.empty())
    {
        LOG_WARNING("HZBBuildNode: Slang lane is on but the shader cache root is unset; "
                    "falling back to the prebuilt GLSL package");
        return false;
    }

    ShaderProgramCompileRequest req{};
    req.debugName = "hzb_build.slang";
    const std::filesystem::path srcPath(src);
    req.baseDirectory = srcPath.parent_path();
    req.cacheRoot = cacheRoot;
    ShaderStageCompileSpec spec{};
    spec.stage = "cs";
    spec.sourcePath = srcPath;
    req.stages.push_back(std::move(spec));

    ShaderProgramCompileResult res{};
    std::string err;
    if (!ShaderCompileService::CompileProgramToCache(req, kind, res, &err))
    {
        LOG_WARNING("HZBBuildNode: Slang-lane compile of {} failed; falling back to the "
                    "prebuilt GLSL package:\n{}", src, err);
        return false;
    }
    outPkg.meta = std::move(res.meta);
    outPkg.stageBytes = std::move(res.stageBytes);
    Logger::Log::Info("HZBBuildNode: chain shader compiled via the Slang lane ({})", src);
    return true;
}

void HZBBuildNode::LoadShaders(IDevice* device)
{
    if (m_LoadAttempted)
        return;
    m_LoadAttempted = true;

    ShaderPackage pkg{};
    bool haveSlangPkg = false;
    if (ShaderCompileService::IsSlangLaneEnabled())
        haveSlangPkg = TryCompileSlangChainShader(device->PreferredShaderSource(), pkg);

    std::string loadErr;
    if (!haveSlangPkg && !LoadShaderPkg("Shaders/hzb_build.shaderpkg", device->PreferredShaderSource(), pkg, &loadErr))
    {
        LOG_WARNING("HZBBuildNode: failed to load Shaders/hzb_build.shaderpkg: {}", loadErr);
        return;
    }
    auto itCs = pkg.stageBytes.find("cs");
    if (itCs == pkg.stageBytes.end() || itCs->second.empty())
    {
        LOG_WARNING("HZBBuildNode: hzb_build.shaderpkg missing cs stage");
        return;
    }

    // Resolve the two storage-image binding indices from the shader's reflected
    // meta by name, so a layout edit in hzb_build.comp stays in lockstep with
    // the C++ binds. Only the INDEX is trusted from reflection — the descriptor
    // type is written explicitly (kStorageImage(4)/kCombinedImageSampler(5) do
    // not line up with DescriptorType), and a reflection miss falls back to the
    // GLSL literal with a warning.
    auto resolveBinding = [&](const char* name, uint32_t fallback) -> uint32_t {
        uint32_t binding = fallback;
        DescriptorType reflectedType{};
        if (!Detail::TryGetSet0BindingByName(pkg.meta, name, binding, reflectedType))
        {
            LOG_WARNING("HZBBuildNode: failed to resolve set0 binding '{}' from reflection; "
                        "falling back to literal {}",
                        name, fallback);
            return fallback;
        }
        return binding;
    };
    m_SrcBinding = resolveBinding("uSrc", 0u);
    m_DstBinding = resolveBinding("uDst", 1u);

    // Explicit set-0 layout: source (readonly storage image) + dest (writeonly
    // storage image), each at its reflected binding index. Matches hzb_build.comp.
    m_Set0Layout = DescriptorSetLayoutDesc{};
    m_Set0Layout.debugName = "HZBBuild_SetLayout";
    for (uint32_t binding : {m_SrcBinding, m_DstBinding})
    {
        DescriptorBinding b{};
        b.binding = binding;
        b.type = DescriptorType::StorageImage;
        b.count = 1u;
        b.shaderStages = kShaderStageCompute;
        // hzb_build.comp declares both images r32f, src readonly. WebGPU bakes
        // format and access into the layout and rejects a mismatch outright.
        b.storageTexelFormat = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
        b.storageReadOnly = (binding == m_SrcBinding);
        m_Set0Layout.bindings.push_back(b);
    }

    ComputePipelineDesc cd{};
    cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(itCs->second));
    cd.DescriptorSetLayouts.push_back(device->InternDescriptorSetLayout(m_Set0Layout));
    cd.PushConstants.Size = sizeof(HzbBuildPC);
    cd.PushConstants.StageMask = kShaderStageCompute;
    cd.DebugName = "HZBBuild";
    m_PipelineId = device->InternComputePipeline(std::move(cd));

    LoadSpdShader(device);
}

void HZBBuildNode::LoadSpdShader(IDevice* device)
{
    // Two-dispatch variant; a load failure leaves the pipeline ids invalid and
    // the node on the per-mip chain. Only binding INDICES are trusted from
    // reflection (see LoadShaders); types and array counts are authored here
    // to match the shaders.

    // Device gate: the finalize shader's static LDS exceeds Vulkan's
    // guaranteed minimum — verify the reported limit up front so pipeline
    // creation can't fail per-frame (see kSpdFinalizeSharedMemoryBytes).
    const uint32_t sharedLimit = device->GetCapabilities().maxComputeSharedMemorySize;
    if (sharedLimit < kSpdFinalizeSharedMemoryBytes)
    {
        LOG_WARNING("HZBBuildNode: device maxComputeSharedMemorySize {} B < {} B required by "
                    "hzb_spd_finalize; using the per-mip chain",
                    sharedLimit, kSpdFinalizeSharedMemoryBytes);
        return;
    }

    const auto loadCs = [&](const char* path, ShaderPackage& pkg) -> bool {
        std::string loadErr;
        if (!LoadShaderPkg(path, device->PreferredShaderSource(), pkg, &loadErr))
        {
            LOG_WARNING("HZBBuildNode: failed to load {} ({}); using the per-mip chain", path,
                        loadErr);
            return false;
        }
        auto it = pkg.stageBytes.find("cs");
        if (it == pkg.stageBytes.end() || it->second.empty())
        {
            LOG_WARNING("HZBBuildNode: {} missing cs stage; using the per-mip chain", path);
            return false;
        }
        return true;
    };
    const auto resolveBinding = [](const ShaderPackage& pkg, const char* name,
                                   uint32_t fallback) -> uint32_t {
        uint32_t binding = fallback;
        DescriptorType reflectedType{};
        if (!Detail::TryGetSet0BindingByName(pkg.meta, name, binding, reflectedType))
        {
            LOG_WARNING("HZBBuildNode: failed to resolve set0 binding '{}' from SPD "
                        "reflection; falling back to literal {}",
                        name, fallback);
            return fallback;
        }
        return binding;
    };
    const auto addBinding = [](DescriptorSetLayoutDesc& layout, uint32_t binding,
                               uint32_t count) {
        DescriptorBinding b{};
        b.binding = binding;
        b.type = DescriptorType::StorageImage;
        b.count = count;
        b.shaderStages = kShaderStageCompute;
        layout.bindings.push_back(b);
    };

    ShaderPackage tilePkg{};
    ShaderPackage finalizePkg{};
    if (!loadCs("Shaders/hzb_spd.shaderpkg", tilePkg) ||
        !loadCs("Shaders/hzb_spd_finalize.shaderpkg", finalizePkg))
        return;

    m_SpdDepthBinding = resolveBinding(tilePkg, "uDepth", 0u);
    m_SpdMip0Binding = resolveBinding(tilePkg, "uMip0", 1u);
    m_SpdMipsBinding = resolveBinding(tilePkg, "uMips", 2u);
    m_SpdSet0Layout = DescriptorSetLayoutDesc{};
    m_SpdSet0Layout.debugName = "HZBBuildSPD_SetLayout";
    addBinding(m_SpdSet0Layout, m_SpdDepthBinding, 1u);
    addBinding(m_SpdSet0Layout, m_SpdMip0Binding, 1u);
    addBinding(m_SpdSet0Layout, m_SpdMipsBinding, kSpdTileMipArrayCount);

    m_SpdFinalizeMipsBinding = resolveBinding(finalizePkg, "uMips", 0u);
    m_SpdFinalizeSet0Layout = DescriptorSetLayoutDesc{};
    m_SpdFinalizeSet0Layout.debugName = "HZBBuildSPDFinalize_SetLayout";
    addBinding(m_SpdFinalizeSet0Layout, m_SpdFinalizeMipsBinding, kSpdFinalizeMipArrayCount);

    auto tileCs = tilePkg.stageBytes.find("cs");
    ComputePipelineDesc cd{};
    cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(tileCs->second));
    cd.DescriptorSetLayouts.push_back(device->InternDescriptorSetLayout(m_SpdSet0Layout));
    cd.PushConstants.Size = sizeof(HzbSpdPC);
    cd.PushConstants.StageMask = kShaderStageCompute;
    cd.DebugName = "HZBBuildSPD";
    m_SpdPipelineId = device->InternComputePipeline(std::move(cd));

    auto finCs = finalizePkg.stageBytes.find("cs");
    ComputePipelineDesc fd{};
    fd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(finCs->second));
    fd.DescriptorSetLayouts.push_back(device->InternDescriptorSetLayout(m_SpdFinalizeSet0Layout));
    fd.PushConstants.Size = sizeof(HzbSpdPC);
    fd.PushConstants.StageMask = kShaderStageCompute;
    fd.DebugName = "HZBBuildSPDFinalize";
    m_SpdFinalizePipelineId = device->InternComputePipeline(std::move(fd));

    LoadSpdMinMaxShader(device);
}

void HZBBuildNode::LoadSpdMinMaxShader(IDevice* device)
{
    // Fused MIN+MAX variant. Reached only from LoadSpdShader's tail, so the
    // maxComputeSharedMemorySize gate has already passed — the fused shaders
    // reuse one set of LDS tiles across two sequential phases and so need
    // exactly the MIN-only path's shared memory. A load failure leaves the
    // pipeline ids invalid and each pyramid on its own build.
    const auto loadCs = [&](const char* path, ShaderPackage& pkg) -> bool {
        std::string loadErr;
        if (!LoadShaderPkg(path, device->PreferredShaderSource(), pkg, &loadErr))
        {
            LOG_WARNING("HZBBuildNode: failed to load {} ({}); building the SSR Hi-Z separately",
                        path, loadErr);
            return false;
        }
        auto it = pkg.stageBytes.find("cs");
        if (it == pkg.stageBytes.end() || it->second.empty())
        {
            LOG_WARNING("HZBBuildNode: {} missing cs stage; building the SSR Hi-Z separately",
                        path);
            return false;
        }
        return true;
    };
    const auto resolveBinding = [](const ShaderPackage& pkg, const char* name,
                                   uint32_t fallback) -> uint32_t {
        uint32_t binding = fallback;
        DescriptorType reflectedType{};
        if (!Detail::TryGetSet0BindingByName(pkg.meta, name, binding, reflectedType))
        {
            LOG_WARNING("HZBBuildNode: failed to resolve set0 binding '{}' from fused SPD "
                        "reflection; falling back to literal {}",
                        name, fallback);
            return fallback;
        }
        return binding;
    };
    const auto addBinding = [](DescriptorSetLayoutDesc& layout, uint32_t binding,
                               uint32_t count) {
        DescriptorBinding b{};
        b.binding = binding;
        b.type = DescriptorType::StorageImage;
        b.count = count;
        b.shaderStages = kShaderStageCompute;
        layout.bindings.push_back(b);
    };

    ShaderPackage tilePkg{};
    ShaderPackage finalizePkg{};
    if (!loadCs("Shaders/hzb_spd_minmax.shaderpkg", tilePkg) ||
        !loadCs("Shaders/hzb_spd_minmax_finalize.shaderpkg", finalizePkg))
        return;

    m_SpdMinMaxDepthBinding = resolveBinding(tilePkg, "uDepth", 0u);
    m_SpdMinMaxMip0MinBinding = resolveBinding(tilePkg, "uMip0Min", 1u);
    m_SpdMinMaxMipsMinBinding = resolveBinding(tilePkg, "uMipsMin", 2u);
    m_SpdMinMaxMip0MaxBinding = resolveBinding(tilePkg, "uMip0Max", 3u);
    m_SpdMinMaxMipsMaxBinding = resolveBinding(tilePkg, "uMipsMax", 4u);
    m_SpdMinMaxSet0Layout = DescriptorSetLayoutDesc{};
    m_SpdMinMaxSet0Layout.debugName = "HZBBuildSPDMinMax_SetLayout";
    addBinding(m_SpdMinMaxSet0Layout, m_SpdMinMaxDepthBinding, 1u);
    addBinding(m_SpdMinMaxSet0Layout, m_SpdMinMaxMip0MinBinding, 1u);
    addBinding(m_SpdMinMaxSet0Layout, m_SpdMinMaxMipsMinBinding, kSpdTileMipArrayCount);
    addBinding(m_SpdMinMaxSet0Layout, m_SpdMinMaxMip0MaxBinding, 1u);
    addBinding(m_SpdMinMaxSet0Layout, m_SpdMinMaxMipsMaxBinding, kSpdTileMipArrayCount);

    m_SpdMinMaxFinalizeMipsMinBinding = resolveBinding(finalizePkg, "uMipsMin", 0u);
    m_SpdMinMaxFinalizeMipsMaxBinding = resolveBinding(finalizePkg, "uMipsMax", 1u);
    m_SpdMinMaxFinalizeSet0Layout = DescriptorSetLayoutDesc{};
    m_SpdMinMaxFinalizeSet0Layout.debugName = "HZBBuildSPDMinMaxFinalize_SetLayout";
    addBinding(m_SpdMinMaxFinalizeSet0Layout, m_SpdMinMaxFinalizeMipsMinBinding,
               kSpdFinalizeMipArrayCount);
    addBinding(m_SpdMinMaxFinalizeSet0Layout, m_SpdMinMaxFinalizeMipsMaxBinding,
               kSpdFinalizeMipArrayCount);

    auto tileCs = tilePkg.stageBytes.find("cs");
    ComputePipelineDesc cd{};
    cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(tileCs->second));
    cd.DescriptorSetLayouts.push_back(device->InternDescriptorSetLayout(m_SpdMinMaxSet0Layout));
    cd.PushConstants.Size = sizeof(HzbSpdPC);
    cd.PushConstants.StageMask = kShaderStageCompute;
    cd.DebugName = "HZBBuildSPDMinMax";
    m_SpdMinMaxPipelineId = device->InternComputePipeline(std::move(cd));

    auto finCs = finalizePkg.stageBytes.find("cs");
    ComputePipelineDesc fd{};
    fd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(finCs->second));
    fd.DescriptorSetLayouts.push_back(
        device->InternDescriptorSetLayout(m_SpdMinMaxFinalizeSet0Layout));
    fd.PushConstants.Size = sizeof(HzbSpdPC);
    fd.PushConstants.StageMask = kShaderStageCompute;
    fd.DebugName = "HZBBuildSPDMinMaxFinalize";
    m_SpdMinMaxFinalizePipelineId = device->InternComputePipeline(std::move(fd));
}

void HZBBuildNode::DeclareSpdMinMaxBuild(ViewDeclare& d, RenderGraph::RGTexture depth,
                                         RenderGraph::RGTexture hzbMin,
                                         RenderGraph::RGTexture hzbMax, uint32_t w, uint32_t h,
                                         uint32_t levels)
{
    const uint32_t wgX = (w + kSpdTileSize - 1u) / kSpdTileSize;
    const uint32_t wgY = (h + kSpdTileSize - 1u) / kSpdTileSize;
    const uint32_t tileMips = std::min(levels, 7u);

    // Tile pass: mips 0..6 of BOTH pyramids per 64x64 tile. The depth block
    // each thread loads is consumed by both reductions, which is the read this
    // fusion saves; LDS is reused across the two phases, not doubled.
    d.Frame.AddComputePass(
        d.PassName("HZBBuild.SPDMinMax").c_str(), Rendering::PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(depth, RenderGraph::RGTextureRead::Storage);
            p.Write(hzbMin, RenderGraph::RGTextureWrite::Storage,
                    RenderGraph::RGRange{.BaseMip = 0u, .MipCount = tileMips});
            p.Write(hzbMax, RenderGraph::RGTextureWrite::Storage,
                    RenderGraph::RGRange{.BaseMip = 0u, .MipCount = tileMips});
        },
        [this, depth, hzbMin, hzbMax, w, h, levels, wgX, wgY](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl)
                return;
            const TextureHandle depthTex = ctx.GetTexture(depth);
            const TextureViewHandle minMip0 = ctx.GetOrCreatePooledMipView(hzbMin, 0u);
            const TextureViewHandle maxMip0 = ctx.GetOrCreatePooledMipView(hzbMax, 0u);
            if (!depthTex.IsValid() || !minMip0.IsValid() || !maxMip0.IsValid())
                return;

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_SpdMinMaxSet0Layout;
            dsDesc.transient = true;
            dsDesc.debugName = "HZBBuild.SPDMinMax.Set0";
            DescriptorSetHandle ds = dev->CreateDescriptorSet(dsDesc);
            if (!ds.IsValid())
                return;
            dev->UpdateStorageImageBinding(ds, m_SpdMinMaxDepthBinding, depthTex);
            dev->UpdateStorageImageBinding(ds, m_SpdMinMaxMip0MinBinding, minMip0);
            dev->UpdateStorageImageBinding(ds, m_SpdMinMaxMip0MaxBinding, maxMip0);
            for (uint32_t i = 0; i < kSpdTileMipArrayCount; ++i)
            {
                // Slots past the real chain re-bind the deepest mip: a valid
                // descriptor the shader never addresses (mipCount clamps).
                const uint32_t mip = std::min(i + 1u, levels - 1u);
                dev->UpdateStorageImageBinding(ds, m_SpdMinMaxMipsMinBinding,
                                               ctx.GetOrCreatePooledMipView(hzbMin, mip), i);
                dev->UpdateStorageImageBinding(ds, m_SpdMinMaxMipsMaxBinding,
                                               ctx.GetOrCreatePooledMipView(hzbMax, mip), i);
            }

            PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_SpdMinMaxPipelineId);
            if (!pipe.IsValid())
            {
                // Fail visible: a dead tile pipeline leaves both pyramids
                // stale, and a stale occlusion pyramid under camera motion can
                // cull instances the fresh depth would have kept.
                if (!m_WarnedSpdMinMaxTilePipeline)
                {
                    m_WarnedSpdMinMaxTilePipeline = true;
                    LOG_WARNING("HZBBuildNode: fused SPD tile pipeline creation failed — HZB and "
                                "SSR Hi-Z pyramids are not being rebuilt (stale sources)");
                }
                return;
            }
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            HzbSpdPC pc{w, h, levels};
            cl->SetPushConstants(pc);
            cl->Dispatch(wgX, wgY, 1u);
        });

    // Finalize pass: one workgroup repairs the deferred odd-extent folds and
    // builds mips 7+ for both pyramids. Chains of 1-2 levels have nothing to
    // repair (mip 1 is exact from the tile pass).
    if (levels < 3u)
        return;

    d.Frame.AddComputePass(
        d.PassName("HZBBuild.SPDMinMaxFinalize").c_str(), Rendering::PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            // Reads mips 1..6 interior (tile-pass output), writes mips 2..
            // edges + mips 7.. whole. Declared as one identical-range
            // Read+Write pair per pyramid so the graph merges each into a
            // single storage access (General, ShaderRead|ShaderWrite) —
            // overlapping but DIFFERENT ranges with a write would self-hazard.
            const RenderGraph::RGRange range{.BaseMip = 1u, .MipCount = levels - 1u};
            p.Read(hzbMin, RenderGraph::RGTextureRead::Storage, range);
            p.Write(hzbMin, RenderGraph::RGTextureWrite::Storage, range);
            p.Read(hzbMax, RenderGraph::RGTextureRead::Storage, range);
            p.Write(hzbMax, RenderGraph::RGTextureWrite::Storage, range);
        },
        [this, hzbMin, hzbMax, w, h, levels](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl)
                return;
            // Mip 1 is the shallowest level this pass addresses (levels >= 3
            // here), so an invalid view for it means a pyramid is gone.
            if (!ctx.GetOrCreatePooledMipView(hzbMin, 1u).IsValid() ||
                !ctx.GetOrCreatePooledMipView(hzbMax, 1u).IsValid())
                return;

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_SpdMinMaxFinalizeSet0Layout;
            dsDesc.transient = true;
            dsDesc.debugName = "HZBBuild.SPDMinMaxFinalize.Set0";
            DescriptorSetHandle ds = dev->CreateDescriptorSet(dsDesc);
            if (!ds.IsValid())
                return;
            for (uint32_t i = 0; i < kSpdFinalizeMipArrayCount; ++i)
            {
                const uint32_t mip = std::min(i + 1u, levels - 1u);
                dev->UpdateStorageImageBinding(ds, m_SpdMinMaxFinalizeMipsMinBinding,
                                               ctx.GetOrCreatePooledMipView(hzbMin, mip), i);
                dev->UpdateStorageImageBinding(ds, m_SpdMinMaxFinalizeMipsMaxBinding,
                                               ctx.GetOrCreatePooledMipView(hzbMax, mip), i);
            }

            PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_SpdMinMaxFinalizePipelineId);
            if (!pipe.IsValid())
            {
                // A skipped finalize leaves the deferred folds unrepaired,
                // which over-culls (the MIN pyramid reads too near).
                if (!m_WarnedSpdMinMaxFinalizePipeline)
                {
                    m_WarnedSpdMinMaxFinalizePipeline = true;
                    LOG_WARNING("HZBBuildNode: fused SPD finalize pipeline creation failed — "
                                "pyramid folds are unrepaired (over-culling)");
                }
                return;
            }
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            HzbSpdPC pc{w, h, levels};
            cl->SetPushConstants(pc);
            cl->Dispatch(1u, 1u, 1u);
        });
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
