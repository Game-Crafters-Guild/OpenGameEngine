#include "Engine/Rendering/ScreenSpaceShadows/ScreenSpaceShadowPasses.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "Mathematics/MatrixOps.h"
#include "Engine/Rendering/Pipeline/Nodes/PipelineNodeUtils.h"
#include "Engine/Rendering/ScreenSpaceShadows/ScreenSpaceShadowDispatch.h"
#include "Logger/Logger.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

namespace GameEngine::Engine::Renderer
{
namespace
{
constexpr const char* kTraceShader = "Shaders/screen_space_shadow.shaderpkg";
constexpr const char* kPrepareShader = "Shaders/screen_space_shadow_prepare.shaderpkg";
constexpr const char* kResolveShader = "Shaders/screen_space_shadow_resolve.shaderpkg";
constexpr const char* kMatchShader = "Shaders/screen_space_shadow_match.shaderpkg";
constexpr uint32_t kPushConstantBudget = 128;
constexpr uint32_t kImageGroupSize = 8; // prepare/resolve/match local_size_xy
constexpr float kMinThickness = 0.0001f;
constexpr float kMaxThickness = 0.05f;
constexpr float kBilinearThresholdScale = 4.0f;
constexpr float kContrast = 4.0f;
constexpr float kPerspectiveEpsilon = 1e-6f;
}

ScreenSpaceShadowPasses::ScreenSpaceShadowPasses(Rendering::IDevice* device) : m_Device(device) {}
ScreenSpaceShadowPasses::~ScreenSpaceShadowPasses()
{
    if (m_Device && m_Sampler.IsValid())
        m_Device->DestroySampler(m_Sampler);
}

bool ScreenSpaceShadowPasses::LoadShader()
{
    if (!m_Device)
        return false;
    const auto load = [this](const char* path, Rendering::ComputePipelineId& pipeline,
                             std::unique_ptr<Rendering::ShaderMeta>& meta,
                             Rendering::DescriptorSetLayoutDesc& layout)
    {
        if (pipeline.IsValid())
            return m_Device->GetOrCreateComputePipeline(pipeline).IsValid();
        Rendering::ShaderPackage package{};
        std::string error;
        if (!Rendering::LoadShaderPkg(path, m_Device->PreferredShaderSource(), package, &error))
        {
            if (!m_WarnedLoadFailure)
                Logger::Log::Warning("ScreenSpaceShadows: {}", error);
            m_WarnedLoadFailure = true;
            return false;
        }
        auto stage = package.stageBytes.find("cs");
        if (stage == package.stageBytes.end() || stage->second.empty())
            return false;
        meta = std::make_unique<Rendering::ShaderMeta>(std::move(package.meta));
        Rendering::ComputePipelineDesc desc{};
        desc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(stage->second));
        desc.DebugName = path;
        if (!Rendering::MaterialHelper::ApplyShaderMetaToComputeDesc(
            *m_Device, *meta, desc, Rendering::MaterialBuilder::MergeMode::Auto,
            {true, kPushConstantBudget}, [&layout](uint32_t set, Rendering::DescriptorSetLayoutDesc& value)
            { if (set == 0) layout = value; }, &error))
            return false;
        pipeline = m_Device->InternComputePipeline(std::move(desc));
        return m_Device->GetOrCreateComputePipeline(pipeline).IsValid();
    };
    if (!load(kTraceShader, m_Pipeline, m_Meta, m_Layout) ||
        !load(kPrepareShader, m_PreparePipeline,
              m_PrepareMeta, m_PrepareLayout) ||
        !load(kResolveShader, m_ResolvePipeline,
              m_ResolveMeta, m_ResolveLayout) ||
        !load(kMatchShader, m_MatchPipeline,
              m_MatchMeta, m_MatchLayout))
        return false;
    if (!m_Sampler.IsValid())
        m_Sampler = m_Device->CreateSampler(Rendering::SamplerDesc::PointClamp("ScreenSpaceShadows"));
    return m_Sampler.IsValid();
}

