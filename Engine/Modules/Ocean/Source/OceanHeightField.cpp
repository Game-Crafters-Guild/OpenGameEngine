#include "Ocean/OceanHeightField.h"
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

bool OceanHeightField::Initialize(IDevice* device)
{
    if (m_Ready)
        return true;
    if (!device)
        return false;
    m_Device = device;

    const fs::path shaderDir = OceanShaderDirectory("ocean_height_query.comp");
    if (shaderDir.empty())
    {
        Logger::Log::Warning("OceanHeightField: ocean_height_query.comp not found");
        return false;
    }
    const fs::path includeDir = shaderDir.parent_path(); // Assets/Shaders, for "Ocean/..." includes
    const fs::path cacheRoot = fs::path(".Cache") / "Shaders";

    ShaderProgramCompileRequest req{};
    req.debugName = "ocean_height_query";
    req.baseDirectory = shaderDir;
    req.cacheRoot = cacheRoot;
    req.includeDirs = {includeDir};
    req.stages = {{"cs", "ocean_height_query.comp", "main", {}}};

    ShaderProgramCompileResult result{};
    std::string err;
    if (!LoadOceanShaderProgram(req, device->PreferredShaderSource(), result, &err))
    {
        Logger::Log::Warning("OceanHeightField: compile failed: {}", err);
        return false;
    }
    auto it = result.stageBytes.find("cs");
    if (it == result.stageBytes.end() || it->second.empty())
    {
        Logger::Log::Warning("OceanHeightField: no SPIR-V");
        return false;
    }
    auto shaderBytes = std::make_shared<const std::vector<uint8_t>>(std::move(it->second));

    // Layout: UBO(0), FFT-displacement sampler(1), output displacement storage buffer(2).
    m_Layout = DescriptorSetLayoutDesc{};
    m_Layout.debugName = "OceanHeightQuery_Set0";
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
    cd.DebugName = "OceanHeightQuery";
    m_Pipe = m_Device->InternComputePipeline(cd);
    if (!m_Pipe.IsValid())
        return false;

    // The ring below IS the compute output and is mapped by the CPU, which
    // needs a buffer that is both Storage and host-visible. WebGPU cannot
    // express that — MapRead composes with CopyDst and nothing else — so the
    // buffer comes back without Storage usage and every dispatch binding it is
    // rejected. Declining keeps the water: this subsystem only serves CPU
    // height/displacement queries, and the surface itself never reads it.
    if (m_Device && !m_Device->GetCapabilities().supportsMappableStorageBuffers)
    {
        Logger::Log::Info("OceanHeightField: CPU displacement queries need a mappable storage "
                          "buffer, which this backend does not offer; height queries stay off.");
        return false;
    }

    // Host-visible storage ring: GPU UAV-writes the bake, the CPU maps the oldest
    // slot. Size FramesInFlight + 2 so the slot read each frame is guaranteed
    // complete without a fence token (the bake runs every frame — regular latency).
    // Unlike copy-based readback rings this one IS the compute output, so it keeps
    // Storage usage. The memory class does not constrain that — vkUsage is built
    // from BufferDesc::usage independently — so the class is Readback, which asks
    // VMA for a host-cached type. The CPU memcpy's the whole slot out every frame,
    // and reading the write-combined memory Upload asks for is far slower.
    const uint32 ringCount = std::max(1u, m_Device->GetFramesInFlight()) + 2u;
    BufferDesc bd{};
    bd.size =
        sizeof(OceanHeightField::DisplacementSample) * size_t(kResolution) * size_t(kResolution);
    bd.usage = static_cast<uint32>(BufferUsage::Storage) | static_cast<uint32>(BufferUsage::TransferDst);
    bd.memoryUsage = BufferMemoryUsage::Readback;
    bd.flags = BufferCreateFlags::PersistentlyMapped;
    bd.debugName = "Ocean_DisplacementField_Readback";
    if (!m_Readback.Init(m_Device, bd, ringCount))
    {
        Logger::Log::Warning("OceanHeightField: readback ring allocation failed");
        return false;
    }

    m_CPUDisplacements.assign(size_t(kResolution) * size_t(kResolution), {});

    m_Ready = true;
    Logger::Log::Info("OceanHeightField: ready ({}x{} tile {}m, {} m/texel, ring {})", kResolution,
                      kResolution, kTileMeters, m_TexelSize, m_Readback.Count());
    return true;
}

