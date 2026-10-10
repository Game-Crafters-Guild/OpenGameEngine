#include "Ocean/OceanWaveMaskSim.h"
#include "Ocean/OceanShaderProgram.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "OceanShaderDirectory.h"

#include "Logger/Logger.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Utils/TextureUploadHelpers.h"

#include <algorithm>
#include <cstdint>
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

TextureHandle CreateNeutralWaveMaskTexture(IDevice* device)
{
    if (!device)
        return {};

    TextureDesc td{};
    td.width = 1;
    td.height = 1;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.format = static_cast<uint32>(TextureFormat::R16G16_FLOAT);
    td.usage = static_cast<uint32>(TextureUsage::ShaderResource) |
               static_cast<uint32>(TextureUsage::TransferDst);
    td.sampleCount = 1;
    td.initialState = ResourceState::Undefined;
    td.debugName = "Ocean_WaveMaskMap_Neutral";

    TextureHandle texture = device->CreateTexture(td);
    if (!texture.IsValid())
        return {};

    const uint16_t neutralRgHalf[2] = {0x3C00u, 0x3C00u}; // half-float 1.0, 1.0
    UploadTexture2D(device, texture, neutralRgHalf, 1, 1, sizeof(neutralRgHalf),
                    "Ocean_WaveMaskMap_Neutral_Upload");
    return texture;
}

} // anonymous namespace

bool OceanWaveMaskSim::Initialize(IDevice* device)
{
    if (m_Ready)
        return true;
    if (!device)
        return false;
    m_Device = device;

    if (!m_Mask.Initialize(device, kResolution, kLodCount, TextureFormat::R16G16B16A16_FLOAT,
                           kBaseScale, "Ocean_WaveMask"))
    {
        Logger::Log::Warning("OceanWaveMaskSim: mask cascade allocation failed");
        return false;
    }
    for (uint32 page = 0u; page < kOceanLocalFFTMaskPages; ++page)
    {
        const char* debugName = (page == 0u) ? "Ocean_LocalFFTMask0" : "Ocean_LocalFFTMask1";
        if (!m_LocalFFTMasks[page].Initialize(
                device, kResolution, kLodCount, TextureFormat::R16G16B16A16_FLOAT, kBaseScale,
                debugName))
        {
            Logger::Log::Warning("OceanWaveMaskSim: local FFT mask page allocation failed");
            return false;
        }
    }
    m_Resolution = m_Mask.GetResolution();
    m_LodCount = m_Mask.GetLodCount();

    m_Sampler = device->CreateSampler(SamplerDesc::MaterialLinearClamp("Ocean_WaveMask_Sampler"));
    if (!m_Sampler.IsValid())
        return false;
    m_DummyWaveMaskTexture = CreateNeutralWaveMaskTexture(device);
    if (!m_DummyWaveMaskTexture.IsValid())
        return false;

    const fs::path shaderDir = OceanShaderDirectory("ocean_wavemask_sim.comp");
    if (shaderDir.empty())
    {
        Logger::Log::Error("OceanWaveMaskSim: ocean_wavemask_sim.comp not found");
        return false;
    }
    const fs::path includeDir = shaderDir.parent_path();
    const fs::path cacheRoot = fs::path(".Cache") / "Shaders";

    ShaderProgramCompileRequest req{};
    req.debugName = "ocean_wavemask_sim";
    req.baseDirectory = shaderDir;
    req.cacheRoot = cacheRoot;
    req.includeDirs = {includeDir};
    req.stages = {{"cs", "ocean_wavemask_sim.comp", "main", {}}};

    ShaderProgramCompileResult result{};
    std::string err;
    if (!LoadOceanShaderProgram(req, device->PreferredShaderSource(), result, &err))
    {
        Logger::Log::Error("OceanWaveMaskSim: compile failed: {}", err);
        return false;
    }
    auto it = result.stageBytes.find("cs");
    if (it == result.stageBytes.end() || it->second.empty())
    {
        Logger::Log::Error("OceanWaveMaskSim: no SPIR-V");
        return false;
    }
    auto shaderBytes = std::make_shared<const std::vector<uint8_t>>(std::move(it->second));

    m_Layout = DescriptorSetLayoutDesc{};
    m_Layout.debugName = "OceanWaveMask_Set0";
    auto addBinding = [&](uint32 binding, DescriptorType type) {
        DescriptorBinding b{};
        b.binding = binding;
        b.type = type;
        b.count = 1;
        b.shaderStages = kShaderStageCompute;
        m_Layout.bindings.push_back(b);
    };
    addBinding(0, DescriptorType::UniformBuffer);
    addBinding(1, DescriptorType::StorageImage);
    for (uint32 page = 0u; page < kOceanLocalFFTMaskPages; ++page)
        addBinding(2u + page, DescriptorType::StorageImage);
    const uint32 firstTextureBinding = 2u + kOceanLocalFFTMaskPages;
    for (uint32 i = 0u; i < kMaxOceanWaveMaskTextureSources; ++i)
        addBinding(firstTextureBinding + i, DescriptorType::CombinedImageSampler);

    ComputePipelineDesc cd{};
    cd.ComputeShader = shaderBytes;
    Rendering::ApplyMetaImageShapeToLayout(result.meta, m_Layout);
    cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
    cd.DebugName = "OceanWaveMask";
    m_Pipe = m_Device->InternComputePipeline(cd);
    if (!m_Pipe.IsValid())
        return false;

    m_Ready = true;
    Logger::Log::Info("OceanWaveMaskSim: ready ({}x{} x{} LODs)", m_Resolution, m_Resolution,
                      m_LodCount);
    return true;
}

