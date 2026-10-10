#pragma once

#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <cstdint>

namespace GameEngine::Rendering
{
class IDevice;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{
class RenderServices;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer::Pipeline
{
struct ViewDeclare;
} // namespace GameEngine::Engine::Renderer::Pipeline

namespace GameEngine::Ocean
{

// Renders the opaque world a second time, reflected across the sea plane, into a
// scaled colour texture the ocean surface samples for mirror-like reflections
// (the reference's planar OceanPlanarReflection + _ReflectionTex).
//
// Unlike the other ocean sims (compute passes), this re-renders scene GEOMETRY,
// so it reuses the engine's multi-view machinery instead of a bespoke shader:
//   - A persistent secondary view + camera, allocated once. The view is marked
//     activeRenderPipeline=false (the main pipeline skips it) but keeps the main
//     view's render-layer mask + world id, so render extraction STILL submits the
//     whole scene to it (extraction gates on renderLayerMask directly, not on the
//     pipeline-active flag). We then drive its world pass explicitly, mirroring
//     the editor thumbnail path.
//   - Each frame: mirror the main camera across y = SeaLevel (a Householder
//     reflection), build the secondary view's draw stream (lights, batch keys,
//     bucketer), and AddWorldPassForView into a feature-owned scaled target.
//   - The mirror matrix flips triangle winding; the view is flagged
//     SetViewWorldPassFlipY so the world pass inverts the viewport Y to keep the
//     baked front face correct. The image lands vertically mirrored, which the
//     surface compensates for by flipping V when sampling.
//
// The colour target is feature-owned (a stable physical) so the forward
// contributor can bind it descriptor-direct on the main surface draw, exactly
// like OceanSceneGrab's refraction grab; a settle pass returns it to
// ShaderResource each frame. Degrades cleanly: any decline (no camera, the
// bucketer can't schedule this frame) leaves the surface on the procedural sky
// dome (the PlanarReflectionAvailable gate stays 0).
class OceanPlanarReflection
{
public:
    ~OceanPlanarReflection();

    // Records the reflection capture for this frame. Returns true when a
    // reflection texture is available to bind on the ocean surface this frame.
    // seaLevel is the calm water plane the camera is mirrored across.
    bool DeclareForView(Engine::Renderer::Pipeline::ViewDeclare& d, float seaLevel,
                        float resolutionScale, uint32_t frameIndex);

    bool IsReady(uint32_t frameIndex) const
    {
        return m_ReflectionReady && m_ReadyFrameIndex == frameIndex;
    }
    ::GameEngine::Rendering::TextureHandle GetReflectionTexture() const { return m_ColorTexture; }
    ::GameEngine::Rendering::SamplerHandle GetSampler() const { return m_Sampler; }

    // Re-import the reflection colour into this frame's graph so a same-frame
    // consumer can declare a Sampled read (external imports dedup by physical
    // handle, so this returns the id DeclareForView created — one declared RAW
    // edge orders the consumer after the mirror world pass). Invalid when the
    // reflection isn't ready this frame.
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportForSampling(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame, uint32_t frameIndex) const;

    // The secondary view the forward contributors emit into — the ocean's own
    // contributor checks this to skip the surface (water must not reflect itself).
    // 0 until the view is allocated (first DeclareForView).
    ::GameEngine::Rendering::ViewId GetReflectionViewId() const { return m_ViewId; }

private:
    // Allocate the persistent reflection view + camera once, configured to
    // receive extraction submissions but skip the active pipeline.
    void EnsureView(Engine::Renderer::RenderServices& rs,
                    const ::GameEngine::Rendering::ViewDesc& mainView);

    // (Re)create the scaled colour target + sampler when the size changes.
    bool EnsureColorTexture(::GameEngine::Rendering::IDevice& device, uint32_t width,
                            uint32_t height);

    // Build the mirrored camera from the main camera by reflecting across the
    // horizontal plane y = seaLevel (position + orientation), keeping the main
    // projection. viewProj is recomputed so culling + the world pass agree.
    static ::GameEngine::Rendering::CameraData MakeMirroredCamera(
        const ::GameEngine::Rendering::CameraData& main, float seaLevel);

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    Engine::Renderer::RenderServices* m_RenderServices = nullptr;

    ::GameEngine::Rendering::CameraId m_CameraId = 0;
    ::GameEngine::Rendering::ViewId m_ViewId = 0;
    bool m_ViewAllocated = false;

    ::GameEngine::Rendering::TextureHandle m_ColorTexture; // scaled RGBA16F
    ::GameEngine::Rendering::SamplerHandle m_Sampler;
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;

    bool m_ReflectionReady = false; // available to bind this frame
    uint32_t m_ReadyFrameIndex = UINT32_MAX;
};

} // namespace GameEngine::Ocean
