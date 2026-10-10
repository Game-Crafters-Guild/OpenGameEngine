#include "Engine/Rendering/Pipeline/Nodes/FullscreenShaderNode.h"

#include "Core/Engine.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/Pipeline/Nodes/PipelineNodeUtils.h"
#include "Types/StringUtils.h"

#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/Passes/FinalizeContract.h"
#include "Rendering/Passes/TonemapPass.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Materials/ShaderMetaValidation.h"
#include "Types/StringId.h"

#include <algorithm>
#include <limits>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <nlohmann/json.hpp>
#include <unordered_set>

namespace GameEngine::Engine::Renderer { using namespace ::GameEngine::Rendering; }

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

namespace Detail
{

class FullscreenShaderNodeRenderer
{
  public:
    struct PassCustomization
    {
        RenderGraph::RGTexture Output{};
        std::vector<std::pair<std::string, RenderGraph::RGTexture>> SampledTextures;
        std::vector<std::pair<std::string, float>> FloatPushConstants;
    };

    using PassCustomizer = std::function<bool(
        ViewDeclare&, const RenderGraph::RGResourceDesc&, PassCustomization&)>;

    static void Declare(
        FullscreenShaderNode& node, ViewDeclare& d,
        const PassCustomizer& customizePass = {});
    static bool HasShaderTextureBinding(
        const FullscreenShaderNode& node, std::string_view binding);
    static const std::string& NodeId(const FullscreenShaderNode& node)
    {
        return node.m_Id;
    }
};

} // namespace Detail

namespace
{
// ~6e-4 tolerance: below any user-visible PP slider step, above
// PostProcessVolume float-blend noise.
constexpr float kSkipEpsilon = 1.0f / 1024.0f;

// A named gate reads 1.0 (active) or 0.0; a name the settings do not know reads
// as active, so a typo keeps the input bound rather than silently dropping it.
bool IsGateInactive(const PostProcessSettings& settings, const std::string& gate)
{
    float value = 1.0f;
    return settings.TryReadField(gate, value) && std::abs(value) <= kSkipEpsilon;
}
} // namespace

FullscreenShaderNode::~FullscreenShaderNode()
{
    if (m_Device)
    {
        if (m_LinearClampSampler.IsValid())
        {
            m_Device->DestroySampler(m_LinearClampSampler);
            m_LinearClampSampler = {};
        }
        if (m_LinearRepeatSampler.IsValid())
        {
            m_Device->DestroySampler(m_LinearRepeatSampler);
            m_LinearRepeatSampler = {};
        }
    }
    m_BasePipelineId = {};
    m_Set0Layout = {};
    m_ShaderMeta.reset();
    m_Device = nullptr;
}

bool FullscreenShaderNode::Initialize(std::string nodeId, std::string nodeJson, std::string* outError)
{
    m_Id = std::move(nodeId);
    m_Json = std::move(nodeJson);
    m_Inputs.clear();
    m_InputGates.clear();
    m_AssetTextures.clear();
    m_InputUsages.clear();
    m_Buffers.clear();
    m_BufferUsages.clear();
    m_Samplers.clear();
    m_OutputRef = Names::View::Resolve;
    m_ShaderPkg.clear();
    m_RequiredPackage.clear();
    m_VsBytes.clear();
    m_FsBytes.clear();
    m_BasePipelineId = {};
    m_Set0Layout = {};
    m_ShaderMeta.reset();
    m_PipelineDebugName.clear();
    m_AlphaBlend = false;
    m_BlendMode = BlendMode::Disabled;
    m_ElideWhenIdentity = false;
    m_PushConstantDefaults.clear();
    m_PPOverrides = false;
    m_SkipWhenFields.clear();
    m_PassthroughInputName.clear();
    m_ServiceTextureBindings.clear();
    m_InputNames.clear();
    m_BufferNames.clear();
    m_LoggedShaderPkgLoadFailure = false;
    m_LoggedMetaValidationIssues = false;
    m_LoggedMissingBindings = false;
    m_LoggedMissingBufferBindings = false;
    m_UsePingPongFullscreenChain = false;
    m_PingPongSceneColorBinding = "uSceneColor";
    m_PingPongPassCountField.clear();
    m_LoggedPingPongMissingSceneInput = false;
    m_LoggedPingPongBadPassCountField = false;

    try
    {
        const auto j = nlohmann::json::parse(m_Json);
        if (!j.is_object())
        {
            if (outError)
                *outError = "node JSON is not an object";
            return false;
        }

        if (j.contains("output") && j["output"].is_string())
            m_OutputRef = j["output"].get<std::string>();

        if (j.contains("inputs") && j["inputs"].is_object())
        {
            for (auto it = j["inputs"].begin(); it != j["inputs"].end(); ++it)
            {
                if (!it.value().is_string())
                    continue;
                m_Inputs[it.key()] = it.value().get<std::string>();
            }
        }
        if (j.contains("assetTextures") && j["assetTextures"].is_object())
        {
            for (auto it = j["assetTextures"].begin(); it != j["assetTextures"].end(); ++it)
            {
                if (it.value().is_string())
                    m_AssetTextures[it.key()] = it.value().get<std::string>();
            }
        }
        if (j.contains("inputUsages") && j["inputUsages"].is_object())
        {
            for (auto it = j["inputUsages"].begin(); it != j["inputUsages"].end(); ++it)
            {
                if (!it.value().is_string())
                    continue;
                m_InputUsages[it.key()] = it.value().get<std::string>();
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

        if (j.contains("buffers") && j["buffers"].is_object())
        {
            for (auto it = j["buffers"].begin(); it != j["buffers"].end(); ++it)
            {
                if (!it.value().is_string())
                    continue;
                m_Buffers[it.key()] = it.value().get<std::string>();
            }
        }
        if (j.contains("bufferUsages") && j["bufferUsages"].is_object())
        {
            for (auto it = j["bufferUsages"].begin(); it != j["bufferUsages"].end(); ++it)
            {
                if (!it.value().is_string())
                    continue;
                m_BufferUsages[it.key()] = it.value().get<std::string>();
            }
        }

        if (j.contains("alphaBlend") && j["alphaBlend"].is_boolean())
            m_AlphaBlend = j["alphaBlend"].get<bool>();

        if (j.contains("elideWhenIdentity") && j["elideWhenIdentity"].is_boolean())
            m_ElideWhenIdentity = j["elideWhenIdentity"].get<bool>();

        // Optional explicit blend mode (overrides alphaBlend).
        // "Disabled" (default), "Alpha", "Additive"
        if (j.contains("blendMode") && j["blendMode"].is_string())
        {
            const std::string v = j["blendMode"].get<std::string>();
            if (auto mode = BlendModeTable().Lookup(v))
                m_BlendMode = *mode;
            else
                WarnUnknownEnumOnce("BlendMode", v, BlendModeTable().ValidNames());
        }

        if (j.contains("shaderPkg") && j["shaderPkg"].is_string())
            m_ShaderPkg = j["shaderPkg"].get<std::string>();

        if (j.contains("requiresPackage"))
        {
            if (!j["requiresPackage"].is_string() || j["requiresPackage"].get<std::string>().empty())
            {
                if (outError)
                    *outError = "FullscreenShader node 'requiresPackage' must be a non-empty package name.";
                return false;
            }
            m_RequiredPackage = j["requiresPackage"].get<std::string>();
        }

        // Parse push constant defaults: "pushConstants": { "exposure": 1.0, "tonemapMode": 5 }
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
                    continue; // skip unsupported types
                }
                m_PushConstantDefaults.push_back(entry);
            }
        }

        if (j.contains("ppOverrides") && j["ppOverrides"].is_boolean())
            m_PPOverrides = j["ppOverrides"].get<bool>();

        // Ping-pong chain: "pingPongChain": true, or an object naming the
        // scene-color binding the chain rethreads between segments.
        if (j.contains("pingPongChain"))
        {
            const auto& pch = j["pingPongChain"];
            if (pch.is_boolean())
                m_UsePingPongFullscreenChain = pch.get<bool>();
            else if (pch.is_object())
            {
                m_UsePingPongFullscreenChain = true;
                if (pch.contains("sceneColorBinding") && pch["sceneColorBinding"].is_string())
                    m_PingPongSceneColorBinding = pch["sceneColorBinding"].get<std::string>();
                if (pch.contains("passCountField") && pch["passCountField"].is_string())
                    m_PingPongPassCountField = pch["passCountField"].get<std::string>();
            }
        }

        // Skip-pass gating. Pass is skipped when all named PP fields are within
        // epsilon of their target values. The gate reads PostProcessSettings
        // directly and is independent of ppOverrides — chain passes (Bloom*)
        // that don't ingest PP fields themselves can still be gated by the
        // upstream effect's amplitude (e.g. BloomBlur* gated on bloomIntensity).
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

        if (j.contains("inputGates") && j["inputGates"].is_object())
        {
            for (auto it = j["inputGates"].begin(); it != j["inputGates"].end(); ++it)
            {
                if (it.value().is_string())
                    m_InputGates.emplace(it.key(), it.value().get<std::string>());
            }
        }

        if (j.contains("stitchWhenSkipped") && j["stitchWhenSkipped"].is_boolean())
            m_StitchWhenSkipped = j["stitchWhenSkipped"].get<bool>();

        if (j.contains("passthroughInput") && j["passthroughInput"].is_string())
            m_PassthroughInputName = j["passthroughInput"].get<std::string>();

        if (j.contains("serviceTextures") && j["serviceTextures"].is_array())
        {
            for (const auto& v : j["serviceTextures"])
            {
                if (v.is_string())
                    m_ServiceTextureBindings.push_back(v.get<std::string>());
            }
        }

        if (m_ShaderPkg.empty())
        {
            if (outError)
                *outError = "FullscreenShader node requires 'shaderPkg' (.shaderpkg path/name).";
            return false;
        }
    }
    catch (const std::exception& e)
    {
        if (outError)
            *outError = std::string("JSON parse failed: ") + e.what();
        return false;
    }

    // Pre-build stable name arrays for zero-copy lambda captures.
    m_InputNames.reserve(m_Inputs.size());
    for (const auto& in : m_Inputs)
        m_InputNames.push_back(in.first);
    m_BufferNames.reserve(m_Buffers.size());
    for (const auto& in : m_Buffers)
        m_BufferNames.push_back(in.first);

    return true;
}

