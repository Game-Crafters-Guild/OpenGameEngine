#include "Ocean/OceanGPUQuery.h"
#include "Ocean/OceanShaderProgram.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "OceanShaderDirectory.h"

#include "Logger/Logger.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/ShaderCompileService.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>

namespace GameEngine::Ocean
{
namespace fs = std::filesystem;
using namespace ::GameEngine::Rendering;

namespace
{
}

bool OceanGPUQuery::Initialize(IDevice* device)
{
    if (m_Ready)
        return true;
    if (!device)
        return false;
    m_Device = device;
    const fs::path shaderDirectory = OceanShaderDirectory("ocean_gpu_query.comp");
    if (shaderDirectory.empty())
        return false;

    ShaderProgramCompileRequest request{};
    request.debugName = "ocean_gpu_query";
    request.baseDirectory = shaderDirectory;
    request.cacheRoot = fs::path(".Cache") / "Shaders";
    request.includeDirs = {shaderDirectory.parent_path()};
    request.stages = {{"cs", "ocean_gpu_query.comp", "main", {}}};
    ShaderProgramCompileResult result{};
    std::string error;
    if (!LoadOceanShaderProgram(request, device->PreferredShaderSource(), result, &error))
    {
        Logger::Log::Warning("OceanGPUQuery: compile failed: {}", error);
        return false;
    }
    auto stage = result.stageBytes.find("cs");
    if (stage == result.stageBytes.end() || stage->second.empty())
        return false;
    auto shaderBytes = std::make_shared<const std::vector<uint8_t>>(std::move(stage->second));

    m_Layout.debugName = "OceanGPUQuery_Set0";
    auto binding = [&](uint32 index, DescriptorType type) {
        DescriptorBinding desc{};
        desc.binding = index;
        desc.type = type;
        desc.count = 1u;
        desc.shaderStages = kShaderStageCompute;
        m_Layout.bindings.push_back(desc);
    };
    binding(0u, DescriptorType::UniformBuffer);
    binding(1u, DescriptorType::CombinedImageSampler);
    binding(2u, DescriptorType::CombinedImageSampler);
    binding(3u, DescriptorType::StorageBuffer);

    ComputePipelineDesc pipeline{};
    pipeline.ComputeShader = shaderBytes;
    Rendering::ApplyMetaImageShapeToLayout(result.meta, m_Layout);
    pipeline.DescriptorSetLayouts.push_back(device->InternDescriptorSetLayout(m_Layout));
    pipeline.DebugName = "OceanGPUQuery";
    m_Pipeline = device->InternComputePipeline(pipeline);
    if (!m_Pipeline.IsValid())
        return false;

    // The dispatch writes results straight into the slot as a storage UAV, and
    // the CPU reads them back — Readback asks VMA for a host-cached type, which
    // is what the CPU-side copy wants.
    BufferDesc output{};
    output.size = sizeof(ResultGPU) * kMaxOceanGPUQueryPointsPerDispatch;
    output.usage = static_cast<uint32>(BufferUsage::Storage) |
                   static_cast<uint32>(BufferUsage::TransferDst);
    output.memoryUsage = BufferMemoryUsage::Readback;
    output.flags = BufferCreateFlags::PersistentlyMapped;
    output.debugName = "Ocean_GPUQuery_Readback";
    const uint32 ringCount = std::max(device->GetFramesInFlight(), 1u) + 2u;
    if (!m_Readback.Init(device, output, ringCount))
        return false;
    m_Ready = true;
    return true;
}

OceanGPUQueryToken OceanGPUQuery::Enqueue(const OceanSurfaceQueryPoint* points, uint32 count,
                                          float32 minimumSpatialLength)
{
    if (!points || count == 0u || count > kMaxOceanGPUQueryPointsPerDispatch)
        return 0u;
    OceanGPUQueryToken token = m_NextToken.fetch_add(1u, std::memory_order_relaxed);
    if (token == 0u)
        token = m_NextToken.fetch_add(1u, std::memory_order_relaxed);
    Request request{};
    request.Token = token;
    request.MinimumSpatialLength = std::max(minimumSpatialLength, 0.0f);
    request.Points.assign(points, points + count);
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Pending.push_back(std::move(request));
    return token;
}

bool OceanGPUQuery::HasPending() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return !m_Pending.empty();
}

