#include "Ocean/OceanLightShafts.h"
#include "Ocean/OceanShaderProgram.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "Ocean/OceanTypes.h"
#include "OceanShaderDirectory.h"

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineDescTranslator.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGFullscreen.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Utils/TextureUploadHelpers.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>

namespace GameEngine::Ocean
{

namespace fs = std::filesystem;
using namespace ::GameEngine::Rendering;

namespace
{
constexpr uint32_t kLightShaftsFragmentStage = kShaderStageFragment;

// std140 mirror of OceanReflectedCausticsParams in ocean_lightshafts.frag.
struct alignas(16) OceanReflectedCausticsParamsGPU
{
    float InvViewProj[16]{};
    float CameraPos[4]{};
    float Time[4]{};      // x time
    float SunDirection[4]{}; // xyz toward sun
    float Ocean[4]{};        // x seaLevel, y causticsAvailable, z surface displacement bound, w minimum reflected rise
    float Caustics[4]{};     // x scale, y average, z strength, w pad
    float ReflectedCaustics[4]{}; // x strength, y height, z falloff, w pad
    float FFTParams[4]{};    // x legacy waveMode, y fftCascadeCount, z shapeWeight, w maxVertical
    float LocalFFT[4]{};     // x = ready local FFT stream bit mask, yzw pad
    float LocalFFTMaskCascadeOriginScale[kMaxOceanLodCascades * 4]{};
    float LocalFFTMaskCascadeMeta[4]{}; // x lod count
    float Gerstner[4]{};     // legacy ABI pad
    float Waves[128]{};      // legacy ABI pad
};
static_assert(sizeof(OceanReflectedCausticsParamsGPU) == 848,
              "OceanReflectedCausticsParamsGPU must match the std140 UBO in ocean_lightshafts.frag");

} // namespace

float ReflectedCausticsSurfaceDisplacementBound(const OceanParamsGPU& params,
                                                const float* measuredFFTMaxVertical)
{
    // Mirrors OceanHeightAt in ocean_lightshafts.frag: FFT mode clamps the summed
    // displacement to +/-MaxVerticalDisplacement; Gerstner mode adds up to 16 waves.
    if (params.WaveMode >= 1u && params.FFTCascadeCount >= 1u)
    {
        const float clamp = std::max(params.MaxVerticalDisplacement, 0.0f);
        if (!measuredFFTMaxVertical)
            return clamp;
        return std::min(clamp, std::max(2.0f * *measuredFFTMaxVertical,
                                        kReflectedCausticsMeasuredBoundFloor));
    }
    float bound = 0.0f;
    for (uint32_t i = 0; i < params.GerstnerWaveCount && i < 16u; ++i)
        bound += std::abs(params.Waves[i].Amplitude);
    return bound;
}

namespace
{

// Builds locally, then one copy into the caller's upload-ring slot: the ring
// is host-coherent (often write-combined), so a single memcpy-sized store
// beats field-by-field writes there.
void FillReflectedCausticsParams(OceanReflectedCausticsParamsGPU& out,
                                 const float invViewProj[16], const float cameraPos[4],
                                 const OceanParamsGPU& params,
                                 const OceanUnderwaterSettings& settings, bool causticsAvailable,
                                 float surfaceDisplacementBound, uint32_t localFFTReadyMask,
                                 const OceanCascadeLayoutGPU* localFFTMaskLayout)
{
    OceanReflectedCausticsParamsGPU u{};
    std::memcpy(u.InvViewProj, invViewProj, sizeof(float) * 16);
    u.CameraPos[0] = cameraPos[0];
    u.CameraPos[1] = cameraPos[1];
    u.CameraPos[2] = cameraPos[2];
    u.Time[0] = params.Time;
    u.SunDirection[0] = params.SunDirection[0];
    u.SunDirection[1] = params.SunDirection[1];
    u.SunDirection[2] = params.SunDirection[2];
    u.Ocean[0] = params.SeaLevel;
    u.Ocean[1] = causticsAvailable ? 1.0f : 0.0f;
    u.Ocean[2] = surfaceDisplacementBound;
    u.Ocean[3] = kReflectedCausticsMinRise;
    u.Caustics[0] = std::max(params.CausticsScale, 0.001f);
    u.Caustics[1] = params.CausticsAverage;
    u.Caustics[2] = std::max(params.CausticsStrength, 0.0f);
    u.ReflectedCaustics[0] = std::max(settings.ReflectedCaustics_Strength, 0.0f);
    u.ReflectedCaustics[1] = std::max(settings.ReflectedCaustics_Height, 0.0f);
    u.ReflectedCaustics[2] = std::max(settings.ReflectedCaustics_Falloff, 0.01f);
    u.FFTParams[0] = static_cast<float>(params.WaveMode);
    u.FFTParams[1] = static_cast<float>(params.FFTCascadeCount);
    u.FFTParams[2] = params.Weight;
    u.FFTParams[3] = params.MaxVerticalDisplacement;
    u.LocalFFT[0] = static_cast<float>(localFFTReadyMask & ((1u << kMaxOceanLocalFFTStreams) - 1u));
    if (localFFTReadyMask != 0u && localFFTMaskLayout)
    {
        const uint32_t lodCount =
            std::min<uint32_t>(localFFTMaskLayout->LodCount, kMaxOceanLodCascades);
        for (uint32_t i = 0; i < lodCount; ++i)
        {
            std::memcpy(u.LocalFFTMaskCascadeOriginScale + i * 4u,
                        localFFTMaskLayout->CascadeOriginScale[i], sizeof(float) * 4u);
        }
        u.LocalFFTMaskCascadeMeta[0] = static_cast<float>(lodCount);
    }
    u.Gerstner[0] = static_cast<float>(params.GerstnerWaveCount);
    u.Gerstner[1] = params.ChoppyScale;
    for (uint32_t i = 0; i < params.GerstnerWaveCount && i < 16u; ++i)
    {
        const auto& w = params.Waves[i];
        u.Waves[i * 8u + 0u] = w.DirectionX;
        u.Waves[i * 8u + 1u] = w.DirectionZ;
        u.Waves[i * 8u + 2u] = w.Amplitude;
        u.Waves[i * 8u + 3u] = w.Wavelength;
        u.Waves[i * 8u + 4u] = w.Steepness;
        u.Waves[i * 8u + 5u] = w.Speed;
    }
    out = u;
}

// 1x1 zero texture that keeps never-read composite bindings valid: the array
// flavour stands in for absent FFT displacement/mask arrays, the 2D flavour for
// absent depth/caustics. The shader gates their use via the params UBO, so the
// contents are irrelevant.
TextureHandle CreateZeroTexture(IDevice& device, const char* debugName, TextureCreateFlags flags)
{
    TextureDesc td{};
    td.width = 1;
    td.height = 1;
    td.depth = 1;
    td.arrayLayers = 1;
    td.mipLevels = 1;
    td.sampleCount = 1;
    td.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    td.flags = flags;
    td.initialState = ResourceState::Undefined;
    td.debugName = debugName;

    TextureHandle texture = device.CreateTexture(td);
    if (!texture.IsValid())
        return {};

    const uint16_t zeroHalfRgba[4] = {};
    UploadTexture2D(&device, texture, zeroHalfRgba, 1, 1, sizeof(zeroHalfRgba),
                    "Ocean_Dummy_Upload");
    return texture;
}
} // namespace

OceanLightShafts::~OceanLightShafts()
{
    if (!m_Device)
        return;
    if (m_Sampler.IsValid())
        m_Device->DestroySampler(m_Sampler);
    if (m_DummyArrayTexture.IsValid())
        m_Device->DestroyTexture(m_DummyArrayTexture);
    if (m_DummyTexture2D.IsValid())
        m_Device->DestroyTexture(m_DummyTexture2D);
}

bool OceanLightShafts::EnsurePipeline(IDevice& device)
{
    if (!m_PipelineLoadAttempted)
    {
        m_PipelineLoadAttempted = true;

        const fs::path shaderDir = OceanShaderDirectory("ocean_lightshafts.frag");
        if (!shaderDir.empty())
        {
            const fs::path includeDir = shaderDir.parent_path();
            const fs::path cacheRoot = fs::path(".Cache") / "Shaders";

            ShaderProgramCompileRequest req{};
            req.debugName = "ocean_reflected_caustics";
            req.baseDirectory = shaderDir;
            req.cacheRoot = cacheRoot;
            req.includeDirs = {includeDir};
            req.stages = {{"vs", "ocean_fullscreen.vert", "main", {}},
                          {"fs", "ocean_lightshafts.frag", "main", {}}};

            ShaderProgramCompileResult result{};
            std::string err;
            if (LoadOceanShaderProgram(req, device.PreferredShaderSource(), result, &err))
            {
                auto vs = result.stageBytes.find("vs");
                auto fsIt = result.stageBytes.find("fs");
                if (vs != result.stageBytes.end() && fsIt != result.stageBytes.end() &&
                    !vs->second.empty() && !fsIt->second.empty())
                {
                    m_Layout.debugName = "Ocean.ReflectedCaustics.Set0";
                    m_Layout.bindings = {
                        {0, DescriptorType::CombinedImageSampler, 1, kLightShaftsFragmentStage},
                        {1, DescriptorType::UniformBuffer, 1, kLightShaftsFragmentStage},
                        {2, DescriptorType::CombinedImageSampler, 1, kLightShaftsFragmentStage},
                        {3, DescriptorType::CombinedImageSampler, 1, kLightShaftsFragmentStage},
                        {4, DescriptorType::CombinedImageSampler, 1, kLightShaftsFragmentStage},
                        {5, DescriptorType::CombinedImageSampler, 1, kLightShaftsFragmentStage},
                        {6, DescriptorType::CombinedImageSampler, 1, kLightShaftsFragmentStage},
                        {7, DescriptorType::CombinedImageSampler, 1, kLightShaftsFragmentStage},
                        {10, DescriptorType::CombinedImageSampler, 1, kLightShaftsFragmentStage},
                        {11, DescriptorType::CombinedImageSampler, 1, kLightShaftsFragmentStage},
                        {12, DescriptorType::CombinedImageSampler, 1, kLightShaftsFragmentStage},
                        {13, DescriptorType::CombinedImageSampler, 1, kLightShaftsFragmentStage},
                        {14, DescriptorType::CombinedImageSampler, 1, kLightShaftsFragmentStage},
                        {15, DescriptorType::CombinedImageSampler, 1, kLightShaftsFragmentStage},
                        {16, DescriptorType::CombinedImageSampler, 1, kLightShaftsFragmentStage}};
                    m_Pipeline = RenderGraph::MakeFullscreenPipelineDesc("Ocean.ReflectedCaustics");
                    m_Pipeline.vertexShader = vs->second;
                    m_Pipeline.pixelShader = fsIt->second;
                    Rendering::ApplyMetaImageShapeToLayout(result.meta, m_Layout);
                    m_Pipeline.descriptorSetLayouts.push_back(m_Layout);
                }
            }
            else if (!m_WarnedLoadFailed)
            {
                m_WarnedLoadFailed = true;
                Logger::Log::Warning("OceanReflectedCaustics: composite compile failed: {}", err);
            }
        }
        if (m_Pipeline.vertexShader.empty() && !m_WarnedLoadFailed)
        {
            m_WarnedLoadFailed = true;
            Logger::Log::Warning("OceanReflectedCaustics: sources unavailable; pass declines.");
        }

        RenderGraph::LoadCopyPipelineDesc(device.PreferredShaderSource(),
                                          "Ocean.ReflectedCaustics.Copy",
                                          "Ocean.ReflectedCaustics.Copy.Set0", m_CopyLayout,
                                          m_CopyPipeline);

        // MSAA resolve program (sampler2DMS averaging) for the scene copy when
        // SceneColor is multisampled.
        if (!shaderDir.empty())
        {
            ShaderProgramCompileRequest rreq{};
            rreq.debugName = "ocean_reflected_caustics_resolve";
            rreq.baseDirectory = shaderDir;
            rreq.cacheRoot = fs::path(".Cache") / "Shaders";
            rreq.includeDirs = {shaderDir.parent_path()};
            rreq.stages = {{"vs", "ocean_fullscreen.vert", "main", {}},
                           {"fs", "ocean_resolve.frag", "main", {}}};
            ShaderProgramCompileResult rres{};
            std::string rerr;
            if (LoadOceanShaderProgram(rreq, device.PreferredShaderSource(), rres, &rerr))
            {
                auto rvs = rres.stageBytes.find("vs");
                auto rfs = rres.stageBytes.find("fs");
                if (rvs != rres.stageBytes.end() && rfs != rres.stageBytes.end() &&
                    !rvs->second.empty() && !rfs->second.empty())
                {
                    m_ResolveLayout.debugName = "Ocean.ReflectedCaustics.Resolve.Set0";
                    m_ResolveLayout.bindings = {
                        {0, DescriptorType::CombinedImageSampler, 1, kLightShaftsFragmentStage}};
                    m_ResolvePipeline =
                        RenderGraph::MakeFullscreenPipelineDesc("Ocean.ReflectedCaustics.Resolve");
                    m_ResolvePipeline.vertexShader = rvs->second;
                    m_ResolvePipeline.pixelShader = rfs->second;
                    Rendering::ApplyMetaImageShapeToLayout(rres.meta, m_ResolveLayout);
                    m_ResolvePipeline.descriptorSetLayouts.push_back(m_ResolveLayout);
                }
            }
        }
    }
    if (!m_PipelineId.IsValid() && !m_Pipeline.vertexShader.empty())
        m_PipelineId = PipelineDescTranslator::InternGraphics(device, m_Pipeline);
    if (!m_CopyPipelineId.IsValid() && !m_CopyPipeline.vertexShader.empty())
        m_CopyPipelineId = PipelineDescTranslator::InternGraphics(device, m_CopyPipeline);
    if (!m_ResolvePipelineId.IsValid() && !m_ResolvePipeline.vertexShader.empty())
        m_ResolvePipelineId = PipelineDescTranslator::InternGraphics(device, m_ResolvePipeline);
    if (m_PipelineId.IsValid() && !m_Sampler.IsValid())
        m_Sampler = device.CreateSampler(SamplerDesc::MaterialLinearClamp("Ocean_ReflectedCaustics_Sampler"));
    if (m_PipelineId.IsValid() && !m_DummyArrayTexture.IsValid())
        m_DummyArrayTexture = CreateZeroTexture(device, "Ocean_ReflectedCaustics_DummyArray",
                                                TextureCreateFlags::ForceArrayView);
    if (m_PipelineId.IsValid() && !m_DummyTexture2D.IsValid())
        m_DummyTexture2D = CreateZeroTexture(device, "Ocean_ReflectedCaustics_Dummy2D",
                                             TextureCreateFlags::None);
    return m_PipelineId.IsValid() && m_CopyPipelineId.IsValid() && m_Sampler.IsValid() &&
           m_DummyArrayTexture.IsValid() && m_DummyTexture2D.IsValid();
}

bool OceanLightShafts::DeclareForView(Engine::Renderer::Pipeline::ViewDeclare& d,
                                      const OceanParamsGPU& params,
                                      const OceanUnderwaterSettings& settings,
                                      const OceanShapeSampleInputs& shape,
                                      TextureHandle caustics, SamplerHandle causticsSampler,
                                      float surfaceDisplacementBound)
{
    auto* device = d.Services.GetDevice();
    if (!device)
        return false;
    m_Device = device;

    const RenderGraph::RGTexture sceneColor = d.ResolveTexture(Engine::Renderer::Pipeline::Names::Res::SceneColor);
    if (!sceneColor.IsValid())
        return false;

    const RenderGraph::RGResourceDesc scd = d.Frame.Graph().ResourceDesc(sceneColor.Id);
    const bool msaa = scd.SampleCount > 1; // resolve via sampler2DMS instead of declining

    RenderGraph::RGTexture depth = d.ResolveTexture(Engine::Renderer::Pipeline::Names::View::DepthResolved);
    if (!depth.IsValid())
        depth = d.ViewDepthResolved.IsValid() ? d.ViewDepthResolved : d.ViewDepth;
    if (!depth.IsValid())
        return false;

    if (!EnsurePipeline(*device))
        return false;
    if (msaa && !m_ResolvePipelineId.IsValid())
        return false;

    const CameraData* camera = d.Services.Views().FindCameraData(d.View.cameraId);
    if (!camera)
        return false;

    Mathematics::Matrix4x4 vp{};
    std::memcpy(vp.Data(), camera->viewProj, sizeof(float) * 16);
    const Mathematics::Matrix4x4 invVp = Mathematics::Inverse(vp);

    const bool causticsAvailable = caustics.IsValid() && causticsSampler.IsValid() &&
                                   settings.CausticsOnGeometry &&
                                   settings.ReflectedCaustics_Strength > 0.0f &&
                                   settings.ReflectedCaustics_Height > 0.0f;
    uint32_t validLocalFFTMask =
        shape.LocalFFTReadyMask & ((1u << kMaxOceanLocalFFTStreams) - 1u);
    if (!shape.LocalFFTMaskLayout || shape.LocalFFTMaskLayout->LodCount == 0u)
    {
        validLocalFFTMask = 0u;
    }
    else
    {
        for (uint32_t page = 0; page < kOceanLocalFFTMaskPages; ++page)
        {
            if (shape.LocalFFTMasks[page].IsValid() && shape.LocalFFTMaskSamplers[page].IsValid())
                continue;
            const uint32_t pageMask =
                ((1u << kOceanLocalFFTMaskChannels) - 1u) <<
                (page * kOceanLocalFFTMaskChannels);
            validLocalFFTMask &= ~pageMask;
        }
    }
    for (uint32_t stream = 0; stream < kMaxOceanLocalFFTStreams; ++stream)
    {
        if (!shape.LocalDisplacements[stream].IsValid() ||
            !shape.LocalDisplacementSamplers[stream].IsValid())
        {
            validLocalFFTMask &= ~(1u << stream);
        }
    }
    // Params fill + upload at DECLARE time (the frame's host-coherent upload
    // ring); the composite pass captures only {buffer, offset}.
    const auto paramsAlloc = d.Frame.AllocUpload<OceanReflectedCausticsParamsGPU>();
    if (!paramsAlloc.Valid())
        return false;
    FillReflectedCausticsParams(*paramsAlloc.Ptr, invVp.Data(), camera->cameraPos, params,
                                settings, causticsAvailable, surfaceDisplacementBound,
                                validLocalFFTMask,
                                shape.LocalFFTMaskLayout);

    const uint32_t renderW = d.RenderWidth;
    const uint32_t renderH = d.RenderHeight;

    TextureDesc copyDesc{};
    copyDesc.width = renderW;
    copyDesc.height = renderH;
    copyDesc.depth = 1;
    copyDesc.arrayLayers = 1;
    copyDesc.mipLevels = 1;
    copyDesc.sampleCount = 1;
    copyDesc.format = scd.Format;
    copyDesc.usage =
        static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    copyDesc.debugName = "Ocean.ReflectedCaustics.SceneCopy";
    const std::string base =
        "Ocean.ReflectedCaustics.View" + std::to_string(static_cast<uint32_t>(d.View.id));
    const RenderGraph::RGTexture sceneCopy =
        d.Frame.CreateTexture((base + ".SceneCopy").c_str(), copyDesc);
    const RenderGraph::RGTexture compositeTarget =
        msaa ? d.Frame.CreateTexture((base + ".CompositeColor").c_str(), copyDesc) : sceneColor;
    if (!compositeTarget.IsValid())
        return false;

    // Scene copy: snapshot SceneColor so the composite can read the scene while
    // writing it. The helper picks the sampler2DMS resolve fork off SceneColor's
    // SampleCount — the same selection the msaa decline check above validated.
    RenderGraph::AddCopyPass(d.Frame, sceneColor, sceneCopy, (base + ".SceneCopy").c_str(),
                             Rendering::PassPhase::kPostProcess, m_CopyPipelineId,
                             m_ResolvePipelineId, m_CopyLayout, m_ResolveLayout, m_Sampler,
                             renderW, renderH);

    // Composite inputs, resolved at declare: everything but the scene copy and
    // depth is a feature-owned physical, bound descriptor-direct. Absent sources
    // fall back to the zero dummies — the shader gates their use via the params
    // UBO, so contents are irrelevant; the binding just has to be valid.
    const TextureHandle displacementTex =
        shape.Displacement.IsValid() ? shape.Displacement : m_DummyArrayTexture;
    const SamplerHandle displacementSamp =
        shape.Displacement.IsValid() ? shape.DisplacementSampler : m_Sampler;

    constexpr uint32_t kCompositeTextureInputs =
        4u + kMaxOceanLocalFFTStreams + kOceanLocalFFTMaskPages;
    static_assert(kCompositeTextureInputs <= RenderGraph::kMaxFullscreenTextureInputs,
                  "composite input count exceeds the fullscreen helper capacity");
    RenderGraph::RGFullscreenTextureInput textures[kCompositeTextureInputs]{};
    uint32_t textureCount = 0;
    auto addRaw = [&](uint32_t binding, TextureHandle texture, SamplerHandle texSampler,
                      RenderGraph::RGTexture rg = {})
    {
        RenderGraph::RGFullscreenTextureInput& in = textures[textureCount++];
        in.Binding = binding;
        in.Texture = rg; // valid => declared read (orders after the producing sim)
        in.RawTexture = texture;
        in.Sampler = texSampler;
    };

    // Scene snapshot: required — no snapshot, no composite.
    {
        RenderGraph::RGFullscreenTextureInput& in = textures[textureCount++];
        in.Binding = 0;
        in.Texture = sceneCopy;
        in.Sampler = m_Sampler;
        in.Required = true;
    }
    // Depth: graph read; a declined physical binds the 2D dummy so the
    // descriptor stays valid.
    {
        RenderGraph::RGFullscreenTextureInput& in = textures[textureCount++];
        in.Binding = 2;
        in.Texture = depth;
        in.Fallback = m_DummyTexture2D;
        in.Sampler = m_Sampler;
    }
    if (caustics.IsValid() && causticsSampler.IsValid())
        addRaw(3, caustics, causticsSampler);
    else
        addRaw(3, m_DummyTexture2D, m_Sampler);
    addRaw(4, displacementTex, displacementSamp, shape.DisplacementRG);
    // validLocalFFTMask already dropped streams and mask pages whose texture or
    // sampler is invalid, so a set bit implies a bindable pair.
    const uint32_t localBindings[kMaxOceanLocalFFTStreams] = {5u, 6u, 7u, 10u, 12u, 13u, 14u, 15u};
    for (uint32_t stream = 0; stream < kMaxOceanLocalFFTStreams; ++stream)
    {
        const bool ready = (validLocalFFTMask & (1u << stream)) != 0u;
        addRaw(localBindings[stream], ready ? shape.LocalDisplacements[stream] : displacementTex,
               ready ? shape.LocalDisplacementSamplers[stream] : displacementSamp,
               ready ? shape.LocalDisplacementsRG[stream] : shape.DisplacementRG);
    }
    const uint32_t localMaskBindings[kOceanLocalFFTMaskPages] = {11u, 16u};
    for (uint32_t page = 0; page < kOceanLocalFFTMaskPages; ++page)
    {
        const bool ready =
            shape.LocalFFTMasks[page].IsValid() && shape.LocalFFTMaskSamplers[page].IsValid();
        addRaw(localMaskBindings[page], ready ? shape.LocalFFTMasks[page] : m_DummyArrayTexture,
               ready ? shape.LocalFFTMaskSamplers[page] : m_Sampler,
               ready ? shape.LocalFFTMasksRG[page] : RenderGraph::RGTexture{});
    }

    RenderGraph::RGFullscreenBufferInput paramsInput{};
    paramsInput.Binding = 1;
    paramsInput.Buffer = paramsAlloc.Buffer;
    paramsInput.Offset = paramsAlloc.Offset;
    paramsInput.Size = sizeof(OceanReflectedCausticsParamsGPU);

    const std::string compositeName = base + ".Composite";
    RenderGraph::RGFullscreenDesc composite{};
    composite.Name = compositeName.c_str();
    composite.Phase = Rendering::PassPhase::kPostProcess;
    composite.Pipeline = m_PipelineId;
    composite.Layout = m_Layout;
    composite.Textures =
        std::span<const RenderGraph::RGFullscreenTextureInput>(textures, textureCount);
    composite.Buffers = std::span<const RenderGraph::RGFullscreenBufferInput>(&paramsInput, 1);
    composite.Target = compositeTarget;
    composite.Ops.Load = RenderGraph::RGLoadOp::DontCare;
    composite.Ops.Store = RenderGraph::RGStoreOp::Store;
    composite.Width = renderW;
    composite.Height = renderH;
    RenderGraph::AddFullscreenPass(d.Frame, composite);

    if (compositeTarget.Id != sceneColor.Id)
        d.PublishTexture(Engine::Renderer::Pipeline::Names::Res::SceneColor, compositeTarget);

    return true;
}

} // namespace GameEngine::Ocean
