#include "Ocean/OceanDynWavesReadback.h"
#include "Ocean/OceanShaderProgram.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "OceanShaderDirectory.h"

#include "Logger/Logger.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
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

namespace
{

} // anonymous namespace

bool OceanDynWavesReadback::Initialize(IDevice* device)
{
    if (m_Ready)
        return true;
    if (!device)
        return false;
    m_Device = device;

    const fs::path shaderDir = OceanShaderDirectory("ocean_dynwaves_query.comp");
    if (shaderDir.empty())
    {
        Logger::Log::Warning("OceanDynWavesReadback: ocean_dynwaves_query.comp not found");
        return false;
    }
    const fs::path includeDir = shaderDir.parent_path(); // Assets/Shaders, for "Ocean/..." includes
    const fs::path cacheRoot = fs::path(".Cache") / "Shaders";

    ShaderProgramCompileRequest req{};
    req.debugName = "ocean_dynwaves_query";
    req.baseDirectory = shaderDir;
    req.cacheRoot = cacheRoot;
    req.includeDirs = {includeDir};
    req.stages = {{"cs", "ocean_dynwaves_query.comp", "main", {}}};

    ShaderProgramCompileResult result{};
    std::string err;
    if (!LoadOceanShaderProgram(req, device->PreferredShaderSource(), result, &err))
    {
        Logger::Log::Warning("OceanDynWavesReadback: compile failed: {}", err);
        return false;
    }
    auto it = result.stageBytes.find("cs");
    if (it == result.stageBytes.end() || it->second.empty())
    {
        Logger::Log::Warning("OceanDynWavesReadback: no SPIR-V");
        return false;
    }
    auto shaderBytes = std::make_shared<const std::vector<uint8_t>>(std::move(it->second));

    m_Layout = DescriptorSetLayoutDesc{};
    m_Layout.debugName = "OceanDynWavesQuery_Set0";
    auto addBinding = [&](uint32 binding, DescriptorType type) {
        DescriptorBinding b{};
        b.binding = binding;
        b.type = type;
        b.count = 1;
        b.shaderStages = kShaderStageCompute;
        m_Layout.bindings.push_back(b);
    };
    addBinding(0, DescriptorType::UniformBuffer);
    addBinding(1, DescriptorType::CombinedImageSampler);
    addBinding(2, DescriptorType::StorageBuffer);

    ComputePipelineDesc cd{};
    cd.ComputeShader = shaderBytes;
    Rendering::ApplyMetaImageShapeToLayout(result.meta, m_Layout);
    cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
    cd.DebugName = "OceanDynWavesQuery";
    m_Pipe = m_Device->InternComputePipeline(cd);
    if (!m_Pipe.IsValid())
        return false;

    const uint32 ringCount = std::max(1u, m_Device->GetFramesInFlight()) + 2u;
    // The query dispatch writes each slot directly as a storage UAV (no copy
    // pass), so the slots carry Storage usage on top of the ring's usual
    // TransferDst. The memory class stays Readback: the CPU copies every slot
    // out each frame, and vkUsage is built from BufferDesc::usage independently
    // of the class, so Storage and Readback compose.
    BufferDesc bd{};
    bd.size = sizeof(float) * size_t(kResolution) * size_t(kResolution);
    bd.usage = static_cast<uint32>(BufferUsage::Storage) | static_cast<uint32>(BufferUsage::TransferDst);
    bd.memoryUsage = BufferMemoryUsage::Readback;
    bd.flags = BufferCreateFlags::PersistentlyMapped;
    bd.debugName = "Ocean_DynWaves_Readback";
    if (!m_Ring.Init(m_Device, bd, ringCount))
    {
        Logger::Log::Warning("OceanDynWavesReadback: readback ring allocation failed");
        return false;
    }

    m_CPUHeights.assign(size_t(kResolution) * size_t(kResolution), 0.0f);

    m_Ready = true;
    Logger::Log::Info("OceanDynWavesReadback: ready ({}x{} tile {}m, {} m/texel, ring {})",
                      kResolution, kResolution, kTileMeters, m_TexelSize, m_Ring.Count());
    return true;
}

void OceanDynWavesReadback::BeginFrame(float cameraX, float cameraZ)
{
    const float halfTile = kTileMeters * 0.5f;
    m_OriginX = std::floor((cameraX - halfTile) / m_TexelSize) * m_TexelSize;
    m_OriginZ = std::floor((cameraZ - halfTile) / m_TexelSize) * m_TexelSize;
}

void OceanDynWavesReadback::SetFrameInputs(TextureHandle dynWaves, SamplerHandle dynSampler,
                                           const OceanCascadeLayoutGPU& layout)
{
    m_DynWaves = dynWaves;
    m_DynSampler = dynSampler;
    m_CascadeLayout = layout;
}

