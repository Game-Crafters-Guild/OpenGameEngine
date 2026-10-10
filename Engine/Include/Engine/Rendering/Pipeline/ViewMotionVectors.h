#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Geometry/VertexAttributeFlags.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace GameEngine::Engine::Renderer::Pipeline
{
// The unwritten value of the motion target: a viewport-UV delta far outside
// any real one, with validity zero. Both consumers read it as "no exact motion
// here — reproject analytically". Mirrors GE_MOTION_VECTOR_SENTINEL
// (Shaders/Includes/motion_vector_payload.glsl) and taa_resolve.comp's
// kMVSentinel; whichever owner clears the target clears to this.
inline constexpr float kMotionVectorSentinelDelta = 100.0f;

// Shared by SSR and TAA. The first consumer records the movers pass and
// publishes the texture; subsequent consumers reuse the same frame resource.
// .rg is unjittered current-minus-previous UV, .b previous raw depth, .a validity.
// Unwritten pixels carry a large motion sentinel for analytic camera fallback.
class ViewMotionVectors
{
    using BufferHandle = Rendering::BufferHandle;
    using GraphicsPipelineId = Rendering::GraphicsPipelineId;
    using ShaderMeta = Rendering::ShaderMeta;
    using DescriptorSetLayoutDesc = Rendering::DescriptorSetLayoutDesc;
    using IDevice = Rendering::IDevice;
  public:
    Rendering::RenderGraph::RGTexture Declare(ViewDeclare& d);
    // How one frame mover's motion-vector draw must be issued.
    enum class MoverMotionPath : uint8_t
    {
        Rigid,   // no active palette: the instance transform is the whole motion
        Skinned, // dual-skinned draw, current + previous palettes
        Skip,    // palette-active but not exactly drawable this frame
    };

    // `vertexFlags` is the mesh bucket's core vertex layout, `paletteActive`
    // whether the instance has a live bone palette this frame, `hasSkinStreams`
    // whether the joint/weight vertex buffers are bound, `skinnedShaderAvailable`
    // whether the dual-skinned SPIR-V is staged.
    static MoverMotionPath ClassifyMoverMotionPath(Rendering::VertexAttributeFlags vertexFlags,
                                                   bool paletteActive, bool hasSkinStreams,
                                                   bool skinnedShaderAvailable);

  private:
    bool EnsureLoaded(IDevice* device);
    GraphicsPipelineId GetOrCreateMVPipeline(IDevice* device, uint32_t strideBytes, bool skinned);
    bool m_WarnedBindingMismatch = false;
    bool m_WarnedSkinnedBindingMismatch = false;
    // taa_motion_vectors — one graphics PSO per (core-vertex-stream stride,
    // skinned) pair. The position-only vertex input differs across mesh
    // buckets only by binding stride; the skinned variant adds the joint and
    // weight streams and a second (previous-frame) palette binding, so it is a
    // different shader and cannot share a PSO with the rigid one.
    std::vector<uint8_t> m_MVVertexSpv;
    std::vector<uint8_t> m_MVFragmentSpv;
    std::unique_ptr<ShaderMeta> m_MVMeta;
    DescriptorSetLayoutDesc m_MVSet0Layout{};
    std::vector<uint8_t> m_MVSkinnedVertexSpv;
    std::unique_ptr<ShaderMeta> m_MVSkinnedMeta;
    DescriptorSetLayoutDesc m_MVSkinnedSet0Layout{};
    bool m_WarnedSkinnedMVUnstaged = false;
    struct MVPipelineKey
    {
        uint32_t StrideBytes = 0;
        bool Skinned = false;

        bool operator==(const MVPipelineKey&) const = default;
    };
    struct MVPipelineKeyHash
    {
        std::size_t operator()(const MVPipelineKey& k) const
        {
            return std::hash<uint32_t>{}(k.StrideBytes) ^ (k.Skinned ? 0x9E3779B9u : 0u);
        }
    };
    std::unordered_map<MVPipelineKey, GraphicsPipelineId, MVPipelineKeyHash> m_MVPipelines;

};
} // namespace GameEngine::Engine::Renderer::Pipeline
