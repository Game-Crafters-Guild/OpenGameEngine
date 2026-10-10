#include "Ocean/OceanCombineSim.h"
#include "Ocean/OceanShaderProgram.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "OceanShaderDirectory.h"

#include "Logger/Logger.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/ShaderCompileService.h"

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

bool OceanCombineSim::Initialize(IDevice* device)
{
    if (m_Ready)
        return true;
    if (!device)
        return false;
    m_Device = device;

    // RGBA16F (dispX, height, dispZ) combined displacement cascade — single array
    // (the bake fully overwrites every texel each frame, so no ping-pong).
    if (!m_Combine.Initialize(device, kCombineResolution, kCombineLodCount,
                              TextureFormat::R16G16B16A16_FLOAT, kCombineBaseScale, "Ocean_Combine"))
    {
        Logger::Log::Warning("OceanCombineSim: combine cascade allocation failed");
        return false;
    }
    m_Resolution = m_Combine.GetResolution();
    m_LodCount = m_Combine.GetLodCount();

    m_Sampler = device->CreateSampler(SamplerDesc::MaterialLinearClamp("Ocean_Combine_Sampler"));
    if (!m_Sampler.IsValid())
        return false;

    const fs::path shaderDir = OceanShaderDirectory("ocean_combine.comp");
    if (shaderDir.empty())
    {
        Logger::Log::Error("OceanCombineSim: ocean_combine.comp not found");
        return false;
    }
    const fs::path includeDir = shaderDir.parent_path(); // Assets/Shaders, for "Ocean/..." includes
    const fs::path cacheRoot = fs::path(".Cache") / "Shaders";

    ShaderProgramCompileRequest req{};
    req.debugName = "ocean_combine";
    req.baseDirectory = shaderDir;
    req.cacheRoot = cacheRoot;
    req.includeDirs = {includeDir};
    req.stages = {{"cs", "ocean_combine.comp", "main", {}}};

    ShaderProgramCompileResult result{};
    std::string err;
    if (!LoadOceanShaderProgram(req, device->PreferredShaderSource(), result, &err))
    {
        Logger::Log::Error("OceanCombineSim: compile failed: {}", err);
        return false;
    }
    auto it = result.stageBytes.find("cs");
    if (it == result.stageBytes.end() || it->second.empty())
    {
        Logger::Log::Error("OceanCombineSim: no SPIR-V");
        return false;
    }
    auto shaderBytes = std::make_shared<const std::vector<uint8_t>>(std::move(it->second));

    // Layout: UBO(0), FFT-displacement sampler(1), output combined cascade(2).
    m_Layout = DescriptorSetLayoutDesc{};
    m_Layout.debugName = "OceanCombine_Set0";
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
    addBinding(2, DescriptorType::StorageImage);

    ComputePipelineDesc cd{};
    cd.ComputeShader = shaderBytes;
    Rendering::ApplyMetaImageShapeToLayout(result.meta, m_Layout);
    cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
    cd.DebugName = "OceanCombine";
    m_Pipe = m_Device->InternComputePipeline(cd);
    if (!m_Pipe.IsValid())
        return false;

    m_Ready = true;
    Logger::Log::Info("OceanCombineSim: ready ({}x{} x{} LODs)", m_Resolution, m_Resolution, m_LodCount);
    return true;
}

void OceanCombineSim::BeginFrame(float cameraX, float cameraZ)
{
    m_Combine.SnapToCamera(cameraX, cameraZ);
}

void OceanCombineSim::SetFrameInputs(TextureHandle fftDisplacement, SamplerHandle fftSampler,
                                     uint32 fftCascadeCount, float waveOriginOffsetX,
                                     float waveOriginOffsetZ)
{
    m_FFTDisplacement = fftDisplacement;
    m_FFTSampler = fftSampler;
    m_Params.FFTCascadeCount = fftCascadeCount;
    m_Params.WaveOriginOffsetX = waveOriginOffsetX;
    m_Params.WaveOriginOffsetZ = waveOriginOffsetZ;
}

void OceanCombineSim::FillParams(OceanCombineParamsGPU& out)
{
    m_Params.Resolution = m_Resolution;
    m_Params.LodCount = m_LodCount;
    m_Params.Cascade = m_Combine.GetLayout();
    out = m_Params;
}

void OceanCombineSim::RecordDispatch(IDevice* device, CommandList* cl,
                                     BufferHandle paramsBuffer, uint64 paramsOffset)
{
    if (!m_Ready || !cl || !device || !paramsBuffer.IsValid())
        return;
    if (!m_FFTDisplacement.IsValid() || !m_FFTSampler.IsValid())
        return;

    const TextureHandle combineTex = m_Combine.GetTexture();
    const uint32 layers = m_LodCount;

    PipelineHandle pipe = device->GetOrCreateComputePipeline(m_Pipe);
    if (!pipe)
        return;

    // The combine target moves ShaderResource -> UnorderedAccess for the write,
    // then back to ShaderResource for the world pass. Every texel is overwritten,
    // so there is no read of the previous contents (no clear needed).
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(
        combineTex, ResourceState::ShaderResource, ResourceState::UnorderedAccess, 0, 1, 0, layers));

    cl->SetPipeline(pipe);

    DescriptorSetDesc dsDesc{};
    dsDesc.layout = m_Layout;
    dsDesc.transient = true;
    dsDesc.debugName = "OceanCombine_DS";
    DescriptorSetHandle ds = device->CreateDescriptorSet(dsDesc);

    device->UpdateBufferBinding(ds, 0, paramsBuffer, paramsOffset, sizeof(OceanCombineParamsGPU));
    device->UpdateCombinedImageSamplerBinding(ds, 1, m_FFTDisplacement, m_FFTSampler);
    device->UpdateStorageImageBinding(ds, 2, combineTex);

    cl->BindDescriptorSet(0, ds, pipe);

    const uint32 g8 = (m_Resolution + 7u) / 8u;
    cl->Dispatch(g8, g8, layers);

    cl->Barrier(ResourceBarrier::CreateTextureBarrier(
        combineTex, ResourceState::UnorderedAccess, ResourceState::ShaderResource, 0, 1, 0, layers));
}

RenderGraph::RGTexture OceanCombineSim::ImportRG(RenderGraph::RGFrame& frame) const
{
    return m_Combine.ImportRG(frame);
}

} // namespace GameEngine::Ocean