bool OceanGPUQuery::PrepareNext(const OceanParamsGPU& ocean,
                                const OceanCascadeLayoutGPU& combinedLayout,
                                bool combinedAvailable)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Pending.empty())
        return false;
    m_Active = std::move(m_Pending.front());
    m_Pending.pop_front();
    m_Params = {};
    m_Params.PointCount = static_cast<uint32>(m_Active.Points.size());
    m_Params.FFTCascadeCount = ocean.FFTCascadeCount;
    m_Params.CombinedAvailable = combinedAvailable ? 1u : 0u;
    m_Params.SeaLevel = ocean.SeaLevel;
    m_Params.Weight = ocean.Weight;
    m_Params.MaxHorizontal = ocean.MaxHorizontalDisplacement;
    m_Params.MaxVertical = ocean.MaxVerticalDisplacement;
    m_Params.WaveOriginOffsetX = ocean.WaveOriginOffsetX;
    m_Params.WaveOriginOffsetZ = ocean.WaveOriginOffsetZ;
    m_Params.CombinedCascade = combinedLayout;
    for (uint32 i = 0u; i < m_Params.PointCount; ++i)
    {
        m_Params.Points[i][0] = m_Active.Points[i].X;
        m_Params.Points[i][1] = m_Active.Points[i].Z;
        m_Params.Points[i][2] = m_Active.MinimumSpatialLength;
        m_Params.Points[i][3] = ocean.Time;
    }
    return true;
}

void OceanGPUQuery::SetFrameInputs(TextureHandle fft, SamplerHandle fftSampler,
                                   TextureHandle combined, SamplerHandle combinedSampler)
{
    m_FFT = fft;
    m_FFTSampler = fftSampler;
    m_Combined = combined;
    m_CombinedSampler = combinedSampler;
}

void OceanGPUQuery::RecordDispatch(IDevice* device, CommandList* commandList,
                                   const RenderGraph::RGFrame& frame,
                                   BufferHandle paramsBuffer, uint64 paramsOffset)
{
    if (!m_Ready || !device || !commandList || !paramsBuffer.IsValid() ||
        !m_FFT.IsValid() || !m_FFTSampler.IsValid() || m_Active.Token == 0u)
        return;
    const PipelineHandle pipeline = device->GetOrCreateComputePipeline(m_Pipeline);
    if (!pipeline)
        return;
    ReadbackMeta metadata{m_Active.Token, static_cast<uint32>(m_Active.Points.size()),
                          m_Params.Points[0][3]};
    const BufferHandle output = m_Readback.BeginWrite(frame, metadata);
    if (!output.IsValid())
        return;

    commandList->SetPipeline(pipeline);
    DescriptorSetDesc setDesc{};
    setDesc.layout = m_Layout;
    setDesc.transient = true;
    setDesc.debugName = "OceanGPUQuery_DS";
    const DescriptorSetHandle set = device->CreateDescriptorSet(setDesc);
    device->UpdateBufferBinding(set, 0u, paramsBuffer, paramsOffset,
                                sizeof(OceanGPUQueryParamsGPU));
    device->UpdateCombinedImageSamplerBinding(set, 1u, m_FFT, m_FFTSampler);
    device->UpdateCombinedImageSamplerBinding(
        set, 2u, m_Combined.IsValid() ? m_Combined : m_FFT,
        m_CombinedSampler.IsValid() ? m_CombinedSampler : m_FFTSampler);
    device->UpdateStorageBufferBinding(set, 3u, output, 0u,
                                       sizeof(ResultGPU) * kMaxOceanGPUQueryPointsPerDispatch);
    commandList->BindDescriptorSet(0u, set, pipeline);
    commandList->Dispatch((metadata.Count + 63u) / 64u, 1u, 1u);

    // No-op unless the backend keeps the mappable slot in a second allocation
    // (RGReadbackRing::RecordResolve).
    m_Readback.RecordResolve(commandList, output);

    m_Active = {};
}

void OceanGPUQuery::PollCompleted(IDevice* device)
{
    if (!m_Ready || !device)
        return;
    ReadbackMeta metadata{};
    const auto* mapped = static_cast<const ResultGPU*>(
        m_Readback.MapNewestReady(device, &metadata));
    if (!mapped || metadata.Token == 0u || metadata.Count == 0u)
        return;
    std::vector<OceanSurfaceSample> samples(metadata.Count);
    for (uint32 i = 0u; i < metadata.Count; ++i)
    {
        OceanSurfaceSample& sample = samples[i];
        sample.Valid = true;
        sample.Source = OceanQuerySource::GPUQuery;
        std::memcpy(sample.PositionWS, mapped[i].Position, sizeof(float32) * 3u);
        std::memcpy(sample.NormalWS, mapped[i].Normal, sizeof(float32) * 3u);
        std::memcpy(sample.DisplacementWS, mapped[i].Displacement, sizeof(float32) * 3u);
        sample.Height = sample.PositionWS[1];
    }
    m_Readback.Unmap(device);
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Completed[metadata.Token] = std::move(samples);
}

bool OceanGPUQuery::TryTake(OceanGPUQueryToken token, std::vector<OceanSurfaceSample>& out)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto found = m_Completed.find(token);
    if (found == m_Completed.end())
        return false;
    out = std::move(found->second);
    m_Completed.erase(found);
    return true;
}

void OceanGPUQuery::Clear()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Pending.clear();
    m_Completed.clear();
    m_Active = {};
    m_Readback.DropPendings();
}

} // namespace GameEngine::Ocean