void OceanDynWavesReadback::FillParams(OceanDynWavesQueryParamsGPU& out)
{
    out = {};
    out.Resolution = kResolution;
    out.DynLodCount = std::min(m_CascadeLayout.LodCount, kMaxOceanLodCascades);
    out.OriginX = m_OriginX;
    out.OriginZ = m_OriginZ;
    out.TexelSize = m_TexelSize;
    out.Cascade = m_CascadeLayout;
    if (out.Cascade.LodCount > out.DynLodCount)
        out.Cascade.LodCount = out.DynLodCount;
}

void OceanDynWavesReadback::RecordDispatch(IDevice* device, CommandList* cl,
                                           const RenderGraph::RGFrame& frame,
                                           BufferHandle paramsBuffer, uint64 paramsOffset)
{
    if (!m_Ready || !cl || !device || !m_Ring.IsInitialized() || !paramsBuffer.IsValid())
        return;
    if (!m_DynWaves.IsValid() || !m_DynSampler.IsValid() || m_CascadeLayout.LodCount == 0)
        return;

    PipelineHandle pipe = device->GetOrCreateComputePipeline(m_Pipe);
    if (!pipe)
        return;

    // BeginWrite registers a completion pending, so it must not run on frames
    // where the pipeline is unavailable and nothing dispatches.
    ReadbackMeta meta{};
    meta.OriginX = m_OriginX;
    meta.OriginZ = m_OriginZ;
    const BufferHandle outBuf = m_Ring.BeginWrite(frame, meta);
    if (!outBuf.IsValid())
        return;

    cl->SetPipeline(pipe);

    DescriptorSetDesc dsDesc{};
    dsDesc.layout = m_Layout;
    dsDesc.transient = true;
    dsDesc.debugName = "OceanDynWavesQuery_DS";
    DescriptorSetHandle ds = device->CreateDescriptorSet(dsDesc);

    device->UpdateBufferBinding(ds, 0, paramsBuffer, paramsOffset, sizeof(OceanDynWavesQueryParamsGPU));
    device->UpdateCombinedImageSamplerBinding(ds, 1, m_DynWaves, m_DynSampler);
    device->UpdateStorageBufferBinding(ds, 2, outBuf, 0,
                                       sizeof(float) * size_t(kResolution) * size_t(kResolution));

    cl->BindDescriptorSet(0, ds, pipe);

    const uint32 g8 = (kResolution + 7u) / 8u;
    cl->Dispatch(g8, g8, 1);

    // No-op unless the backend keeps the mappable slot in a second allocation
    // (RGReadbackRing::RecordResolve).
    m_Ring.RecordResolve(cl, outBuf);

    ReadbackMeta ready{};
    if (const void* mapped = m_Ring.MapNewestReady(device, &ready))
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        std::memcpy(m_CPUHeights.data(), mapped,
                    sizeof(float) * size_t(kResolution) * size_t(kResolution));
        m_ReadOriginX = ready.OriginX;
        m_ReadOriginZ = ready.OriginZ;
        m_HasData = true;
        m_Ring.Unmap(device);
    }
}

bool OceanDynWavesReadback::Sample(float worldX, float worldZ, float& outHeight) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_HasData)
        return false;

    const float gx = (worldX - m_ReadOriginX) / m_TexelSize - 0.5f;
    const float gz = (worldZ - m_ReadOriginZ) / m_TexelSize - 0.5f;
    const float fi = std::floor(gx);
    const float fj = std::floor(gz);
    const int i0 = static_cast<int>(fi);
    const int j0 = static_cast<int>(fj);
    if (i0 < 0 || j0 < 0 || i0 >= static_cast<int>(kResolution) - 1 ||
        j0 >= static_cast<int>(kResolution) - 1)
        return false;

    const float tx = gx - fi;
    const float tz = gz - fj;
    const uint32 n = kResolution;
    const float h00 = m_CPUHeights[size_t(j0) * n + i0];
    const float h10 = m_CPUHeights[size_t(j0) * n + (i0 + 1)];
    const float h01 = m_CPUHeights[size_t(j0 + 1) * n + i0];
    const float h11 = m_CPUHeights[size_t(j0 + 1) * n + (i0 + 1)];
    const float h0 = h00 + (h10 - h00) * tx;
    const float h1 = h01 + (h11 - h01) * tx;
    outHeight = h0 + (h1 - h0) * tz;
    return true;
}

void OceanDynWavesReadback::OnOriginShift(float32 shiftX, float32 shiftZ, bool invalidate)
{
    m_OriginX -= shiftX;
    m_OriginZ -= shiftZ;
    for (uint32 lod = 0u; lod < m_CascadeLayout.LodCount; ++lod)
    {
        m_CascadeLayout.CascadeOriginScale[lod][0] -= shiftX;
        m_CascadeLayout.CascadeOriginScale[lod][1] -= shiftZ;
    }
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_ReadOriginX -= shiftX;
    m_ReadOriginZ -= shiftZ;
    m_Ring.DropPendings();
    if (invalidate)
        m_HasData = false;
}

} // namespace GameEngine::Ocean
