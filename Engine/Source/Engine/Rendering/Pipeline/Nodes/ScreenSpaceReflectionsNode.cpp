#include "Engine/Rendering/Pipeline/Nodes/ScreenSpaceReflectionsNode.h"

#include "Engine/Rendering/CameraAspectRatio.h"
#include "Engine/Rendering/Pipeline/Nodes/PipelineNodeUtils.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/RenderServices.h"

#include "Core/Time.h"
#include "Logger/Logger.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Common/Math.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Utils/TextureUploadHelpers.h"

#include "SssrBlueNoiseData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

namespace
{
// A ray record packs its pixel as x | (y << 16) into one uint
// (sssr_classify.comp), so each dimension must fit in 16 bits.
constexpr uint32_t kMaxPackedRayDimension = 0xffffu;
// Ray-list slots past the appended count are never fetched on the normal path —
// sssr_intersect.comp gates on Args.rayCount first. The sentinel keeps a stale
// slot benign if the count and the emitted grid ever disagree.
constexpr uint32_t kEmptyRayRecord = 0xffffffffu;
// Side of a denoise tile and of an average-radiance block. Every SSSR compute
// pass dispatches 8x8 workgroups, classify appends one list entry per tile, and
// reproject reduces one workgroup to one average-radiance texel — so the tile
// grid, the tile-list capacity and the average target's extent are all this one
// number. Must match kSssrAvgRadianceBlock in Includes/sssr_common.glsl.
constexpr uint32_t kDenoiseTileSize = 8u;

uint32_t SssrDebugMode()
{
    static const uint32_t value = []
    {
        if (const char* text = std::getenv("GE_SSSR_DEBUG"))
            return static_cast<uint32_t>(std::clamp(std::atoi(text), 0, 3));
        return 0u;
    }();
    return value;
}

// Pins the frame counter the SSSR shaders animate their blue noise and rate
// control from, so successive frames — and separate builds — produce the same
// stochastic sequence for the same pose. That turns an A/B comparison of two
// binaries into an exact diff instead of a noise-bounded one. Unset in every
// normal run; only the push constant is pinned, never the history bookkeeping,
// which would falsely invalidate history every frame.
std::optional<uint32_t> SssrFramePin()
{
    static const std::optional<uint32_t> value = []() -> std::optional<uint32_t>
    {
        const char* text = std::getenv("GE_SSSR_FRAME_PIN");
        if (!text || *text == '\0')
            return std::nullopt;
        return static_cast<uint32_t>(std::max(0, std::atoi(text)));
    }();
    return value;
}
} // namespace

ScreenSpaceReflectionsNode::~ScreenSpaceReflectionsNode()
{
    if (!m_Device)
        return;
    if (m_BlueNoiseTexture.IsValid())
        m_Device->DestroyTexture(m_BlueNoiseTexture);
    if (m_PointSampler.IsValid())
        m_Device->DestroySampler(m_PointSampler);
    if (m_LinearSampler.IsValid())
        m_Device->DestroySampler(m_LinearSampler);
}

bool ScreenSpaceReflectionsNode::Initialize(std::string nodeId, std::string nodeJson,
                                            std::string* outError)
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
    catch (const std::exception& e)
    {
        if (outError)
            *outError = std::string("JSON parse failed: ") + e.what();
        return false;
    }
    return true;
}

void ScreenSpaceReflectionsNode::Declare(RenderPipelineInstance& instance,
                                         const PipelineDeclareContext& /*ctx*/)
{
    if (auto* device = instance.GetRenderServices().GetDevice())
        LoadShaders(device);
}

DescriptorSetHandle ScreenSpaceReflectionsNode::NewSet(IDevice* device, Program program,
                                                        const char* debugName) const
{
    DescriptorSetDesc desc{};
    desc.layout = m_Programs[program].Set0Layout;
    desc.transient = true;
    desc.debugName = debugName;
    return device->CreateDescriptorSet(desc);
}

PipelineHandle ScreenSpaceReflectionsNode::BeginProgram(RenderGraph::RGContext& ctx,
                                                         Program program,
                                                         DescriptorSetHandle set,
                                                         const PostProcessSettings& settings,
                                                         uint32_t frameIndex,
                                                         bool historyValid,
                                                         const Mathematics::Rect& viewportRect) const
{
    const ComputeProgram& p = m_Programs[program];
    PipelineHandle pipeline = ctx.GetOrCreatePipelineVariant(p.Pipeline);
    if (!pipeline.IsValid() || !ctx.Cmd)
        return {};
    ctx.Cmd->SetPipeline(pipeline);
    if (p.Meta && !p.Meta->PushConstants.empty())
    {
        NamedPushConstantWriter writer(*p.Meta, p.Meta->PushConstants[0].Name);
        if (writer.IsValid())
        {
            writer.Add("sssrIntensity", settings.SSSRIntensity);
            writer.Add("sssrMaxDistance", settings.SSSRMaxDistance);
            writer.Add("sssrThickness", settings.SSSRThickness);
            writer.Add("sssrEdgeFade", settings.SSSREdgeFade);
            writer.Add("sssrMaxSteps", settings.SSSRMaxSteps);
            writer.Add("frameIndex", frameIndex);
            writer.Add("historyValid", historyValid ? 1u : 0u);
            writer.Add("debugMode", SssrDebugMode());
            writer.Add("sampleQuality", settings.SSSRSampleQuality);
            writer.Add("multiBounce", settings.SSSRMultiBounce);
            // Rect is four consecutive floats in the shader's member order.
            static_assert(sizeof(Mathematics::Rect) == 4 * sizeof(float));
            writer.Add("viewportRect", viewportRect);
            writer.Flush(ctx.Cmd);
        }
    }
    ctx.Cmd->BindDescriptorSet(0, set, pipeline);
    return pipeline;
}

