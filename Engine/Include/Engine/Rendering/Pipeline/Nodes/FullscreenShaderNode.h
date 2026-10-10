#pragma once

#include "Engine/Rendering/Pipeline/PipelineEnumTables.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <cstdint>
#include <memory>
#include <limits>
#include <string>
#include <unordered_map>

namespace GameEngine::Engine::Renderer { class RenderServices; }
#include <vector>

namespace GameEngine::Rendering
{
class IDevice;
class NamedPushConstantWriter;
struct PipelineDesc;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{
struct CubeLutGpuBindingState;
struct PostProcessSettings;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
namespace Detail
{
class FullscreenShaderNodeRenderer;
}

// Generic fullscreen graphics node.
//
// Minimal JSON shape (schemaVersion 2):
// {
//   "id": "Tonemap",
//   "type": "FullscreenShader",
//   "enabled": true,
//   "inputs": { "HDR": "SceneColor" },
//   "output": "View.Resolve",
//   "shaderPkg": "tonemap.shaderpkg",
//   "samplers": { "HDR": "LinearClamp", "NoiseVolume": "LinearRepeat" }
// }
class FullscreenShaderNode : public IRenderPipelineNode
{
  public:
    ~FullscreenShaderNode() override;
    const char* GetTypeName() const override { return "FullscreenShader"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& ctx) override;
    void DeclareForView(ViewDeclare& d) override;
    GameEngine::Rendering::GraphicsPipelineId GetBaseGraphicsPipelineId(GameEngine::Rendering::IDevice& /*device*/) const override
    {
        return m_BasePipelineId;
    }

  private:
    friend class Detail::FullscreenShaderNodeRenderer;

    // Shared blend-mode enum + string table (Pipeline::BlendMode) so the
    // compiler validates the same "blendMode" strings this node parses.
    using BlendMode = ::GameEngine::Engine::Renderer::Pipeline::BlendMode;

    void EnsureCachedResources(GameEngine::Rendering::IDevice* device);
    GameEngine::Rendering::SamplerHandle SamplerForPreset(const std::string& preset) const;

    static constexpr int32_t kMaxPingPongChainSegments = 4;

    // Shared fill for the auto-params UBO and the push-constant range: JSON
    // defaults first, then (under ppOverrides) the reflection-driven PP fields,
    // shaderAnimationTime, and CubeLUT state. Both paths MUST stay identical —
    // a source added to only one silently starves the other shader family.
    void WritePushConstantDefaults(GameEngine::Rendering::NamedPushConstantWriter& writer) const;
    void WriteReflectedPPFields(GameEngine::Rendering::NamedPushConstantWriter& writer,
                                const GameEngine::Rendering::BlockLayout& block,
                                const GameEngine::Engine::Renderer::PostProcessSettings& settings,
                                const GameEngine::Engine::Renderer::CubeLutGpuBindingState& cubeLut,
                                float shaderAnimationTime) const;
    // RenderGraph declare path: a skipped/declined stage threads the chain variable —
    // publish the output ref as the resolved passthrough INPUT (the
    // replacement for the old skip-pass output aliasing). Never resolves the
    // output (no dead pool alloc).
    void StitchThrough(ViewDeclare& d);

    std::string m_Id;
    std::string m_Json;

    std::string m_OutputRef = Names::View::Resolve;
    std::unordered_map<std::string, std::string> m_Inputs; // bindingName -> resourceRef
    // Inputs sampled only while a PostProcessSettings gate reads active. This stage
    // neither resolves nor reads a gated-off input and its binding samples the
    // engine's black texture; a skipped stage upstream that passes its input
    // through can still allocate that input (#2158).
    // JSON: "inputGates": { "uScattering": "bloomScatteringActive" }.
    std::unordered_map<std::string, std::string> m_InputGates; // bindingName -> gate name
    // Package/project texture assets bound directly by source-prefixed path.
    // JSON: "assetTextures": { "uLensDirt": "package-alias:/Textures/dirt.png" }.
    std::unordered_map<std::string, std::string> m_AssetTextures;
    std::unordered_map<std::string, std::string> m_InputUsages; // bindingName -> RGUsage string
    std::unordered_map<std::string, std::string> m_Buffers; // bindingName -> resourceRef
    std::unordered_map<std::string, std::string> m_BufferUsages; // bindingName -> RGUsage string

    std::string m_ShaderPkg;
    // Optional package whose mounted asset source authorizes this pass.
    // A disabled/unavailable package makes the node a passthrough.
    std::string m_RequiredPackage;
    std::vector<uint8_t> m_VsBytes;
    std::vector<uint8_t> m_FsBytes;

    // Per-input sampler preset. Unknown names fall back to LinearClamp.
    std::unordered_map<std::string, std::string> m_Samplers;

    // When true and blendMode is not specified, defaults to Alpha blending.
    bool m_AlphaBlend = false;
    BlendMode m_BlendMode = BlendMode::Disabled;
    // Pure-copy stages (FinalCopy): when the sole input is already
    // single-sample the copy adds nothing — the stage elides itself and
    // threads the chain variable instead (the old graph reached the same
    // end state by culling the readerless copy). JSON "elideWhenIdentity".
    bool m_ElideWhenIdentity = false;
    bool m_PPOverrides = false;

