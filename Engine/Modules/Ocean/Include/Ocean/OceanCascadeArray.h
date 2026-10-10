#pragma once

#include "Ocean/OceanTypes.h"

#include "Rendering/Core/Device.h" // TextureFormat
#include "Rendering/Core/Handle.h"

#include <cstdint>
#include <string>

namespace GameEngine::Rendering
{
class IDevice;
} // namespace GameEngine::Rendering

namespace GameEngine::Rendering::RenderGraph
{
class RGFrame;
struct RGTexture;
} // namespace GameEngine::Rendering::RenderGraph

namespace GameEngine::Ocean
{

// RAII wrapper around one camera-snapped sim cascade: a persistent
// Texture2DArray (res x res x lodCount) plus a per-layer View2D for each LOD,
// and the std140 OceanCascadeLayoutGPU that records where each layer sits in the
// world. Every snapped simulation (foam first; later flow/depth/clip/albedo)
// owns one instance with its own format.
//
// Creation mirrors ShadowMapRenderFeature's persistent moments array:
// UnorderedAccess | ShaderResource usage, ForceArrayView, persistent, and an
// initialState of ShaderResource so the first frame's barrier from
// ShaderResource -> UnorderedAccess is valid. Per-layer View2D views match the
// per-cascade bindless views CSM creates for its shadow array.
class OceanCascadeArray
{
public:
    ~OceanCascadeArray();

    OceanCascadeArray() = default;
    OceanCascadeArray(const OceanCascadeArray&) = delete;
    OceanCascadeArray& operator=(const OceanCascadeArray&) = delete;

    // Allocates the array + per-layer views. baseScale is the world extent (in
    // meters) of LOD 0; each higher LOD doubles it. Returns false on failure
    // (the owning sim should degrade to its previous per-pixel path).
    bool Initialize(::GameEngine::Rendering::IDevice* device, uint32 resolution, uint32 lodCount,
                    ::GameEngine::Rendering::TextureFormat format, float baseScale, const char* debugName);

    bool IsReady() const { return m_Ready; }
    uint32 GetResolution() const { return m_Resolution; }
    uint32 GetLodCount() const { return m_LodCount; }

    ::GameEngine::Rendering::TextureHandle GetTexture() const { return m_Texture; }
    // Per-LOD View2D (one array layer). Used for per-layer storage-image writes.
    ::GameEngine::Rendering::TextureViewHandle GetLayerView(uint32 lod) const;

    // Import into the frame's render graph at the resting-state contract
    // (created at ShaderResource; every graph frame that writes it restores
    // ShaderReadOnly via MarkOutput, so the fixed claim stays truthful).
    // Dedups by physical handle within a frame — producer and consumer arms
    // may both import. Invalid handle when the array isn't ready.
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportRG(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame) const;

    const OceanCascadeLayoutGPU& GetLayout() const { return m_Layout; }

    // Runtime base-scale change (OceanRenderer.MinScale): the next SnapToCamera
    // re-derives every layer's texel/extent from it — no texture recreation, since
    // only the layout math depends on it.
    void SetBaseScale(float baseScale) { m_BaseScale = baseScale; }
    float GetBaseScale() const { return m_BaseScale; }

    // Runtime resolution change (OceanRenderer.LodDataResolution): destroys and
    // recreates the array at the new per-axis resolution, keeping the LOD count,
    // format, and base scale. Returns false on failure (the caller keeps using the
    // old array). Call only at a safe point (frame start, before this frame's
    // dispatches are recorded).
    bool Resize(::GameEngine::Rendering::IDevice* device, uint32 resolution);

    // Re-center every LOD layer on the camera. Per LOD: texel = baseScale*2^lod /
    // resolution, extent = baseScale*2^lod, origin =
    // floor((cameraXZ - extent*0.5) / texel) * texel. Snapping origin to the texel
    // grid keeps the sampled world points stable as the camera moves, so the
    // simulation does not swim. Returns true when the layout changed.
    // While the coarsest layer is anchored (AnchorCoarsestLevel), SnapToCamera
    // leaves that layer where the anchor put it and snaps only the finer ones.
    bool SnapToCamera(float cameraX, float cameraZ);

    // Pin the coarsest LOD layer to a fixed world square that holds the XZ
    // rectangle [minX, maxX] x [minZ, maxZ] inside the band the surface samples
    // (uv 0.04 to 0.96), never smaller than LOD 0. A stateless bake over that
    // layer then covers everything inside the rectangle wherever the camera is,
    // at one texel per (rectangle span / 0.88 / resolution): a rectangle spanning
    // kilometers resolves its far shorelines coarsely. Takes effect at the next
    // SnapToCamera.
    void AnchorCoarsestLevel(float minX, float minZ, float maxX, float maxZ);
    // Return the coarsest layer to camera snapping.
    void ClearCoarsestAnchor() { m_CoarsestAnchored = false; }

    // Rebase an already-populated cascade into a new floating-origin coordinate
    // frame without moving texels or changing simulation phase.
    void RebaseOrigin(float32 shiftX, float32 shiftZ)
    {
        for (uint32 lod = 0u; lod < m_Layout.LodCount; ++lod)
        {
            m_Layout.CascadeOriginScale[lod][0] -= shiftX;
            m_Layout.CascadeOriginScale[lod][1] -= shiftZ;
        }
        m_CoarsestAnchor[0] -= shiftX;
        m_CoarsestAnchor[1] -= shiftZ;
    }

private:
    void Destroy();

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    bool m_Ready = false;
    uint32 m_Resolution = 0;
    uint32 m_LodCount = 0;
    float m_BaseScale = 0.0f;
    ::GameEngine::Rendering::TextureFormat m_Format{}; // stored for Resize
    std::string m_DebugName;                           // stored for Resize + the RG import name

    ::GameEngine::Rendering::TextureHandle m_Texture;
    ::GameEngine::Rendering::TextureViewHandle m_LayerViews[kMaxOceanLodCascades] = {};
    OceanCascadeLayoutGPU m_Layout;
    // The anchored coarsest layer: origin X, origin Z, texel size, scale (the
    // layer's extent / baseScale), as in a CascadeOriginScale entry.
    bool m_CoarsestAnchored = false;
    float m_CoarsestAnchor[4] = {};
};

} // namespace GameEngine::Ocean
