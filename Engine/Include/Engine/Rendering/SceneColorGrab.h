#pragma once

#include "Rendering/Core/Device.h" // PipelineDesc, DescriptorSetLayoutDesc
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h"     // GraphicsPipelineId
#include "Rendering/Core/RenderGraph/RGFrame.h"     // RGTexture (frame-scoped grab id)

#include <cstdint>
#include <string>

namespace GameEngine::Rendering
{
class IDevice;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{
namespace Pipeline
{
struct ViewDeclare;
} // namespace Pipeline

// Snapshots a scene-colour render target into a sampleable, feature-owned texture so a
// later pass can refract / distort it. Generic over the SOURCE resource name so it is
// reusable: the StandardPBR transmission lobe grabs the post-world "SceneColor" (lit
// opaque) for the dedicated transmissive pass to refract; the ocean (later) can grab its
// pre-world "View.Color". The grab texture is an imported external: the consuming pass
// binds the physical descriptor-direct AND must declare an in-graph Read against the
// frame-scoped RGTexture this class exposes (GetGrabTextureRG) — that read is the
// copy->consumer RAW edge; without it ordering rides only the scheduler's
// declaration-order tie-break. The transmissive world pass plumbs it through
// WorldPassTargetsRG::SceneGrab.
//
// Phase-1 scope: a single fullscreen copy (Shaders/copy.shaderpkg). When the source is
// multisampled the grab DECLINES (not ready) so the consumer falls back to the env cube,
// rather than carrying an MSAA-resolve path (the ocean keeps its own resolve arm because
// water must always refract).
class SceneColorGrab
{
public:
    SceneColorGrab(std::string sourceResourceName, std::string debugName);
    ~SceneColorGrab();

    // Records the copy (+ settle) passes for this view. Returns true when a grab texture
    // is available to bind/read this frame (false declines: source missing or multisampled).
    bool DeclareForView(Pipeline::ViewDeclare& d, uint32_t frameIndex);

    // After an in-place device rebuild: the grab texture and the sampler died
    // with the old device. Forgets them without destroying them; the next
    // DeclareForView recreates both.
    void OnDeviceRebuilt();
    bool IsReady(uint32_t frameIndex) const
    {
        return m_GrabTexture.IsValid() && m_ReadyFrameIndex == frameIndex;
    }
    ::GameEngine::Rendering::TextureHandle GetGrabTexture() const { return m_GrabTexture; }
    // Frame-scoped render-graph id of this frame's grab import — valid only for the
    // frame DeclareForView last succeeded on. The consuming pass declares its
    // Read(Sampled) against this id (the copy->consumer RAW edge).
    ::GameEngine::Rendering::RenderGraph::RGTexture GetGrabTextureRG() const { return m_GrabRG; }
    ::GameEngine::Rendering::SamplerHandle GetSampler() const { return m_Sampler; }
    uint32_t GetFormat() const { return m_Format; }

private:
    // Lazily build the copy pipeline (stock copy.shaderpkg) + the linear-clamp sampler.
    // Returns false (declines) when the package is unavailable.
    bool EnsurePipeline(::GameEngine::Rendering::IDevice& device);

    // (Re)create the grab texture when the render-target size or format changes.
    bool EnsureTexture(::GameEngine::Rendering::IDevice& device, uint32_t width, uint32_t height);

    std::string m_SourceName;
    std::string m_DebugName;
    // Stable backing for the const char* desc/debug-label fields (composed once in the ctor
    // so the device's debug labels and the interned pipeline desc never dangle).
    std::string m_CopyLayoutName;
    std::string m_CopyPipelineName;
    std::string m_SamplerName;
    std::string m_SettleName;

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    ::GameEngine::Rendering::TextureHandle m_GrabTexture;
    // This frame's grab import id; reset at each DeclareForView (frame-scoped).
    ::GameEngine::Rendering::RenderGraph::RGTexture m_GrabRG{};
    ::GameEngine::Rendering::SamplerHandle m_Sampler;
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    uint32_t m_Format = 0; // matches the source colour format
    uint32_t m_ReadyFrameIndex = UINT32_MAX;

    ::GameEngine::Rendering::PipelineDesc m_CopyPipeline{};
    ::GameEngine::Rendering::GraphicsPipelineId m_CopyPipelineId{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_CopyLayout{};

    bool m_PipelineLoadAttempted = false;
};

} // namespace GameEngine::Engine::Renderer