    // Cached GPU resources
    GameEngine::Rendering::IDevice* m_Device = nullptr;
    GameEngine::Engine::Renderer::RenderServices* m_RenderServices = nullptr;
    GameEngine::Rendering::SamplerHandle m_LinearClampSampler{};
    GameEngine::Rendering::SamplerHandle m_LinearRepeatSampler{};
    // Interned base pipeline (shaders + static state + meta-applied layouts).
    // Concrete (id, formatKey) handles are resolved at execute time via the
    // active pass context.
    GameEngine::Rendering::GraphicsPipelineId m_BasePipelineId{};
    GameEngine::Rendering::DescriptorSetLayoutDesc m_Set0Layout{};
    std::string m_PipelineDebugName;
    // Cached metadata for binding name->slot mapping.
    std::unique_ptr<GameEngine::Rendering::ShaderMeta> m_ShaderMeta;

    // Push constant defaults parsed from JSON "pushConstants" object.
    struct PushConstantEntry
    {
        std::string Name;
        enum class Type : uint8_t { Float, Int } ValueType;
        union { float F; int32_t I; } Value;
    };
    std::vector<PushConstantEntry> m_PushConstantDefaults;

    // Skip-pass gating: each entry pairs a PostProcessSettings field name with a
    // target value. When ALL listed fields are within epsilon of their targets,
    // the pass is treated as a no-op and skipped via SetActivationPredicate.
    // Downstream readers transparently see the input via RG skip-pass aliasing.
    // Parsed from JSON "skipWhen": { "fieldName": targetValue, ... }.
    struct SkipWhenField
    {
        std::string Name;
        float Value;
    };
    std::vector<SkipWhenField> m_SkipWhenFields;

    // Skip-pass aliasing disambiguation: when a pass has multiple
    // alias-compatible inputs and is skipped, the RG alias-map builder can't
    // pick which input downstream readers should resolve to. This names a
    // specific input binding (e.g. "uHDR" for BloomCombine which has both
    // "uBloom" and "uHDR" as same-format reads). Optional — single-input passes
    // auto-detect correctly. Parsed from JSON "passthroughInput".
    std::string m_PassthroughInputName;

    // Optional extra combined samplers resolved from RenderServices.
    // JSON: "serviceTextures": ["uLut3d", "uLut1d"] matching shader binding names.
    std::vector<std::string> m_ServiceTextureBindings;

    // Pre-built binding name arrays (populated once at Initialize, stable across frames).
    std::vector<std::string> m_InputNames;
    std::vector<std::string> m_BufferNames;

    // Blueprint "stitchWhenSkipped" (default true): whether a skipWhen-gated
    // skip threads this pass's input through under its output name. Threading
    // RESOLVES that input, which materializes it — so a pass whose output no
    // live consumer reads while the gate is closed sets this false to keep a
    // disabled effect from allocating its chain. Failure paths (missing
    // package / pipeline) always stitch: there the chain must not break.
    bool m_StitchWhenSkipped = true;

    // One-time validation logging to avoid per-frame spam.
    bool m_LoggedShaderPkgLoadFailure = false;
    bool m_LoggedMetaValidationIssues = false;
    bool m_LoggedMissingBindings = false;
    bool m_LoggedMissingBufferBindings = false;
    bool m_LoggedStitchIssue = false;
    bool m_LoggedUnresolvedInput = false;
    bool m_LoggedUnresolvedOutput = false;
    bool m_LoggedSelfReferentialOutput = false;
    bool m_LoggedMultisampledInput = false;
    bool m_LoggedDefaultedBindings = false;

    // Optional: run the fullscreen draw N times through ping-pong HDR targets.
    // Segment count comes from a PostProcessSettings field at declaration. JSON
    // "pingPongChain": true or { "sceneColorBinding": "uSceneColor" }.
    bool m_UsePingPongFullscreenChain = false;
    std::string m_PingPongSceneColorBinding = "uSceneColor";
    // PostProcessSettings field (JSON name, via TryReadField) that supplies the
    // segment count. JSON "pingPongChain": { "passCountField": "iterationCount" }.
    std::string m_PingPongPassCountField;
    bool m_LoggedPingPongMissingSceneInput = false;
    bool m_LoggedPingPongBadPassCountField = false;

};

// Fullscreen stage with persistent, per-view ping-pong history. Keeping the
// history map here avoids charging every ordinary fullscreen node for temporal
// state it can never use.
class TemporalFullscreenShaderNode final : public FullscreenShaderNode
{
  public:
    const char* GetTypeName() const override { return "TemporalFullscreenShader"; }
    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(ViewDeclare& d) override;

    // Whether the ping-pong history about to be read holds this view's
    // immediately preceding frame at the current extent and world.
    // historyFresh comes from ImportPersistentTexture: true means the pool
    // handed back a recycled/undefined physical, so nothing this view wrote
    // survives in it.
    static bool ComputeHistoryValid(uint64_t historyFrame, uint64_t lastWrittenFrame,
                                    uint64_t frameIndex, uint32_t prevWidth, uint32_t prevHeight,
                                    uint32_t width, uint32_t height, uint64_t prevWorldId,
                                    uint64_t worldId, bool historyFresh);

  private:
    struct HistoryState
    {
        uint64_t LastWrittenFrame = std::numeric_limits<uint64_t>::max();
        uint64_t HistoryFrame = 0;
        uint64_t WorldId = 0;
        uint32_t Width = 0;
        uint32_t Height = 0;
    };

    std::string m_HistoryBinding;
    std::string m_HistoryValidPushConstant;
    std::unordered_map<uint32_t, HistoryState> m_History;
    bool m_LoggedMissingHistoryBinding = false;
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
