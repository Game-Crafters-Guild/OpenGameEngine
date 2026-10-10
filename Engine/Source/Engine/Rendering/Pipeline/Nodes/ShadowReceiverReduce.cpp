#include "Engine/Rendering/Pipeline/Nodes/ShadowReceiverReduce.h"

#include "Engine/Rendering/Pipeline/Nodes/PipelineNodeUtils.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"

#include "Logger/Logger.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;
using Mathematics::Matrix4x4;
using Mathematics::Vector3;

namespace
{
// The shader samples one pixel of each kSamplingStride x kSamplingStride block,
// each thread reduces kSamplesPerThread x kSamplesPerThread samples, and a
// workgroup is kWorkgroupSize x kWorkgroupSize threads
// (shadow_receiver_reduce.glsl).
constexpr uint32_t kSamplingStride = 2u;
constexpr uint32_t kSamplesPerThread = 2u;
constexpr uint32_t kWorkgroupSize = 16u;
constexpr uint32_t kSideBytes = ShadowReceiverMeasurement::kWordsPerSide * sizeof(uint32_t);
// The reduce waits this many identical frames before idle elision skips it.
constexpr uint32_t kElisionSettleFrames = 3;
// Frames between the elision gate's opt-in statistics lines.
constexpr uint32_t kElisionReportWindowFrames = 600;
} // namespace

void ShadowReceiverReduce::DispatchGroups(uint32_t width, uint32_t height, uint32_t& outX, uint32_t& outY)
{
    const uint32_t texelsPerGroup = kSamplingStride * kSamplesPerThread * kWorkgroupSize;
    outX = (width + texelsPerGroup - 1) / texelsPerGroup;
    outY = (height + texelsPerGroup - 1) / texelsPerGroup;
}

ShadowReceiverReduce::~ShadowReceiverReduce()
{
    if (m_Device && m_Sampler.IsValid())
        m_Device->DestroySampler(m_Sampler);
}

void ShadowReceiverReduce::LoadPipeline(IDevice* device, const char* package, const char* debugName,
                                        ReducePipeline& out)
{
    ShaderPackage pkg{};
    std::string loadErr;
    if (!LoadShaderPkg(package, device->PreferredShaderSource(), pkg, &loadErr))
    {
        LOG_WARNING("ShadowReceiverReduce: failed to load {}: {}", package, loadErr);
        return;
    }
    auto itCs = pkg.stageBytes.find("cs");
    if (itCs == pkg.stageBytes.end() || itCs->second.empty())
    {
        LOG_WARNING("ShadowReceiverReduce: {} has no cs stage", package);
        return;
    }

    out.Meta = std::make_unique<ShaderMeta>(std::move(pkg.meta));
    ComputePipelineDesc cd{};
    cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(itCs->second));
    cd.DebugName = debugName;
    std::string err;
    MaterialHelper::ApplyShaderMetaToComputeDesc(
        *device, *out.Meta, cd, MaterialBuilder::MergeMode::Auto, {true, 128},
        [&out](uint32_t setIndex, DescriptorSetLayoutDesc& dsl)
        {
            if (setIndex == 0)
                out.Set0Layout = dsl;
        },
        &err);
    out.Id = device->InternComputePipeline(std::move(cd));
}

void ShadowReceiverReduce::EnsureResources(IDevice* device)
{
    if (m_LoadAttempted || !device)
        return;
    m_LoadAttempted = true;
    m_Device = device;
    LoadPipeline(device, "Shaders/depth_reduce.shaderpkg", "SDSM_DepthReduce", m_SingleSample);
    LoadPipeline(device, "Shaders/depth_reduce_ms.shaderpkg", "SDSM_DepthReduceMS", m_Multisampled);
    // The kernel reads single texels (texelFetch): no filtering applies.
    m_Sampler = device->CreateSampler(SamplerDesc::PointClamp("SDSM_DepthSampler"));
}