void OceanWaveMaskSim::BeginFrame(float cameraX, float cameraZ)
{
    m_Mask.SnapToCamera(cameraX, cameraZ);
    for (auto& localMask : m_LocalFFTMasks)
        localMask.SnapToCamera(cameraX, cameraZ);
}

void OceanWaveMaskSim::SetSources(const OceanWaveMaskSourceGPU* sources, uint32 count)
{
    const uint32 n = std::min(count, kMaxOceanWaveMaskSources);
    m_Params.SourceCount = n;
    if (sources && n > 0)
        std::memcpy(m_Params.Sources, sources,
                    static_cast<size_t>(n) * sizeof(OceanWaveMaskSourceGPU));
}

void OceanWaveMaskSim::SetPolygonSources(const OceanWaveMaskPolygonGPU* polygons, uint32 count)
{
    const uint32 n = std::min(count, kMaxOceanWaveMaskPolygons);
    m_Params.PolygonCount = n;
    if (polygons && n > 0)
        std::memcpy(m_Params.Polygons, polygons,
                    static_cast<size_t>(n) * sizeof(OceanWaveMaskPolygonGPU));
}

void OceanWaveMaskSim::SetTextureSources(const OceanWaveMaskTextureSourceGPU* sources,
                                         const TextureHandle* textures,
                                         uint32 count)
{
    const uint32 n =
        (sources && textures) ? std::min(count, kMaxOceanWaveMaskTextureSources) : 0u;
    m_Params.TextureSourceCount = n;
    m_WaveMaskTextures.fill({});

    if (n == 0u)
        return;

    std::memcpy(m_Params.TextureSources, sources,
                static_cast<size_t>(n) * sizeof(OceanWaveMaskTextureSourceGPU));
    for (uint32 i = 0u; i < n; ++i)
        m_WaveMaskTextures[i] = textures[i];
}

void OceanWaveMaskSim::FillParams(OceanWaveMaskParamsGPU& out)
{
    m_Params.Resolution = m_Resolution;
    m_Params.LodCount = m_LodCount;
    m_Params.Cascade = m_Mask.GetLayout();
    out = m_Params;
}

void OceanWaveMaskSim::RecordDispatch(IDevice* device, CommandList* cl,
                                      BufferHandle paramsBuffer, uint64 paramsOffset)
{
    if (!m_Ready || !cl || !device || !paramsBuffer.IsValid())
        return;

    const TextureHandle mask = m_Mask.GetTexture();
    const uint32 layers = m_LodCount;

    PipelineHandle pipe = device->GetOrCreateComputePipeline(m_Pipe);
    if (!pipe)
        return;

    // Sync is graph-derived: the pass declares Write on the mask + both local
    // pages, the first declared reader restores ShaderReadOnly, and MarkOutput
    // covers reader-less frames. No manual barriers in here.
    cl->SetPipeline(pipe);

    DescriptorSetDesc dsDesc{};
    dsDesc.layout = m_Layout;
    dsDesc.transient = true;
    dsDesc.debugName = "OceanWaveMask_DS";
    DescriptorSetHandle ds = device->CreateDescriptorSet(dsDesc);

    device->UpdateBufferBinding(ds, 0, paramsBuffer, paramsOffset, sizeof(OceanWaveMaskParamsGPU));
    device->UpdateStorageImageBinding(ds, 1, mask);
    for (uint32 page = 0u; page < kOceanLocalFFTMaskPages; ++page)
        device->UpdateStorageImageBinding(ds, 2u + page, m_LocalFFTMasks[page].GetTexture());
    const uint32 firstTextureBinding = 2u + kOceanLocalFFTMaskPages;
    for (uint32 i = 0u; i < kMaxOceanWaveMaskTextureSources; ++i)
    {
        TextureHandle maskTexture =
            m_WaveMaskTextures[i].IsValid() ? m_WaveMaskTextures[i] : m_DummyWaveMaskTexture;
        device->UpdateCombinedImageSamplerBinding(ds, firstTextureBinding + i, maskTexture,
                                                  m_Sampler);
    }

    cl->BindDescriptorSet(0, ds, pipe);

    const uint32 g8 = (m_Resolution + 7u) / 8u;
    cl->Dispatch(g8, g8, layers);
}

RenderGraph::RGTexture OceanWaveMaskSim::ImportMaskRG(RenderGraph::RGFrame& frame) const
{
    return m_Mask.ImportRG(frame);
}

RenderGraph::RGTexture OceanWaveMaskSim::ImportLocalFFTMaskRG(RenderGraph::RGFrame& frame,
                                                              uint32 page) const
{
    return (page < kOceanLocalFFTMaskPages) ? m_LocalFFTMasks[page].ImportRG(frame)
                                            : RenderGraph::RGTexture{};
}

} // namespace GameEngine::Ocean
