#include "Ocean/OceanRasterDepthCapture.h"
#include "Ocean/OceanShaderProgram.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "OceanShaderDirectory.h"

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/ShaderCompileService.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

namespace GameEngine::Ocean
{

namespace fs = std::filesystem;
using namespace ::GameEngine::Rendering;
namespace RG = ::GameEngine::Rendering::RenderGraph;

namespace
{
struct alignas(16) OceanRasterDepthParamsGPU
{
    float NearFarDeepResolution[4]{}; // x near/top padding, y far, z deep, w resolution
};
static_assert(sizeof(OceanRasterDepthParamsGPU) == 16);

bool NearlyEqual(float32 a, float32 b, float32 epsilon = 1e-4f)
{
    return std::fabs(a - b) <= epsilon;
}

float32 SnapToTexel(float32 value, float32 texelSize)
{
    if (texelSize <= 1e-6f)
        return value;
    return std::round(value / texelSize) * texelSize;
}

} // namespace

OceanRasterDepthCapture::~OceanRasterDepthCapture()
{
    if (m_Device)
    {
        if (m_OutputTexture.IsValid())
            m_Device->DestroyTexture(m_OutputTexture);
        if (m_Sampler.IsValid())
            m_Device->DestroySampler(m_Sampler);
        m_Readback.Destroy(m_Device);
    }
    if (m_RenderServices && m_ViewAllocated)
    {
        if (m_ViewId != 0)
            m_RenderServices->Views().ReleaseView(m_ViewId);
        if (m_CameraId != 0)
            m_RenderServices->Views().ReleaseCamera(m_CameraId);
    }
}

bool OceanRasterDepthCapture::EnsurePipeline(IDevice& device)
{
    if (!m_PipelineLoadAttempted)
    {
        m_PipelineLoadAttempted = true;

        const fs::path shaderDir = OceanShaderDirectory("ocean_raster_depth_convert.comp");
        if (shaderDir.empty())
        {
            Logger::Log::Warning("OceanRasterDepthCapture: converter shader unavailable");
            return false;
        }

        ShaderProgramCompileRequest req{};
        req.debugName = "ocean_raster_depth_convert";
        req.baseDirectory = shaderDir;
        req.cacheRoot = fs::path(".Cache") / "Shaders";
        req.includeDirs = {shaderDir.parent_path()};
        req.stages = {{"cs", "ocean_raster_depth_convert.comp", "main", {}}};

        ShaderProgramCompileResult result{};
        std::string err;
        if (!LoadOceanShaderProgram(req, device.PreferredShaderSource(), result, &err))
        {
            if (!m_WarnedLoadFailed)
            {
                m_WarnedLoadFailed = true;
                Logger::Log::Warning("OceanRasterDepthCapture: converter compile failed: {}", err);
            }
            return false;
        }
        auto cs = result.stageBytes.find("cs");
        if (cs == result.stageBytes.end() || cs->second.empty())
            return false;

        m_Layout = {};
        m_Layout.debugName = "Ocean.RasterDepthConvert.Set0";
        auto addBinding = [&](uint32 binding, DescriptorType type, uint32 stages)
        {
            DescriptorBinding b{};
            b.binding = binding;
            b.type = type;
            b.count = 1;
            b.shaderStages = stages;
            m_Layout.bindings.push_back(b);
        };
        addBinding(0, DescriptorType::CombinedImageSampler, kShaderStageCompute);
        addBinding(1, DescriptorType::StorageImage, kShaderStageCompute);
        addBinding(2, DescriptorType::UniformBuffer, kShaderStageCompute);

        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(cs->second));
        Rendering::ApplyMetaImageShapeToLayout(result.meta, m_Layout);
        cd.DescriptorSetLayouts.push_back(device.InternDescriptorSetLayout(m_Layout));
        cd.DebugName = "Ocean.RasterDepthConvert";
        m_PipelineId = device.InternComputePipeline(cd);
    }

    return m_PipelineId.IsValid();
}