void OceanHeightField::BeginFrame(float cameraX, float cameraZ)
{
    // Snap the tile's min corner to the texel grid so the field is stable
    // frame-to-frame (no shimmer as the camera drifts sub-texel). The tile is
    // centered on the camera.
    const float halfTile = kTileMeters * 0.5f;
    m_OriginX = std::floor((cameraX - halfTile) / m_TexelSize) * m_TexelSize;
    m_OriginZ = std::floor((cameraZ - halfTile) / m_TexelSize) * m_TexelSize;
}

void OceanHeightField::SetFrameInputs(TextureHandle fftDisplacement, SamplerHandle fftSampler,
                                      uint32 fftCascadeCount, float seaLevel, float weight,
                                      float maxHorizontal, float maxVertical,
                                      float waveOriginOffsetX, float waveOriginOffsetZ)
{
    m_FFTDisplacement = fftDisplacement;
    m_FFTSampler = fftSampler;
    m_FFTCascadeCount = fftCascadeCount;
    m_SeaLevel = seaLevel;
    m_Weight = weight;
    m_MaxHorizontal = maxHorizontal;
    m_MaxVertical = maxVertical;
    m_WaveOriginOffsetX = waveOriginOffsetX;
    m_WaveOriginOffsetZ = waveOriginOffsetZ;
}

void OceanHeightField::OnOriginShift(float32 shiftX, float32 shiftZ, bool invalidate)
{
    m_OriginX -= shiftX;
    m_OriginZ -= shiftZ;
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_ReadOriginX -= shiftX;
    m_ReadOriginZ -= shiftZ;
    // Payload metadata in pending GPU slots still uses the pre-shift coordinate
    // frame and cannot be rewritten safely. Drop it; a fresh non-blocking bake
    // repopulates the cache while collision falls through to baked/Gerstner.
    m_Readback.DropPendings();
    if (invalidate)
    {
        m_HasData = false;
    }
}

void OceanHeightField::DiscardCpuData()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Readback.DropPendings();
    m_HasData = false;
}

void OceanHeightField::FillParams(OceanHeightQueryParamsGPU& out) const
{
    OceanHeightQueryParamsGPU p{};
    p.Resolution = kResolution;
    p.FFTCascadeCount = m_FFTCascadeCount;
    p.OriginX = m_OriginX;
    p.OriginZ = m_OriginZ;
    p.TexelSize = m_TexelSize;
    p.SeaLevel = m_SeaLevel;
    p.Weight = m_Weight;
    p.MaxHorizontal = m_MaxHorizontal;
    p.MaxVertical = m_MaxVertical;
    p.WaveOriginOffsetX = m_WaveOriginOffsetX;
    p.WaveOriginOffsetZ = m_WaveOriginOffsetZ;
    out = p;
}