Mathematics::Rect ScreenSpaceReflectionsNode::ComputeViewportRect(const ViewLetterbox& letterbox,
                                                                  uint32_t targetWidth,
                                                                  uint32_t targetHeight)
{
    const Mathematics::Rect wholeTarget{0.0f, 0.0f, 1.0f, 1.0f};
    if (!letterbox.active || targetWidth == 0 || targetHeight == 0 || letterbox.width == 0 ||
        letterbox.height == 0)
        return wholeTarget;
    const float w = static_cast<float>(targetWidth);
    const float h = static_cast<float>(targetHeight);
    return Mathematics::Rect{static_cast<float>(letterbox.x) / w,
                             static_cast<float>(letterbox.y) / h,
                             static_cast<float>(letterbox.width) / w,
                             static_cast<float>(letterbox.height) / h};
}

void ScreenSpaceReflectionsNode::DeclareForView(ViewDeclare& d)
{
    const RenderGraph::RGTexture input = d.ResolveTexture(m_InputKey);
    // Resolve the actual pool-backed output before publishing the inactive
    // stitch-through under the same blackboard name.
    const RenderGraph::RGTexture output = d.ResolveTexture(m_OutputKey);
    if (input.IsValid())
        d.PublishTexture(m_OutputKey, input);

    const PostProcessSettings settings =
        d.Services.GetEffectivePostProcessSettings(d.View.id, d.View.worldId);
    if (!settings.IsSSSRActive() || !m_ShadersLoaded || !input.IsValid() || !output.IsValid())
    {
        if (!m_LoggedSkipReason)
        {
            m_LoggedSkipReason = true;
            LOG_INFO("ScreenSpaceReflectionsNode: view {} skipping SSSR "
                     "(active={} intensity={} maxDistance={} maxSteps={} shaders={} "
                     "input={} output={})",
                     static_cast<uint32_t>(d.View.id), settings.IsSSSRActive(),
                     settings.SSSRIntensity, settings.SSSRMaxDistance, settings.SSSRMaxSteps,
                     m_ShadersLoaded, input.IsValid(), output.IsValid());
        }
        return;
    }

    const RenderGraph::RGTexture depth = d.ViewDepthResolved;
    // Nearest-surface (MAX) pyramid: the intersect's crossing test needs the tile
    // NEAREST depth, not the occlusion HZB's farthest bound (which over-steps).
    const RenderGraph::RGTexture hzb = d.ResolveTexture(Names::View::SSRHiZ);
    // The G-buffer comes only through the *.Written names ReflectionsProvider
    // publishes on frames it attaches the SSR MRT. Resolving the raw names
    // would succeed even on frames nothing writes them (blueprint-declared
    // resources materialize on demand; MSAA withholds the attachments), so a
    // valid handle alone is not evidence the surfaces exist.
    const auto specularWeight = d.ResolveTexture(Names::View::SSRSpecularWeightWritten);
    const auto specularRadiance = d.ResolveTexture(Names::View::SSRSpecularRadianceWritten);
    const RenderGraph::RGTexture normalRoughness =
        d.ResolveTexture(Names::View::NormalRoughnessWritten);
    if (!normalRoughness.IsValid() || !specularWeight.IsValid() || !specularRadiance.IsValid())
    {
        if (!m_WarnedGBufferMissing)
        {
            m_WarnedGBufferMissing = true;
            LOG_WARNING("ScreenSpaceReflectionsNode: SSSR is active for view {} but the world "
                        "pass did not write the SSR G-buffer this frame — skipping SSSR. Likely "
                        "cause: MSAA on this view (the SSR targets are single-sample); disable "
                        "MSAA or disable SSSR for this view.",
                        static_cast<uint32_t>(d.View.id));
        }
        return;
    }
    const PipelineBufferBindingRG viewParams = d.ResolveBuffer(Names::Res::ViewParams);
    if (!depth.IsValid() || !hzb.IsValid() || !viewParams.IsValid())
    {
        if (!m_LoggedSkipReason)
        {
            m_LoggedSkipReason = true;
            LOG_WARNING("ScreenSpaceReflectionsNode: view {} skipping SSSR — "
                        "depth={} hzb={} viewParams={} (HZBBuild publishes View.SSRHiZ only "
                        "while SSSR is active)",
                        static_cast<uint32_t>(d.View.id), depth.IsValid(), hzb.IsValid(),
                        viewParams.IsValid());
        }
        return;
    }

    const uint32_t width = d.RenderWidth;
    const uint32_t height = d.RenderHeight;
    if (width == 0 || height == 0 || width > kMaxPackedRayDimension ||
        height > kMaxPackedRayDimension)
    {
        if (!m_LoggedSkipReason)
        {
            m_LoggedSkipReason = true;
            LOG_WARNING("ScreenSpaceReflectionsNode: view {} skipping SSSR — extent {}x{}",
                        static_cast<uint32_t>(d.View.id), width, height);
        }
        return;
    }

    // The kernels convert between target UV and the camera's NDC on every ray,
    // so they need the rectangle the world was actually rasterized into. It is
    // the same ViewLetterbox the motion pass reads to decide its sentinel.
    const Mathematics::Rect viewportRect =
        ComputeViewportRect(d.Services.Views().GetViewLetterbox(d.View.id), width, height);

    const auto motionVectors = m_MotionVectors.Declare(d);
    if (!motionVectors.IsValid())
    {
        if (!m_LoggedSkipReason)
        {
            m_LoggedSkipReason = true;
            LOG_WARNING("ScreenSpaceReflectionsNode: view {} skipping SSSR — motion vectors unavailable",
                        static_cast<uint32_t>(d.View.id));
        }
        return;
    }
    struct ReprojectionParams { float PrevInvViewProj[16]; };
    auto reprojectionParams = d.Frame.AllocUpload<ReprojectionParams>();
    if (!reprojectionParams.Valid()) return;
    const ViewDeformationClock clock =
        d.Services.TemporalHistory().ResolveDeformationClock(Time::GetCumulativeSeconds());
    const ViewTemporalSample currentSample{d.Services.Views().ResolveCameraData(d.View.id),
                                           clock.TimeSeconds,
                                           d.Services.GetScrollAnimationTimeSeconds(), clock.Origin};
    // A declare path: the rotation takes the frame stream's submitted-frame
    // count so a declared-and-abandoned frame never becomes a later previous.
    const auto* previousSample = d.Services.TemporalHistory().Advance(
        d.View.id, d.Frame.FrameIndex(), currentSample, d.Frame.SubmittedFrameCount());
    Matrix4x4 previousVP;
    std::memcpy(previousVP.Data(), previousSample->Camera.viewProj,
                sizeof(previousSample->Camera.viewProj));
    const auto previousInvVP = GameEngine::Mathematics::Inverse(previousVP);
    std::memcpy(reprojectionParams.Ptr->PrevInvViewProj, previousInvVP.Data(), sizeof(float) * 16);

    const uint64_t frameIndex64 = d.Frame.FrameIndex();
    ViewHistory& vh = m_ViewHistory[static_cast<uint32_t>(d.View.id)];
    // History is per-view and always holds THIS view's previous render, so its
    // validity does not depend on the frame index being exactly +1. The graph
    // frame index is a per-window stream counter, and an OnDemand view that
    // lapses (hidden tab, collapsed pane, inactive split) resumes with an
    // arbitrary jump in it — a strict +1 test would throw away valid surviving
    // history on every such resume and leave fresh stochastic noise every
    // frame. The reproject pass's per-pixel depth test handles disocclusion
    // when the gap is large or the camera moved.
    //
    // Whether the physical we read still holds what we wrote is the pool's
    // question, not ours: an idle age-out, a resize realloc or a device rebuild
    // all hand back a fresh, undefined physical under the same name and desc,
    // which extent equality cannot detect. historyFresh is filled by the imports
    // below and folded into historyValid there.
    const bool historyRendered = vh.HistoryFrame > 0ull && vh.LastWrittenFrame != frameIndex64 &&
                                 vh.Width == width && vh.Height == height;
    const uint32_t parity = static_cast<uint32_t>(vh.HistoryFrame++ & 1ull);
    vh.LastWrittenFrame = frameIndex64;
    vh.Width = width;
    vh.Height = height;

    TextureDesc workDesc{};
    workDesc.width = width;
    workDesc.height = height;
    workDesc.mipLevels = 1;
    workDesc.arrayLayers = 1;
    workDesc.sampleCount = 1;
    workDesc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    workDesc.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess |
                                           TextureUsage::ShaderResource);

    workDesc.debugName = "SSSR.Radiance";
    const auto radiance = d.Frame.CreateTexture(d.PassName("Radiance").c_str(), workDesc);
    workDesc.debugName = "SSSR.HitData";
    const auto hitData = d.Frame.CreateTexture(d.PassName("HitData").c_str(), workDesc);
    workDesc.debugName = "SSSR.Reprojected";
    const auto reprojected = d.Frame.CreateTexture(d.PassName("Reprojected").c_str(), workDesc);
    workDesc.debugName = "SSSR.ReprojectedMoments";
    const auto reprojectedMoments =
        d.Frame.CreateTexture(d.PassName("ReprojectedMoments").c_str(), workDesc);
    workDesc.debugName = "SSSR.Prefiltered";
    const auto prefiltered = d.Frame.CreateTexture(d.PassName("Prefiltered").c_str(), workDesc);

    // One workgroup of every SSSR pass covers one tile, so this is both the
    // dispatch grid and the average-radiance target's extent.
    const uint32_t gx = (width + kDenoiseTileSize - 1u) / kDenoiseTileSize;
    const uint32_t gy = (height + kDenoiseTileSize - 1u) / kDenoiseTileSize;

    // Block-scale reflection reference: reproject reduces each of its workgroups
    // to one texel here, the prefilter samples it back bilinearly. 1/64 of the
    // frame's area, and the widest support in the denoise chain.
    TextureDesc avgDesc = workDesc;
    avgDesc.width = gx;
    avgDesc.height = gy;
    avgDesc.debugName = "SSSR.AverageRadiance";
    const auto avgRadiance =
        d.Frame.CreateTexture(d.PassName("AverageRadiance").c_str(), avgDesc);

    TextureDesc historyDesc = workDesc;
    historyDesc.persistent = true;
    const std::string historyBase =
        "SSSR.View" + std::to_string(static_cast<uint32_t>(d.View.id));
    historyDesc.debugName = "SSSR.History";
    const auto historyWrite = d.Frame.ImportPersistentTexture(
        (historyBase + ".History" + std::to_string(parity)).c_str(), historyDesc);
    bool historyReadFresh = false;
    const auto historyRead = d.Frame.ImportPersistentTexture(
        (historyBase + ".History" + std::to_string(parity ^ 1u)).c_str(), historyDesc,
        &historyReadFresh);
    historyDesc.debugName = "SSSR.Moments";
    const auto momentsWrite = d.Frame.ImportPersistentTexture(
        (historyBase + ".Moments" + std::to_string(parity)).c_str(), historyDesc);
    bool momentsReadFresh = false;
    const auto momentsRead = d.Frame.ImportPersistentTexture(
        (historyBase + ".Moments" + std::to_string(parity ^ 1u)).c_str(), historyDesc,
        &momentsReadFresh);
    // World normal (oct), roughness and log2 linear depth. Half-float log
    // depth retains relative precision near and far at half the bandwidth of
    // RGBA32F; the shader decodes before its 2% relative validation.
    TextureDesc surfaceDesc = historyDesc;
    surfaceDesc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    surfaceDesc.debugName = "SSSR.SurfaceHistory";
    const auto surfaceWrite = d.Frame.ImportPersistentTexture(
        (historyBase + ".Surface" + std::to_string(parity)).c_str(), surfaceDesc);
    bool surfaceReadFresh = false;
    const auto surfaceRead = d.Frame.ImportPersistentTexture(
        (historyBase + ".Surface" + std::to_string(parity ^ 1u)).c_str(), surfaceDesc,
        &surfaceReadFresh);
    // Previous frame's final composited color (written by Composite, read
    // reprojected by next frame's Intersect for multi-bounce). Single mip: the
    // contact-hardening LOD clamps to 0 on this path; the prefilter compensates.
    historyDesc.debugName = "SSSR.SceneHistory";
    const auto sceneHistoryWrite = d.Frame.ImportPersistentTexture(
        (historyBase + ".Scene" + std::to_string(parity)).c_str(), historyDesc);
    bool sceneHistoryReadFresh = false;
    const auto sceneHistoryRead = d.Frame.ImportPersistentTexture(
        (historyBase + ".Scene" + std::to_string(parity ^ 1u)).c_str(), historyDesc,
        &sceneHistoryReadFresh);

    // A fresh physical on any history plane means nothing we wrote survives:
    // accumulate from scratch this frame rather than blending against undefined
    // memory (Intersect's multi-bounce read of the scene history is gated on the
    // same flag). The pool arms discharge at the bottom, once the passes that
    // rewrite these planes have actually been declared.
    const bool historyValid =
        historyRendered && !historyReadFresh && !momentsReadFresh && !sceneHistoryReadFresh &&
        !surfaceReadFresh;

    BufferDesc rayDesc{};
    const size_t rayBytes = static_cast<size_t>(width) * height * sizeof(uint32_t);
    rayDesc.size = rayBytes;
    rayDesc.stride = sizeof(uint32_t);
    rayDesc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferDst);
    rayDesc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    rayDesc.debugName = "SSSR.RayList";
    const auto rayList = d.Frame.CreateBuffer(d.PassName("RayList").c_str(), rayDesc);

    // One denoise tile per entry; the prefilter runs one workgroup per entry.
    BufferDesc tileDesc{};
    const size_t tileBytes = static_cast<size_t>(gx) * gy * sizeof(uint32_t);
    tileDesc.size = tileBytes;
    tileDesc.stride = sizeof(uint32_t);
    tileDesc.usage = static_cast<uint32_t>(BufferUsage::Storage);
    tileDesc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    tileDesc.debugName = "SSSR.DenoiseTileList";
    const auto tileList = d.Frame.CreateBuffer(d.PassName("DenoiseTileList").c_str(), tileDesc);

    BufferDesc argsDesc{};
    // Two VkDispatchIndirectCommand triples: Intersect's ray dispatch at offset 0,
    // Prefilter's tile dispatch at offset kTileArgsOffset. Classify appends into
    // each triple's fourth word (the count vkCmdDispatchIndirect does not read),
    // and PrepareArgs turns those counts into the grids. Field order is shared
    // with the shaders through Includes/sssr_dispatch.glsl.
    constexpr size_t kArgsBytes = sizeof(uint32_t) * 8u;
    constexpr size_t kTileArgsOffset = sizeof(uint32_t) * 4u;
    argsDesc.size = kArgsBytes;
    argsDesc.stride = sizeof(uint32_t);
    argsDesc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::Indirect |
                                           BufferUsage::TransferDst);
    argsDesc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    argsDesc.debugName = "SSSR.IndirectArgs";
    const auto indirectArgs = d.Frame.CreateBuffer(d.PassName("IndirectArgs").c_str(), argsDesc);

    if (!radiance.IsValid() || !hitData.IsValid() || !reprojected.IsValid() ||
        !reprojectedMoments.IsValid() || !prefiltered.IsValid() || !avgRadiance.IsValid() ||
        !historyWrite.IsValid() || !historyRead.IsValid() || !momentsWrite.IsValid() ||
        !momentsRead.IsValid() || !rayList.IsValid() || !indirectArgs.IsValid() ||
        !tileList.IsValid() || !surfaceRead.IsValid() || !surfaceWrite.IsValid())
    {
        if (!m_LoggedSkipReason)
        {
            m_LoggedSkipReason = true;
            LOG_WARNING("ScreenSpaceReflectionsNode: view {} skipping SSSR — work/history "
                        "resource create failed",
                        static_cast<uint32_t>(d.View.id));
        }
        return;
    }

    if (!m_LoggedChainDeclared)
    {
        m_LoggedChainDeclared = true;
        LOG_INFO("ScreenSpaceReflectionsNode: view {} declaring SSSR chain {}x{} "
                 "intensity={}",
                 static_cast<uint32_t>(d.View.id), width, height, settings.SSSRIntensity);
    }

    const uint32_t frameIndex = SssrFramePin().value_or(static_cast<uint32_t>(frameIndex64));

    // Reset the append counter/dispatch args and sentinel-fill the rounded
    // tail of the ray list before classification writes compact records.
    d.Frame.AddPass(
        d.PassName("ClearRayList").c_str(), PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Write(rayList, RenderGraph::RGBufferWrite::CopyDst);
            p.Write(indirectArgs, RenderGraph::RGBufferWrite::CopyDst);
        },
        [rayList, indirectArgs, rayBytes](RenderGraph::RGContext& ctx)
        {
            if (!ctx.Cmd) return;
            ctx.Cmd->FillBuffer(ctx.GetBuffer(rayList), 0, rayBytes, kEmptyRayRecord);
            ctx.Cmd->FillBuffer(ctx.GetBuffer(indirectArgs), 0, kArgsBytes, 0u);
        });

    d.Frame.AddPass(
        d.PassName("ClassifyTiles").c_str(), PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(specularWeight, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(depth, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(momentsRead, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(surfaceRead, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(motionVectors, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(normalRoughness, RenderGraph::RGTextureRead::SampledCompute);
            p.Write(radiance, RenderGraph::RGTextureWrite::Storage);
            p.Write(hitData, RenderGraph::RGTextureWrite::Storage);
            // Classify defines the prefilter's whole target; the prefilter itself
            // only visits listed tiles. This is what keeps the pixels it skips out
            // of the temporal resolve's neighbourhood statistics as zero rather
            // than as aliased transient memory.
            p.Write(prefiltered, RenderGraph::RGTextureWrite::Storage);
            p.Write(rayList, RenderGraph::RGBufferWrite::Storage);
            p.Write(indirectArgs, RenderGraph::RGBufferWrite::Storage);
            p.Write(tileList, RenderGraph::RGBufferWrite::Storage);
        },
        [=, this](RenderGraph::RGContext& ctx)
        {
            auto* device = ctx.GetDevice();
            if (!device || !ctx.Cmd) return;
            auto set = NewSet(device, Classify, "SSSR.Classify.Set0");
            const auto& meta = *m_Programs[Classify].Meta;
            NamedDescriptorWriter writer(device, set, meta, 0);
            writer.AddCombinedImageSampler("uSpecularWeight", ctx.GetTexture(specularWeight), m_PointSampler);
            writer.AddCombinedImageSampler("uDepth", ctx.GetTexture(depth), m_PointSampler);
            writer.AddCombinedImageSampler("uNormalRoughness", ctx.GetTexture(normalRoughness),
                                           m_PointSampler);
            writer.AddStorageBuffer("RayList", ctx.GetBuffer(rayList), 0, rayBytes);
            writer.AddStorageBuffer("Args", ctx.GetBuffer(indirectArgs), 0, kArgsBytes);
            writer.AddStorageBuffer("TileList", ctx.GetBuffer(tileList), 0, tileBytes);
            writer.AddCombinedImageSampler("uMotionVectors", ctx.GetTexture(motionVectors),
                                           m_PointSampler);
            writer.AddCombinedImageSampler("uMoments", ctx.GetTexture(momentsRead), m_PointSampler);
            writer.AddCombinedImageSampler("uSurfaceHistory", ctx.GetTexture(surfaceRead), m_PointSampler);
            writer.AddUniformBuffer("ViewParams", viewParams.Buffer, viewParams.Offset,
                                    viewParams.Size);
            writer.Flush();
            Detail::BindStorageImageByName(device, set, meta, "uRadiance",
                                           ctx.GetTexture(radiance));
            Detail::BindStorageImageByName(device, set, meta, "uHitData",
                                           ctx.GetTexture(hitData));
            Detail::BindStorageImageByName(device, set, meta, "uPrefiltered",
                                           ctx.GetTexture(prefiltered));
            if (BeginProgram(ctx, Classify, set, settings, frameIndex, historyValid, viewportRect).IsValid())
                ctx.Cmd->Dispatch(gx, gy, 1);
        });

    // One thread, between the appends and the first consumer of the grids they
    // drive. A workgroup count is not something an atomic can produce: it has to be
    // bounded at the dispatch-width limit and the overflow spilled into Y, which is
    // a division over the final count rather than a running maximum.
    d.Frame.AddPass(
        d.PassName("PrepareArgs").c_str(), PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        { p.Write(indirectArgs, RenderGraph::RGBufferWrite::Storage); },
        [=, this](RenderGraph::RGContext& ctx)
        {
            auto* device = ctx.GetDevice();
            if (!device || !ctx.Cmd) return;
            auto set = NewSet(device, PrepareArgs, "SSSR.PrepareArgs.Set0");
            const auto& meta = *m_Programs[PrepareArgs].Meta;
            NamedDescriptorWriter writer(device, set, meta, 0);
            writer.AddStorageBuffer("Args", ctx.GetBuffer(indirectArgs), 0, kArgsBytes);
            writer.Flush();
            if (BeginProgram(ctx, PrepareArgs, set, settings, frameIndex, historyValid, viewportRect)
                    .IsValid())
                ctx.Cmd->Dispatch(1, 1, 1);
        });

    d.Frame.AddPass(
        d.PassName("Intersect").c_str(), PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(input, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(depth, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(motionVectors, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(hzb, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(normalRoughness, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(specularWeight, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(specularRadiance, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(sceneHistoryRead, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(rayList, RenderGraph::RGBufferRead::Storage);
            // Two accesses on one buffer: the dispatch grid as indirect args, and
            // the ray count the shader gates its lanes on as an SSBO. The graph ORs
            // both into the barrier scope; declaring only the indirect access would
            // leave the shader read unsynchronized.
            p.Read(indirectArgs, RenderGraph::RGBufferRead::Indirect);
            p.Read(indirectArgs, RenderGraph::RGBufferRead::Storage);
            p.Write(radiance, RenderGraph::RGTextureWrite::Storage);
            p.Write(hitData, RenderGraph::RGTextureWrite::Storage);
        },
        [=, this](RenderGraph::RGContext& ctx)
        {
            auto* device = ctx.GetDevice();
            if (!device || !ctx.Cmd) return;
            auto set = NewSet(device, Intersect, "SSSR.Intersect.Set0");
            const auto& meta = *m_Programs[Intersect].Meta;
            NamedDescriptorWriter writer(device, set, meta, 0);
            writer.AddCombinedImageSampler("uSceneColor", ctx.GetTexture(input), m_LinearSampler);
            writer.AddCombinedImageSampler("uDepth", ctx.GetTexture(depth), m_PointSampler);
            writer.AddCombinedImageSampler("uHZB", ctx.GetTexture(hzb), m_PointSampler);
            writer.AddCombinedImageSampler("uNormalRoughness", ctx.GetTexture(normalRoughness),
                                           m_PointSampler);
            writer.AddCombinedImageSampler("uBlueNoise", m_BlueNoiseTexture, m_PointSampler);
            writer.AddCombinedImageSampler("uSpecularWeight", ctx.GetTexture(specularWeight), m_PointSampler);
            writer.AddCombinedImageSampler("uSpecularRadiance", ctx.GetTexture(specularRadiance), m_PointSampler);
            writer.AddCombinedImageSampler("uPrevScene", ctx.GetTexture(sceneHistoryRead),
                                           m_LinearSampler);
            writer.AddStorageBuffer("RayList", ctx.GetBuffer(rayList), 0, rayBytes);
            writer.AddStorageBuffer("Args", ctx.GetBuffer(indirectArgs), 0, kArgsBytes);
            writer.AddCombinedImageSampler("uMotionVectors", ctx.GetTexture(motionVectors),
                                           m_PointSampler);
            writer.AddUniformBuffer("ViewParams", viewParams.Buffer, viewParams.Offset,
                                    viewParams.Size);
            writer.Flush();
            Detail::BindStorageImageByName(device, set, meta, "uRadiance",
                                           ctx.GetTexture(radiance));
            Detail::BindStorageImageByName(device, set, meta, "uHitData",
                                           ctx.GetTexture(hitData));
            if (BeginProgram(ctx, Intersect, set, settings, frameIndex, historyValid, viewportRect)
                    .IsValid())
                ctx.Cmd->DispatchIndirect(ctx.GetBuffer(indirectArgs), 0);
        });

    d.Frame.AddPass(
        d.PassName("Reproject").c_str(), PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(radiance, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(hitData, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(depth, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(motionVectors, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(normalRoughness, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(historyRead, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(momentsRead, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(surfaceRead, RenderGraph::RGTextureRead::SampledCompute);
            p.Write(reprojected, RenderGraph::RGTextureWrite::Storage);
            p.Write(reprojectedMoments, RenderGraph::RGTextureWrite::Storage);
            // Full-screen, so every average-radiance texel is defined. The
            // prefilter's apron reads blocks outside its own denoise tile, and
            // that read is only safe while this producer covers the whole frame.
            p.Write(avgRadiance, RenderGraph::RGTextureWrite::Storage);
        },
        [=, this](RenderGraph::RGContext& ctx)
        {
            auto* device = ctx.GetDevice(); if (!device || !ctx.Cmd) return;
            auto set = NewSet(device, Reproject, "SSSR.Reproject.Set0");
            const auto& meta = *m_Programs[Reproject].Meta;
            NamedDescriptorWriter writer(device, set, meta, 0);
            writer.AddCombinedImageSampler("uCurrent", ctx.GetTexture(radiance), m_PointSampler);
            writer.AddCombinedImageSampler("uHitData", ctx.GetTexture(hitData), m_PointSampler);
            writer.AddCombinedImageSampler("uDepth", ctx.GetTexture(depth), m_PointSampler);
            writer.AddCombinedImageSampler("uNormalRoughness", ctx.GetTexture(normalRoughness),
                                           m_PointSampler);
            writer.AddCombinedImageSampler("uHistory", ctx.GetTexture(historyRead), m_LinearSampler);
            writer.AddCombinedImageSampler("uMoments", ctx.GetTexture(momentsRead), m_PointSampler);
            writer.AddCombinedImageSampler("uSurfaceHistory", ctx.GetTexture(surfaceRead),
                                           m_PointSampler);
            writer.AddCombinedImageSampler("uMotionVectors", ctx.GetTexture(motionVectors),
                                           m_PointSampler);
            writer.AddUniformBuffer("SssrReprojection", reprojectionParams.Buffer,
                                    reprojectionParams.Offset, sizeof(ReprojectionParams));
            writer.AddUniformBuffer("ViewParams", viewParams.Buffer, viewParams.Offset,
                                    viewParams.Size);
            writer.Flush();
            Detail::BindStorageImageByName(device, set, meta, "uReprojected",
                                           ctx.GetTexture(reprojected));
            Detail::BindStorageImageByName(device, set, meta, "uReprojectedMoments",
                                           ctx.GetTexture(reprojectedMoments));
            Detail::BindStorageImageByName(device, set, meta, "uAvgRadiance",
                                           ctx.GetTexture(avgRadiance));
            if (BeginProgram(ctx, Reproject, set, settings, frameIndex, historyValid, viewportRect).IsValid())
                ctx.Cmd->Dispatch(gx, gy, 1);
        });

    d.Frame.AddPass(
        d.PassName("Prefilter").c_str(), PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(radiance, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(depth, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(normalRoughness, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(avgRadiance, RenderGraph::RGTextureRead::SampledCompute);
            // The temporal half of the drive signal. Zero on a converged static
            // pose by construction, which is why the spatial half exists.
            p.Read(reprojectedMoments, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(tileList, RenderGraph::RGBufferRead::Storage);
            // As for Intersect: indirect args plus an SSBO read of the tile count
            // the workgroup gate gets its bound from.
            p.Read(indirectArgs, RenderGraph::RGBufferRead::Indirect);
            p.Read(indirectArgs, RenderGraph::RGBufferRead::Storage);
            p.Write(prefiltered, RenderGraph::RGTextureWrite::Storage);
        },
        [=, this](RenderGraph::RGContext& ctx)
        {
            auto* device = ctx.GetDevice(); if (!device || !ctx.Cmd) return;
            auto set = NewSet(device, Prefilter, "SSSR.Prefilter.Set0");
            const auto& meta = *m_Programs[Prefilter].Meta;
            NamedDescriptorWriter writer(device, set, meta, 0);
            writer.AddCombinedImageSampler("uRadiance", ctx.GetTexture(radiance), m_PointSampler);
            writer.AddCombinedImageSampler("uDepth", ctx.GetTexture(depth), m_PointSampler);
            writer.AddCombinedImageSampler("uNormalRoughness", ctx.GetTexture(normalRoughness),
                                           m_PointSampler);
            // Linear: a pixel reads a blend of its own block's average and its
            // neighbours', so the reference varies smoothly across a block edge.
            writer.AddCombinedImageSampler("uAvgRadiance", ctx.GetTexture(avgRadiance),
                                           m_LinearSampler);
            writer.AddCombinedImageSampler("uReprojectedMoments",
                                           ctx.GetTexture(reprojectedMoments), m_PointSampler);
            writer.AddStorageBuffer("TileList", ctx.GetBuffer(tileList), 0, tileBytes);
            writer.AddStorageBuffer("Args", ctx.GetBuffer(indirectArgs), 0, kArgsBytes);
            writer.AddUniformBuffer("ViewParams", viewParams.Buffer, viewParams.Offset,
                                    viewParams.Size);
            writer.Flush();
            Detail::BindStorageImageByName(device, set, meta, "uFiltered",
                                           ctx.GetTexture(prefiltered));
            // One workgroup per denoise tile. Tiles with no reflective pixel are
            // never launched, so the cost tracks the reflective share of the frame
            // instead of its resolution.
            if (BeginProgram(ctx, Prefilter, set, settings, frameIndex, historyValid, viewportRect).IsValid())
                ctx.Cmd->DispatchIndirect(ctx.GetBuffer(indirectArgs), kTileArgsOffset);
        });

    // History resources are persistent exports. Record the compute dispatch on
    // the graphics queue so their final ownership is export-safe even when no
    // later graphics pass samples them this frame.
    d.Frame.AddPass(
        d.PassName("TemporalResolve").c_str(), PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(prefiltered, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(reprojected, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(reprojectedMoments, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(depth, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(normalRoughness, RenderGraph::RGTextureRead::SampledCompute);
            p.Write(historyWrite, RenderGraph::RGTextureWrite::Storage);
            p.Write(momentsWrite, RenderGraph::RGTextureWrite::Storage);
            p.Write(surfaceWrite, RenderGraph::RGTextureWrite::Storage);
            p.Read(hitData, RenderGraph::RGTextureRead::SampledCompute);
        },
        [=, this](RenderGraph::RGContext& ctx)
        {
            auto* device = ctx.GetDevice(); if (!device || !ctx.Cmd) return;
            auto set = NewSet(device, Temporal, "SSSR.Temporal.Set0");
            const auto& meta = *m_Programs[Temporal].Meta;
            NamedDescriptorWriter writer(device, set, meta, 0);
            writer.AddCombinedImageSampler("uCurrent", ctx.GetTexture(prefiltered), m_PointSampler);
            writer.AddCombinedImageSampler("uHitData", ctx.GetTexture(hitData), m_PointSampler);
            writer.AddCombinedImageSampler("uReprojected", ctx.GetTexture(reprojected), m_PointSampler);
            writer.AddCombinedImageSampler("uReprojectedMoments",
                                           ctx.GetTexture(reprojectedMoments), m_PointSampler);
            writer.AddCombinedImageSampler("uDepth", ctx.GetTexture(depth), m_PointSampler);
            writer.AddCombinedImageSampler("uNormalRoughness", ctx.GetTexture(normalRoughness),
                                           m_PointSampler);
            writer.AddUniformBuffer("ViewParams", viewParams.Buffer, viewParams.Offset,
                                    viewParams.Size);
            writer.Flush();
            Detail::BindStorageImageByName(device, set, meta, "uHistoryOut",
                                           ctx.GetTexture(historyWrite));
            Detail::BindStorageImageByName(device, set, meta, "uMomentsOut",
                                           ctx.GetTexture(momentsWrite));
            Detail::BindStorageImageByName(device, set, meta, "uSurfaceOut",
                                           ctx.GetTexture(surfaceWrite));
            if (BeginProgram(ctx, Temporal, set, settings, frameIndex, historyValid, viewportRect).IsValid())
                ctx.Cmd->Dispatch(gx, gy, 1);
        });

    d.Frame.AddPass(
        d.PassName("Composite").c_str(), PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(input, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(historyWrite, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(depth, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(normalRoughness, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(specularWeight, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(specularRadiance, RenderGraph::RGTextureRead::SampledCompute);
            p.Write(output, RenderGraph::RGTextureWrite::Storage);
            p.Write(sceneHistoryWrite, RenderGraph::RGTextureWrite::Storage);
        },
        [=, this](RenderGraph::RGContext& ctx)
        {
            auto* device = ctx.GetDevice(); if (!device || !ctx.Cmd) return;
            auto set = NewSet(device, Composite, "SSSR.Composite.Set0");
            const auto& meta = *m_Programs[Composite].Meta;
            NamedDescriptorWriter writer(device, set, meta, 0);
            writer.AddCombinedImageSampler("uSceneColor", ctx.GetTexture(input), m_PointSampler);
            writer.AddCombinedImageSampler("uReflections", ctx.GetTexture(historyWrite), m_PointSampler);
            writer.AddCombinedImageSampler("uDepth", ctx.GetTexture(depth), m_PointSampler);
            writer.AddCombinedImageSampler("uNormalRoughness", ctx.GetTexture(normalRoughness),
                                           m_PointSampler);
            writer.AddCombinedImageSampler("uSpecularWeight", ctx.GetTexture(specularWeight), m_PointSampler);
            writer.AddCombinedImageSampler("uSpecularRadiance", ctx.GetTexture(specularRadiance), m_PointSampler);
            writer.AddUniformBuffer("ViewParams", viewParams.Buffer, viewParams.Offset,
                                    viewParams.Size);
            writer.Flush();
            Detail::BindStorageImageByName(device, set, meta, "uOutput", ctx.GetTexture(output));
            Detail::BindStorageImageByName(device, set, meta, "uSceneHistoryOut",
                                           ctx.GetTexture(sceneHistoryWrite));
            if (BeginProgram(ctx, Composite, set, settings, frameIndex, historyValid, viewportRect)
                    .IsValid())
                ctx.Cmd->Dispatch(gx, gy, 1);
        });

    // Mark only now that the passes declaring the full-surface writes exist:
    // marking before the early-outs above would discharge the pool's freshness
    // arm on a frame that never declared the rewrite, and the next frame would
    // blend against an undefined physical as "resident" history.
    d.Frame.MarkPersistentTextureInitialized(historyWrite);
    d.Frame.MarkPersistentTextureInitialized(momentsWrite);
    d.Frame.MarkPersistentTextureInitialized(surfaceWrite);
    d.Frame.MarkPersistentTextureInitialized(sceneHistoryWrite);

    d.PublishTexture(m_OutputKey, output);
}

void ScreenSpaceReflectionsNode::LoadShaders(IDevice* device)
{
    if (m_LoadAttempted)
        return;
    m_LoadAttempted = true;
    m_Device = device;
    if (!device->GetCapabilities().supportsFilterableFloat32)
    {
        // The intersect/reproject taps sample R32F sources (HZB, resolved
        // depth) through filtering samplers; with reflected filterability the
        // layouts say Float, and binding an R32F view to a Float layout needs
        // WebGPU's float32-filterable feature. Without it the bind fails and
        // an invalid pass poisons the shared per-frame encoder — decline
        // instead (the effect stays a no-op; a persisted scene setting must
        // not black the screen).
        LOG_INFO("ScreenSpaceReflectionsNode: SSSR needs float32-filterable, which this "
                 "adapter lacks; the effect stays inactive.");
        return;
    }
    constexpr std::array<const char*, ProgramCount> paths = {
        "Shaders/sssr_classify.shaderpkg",  "Shaders/sssr_prepare_args.shaderpkg",
        "Shaders/sssr_intersect.shaderpkg", "Shaders/sssr_reproject.shaderpkg",
        "Shaders/sssr_prefilter.shaderpkg", "Shaders/sssr_temporal.shaderpkg",
        "Shaders/sssr_composite.shaderpkg"};
    constexpr std::array<const char*, ProgramCount> names = {
        "SSSR_Classify",  "SSSR_PrepareArgs", "SSSR_Intersect", "SSSR_Reproject",
        "SSSR_Prefilter", "SSSR_Temporal",    "SSSR_Composite"};

    bool ok = true;
    for (size_t i = 0; i < ProgramCount; ++i)
    {
        ShaderPackage package{};
        std::string error;
        if (!LoadShaderPkg(paths[i], device->PreferredShaderSource(), package, &error))
        {
            LOG_WARNING("ScreenSpaceReflectionsNode: failed to load {}: {}", paths[i], error);
            ok = false;
            continue;
        }
        auto cs = package.stageBytes.find("cs");
        if (cs == package.stageBytes.end() || cs->second.empty())
        {
            LOG_WARNING("ScreenSpaceReflectionsNode: {} has no compute stage", paths[i]);
            ok = false;
            continue;
        }
        auto& out = m_Programs[i];
        out.Meta = std::make_unique<ShaderMeta>(std::move(package.meta));
        ComputePipelineDesc desc{};
        desc.ComputeShader =
            std::make_shared<const std::vector<uint8_t>>(std::move(cs->second));
        desc.DebugName = names[i];
        auto patchLayout = [&](uint32_t setIndex, DescriptorSetLayoutDesc& layout)
        {
            if (setIndex != 0)
                return;
            // Combined-image bindings cook to WGSL `texture_2d<f32>` + filtering
            // `sampler`. A compute layout that leaves imageFilterableFloat unset
            // declares UnfilterableFloat, which cannot pair with a filtering
            // sampler — pipeline creation fails and the invalid pipeline poisons
            // the frame encoder (black canvas, including UI). Intersect/reproject
            // also filter R32F sources (HZB, resolved depth); that bind needs
            // float32-filterable, which LoadShaders already declines without.
            for (auto& b : layout.bindings)
            {
                if (b.type == DescriptorType::CombinedImageSampler ||
                    b.type == DescriptorType::Texture)
                    b.imageFilterableFloat = true;
            }
            out.Set0Layout = layout;
        };
        std::string applyError;
        MaterialHelper::ApplyShaderMetaToComputeDesc(
            *device, *out.Meta, desc, MaterialBuilder::MergeMode::Auto,
            {true, 128}, patchLayout, &applyError);
        out.Pipeline = device->InternComputePipeline(std::move(desc));
        ok &= out.Pipeline.IsValid();
    }
    if (!m_PointSampler.IsValid())
        m_PointSampler = device->CreateSampler(SamplerDesc::PointClamp("SSSR.PointClamp"));
    if (!m_LinearSampler.IsValid())
        m_LinearSampler =
            device->CreateSampler(SamplerDesc::MaterialLinearClamp("SSSR.LinearClamp"));

    // Upload the precomputed blue-noise tile once. The intersect samples it with
    // GE_BlueNoise (wrap addressing via texelFetch mod, so PointClamp is fine).
    if (!m_BlueNoiseTexture.IsValid())
    {
        TextureDesc bnDesc{};
        bnDesc.width = kSssrBlueNoiseSize;
        bnDesc.height = kSssrBlueNoiseSize;
        bnDesc.mipLevels = 1;
        bnDesc.arrayLayers = 1;
        bnDesc.sampleCount = 1;
        bnDesc.format = static_cast<uint32_t>(TextureFormat::R8G8_UNORM);
        bnDesc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
        bnDesc.debugName = "SSSR.BlueNoise";
        m_BlueNoiseTexture = device->CreateTexture(bnDesc);
        if (m_BlueNoiseTexture.IsValid())
            Rendering::UploadTexture2D(device, m_BlueNoiseTexture, kSssrBlueNoiseRG,
                                       kSssrBlueNoiseSize, kSssrBlueNoiseSize,
                                       static_cast<size_t>(kSssrBlueNoiseSize) * 2u,
                                       "SSSR.BlueNoiseStaging");
    }

    m_ShadersLoaded = ok && m_PointSampler.IsValid() && m_LinearSampler.IsValid() &&
                      m_BlueNoiseTexture.IsValid();
    if (!m_ShadersLoaded)
    {
        LOG_WARNING("ScreenSpaceReflectionsNode: SSSR shaders did not load "
                    "(programs={} pointSampler={} linearSampler={} blueNoise={})",
                    ok, m_PointSampler.IsValid(), m_LinearSampler.IsValid(),
                    m_BlueNoiseTexture.IsValid());
    }
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