bool OceanRasterDepthCapture::EnsureOutput(IDevice& device, uint32 resolution)
{
    resolution = std::clamp(resolution, 16u, 4096u);
    if (m_OutputTexture.IsValid() && m_Resolution == resolution)
        return true;

    if (m_OutputTexture.IsValid())
        device.DestroyTexture(m_OutputTexture);

    TextureDesc td{};
    td.width = resolution;
    td.height = resolution;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.sampleCount = 1;
    td.format = static_cast<uint32>(TextureFormat::R32_FLOAT);
    td.usage = static_cast<uint32>(TextureUsage::ShaderResource |
                                   TextureUsage::UnorderedAccess |
                                   TextureUsage::TransferSrc);
    td.initialState = ResourceState::ShaderResource;
    td.debugName = "Ocean_RasterWaterDepth";
    m_OutputTexture = device.CreateTexture(td);
    if (!m_OutputTexture.IsValid())
        return false;

    if (!m_Sampler.IsValid())
        m_Sampler =
            device.CreateSampler(SamplerDesc::MaterialLinearClamp("Ocean_RasterWaterDepth_Sampler"));

    m_Resolution = resolution;
    m_HasSampleableTexture = false;
    m_FramesUntilSampleable = 0u;
    m_HasCaptureFootprint = false;
    m_HaveLastCaptureFrame = false;
    {
        std::lock_guard<std::mutex> lock(m_ReadbackMutex);
        m_HasReadbackData = false;
    }
    return m_Sampler.IsValid();
}

bool OceanRasterDepthCapture::EnsureReadbackRing(IDevice& device, uint32 resolution)
{
    resolution = std::clamp(resolution, 16u, 4096u);
    const uint32 ringCount = std::max(1u, device.GetFramesInFlight()) + 2u;
    if (m_Readback.IsInitialized() && m_Readback.Count() == ringCount &&
        m_ReadbackResolution == resolution)
    {
        return true;
    }

    BufferDesc bd{};
    bd.size = sizeof(float32) * size_t(resolution) * size_t(resolution);
    bd.usage = static_cast<uint32>(BufferUsage::TransferDst);
    bd.memoryUsage = BufferMemoryUsage::Readback;
    bd.debugName = "Ocean_RasterDepth_Readback";
    if (!m_Readback.Init(&device, bd, ringCount))
        return false;

    m_ReadbackResolution = resolution;
    {
        std::lock_guard<std::mutex> lock(m_ReadbackMutex);
        m_CPUWaterDepth.assign(size_t(resolution) * size_t(resolution), 60000.0f);
        m_HasReadbackData = false;
    }
    return true;
}

void OceanRasterDepthCapture::InvalidateReadback()
{
    // Invalidation means DISCARD: in-flight readbacks must not resolve as
    // fresh after a settings toggle / capture retarget, or pre-invalidation
    // depth resurrects into SampleDepth on the re-enable frames.
    m_Readback.DropPendings();
    m_HasSampleableTexture = false;
    m_FramesUntilSampleable = 0u;
    m_HasCaptureFootprint = false;
    m_HaveLastCaptureFrame = false;
    m_HaveLastSampleableTickFrame = false;
    std::lock_guard<std::mutex> lock(m_ReadbackMutex);
    m_HasReadbackData = false;
}

void OceanRasterDepthCapture::ResolveReadyReadback()
{
    ReadbackMeta meta{};
    if (const void* mapped = m_Readback.MapNewestReady(m_Device, &meta))
    {
        std::lock_guard<std::mutex> lock(m_ReadbackMutex);
        const size_t count = size_t(m_ReadbackResolution) * size_t(m_ReadbackResolution);
        if (m_CPUWaterDepth.size() != count)
            m_CPUWaterDepth.resize(count, 60000.0f);
        std::memcpy(m_CPUWaterDepth.data(), mapped, sizeof(float32) * count);
        m_ReadOriginX = meta.OriginX;
        m_ReadOriginZ = meta.OriginZ;
        m_ReadSizeX = meta.SizeX;
        m_ReadSizeZ = meta.SizeZ;
        m_ReadDeepWaterDepth = meta.Deep;
        m_HasReadbackData = true;
        m_Readback.Unmap(m_Device);
    }
}

RG::RGTexture OceanRasterDepthCapture::ImportRG(RG::RGFrame& frame) const
{
    if (!m_OutputTexture.IsValid())
        return {};
    return frame.ImportExternalTexture("Ocean.RasterDepth.WaterDepth", m_OutputTexture,
                                       ResourceState::ShaderResource, TextureFormat::R32_FLOAT);
}