bool TemporalFullscreenShaderNode::Initialize(std::string nodeId, std::string nodeJson,
                                              std::string* outError)
{
    const std::string temporalJson = nodeJson;
    if (!FullscreenShaderNode::Initialize(std::move(nodeId), std::move(nodeJson), outError))
        return false;

    m_HistoryBinding.clear();
    m_HistoryValidPushConstant.clear();
    m_History.clear();
    try
    {
        const auto j = nlohmann::json::parse(temporalJson);
        if (!j.contains("temporalHistory") || !j["temporalHistory"].is_object())
        {
            if (outError)
                *outError = "TemporalFullscreenShader requires a 'temporalHistory' object.";
            return false;
        }

        const auto& history = j["temporalHistory"];
        if (history.contains("binding") && history["binding"].is_string())
            m_HistoryBinding = history["binding"].get<std::string>();
        if (history.contains("validPushConstant") &&
            history["validPushConstant"].is_string())
        {
            m_HistoryValidPushConstant =
                history["validPushConstant"].get<std::string>();
        }
        if (m_HistoryBinding.empty())
        {
            if (outError)
                *outError = "TemporalFullscreenShader temporalHistory requires 'binding'.";
            return false;
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

bool TemporalFullscreenShaderNode::ComputeHistoryValid(uint64_t historyFrame,
                                                       uint64_t lastWrittenFrame,
                                                       uint64_t frameIndex, uint32_t prevWidth,
                                                       uint32_t prevHeight, uint32_t width,
                                                       uint32_t height, uint64_t prevWorldId,
                                                       uint64_t worldId, bool historyFresh)
{
    // Frame-adjacent continuity is this node's rule: a feedback effect has no
    // reprojection to bridge a gap with. The pool's freshness answer is the
    // condition no counter can see — a same-name physical that no longer holds
    // what this view wrote (idle age-out, a resize or usage-widening realloc, a
    // device rebuild).
    const bool continuous = lastWrittenFrame != std::numeric_limits<uint64_t>::max() &&
                            lastWrittenFrame + 1ull == frameIndex;
    const bool extentStable = prevWidth == width && prevHeight == height;
    const bool worldStable = prevWorldId == worldId;
    return historyFrame > 0 && continuous && extentStable && worldStable && !historyFresh;
}

void TemporalFullscreenShaderNode::DeclareForView(ViewDeclare& d)
{
    Detail::FullscreenShaderNodeRenderer::Declare(
        *this, d,
        [this](ViewDeclare& d, const RenderGraph::RGResourceDesc& outputDesc,
               Detail::FullscreenShaderNodeRenderer::PassCustomization& customization)
        {
            if (outputDesc.SampleCount != 1)
            {
                if (!m_HistoryValidPushConstant.empty())
                    customization.FloatPushConstants.emplace_back(
                        m_HistoryValidPushConstant, 0.0f);
                return true;
            }

            if (!Detail::FullscreenShaderNodeRenderer::HasShaderTextureBinding(
                    *this, m_HistoryBinding) &&
                !m_LoggedMissingHistoryBinding)
            {
                m_LoggedMissingHistoryBinding = true;
                Logger::Log::Warning(
                    "RenderPipeline TemporalFullscreenShader '{}' shaderpkg is missing history "
                    "binding '{}'.",
                    Detail::FullscreenShaderNodeRenderer::NodeId(*this), m_HistoryBinding);
            }

            const uint32_t viewId = static_cast<uint32_t>(d.View.id);
            const uint64_t worldId = d.View.worldId;
            const uint64_t frameIndex = d.Frame.FrameIndex();
            auto& history = m_History[viewId];
            const uint32_t parity = static_cast<uint32_t>(history.HistoryFrame & 1ull);
            Rendering::TextureDesc historyDesc{};
            historyDesc.width = outputDesc.Width;
            historyDesc.height = outputDesc.Height;
            historyDesc.depth = 1;
            historyDesc.mipLevels = 1;
            historyDesc.arrayLayers = 1;
            historyDesc.sampleCount = 1;
            historyDesc.format = outputDesc.Format;
            historyDesc.usage = static_cast<uint32_t>(
                Rendering::TextureUsage::RenderTarget |
                Rendering::TextureUsage::ShaderResource);
            historyDesc.persistent = true;
            historyDesc.debugName = "Fullscreen.TemporalHistory";

            const std::string historyBase =
                "Pipeline.Fullscreen." + Detail::FullscreenShaderNodeRenderer::NodeId(*this) +
                ".History.View" +
                std::to_string(viewId);
            RenderGraph::RGTexture historyWrite = d.Frame.ImportPersistentTexture(
                (historyBase + "." + std::to_string(parity)).c_str(), historyDesc);
            bool historyReadFresh = false;
            RenderGraph::RGTexture historyRead = d.Frame.ImportPersistentTexture(
                (historyBase + "." + std::to_string(parity ^ 1u)).c_str(), historyDesc,
                &historyReadFresh);
            const bool historyValid = ComputeHistoryValid(
                history.HistoryFrame, history.LastWrittenFrame, frameIndex, history.Width,
                history.Height, outputDesc.Width, outputDesc.Height, history.WorldId, worldId,
                historyReadFresh);
            if (!historyRead.IsValid() || !historyWrite.IsValid())
            {
                if (!m_HistoryValidPushConstant.empty())
                    customization.FloatPushConstants.emplace_back(
                        m_HistoryValidPushConstant, 0.0f);
                history.LastWrittenFrame = std::numeric_limits<uint64_t>::max();
                history.HistoryFrame = 0;
            }
            else
            {
                if (!m_HistoryValidPushConstant.empty())
                {
                    customization.FloatPushConstants.emplace_back(
                        m_HistoryValidPushConstant, historyValid ? 1.0f : 0.0f);
                }
                customization.Output = historyWrite;
                customization.SampledTextures.emplace_back(m_HistoryBinding, historyRead);
                // The renderer declares the pass unconditionally once this
                // customization is accepted, and its fullscreen draw covers
                // every texel of this parity's history: discharge the pool's
                // freshness arm so the next frame's read of it is trusted.
                d.Frame.MarkPersistentTextureInitialized(historyWrite);
                history.LastWrittenFrame = frameIndex;
                history.HistoryFrame++;
            }

            history.WorldId = worldId;
            history.Width = outputDesc.Width;
            history.Height = outputDesc.Height;
            return true;
        });
}

void FullscreenShaderNode::EnsureCachedResources(GameEngine::Rendering::IDevice* device)
{
    if (!device)
        return;

    if (m_Device && m_Device != device)
    {
        // Device changed: best-effort cleanup on the old device, then rebuild.
        if (m_LinearClampSampler.IsValid())
        {
            m_Device->DestroySampler(m_LinearClampSampler);
            m_LinearClampSampler = {};
        }
        if (m_LinearRepeatSampler.IsValid())
        {
            m_Device->DestroySampler(m_LinearRepeatSampler);
            m_LinearRepeatSampler = {};
        }
        m_BasePipelineId = {};
        m_Set0Layout = {};
        m_ShaderMeta.reset();
    }
    m_Device = device;

    if (!m_LinearClampSampler.IsValid())
    {
        auto sd = GameEngine::Rendering::SamplerDesc::MaterialLinearClamp("Pipeline_FS_LinearClamp");
        m_LinearClampSampler = device->CreateSampler(sd);
    }
    if (!m_LinearRepeatSampler.IsValid())
    {
        auto sd = GameEngine::Rendering::SamplerDesc::MaterialLinearRepeat("Pipeline_FS_LinearRepeat");
        m_LinearRepeatSampler = device->CreateSampler(sd);
    }

    if (!m_BasePipelineId.IsValid())
    {
        // Build stable debug name storage so the interned desc keeps a
        // stable string view.
        m_PipelineDebugName = std::string("Pipeline.FullscreenShader.") + m_Id;

        if (m_VsBytes.empty() || m_FsBytes.empty() || !m_ShaderMeta)
        {
            GameEngine::Rendering::ShaderPackage pkg{};
            std::string loadErr;
            // Try pre-loaded cache first (populated by background thread during Init).
            bool loaded = m_RenderServices
                ? m_RenderServices->Spine().TryConsumePreLoadedShaderPackage(m_ShaderPkg, pkg)
                : false;
            if (!loaded && !GameEngine::Rendering::LoadShaderPkg(
                                m_ShaderPkg, device->PreferredShaderSource(), pkg, &loadErr))
            {
                if (!m_LoggedShaderPkgLoadFailure)
                {
                    m_LoggedShaderPkgLoadFailure = true;
                    Logger::Log::Warning("RenderPipeline FullscreenShader '{}': failed to load shaderpkg '{}': {}", m_Id, m_ShaderPkg, loadErr);
                }
            }
            else
            {
                auto itVs = pkg.stageBytes.find("vs");
                auto itFs = pkg.stageBytes.find("fs");
                if (itVs == pkg.stageBytes.end() || itVs->second.empty() || itFs == pkg.stageBytes.end() || itFs->second.empty())
                {
                    if (!m_LoggedShaderPkgLoadFailure)
                    {
                        m_LoggedShaderPkgLoadFailure = true;
                        Logger::Log::Warning("RenderPipeline FullscreenShader '{}': shaderpkg '{}' missing vs/fs stage bytes", m_Id, m_ShaderPkg);
                    }
                }
                else
                {
                    m_VsBytes = std::move(itVs->second);
                    m_FsBytes = std::move(itFs->second);
                    m_ShaderMeta = std::make_unique<GameEngine::Rendering::ShaderMeta>(std::move(pkg.meta));
                }
            }
        }

        if (m_VsBytes.empty() || m_FsBytes.empty())
        {
            // Shader bytes not available; don't create a pipeline.
            return;
        }

        GameEngine::Rendering::GraphicsPipelineDesc gd{};
        gd.Kind = GameEngine::Rendering::GraphicsPipelineKind::VertexFragment;
        gd.VertexShader = std::make_shared<const std::vector<uint8_t>>(m_VsBytes);
        gd.PixelShader  = std::make_shared<const std::vector<uint8_t>>(m_FsBytes);
        gd.DebugName = m_PipelineDebugName;

        // Fullscreen post defaults: no culling, no depth, dynamic viewport/scissor.
        gd.Rasterization.cullMode = GameEngine::Rendering::CullModeFlagBits::None;
        gd.DepthStencil.depthTestEnable = false;
        gd.DepthStencil.depthWriteEnable = false;
        GameEngine::Rendering::DynamicStateInfo dyn{};
        dyn.states = {GameEngine::Rendering::DynamicState::Viewport, GameEngine::Rendering::DynamicState::Scissor};
        gd.DynamicState = dyn;
        const BlendMode mode = (m_BlendMode != BlendMode::Disabled) ? m_BlendMode : (m_AlphaBlend ? BlendMode::Alpha : BlendMode::Disabled);
        if (mode == BlendMode::Alpha || mode == BlendMode::Additive)
        {
            GameEngine::Rendering::ColorBlendAttachmentState blend{};
            blend.blendEnable = true;
            if (mode == BlendMode::Alpha)
            {
                blend.srcColorBlendFactor = GameEngine::Rendering::BlendFactor::SrcAlpha;
                blend.dstColorBlendFactor = GameEngine::Rendering::BlendFactor::OneMinusSrcAlpha;
                blend.srcAlphaBlendFactor = GameEngine::Rendering::BlendFactor::SrcAlpha;
                blend.dstAlphaBlendFactor = GameEngine::Rendering::BlendFactor::OneMinusSrcAlpha;
            }
            else // Additive
            {
                blend.srcColorBlendFactor = GameEngine::Rendering::BlendFactor::One;
                blend.dstColorBlendFactor = GameEngine::Rendering::BlendFactor::One;
                blend.srcAlphaBlendFactor = GameEngine::Rendering::BlendFactor::One;
                blend.dstAlphaBlendFactor = GameEngine::Rendering::BlendFactor::One;
            }
            gd.ColorBlend.attachments = {blend};
        }

        if (m_ShaderMeta)
        {
            // Validate shader meta and emit issues once.
            if (!m_LoggedMetaValidationIssues)
            {
                const auto report = GameEngine::Rendering::ValidateShaderMeta(*m_ShaderMeta, 128);
                if (!report.Issues.empty())
                {
                    m_LoggedMetaValidationIssues = true;
                    for (const auto& iss : report.Issues)
                    {
                        const auto sev = static_cast<uint32_t>(iss.Severity);
                        if (iss.Severity == GameEngine::Rendering::IssueSeverity::Error)
                        {
                            Logger::Log::Error("RenderPipeline FullscreenShader '{}' meta [{}:{}]: {}", m_Id, sev, iss.Code, iss.Message);
                        }
                        else if (iss.Severity == GameEngine::Rendering::IssueSeverity::Warning)
                        {
                            Logger::Log::Warning("RenderPipeline FullscreenShader '{}' meta [{}:{}]: {}", m_Id, sev, iss.Code, iss.Message);
                        }
                        else
                        {
                            Logger::Log::Info("RenderPipeline FullscreenShader '{}' meta [{}:{}]: {}", m_Id, sev, iss.Code, iss.Message);
                        }
                    }
                }
            }

            // Apply meta to the typed desc: builds + interns set layouts and
            // populates push constants. The patch callback snapshots set-0
            // for draw-time descriptor allocation. DB-eligibility is now set
            // by the Vulkan backend at layout-create time (see
            // VulkanDevice.cpp:11908) so no per-callsite flip is needed.
            m_Set0Layout = Rendering::DescriptorSetLayoutDesc{};
            auto patchLayout = [this](uint32_t setIndex, GameEngine::Rendering::DescriptorSetLayoutDesc& dsl) {
                if (setIndex == 0)
                    m_Set0Layout = dsl;
            };

            std::string err;
            if (!GameEngine::Rendering::MaterialHelper::ApplyShaderMetaToGraphicsDesc(
                    *device,
                    *m_ShaderMeta,
                    gd,
                    GameEngine::Rendering::MaterialBuilder::MergeMode::Auto,
                    {true, 128},
                    patchLayout,
                    &err))
            {
                // Keep going; the backend may still reflect in dev builds.
                Logger::Log::Warning("RenderPipeline FullscreenShader '{}': failed to apply shader meta from shaderpkg '{}': {}", m_Id, m_ShaderPkg, err);
            }

            // Validate that declared node inputs exist in the shader's set0 bindings (by name).
            if (!m_LoggedMissingBindings && !m_Inputs.empty())
            {
                std::unordered_set<std::string> set0Names;
                for (const auto& s : m_ShaderMeta->Sets)
                {
                    if (s.Set != 0)
                        continue;
                    for (const auto& b : s.Bindings)
                    {
                        if (!b.Name.empty())
                            set0Names.insert(b.Name);
                    }
                    break;
                }
                std::vector<std::string> missing;
                for (const auto& in : m_Inputs)
                {
                    if (set0Names.find(in.first) == set0Names.end())
                    {
                        missing.push_back(in.first);
                    }
                }
                for (const auto& in : m_AssetTextures)
                {
                    if (set0Names.find(in.first) == set0Names.end())
                        missing.push_back(in.first);
                }
                if (!missing.empty())
                {
                    m_LoggedMissingBindings = true;
                    std::string list;
                    for (size_t i = 0; i < missing.size(); ++i)
                    {
                        if (i)
                            list += ", ";
                        list += missing[i];
                    }
                    Logger::Log::Warning(
                        "RenderPipeline FullscreenShader '{}' shaderpkg '{}' is missing bindings for inputs: {}",
                        m_Id,
                        m_ShaderPkg,
                        list);
                }
            }

            if (!m_LoggedMissingBufferBindings && !m_Buffers.empty())
            {
                std::unordered_set<std::string> set0Names;
                for (const auto& s : m_ShaderMeta->Sets)
                {
                    if (s.Set != 0)
                        continue;
                    for (const auto& b : s.Bindings)
                    {
                        if (!b.Name.empty())
                            set0Names.insert(b.Name);
                    }
                    break;
                }
                std::vector<std::string> missing;
                for (const auto& in : m_Buffers)
                {
                    if (set0Names.find(in.first) == set0Names.end())
                        missing.push_back(in.first);
                }
                if (!missing.empty())
                {
                    m_LoggedMissingBufferBindings = true;
                    std::string list;
                    for (size_t i = 0; i < missing.size(); ++i)
                    {
                        if (i)
                            list += ", ";
                        list += missing[i];
                    }
                    Logger::Log::Warning(
                        "RenderPipeline FullscreenShader '{}' shaderpkg '{}' is missing bindings for buffers: {}",
                        m_Id,
                        m_ShaderPkg,
                        list);
                }
            }
        }

        m_BasePipelineId = device->InternGraphicsPipeline(std::move(gd));
    }
}

GameEngine::Rendering::SamplerHandle
FullscreenShaderNode::SamplerForPreset(const std::string& preset) const
{
    const std::string p = ToLowerAscii(preset);
    if (p == "linearrepeat" || p == "linear_repeat" || p == "repeat" || p == "wrap")
        return m_LinearRepeatSampler.IsValid() ? m_LinearRepeatSampler : m_LinearClampSampler;
    return m_LinearClampSampler;
}

void FullscreenShaderNode::Declare(RenderPipelineInstance& instance,
                                   const PipelineDeclareContext& /*ctx*/)
{
    m_RenderServices = &instance.GetRenderServices();
    if (!m_RequiredPackage.empty() &&
        !m_RenderServices->IsPackageAvailable(m_RequiredPackage))
        return;
    EnsureCachedResources(m_RenderServices->GetDevice());
}

void FullscreenShaderNode::StitchThrough(ViewDeclare& d)
{
    std::string srcRef;
    if (!m_PassthroughInputName.empty())
    {
        if (auto it = m_Inputs.find(m_PassthroughInputName); it != m_Inputs.end())
            srcRef = it->second;
    }
    else if (m_Inputs.size() == 1)
    {
        // Every single-input stage passes its sole input through — the old
        // graph auto-detected exactly this (the generic implicit rule).
        srcRef = m_Inputs.begin()->second;
    }
    if (srcRef.empty())
    {
        if (!m_LoggedStitchIssue)
        {
            m_LoggedStitchIssue = true;
            Logger::Log::Warning(
                "RenderPipeline FullscreenShader '{}': skipped with {} inputs and no "
                "passthroughInput — output '{}' is not threaded this frame",
                m_Id, m_Inputs.size(), m_OutputRef);
        }
        return;
    }
    const RenderGraph::RGTexture src = d.ResolveTexture(srcRef);
    if (src.IsValid())
        d.PublishTexture(m_OutputRef, src);
}

void FullscreenShaderNode::WritePushConstantDefaults(
    Rendering::NamedPushConstantWriter& writer) const
{
    for (const auto& entry : m_PushConstantDefaults)
    {
        switch (entry.ValueType)
        {
        case PushConstantEntry::Type::Float:
            writer.Add(entry.Name, entry.Value.F);
            break;
        case PushConstantEntry::Type::Int:
            writer.Add(entry.Name, entry.Value.I);
            break;
        }
    }
}

void FullscreenShaderNode::WriteReflectedPPFields(Rendering::NamedPushConstantWriter& writer,
                                                  const Rendering::BlockLayout& block,
                                                  const PostProcessSettings& settings,
                                                  const CubeLutGpuBindingState& cubeLut,
                                                  float shaderAnimationTime) const
{
    if (!m_PPOverrides)
        return;
    for (const auto& member : block.Members)
    {
        // Any fullscreen shader can declare a member with this name to receive
        // the engine's shader animation time in seconds.
        if (member.Name == "shaderAnimationTime")
        {
            writer.Add(member.Name, shaderAnimationTime);
            continue;
        }
        if (settings.TryWriteField(member.Name, writer))
            continue;
        (void)TextureService::TryWriteCubeLutPushMember(member.Name, cubeLut, writer);
    }
}

void FullscreenShaderNode::DeclareForView(ViewDeclare& d)
{
    Detail::FullscreenShaderNodeRenderer::Declare(*this, d);
}

bool Detail::FullscreenShaderNodeRenderer::HasShaderTextureBinding(
    const FullscreenShaderNode& node, std::string_view binding)
{
    if (!node.m_ShaderMeta)
        return false;

    for (const auto& set : node.m_ShaderMeta->Sets)
    {
        if (set.Set != 0)
            continue;
        for (const auto& candidate : set.Bindings)
        {
            if (candidate.Name == binding)
                return true;
        }
        break;
    }
    return false;
}

void Detail::FullscreenShaderNodeRenderer::Declare(
    FullscreenShaderNode& node, ViewDeclare& d,
    const PassCustomizer& customizePass)
{
    auto* services = &d.Services;
    node.m_RenderServices = services;
    if (!node.m_RequiredPackage.empty() && !services->IsPackageAvailable(node.m_RequiredPackage))
    {
        node.StitchThrough(d);
        return;
    }
    node.EnsureCachedResources(services->GetDevice());

    if (!node.m_BasePipelineId.IsValid())
    {
        // Headless / shader unavailable (warn-once already emitted by
        // EnsureCachedResources): the chain must not break.
        node.StitchThrough(d);
        return;
    }

    const auto viewId = d.View.id;
    const uint64 worldId = d.View.worldId;

    // PP settings evaluated ONCE at declaration — the skip gate and the
    // exec-time push constants must agree (captured by value below).
    PostProcessSettings settings =
        services->GetEffectivePostProcessSettings(viewId, worldId);
    // A diagnostic view writes display-range values, not scene radiance, so it
    // reaches the display unexposed and untonemapped.
    if (node.m_Id == "Tonemap" && worldId != 0)
    {
        auto* shadows = services->GetFeature<ShadowMapRenderFeature>();
        if (shadows != nullptr && shadows->ShowsDiagnosticValues())
        {
            settings.Exposure = 1.0f;
            settings.AutoExposureActive = false;
            settings.TonemapMode = 5; // Linear: preserve the diagnostic values.
            settings.IctcpChromaCompression = 0.0f;
        }
    }
    // The same volume stack can feed views with different render heights, so the
    // per-view derived push fields (bloom + fog-glow pyramid scales) resolve here.
    settings.ResolveDerivedForRenderHeight(d.RenderHeight);

    // The old SetActivationPredicate, evaluated at declaration: skipped =
    // not declared (+ the chain variable threads through).
    if (!node.m_SkipWhenFields.empty())
    {
        bool skip = true;
        if (node.m_Id == "Tonemap")
        {
            // HDR display headroom is applied inside the pass body — in an
            // HDR output mode Tonemap must run even when its skipWhen fields
            // match (e.g. an explicit Linear volume), or the headroom is lost.
            if (auto* dev = services->GetDevice();
                dev && Rendering::IsHdrOutputModeActive(dev->GetActiveHdrOutputMode()))
                skip = false;
        }
        if (skip)
        {
            for (const auto& f : node.m_SkipWhenFields)
            {
                float v = 0.0f;
                if (!settings.TryReadField(f.Name, v) || std::abs(v - f.Value) > kSkipEpsilon)
                {
                    skip = false; // unknown field or live effect: run
                    break;
                }
            }
        }
        if (skip)
        {
            if (node.m_StitchWhenSkipped)
                node.StitchThrough(d);
            return;
        }
    }

    // Identity elision (FinalCopy): a pure single-input copy whose input is
    // already single-sample adds nothing — thread the chain variable. The
    // old graph reached the same end state by culling the copy once the
    // spine redirected View.Resolve away from it; here it is structural and
    // explicit. Never elides a blending stage or a multisampled input (the
    // copy IS the resolve fallback then).
    //
    // An extent mismatch is NOT an identity, whatever the formats say: under an
    // internal-resolution split the stage's input and output sit on opposite
    // sides of the crossing, and eliding would publish the internal-extent
    // texture under the display-extent name — a silent resolution downgrade of
    // everything downstream, or (when the output is the smaller of the two) a
    // downscale of the finished image. Running the stage performs the resample
    // the extents ask for.
    if (node.m_ElideWhenIdentity && node.m_Inputs.size() == 1 && !node.m_AlphaBlend &&
        node.m_BlendMode == FullscreenShaderNode::BlendMode::Disabled)
    {
        const RenderGraph::RGTexture in = d.ResolveTexture(node.m_Inputs.begin()->second);
        // The output is RESOLVED, not peeked: the extent that decides identity
        // is the one the stage would actually render into, and resolving is
        // idempotent within the frame (the stage resolves the same ref below).
        const RenderGraph::RGTexture out = d.ResolveTexture(node.m_OutputRef);
        if (in.IsValid() && d.Frame.Graph().ResourceDesc(in.Id).SampleCount == 1)
        {
            const auto& inDesc = d.Frame.Graph().ResourceDesc(in.Id);
            const bool sameExtent =
                !out.IsValid() || (d.Frame.Graph().ResourceDesc(out.Id).Width == inDesc.Width &&
                                   d.Frame.Graph().ResourceDesc(out.Id).Height == inDesc.Height);
            if (sameExtent)
            {
                d.PublishTexture(node.m_OutputRef, in);
                return;
            }
        }
    }

    // Inputs/buffers resolved AT DECLARATION; an unresolved ref declines the
    // stage (stitch keeps the chain alive). Physicals captured by value —
    // chain textures are pool imports, so they exist at declaration.
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
    texBinds.reserve(node.m_InputNames.size());
    std::vector<std::pair<RenderGraph::RGTexture, bool>> texEdges; // (texture, isWrite)
    for (const auto& name : node.m_InputNames)
    {
        if (auto gate = node.m_InputGates.find(name);
            gate != node.m_InputGates.end() && IsGateInactive(settings, gate->second))
        {
            texBinds.push_back({name, services->Textures().GetDefaultBlackTexture()});
            continue;
        }
        const RenderGraph::RGTexture t = d.ResolveTexture(node.m_Inputs[name]);
        if (!t.IsValid())
        {
            if (!node.m_LoggedUnresolvedInput)
            {
                node.m_LoggedUnresolvedInput = true;
                Logger::Log::Warning(
                    "RenderPipeline FullscreenShader '{}': input '{}' ref '{}' did not resolve "
                    "— stage skipped (chain threads through)",
                    node.m_Id, name, node.m_Inputs[name]);
            }
            node.StitchThrough(d);
            return;
        }
        bool isWrite = false;
        if (auto itU = node.m_InputUsages.find(name); itU != node.m_InputUsages.end())
            isWrite = (itU->second == "StorageWrite" || itU->second == "storageWrite");
        // A multisampled Sampled input cannot bind to the stage's sampler2D —
        // declaring would sample MSAA memory through a non-MS descriptor
        // (undefined). Decline + stitch keeps the chain alive and diagnosable;
        // a real resolve-capable fallback is the follow-up.
        if (!isWrite && d.Frame.Graph().ResourceDesc(t.Id).SampleCount > 1)
        {
            if (!node.m_LoggedMultisampledInput)
            {
                node.m_LoggedMultisampledInput = true;
                Logger::Log::Warning(
                    "RenderPipeline FullscreenShader '{}': input '{}' is multisampled — "
                    "stage skipped (chain threads through; no resolve-capable fallback yet)",
                    node.m_Id, name);
            }
            node.StitchThrough(d);
            return;
        }
        texEdges.push_back({t, isWrite});
        texBinds.push_back({name, d.Frame.PhysicalTexture(t)});
    }

    // Asset textures are ordinary descriptors, not render-graph resources.
    // Resolve them after package availability gating so a disabled package
    // never probes its unmounted source.
    GameEngine::AssetManager* assets = GameEngine::EngineCore::GetInstance().TryGetAssetManager();
    if (!node.m_AssetTextures.empty() && assets)
    {
        for (const auto& [name, path] : node.m_AssetTextures)
        {
            const GUID guid = assets->ResolveAssetGuid(path);
            const Rendering::TextureHandle texture = services->Textures().GetOrUpload(guid);
            if (texture.IsValid())
                texBinds.push_back({name, texture});
        }
    }

    std::vector<BufBind> bufBinds;
    bufBinds.reserve(node.m_BufferNames.size());
    std::vector<std::pair<RenderGraph::RGBuffer, bool>> bufEdges; // (buffer, isWrite)
    for (const auto& name : node.m_BufferNames)
    {
        const PipelineBufferBindingRG b = d.ResolveBuffer(node.m_Buffers[name]);
        if (!b.IsValid())
        {
            if (!node.m_LoggedMissingBufferBindings)
            {
                node.m_LoggedMissingBufferBindings = true;
                Logger::Log::Warning(
                    "RenderPipeline FullscreenShader '{}': buffer '{}' ref '{}' did not resolve "
                    "— stage skipped (chain threads through)",
                    node.m_Id, name, node.m_Buffers[name]);
            }
            node.StitchThrough(d);
            return;
        }
        if (b.Graph.IsValid())
        {
            bool isWrite = false;
            if (auto itU = node.m_BufferUsages.find(name); itU != node.m_BufferUsages.end())
                isWrite = (itU->second == "StorageWrite" || itU->second == "storageWrite");
            bufEdges.push_back({b.Graph, isWrite});
        }
        bufBinds.push_back({name, b.Buffer, b.Offset, b.Size});
    }

    // Output resolved LAST: a declined stage never materializes a pool
    // texture nothing writes. (The old presentation-target/backbuffer
    // fallback dies — the frame driver owns the backbuffer.)
    const RenderGraph::RGTexture out = d.ResolveTexture(node.m_OutputRef);
    if (!out.IsValid())
    {
        if (!node.m_LoggedUnresolvedOutput)
        {
            node.m_LoggedUnresolvedOutput = true;
            Logger::Log::Warning(
                "RenderPipeline FullscreenShader '{}': output ref '{}' did not resolve — "
                "stage skipped (chain threads through)",
                node.m_Id, node.m_OutputRef);
        }
        node.StitchThrough(d);
        return;
    }
    // A stage sampling its own color attachment is a Vulkan feedback loop the
    // graph would silently merge (the combined access takes the write layout)
    // — reachable when chain threading desynchronizes a ping-pong pair.
    // Decline + stitch; only Sampled (non-write) inputs participate.
    for (const auto& [t, isWrite] : texEdges)
    {
        if (isWrite || t.Id != out.Id)
            continue;
        if (!node.m_LoggedSelfReferentialOutput)
        {
            node.m_LoggedSelfReferentialOutput = true;
            Logger::Log::Warning(
                "RenderPipeline FullscreenShader '{}': input and output resolve to the same "
                "texture (feedback loop) — stage skipped (chain threads through)",
                node.m_Id);
        }
        node.StitchThrough(d);
        return;
    }
    // Desc snapshots BY VALUE — references into the resource table don't
    // survive later resolves (the materializer can grow it).
    const RenderGraph::RGResourceDesc od = d.Frame.Graph().ResourceDesc(out.Id);

    PassCustomization customization{};
    if (customizePass && !customizePass(d, od, customization))
    {
        node.StitchThrough(d);
        return;
    }
    for (const auto& [binding, texture] : customization.SampledTextures)
    {
        if (!texture.IsValid())
            continue;
        texEdges.push_back({texture, false});
        texBinds.push_back({binding, d.Frame.PhysicalTexture(texture)});
    }
    const RenderGraph::RGTexture passOutput =
        customization.Output.IsValid() ? customization.Output : out;

    // MSAA resolve pair: only when targeting the multisampled View.Color
    // with a single-sample, format-matched, distinct resolve (VUID-06865).
    RenderGraph::RGTexture resolvePair{};
    if (out.Id == d.ViewColor.Id && d.ViewResolve.IsValid() && d.ViewResolve.Id != out.Id &&
        od.SampleCount > 1)
    {
        const RenderGraph::RGResourceDesc rd = d.Frame.Graph().ResourceDesc(d.ViewResolve.Id);
        if (rd.SampleCount == 1 && rd.Format == od.Format)
        {
            resolvePair = d.ViewResolve;
        }
        else
        {
            static std::atomic<int> sResolveWarnBudget{8};
            if (sResolveWarnBudget.fetch_sub(1, std::memory_order_relaxed) > 0)
                Logger::Log::Warning(
                    "RenderPipeline FullscreenShader '{}': skipping resolve pair due to an "
                    "invalid resolve target (colorSamples={}, resolveSamples={}, colorFmt={}, "
                    "resolveFmt={}).",
                    node.m_Id, od.SampleCount, rd.SampleCount, od.Format, rd.Format);
        }
    }

    const bool blendEnabled = (node.m_BlendMode != FullscreenShaderNode::BlendMode::Disabled) || node.m_AlphaBlend;
    const uint32_t outW = od.Width;
    const uint32_t outH = od.Height;
    const float shaderAnimationTime = services->GetShaderAnimationTimeSeconds();

    // CubeLUT GPU binding captured at declaration (identity fallbacks).
    CubeLutGpuBindingState cubeLut{};
    if (node.m_PPOverrides)
        (void)services->Textures().TryGetCubeLutGpuBinding(
            services->GetEffectivePostProcessSettings(viewId, worldId), cubeLut);

    Rendering::TextureHandle bloomLensDirt{};
    {
        GUID::Data raw{};
        static_assert(sizeof(raw) == sizeof(settings.BloomLensDirtAssetGuidWords));
        std::memcpy(raw.data(), settings.BloomLensDirtAssetGuidWords,
                    sizeof(settings.BloomLensDirtAssetGuidWords));
        const GUID guid(raw);
        if (!guid.IsNull())
            bloomLensDirt = services->Textures().GetOrUpload(guid);
    }

    // Ping-pong chain: run the same fullscreen draw N times, rethreading the
    // scene-color binding through two transient targets (iterative effects,
    // e.g. an iterative fullscreen filter). Segment count comes from the DECLARATION-captured
    // settings, so only live segments are declared — no zeroed tail passes.
    int32_t chainCount = 1;
    size_t sceneColorIdx = std::numeric_limits<size_t>::max();
    if (node.m_UsePingPongFullscreenChain)
    {
        for (size_t i = 0; i < node.m_InputNames.size(); ++i)
        {
            if (node.m_InputNames[i] == node.m_PingPongSceneColorBinding)
            {
                sceneColorIdx = i;
                break;
            }
        }
        if (sceneColorIdx == std::numeric_limits<size_t>::max())
        {
            if (!node.m_LoggedPingPongMissingSceneInput)
            {
                node.m_LoggedPingPongMissingSceneInput = true;
                Logger::Log::Warning(
                    "RenderPipeline FullscreenShader '{}': pingPongChain enabled but no input "
                    "binding named '{}' (check node 'inputs') — running a single pass",
                    node.m_Id, node.m_PingPongSceneColorBinding);
            }
        }
        else
        {
            float passCount = 1.0f;
            if (!node.m_PingPongPassCountField.empty() &&
                settings.TryReadField(node.m_PingPongPassCountField, passCount))
            {
                chainCount = std::clamp(static_cast<int32_t>(passCount + 0.5f), 1,
                                        FullscreenShaderNode::kMaxPingPongChainSegments);
            }
            else if (!node.m_LoggedPingPongBadPassCountField)
            {
                node.m_LoggedPingPongBadPassCountField = true;
                Logger::Log::Warning(
                    "RenderPipeline FullscreenShader '{}': pingPongChain has no resolvable "
                    "'passCountField' ('{}') — running a single pass",
                    node.m_Id, node.m_PingPongPassCountField);
            }
        }
    }

    RenderGraph::RGTexture ping[2]{};
    if (chainCount > 1)
    {
        Rendering::TextureDesc pdesc{};
        pdesc.width = od.Width;
        pdesc.height = od.Height;
        pdesc.depth = 1;
        pdesc.mipLevels = 1;
        pdesc.arrayLayers = 1;
        pdesc.sampleCount = 1;
        pdesc.format = od.Format;
        pdesc.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget |
                                            Rendering::TextureUsage::ShaderResource);
        const std::string pingAName = d.PassName(".PingA");
        const std::string pingBName = d.PassName(".PingB");
        pdesc.debugName = pingAName.c_str();
        ping[0] = d.Frame.CreateTexture(pingAName.c_str(), pdesc);
        pdesc.debugName = pingBName.c_str();
        ping[1] = d.Frame.CreateTexture(pingBName.c_str(), pdesc);
        if (!ping[0].IsValid() || !ping[1].IsValid())
            chainCount = 1;
    }

    static const char* const kSegSuffix[FullscreenShaderNode::kMaxPingPongChainSegments] = {"", ".Pp1", ".Pp2", ".Pp3"};

    for (int32_t seg = 0; seg < chainCount; ++seg)
    {
        const bool finalSeg = (seg == chainCount - 1);
        // Segment source/target: scene -> A -> B -> A -> ... -> out.
        const RenderGraph::RGTexture segSrc = (seg == 0) ? RenderGraph::RGTexture{} : ping[(seg - 1) & 1];
        const RenderGraph::RGTexture finalOut = passOutput;
        const RenderGraph::RGTexture segOut = finalSeg ? finalOut : ping[seg & 1];

        std::vector<TexBind> segTexBinds = texBinds;
        if (seg > 0)
            segTexBinds[sceneColorIdx].Tex = d.Frame.PhysicalTexture(segSrc);

        d.Frame.AddPass(
            d.PassName(kSegSuffix[seg]).c_str(), Rendering::PassPhase::kPostProcess,
            [&](RenderGraph::RGPassBuilder& p)
            {
                for (size_t i = 0; i < texEdges.size(); ++i)
                {
                    auto [t, isWrite] = texEdges[i];
                    if (seg > 0 && i == sceneColorIdx)
                        t = segSrc;
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

                RenderGraph::RGAttachmentOps ops{};
                // Full-screen overwrite: DontCare unless blending reads the dst.
                // Deliberate change from the old unconditional Load (a Load here
                // would also derive a read that keeps dead producers alive) —
                // flagged for the gate-1 visual pass. Non-final chain segments
                // always fully overwrite their ping target.
                ops.Load = (blendEnabled && finalSeg) ? RenderGraph::RGLoadOp::Load
                                                      : RenderGraph::RGLoadOp::DontCare;
                ops.Store = RenderGraph::RGStoreOp::Store;
                if (finalSeg && resolvePair.IsValid())
                    p.AttachColorResolve(0, segOut, resolvePair, ops);
                else
                    p.AttachColor(0, segOut, ops);
            },
            [&node, texBinds = std::move(segTexBinds), bufBinds, settings, cubeLut, bloomLensDirt,
             shaderAnimationTime, floatPushConstants = customization.FloatPushConstants,
             outW, outH](
                RenderGraph::RGContext& ctx)
            {
                auto* cl = ctx.Cmd;
                auto* device = ctx.GetDevice();
                if (!cl || !device || !node.m_BasePipelineId.IsValid())
                    return;
                const Rendering::PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(node.m_BasePipelineId);
                if (!pipe.IsValid())
                    return;
                cl->SetPipeline(pipe);

                // Push constants: JSON defaults, the Tonemap outEncoding block
                // (verbatim — the single-terminal-OETF invariant), then the
                // reflection-driven PP fields from the DECLARATION-captured
                // settings (skip gate and shader inputs always agree).
                if (node.m_ShaderMeta && !node.m_ShaderMeta->PushConstants.empty() &&
                    (!node.m_PushConstantDefaults.empty() || node.m_PPOverrides))
                {
                    Rendering::NamedPushConstantWriter pcw(*node.m_ShaderMeta,
                                                           node.m_ShaderMeta->PushConstants[0].Name);
                    if (pcw.IsValid())
                    {
                        node.WritePushConstantDefaults(pcw);

                        if (node.m_Id == "Tonemap")
                        {
                            // The per-view Tonemap only maps scene HDR ->
                            // display-referred LINEAR; it never applies the output
                            // transfer function. Exactly one terminal pass owns
                            // the encode (FinalSRGBEncode or the hardware sRGB
                            // swapchain). This node's source is the pipeline's
                            // scene-HDR chain, upstream of any UI blend, so its
                            // input space is linear in every output config.
                            pcw.Add("outEncoding",
                                    Passes::SelectTonemapOutEncoding(
                                        device, Passes::FinalizeInputSpace::Linear));
                            const auto hdrState = device->GetHdrOutputState();
                            const auto& hdrMetadata = hdrState.staticMetadata;
                            const float outputMaxLinear =
                                Rendering::GetHdrOutputMaxLinearValue(hdrState);
                            pcw.Add("paperWhiteNits", hdrMetadata.paperWhiteNits);
                            pcw.Add("maxOutputNits",
                                    std::max(hdrMetadata.paperWhiteNits,
                                             hdrMetadata.paperWhiteNits * outputMaxLinear));
                        }

                        node.WriteReflectedPPFields(pcw, node.m_ShaderMeta->PushConstants[0].Block, settings,
                                               cubeLut, shaderAnimationTime);
                        if (node.m_Id == "Tonemap")
                        {
                            const HdrOutputMode mode = device->GetActiveHdrOutputMode();
                            const bool ictcpOutput = mode == HdrOutputMode::HDR10_PQ ||
                                                     mode == HdrOutputMode::HDR10Plus;
                            pcw.Add("ictcpChromaCompression",
                                    ictcpOutput
                                        ? std::clamp(settings.IctcpChromaCompression, 0.0f, 1.0f)
                                        : 0.0f);
                        }
                        for (const auto& [name, value] : floatPushConstants)
                        {
                            pcw.Add(name, value);
                        }

                        pcw.Flush(cl);
                    }
                }

                // Viewport from the DECLARED output extent (the old swapchain /
                // 1280x720 fallback chain dies — the desc is always known here).
                cl->SetViewport(0.0f, 0.0f, (float)outW, (float)outH);
                cl->SetScissor(0, 0, (int)outW, (int)outH);

                if (node.m_ShaderMeta && !node.m_Set0Layout.bindings.empty())
                {
                    Rendering::DescriptorSetDesc ds0{};
                    ds0.layout = node.m_Set0Layout;
                    ds0.transient = true;
                    ds0.debugName = "Pipeline.FullscreenShader.Set0";
                    auto set0 = device->CreateDescriptorSet(ds0);

                    // Track which reflected set0 bindings we actually write so the
                    // unwired remainder can be safely defaulted below.
                    std::unordered_set<uint32_t> writtenBindings;

                    uint32_t binding = 0;
                    Rendering::DescriptorType dtype{};
                    for (const auto& bb : bufBinds)
                    {
                        if (!bb.Buf.IsValid())
                            continue;
                        if (Detail::TryGetSet0BindingByName(*node.m_ShaderMeta, bb.Name, binding, dtype))
                        {
                            if (dtype == Rendering::DescriptorType::StorageBuffer ||
                                dtype == Rendering::DescriptorType::UniformBuffer)
                            {
                                Rendering::DescriptorSetUpdate u{};
                                u.binding = binding;
                                u.type = dtype;
                                u.buffers = {bb.Buf};
                                // Ring-backed bindings live at a nonzero offset
                                // inside the shared upload buffer — the old
                                // whole-buffer update would bind the wrong data.
                                if (bb.Offset != 0 || bb.Size != 0)
                                {
                                    u.bufferOffsets = {static_cast<size_t>(bb.Offset)};
                                    u.bufferRanges = {static_cast<size_t>(bb.Size)};
                                }
                                device->UpdateDescriptorSet(set0, u);
                                writtenBindings.insert(binding);
                            }
                        }
                    }

                    Rendering::NamedDescriptorWriter wdesc(device, set0, *node.m_ShaderMeta, 0);
                    for (const auto& tb : texBinds)
                    {
                        if (!tb.Tex.IsValid() || !wdesc.Has(tb.Name))
                            continue;
                        std::string samplerPreset;
                        if (auto it = node.m_Samplers.find(tb.Name); it != node.m_Samplers.end())
                            samplerPreset = it->second;
                        const auto sampler = node.SamplerForPreset(samplerPreset);
                        if (sampler.IsValid())
                        {
                            wdesc.AddCombinedImageSampler(tb.Name, tb.Tex, sampler);
                            if (Detail::TryGetSet0BindingByName(*node.m_ShaderMeta, tb.Name, binding, dtype))
                                writtenBindings.insert(binding);
                        }
                    }
                    for (const auto& svc : node.m_ServiceTextureBindings)
                    {
                        if (!wdesc.Has(svc))
                            continue;
                        Rendering::TextureHandle svcTex{};
                        if (svc == "uLut3d")
                            svcTex = cubeLut.Lut3D;
                        else if (svc == "uLut1d")
                            svcTex = cubeLut.Lut1DStrip;
                        else if (svc == "uLensDirt")
                            svcTex = bloomLensDirt;
                        if (svcTex.IsValid())
                        {
                            wdesc.AddCombinedImageSampler(svc, svcTex, node.m_LinearClampSampler);
                            if (Detail::TryGetSet0BindingByName(*node.m_ShaderMeta, svc, binding, dtype))
                                writtenBindings.insert(binding);
                        }
                    }
                    // tonemap.frag's ACES 2 tier tables: engine-owned static
                    // data (set 0, binding 2), not graph-wired — bound whenever
                    // the shader declares the block (the Tonemap node, and any
                    // shader including tonemap_aces2.glsl).
                    if (wdesc.Has("Aces2Tables"))
                    {
                        const Rendering::BufferHandle aces2Tables =
                            Rendering::Passes::GetAces2TablesBuffer(device);
                        if (aces2Tables.IsValid())
                        {
                            wdesc.AddStorageBuffer("Aces2Tables", aces2Tables, 0,
                                                   Rendering::Passes::GetAces2TablesBufferBytes());
                            if (Detail::TryGetSet0BindingByName(*node.m_ShaderMeta, "Aces2Tables", binding, dtype))
                                writtenBindings.insert(binding);
                        }
                    }
                    wdesc.Flush();

                    // Any reflected set0 binding the rendergraph didn't wire is left
                    // unbound by the writes above — on a device without
                    // nullDescriptor that is a VUID + undefined read. Default-fill
                    // the remainder with safe placeholders (real binds above win).
                    const Rendering::BufferHandle defaultBuffer =
                        node.m_RenderServices ? node.m_RenderServices->GetDefaultPlaceholderBuffer()
                                         : Rendering::BufferHandle{};
                    const Rendering::TextureHandle defaultTexture =
                        node.m_RenderServices ? node.m_RenderServices->Textures().GetDefaultBlackTexture()
                                         : Rendering::TextureHandle{};
                    Detail::DefaultFillUnwrittenSet0Bindings(
                        device, set0, node.m_Set0Layout, *node.m_ShaderMeta, writtenBindings, defaultBuffer,
                        defaultTexture, node.m_LinearClampSampler, node.m_Id, node.m_LoggedDefaultedBindings);

                    cl->BindDescriptorSet(0, set0, pipe);
                }

                cl->Draw(3, 1);
            });
    }

    // The chain variable: downstream resolvers of the output ref see THIS
    // stage's output from here on.
    d.PublishTexture(node.m_OutputRef, passOutput);
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
