#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Core/RecomputeElision.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <array>

namespace GameEngine::Engine::Renderer { class RenderServices; }
#include <unordered_map>
#include <vector>

namespace GameEngine::Rendering
{
class IDevice;
struct PipelineDesc;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// Minimal compute node (MVP): dispatches a compute shader with optional output texture/buffer targets.
// JSON shape (schemaVersion 2):
// {
//   "id": "MyCompute",
//   "type": "ComputeShader",
//   "shaderPkg": "my_compute.shaderpkg",
//   "dispatch": { "x": 8, "y": 8, "z": 1 }, // also supports string expressions for x/y/z
//   "buffers": { "MySsbo": "SomeBufferRef" },
//   "bufferUsages": { "MySsbo": "StorageRead" }, // optional; defaults to StorageRead
    //   "uniformWrites": { "MyUbo": { "u32x4": [1,2,3,4] } },
    //   "postProcessUniformBuffers": ["PostFxParams"], // fill reflected UBO members from PostProcessSettings
//   "targetTexture": "SomeTexture",
//   "targetBuffer": "SomeBuffer",
//   "runOnce": true // optional: dispatch until it succeeds once, then keep the persistent target
// }
class ComputeShaderNode final : public IRenderPipelineNode
{
  public:
    ~ComputeShaderNode() override;
    const char* GetTypeName() const override { return "ComputeShader"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    void EnsureCachedPipeline(GameEngine::Rendering::IDevice* device);

    std::string m_Id;
    std::string m_Json;

    std::string m_ShaderPkg;
    std::string m_TargetTextureRef;
    // Blueprint "targetTextureReadWrite": the dispatch imageLoads its target
    // before storing (read-modify-write), so the graph must give the pass
    // ShaderRead visibility of the previous writer, not just write ordering.
    bool m_TargetTextureReadWrite = false;
    std::string m_TargetBufferRef;

    uint32_t m_DispatchX = 1;
    uint32_t m_DispatchY = 1;
    uint32_t m_DispatchZ = 1;
    std::string m_DispatchXExpr;
    std::string m_DispatchYExpr;
    std::string m_DispatchZExpr;
    // Effect gate, same contract as FullscreenShaderNode's: when every listed
    // resolved-settings field matches its target value the dispatch is not
    // declared at all, so a disabled effect costs no dispatch AND never
    // materializes this node's target resources. Parsed from JSON
    // "skipWhen": { "fieldName": targetValue, ... }.
    struct SkipWhenField
    {
        std::string Name;
        float Value;
    };
    std::vector<SkipWhenField> m_SkipWhenFields;

    bool m_RunOnce = false;
    bool m_RunOnceDispatched = false;
    uint64_t m_RunOnceDeclaredFrame = UINT64_MAX;
    // Physical texture the completed runOnce dispatch wrote. The done flag is
    // only honored while this texture is still alive: a pool recreation
    // (device rebuild, pipeline reload, project switch) discards the baked
    // content, so a changed handle re-arms the dispatch.
    Rendering::TextureHandle m_RunOnceTargetTex{};
    // Idle recompute elision (blueprint "idleElide": true — DepthMinMax,
    // ClusteredLightCull). Per-view gates over the node's complete dispatch
    // input set: unjittered logic-domain camera bytes + extent + engine
    // content/light/depth-dynamic epochs + persistent-binding physical
    // identities + UBO/push-constant content + dispatch dims. Upload-ring
    // binding OFFSETS are deliberately excluded (fresh alloc every frame);
    // their CONTENT rides the epochs or is appended directly.
    bool m_IdleElide = false;
    struct ViewElisionGate
    {
        GameEngine::Rendering::RecomputeElisionGate Gate;
        bool LogState = false;
    };
    std::unordered_map<uint32_t, ViewElisionGate> m_ElisionGates; // keyed by ViewId

    // Named resource bindings (bindingName -> resourceRef).
    // Bound via shaderdesc metadata (set0) when available.
    std::unordered_map<std::string, std::string> m_Inputs;   // textures (bindingName -> textureRef)
    std::unordered_map<std::string, std::string> m_Samplers; // samplers (bindingName -> sampler name/preset)
    std::unordered_map<std::string, std::string> m_InputUsageStrings;
    std::unordered_map<std::string, std::string> m_BufferBindings;
    std::unordered_map<std::string, std::string> m_BufferUsageStrings;
    std::unordered_map<std::string, std::array<uint32_t, 4>> m_UniformWritesU32x4;
    std::vector<std::string> m_PostProcessUniformBuffers;

    struct PushConstantEntry
    {
        std::string Name;
        enum class Type : uint8_t { Float, Int } ValueType;
        union { float F; int32_t I; } Value;
    };
    std::vector<PushConstantEntry> m_PushConstantDefaults;
    bool m_PPOverrides = false;

    // Cached GPU resources
    GameEngine::Rendering::IDevice* m_Device = nullptr;
    GameEngine::Engine::Renderer::RenderServices* m_RenderServices = nullptr;
    GameEngine::Rendering::SamplerHandle m_LinearClampSampler{};
    std::unique_ptr<GameEngine::Rendering::ShaderMeta> m_ShaderMeta;
    GameEngine::Rendering::ComputePipelineId m_PipelineId{};
    GameEngine::Rendering::DescriptorSetLayoutDesc m_Set0Layout;
    std::vector<uint8_t> m_CsBytes;
    std::string m_PipelineDebugName;

    bool m_RequiresLights = false;

    std::unordered_map<std::string, double> m_DispatchVars;

    // One-time validation logging to avoid per-frame spam.
    bool m_LoggedShaderPkgIssues = false;
    bool m_LoggedShaderPkgLoadFailure = false;
    bool m_LoggedDispatchExprFailure = false;
    bool m_LoggedBindingsMissingFromMeta = false;
    bool m_LoggedMissingTextureBindings = false;
    bool m_LoggedMissingSamplerBindings = false;
    bool m_LoggedDefaultedBindings = false;
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