void OceanRasterDepthCapture::ScheduleReadback(RG::RGFrame& frame, RG::RGTexture output,
                                               uint32 resolution)
{
    if (!m_Device || !output.IsValid() || !EnsureReadbackRing(*m_Device, resolution))
        return;

    ReadbackMeta meta{};
    meta.OriginX = m_OriginX;
    meta.OriginZ = m_OriginZ;
    meta.SizeX = m_SizeX;
    meta.SizeZ = m_SizeZ;
    meta.Deep = m_DeepWaterDepth;
    const BufferHandle readbackBuffer = m_Readback.BeginWrite(frame, meta);
    ResolveReadyReadback();
    if (!readbackBuffer.IsValid())
        return;
    const uint32 copyResolution = resolution;
    const size_t rowPitch = sizeof(float32) * size_t(copyResolution);
    frame.AddPass(
        "Ocean.RasterDepth.Readback", PassPhase::kWorldRender,
        [&](RG::RGPassBuilder& p)
        {
            p.Read(output, RG::RGTextureRead::CopySrc);
            p.PreventCulling();
        },
        [output, readbackBuffer, copyResolution, rowPitch](RG::RGContext& ctx)
        {
            const TextureHandle tex = ctx.GetTexture(output);
            if (tex.IsValid() && readbackBuffer.IsValid() && ctx.Cmd)
            {
                ctx.Cmd->CopyTextureToBuffer(tex, readbackBuffer, copyResolution,
                                             copyResolution, 0, 0, 0, rowPitch);
            }
        });
}

bool OceanRasterDepthCapture::SampleDepth(float32 worldX, float32 worldZ,
                                          float32& outDepth) const
{
    std::lock_guard<std::mutex> lock(m_ReadbackMutex);
    if (!m_HasReadbackData || m_ReadbackResolution < 2u || m_CPUWaterDepth.empty())
        return false;

    const float32 sizeX = std::max(m_ReadSizeX, 1e-3f);
    const float32 sizeZ = std::max(m_ReadSizeZ, 1e-3f);
    const float32 u = (worldX - m_ReadOriginX) / sizeX;
    const float32 v = (worldZ - m_ReadOriginZ) / sizeZ;
    if (u < 0.0f || v < 0.0f || u > 1.0f || v > 1.0f)
        return false;

    const float32 halfTexel = 0.5f / float32(m_ReadbackResolution);
    const float32 sampleU = std::clamp(u, halfTexel, 1.0f - halfTexel);
    const float32 sampleV = std::clamp(v, halfTexel, 1.0f - halfTexel);
    const float32 gx = sampleU * float32(m_ReadbackResolution) - 0.5f;
    const float32 gz = sampleV * float32(m_ReadbackResolution) - 0.5f;
    const int x0 = std::clamp(static_cast<int>(std::floor(gx)), 0,
                              static_cast<int>(m_ReadbackResolution) - 2);
    const int z0 = std::clamp(static_cast<int>(std::floor(gz)), 0,
                              static_cast<int>(m_ReadbackResolution) - 2);
    const float32 tx = gx - float32(x0);
    const float32 tz = gz - float32(z0);
    const uint32 n = m_ReadbackResolution;
    const float32 d00 = m_CPUWaterDepth[size_t(z0) * n + uint32(x0)];
    const float32 d10 = m_CPUWaterDepth[size_t(z0) * n + uint32(x0 + 1)];
    const float32 d01 = m_CPUWaterDepth[size_t(z0 + 1) * n + uint32(x0)];
    const float32 d11 = m_CPUWaterDepth[size_t(z0 + 1) * n + uint32(x0 + 1)];
    const float32 d0 = d00 + (d10 - d00) * tx;
    const float32 d1 = d01 + (d11 - d01) * tx;
    const float32 depth = d0 + (d1 - d0) * tz;
    const float32 deep = std::max(m_ReadDeepWaterDepth, 1.0f);
    if (depth >= deep - 1e-3f)
        return false;
    outDepth = std::clamp(depth, 0.0f, deep);
    return true;
}