Rendering::RenderGraph::RGTexture ScreenSpaceShadowPasses::DeclareMaskPass(
    Rendering::RenderGraph::RGFrame& frame, uint32_t viewId,
    Rendering::RenderGraph::RGTexture depth, const Rendering::CameraData& camera,
    const float towardLight[3], float thickness,
    const ScreenSpaceShadowFilter* filter)
{
    namespace RG = Rendering::RenderGraph;
    if (!depth.IsValid() || !towardLight || !std::isfinite(thickness) ||
        // The radial projection assumes perspective depth. Orthographic views
        // retain their existing directional shadow source.
        std::abs(camera.proj[15]) > kPerspectiveEpsilon || camera.proj[0] == 0 || camera.proj[5] == 0)
        return {};
    const auto source = frame.Graph().ResourceDesc(depth.Id);
    if (source.SampleCount > 1 || !m_Device)
        return {};
    const uint64_t visibilityBytes = static_cast<uint64_t>(source.Width) * source.Height * sizeof(uint32_t);
    const auto storageLimit = m_Device->GetCapabilities().maxStorageBufferBindingSize;
    if (storageLimit != 0 && visibilityBytes > storageLimit)
        return {};
    float clip[4]{};
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 3; ++col)
            clip[row] += camera.viewProj[col * 4 + row] * towardLight[col];
    const auto plan = BuildScreenSpaceShadowDispatches(clip, source.Width, source.Height);
    if (!plan.Count ||
        !m_Device->IsTextureFormatSupported(Rendering::TextureFormat::R32_UINT,
            static_cast<uint32_t>(Rendering::TextureUsage::UnorderedAccess |
                                   Rendering::TextureUsage::ShaderResource)) || !LoadShader())
        return {};
    Rendering::TextureDesc desc{};
    desc.width = source.Width;
    desc.height = source.Height;
    desc.depth = desc.mipLevels = desc.arrayLayers = desc.sampleCount = 1;
    desc.format = static_cast<uint32_t>(Rendering::TextureFormat::R32_UINT);
    desc.usage = static_cast<uint32_t>(Rendering::TextureUsage::UnorderedAccess |
                                       Rendering::TextureUsage::ShaderResource);
    char name[64];
    std::snprintf(name, sizeof(name), "ScreenSpaceShadows.View%u", viewId);
    desc.debugName = name;
    const auto mask = frame.ImportPersistentTexture(name, desc);
    if (!mask.IsValid() || !frame.PhysicalTexture(mask).IsValid())
        return {};
    Rendering::BufferDesc visibilityDesc{};
    visibilityDesc.size = visibilityBytes;
    visibilityDesc.usage = static_cast<uint32_t>(Rendering::BufferUsage::Storage);
    char visibilityName[80];
    std::snprintf(visibilityName, sizeof(visibilityName), "%s.Visibility", name);
    visibilityDesc.debugName = visibilityName;
    const auto visibility = frame.CreateBuffer(visibilityName, visibilityDesc);
    if (!visibility.IsValid())
        return {};
    struct Params
    {
        float Light[4];
        int32_t Wave[4];
        float Settings[4];
    };
    static_assert(sizeof(Params) == 48);
    struct DispatchUpload
    {
        Rendering::BufferHandle Buffer;
        uint64_t Offset;
    };
    std::array<DispatchUpload, kScreenSpaceShadowMaxDispatches> uploads{};
    for (uint32_t i = 0; i < plan.Count; ++i)
    {
        auto upload = frame.AllocUpload<Params>();
        if (!upload.Valid())
            return {};
        *upload.Ptr = {};
        std::copy_n(plan.Light, 4, upload.Ptr->Light);
        std::copy_n(plan.Dispatches[i].Offset, 2, upload.Ptr->Wave);
        upload.Ptr->Wave[2] = static_cast<int32_t>(source.Width);
        upload.Ptr->Wave[3] = static_cast<int32_t>(source.Height);
        upload.Ptr->Settings[0] = std::clamp(thickness, kMinThickness, kMaxThickness);
        upload.Ptr->Settings[1] = upload.Ptr->Settings[0] * kBilinearThresholdScale;
        upload.Ptr->Settings[2] = kContrast;
        uploads[i] = {upload.Buffer, upload.Offset};
    }
    char prepareName[80];
    std::snprintf(prepareName, sizeof(prepareName), "%s.Prepare", name);
    frame.AddComputePass(prepareName, Rendering::PassPhase::kDefault,
        [=](RG::RGPassBuilder& pass)
        {
            pass.Read(depth, RG::RGTextureRead::Sampled);
            pass.Write(visibility, RG::RGBufferWrite::Storage);
        },
        [this, depth, visibility, bytes = visibilityDesc.size, width = source.Width, height = source.Height](RG::RGContext& context)
        {
            auto* device = context.GetDevice();
            auto* command = context.Cmd;
            if (!device || !command)
                return;
            const auto pipeline = context.GetOrCreatePipelineVariant(m_PreparePipeline);
            if (!pipeline.IsValid())
                return;
            Rendering::DescriptorSetDesc setDesc{};
            setDesc.layout = m_PrepareLayout;
            setDesc.transient = true;
            setDesc.debugName = "ScreenSpaceShadows.Prepare.Set0";
            auto set = device->CreateDescriptorSet(setDesc);
            Rendering::NamedDescriptorWriter writer(device, set, *m_PrepareMeta, 0);
            writer.AddCombinedImageSampler("uDepth", context.GetTexture(depth), m_Sampler);
            writer.AddStorageBuffer("Visibility", context.GetBuffer(visibility), 0, bytes);
            writer.Flush();
            command->SetPipeline(pipeline);
            command->BindDescriptorSet(0, set, pipeline);
            command->Dispatch((width + kImageGroupSize - 1) / kImageGroupSize, (height + kImageGroupSize - 1) / kImageGroupSize, 1);
        });
    frame.AddComputePass(name, Rendering::PassPhase::kDefault,
        [=](RG::RGPassBuilder& pass)
        {
            pass.Read(depth, RG::RGTextureRead::Sampled);
            pass.Read(visibility, RG::RGBufferRead::Storage);
            pass.Write(visibility, RG::RGBufferWrite::Storage);
        },
        [this, depth, visibility, bytes = visibilityDesc.size, plan, uploads](RG::RGContext& context)
        {
            auto* device = context.GetDevice();
            auto* command = context.Cmd;
            if (!device || !command)
                return;
            const auto pipeline = context.GetOrCreatePipelineVariant(m_Pipeline);
            if (!pipeline.IsValid())
                return;
            command->SetMarker("ScreenSpaceShadows");
            command->SetPipeline(pipeline);
            for (uint32_t i = 0; i < plan.Count; ++i)
            {
                Rendering::DescriptorSetDesc setDesc{};
                setDesc.layout = m_Layout;
                setDesc.transient = true;
                setDesc.debugName = "ScreenSpaceShadows.Set0";
                auto set = device->CreateDescriptorSet(setDesc);
                Rendering::NamedDescriptorWriter writer(device, set, *m_Meta, 0);
                writer.AddCombinedImageSampler("uDepth", context.GetTexture(depth), m_Sampler);
                writer.AddUniformBuffer("ScreenSpaceShadowParams", uploads[i].Buffer,
                                        uploads[i].Offset, sizeof(Params));
                writer.AddStorageBuffer("Visibility", context.GetBuffer(visibility), 0, bytes);
                writer.Flush();
                command->BindDescriptorSet(0, set, pipeline);
                const auto& dispatch = plan.Dispatches[i];
                command->Dispatch(dispatch.Groups[0], dispatch.Groups[1], dispatch.Groups[2]);
            }
        });
    char resolveName[80];
    std::snprintf(resolveName, sizeof(resolveName), "%s.Resolve", name);
    frame.AddComputePass(resolveName, Rendering::PassPhase::kDefault,
        [=](RG::RGPassBuilder& pass)
        {
            pass.Read(depth, RG::RGTextureRead::Sampled);
            pass.Read(visibility, RG::RGBufferRead::Storage);
            pass.Write(mask, RG::RGTextureWrite::Storage);
        },
        [this, depth, visibility, mask, bytes = visibilityDesc.size,
         width = source.Width, height = source.Height](RG::RGContext& context)
        {
            auto* device = context.GetDevice();
            auto* command = context.Cmd;
            if (!device || !command)
                return;
            const auto pipeline = context.GetOrCreatePipelineVariant(m_ResolvePipeline);
            if (!pipeline.IsValid())
                return;
            Rendering::DescriptorSetDesc setDesc{};
            setDesc.layout = m_ResolveLayout;
            setDesc.transient = true;
            setDesc.debugName = "ScreenSpaceShadows.Resolve.Set0";
            auto set = device->CreateDescriptorSet(setDesc);
            Rendering::NamedDescriptorWriter writer(device, set, *m_ResolveMeta, 0);
            writer.AddCombinedImageSampler("uDepth", context.GetTexture(depth), m_Sampler);
            writer.AddStorageBuffer("Visibility", context.GetBuffer(visibility), 0, bytes);
            writer.Flush();
            Pipeline::Nodes::Detail::BindStorageImageByName(
                device, set, *m_ResolveMeta, "uMask", context.GetTexture(mask));
            command->SetPipeline(pipeline);
            command->BindDescriptorSet(0, set, pipeline);
            command->Dispatch((width + kImageGroupSize - 1) / kImageGroupSize, (height + kImageGroupSize - 1) / kImageGroupSize, 1);
        });
    auto resolvedMask = mask;
    if (filter && filter->Constants.IsValid() && filter->Bytes > 0 && filter->Resolution > 0)
    {
        const auto matchedName = std::string(name) + ".Matched";
        desc.debugName = matchedName.c_str();
        const auto matched = frame.ImportPersistentTexture(matchedName.c_str(), desc);
        struct MatchParams { float Inverse[16]; float Projection[4]; };
        auto upload = frame.AllocUpload<MatchParams>();
        if (matched.IsValid() && frame.PhysicalTexture(matched).IsValid() && upload.Valid())
        {
            Mathematics::Matrix4x4 vp;
            std::memcpy(vp.Data(), camera.viewProjRel, sizeof(float) * 16);
            const auto inverse = Mathematics::Inverse(vp);
            std::memcpy(upload.Ptr->Inverse, inverse.Data(), sizeof(float) * 16);
            upload.Ptr->Projection[0] = camera.proj[10];
            upload.Ptr->Projection[1] = camera.proj[14];
            upload.Ptr->Projection[2] = static_cast<float>(filter->Resolution);
            upload.Ptr->Projection[3] = 0;
            frame.AddComputePass(matchedName.c_str(), Rendering::PassPhase::kDefault,
                [=](RG::RGPassBuilder& pass)
                {
                    pass.Read(depth, RG::RGTextureRead::Sampled);
                    pass.Read(mask, RG::RGTextureRead::Sampled);
                    pass.Read(visibility, RG::RGBufferRead::Storage);
                    pass.Write(matched, RG::RGTextureWrite::Storage);
                },
                [this, depth, mask, visibility, matched, filter = *filter,
                 buffer = upload.Buffer, offset = upload.Offset, bytes = visibilityDesc.size,
                 width = source.Width, height = source.Height](RG::RGContext& context)
                {
                    auto* device = context.GetDevice();
                    auto* command = context.Cmd;
                    if (!device || !command) return;
                    const auto pipeline = context.GetOrCreatePipelineVariant(m_MatchPipeline);
                    if (!pipeline.IsValid()) return;
                    Rendering::DescriptorSetDesc setDesc{};
                    setDesc.layout = m_MatchLayout;
                    setDesc.transient = true;
                    setDesc.debugName = "ScreenSpaceShadows.Match.Set0";
                    const auto set = device->CreateDescriptorSet(setDesc);
                    Rendering::NamedDescriptorWriter writer(device, set, *m_MatchMeta, 0);
                    writer.AddCombinedImageSampler("uDepth", context.GetTexture(depth), m_Sampler);
                    writer.AddCombinedImageSampler("uResolved", context.GetTexture(mask), m_Sampler);
                    writer.AddStorageBuffer("Visibility", context.GetBuffer(visibility), 0, bytes);
                    // Reflection exposes the GLSL instance names, not block types.
                    if (!writer.TryAddUniformBuffer("Params", buffer, offset, sizeof(MatchParams)) ||
                        !writer.TryAddUniformBuffer("Shadow", filter.Constants, filter.Offset, filter.Bytes))
                    {
                        Logger::Log::Error("ScreenSpaceShadows: missing matching shader uniform binding");
                        return;
                    }
                    writer.Flush();
                    Pipeline::Nodes::Detail::BindStorageImageByName(
                        device, set, *m_MatchMeta, "uOutput", context.GetTexture(matched));
                    command->SetPipeline(pipeline);
                    command->BindDescriptorSet(0, set, pipeline);
                    command->Dispatch((width + kImageGroupSize - 1) / kImageGroupSize, (height + kImageGroupSize - 1) / kImageGroupSize, 1);
                });
            resolvedMask = matched;
        }
    }
    // Shadows use only this frame's depth. Spatial reconstruction must not
    // continue changing the visibility after camera motion has stopped.
    return resolvedMask;
}
} // namespace GameEngine::Engine::Renderer
