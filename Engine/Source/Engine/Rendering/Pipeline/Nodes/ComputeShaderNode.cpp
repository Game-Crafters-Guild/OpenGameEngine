#include "Engine/Rendering/Pipeline/Nodes/ComputeShaderNode.h"

#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/Pipeline/ExpressionEvaluator.h"
#include "Engine/Rendering/Pipeline/Nodes/PipelineNodeUtils.h"
#include "Types/StringUtils.h"

#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Materials/ShaderMetaValidation.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/Core/NamedPushConstantWriter.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstring>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>

namespace GameEngine::Engine::Renderer { using namespace ::GameEngine::Rendering; }

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;
namespace
{
static std::string BindingLeafName(const std::string& ref)
{
    if (ref.empty())
        return {};
    const size_t dot = ref.find_last_of('.');
    if (dot == std::string::npos)
        return ref;
    return ref.substr(dot + 1);
}

static const GameEngine::Rendering::DescriptorBindingMeta* FindSet0Binding(
    const GameEngine::Rendering::ShaderMeta& meta,
    const std::string& name)
{
    for (const auto& set : meta.Sets)
    {
        if (set.Set != 0)
            continue;
        for (const auto& binding : set.Bindings)
        {
            if (binding.Name == name)
                return &binding;
        }
    }
    return nullptr;
}

static bool WritePostProcessUniformMember(
    uint8_t* dst,
    size_t dstSize,
    const GameEngine::Rendering::Member& member,
    float value)
{
    if (!dst || member.Type.Kind != GameEngine::Rendering::TypeKind::Scalar ||
        member.Offset + sizeof(float) > dstSize)
    {
        return false;
    }

    switch (member.Type.Base)
    {
    case GameEngine::Rendering::BaseType::Float:
        std::memcpy(dst + member.Offset, &value, sizeof(value));
        return true;
    case GameEngine::Rendering::BaseType::Int:
    {
        const int32_t v = static_cast<int32_t>(value);
        std::memcpy(dst + member.Offset, &v, sizeof(v));
        return true;
    }
    case GameEngine::Rendering::BaseType::UInt:
    case GameEngine::Rendering::BaseType::Bool:
    {
        const uint32_t v = static_cast<uint32_t>(std::max(value, 0.0f));
        std::memcpy(dst + member.Offset, &v, sizeof(v));
        return true;
    }
    default:
        return false;
    }
}
} // namespace

ComputeShaderNode::~ComputeShaderNode()
{
    if (m_Device && m_LinearClampSampler.IsValid())
    {
        m_Device->DestroySampler(m_LinearClampSampler);
        m_LinearClampSampler = {};
    }
    m_PipelineId = {};
    m_Set0Layout = {};
    m_ShaderMeta.reset();
    m_Device = nullptr;
}