void OceanHeightField::RecordDispatch(IDevice* device, CommandList* cl,
                                      const RenderGraph::RGFrame& frame,
                                      BufferHandle paramsBuffer, uint64 paramsOffset)
{
    if (!m_Ready || !cl || !device || !m_Readback.IsInitialized())
        return;
    if (!m_FFTDisplacement.IsValid() || !m_FFTSampler.IsValid())
        return;
    if (!paramsBuffer.IsValid())
        return;

    // Resolve the pipeline before BeginWrite: a pending must only register on
    // frames that actually record a dispatch.
    PipelineHandle pipe = device->GetOrCreateComputePipeline(m_Pipe);
    if (!pipe)
        return;

    ReadbackMeta meta{};
    meta.OriginX = m_OriginX;
    meta.OriginZ = m_OriginZ;
    const BufferHandle outBuf = m_Readback.BeginWrite(frame, meta);
    if (!outBuf.IsValid())
        return;

    cl->SetPipeline(pipe);

    DescriptorSetDesc dsDesc{};
    dsDesc.layout = m_Layout;
    dsDesc.transient = true;
    dsDesc.debugName = "OceanHeightQuery_DS";
    DescriptorSetHandle ds = device->CreateDescriptorSet(dsDesc);

    device->UpdateBufferBinding(ds, 0, paramsBuffer, paramsOffset, sizeof(OceanHeightQueryParamsGPU));
    device->UpdateCombinedImageSamplerBinding(ds, 1, m_FFTDisplacement, m_FFTSampler);
    device->UpdateStorageBufferBinding(ds, 2, outBuf, 0,
                                       sizeof(DisplacementSample) *
                                           size_t(kResolution) * size_t(kResolution));

    cl->BindDescriptorSet(0, ds, pipe);

    const uint32 g8 = (kResolution + 7u) / 8u;
    cl->Dispatch(g8, g8, 1);

    // No GPU-side barrier: nothing else reads this buffer in-frame, and a slot
    // only becomes readable once its frame's submission token has signaled. The
    // signaled timeline wait plus MapBuffer's invalidate make the compute write
    // visible to the CPU read below — a host-cached type need not be coherent,
    // which is why the invalidate is load-bearing here (the same
    // fence-not-barrier readback contract the SDSM depth-bounds path relies on).

    // Map the newest token-signaled slot into the CPU cache — null until the
    // first stamped write completes, so we never read a slot mid-flight.
    ReadbackMeta readMeta{};
    if (const void* mapped = m_Readback.MapNewestReady(device, &readMeta))
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            std::memcpy(m_CPUDisplacements.data(), mapped,
                        sizeof(DisplacementSample) * size_t(kResolution) * size_t(kResolution));
            m_ReadOriginX = readMeta.OriginX;
            m_ReadOriginZ = readMeta.OriginZ;
            m_HasData = true;
        }
        m_Readback.Unmap(device);
        // This thread is the cache's only writer, so the scan reads it without
        // holding the lock the buoyancy queries wait on.
        float maxVertical = 0.0f;
        for (const DisplacementSample& s : m_CPUDisplacements)
            maxVertical = std::max(maxVertical, std::abs(s.Dy));
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_RecentMaxVertical.Add(maxVertical, OceanRecentMaximum::Clock::now());
    }
}

bool OceanHeightField::GetRecentMaxVerticalDisplacement(float& outMeters) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_RecentMaxVertical.Get(OceanRecentMaximum::Clock::now(), outMeters);
}

bool OceanHeightField::Sample(float worldX, float worldZ, float& outHeight) const
{
    float dx = 0.0f;
    float dy = 0.0f;
    float dz = 0.0f;
    return SampleDisplacement(worldX, worldZ, dx, dy, dz, outHeight);
}

bool OceanHeightField::SampleDisplacement(float worldX, float worldZ,
                                          float& outDx, float& outDy, float& outDz,
                                          float& outHeight) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_HasData)
        return false;

    // Fractional texel coordinate (integer = texel center).
    const float gx = (worldX - m_ReadOriginX) / m_TexelSize - 0.5f;
    const float gz = (worldZ - m_ReadOriginZ) / m_TexelSize - 0.5f;
    const float fi = std::floor(gx);
    const float fj = std::floor(gz);
    const int i0 = static_cast<int>(fi);
    const int j0 = static_cast<int>(fj);
    // Need i0,i0+1 and j0,j0+1 in range for the bilinear footprint.
    if (i0 < 0 || j0 < 0 || i0 >= static_cast<int>(kResolution) - 1 ||
        j0 >= static_cast<int>(kResolution) - 1)
        return false;

    const float tx = gx - fi;
    const float tz = gz - fj;
    const uint32 n = kResolution;
    const DisplacementSample s00 = m_CPUDisplacements[size_t(j0) * n + i0];
    const DisplacementSample s10 = m_CPUDisplacements[size_t(j0) * n + (i0 + 1)];
    const DisplacementSample s01 = m_CPUDisplacements[size_t(j0 + 1) * n + i0];
    const DisplacementSample s11 = m_CPUDisplacements[size_t(j0 + 1) * n + (i0 + 1)];
    auto bilerp = [&](float a00, float a10, float a01, float a11) {
        const float a0 = a00 + (a10 - a00) * tx;
        const float a1 = a01 + (a11 - a01) * tx;
        return a0 + (a1 - a0) * tz;
    };
    outDx = bilerp(s00.Dx, s10.Dx, s01.Dx, s11.Dx);
    outDy = bilerp(s00.Dy, s10.Dy, s01.Dy, s11.Dy);
    outDz = bilerp(s00.Dz, s10.Dz, s01.Dz, s11.Dz);
    outHeight = bilerp(s00.Height, s10.Height, s01.Height, s11.Height);
    return true;
}

} // namespace GameEngine::Ocean
