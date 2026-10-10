#include "Ocean/OceanAlbedoSim.h"
#include "Ocean/OceanShaderProgram.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "OceanShaderDirectory.h"

#include "Logger/Logger.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/ShaderCompileService.h"

#include <algorithm>
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

bool OceanAlbedoSim::Initialize(IDevice* device)
{
    if (m_Ready)
        return true;
    if (!device)
        return false;
    m_Device = device;

    // RGBA8_UNORM albedo cascade (surface paint). Single persistent array (the bake
    // is stateless, no ping-pong).
    if (!m_Albedo.Initialize(device, kAlbedoResolution, kAlbedoLodCount, TextureFormat::RGBA8_UNORM,
                             kAlbedoBaseScale, "Ocean_Albedo"))
    {
        Logger::Log::Warning("OceanAlbedoSim: albedo cascade allocation failed");
        return false;
    }
    m_Resolution = m_Albedo.GetResolution();
    m_LodCount = m_Albedo.GetLodCount();

    m_Sampler = device->CreateSampler(SamplerDesc::MaterialLinearClamp("Ocean_Albedo_Sampler"));
    if (!m_Sampler.IsValid())
        return false;

    const fs::path shaderDir = OceanShaderDirectory("ocean_albedo_sim.comp");
    if (shaderDir.empty())
    {
        Logger::Log::Error("OceanAlbedoSim: ocean_albedo_sim.comp not found");
        return false;
    }
    const fs::path includeDir = shaderDir.parent_path(); // Assets/Shaders, for "Ocean/..." includes
    const fs::path cacheRoot = fs::path(".Cache") / "Shaders";

    ShaderProgramCompileRequest req{};
    req.debugName = "ocean_albedo_sim";
    req.baseDirectory = shaderDir;
    req.cacheRoot = cacheRoot;
    req.includeDirs = {includeDir};
    req.stages = {{"cs", "ocean_albedo_sim.comp", "main", {}}};

    ShaderProgramCompileResult result{};
    std::string err;
    if (!LoadOceanShaderProgram(req, device->PreferredShaderSource(), result, &err))
    {
        Logger::Log::Error("OceanAlbedoSim: compile failed: {}", err);
        return false;
    }
    auto it = result.stageBytes.find("cs");
    if (it == result.stageBytes.end() || it->second.empty())
    {
        Logger::Log::Error("OceanAlbedoSim: no SPIR-V");
        return false;
    }
    auto shaderBytes = std::make_shared<const std::vector<uint8_t>>(std::move(it->second));

    // Layout: UBO(0), output-albedo storage image(1).
    m_Layout = DescriptorSetLayoutDesc{};
    m_Layout.debugName = "OceanAlbedo_Set0";
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

    ComputePipelineDesc cd{};
    cd.ComputeShader = shaderBytes;
    Rendering::ApplyMetaImageShapeToLayout(result.meta, m_Layout);
    cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
    cd.DebugName = "OceanAlbedo";
    m_Pipe = m_Device->InternComputePipeline(cd);
    if (!m_Pipe.IsValid())
        return false;

    m_Ready = true;
    Logger::Log::Info("OceanAlbedoSim: ready ({}x{} x{} LODs)", m_Resolution, m_Resolution, m_LodCount);
    return true;
}

bool OceanAlbedoSim::BeginFrame(float cameraX, float cameraZ)
{
    if (m_Albedo.SnapToCamera(cameraX, cameraZ))
        m_DispatchDirty = true;
    return m_DispatchDirty;
}

void OceanAlbedoSim::SetSources(const OceanAlbedoSourceGPU* sources, uint32 count)
{
    const uint32 n = sources ? std::min(count, kMaxOceanAlbedoSources) : 0u;
    const bool changed =
        m_Params.SourceCount != n ||
        (n > 0 && std::memcmp(m_Params.Sources, sources,
                              static_cast<size_t>(n) * sizeof(OceanAlbedoSourceGPU)) != 0);
    m_Params.SourceCount = n;
    if (sources && n > 0)
        std::memcpy(m_Params.Sources, sources, static_cast<size_t>(n) * sizeof(OceanAlbedoSourceGPU));
    if (changed)
        m_DispatchDirty = true;
}

void OceanAlbedoSim::FillParams(OceanAlbedoParamsGPU& out)
{
    m_Params.Resolution = m_Resolution;
    m_Params.LodCount = m_LodCount;
    // The layout BeginFrame snapped drives the dispatch; it is fixed before the
    // pass is declared, so filling at declare time is safe.
    m_Params.Cascade = m_Albedo.GetLayout();
    out = m_Params;
}

void OceanAlbedoSim::RecordDispatch(IDevice* device, CommandList* cl,
                                    BufferHandle paramsBuffer, uint64 paramsOffset)
{
    if (!m_Ready || !cl || !device || !paramsBuffer.IsValid())
        return;

    const TextureHandle albedo = m_Albedo.GetTexture();
    const uint32 layers = m_LodCount;

    PipelineHandle pipe = device->GetOrCreateComputePipeline(m_Pipe);
    if (!pipe)
        return;

    // Sync is graph-derived: the pass declares Write on the cascade, the first
    // declared reader restores ShaderReadOnly, and MarkOutput covers reader-less
    // frames. No manual barriers in here.

    cl->SetPipeline(pipe);

    DescriptorSetDesc dsDesc{};
    dsDesc.layout = m_Layout;
    dsDesc.transient = true;
    dsDesc.debugName = "OceanAlbedo_DS";
    DescriptorSetHandle ds = device->CreateDescriptorSet(dsDesc);

    device->UpdateBufferBinding(ds, 0, paramsBuffer, paramsOffset, sizeof(OceanAlbedoParamsGPU));
    device->UpdateStorageImageBinding(ds, 1, albedo);

    cl->BindDescriptorSet(0, ds, pipe);

    const uint32 g8 = (m_Resolution + 7u) / 8u;
    cl->Dispatch(g8, g8, layers);

    m_DispatchDirty = false;
}

RenderGraph::RGTexture OceanAlbedoSim::ImportRG(RenderGraph::RGFrame& frame) const
{
    return m_Albedo.ImportRG(frame);
}

} // namespace GameEngine::Ocean