bool ComputeShaderNode::Initialize(std::string nodeId, std::string nodeJson, std::string* outError)
{
    m_Id = std::move(nodeId);
    m_Json = std::move(nodeJson);

    m_ShaderPkg.clear();
    m_TargetTextureRef.clear();
    m_TargetBufferRef.clear();
    m_DispatchX = 1;
    m_DispatchY = 1;
    m_DispatchZ = 1;
    m_DispatchXExpr.clear();
    m_DispatchYExpr.clear();
    m_DispatchZExpr.clear();
    m_RunOnce = false;
    m_RunOnceDispatched = false;
    m_RunOnceDeclaredFrame = UINT64_MAX;
    m_IdleElide = false;
    m_ElisionGates.clear(); // re-Initialize (hot reload / blueprint swap) restarts every gate
    m_Inputs.clear();
    m_Samplers.clear();
    m_InputUsageStrings.clear();
    m_BufferBindings.clear();
    m_BufferUsageStrings.clear();
    m_UniformWritesU32x4.clear();
    m_PostProcessUniformBuffers.clear();
    m_PushConstantDefaults.clear();
    m_PPOverrides = false;
    m_RequiresLights = false;
    m_CsBytes.clear();
    m_PipelineId = {};
    m_Set0Layout = {};
    m_ShaderMeta.reset();
    m_PipelineDebugName.clear();
    m_LoggedShaderPkgIssues = false;
    m_LoggedShaderPkgLoadFailure = false;
    m_LoggedDispatchExprFailure = false;
    m_LoggedBindingsMissingFromMeta = false;
    m_LoggedMissingTextureBindings = false;
    m_LoggedMissingSamplerBindings = false;
    if (m_Device && m_LinearClampSampler.IsValid())
    {
        m_Device->DestroySampler(m_LinearClampSampler);
        m_LinearClampSampler = {};
    }
    m_Device = nullptr;

    try
    {
        const auto j = nlohmann::json::parse(m_Json);
        if (!j.is_object())
        {
            if (outError)
                *outError = "node JSON is not an object";
            return false;
        }

        if (j.contains("shaderPkg") && j["shaderPkg"].is_string())
            m_ShaderPkg = j["shaderPkg"].get<std::string>();
        if (m_ShaderPkg.empty())
        {
            if (outError)
                *outError = "ComputeShader node requires 'shaderPkg' (.shaderpkg path/name).";
            return false;
        }

        if (j.contains("targetTexture") && j["targetTexture"].is_string())
            m_TargetTextureRef = j["targetTexture"].get<std::string>();
        if (j.contains("targetTextureReadWrite") && j["targetTextureReadWrite"].is_boolean())
            m_TargetTextureReadWrite = j["targetTextureReadWrite"].get<bool>();
        if (j.contains("targetBuffer") && j["targetBuffer"].is_string())
            m_TargetBufferRef = j["targetBuffer"].get<std::string>();

        if (j.contains("dispatch") && j["dispatch"].is_object())
        {
            const auto& d = j["dispatch"];
            if (d.contains("x") && d["x"].is_number_integer())
                m_DispatchX = (uint32_t)std::max<int>(1, d["x"].get<int>());
            else if (d.contains("x") && d["x"].is_string())
                m_DispatchXExpr = d["x"].get<std::string>();
            if (d.contains("y") && d["y"].is_number_integer())
                m_DispatchY = (uint32_t)std::max<int>(1, d["y"].get<int>());
            else if (d.contains("y") && d["y"].is_string())
                m_DispatchYExpr = d["y"].get<std::string>();
            if (d.contains("z") && d["z"].is_number_integer())
                m_DispatchZ = (uint32_t)std::max<int>(1, d["z"].get<int>());
            else if (d.contains("z") && d["z"].is_string())
                m_DispatchZExpr = d["z"].get<std::string>();
        }

        if (j.contains("buffers") && j["buffers"].is_object())
        {
            for (auto it = j["buffers"].begin(); it != j["buffers"].end(); ++it)
            {
                if (!it.value().is_string())
                    continue;
                m_BufferBindings[it.key()] = it.value().get<std::string>();
            }
        }
        if (j.contains("inputs") && j["inputs"].is_object())
        {
            for (auto it = j["inputs"].begin(); it != j["inputs"].end(); ++it)
            {
                if (!it.value().is_string())
                    continue;
                m_Inputs[it.key()] = it.value().get<std::string>();
            }
        }
        if (j.contains("samplers") && j["samplers"].is_object())
        {
            for (auto it = j["samplers"].begin(); it != j["samplers"].end(); ++it)
            {
                if (!it.value().is_string())
                    continue;
                m_Samplers[it.key()] = it.value().get<std::string>();
            }
        }
        if (j.contains("bufferUsages") && j["bufferUsages"].is_object())
        {
            for (auto it = j["bufferUsages"].begin(); it != j["bufferUsages"].end(); ++it)
            {
                if (!it.value().is_string())
                    continue;
                m_BufferUsageStrings[it.key()] = it.value().get<std::string>();
            }
        }
        if (j.contains("inputUsages") && j["inputUsages"].is_object())
        {
            for (auto it = j["inputUsages"].begin(); it != j["inputUsages"].end(); ++it)
            {
                if (!it.value().is_string())
                    continue;
                m_InputUsageStrings[it.key()] = it.value().get<std::string>();
            }
        }
        else if (j.contains("textureUsages") && j["textureUsages"].is_object())
        {
            // Alias for consistency with other nodes.
            for (auto it = j["textureUsages"].begin(); it != j["textureUsages"].end(); ++it)
            {
                if (!it.value().is_string())
                    continue;
                m_InputUsageStrings[it.key()] = it.value().get<std::string>();
            }
        }

        if (j.contains("requiresLights") && j["requiresLights"].is_boolean())
            m_RequiresLights = j["requiresLights"].get<bool>();
        if (j.contains("skipWhen") && j["skipWhen"].is_object())
        {
            for (auto it = j["skipWhen"].begin(); it != j["skipWhen"].end(); ++it)
            {
                if (!it.value().is_number())
                    continue;
                SkipWhenField entry{};
                entry.Name = it.key();
                entry.Value = it.value().get<float>();
                m_SkipWhenFields.push_back(std::move(entry));
            }
        }

        if (j.contains("runOnce") && j["runOnce"].is_boolean())
            m_RunOnce = j["runOnce"].get<bool>();
        if (j.contains("idleElide") && j["idleElide"].is_boolean())
            m_IdleElide = j["idleElide"].get<bool>();
        if (j.contains("ppOverrides") && j["ppOverrides"].is_boolean())
            m_PPOverrides = j["ppOverrides"].get<bool>();
        if (j.contains("pushConstants") && j["pushConstants"].is_object())
        {
            for (auto it = j["pushConstants"].begin(); it != j["pushConstants"].end(); ++it)
            {
                PushConstantEntry entry{};
                entry.Name = it.key();
                if (it.value().is_number_float())
                {
                    entry.ValueType = PushConstantEntry::Type::Float;
                    entry.Value.F = it.value().get<float>();
                }
                else if (it.value().is_number_integer())
                {
                    entry.ValueType = PushConstantEntry::Type::Int;
                    entry.Value.I = it.value().get<int32_t>();
                }
                else
                {
                    continue;
                }
                m_PushConstantDefaults.push_back(entry);
            }
        }

        if (j.contains("uniformWrites") && j["uniformWrites"].is_object())
        {
            for (auto it = j["uniformWrites"].begin(); it != j["uniformWrites"].end(); ++it)
            {
                if (!it.value().is_object())
                    continue;
                const auto& o = it.value();
                if (o.contains("u32x4") && o["u32x4"].is_array() && o["u32x4"].size() == 4)
                {
                    std::array<uint32_t, 4> v{};
                    bool ok = true;
                    for (size_t i = 0; i < 4; ++i)
                    {
                        if (!o["u32x4"][i].is_number_integer())
                        {
                            ok = false;
                            break;
                        }
                        v[i] = (uint32_t)o["u32x4"][i].get<int64_t>();
                    }
                    if (ok)
                    {
                        m_UniformWritesU32x4[it.key()] = v;
                    }
                }
            }
        }
        if (j.contains("postProcessUniformBuffers"))
        {
            const auto& ppBuffers = j["postProcessUniformBuffers"];
            if (ppBuffers.is_array())
            {
                for (const auto& entry : ppBuffers)
                {
                    if (entry.is_string())
                        m_PostProcessUniformBuffers.push_back(entry.get<std::string>());
                }
            }
            else if (ppBuffers.is_object())
            {
                for (auto it = ppBuffers.begin(); it != ppBuffers.end(); ++it)
                {
                    if (it.value().is_boolean() && !it.value().get<bool>())
                        continue;
                    m_PostProcessUniformBuffers.push_back(it.key());
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        if (outError)
            *outError = std::string("JSON parse failed: ") + e.what();
        return false;
    }

    return true;
}

void ComputeShaderNode::EnsureCachedPipeline(GameEngine::Rendering::IDevice* device)
{
    if (!device)
        return;

    if (m_Device && m_Device != device)
    {
        if (m_LinearClampSampler.IsValid())
        {
            m_Device->DestroySampler(m_LinearClampSampler);
            m_LinearClampSampler = {};
        }
        m_CsBytes.clear();
        m_PipelineId = {};
        m_Set0Layout = {};
        m_ShaderMeta.reset();
        m_RunOnceDispatched = false;
        m_RunOnceDeclaredFrame = UINT64_MAX;
    }
    m_Device = device;

    if (!m_LinearClampSampler.IsValid())
    {
        auto sd = GameEngine::Rendering::SamplerDesc::MaterialLinearClamp("Pipeline.Compute.LinearClamp");
        m_LinearClampSampler = m_Device->CreateSampler(sd);
    }

    if (m_CsBytes.empty() || !m_ShaderMeta)
    {
        GameEngine::Rendering::ShaderPackage pkg{};
        std::string loadErr;
        bool loaded = m_RenderServices
            ? m_RenderServices->Spine().TryConsumePreLoadedShaderPackage(m_ShaderPkg, pkg)
            : false;
        // Any failure to produce the package — no resolver installed yet, the resolver
        // finding nothing, or a bad file — is treated as load-failed: the node declares
        // nothing until a retry succeeds (the same contract GPUDrawStreamBuilder's loader
        // uses). The catch is a belt for an allocation failure inside the load, not for
        // resolution, which reports a miss by returning empty.
        bool loadCallFailed = false;
        if (!loaded)
        {
            try
            {
                loadCallFailed = !GameEngine::Rendering::LoadShaderPkg(
                    m_ShaderPkg, device->PreferredShaderSource(), pkg, &loadErr);
            }
            catch (const std::exception& e)
            {
                loadCallFailed = true;
                loadErr = e.what();
            }
        }
        if (!loaded && loadCallFailed)
        {
            if (!m_LoggedShaderPkgLoadFailure)
            {
                m_LoggedShaderPkgLoadFailure = true;
                Logger::Log::Warning("RenderPipeline ComputeShader '{}': failed to load shaderpkg '{}': {}", m_Id, m_ShaderPkg, loadErr);
            }
            m_CsBytes.clear();
            m_ShaderMeta.reset();
        }
        else
        {
            auto it = pkg.stageBytes.find("cs");
            if (it == pkg.stageBytes.end() || it->second.empty())
            {
                if (!m_LoggedShaderPkgLoadFailure)
                {
                    m_LoggedShaderPkgLoadFailure = true;
                    Logger::Log::Warning("RenderPipeline ComputeShader '{}': shaderpkg '{}' missing 'cs' stage bytes", m_Id, m_ShaderPkg);
                }
                m_CsBytes.clear();
                m_ShaderMeta.reset();
            }
            else
            {
                m_CsBytes = std::move(it->second);
                m_ShaderMeta = std::make_unique<GameEngine::Rendering::ShaderMeta>(std::move(pkg.meta));
            }
        }
    }

    if (!m_PipelineId.IsValid() && !m_CsBytes.empty())
    {
        m_PipelineDebugName = std::string("Pipeline.ComputeShader.") + m_Id;

        GameEngine::Rendering::ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(m_CsBytes);
        cd.DebugName = m_PipelineDebugName;

        if (m_ShaderMeta)
        {
            const auto report = GameEngine::Rendering::ValidateShaderMeta(*m_ShaderMeta, 64);
            if (!report.Issues.empty() && !m_LoggedShaderPkgIssues)
            {
                m_LoggedShaderPkgIssues = true;
                for (const auto& iss : report.Issues)
                {
                    const auto sev = static_cast<uint32_t>(iss.Severity);
                    if (iss.Severity == GameEngine::Rendering::IssueSeverity::Error)
                        Logger::Log::Error("RenderPipeline ComputeShader '{}' meta [{}:{}]: {}", m_Id, sev, iss.Code, iss.Message);
                    else if (iss.Severity == GameEngine::Rendering::IssueSeverity::Warning)
                        Logger::Log::Warning("RenderPipeline ComputeShader '{}' meta [{}:{}]: {}", m_Id, sev, iss.Code, iss.Message);
                    else
                        Logger::Log::Info("RenderPipeline ComputeShader '{}' meta [{}:{}]: {}", m_Id, sev, iss.Code, iss.Message);
                }
            }

            // Snapshot set-0 layout for draw-time descriptor allocation via
            // the patch callback. No DB-eligibility patch — backend handles
            // it at layout-create time.
            m_Set0Layout = GameEngine::Rendering::DescriptorSetLayoutDesc{};
            auto patchLayout = [this](uint32_t setIndex, GameEngine::Rendering::DescriptorSetLayoutDesc& dsl) {
                if (setIndex == 0)
                    m_Set0Layout = dsl;
            };

            std::string err;
            if (!GameEngine::Rendering::MaterialHelper::ApplyShaderMetaToComputeDesc(
                    *device, *m_ShaderMeta, cd,
                    GameEngine::Rendering::MaterialBuilder::MergeMode::Auto,
                    {true, 128}, patchLayout, &err))
            {
                Logger::Log::Warning("RenderPipeline ComputeShader '{}': failed to apply shader meta from shaderpkg '{}': {}", m_Id, m_ShaderPkg, err);
            }
        }

        m_PipelineId = device->InternComputePipeline(std::move(cd));
    }
}

void ComputeShaderNode::DeclareForView(ViewDeclare& d)
{
    EnsureCachedPipeline(d.Services.GetDevice());
    if (!m_PipelineId.IsValid())
        return;
    if (m_RunOnce && m_RunOnceDispatched)
    {
        // Trust the done flag only while the baked artifact is still alive:
        // if the pool recreated the persistent target since the dispatch, the
        // content is gone and the bake must run again.
        bool targetAlive = true;
        if (!m_TargetTextureRef.empty())
        {
            const RenderGraph::RGTexture t = d.ResolveTexture(m_TargetTextureRef);
            targetAlive = t.IsValid() && d.Frame.PhysicalTexture(t) == m_RunOnceTargetTex;
        }
        if (targetAlive)
            return;
        m_RunOnceDispatched = false;
        m_RunOnceDeclaredFrame = UINT64_MAX;
    }

    // The old activation predicate, evaluated at declaration (the spine's
    // node loop already mask-gates the view).
    if (m_RequiresLights)
    {
        const auto* vd = d.Services.Views().FindViewDesc(d.View.id);
        const uint64_t wId = vd ? vd->worldId : 0u;
        if (d.Services.GetWorldLights(wId).empty())
            return;
    }
    const PostProcessSettings settings =
        d.Services.GetEffectivePostProcessSettings(d.View.id, d.View.worldId);

    // Effect gate (blueprint skipWhen): a disabled effect declares no dispatch
    // — and, because target resources materialize on first resolve, allocates
    // none of this node's outputs either. Evaluated on a copy carrying the
    // per-view derived fields so height-derived gates read their live value,
    // leaving the ppOverrides `settings` above byte-identical for every
    // existing node.
    if (!m_SkipWhenFields.empty())
    {
        PostProcessSettings gateSettings = settings;
        gateSettings.ResolveDerivedForRenderHeight(d.RenderHeight);
        // Tolerance mirrors FullscreenShaderNode: below any user-visible
        // slider step, above PostProcessVolume float-blend noise.
        constexpr float kSkipEpsilon = 1.0f / 1024.0f;
        bool skip = true;
        for (const auto& f : m_SkipWhenFields)
        {
            float v = 0.0f;
            if (!gateSettings.TryReadField(f.Name, v) || std::abs(v - f.Value) > kSkipEpsilon)
            {
                skip = false; // unknown field or live effect: run
                break;
            }
        }
        if (skip)
            return;
    }

    // Idle recompute elision (blueprint idleElide): collect the dispatch's
    // content inputs as they are resolved below; the gate decides just before
    // the pass would be declared.
    ElisionInputBlob elisionBlob;

    // Resolve every binding at declaration. A missing required ref means the
    // producer didn't declare this frame — skip the whole dispatch (the old
    // exec-time missing-binding guard, moved up).
    struct TexBind
    {
        std::string Name;
        Rendering::TextureHandle Tex;
    };
    struct BufBind
    {
        std::string Name;
        Rendering::BufferHandle Buf;
        uint64_t Offset = 0;
        uint64_t Size = 0;
    };
    std::vector<TexBind> texBinds;
    texBinds.reserve(m_Inputs.size());
    std::vector<std::pair<RenderGraph::RGTexture, bool>> texEdges; // (texture, isWrite)
    for (const auto& kv : m_Inputs)
    {
        const RenderGraph::RGTexture t = d.ResolveTexture(kv.second);
        if (!t.IsValid())
        {
            if (!m_LoggedMissingTextureBindings)
            {
                m_LoggedMissingTextureBindings = true;
                Logger::Log::Warning(
                    "RenderPipeline ComputeShader '{}': input '{}' ref '{}' did not resolve — "
                    "dispatch skipped this frame",
                    m_Id, kv.first, kv.second);
            }
            return;
        }
        bool isWrite = false;
        if (auto itU = m_InputUsageStrings.find(kv.first); itU != m_InputUsageStrings.end())
        {
            const std::string u = itU->second;
            isWrite = (u == "StorageWrite" || u == "storageWrite" || u == "UAV" || u == "uav");
        }
        texEdges.push_back({t, isWrite});
        texBinds.push_back({kv.first, d.Frame.PhysicalTexture(t)});
    }

    std::vector<BufBind> bufBinds;
    bufBinds.reserve(m_BufferBindings.size() + m_UniformWritesU32x4.size() +
                     m_PostProcessUniformBuffers.size());
    std::vector<std::pair<RenderGraph::RGBuffer, bool>> bufEdges; // (buffer, isWrite)
    for (const auto& bindingName : m_PostProcessUniformBuffers)
    {
        const auto* bindingMeta = m_ShaderMeta ? FindSet0Binding(*m_ShaderMeta, bindingName) : nullptr;
        if (!bindingMeta || bindingMeta->Type != GameEngine::Rendering::ShaderMetaBindingType::kUniformBuffer ||
            !bindingMeta->Block.has_value())
        {
            if (!m_LoggedBindingsMissingFromMeta)
            {
                m_LoggedBindingsMissingFromMeta = true;
                Logger::Log::Warning(
                    "RenderPipeline ComputeShader '{}': post-process uniform buffer '{}' was not found as a set-0 UBO in shader meta — dispatch skipped this frame",
                    m_Id, bindingName);
            }
            return;
        }

        const uint64_t bytes = std::max<uint32_t>(bindingMeta->Block->Size, 1u);
        auto alloc = d.Frame.AllocUpload(bytes, 256);
        if (!alloc.Ptr)
            return;
        std::memset(alloc.Ptr, 0, static_cast<size_t>(bytes));
        for (const auto& member : bindingMeta->Block->Members)
        {
            float value = 0.0f;
            if (settings.TryReadField(member.Name, value))
            {
                (void)WritePostProcessUniformMember(
                    static_cast<uint8_t*>(alloc.Ptr),
                    static_cast<size_t>(bytes),
                    member,
                    value);
            }
        }
        // Ring alloc: the OFFSET is fresh every frame (excluded), the CONTENT
        // is the input — append the just-written bytes.
        if (m_IdleElide)
            elisionBlob.AppendBytes(alloc.Ptr, static_cast<size_t>(bytes));

        bufBinds.push_back({bindingName, alloc.Buffer, alloc.Offset, bytes});
    }
    for (const auto& kv : m_BufferBindings)
    {
        // uniformWrites targets (ClusterParams) become their own ring alloc,
        // written here — the old exec mapped the blueprint Upload buffer.
        if (auto itW = m_UniformWritesU32x4.find(kv.first); itW != m_UniformWritesU32x4.end())
        {
            auto alloc = d.Frame.AllocUpload(sizeof(uint32_t) * 4, 256);
            if (!alloc.Ptr)
                return;
            std::memcpy(alloc.Ptr, itW->second.data(), sizeof(uint32_t) * 4);
            if (m_IdleElide)
                elisionBlob.AppendBytes(itW->second.data(), sizeof(uint32_t) * 4);
            const PipelineBufferBindingRG binding{alloc.Buffer, alloc.Offset,
                                                  sizeof(uint32_t) * 4, {}};
            d.PublishBuffer(kv.second, binding);
            bufBinds.push_back({kv.first, binding.Buffer, binding.Offset, binding.Size});
            continue;
        }

        const PipelineBufferBindingRG b = d.ResolveBuffer(kv.second);
        if (!b.IsValid())
        {
            if (!m_LoggedBindingsMissingFromMeta)
            {
                m_LoggedBindingsMissingFromMeta = true;
                Logger::Log::Warning(
                    "RenderPipeline ComputeShader '{}': buffer '{}' ref '{}' did not resolve — "
                    "dispatch skipped this frame",
                    m_Id, kv.first, kv.second);
            }
            return;
        }
        if (b.Graph.IsValid())
        {
            bool isWrite = false;
            if (auto itU = m_BufferUsageStrings.find(kv.first); itU != m_BufferUsageStrings.end())
            {
                const std::string u = itU->second;
                isWrite = (u == "StorageWrite" || u == "storageWrite" || u == "UAV" || u == "uav");
            }
            bufEdges.push_back({b.Graph, isWrite});
        }
        bufBinds.push_back({kv.first, b.Buffer, b.Offset, b.Size});
    }

    // Compute-target refs: resolve through the blackboard; bound by leaf
    // name / "TargetBuffer"/"TargetTexture" convention at exec (the old
    // SetComputeTarget context plumbing has no RenderGraph equivalent).
    Rendering::TextureHandle targetTex{};
    RenderGraph::RGTexture targetTexRG{};
    if (!m_TargetTextureRef.empty())
    {
        targetTexRG = d.ResolveTexture(m_TargetTextureRef);
        if (targetTexRG.IsValid())
            targetTex = d.Frame.PhysicalTexture(targetTexRG);
    }
    Rendering::BufferHandle targetBuf{};
    RenderGraph::RGBuffer targetBufRG{};
    if (!m_TargetBufferRef.empty())
    {
        const PipelineBufferBindingRG b = d.ResolveBuffer(m_TargetBufferRef);
        if (b.IsValid())
        {
            targetBuf = b.Buffer;
            targetBufRG = b.Graph;
        }
    }

    if (m_RunOnce)
    {
        const uint64_t frameIndex = d.Frame.FrameIndex();
        if (m_RunOnceDeclaredFrame == frameIndex)
            return;
        m_RunOnceDeclaredFrame = frameIndex;
    }

    // Dispatch counts at DECLARATION against the view extent — the same
    // numbers the buffer-size expressions used (cluster-grid agreement).
    // The old exec's GetTextureSize/swapchain/1280×720 fallback chain dies.
    uint32_t dx = m_DispatchX;
    uint32_t dy = m_DispatchY;
    uint32_t dz = m_DispatchZ;
    if (!m_DispatchXExpr.empty() || !m_DispatchYExpr.empty() || !m_DispatchZExpr.empty())
    {
        m_DispatchVars["renderWidth"] = (double)std::max(d.RenderWidth, 1u);
        m_DispatchVars["renderHeight"] = (double)std::max(d.RenderHeight, 1u);
        auto evalU32 = [this](const std::string& e, uint32_t fallback) -> uint32_t
        {
            if (e.empty())
                return fallback;
            double v = 0.0;
            std::string err;
            if (!Pipeline::Expr::EvalExpression(e, m_DispatchVars, v, &err))
            {
                if (!m_LoggedDispatchExprFailure)
                {
                    m_LoggedDispatchExprFailure = true;
                    Logger::Log::Warning(
                        "RenderPipeline ComputeShader '{}': dispatch expression failed: '{}': {}",
                        m_Id, e, err);
                }
                return fallback;
            }
            const double c = std::ceil(v);
            if (!(c > 0.0))
                return 1;
            const uint64_t u = (uint64_t)c;
            return u > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)u;
        };
        dx = evalU32(m_DispatchXExpr, dx);
        dy = evalU32(m_DispatchYExpr, dy);
        dz = evalU32(m_DispatchZExpr, dz);
    }

    // Safe placeholders for any reflected set0 binding the rendergraph leaves
    // unwired (captured at declaration; handles are cheap PODs).
    const Rendering::BufferHandle defaultBuffer = d.Services.GetDefaultPlaceholderBuffer();
    const Rendering::TextureHandle defaultTexture = d.Services.Textures().GetDefaultBlackTexture();

    // ── Idle recompute elision (lever #2, blueprint idleElide — DepthMinMax /
    // ClusteredLightCull). The dispatch's output lives in persistent per-view
    // pool buffers; when its complete input set is byte-identical for
    // settleFrames consecutive frames, skipping reproduces it exactly.
    // Camera bytes are the UNJITTERED logic-domain CameraData (ViewRegistry
    // two-domain rule): under TAA the raster-domain depth wobbles sub-pixel
    // per jitter phase, and the always-on pipeline re-derives tile depth
    // bounds from it every frame — the elided path freezes one phase of that
    // sub-pixel wobble instead (same class of approximation, fail-soft in
    // shading only; geometry/draws are untouched by these passes).
    if (m_IdleElide)
    {
        const auto& idle = d.Services.GetIdleElisionFrameState();
        const Rendering::CameraData cam = d.Services.Views().ResolveCameraData(d.View.id);
        static_assert(sizeof(Rendering::CameraData) == 352,
                      "CameraData layout changed — padding-free memcmp assumption needs re-verify");
        elisionBlob.AppendBytes(&cam, sizeof(cam));
        elisionBlob.Append(d.RenderWidth);
        elisionBlob.Append(d.RenderHeight);
        elisionBlob.Append(idle.ContentEpoch);
        elisionBlob.Append(idle.LightEpoch);
        elisionBlob.Append(idle.DepthDynamicEpoch);
        elisionBlob.Append(dx);
        elisionBlob.Append(dy);
        elisionBlob.Append(dz);
        elisionBlob.Append(defaultBuffer.id);
        elisionBlob.Append(defaultTexture.id);
        elisionBlob.Append(targetTex.id);
        elisionBlob.Append(targetBuf.id);
        for (const TexBind& t : texBinds)
            elisionBlob.Append(t.Tex.id);
        for (const BufBind& bb : bufBinds)
        {
            // Persistent pool bindings carry identity (a pool recreate means
            // the retained output is gone); ring allocs already appended
            // their CONTENT above and their offsets are excluded by design.
            elisionBlob.Append(static_cast<uint64_t>(bb.Size));
        }
        for (const auto& [g, isWrite] : bufEdges)
        {
            (void)g;
            elisionBlob.Append(isWrite);
        }
        // Persistent (graph-tracked) buffer identities: Graph-valid bindings
        // resolve to pool physicals whose handle bits must break equality on
        // recreate. (Ring allocs publish with an invalid Graph id.)
        for (const auto& kv : m_BufferBindings)
        {
            const PipelineBufferBindingRG b = d.ResolveBuffer(kv.second);
            if (b.Graph.IsValid())
            {
                elisionBlob.Append(b.Buffer.id);
                elisionBlob.Append(b.Offset);
                elisionBlob.Append(b.Size);
            }
        }

        constexpr uint32_t kSettleFrames = 3;
        ViewElisionGate& gate = m_ElisionGates[static_cast<uint32_t>(d.View.id)];
        const Rendering::RecomputeElisionGate::Decision decision = gate.Gate.Evaluate(
            d.Frame.FrameIndex(), std::move(elisionBlob),
            idle.AllowCluster, kSettleFrames);
        // Settle/unsettle edges are the same opt-in instrument as the window below.
        if (Rendering::IdleElisionLoggingEnabled() && decision.Skip != gate.LogState)
        {
            if (decision.Skip)
                Logger::Log::Info("[IdleElision] Compute '{}' view {} engaged", m_Id,
                                  static_cast<uint32_t>(d.View.id));
            else
                Logger::Log::Info("[IdleElision] Compute '{}' view {} disengaged: {}", m_Id,
                                  static_cast<uint32_t>(d.View.id),
                                  Rendering::ElisionDisengageReason(decision.Cause,
                                                                    decision.Skip));
            gate.LogState = decision.Skip;
        }
        if (gate.Gate.ShouldReportWindow(600) && Rendering::IdleElisionLoggingEnabled())
        {
            using Rendering::ElisionCause;
            const auto& st = gate.Gate.GetStats();
            Logger::Log::Info(
                "[IdleElision] Compute '{}' view {} window: eval {} skip {} | first {} forced {} "
                "gap {} changed {} unsettled {}",
                m_Id, static_cast<uint32_t>(d.View.id), st.Evaluated, st.Skipped,
                st.CauseCounts[static_cast<size_t>(ElisionCause::FirstEvaluate)],
                st.CauseCounts[static_cast<size_t>(ElisionCause::Forced)],
                st.CauseCounts[static_cast<size_t>(ElisionCause::EvaluationGap)],
                st.CauseCounts[static_cast<size_t>(ElisionCause::InputsChanged)],
                st.CauseCounts[static_cast<size_t>(ElisionCause::NotSettled)]);
        }
        if (decision.Skip)
            return; // retained pool outputs are exact for these inputs
    }

    d.Frame.AddComputePass(
        d.PassName().c_str(), Rendering::PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            for (const auto& [t, isWrite] : texEdges)
            {
                if (isWrite)
                    p.Write(t, RenderGraph::RGTextureWrite::Storage);
                else
                    p.Read(t, RenderGraph::RGTextureRead::Sampled);
            }
            for (const auto& [b, isWrite] : bufEdges)
            {
                if (isWrite)
                    p.Write(b, RenderGraph::RGBufferWrite::Storage);
                else
                    p.Read(b, RenderGraph::RGBufferRead::Storage);
            }
            if (targetTexRG.IsValid())
            {
                // Read-modify-write targets (imageLoad + imageStore) also
                // declare a storage read so the previous writer's texels are
                // made visible, not merely ordered (write->write alone carries
                // no read visibility).
                if (m_TargetTextureReadWrite)
                    p.Read(targetTexRG, RenderGraph::RGTextureRead::Storage);
                p.Write(targetTexRG, RenderGraph::RGTextureWrite::Storage);
            }
            if (targetBufRG.IsValid())
                p.Write(targetBufRG, RenderGraph::RGBufferWrite::Storage);
        },
        [this, texBinds = std::move(texBinds), bufBinds = std::move(bufBinds), targetTex,
         targetBuf, dx, dy, dz, settings, defaultBuffer, defaultTexture](RenderGraph::RGContext& ctx)
        {
            auto* cl = ctx.Cmd;
            auto* device = ctx.GetDevice();
            if (!cl || !device || !m_PipelineId.IsValid())
                return;
            const Rendering::PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_PipelineId);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);

            if (m_ShaderMeta && !m_ShaderMeta->PushConstants.empty() &&
                (!m_PushConstantDefaults.empty() || m_PPOverrides))
            {
                Rendering::NamedPushConstantWriter pcw(*m_ShaderMeta,
                                                       m_ShaderMeta->PushConstants[0].Name);
                if (pcw.IsValid())
                {
                    for (const auto& entry : m_PushConstantDefaults)
                    {
                        switch (entry.ValueType)
                        {
                        case PushConstantEntry::Type::Float:
                            pcw.Add(entry.Name, entry.Value.F);
                            break;
                        case PushConstantEntry::Type::Int:
                            pcw.Add(entry.Name, entry.Value.I);
                            break;
                        }
                    }

                    if (m_PPOverrides)
                    {
                        const auto& range = m_ShaderMeta->PushConstants[0];
                        for (const auto& member : range.Block.Members)
                            (void)settings.TryWriteField(member.Name, pcw);
                    }

                    pcw.Flush(cl);
                }
            }

            if (m_ShaderMeta && !m_Set0Layout.bindings.empty())
            {
                Rendering::DescriptorSetDesc ds0{};
                ds0.layout = m_Set0Layout;
                ds0.transient = true;
                ds0.debugName = "Pipeline.ComputeShader.Set0";
                const auto set0 = device->CreateDescriptorSet(ds0);

                uint32_t binding = 0;
                Rendering::DescriptorType dtype{};
                auto overrideTypeFromLayout = [&](uint32_t b, Rendering::DescriptorType& inOut)
                {
                    for (const auto& lb : m_Set0Layout.bindings)
                    {
                        if (lb.binding == b)
                        {
                            inOut = lb.type;
                            return;
                        }
                    }
                };

                std::vector<Rendering::DescriptorSetUpdate> updates;
                updates.reserve(bufBinds.size() + texBinds.size() + m_Samplers.size() + 2);

                for (const auto& bb : bufBinds)
                {
                    if (!bb.Buf.IsValid())
                        continue;
                    if (Detail::TryGetSet0BindingByName(*m_ShaderMeta, bb.Name, binding, dtype))
                    {
                        overrideTypeFromLayout(binding, dtype);
                        if (dtype == Rendering::DescriptorType::StorageBuffer ||
                            dtype == Rendering::DescriptorType::UniformBuffer)
                        {
                            Rendering::DescriptorSetUpdate u{};
                            u.binding = binding;
                            u.type = dtype;
                            u.buffers = {bb.Buf};
                            // Ring-backed bindings live at a nonzero offset
                            // inside the shared upload buffer.
                            if (bb.Offset != 0 || bb.Size != 0)
                            {
                                u.bufferOffsets = {static_cast<size_t>(bb.Offset)};
                                u.bufferRanges = {static_cast<size_t>(bb.Size)};
                            }
                            updates.push_back(std::move(u));
                        }
                    }
                }
                for (const auto& tb : texBinds)
                {
                    if (!tb.Tex.IsValid())
                        continue;
                    if (Detail::TryGetSet0BindingByName(*m_ShaderMeta, tb.Name, binding, dtype))
                    {
                        overrideTypeFromLayout(binding, dtype);
                        Rendering::DescriptorSetUpdate u{};
                        u.binding = binding;
                        u.type = dtype;
                        u.textures = {tb.Tex};
                        if (dtype == Rendering::DescriptorType::CombinedImageSampler &&
                            m_LinearClampSampler.IsValid())
                            u.samplers = {m_LinearClampSampler};
                        updates.push_back(std::move(u));
                    }
                }
                for (const auto& kv : m_Samplers)
                {
                    if (Detail::TryGetSet0BindingByName(*m_ShaderMeta, kv.first, binding, dtype))
                    {
                        overrideTypeFromLayout(binding, dtype);
                        if (dtype == Rendering::DescriptorType::Sampler &&
                            m_LinearClampSampler.IsValid())
                        {
                            Rendering::DescriptorSetUpdate u{};
                            u.binding = binding;
                            u.type = Rendering::DescriptorType::Sampler;
                            u.samplers = {m_LinearClampSampler};
                            updates.push_back(std::move(u));
                        }
                    }
                }
                if (targetBuf.IsValid())
                {
                    const std::string leaf = BindingLeafName(m_TargetBufferRef);
                    if (Detail::TryGetSet0BindingByName(*m_ShaderMeta, leaf, binding, dtype) ||
                        Detail::TryGetSet0BindingByName(*m_ShaderMeta, "TargetBuffer", binding,
                                                        dtype))
                    {
                        overrideTypeFromLayout(binding, dtype);
                        if (dtype == Rendering::DescriptorType::StorageBuffer)
                        {
                            Rendering::DescriptorSetUpdate u{};
                            u.binding = binding;
                            u.type = Rendering::DescriptorType::StorageBuffer;
                            u.buffers = {targetBuf};
                            updates.push_back(std::move(u));
                        }
                    }
                }
                if (targetTex.IsValid())
                {
                    const std::string leaf = BindingLeafName(m_TargetTextureRef);
                    if (Detail::TryGetSet0BindingByName(*m_ShaderMeta, leaf, binding, dtype) ||
                        Detail::TryGetSet0BindingByName(*m_ShaderMeta, "TargetTexture", binding,
                                                        dtype))
                    {
                        overrideTypeFromLayout(binding, dtype);
                        if (dtype == Rendering::DescriptorType::StorageImage)
                        {
                            Rendering::DescriptorSetUpdate u{};
                            u.binding = binding;
                            u.type = Rendering::DescriptorType::StorageImage;
                            u.textures = {targetTex};
                            updates.push_back(std::move(u));
                        }
                    }
                }

                if (!updates.empty())
                    device->UpdateDescriptorSetBatch(set0, updates);

                // Any reflected set0 binding the rendergraph didn't wire is left
                // unbound by the batch above — on a device without
                // nullDescriptor that is a VUID + undefined read. Default-fill
                // the remainder with safe placeholders (real binds above win).
                std::unordered_set<uint32_t> writtenBindings;
                writtenBindings.reserve(updates.size());
                for (const auto& u : updates)
                    writtenBindings.insert(u.binding);
                Detail::DefaultFillUnwrittenSet0Bindings(
                    device, set0, m_Set0Layout, *m_ShaderMeta, writtenBindings, defaultBuffer,
                    defaultTexture, m_LinearClampSampler, m_Id, m_LoggedDefaultedBindings);

                cl->BindDescriptorSet(0, set0, pipe);
            }

            cl->Dispatch(dx, dy, dz);
            if (m_RunOnce)
            {
                m_RunOnceDispatched = true;
                m_RunOnceTargetTex = targetTex;
            }
        });
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