void OceanRasterDepthCapture::EnsureView(Engine::Renderer::RenderServices& rs,
                                         const ViewDesc& mainView)
{
    if (m_ViewAllocated)
        return;
    m_RenderServices = &rs;
    m_CameraId = rs.Views().AllocateCamera("Ocean Raster Depth Camera");
    m_ViewId = rs.Views().AllocateView("Ocean Raster Depth", m_CameraId, ViewPurpose::UtilityCapture,
                               Rendering::ViewParticipation::OnDemand);
    rs.Views().SetViewActiveRenderPipeline(m_ViewId, false);
    rs.Views().SetViewRenderLayerMask(m_ViewId, mainView.renderLayerMask);
    rs.Views().SetViewWorldId(m_ViewId, mainView.worldId);
    m_ViewAllocated = true;
}

CameraData OceanRasterDepthCapture::MakeTopDownCamera(
    float32 centerX, float32 centerZ, float32 seaLevel,
    const OceanRasterDepthCaptureSettings& settings) const
{
    using GameEngine::Mathematics::MakeLookAtLH;
    using GameEngine::Mathematics::MakeOrthographicLH_ZO_ReverseZ;
    using GameEngine::Mathematics::Matrix4x4;
    using GameEngine::Mathematics::Vector3;

    const float32 sizeX = std::max(settings.SizeX, 1.0f);
    const float32 sizeZ = std::max(settings.SizeZ, 1.0f);
    const float32 topPadding = std::max(settings.TopPadding, 0.01f);
    const float32 deep = std::max(settings.DeepWaterDepth, 1.0f);

    const Vector3 eye(centerX, seaLevel + topPadding, centerZ);
    const Vector3 target(centerX, seaLevel + topPadding - 1.0f, centerZ);
    const Vector3 up(0.0f, 0.0f, 1.0f);
    const Matrix4x4 view = MakeLookAtLH(eye, target, up);
    const Matrix4x4 proj = MakeOrthographicLH_ZO_ReverseZ(
        -sizeX * 0.5f, sizeX * 0.5f, -sizeZ * 0.5f, sizeZ * 0.5f,
        topPadding, topPadding + deep);
    const Matrix4x4 viewProj = proj * view;

    CameraData out{};
    std::memcpy(out.view, view.Data(), sizeof(float) * 16u);
    std::memcpy(out.proj, proj.Data(), sizeof(float) * 16u);
    std::memcpy(out.viewProj, viewProj.Data(), sizeof(float) * 16u);
    out.cameraPos[0] = eye.x;
    out.cameraPos[1] = eye.y;
    out.cameraPos[2] = eye.z;
    return out;
}