bool ShadowReceiverReduce::IsElided(ViewDeclare& d, const ShadowMapRenderFeature& feature,
                                    const CameraData& camera, uint32_t width, uint32_t height)
{
    // Keys: the UNJITTERED logic-domain camera (the resolve's quantization
    // already collapses TAA jitter wobble at the consumer), the extent, the
    // content and depth-dynamic epochs, and the latest results, so a readback
    // that changes the fit blocks the skip for that update. On wake the readback
    // keeps the pipeline's frames-in-flight lag: every elided frame's reduce
    // would have produced the result already retained.
    const ViewId viewId = d.View.id;
    const auto& idle = d.Services.GetIdleElisionFrameState();
    ElisionInputBlob blob;
    blob.AppendBytes(&camera, sizeof(CameraData));
    blob.Append(width);
    blob.Append(height);
    blob.Append(idle.ContentEpoch);
    blob.Append(idle.DepthDynamicEpoch);
    const ShadowMapRenderFeature::SDSMBounds& bounds = feature.GetSDSMBounds(viewId);
    blob.Append(bounds.nearDepth);
    blob.Append(bounds.farDepth);
    blob.Append(bounds.valid);
    const ShadowReceiverMeasurement& receivers = feature.GetShadowReceivers(viewId);
    blob.Append(receivers.Valid);
    blob.Append(receivers.NearDepth);
    blob.AppendBytes(receivers.Bins.data(), sizeof(receivers.Bins));
    blob.AppendBytes(receivers.Rays.data(), sizeof(receivers.Rays));
    ElisionGate& gate = m_Elision[static_cast<uint32_t>(viewId)];
    const RecomputeElisionGate::Decision decision = gate.Gate.Evaluate(
        d.Frame.FrameIndex(), std::move(blob), idle.AllowSdsm, kElisionSettleFrames);
    if (IdleElisionLoggingEnabled() && decision.Skip != gate.LogState)
    {
        if (decision.Skip)
            LOG_INFO("[IdleElision] SDSM DepthReduce view {} engaged", static_cast<uint32_t>(viewId));
        else
            LOG_INFO("[IdleElision] SDSM DepthReduce view {} disengaged: {}",
                     static_cast<uint32_t>(viewId),
                     ElisionDisengageReason(decision.Cause, decision.Skip));
        gate.LogState = decision.Skip;
    }
    if (IdleElisionLoggingEnabled() && gate.Gate.ShouldReportWindow(kElisionReportWindowFrames))
    {
        const auto& st = gate.Gate.GetStats();
        LOG_INFO("[IdleElision] SDSM DepthReduce view {} window: eval {} skip {} | "
                 "first {} forced {} gap {} changed {} unsettled {}",
                 static_cast<uint32_t>(viewId), st.Evaluated, st.Skipped,
                 st.CauseCounts[static_cast<size_t>(ElisionCause::FirstEvaluate)],
                 st.CauseCounts[static_cast<size_t>(ElisionCause::Forced)],
                 st.CauseCounts[static_cast<size_t>(ElisionCause::EvaluationGap)],
                 st.CauseCounts[static_cast<size_t>(ElisionCause::InputsChanged)],
                 st.CauseCounts[static_cast<size_t>(ElisionCause::NotSettled)]);
    }
    return decision.Skip;
}

