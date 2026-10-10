#pragma once

#include "Rendering/Core/Device.h" // PipelineDesc, DescriptorSetLayoutDesc
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h" // GraphicsPipelineId
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <cstdint>
#include <unordered_map>

namespace GameEngine::Rendering
{
class IDevice;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer::Pipeline
{
struct ViewDeclare;
} // namespace GameEngine::Engine::Renderer::Pipeline

namespace GameEngine::Ocean
{

// Snapshots the opaque scene colour into a sampleable texture so the ocean
// surface can refract it. The ocean draws into the world forward pass, so it
// cannot sample the colour target it renders into — this grab copies the
// post-opaque scene into a separate texture that the surface binds by name.
// Depth-fog
// transparency (the dominant cue) reads ge_sceneDepth, which DOES include the
// world-pass opaque entities via the depth prepass; the colour grab is the
// secondary distortion layer on top.
//
// Recorded by the post-world OceanRenderNode before the ocean surface pass. The grab is a fullscreen
// copy (mirroring the heat-haze scene snapshot): a render-graph pass reads the
// effective world colour target and writes this grab, so the render graph inserts the
// read->write->sample barriers. Under MSAA, View.Color is multisampled and is
// resolved by a sampler2DMS path before being stored in the single-sample grab.
//
// The grab texture is feature-owned (external to the render graph) and imported
// per frame, matching the FFT displacement pattern: a stable physical the world
// pass can bind descriptor-direct without a per-frame graph allocation.
class OceanSceneGrab
{
public:
    ~OceanSceneGrab();

    // Records the grab pass for this view when View.Color resolves. Returns true
    // when a grab texture is available to bind on the ocean draw this frame.
    bool DeclareForView(Engine::Renderer::Pipeline::ViewDeclare& d, uint32_t frameIndex,
                        ::GameEngine::Rendering::RenderGraph::RGTexture* outGraphTexture = nullptr);

    bool IsReady(uint32_t viewId, uint32_t frameIndex) const
    {
        auto it = m_GrabsByView.find(viewId);
        return it != m_GrabsByView.end() && it->second.Texture.IsValid() &&
               it->second.ReadyFrameIndex == frameIndex;
    }
    ::GameEngine::Rendering::TextureHandle GetGrabTexture(uint32_t viewId) const
    {
        auto it = m_GrabsByView.find(viewId);
        return it != m_GrabsByView.end() ? it->second.Texture : ::GameEngine::Rendering::TextureHandle{};
    }
    ::GameEngine::Rendering::SamplerHandle GetSampler() const { return m_Sampler; }
    // True when this frame's grab kept, per pixel, only the MSAA samples on the
    // resolved depth surface; the surface then filters refraction within it.
    bool IsDepthMatched(uint32_t viewId) const
    {
        auto it = m_GrabsByView.find(viewId);
        return it != m_GrabsByView.end() && it->second.DepthMatched;
    }

    // Same-frame sampled-consumer import (the PlanarReflection pattern): returns
    // this view's grab for a declared read, or an invalid handle unless the grab
    // was produced this frame. Dedups by physical against DeclareForView's import.
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportForSampling(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame, uint32_t viewId,
        uint32_t frameIndex) const;

private:
    // Lazily build the copy pipeline (stock copy.shaderpkg) + the linear-clamp
    // sampler. Returns false (declines) when the package is unavailable.
    bool EnsurePipeline(::GameEngine::Rendering::IDevice& device);

    // Lazily build the MSAA-resolve pipeline (runtime-compiled ocean_fullscreen.vert
    // + ocean_resolve.frag) used when View.Color is multisampled. Returns false if the
    // sources are unavailable.
    bool EnsureResolvePipeline(::GameEngine::Rendering::IDevice& device);

    // (Re)create the grab texture when the render target size changes.
    struct ViewGrab
    {
        ::GameEngine::Rendering::TextureHandle Texture;
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t Format = 0;
        uint32_t ReadyFrameIndex = UINT32_MAX;
        bool DepthMatched = false;
    };

    bool EnsureTexture(::GameEngine::Rendering::IDevice& device, ViewGrab& grab,
                       uint32_t width, uint32_t height, uint32_t format);

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;

    ::GameEngine::Rendering::SamplerHandle m_Sampler;
    std::unordered_map<uint32_t, ViewGrab> m_GrabsByView;

    ::GameEngine::Rendering::PipelineDesc m_CopyPipeline{};
    ::GameEngine::Rendering::GraphicsPipelineId m_CopyPipelineId{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_CopyLayout{};

    // MSAA-resolve pipeline (runtime-compiled): averages samples so the grab works
    // when View.Color is multisampled (used in place of the copy pipeline then).
    ::GameEngine::Rendering::PipelineDesc m_ResolvePipeline{};
    ::GameEngine::Rendering::GraphicsPipelineId m_ResolvePipelineId{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_ResolveLayout{};
    bool m_ResolveLoadAttempted = false;

    bool m_PipelineLoadAttempted = false;
    bool m_WarnedLoadFailed = false;
};

} // namespace GameEngine::Ocean
