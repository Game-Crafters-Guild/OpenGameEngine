#include "Ocean/OceanShadowSim.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Ocean/OceanShaderProgram.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "OceanShaderDirectory.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace GameEngine::Ocean
{
using namespace Rendering;
namespace
{
struct alignas(16) ShadowInputGPU
{
    float Rect[4]{};
    float Values[4]{};
};
struct alignas(16) ShadowParamsGPU
{
    OceanCascadeLayoutGPU Current{}, Previous{};
    float Simulation[4]{}; // sea level, previous weight, soft jitter diameter, hard jitter diameter
    float Frame[4]{};      // history valid, seed, input count, pad
    ShadowInputGPU Inputs[64]{};
};
static_assert(sizeof(ShadowParamsGPU) == 2336);
} // namespace
struct OceanShadowSim::History
{
    OceanCascadeArray Fields[2];
    uint32 Index = 0;
    OceanFrameStamp Declared;
    std::chrono::steady_clock::time_point LastUsed{};
    float Time = 0, Sun[3]{};
    bool Valid = false;
};
OceanShadowSim::OceanShadowSim() = default;
OceanShadowSim::~OceanShadowSim()
{
    if (m_Device && m_Sampler.IsValid())
        m_Device->DestroySampler(m_Sampler);
}
bool OceanShadowSim::Initialize(IDevice *device)
{
    if (m_Pipeline.IsValid())
        return true;
    // A program that failed to load stays failed: logged once, no retry per frame.
    if (m_LoadFailed || !device)
        return false;
    m_LoadFailed = true;
    m_Device = device;
    const auto dir = OceanShaderDirectory("ocean_shadow_sim.comp");
    ShaderProgramCompileRequest req{};
    req.debugName = "ocean_shadow_sim";
    req.baseDirectory = dir;
    req.cacheRoot = ".Cache/Shaders";
    req.includeDirs = {dir.parent_path()};
    req.stages = {{"cs", "ocean_shadow_sim.comp", "main", {}}};
    ShaderProgramCompileResult result{};
    std::string error;
    const auto stage = LoadOceanShaderProgram(req, device->PreferredShaderSource(), result, &error)
                           ? result.stageBytes.find("cs")
                           : result.stageBytes.end();
    if (stage == result.stageBytes.end() || stage->second.empty())
    {
        Logger::Log::Error("Ocean shadows: ocean_shadow_sim failed to load, so water shadows stay off until "
                           "restart: {}",
                           error);
        return false;
    }
    m_Layout.debugName = "Ocean.Shadows.Set0";
    m_Layout.bindings = {{0, DescriptorType::UniformBuffer, 1, kShaderStageCompute},
                         {1, DescriptorType::CombinedImageSampler, 1, kShaderStageCompute},
                         {2, DescriptorType::StorageImage, 1, kShaderStageCompute},
                         {8, DescriptorType::UniformBuffer, 1, kShaderStageCompute},
                         {9, DescriptorType::CombinedImageSampler, 1, kShaderStageCompute}};
    Rendering::ApplyMetaImageShapeToLayout(result.meta, m_Layout);
    ComputePipelineDesc desc{};
    desc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(stage->second));
    desc.DescriptorSetLayouts.push_back(device->InternDescriptorSetLayout(m_Layout));
    desc.DebugName = "Ocean.Shadows";
    m_Pipeline = device->InternComputePipeline(desc);
    if (!m_Sampler.IsValid())
        m_Sampler = device->CreateSampler(SamplerDesc::MaterialLinearClamp("Ocean.Shadows.Sampler"));
    m_LoadFailed = !m_Pipeline.IsValid() || !m_Sampler.IsValid();
    return !m_LoadFailed;
}
void OceanShadowSim::RebaseOrigin(float x, float z)
{
    for (auto &[view, h] : m_Histories)
        for (auto &field : h->Fields)
            field.RebaseOrigin(x, z);
}
bool OceanShadowSim::DeclareForView(Engine::Renderer::Pipeline::ViewDeclare &d, const OceanParamsGPU &params)
{
    const uint64 frame = d.Frame.FrameIndex();
    if (!m_Settings.Enabled)
    {
        m_Histories.clear();
        return false;
    }
    const auto data = d.ResolveBuffer(Engine::Renderer::Pipeline::Names::Res::ShadowData);
    const auto array = d.Services.GetShadowMapArrayRG(d.Frame, d.View.id);
    const auto *resources = d.Services.Views().GetViewShadowResources(d.View.id);
    const auto *camera = d.Services.Views().FindCameraData(d.View.cameraId);
    if (!data.IsValid() || !array.IsValid() || !resources || !resources->shadowSampler.IsValid() || !camera ||
        !Initialize(d.Services.GetDevice()))
        return false;
    const uint32 resolution = std::clamp(m_Settings.Resolution, 32u, 1024u);
    const uint32 count = std::clamp(m_Settings.CascadeCount, 1u, kMaxOceanLodCascades);
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(m_Histories, [&](const auto &entry) {
        return entry.first != d.View.id && entry.second &&
               now - entry.second->LastUsed > kOceanViewHistoryIdleTimeout;
    });
    auto &slot = m_Histories[d.View.id];
    if (!slot || slot->Fields[0].GetResolution() != resolution || slot->Fields[0].GetLodCount() != count)
    {
        slot = std::make_unique<History>();
        const std::string prefix = "Ocean.Shadows.View" + std::to_string(d.View.id);
        for (uint32 i = 0; i < 2; ++i)
            if (!slot->Fields[i].Initialize(m_Device, resolution, count, TextureFormat::R16G16_FLOAT, 64.0f,
                                            (prefix + (i ? ".B" : ".A")).c_str()))
            {
                slot.reset();
                return false;
            }
    }
    auto &h = *slot;
    h.LastUsed = now;
    if (h.Declared.IsFrame(frame))
        return true;
    const float elapsed = params.Time - h.Time;
    float sunChange = 0;
    for (uint32 i = 0; i < 3; ++i)
        sunChange += std::abs(h.Sun[i] - params.SunDirection[i]);
    const bool history =
        h.Valid && elapsed >= 0 && elapsed < 1.0f && sunChange < 0.02f && h.Declared.PrecedesFrame(frame);
    const uint32 write = h.Index ^ 1u;
    h.Fields[write].SnapToCamera(camera->cameraPos[0], camera->cameraPos[2]);
    const auto upload = d.Frame.AllocUpload<ShadowParamsGPU>();
    if (!upload.Valid())
        return false;
    ShadowParamsGPU u{};
    u.Current = h.Fields[write].GetLayout();
    u.Previous = h.Fields[h.Index].GetLayout();
    u.Simulation[0] = params.SeaLevel;
    u.Simulation[1] =
        history ? std::pow(std::clamp(m_Settings.TemporalWeight, 0.0f, 0.9999f),
                           std::max(elapsed, 1.0f / 120.0f) * std::max(m_Settings.SimulationFrequency, 1.0f))
                : 0.0f;
    u.Simulation[2] = std::max(m_Settings.JitterDiameter, 0.0f);
    u.Simulation[3] = std::max(m_Settings.HardJitterDiameter, 0.0f);
    u.Frame[0] = history ? 1.0f : 0.0f;
    u.Frame[1] = static_cast<float>(frame % 4096u);
    uint32 inputCount = 0;
    for (const auto &input : m_Inputs)
    {
        if (input.Family != OceanInputFamily::Shadow || inputCount == 64u)
            continue;
        auto &target = u.Inputs[inputCount++];
        target.Rect[0] = input.CenterX;
        target.Rect[1] = input.CenterZ;
        target.Rect[2] = input.ExtentX;
        target.Rect[3] = input.ExtentZ;
        target.Values[0] = input.Value[0];
        target.Values[1] = input.Value[1];
        target.Values[2] = input.Feather;
        target.Values[3] = static_cast<float>(input.Blend);
    }
    u.Frame[2] = static_cast<float>(inputCount);
    *upload.Ptr = u;
    const auto previous = h.Fields[h.Index].ImportRG(d.Frame), output = h.Fields[write].ImportRG(d.Frame);
    const auto pipe = m_Pipeline;
    const auto layout = m_Layout;
    const auto sampler = m_Sampler;
    const auto shadowSampler = resources->shadowSampler;
    d.Frame.AddPass(
        "OceanShadowAccumulate", PassPhase::kEarlySetup,
        [&](RenderGraph::RGPassBuilder &p) {
            p.Read(array, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(previous, RenderGraph::RGTextureRead::SampledCompute);
            p.Write(output, RenderGraph::RGTextureWrite::Storage);
            if (data.Graph.IsValid())
                p.Read(data.Graph, RenderGraph::RGBufferRead::Uniform);
        },
        [pipe, layout, sampler, shadowSampler, previous, output, array, data, buffer = upload.Buffer,
         offset = upload.Offset, resolution, count](RenderGraph::RGContext &ctx) {
            auto *device = ctx.GetDevice();
            if (!device || !ctx.Cmd)
                return;
            const auto pso = device->GetOrCreateComputePipeline(pipe);
            if (!pso)
                return;
            DescriptorSetDesc desc{};
            desc.layout = layout;
            desc.transient = true;
            desc.debugName = "Ocean.Shadows.DS";
            const auto ds = device->CreateDescriptorSet(desc);
            device->UpdateBufferBinding(ds, 0, buffer, offset, sizeof(ShadowParamsGPU));
            device->UpdateCombinedImageSamplerBinding(ds, 1, ctx.GetTexture(previous), sampler);
            device->UpdateStorageImageBinding(ds, 2, ctx.GetTexture(output));
            device->UpdateBufferBinding(ds, 8, data.Buffer, data.Offset, data.Size);
            device->UpdateCombinedImageSamplerBinding(ds, 9, ctx.GetTexture(array), shadowSampler);
            ctx.Cmd->SetPipeline(pso);
            ctx.Cmd->BindDescriptorSet(0, ds, pso);
            ctx.Cmd->Dispatch((resolution + 7u) / 8u, (resolution + 7u) / 8u, count);
        });
    d.Frame.MarkOutput(output, RenderGraph::RGImageLayout::ShaderReadOnly);
    h.Index = write;
    h.Declared.Frame = frame;
    h.Time = params.Time;
    h.Valid = true;
    std::copy_n(params.SunDirection, 3, h.Sun);
    return true;
}
bool OceanShadowSim::FillSampling(ViewId view, uint64 frame, OceanShadowSamplingGPU &out) const
{
    out = {};
    const auto it = m_Histories.find(view);
    if (it == m_Histories.end() || !it->second || !it->second->Declared.IsFrame(frame))
        return false;
    out.Layout = it->second->Fields[it->second->Index].GetLayout();
    out.Channels[0] = m_Settings.HardChannelScale;
    out.Channels[1] = m_Settings.SoftChannelScale;
    out.Channels[2] = 1.0f;
    return true;
}
TextureHandle OceanShadowSim::GetTexture(ViewId view) const
{
    const auto it = m_Histories.find(view);
    return it != m_Histories.end() && it->second ? it->second->Fields[it->second->Index].GetTexture()
                                                 : TextureHandle{};
}
RenderGraph::RGTexture OceanShadowSim::ImportRG(RenderGraph::RGFrame &frame, ViewId view) const
{
    const auto it = m_Histories.find(view);
    return it != m_Histories.end() && it->second ? it->second->Fields[it->second->Index].ImportRG(frame)
                                                 : RenderGraph::RGTexture{};
}
} // namespace GameEngine::Ocean