ShadowReceiverReduce::Setup ShadowReceiverReduce::MakeSetup(const CameraData& camera,
                                                            float nearPlane, float farPlane,
                                                            bool orthographic,
                                                            const Vector3& lightDirection,
                                                            float maxShadowDistance,
                                                            uint32_t width, uint32_t height)
{
    Setup setup{};
    ShadowReceiverMeasurement::Context& measured = setup.Measured;
    SetShadowReceiverCamera(camera, orthographic, measured);
    measured.NearPlane = nearPlane;
    measured.FarPlane = farPlane;
    measured.LightDirection = lightDirection;
    measured.BinNear = nearPlane;
    measured.BinFar = std::max(std::min(farPlane, maxShadowDistance), nearPlane * 2.0f);

    // Positions are reconstructed render-origin-relative, against the same
    // origin the cascade fit derives from the camera.
    float viewRel[16];
    float viewProjRel[16];
    int32 originSector[4];
    ComputeRebasedView(camera.view, camera.proj, camera.viewProj, camera.cameraPos[0],
                       camera.cameraPos[1], camera.cameraPos[2], viewRel, viewProjRel,
                       originSector);
    float originX = 0.0f;
    float originY = 0.0f;
    float originZ = 0.0f;
    SectorToWorld(Components::WorldSectorCoord{originSector[0], originSector[1], originSector[2]},
                  originX, originY, originZ);
    for (int axis = 0; axis < 3; ++axis)
        measured.RenderOriginSector[axis] = originSector[axis];

    ParamsGPU& params = setup.Params;
    Matrix4x4 viewProj;
    std::memcpy(viewProj.Data(), viewProjRel, sizeof(viewProjRel));
    const Matrix4x4 invViewProj = Mathematics::Inverse(viewProj);
    std::memcpy(params.InvViewProjRel, invViewProj.Data(), sizeof(params.InvViewProjRel));
    // Rows of the column-major light rotation: light-space axis i of a position
    // p is dot(row i, p).
    const Matrix4x4 lightRot = ShadowMapRenderFeature::CascadeLightRotation(lightDirection);
    const float* m = lightRot.Data();
    float* rows[3] = {params.LightRow0, params.LightRow1, params.LightRow2};
    for (int i = 0; i < 3; ++i)
    {
        rows[i][0] = m[i];
        rows[i][1] = m[4 + i];
        rows[i][2] = m[8 + i];
        rows[i][3] = 0.0f;
    }
    params.CameraPosRel[0] = camera.cameraPos[0] - originX;
    params.CameraPosRel[1] = camera.cameraPos[1] - originY;
    params.CameraPosRel[2] = camera.cameraPos[2] - originZ;
    params.CameraForward[0] = measured.CameraForward.x;
    params.CameraForward[1] = measured.CameraForward.y;
    params.CameraForward[2] = measured.CameraForward.z;
    params.CameraForward[3] = orthographic ? 1.0f : 0.0f;
    measured.CameraLightSpace = lightRot.TransformPoint(
        Vector3{params.CameraPosRel[0], params.CameraPosRel[1], params.CameraPosRel[2]});
    const Mathematics::Vector4 forwardLs = lightRot.Transform(
        Mathematics::Vector4{measured.CameraForward.x, measured.CameraForward.y, measured.CameraForward.z, 0.0f});
    measured.ForwardLightSpace = Vector3{forwardLs.x, forwardLs.y, forwardLs.z};
    params.BinParams[0] = measured.BinNear;
    params.BinParams[1] = ShadowReceiverBinsPerLogUnit(measured.BinNear, measured.BinFar);
    params.BinParams[2] = measured.BinFar;
    params.Extent[0] = width;
    params.Extent[1] = height;
    return setup;
}

void ShadowReceiverReduce::DeclareForView(ViewDeclare& d, ShadowMapRenderFeature& feature,
                                          const CameraData& camera, float nearPlane,
                                          float farPlane, bool orthographic,
                                          const Vector3& lightDirection, float maxShadowDistance)
{
    EnsureResources(d.Services.GetDevice());

    // The view's depth attachment: the world pass, the ocean and the occlusion
    // recover pass write it after this node, and the late declaration reads it
    // after all of them.
    const RenderGraph::RGTexture depth = d.ViewDepth;
    if (!depth.IsValid())
        return;
    const RenderGraph::RGResourceDesc dd = d.Frame.Graph().ResourceDesc(depth.Id);
    if (dd.Width == 0 || dd.Height == 0)
        return;
    const bool multisampled = dd.SampleCount > 1;
    if (!(multisampled ? m_Multisampled : m_SingleSample).Id.IsValid())
        return;
    if (IsElided(d, feature, camera, dd.Width, dd.Height))
        return;

    Request request{};
    request.ViewId = d.View.id;
    request.Depth = depth;
    request.Width = dd.Width;
    request.Height = dd.Height;
    request.Multisampled = multisampled;
    request.PassName = d.PassName("DepthReduce");
    request.Dispatch = MakeSetup(camera, nearPlane, farPlane, orthographic, lightDirection,
                                 maxShadowDistance, dd.Width, dd.Height);

    ShadowMapRenderFeature* featurePtr = &feature;
    d.DeferDeclare([this, featurePtr, request = std::move(request)](
                       RenderGraph::RGFrame& frame, RenderPipelineInstance& /*instance*/)
                   { DeclareLate(frame, *featurePtr, request); });
}