bool OceanRasterDepthCapture::DeclareForView(Engine::Renderer::Pipeline::ViewDeclare& d,
                                             float32 centerX, float32 centerZ,
                                             float32 seaLevel,
                                             const OceanRasterDepthCaptureSettings& settings)
{
    if (!settings.Enabled)
        return false;

    auto* device = d.Services.GetDevice();
    if (!device)
        return false;
    m_Device = device;

    const uint64 graphFrameIndex = d.Frame.FrameIndex();
    if (!m_HaveLastSampleableTickFrame || m_LastSampleableTickFrame != graphFrameIndex)
    {
        m_HaveLastSampleableTickFrame = true;
        m_LastSampleableTickFrame = graphFrameIndex;
        if (m_FramesUntilSampleable > 0u)
        {
            --m_FramesUntilSampleable;
            if (m_FramesUntilSampleable == 0u)
                m_HasSampleableTexture = true;
        }
    }

    const uint32 resolution =
        std::clamp(settings.Resolution == 0u ? 512u : settings.Resolution, 16u, 4096u);
    if (!EnsurePipeline(*device) || !EnsureOutput(*device, resolution))
        return false;

    const uint32 captureMask = settings.RenderLayerMask & d.View.renderLayerMask;
    if (captureMask == 0u)
        return false;

    const float32 sizeX = std::max(settings.SizeX, 1.0f);
    const float32 sizeZ = std::max(settings.SizeZ, 1.0f);
    const float32 texelX = sizeX / static_cast<float32>(resolution);
    const float32 texelZ = sizeZ / static_cast<float32>(resolution);
    const float32 snappedCenterX = SnapToTexel(centerX, texelX);
    const float32 snappedCenterZ = SnapToTexel(centerZ, texelZ);
    const float32 originX = snappedCenterX - sizeX * 0.5f;
    const float32 originZ = snappedCenterZ - sizeZ * 0.5f;
    const float32 deep = std::max(settings.DeepWaterDepth, 1.0f);
    const float32 topPadding = std::max(settings.TopPadding, 0.01f);

    const bool alreadyCapturedThisFrame =
        m_HaveLastCaptureFrame && m_LastCaptureFrame == graphFrameIndex;
    const bool footprintDirty =
        !m_HasCaptureFootprint ||
        !NearlyEqual(m_OriginX, originX) ||
        !NearlyEqual(m_OriginZ, originZ) ||
        !NearlyEqual(m_SizeX, sizeX) ||
        !NearlyEqual(m_SizeZ, sizeZ) ||
        !NearlyEqual(m_DeepWaterDepth, deep) ||
        !NearlyEqual(m_CaptureSeaLevel, seaLevel) ||
        !NearlyEqual(m_CaptureTopPadding, topPadding) ||
        m_CaptureMask != captureMask ||
        m_CaptureWorldId != d.View.worldId;

    if (alreadyCapturedThisFrame || !footprintDirty)
    {
        // Steady state: no new copy to schedule — just poll the pending
        // readback (token-signaled slots resolve into the CPU tile).
        ResolveReadyReadback();
        return true;
    }

    EnsureView(d.Services, d.View);
    if (m_ViewId == 0 || m_CameraId == 0)
        return false;

    // OnDemand arm/capture split (mirrors the reflection probe's cycle): this
    // frame's extraction ran before this declare, so a request made now feeds
    // the view NEXT frame — capturing immediately would bake an empty depth
    // strip. Arm (request + wait one frame), then capture on the armed frame.
    // The +1 check re-arms after any gap (settings toggled off, dropped frame),
    // where the old request has lapsed and extraction skipped the view again.
    if (!m_CaptureArmed || graphFrameIndex != m_ArmedAtFrame + 1)
    {
        d.Services.Views().RequestViewFrame(m_ViewId);
        m_CaptureArmed = true;
        m_ArmedAtFrame = graphFrameIndex;
        ResolveReadyReadback();
        return true;
    }
    m_CaptureArmed = false;

    m_OriginX = originX;
    m_OriginZ = originZ;
    m_SizeX = sizeX;
    m_SizeZ = sizeZ;
    m_DeepWaterDepth = deep;
    m_CaptureSeaLevel = seaLevel;
    m_CaptureTopPadding = topPadding;
    m_CaptureMask = captureMask;
    m_CaptureWorldId = d.View.worldId;
    m_HasCaptureFootprint = true;

    d.Services.Views().SetCameraData(m_CameraId,
                             MakeTopDownCamera(snappedCenterX, snappedCenterZ,
                                               seaLevel, settings));
    d.Services.Views().SetViewRenderLayerMask(m_ViewId, captureMask);
    d.Services.Views().SetViewWorldId(m_ViewId, d.View.worldId);

    d.Services.WriteViewLightBuffer(m_ViewId);
    d.Services.BuildWorldBatchKeysForView(m_ViewId);
    if (!d.Services.ScheduleBucketerDispatchesForView(d.Frame, m_ViewId))
        return false;

    Rendering::ViewClearConfig clear{};
    clear.clearColor = true;
    clear.clearDepth = true;
    clear.clearDepthValue = 0.0f;
    d.Services.Views().SetViewClearConfig(m_ViewId, clear);

    TextureDesc colorDesc{};
    colorDesc.width = resolution;
    colorDesc.height = resolution;
    colorDesc.depth = 1;
    colorDesc.mipLevels = 1;
    colorDesc.arrayLayers = 1;
    colorDesc.sampleCount = 1;
    colorDesc.format = static_cast<uint32>(TextureFormat::RGBA8_UNORM);
    colorDesc.usage = static_cast<uint32>(TextureUsage::RenderTarget);
    colorDesc.debugName = "Ocean.RasterDepth.Color";

    TextureDesc depthDesc{};
    depthDesc.width = resolution;
    depthDesc.height = resolution;
    depthDesc.depth = 1;
    depthDesc.mipLevels = 1;
    depthDesc.arrayLayers = 1;
    depthDesc.sampleCount = 1;
    depthDesc.format = static_cast<uint32>(d.Services.GetDepthFormat());
    depthDesc.usage =
        static_cast<uint32>(TextureUsage::DepthStencil | TextureUsage::ShaderResource);
    depthDesc.debugName = "Ocean.RasterDepth.Depth";

    // The hidden world pass resolves scene targets through RenderServices' physical
    // resource path, so these must be imported from the persistent pool.
    const RG::RGTexture color =
        d.Frame.ImportPersistentTexture("Ocean.RasterDepth.Color", colorDesc);
    const RG::RGTexture depth =
        d.Frame.ImportPersistentTexture("Ocean.RasterDepth.Depth", depthDesc);
    const RG::RGTexture output = d.Frame.ImportExternalTexture(
        "Ocean.RasterDepth.WaterDepth", m_OutputTexture, ResourceState::ShaderResource,
        TextureFormat::R32_FLOAT);
    if (!color.IsValid() || !depth.IsValid() || !output.IsValid())
        return false;

    Engine::Renderer::RenderServices::WorldPassTargetsRG targets{};
    targets.Color = color;
    targets.Depth = depth;
    d.Services.EmitProducerForwardCommandsForView(m_ViewId);
    const auto world = d.Services.AddWorldPassForView(d.Frame, m_ViewId, targets,
                                                      MaterialKeyword::Instanced);
    if (!world.Pass.IsValid())
        return false;

    // Converter params live in the frame's upload ring (host-coherent, written
    // here at declare time); the pass captures only {buffer, offset}.
    const auto paramsAlloc = d.Frame.AllocUpload<OceanRasterDepthParamsGPU>();
    if (!paramsAlloc.Valid())
        return false;
    OceanRasterDepthParamsGPU params{};
    params.NearFarDeepResolution[0] = topPadding;
    params.NearFarDeepResolution[1] = topPadding + deep;
    params.NearFarDeepResolution[2] = deep;
    params.NearFarDeepResolution[3] = static_cast<float32>(m_Resolution);
    *paramsAlloc.Ptr = params;

    const auto layout = m_Layout;
    const ComputePipelineId pipeId = m_PipelineId;
    const SamplerHandle sampler = m_Sampler;
    const uint32 dispatchGroups = (resolution + 7u) / 8u;
    d.Frame.AddPass(
        "Ocean.RasterDepth.Convert", PassPhase::kWorldRender,
        [&](RG::RGPassBuilder& p)
        {
            p.Read(depth, RG::RGTextureRead::SampledCompute);
            p.Write(output, RG::RGTextureWrite::Storage);
        },
        [depth, output, layout, pipeId, sampler, paramsBuffer = paramsAlloc.Buffer,
         paramsOffset = paramsAlloc.Offset, dispatchGroups](RG::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl)
                return;
            const TextureHandle depthTex = ctx.GetTexture(depth);
            const TextureHandle outTex = ctx.GetTexture(output);
            if (!depthTex.IsValid() || !outTex.IsValid())
                return;
            const PipelineHandle pipe = dev->GetOrCreateComputePipeline(pipeId);
            if (!pipe)
                return;

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = layout;
            dsDesc.debugName = "Ocean.RasterDepth.Convert.DS";
            dsDesc.transient = true;
            const DescriptorSetHandle ds = dev->CreateDescriptorSet(dsDesc);
            dev->UpdateCombinedImageSamplerBinding(ds, 0, depthTex, sampler);
            dev->UpdateStorageImageBinding(ds, 1, outTex);
            dev->UpdateBufferBinding(ds, 2, paramsBuffer, paramsOffset,
                                     sizeof(OceanRasterDepthParamsGPU));

            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Dispatch(dispatchGroups, dispatchGroups, 1);
        });

    ScheduleReadback(d.Frame, output, resolution);

    // The readback copy leaves the texture in TransferSrc; consumers sample
    // only after m_FramesUntilSampleable, so the end-of-frame export
    // transition restores ShaderReadOnly for next frame's import claim.
    d.Frame.MarkOutput(output, RG::RGImageLayout::ShaderReadOnly);

    m_HaveLastCaptureFrame = true;
    m_LastCaptureFrame = graphFrameIndex;
    if (!m_HasSampleableTexture)
        m_FramesUntilSampleable = std::max(m_FramesUntilSampleable, 1u);
    return true;
}

} // namespace GameEngine::Ocean
