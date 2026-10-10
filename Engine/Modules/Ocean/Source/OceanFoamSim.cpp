#include "Ocean/OceanFoamSim.h"
#include "Ocean/OceanInputDrawSource.h"
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
#include <cmath>
#include <filesystem>
#include <memory>
#include <vector>

namespace GameEngine::Ocean
{

namespace fs = std::filesystem;
using namespace ::GameEngine::Rendering;

namespace
{
// One step of length `seconds` deposits rate * seconds; the exponential decay
// integrated over the same time deposits rate * (1 - e^(-k t)) / k. This is the
// ratio, so a catch-up step's deposit matches the continuous simulation.
float32 DecayedDepositScale(float32 fadeRate, float32 seconds)
{
    const float32 decayExponent = fadeRate * seconds;
    if (decayExponent < 1e-4f)
        return 1.0f;
    return (1.0f - std::exp(-decayExponent)) / decayExponent;
}
} // anonymous namespace

bool OceanFoamSim::Initialize(IDevice* device)
{
    if (m_Ready)
        return true;
    if (!device)
        return false;
    m_Device = device;

    // RGBA16F foam cascade — ping-pong pair.
    // R = wave/breaking foam, G = shoreline/contact foam, B = freshness/age,
    // A = latest deposit/debug intensity.
    if (!m_Foam[0].Initialize(device, kFoamResolution, kFoamLodCount, TextureFormat::R16G16B16A16_FLOAT,
                              kFoamBaseScale, "Ocean_Foam_A") ||
        !m_Foam[1].Initialize(device, kFoamResolution, kFoamLodCount, TextureFormat::R16G16B16A16_FLOAT,
                              kFoamBaseScale, "Ocean_Foam_B"))
    {
        Logger::Log::Warning("OceanFoamSim: foam cascade allocation failed");
        return false;
    }
    m_Resolution = m_Foam[0].GetResolution();
    m_LodCount = m_Foam[0].GetLodCount();

    m_Sampler = device->CreateSampler(SamplerDesc::MaterialLinearClamp("Ocean_Foam_Sampler"));
    if (!m_Sampler.IsValid())
        return false;

    const fs::path shaderDir = OceanShaderDirectory("ocean_foam_sim.comp");
    if (shaderDir.empty())
    {
        Logger::Log::Error("OceanFoamSim: ocean_foam_sim.comp not found");
        return false;
    }
    const fs::path includeDir = shaderDir.parent_path(); // Assets/Shaders, for "Ocean/..." includes
    const fs::path cacheRoot = fs::path(".Cache") / "Shaders";

    ShaderProgramCompileRequest req{};
    req.debugName = "ocean_foam_sim";
    req.baseDirectory = shaderDir;
    req.cacheRoot = cacheRoot;
    req.includeDirs = {includeDir};
    req.stages = {{"cs", "ocean_foam_sim.comp", "main", {}}};

    ShaderProgramCompileResult result{};
    std::string err;
    if (!LoadOceanShaderProgram(req, device->PreferredShaderSource(), result, &err))
    {
        Logger::Log::Error("OceanFoamSim: compile failed: {}", err);
        return false;
    }
    auto it = result.stageBytes.find("cs");
    if (it == result.stageBytes.end() || it->second.empty())
    {
        Logger::Log::Error("OceanFoamSim: no SPIR-V");
        return false;
    }
    auto shaderBytes = std::make_shared<const std::vector<uint8_t>>(std::move(it->second));

    // Layout: UBO(0), prev-foam sampler(1), FFT-displacement sampler(2),
    // output-foam storage image(3), seabed-depth sampler(4, shoreline term),
    // flow sampler(5, advection), combined displacement sampler(6).
    m_Layout = DescriptorSetLayoutDesc{};
    m_Layout.debugName = "OceanFoam_Set0";
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
    addBinding(2, DescriptorType::CombinedImageSampler);
    addBinding(3, DescriptorType::StorageImage);
    addBinding(4, DescriptorType::CombinedImageSampler);
    addBinding(5, DescriptorType::CombinedImageSampler);
    addBinding(6, DescriptorType::CombinedImageSampler);

    ComputePipelineDesc cd{};
    cd.ComputeShader = shaderBytes;
    Rendering::ApplyMetaImageShapeToLayout(result.meta, m_Layout);
    cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
    cd.DebugName = "OceanFoam";
    m_Pipe = m_Device->InternComputePipeline(cd);
    if (!m_Pipe.IsValid())
        return false;

    m_Ready = true;
    Logger::Log::Info("OceanFoamSim: ready ({}x{} x{} LODs)", m_Resolution, m_Resolution, m_LodCount);
    return true;
}

TextureHandle OceanFoamSim::GetFoamTexture() const
{
    return m_Foam[m_WriteIndex].GetTexture();
}

const OceanCascadeLayoutGPU& OceanFoamSim::GetFoamLayout() const
{
    return m_Foam[m_WriteIndex].GetLayout();
}

RenderGraph::RGTexture OceanFoamSim::ImportWriteRG(RenderGraph::RGFrame& frame) const
{
    return m_Foam[m_WriteIndex].ImportRG(frame);
}

RenderGraph::RGTexture OceanFoamSim::ImportPrevRG(RenderGraph::RGFrame& frame) const
{
    return m_Foam[m_WriteIndex ^ 1u].ImportRG(frame);
}

RenderGraph::RGTexture OceanFoamSim::ImportWriteRG(RenderGraph::RGFrame& frame,
                                                   uint32 writeIndex) const
{
    return m_Foam[writeIndex & 1u].ImportRG(frame);
}

RenderGraph::RGTexture OceanFoamSim::ImportPrevRG(RenderGraph::RGFrame& frame,
                                                  uint32 writeIndex) const
{
    return m_Foam[(writeIndex & 1u) ^ 1u].ImportRG(frame);
}

void OceanFoamSim::BeginFrame(float cameraX, float cameraZ)
{
    // Flip first so the write target is fixed for the whole frame (binding +
    // dispatch agree); the previous write becomes this frame's advection read.
    m_WriteIndex ^= 1u;
    m_Foam[m_WriteIndex].SnapToCamera(cameraX, cameraZ);
}

uint32 OceanFoamSim::BeginSubstep(float cameraX, float cameraZ)
{
    BeginFrame(cameraX, cameraZ);
    return m_WriteIndex;
}

void OceanFoamSim::ConfigureSimulation(float32 frequency, uint32 maximumSubsteps)
{
    m_UseFixedStep = true;
    m_SimulationFrequency = std::max(frequency, 1.0f);
    m_MaximumSubsteps = std::clamp(maximumSubsteps, 1u, 16u);
}

void OceanFoamSim::ClearSimulationConfiguration()
{
    m_UseFixedStep = false;
    m_TimeAccumulator = 0.0f;
}

void OceanFoamSim::SetFrameInputs(TextureHandle fftDisplacement, SamplerHandle fftSampler,
                                  uint32 fftCascadeCount, float fadeRate, float coverage,
                                  float strength, float deltaTime,
                                  float shapeWeight, float shapeMaxHorizontalDisplacement,
                                  float shapeMaxVerticalDisplacement,
                                  float shapeRespectShallowAttenuation,
                                  float shapeSubSurfaceDepthMax, float windVelocityX,
                                  float windVelocityZ, float waveOriginOffsetX,
                                  float waveOriginOffsetZ)
{
    m_FFTDisplacement = fftDisplacement;
    m_FFTSampler = fftSampler;
    m_Params.FFTCascadeCount = fftCascadeCount;
    m_Params.FadeRate = fadeRate;
    m_Params.Coverage = coverage;
    m_Params.Strength = strength;
    m_SkippedTime += static_cast<float32>(m_PendingSubsteps) * m_SubstepDelta;
    if (m_UseFixedStep)
    {
        m_TimeAccumulator += std::max(deltaTime, 0.0f);
        const float32 fixedDelta = 1.0f / m_SimulationFrequency;
        const uint32 available = static_cast<uint32>(
            std::floor((m_TimeAccumulator + 1e-6f) / fixedDelta));
        m_PendingSubsteps = std::min(available, m_MaximumSubsteps);
        m_SubstepDelta = fixedDelta;
        m_TimeAccumulator = std::max(
            0.0f, m_TimeAccumulator - static_cast<float32>(m_PendingSubsteps) * fixedDelta);
        m_TimeAccumulator = std::min(m_TimeAccumulator,
                                     fixedDelta * static_cast<float32>(m_MaximumSubsteps));
    }
    else
    {
        m_PendingSubsteps = deltaTime > 0.0f ? 1u : 0u;
        m_SubstepDelta = std::max(deltaTime, 0.0f);
    }
    m_Params.DeltaTime = m_SubstepDelta;
    m_Params.ShapeWeight = shapeWeight;
    m_Params.ShapeMaxHorizontalDisplacement = shapeMaxHorizontalDisplacement;
    m_Params.ShapeMaxVerticalDisplacement = shapeMaxVerticalDisplacement;
    m_Params.ShapeRespectShallowAttenuation = shapeRespectShallowAttenuation;
    m_Params.ShapeSubSurfaceDepthMax = shapeSubSurfaceDepthMax;
    m_Params.WindVelocityX = windVelocityX;
    m_Params.WindVelocityZ = windVelocityZ;
    m_Params.WaveOriginOffsetX = waveOriginOffsetX;
    m_Params.WaveOriginOffsetZ = waveOriginOffsetZ;
}

void OceanFoamSim::SetCombinedInputs(TextureHandle combinedDisplacement,
                                     SamplerHandle combinedSampler,
                                     const OceanCascadeLayoutGPU& layout,
                                     bool available)
{
    m_CombinedDisplacement = combinedDisplacement;
    m_CombinedSampler = combinedSampler;
    m_Params.CombineCascade = layout;
    m_Params.CombinedDisplacementAvailable =
        (available && combinedDisplacement.IsValid() && combinedSampler.IsValid()) ? 1u : 0u;
}

void OceanFoamSim::SetAuthoredSources(const OceanFoamInputGPU* sources, uint32 count)
{
    const uint32 sourceCount = sources ? std::min(count, kMaxOceanFoamInputs) : 0u;
    m_Params.AuthoredFoamSourceCount = sourceCount;
    for (uint32 i = 0u; i < kMaxOceanFoamInputs; ++i)
        m_Params.AuthoredFoamSources[i] = i < sourceCount ? sources[i] : OceanFoamInputGPU{};
}

void OceanFoamSim::SetShorelineInputs(TextureHandle seabedDepth, SamplerHandle seabedSampler,
                                      float maxDepth, float strength, bool available)
{
    m_SeabedDepth = seabedDepth;
    m_SeabedSampler = seabedSampler;
    m_Params.ShorelineMaxDepth = maxDepth;
    m_Params.ShorelineStrength = strength;
    m_Params.SeabedDepthAvailable = available ? 1u : 0u;
}

void OceanFoamSim::SetFlowInputs(TextureHandle flow, SamplerHandle flowSampler, float flowScale,
                                 bool available)
{
    m_Flow = flow;
    m_FlowSampler = flowSampler;
    m_Params.FlowScale = flowScale;
    m_Params.FlowAvailable = available ? 1u : 0u;
}

void OceanFoamSim::FillParams(OceanFoamParamsGPU& out)
{
    m_Params.Resolution = m_Resolution;
    m_Params.LodCount = m_LodCount;
    // The write target's layout drives the dispatch. The previous target keeps the
    // layout it was written with, so the shader can reproject history by world XZ
    // instead of by same-UV.
    m_Params.Cascade = m_Foam[m_WriteIndex].GetLayout();
    m_Params.PrevCascade = m_Foam[m_WriteIndex ^ 1u].GetLayout();
    out = m_Params;
}

void OceanFoamSim::FillParams(OceanFoamParamsGPU& out, uint32 writeIndex,
                              uint32 substepIndex) const
{
    out = m_Params;
    out.Resolution = m_Resolution;
    out.LodCount = m_LodCount;
    out.DeltaTime = m_SubstepDelta;
    out.ResetHistory = m_ResetHistory && substepIndex == 0u ? 1u : 0u;
    // Catch-up: the first step after skipped frames covers them too. The shader
    // decays by exp(-k dt), exact over any dt; its deposits are linear in dt, so
    // they are scaled to the exponential integral over the whole step.
    if (substepIndex == 0u && m_SkippedTime > 0.0f && !m_ResetHistory)
    {
        out.DeltaTime += m_SkippedTime;
        const float32 waveScale = DecayedDepositScale(m_Params.FadeRate, out.DeltaTime);
        out.Strength *= waveScale;
        out.ShorelineStrength *=
            DecayedDepositScale(m_Params.FadeRate * m_Params.ShorelineFadeRateScale, out.DeltaTime);
        for (uint32 i = 0u; i < out.AuthoredFoamSourceCount; ++i)
        {
            float32* amountFeatherBlend = out.AuthoredFoamSources[i].AmountFeatherBlend;
            if (std::lround(amountFeatherBlend[2]) == static_cast<long>(OceanInputBlendMode::Additive))
                amountFeatherBlend[0] *= waveScale;
        }
    }
    out.Cascade = m_Foam[writeIndex & 1u].GetLayout();
    out.PrevCascade = m_Foam[(writeIndex & 1u) ^ 1u].GetLayout();
}

void OceanFoamSim::RecordDispatch(IDevice* device, CommandList* cl,
                                  BufferHandle paramsBuffer, uint64 paramsOffset)
{
    RecordDispatch(device, cl, paramsBuffer, paramsOffset, m_WriteIndex);
}

void OceanFoamSim::RecordDispatch(IDevice* device, CommandList* cl,
                                  BufferHandle paramsBuffer, uint64 paramsOffset,
                                  uint32 writeIndex)
{
    if (!m_Ready || !cl || !device || !paramsBuffer.IsValid())
        return;
    if (!m_FFTDisplacement.IsValid() || !m_FFTSampler.IsValid())
        return;

    writeIndex &= 1u;
    const uint32 readIndex = writeIndex ^ 1u;
    const TextureHandle prevFoam = m_Foam[readIndex].GetTexture();
    const TextureHandle nextFoam = m_Foam[writeIndex].GetTexture();
    const uint32 layers = m_LodCount;

    PipelineHandle pipe = device->GetOrCreateComputePipeline(m_Pipe);
    if (!pipe)
        return;

    // Sync is graph-derived: the pass declares Read(prev/displacement/seabed/
    // flow) + Write(next), the first declared reader restores ShaderReadOnly,
    // and MarkOutput covers reader-less frames. On the very first dispatch both
    // arrays were created in ShaderResource (initialState) and the read target
    // samples zeroed (no-foam) contents — no clear needed.
    cl->SetPipeline(pipe);

    DescriptorSetDesc dsDesc{};
    dsDesc.layout = m_Layout;
    dsDesc.transient = true;
    dsDesc.debugName = "OceanFoam_DS";
    DescriptorSetHandle ds = device->CreateDescriptorSet(dsDesc);

    device->UpdateBufferBinding(ds, 0, paramsBuffer, paramsOffset, sizeof(OceanFoamParamsGPU));
    device->UpdateCombinedImageSamplerBinding(ds, 1, prevFoam, m_Sampler);
    device->UpdateCombinedImageSamplerBinding(ds, 2, m_FFTDisplacement, m_FFTSampler);
    device->UpdateStorageImageBinding(ds, 3, nextFoam);
    // Binding 4 (seabed depth) must always have a real sampler2DArray. When no
    // seabed cascade is supplied, bind the prev-foam array (same dimensionality)
    // as a harmless stand-in — SeabedDepthAvailable is 0, so it is never sampled.
    const TextureHandle seabedTex = m_SeabedDepth.IsValid() ? m_SeabedDepth : prevFoam;
    const SamplerHandle seabedSamp = m_SeabedSampler.IsValid() ? m_SeabedSampler : m_Sampler;
    device->UpdateCombinedImageSamplerBinding(ds, 4, seabedTex, seabedSamp);
    // Binding 5 (flow) must always have a real sampler2DArray. When no flow
    // cascade is supplied, bind the prev-foam array (same dimensionality) as a
    // harmless stand-in — FlowAvailable is 0, so it is never sampled.
    const TextureHandle flowTex = m_Flow.IsValid() ? m_Flow : prevFoam;
    const SamplerHandle flowSamp = m_FlowSampler.IsValid() ? m_FlowSampler : m_Sampler;
    device->UpdateCombinedImageSamplerBinding(ds, 5, flowTex, flowSamp);
    // Binding 6 (combined displacement) is optional; when absent, bind the raw
    // displacement as a valid stand-in and let CombinedDisplacementAvailable
    // keep the shader on the raw FFT path.
    const TextureHandle combinedTex = m_CombinedDisplacement.IsValid()
        ? m_CombinedDisplacement : m_FFTDisplacement;
    const SamplerHandle combinedSamp = m_CombinedSampler.IsValid()
        ? m_CombinedSampler : m_FFTSampler;
    device->UpdateCombinedImageSamplerBinding(ds, 6, combinedTex, combinedSamp);

    cl->BindDescriptorSet(0, ds, pipe);

    const uint32 g8 = (m_Resolution + 7u) / 8u;
    cl->Dispatch(g8, g8, layers);
}

} // namespace GameEngine::Ocean
