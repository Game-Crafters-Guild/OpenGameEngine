#include "Ocean/OceanSprayGPU.h"
#include "Engine/Rendering/IEnvironmentSource.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Mathematics/VectorOps.h"
#include "Ocean/OceanRenderFeature.h"
#include "Ocean/OceanShaderProgram.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "OceanShaderDirectory.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/PipelineDescTranslator.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGFullscreen.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <memory>
namespace GameEngine::Ocean
{
using namespace Rendering;
namespace
{
struct alignas(16) SprayParamsGPU
{
    float ViewProjection[16]{}, View[16]{};
    float Camera[4]{}, Right[4]{}, Up[4]{}, WindTime[4]{}, Emission[4]{}, Appearance[4]{}, Controls[4]{},
        Shift[4]{}, Light[4]{}, Viewport[4]{};
    OceanCascadeLayoutGPU Layout{};
    float Interaction[4]{};
    uint32 ResourceFlags[4]{};
};
static_assert(sizeof(SprayParamsGPU) == 448);
constexpr uint64 kParticleBytes = 64;
// Combined-image bindings 2..27 of the shape set, less the uniform blocks at 4 and 15.
constexpr uint32 kShapeTextureCount = 24;
constexpr float kDegreesToRadians = Mathematics::Pi / 180.0f;
// Wind speed (m/s) above SprayWindThreshold over which emission ramps to full.
constexpr float kWindRampMetersPerSecond = 5.0f;
// A droplet lives at most this multiple of SprayLifetime (ocean_spray_gpu_sim.comp).
constexpr float kMaxLifetimeScale = 1.4f;
// Lighting inputs the simulation found bound (ocean_spray_gpu_common.glsl).
constexpr uint32 kSpraySunShadows = 1u;
constexpr uint32 kSprayIbl = 2u;
} // namespace
OceanSprayGPU::~OceanSprayGPU()
{
    if (m_Device && m_Sampler.IsValid())
        m_Device->DestroySampler(m_Sampler);
}
void OceanSprayGPU::SetSettings(const Components::OceanSurface &surface, float windSpeed, float windDegrees)
{
    m_Surface = surface;
    m_WindSpeed = windSpeed;
    m_WindDegrees = windDegrees;
    if (!Enabled())
        for (auto &[id, h] : m_History)
            h.Active = false;
}
void OceanSprayGPU::RebaseOrigin(float x, float y, float z)
{
    m_Origin[0] += x;
    m_Origin[1] += y;
    m_Origin[2] += z;
}
bool OceanSprayGPU::Initialize(IDevice *device)
{
    if (m_Draw.IsValid() && m_Simulate.IsValid() && m_Cull.IsValid())
        return true;
    // Programs that failed to load stay failed: logged once, no retry per frame.
    if (m_LoadFailed || !device)
        return false;
    m_LoadFailed = true;
    m_Device = device;
    const auto dir = OceanShaderDirectory("ocean_spray_gpu_sim.comp");
    m_SimLayout.debugName = "Ocean.Spray.Sim.Set0";
    m_SimLayout.bindings = {{0, DescriptorType::UniformBuffer, 1, kShaderStageCompute},
                            {1, DescriptorType::StorageBuffer, 1, kShaderStageCompute},
                            {4, DescriptorType::CombinedImageSampler, 1, kShaderStageCompute},
                            {5, DescriptorType::UniformBuffer, 1, kShaderStageCompute},
                            {6, DescriptorType::CombinedImageSampler, 1, kShaderStageCompute},
                            {7, DescriptorType::CombinedImageSampler, 1, kShaderStageCompute},
                            {8, DescriptorType::UniformBuffer, 1, kShaderStageCompute}};
    // Share the surface's shape functions and resource contract. This includes
    // local spectra, shallow attenuation, wave masks and interactive waves.
    m_OceanLayout.debugName = "Ocean.Spray.Shape.Set2";
    m_OceanLayout.bindings = {{1, DescriptorType::StorageBuffer, 1, kShaderStageCompute},
                              {4, DescriptorType::UniformBuffer, 1, kShaderStageCompute},
                              {15, DescriptorType::UniformBuffer, 1, kShaderStageCompute}};
    // Metal argument-buffer translation preserves even statically unused
    // declarations from ocean_common.glsl, so bind its complete resource layout.
    for (uint32 binding = 2; binding <= 27; ++binding)
        if (binding != 4 && binding != 15)
            m_OceanLayout.bindings.push_back(
                {binding, DescriptorType::CombinedImageSampler, 1, kShaderStageCompute});
    m_CullLayout.debugName = "Ocean.Spray.Cull.Set0";
    m_CullLayout.bindings = {{0, DescriptorType::UniformBuffer, 1, kShaderStageCompute},
                             {1, DescriptorType::StorageBuffer, 1, kShaderStageCompute},
                             {2, DescriptorType::StorageBuffer, 1, kShaderStageCompute},
                             {3, DescriptorType::StorageBuffer, 1, kShaderStageCompute}};
    m_DrawLayout.debugName = "Ocean.Spray.Draw.Set0";
    m_DrawLayout.bindings = {{0, DescriptorType::UniformBuffer, 1, kShaderStageVertex | kShaderStageFragment},
                             {1, DescriptorType::StorageBuffer, 1, kShaderStageVertex},
                             {2, DescriptorType::StorageBuffer, 1, kShaderStageVertex},
                             {3, DescriptorType::CombinedImageSampler, 1, kShaderStageFragment}};
    auto compile = [&](const char *name, const std::vector<ShaderStageCompileSpec> &stages,
                       ShaderProgramCompileResult &result) {
        ShaderProgramCompileRequest req{};
        req.debugName = name;
        req.baseDirectory = dir;
        req.cacheRoot = ".Cache/Shaders";
        req.includeDirs = {dir.parent_path()};
        req.stages = stages;
        std::string error;
        if (!LoadOceanShaderProgram(req, device->PreferredShaderSource(), result, &error))
        {
            Logger::Log::Error("GPU ocean spray: {} failed to load, so spray stays off until restart: {}", name,
                               error);
            return false;
        }
        return true;
    };
    ShaderProgramCompileResult sim{}, cull{}, draw{};
    if (!compile("ocean_spray_sim", {{"cs", "ocean_spray_gpu_sim.comp", "main", {}}}, sim) ||
        !compile("ocean_spray_cull", {{"cs", "ocean_spray_gpu_cull.comp", "main", {}}}, cull) ||
        !compile("ocean_spray_draw",
                 {{"vs", "ocean_spray_gpu.vert", "main", {}}, {"fs", "ocean_spray_gpu.frag", "main", {}}},
                 draw))
        return false;
    Rendering::ApplyMetaImageShapeToLayout(sim.meta, m_SimLayout);
    Rendering::ApplyMetaImageShapeToLayout(sim.meta, m_OceanLayout, 2);
    Rendering::ApplyMetaImageShapeToLayout(draw.meta, m_DrawLayout);
    const auto makeCompute = [&](ShaderProgramCompileResult &result, const DescriptorSetLayoutDesc &layout,
                                 const char *name) {
        ComputePipelineDesc desc{};
        desc.DebugName = name;
        desc.ComputeShader =
            std::make_shared<const std::vector<uint8_t>>(std::move(result.stageBytes.at("cs")));
        desc.DescriptorSetLayouts.push_back(device->InternDescriptorSetLayout(layout));
        if (std::string_view(name) == "Ocean.Spray.Sim")
        {
            DescriptorSetLayoutDesc empty{};
            empty.debugName = "Ocean.Spray.Unused.Set1";
            desc.DescriptorSetLayouts.push_back(device->InternDescriptorSetLayout(empty));
            desc.DescriptorSetLayouts.push_back(device->InternDescriptorSetLayout(m_OceanLayout));
        }
        return device->InternComputePipeline(desc);
    };
    m_Simulate = makeCompute(sim, m_SimLayout, "Ocean.Spray.Sim");
    m_Cull = makeCompute(cull, m_CullLayout, "Ocean.Spray.Cull");
    auto desc = RenderGraph::MakeFullscreenPipelineDesc("Ocean.Spray.Draw");
    desc.vertexShader = std::move(draw.stageBytes.at("vs"));
    desc.pixelShader = std::move(draw.stageBytes.at("fs"));
    desc.descriptorSetLayouts.push_back(m_DrawLayout);
    ColorBlendAttachmentState blend{};
    blend.blendEnable = true;
    blend.srcColorBlendFactor = BlendFactor::One;
    blend.dstColorBlendFactor = BlendFactor::OneMinusSrcAlpha;
    blend.srcAlphaBlendFactor = BlendFactor::One;
    blend.dstAlphaBlendFactor = BlendFactor::OneMinusSrcAlpha;
    desc.colorBlendState.attachments = {blend};
    m_Draw = PipelineDescTranslator::InternGraphics(*device, desc);
    if (!m_Sampler.IsValid())
        m_Sampler = device->CreateSampler(SamplerDesc::MaterialLinearClamp("Ocean.Spray.Sampler"));
    m_LoadFailed = !m_Draw.IsValid() || !m_Simulate.IsValid() || !m_Cull.IsValid() || !m_Sampler.IsValid();
    return !m_LoadFailed;
}
bool OceanSprayGPU::DeclareForView(Engine::Renderer::Pipeline::ViewDeclare &d, OceanRenderFeature &ocean)
{
    auto *device = d.Services.GetDevice();
    if (!device || !Enabled() || (d.View.ActiveRenderLayerMask() & m_Surface.SprayRenderLayerMask) == 0)
        return false;
    // The slot selects the ocean's per-slot resources; the graph's monotonic
    // index is this view's frame identity and the birth seed.
    const uint32 slot = device->GetFrameIndex();
    const uint64 frame = d.Frame.FrameIndex();
    if (!ocean.IsFoamReady() || !Initialize(device))
        return false;
    const auto *camera = d.Services.Views().FindCameraData(d.View.cameraId);
    if (!camera)
        return false;
    auto target = d.ResolveTexture(Engine::Renderer::Pipeline::Names::Res::SceneColor);
    auto depth = d.ResolveTexture(Engine::Renderer::Pipeline::Names::View::DepthResolved);
    if (!depth.IsValid())
        depth = d.ViewDepthResolved.IsValid() ? d.ViewDepthResolved : d.ViewDepth;
    if (!target.IsValid() || !depth.IsValid() || d.Frame.Graph().ResourceDesc(depth.Id).SampleCount > 1)
        return false;
    const uint32 capacity = std::clamp(m_Surface.SprayMaxParticles, 256u, 262144u);
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(m_History, [&](const auto &entry) {
        return entry.first != d.View.id && now - entry.second.LastUsed > kOceanViewHistoryIdleTimeout;
    });
    auto &h = m_History[d.View.id];
    h.LastUsed = now;
    if (h.Declared.IsFrame(frame))
        return true;
    const auto params = ocean.GetParamsForView(d.View.id);
    const float ramp =
        std::clamp((m_WindSpeed - m_Surface.SprayWindThreshold) / kWindRampMetersPerSecond, 0.0f, 1.0f);
    const float spawnRadius = std::max(m_Surface.SpraySpawnRadius, 1.0f);
    const float spawnArea = Mathematics::Pi * spawnRadius * spawnRadius;
    const float crestRate = std::max(m_Surface.SpraySpawnRate, 0.0f) * spawnArea * ramp;
    const float contactRate = m_Surface.SprayGeometryIntersections
                                  ? std::max(m_Surface.SprayIntersectionSpawnRate, 0.0f) * spawnArea * ramp
                                  : 0.0f;
    const float lifetime = std::max(m_Surface.SprayLifetime, 0.05f);
    if (crestRate + contactRate > 0.0f)
        h.LastEmissionTime = params.Time;
    else if (!h.Active || params.Time - h.LastEmissionTime > kMaxLifetimeScale * lifetime)
    {
        h.Active = false;
        return false;
    }
    const float elapsed = params.Time - h.Time;
    const bool reset = !h.Active || elapsed < 0 || elapsed > 0.5f;
    const float dt = reset ? 1.0f / 60.0f : std::clamp(elapsed, 0.0f, 1.0f / 15.0f);
    if (h.StateName.empty())
    {
        const std::string prefix = "Ocean.Spray.View" + std::to_string(d.View.id);
        h.StateName = prefix + ".State";
        h.VisibleName = prefix + ".Visible";
        h.ArgsName = prefix + ".Args";
    }
    BufferDesc stateDesc{};
    stateDesc.size = capacity * kParticleBytes;
    stateDesc.usage = static_cast<uint32>(BufferUsage::Storage | BufferUsage::TransferDst);
    stateDesc.persistent = true;
    stateDesc.debugName = "Ocean.Spray.Particles";
    bool fresh = false;
    const auto state = d.Frame.ImportPersistentBuffer(h.StateName.c_str(), stateDesc, &fresh);
    if (fresh)
        d.Frame.AddBufferZeroInit(state, "OceanSprayInitialize");
    BufferDesc idsDesc = stateDesc;
    idsDesc.size = capacity * sizeof(uint32);
    idsDesc.debugName = "Ocean.Spray.Visible";
    const auto visible = d.Frame.ImportPersistentBuffer(h.VisibleName.c_str(), idsDesc);
    BufferDesc argsDesc = idsDesc;
    argsDesc.size = 16;
    argsDesc.usage |= static_cast<uint32>(BufferUsage::Indirect);
    argsDesc.debugName = "Ocean.Spray.Args";
    const auto args = d.Frame.ImportPersistentBuffer(h.ArgsName.c_str(), argsDesc);
    d.Frame.AddBufferZeroInit(args, "OceanSprayResetDraw");
    const auto upload = d.Frame.AllocUpload<SprayParamsGPU>();
    if (!upload.Valid())
        return false;
    SprayParamsGPU u{};
    std::copy_n(camera->viewProj, 16, u.ViewProjection);
    std::copy_n(camera->view, 16, u.View);
    std::copy_n(camera->cameraPos, 4, u.Camera);
    u.Right[0] = camera->view[0];
    u.Right[1] = camera->view[4];
    u.Right[2] = camera->view[8];
    u.Up[0] = camera->view[1];
    u.Up[1] = camera->view[5];
    u.Up[2] = camera->view[9];
    const float rad = m_WindDegrees * kDegreesToRadians;
    u.WindTime[0] = std::cos(rad) * m_WindSpeed * m_Surface.SprayWindVelocityScale;
    u.WindTime[2] = std::sin(rad) * m_WindSpeed * m_Surface.SprayWindVelocityScale;
    u.WindTime[3] = dt;
    u.Emission[0] = crestRate + contactRate;
    u.Interaction[2] = crestRate / std::max(crestRate + contactRate, 0.001f);
    u.Emission[1] = spawnRadius;
    u.Emission[2] = lifetime;
    u.Emission[3] = std::max(m_Surface.SprayUpVelocity, 0.0f);
    u.Appearance[0] = std::max(m_Surface.SprayStartSize, 0.005f);
    u.Appearance[1] = std::max(m_Surface.SprayEndSize, u.Appearance[0]);
    u.Appearance[2] = std::clamp(m_Surface.SprayOpacity, 0.0f, 1.0f);
    u.Appearance[3] = params.SeaLevel;
    u.Controls[0] = static_cast<float>(capacity);
    u.Controls[2] = reset ? 1.0f : 0.0f;
    u.Controls[3] = std::clamp(m_Surface.SprayEmissionThreshold, 0.0f, 1.0f);
    for (uint32 i = 0; i < 3; ++i)
    {
        u.Shift[i] = m_Origin[i] - h.Origin[i];
        u.Light[i] = params.SunColor[i] / Mathematics::Pi;
    }
    u.Viewport[0] = static_cast<float>(d.RenderWidth);
    u.Viewport[1] = static_cast<float>(d.RenderHeight);
    u.Viewport[2] = std::abs(camera->proj[14]);
    u.Viewport[3] = camera->proj[10];
    u.Layout = ocean.GetFoamSim().GetFoamLayout();
    u.Interaction[0] = m_Surface.SprayGeometryIntersections ? 1.0f : 0.0f;
    u.Interaction[1] = std::max(m_Surface.SprayIntersectionBand, 0.05f);
    // Droplets receive the light the water surface does: the sun through the
    // engine cascaded shadow map and the environment irradiance. Missing inputs
    // bind the engine fallbacks and clear their flag.
    const auto shadowData = d.ResolveBuffer(Engine::Renderer::Pipeline::Names::Res::ShadowData);
    const auto shadowArray = d.Services.GetShadowMapArrayRG(d.Frame, d.View.id);
    const bool sunShadows = shadowData.IsValid() && shadowArray.IsValid();
    auto &ibl = d.Services.EnsureFeature<Engine::Renderer::ImageBasedLightingFeature>();
    if (!ibl.IsInitialized())
        ibl.Initialize(device);
    const BufferHandle envBuffer = ibl.IsInitialized() ? ibl.UploadEnvData(device) : BufferHandle{};
    const TextureHandle irradiance = ibl.IsInitialized() ? ibl.GetIrradianceCube() : TextureHandle{};
    const SamplerHandle cubeSampler = ibl.IsInitialized() ? ibl.GetCubeSampler() : SamplerHandle{};
    if (!envBuffer.IsValid() || !irradiance.IsValid() || !cubeSampler.IsValid())
        return false;
    u.ResourceFlags[3] = (sunShadows ? kSpraySunShadows : 0u) | kSprayIbl;
    u.ResourceFlags[0] =
        (ocean.IsClipReady() && ocean.IsClipEnabled() && ocean.GetClip().HasSources()) ? 1u : 0u;
    u.ResourceFlags[1] =
        (ocean.IsFlowReady() && ocean.IsFlowEnabled() && ocean.GetFlow().HasSources()) ? 1u : 0u;
    u.ResourceFlags[2] = static_cast<uint32>(frame);
    *upload.Ptr = u;
    const bool seabedReady = ocean.IsSeabedDepthReady() && ocean.GetSeabedDepth().HasSeabeds();
    const bool flowReady = ocean.IsFlowReady() && ocean.IsFlowEnabled() && ocean.GetFlow().HasSources();
    const bool dynReady =
        ocean.IsDynWavesReady() && ocean.IsDynWavesEnabled() && !ocean.GetDynWaves().IsQuiescent();
    const bool maskReady = ocean.IsWaveMaskReady() && ocean.GetWaveMask().HasSources();
    const bool clipReady = ocean.IsClipReady() && ocean.IsClipEnabled() && ocean.GetClip().HasSources();
    const auto oceanParams = d.Frame.AllocUpload<OceanParamsGPU>();
    const auto cascadeLayouts = d.Frame.AllocUpload<OceanSampledCascadeLayoutsGPU>();
    const auto combineLayout = d.Frame.AllocUpload<OceanCascadeLayoutGPU>();
    if (!oceanParams.Valid() || !cascadeLayouts.Valid() || !combineLayout.Valid())
        return false;
    ocean.FillViewParams(*oceanParams.Ptr, d.View.id, slot, false, seabedReady, false, flowReady, dynReady,
                         maskReady, clipReady, false, false, false, false);
    OceanRenderFeature::SampledCascadeAvailability available{};
    available[OceanSampledCascade::Foam] = true;
    available[OceanSampledCascade::SeabedDepth] = seabedReady;
    available[OceanSampledCascade::Flow] = flowReady;
    available[OceanSampledCascade::DynWaves] = dynReady;
    available[OceanSampledCascade::WaveMask] = maskReady;
    available[OceanSampledCascade::Clip] = clipReady;
    *cascadeLayouts.Ptr = ocean.SampledCascadeLayouts(available);
    *combineLayout.Ptr = ocean.GetCombineSim().GetLayout();
    const auto foam = ocean.GetFoamSim().ImportWriteRG(d.Frame);
    const auto foamSampler = ocean.GetFoamSampler(), sampler = m_Sampler;
    struct ShapeTexture
    {
        uint32 Binding;
        RenderGraph::RGTexture Texture;
        SamplerHandle Sampler;
    };
    std::array<ShapeTexture, kShapeTextureCount> shapeTextures{};
    uint32 shapeTextureCount = 0;
    const auto add = [&](uint32 binding, RenderGraph::RGTexture texture, SamplerHandle sourceSampler) {
        shapeTextures[shapeTextureCount++] = {binding, texture.IsValid() ? texture : foam,
                                              sourceSampler.IsValid() ? sourceSampler : foamSampler};
    };
    add(2, ocean.IsFFTReady() ? ocean.GetFFT().ImportDisplacementRG(d.Frame) : foam,
        ocean.IsFFTReady() ? ocean.GetDisplacementSampler() : foamSampler);
    add(3, foam, foamSampler);
    for (uint32 binding : {5u, 7u, 12u, 13u, 27u})
        add(binding, depth, sampler); // inactive sampler2D declarations
    add(11, foam, foamSampler);       // inactive albedo array

    add(6, seabedReady ? ocean.GetSeabedDepth().ImportRG(d.Frame) : foam, ocean.GetSeabedDepthSampler());
    add(8, flowReady ? ocean.GetFlow().ImportRG(d.Frame) : foam, ocean.GetFlowSampler());
    add(9, dynReady ? ocean.GetDynWaves().ImportWriteRG(d.Frame) : foam, ocean.GetDynWavesSampler());
    add(10, clipReady ? ocean.GetClip().ImportRG(d.Frame) : foam, ocean.GetClipSampler());
    add(14, ocean.IsCombineReadyForFrame(slot) ? ocean.GetCombineSim().ImportRG(d.Frame) : foam,
        ocean.GetCombineSim().GetSampler());
    add(16, maskReady ? ocean.GetWaveMask().ImportMaskRG(d.Frame) : foam, ocean.GetWaveMaskSampler());
    const uint32 localMask = ocean.GetLocalFFTReadyMaskForFrame(slot);
    for (uint32 i = 0; i < kMaxOceanLocalFFTStreams; ++i)
        add(17u + i, (localMask & (1u << i)) ? ocean.GetLocalFFT(i).ImportDisplacementRG(d.Frame) : foam,
            (localMask & (1u << i)) ? ocean.GetLocalFFT(i).GetSampler() : foamSampler);
    for (uint32 page = 0; page < 2; ++page)
        add(25u + page,
            maskReady && localMask ? ocean.GetWaveMask().ImportLocalFFTMaskRG(d.Frame, page) : foam,
            ocean.GetWaveMaskSampler());
    assert(shapeTextureCount == kShapeTextureCount && "every shape-set image binding must be filled");
    const auto sim = m_Simulate, cull = m_Cull;
    const auto draw = m_Draw;
    const auto simLayout = m_SimLayout, cullLayout = m_CullLayout, drawLayout = m_DrawLayout,
               shapeLayout = m_OceanLayout;
    d.Frame.AddPass(
        "OceanSpraySimulate", PassPhase::kPostProcess,
        [&](RenderGraph::RGPassBuilder &p) {
            p.Read(state, RenderGraph::RGBufferRead::Storage);
            p.Write(state, RenderGraph::RGBufferWrite::Storage);
            p.Read(depth, RenderGraph::RGTextureRead::SampledCompute);
            for (const auto &source : shapeTextures)
                p.Read(source.Texture, RenderGraph::RGTextureRead::SampledCompute);
            if (sunShadows)
            {
                p.Read(shadowArray, RenderGraph::RGTextureRead::SampledCompute);
                if (shadowData.Graph.IsValid())
                    p.Read(shadowData.Graph, RenderGraph::RGBufferRead::Uniform);
            }
        },
        [state, depth, sim, simLayout, shapeLayout, shapeTextures, oceanParams, cascadeLayouts, combineLayout,
         sampler, capacity, buf = upload.Buffer, off = upload.Offset, sunShadows, shadowData, shadowArray,
         shadowFallback = d.Services.GetCascadeShadowFallbackTexture(),
         shadowSampler = d.Services.GetCascadeShadowSampler(), envBuffer, irradiance,
         cubeSampler](RenderGraph::RGContext &ctx) {
            auto *dev = ctx.GetDevice();
            if (!dev || !ctx.Cmd)
                return;
            const auto pso = dev->GetOrCreateComputePipeline(sim);
            if (!pso)
                return;
            DescriptorSetDesc desc{};
            desc.layout = simLayout;
            desc.transient = true;
            desc.debugName = "Ocean.Spray.Sim.DS";
            const auto ds = dev->CreateDescriptorSet(desc);
            dev->UpdateBufferBinding(ds, 0, buf, off, sizeof(SprayParamsGPU));
            dev->UpdateStorageBufferBinding(ds, 1, ctx.GetBuffer(state), 0, capacity * kParticleBytes);
            dev->UpdateCombinedImageSamplerBinding(ds, 4, ctx.GetTexture(depth), sampler);
            if (sunShadows)
            {
                dev->UpdateBufferBinding(ds, 5, shadowData.Buffer, shadowData.Offset, shadowData.Size);
                dev->UpdateCombinedImageSamplerBinding(ds, 6, ctx.GetTexture(shadowArray), shadowSampler);
            }
            else
            {
                dev->UpdateBufferBinding(ds, 5, buf, off, sizeof(SprayParamsGPU));
                dev->UpdateCombinedImageSamplerBinding(ds, 6, shadowFallback, shadowSampler);
            }
            dev->UpdateCombinedImageSamplerBinding(ds, 7, irradiance, cubeSampler);
            dev->UpdateBufferBinding(ds, 8, envBuffer, 0, Engine::Renderer::ImageBasedLightingFeature::kEnvDataBytes);
            desc.layout = shapeLayout;
            desc.debugName = "Ocean.Spray.Shape.DS";
            const auto shapeDS = dev->CreateDescriptorSet(desc);
            dev->UpdateStorageBufferBinding(shapeDS, 1, oceanParams.Buffer, oceanParams.Offset,
                                            sizeof(OceanParamsGPU));
            dev->UpdateBufferBinding(shapeDS, 4, cascadeLayouts.Buffer, cascadeLayouts.Offset,
                                     sizeof(OceanSampledCascadeLayoutsGPU));
            dev->UpdateBufferBinding(shapeDS, 15, combineLayout.Buffer, combineLayout.Offset,
                                     sizeof(OceanCascadeLayoutGPU));
            for (const auto &source : shapeTextures)
                dev->UpdateCombinedImageSamplerBinding(shapeDS, source.Binding,
                                                       ctx.GetTexture(source.Texture), source.Sampler);
            ctx.Cmd->SetPipeline(pso);
            ctx.Cmd->BindDescriptorSet(0, ds, pso);
            ctx.Cmd->BindDescriptorSet(2, shapeDS, pso);
            ctx.Cmd->Dispatch((capacity + 63u) / 64u, 1, 1);
        });
    d.Frame.AddPass(
        "OceanSprayCull", PassPhase::kPostProcess,
        [&](RenderGraph::RGPassBuilder &p) {
            p.Read(state, RenderGraph::RGBufferRead::Storage);
            p.Read(args, RenderGraph::RGBufferRead::Storage);
            p.Write(args, RenderGraph::RGBufferWrite::Storage);
            p.Write(visible, RenderGraph::RGBufferWrite::Storage);
        },
        [state, visible, args, cull, cullLayout, capacity, buf = upload.Buffer,
         off = upload.Offset](RenderGraph::RGContext &ctx) {
            auto *dev = ctx.GetDevice();
            if (!dev || !ctx.Cmd)
                return;
            const auto pso = dev->GetOrCreateComputePipeline(cull);
            if (!pso)
                return;
            DescriptorSetDesc desc{};
            desc.layout = cullLayout;
            desc.transient = true;
            desc.debugName = "Ocean.Spray.Cull.DS";
            const auto ds = dev->CreateDescriptorSet(desc);
            dev->UpdateBufferBinding(ds, 0, buf, off, sizeof(SprayParamsGPU));
            dev->UpdateStorageBufferBinding(ds, 1, ctx.GetBuffer(state), 0, capacity * kParticleBytes);
            dev->UpdateStorageBufferBinding(ds, 2, ctx.GetBuffer(visible), 0, capacity * sizeof(uint32));
            dev->UpdateStorageBufferBinding(ds, 3, ctx.GetBuffer(args), 0, 16);
            ctx.Cmd->SetPipeline(pso);
            ctx.Cmd->BindDescriptorSet(0, ds, pso);
            ctx.Cmd->Dispatch((capacity + 63u) / 64u, 1, 1);
        });
    d.Frame.AddPass(
        "OceanSprayDraw", PassPhase::kPostProcess,
        [&](RenderGraph::RGPassBuilder &p) {
            p.AttachColor(0, target);
            p.Read(depth, RenderGraph::RGTextureRead::Sampled);
            p.Read(state, RenderGraph::RGBufferRead::Storage);
            p.Read(visible, RenderGraph::RGBufferRead::Storage);
            p.Read(args, RenderGraph::RGBufferRead::Indirect);
        },
        [state, visible, args, depth, draw, drawLayout, sampler, capacity, width = d.RenderWidth,
         height = d.RenderHeight, buf = upload.Buffer, off = upload.Offset](RenderGraph::RGContext &ctx) {
            auto *dev = ctx.GetDevice();
            if (!dev || !ctx.Cmd)
                return;
            const auto pso = ctx.GetOrCreatePipelineVariant(draw);
            if (!pso)
                return;
            DescriptorSetDesc desc{};
            desc.layout = drawLayout;
            desc.transient = true;
            desc.debugName = "Ocean.Spray.Draw.DS";
            const auto ds = dev->CreateDescriptorSet(desc);
            dev->UpdateBufferBinding(ds, 0, buf, off, sizeof(SprayParamsGPU));
            dev->UpdateStorageBufferBinding(ds, 1, ctx.GetBuffer(state), 0, capacity * kParticleBytes);
            dev->UpdateStorageBufferBinding(ds, 2, ctx.GetBuffer(visible), 0, capacity * sizeof(uint32));
            dev->UpdateCombinedImageSamplerBinding(ds, 3, ctx.GetTexture(depth), sampler);
            ctx.Cmd->SetPipeline(pso);
            ctx.Cmd->SetViewport(0, 0, static_cast<float>(width), static_cast<float>(height));
            ctx.Cmd->SetScissor(0, 0, width, height);
            ctx.Cmd->BindDescriptorSet(0, ds, pso);
            ctx.Cmd->DrawIndirect(ctx.GetBuffer(args), 1, 16);
        });
    d.Frame.MarkOutput(state);
    h.Time = params.Time;
    h.Declared.Frame = frame;
    h.Active = true;
    std::copy_n(m_Origin, 3, h.Origin);
    return true;
}
} // namespace GameEngine::Ocean