void ShadowReceiverReduce::DeclareLate(RenderGraph::RGFrame& frame, ShadowMapRenderFeature& feature,
                                       const Request& request) const
{
    const auto params = frame.AllocUpload<ParamsGPU>();
    if (!params.Valid())
        return;
    *params.Ptr = request.Dispatch.Params;
    // Acquired last: the pending is queued here, and the pass's first commands
    // reset the slot to the "nothing measured" sentinels, so a bail inside the
    // pass reads back as a degenerate frame rather than as stale results.
    const BufferHandle slot = feature.AcquireSdsmSlotRG(frame, request.ViewId, request.Dispatch.Measured);
    if (!slot.IsValid())
        return;

    ShadowMapRenderFeature* featurePtr = &feature;
    const BufferHandle paramsBuffer = params.Buffer;
    const uint64_t paramsOffset = params.Offset;
    frame.AddPass(
        request.PassName.c_str(), PassPhase::kDefault,
        [&request](RenderGraph::RGPassBuilder& p)
        {
            // Stage-qualified: the dispatch samples the depth, so the barrier out
            // of its writers must name the compute stage.
            p.Read(request.Depth, RenderGraph::RGTextureRead::SampledCompute);
            // CPU-consumed output (the readback slot is not a graph resource).
            p.PreventCulling();
        },
        [this, featurePtr, request, slot, paramsBuffer, paramsOffset](RenderGraph::RGContext& ctx)
        { Record(ctx, *featurePtr, request, slot, paramsBuffer, paramsOffset); });
}

void ShadowReceiverReduce::Record(RenderGraph::RGContext& ctx, ShadowMapRenderFeature& feature,
                                  const Request& request, BufferHandle slot, BufferHandle params,
                                  uint64_t paramsOffset) const
{
    auto* dev = ctx.GetDevice();
    auto* cl = ctx.Cmd;
    if (!dev || !cl)
        return;

    // Sentinels FIRST: any later bail must leave the slot reading as "nothing
    // measured", never as the output of the reduce that last used it. A GPU
    // fill, not a CPU write: the slot's prior contents stay mappable until this
    // frame's GPU work runs.
    cl->FillBuffer(slot, 0, kSideBytes, 0xFFFFFFFFu);
    cl->FillBuffer(slot, kSideBytes, kSideBytes, 0u);
    cl->Barrier(ResourceBarrier::CreateBufferBarrier(slot, ResourceState::CopyDest,
                                                     ResourceState::UnorderedAccess));

    const auto depthPhysical = ctx.GetTexture(request.Depth);
    if (!depthPhysical.IsValid())
        return;
    const ReducePipeline& pipeline = request.Multisampled ? m_Multisampled : m_SingleSample;
    const PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(pipeline.Id);
    if (!pipe.IsValid() || !pipeline.Meta || pipeline.Set0Layout.bindings.empty())
        return;
    cl->SetPipeline(pipe);

    DescriptorSetDesc ds0{};
    ds0.layout = pipeline.Set0Layout;
    ds0.transient = true;
    ds0.debugName = "SDSM_DepthReduce.Set0";
    auto set0 = dev->CreateDescriptorSet(ds0);
    NamedDescriptorWriter writer(dev, set0, *pipeline.Meta, 0);
    if (writer.Has("DepthTex") && m_Sampler.IsValid())
        writer.AddCombinedImageSampler("DepthTex", depthPhysical, m_Sampler);
    writer.Flush();

    uint32_t binding = 0;
    DescriptorType type{};
    if (Detail::TryGetSet0BindingByName(*pipeline.Meta, "ReceiverBoundsBuffer", binding, type))
    {
        DescriptorSetUpdate u{};
        u.binding = binding;
        u.type = DescriptorType::StorageBuffer;
        u.buffers = {slot};
        dev->UpdateDescriptorSet(set0, u);
    }
    if (Detail::TryGetSet0BindingByName(*pipeline.Meta, "ReceiverReduceParams", binding, type))
    {
        DescriptorSetUpdate u{};
        u.binding = binding;
        u.type = DescriptorType::UniformBuffer;
        u.buffers = {params};
        u.bufferOffsets = {static_cast<size_t>(paramsOffset)};
        u.bufferRanges = {sizeof(ParamsGPU)};
        dev->UpdateDescriptorSet(set0, u);
    }
    cl->BindDescriptorSet(0, set0, pipe);

    uint32_t groupsX = 0;
    uint32_t groupsY = 0;
    DispatchGroups(request.Width, request.Height, groupsX, groupsY);
    cl->Dispatch(groupsX, groupsY, 1);

    // Where a storage buffer cannot also be mapped, the ring handed out a
    // device-local buffer and keeps the mappable slot beside it; this copies one
    // into the other. A no-op on the backends that map the buffer the dispatch wrote.
    feature.ResolveSdsmSlotRG(request.ViewId, cl, slot);
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
