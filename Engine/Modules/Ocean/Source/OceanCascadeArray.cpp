#include "Ocean/OceanCascadeArray.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace GameEngine::Ocean
{

using namespace ::GameEngine::Rendering;

OceanCascadeArray::~OceanCascadeArray()
{
    Destroy();
}

void OceanCascadeArray::Destroy()
{
    if (!m_Device)
        return;
    for (uint32 lod = 0; lod < m_LodCount; ++lod)
    {
        if (m_LayerViews[lod].IsValid())
            m_Device->DestroyTextureView(m_LayerViews[lod]);
        m_LayerViews[lod] = {};
    }
    if (m_Texture.IsValid())
        m_Device->DestroyTexture(m_Texture);
    m_Texture = {};
    m_Ready = false;
}

bool OceanCascadeArray::Initialize(IDevice* device, uint32 resolution, uint32 lodCount,
                                   TextureFormat format, float baseScale, const char* debugName)
{
    if (m_Ready)
        return true;
    if (!device || resolution == 0 || lodCount == 0)
        return false;

    m_Device = device;
    m_Resolution = resolution;
    m_LodCount = std::min(lodCount, kMaxOceanLodCascades);
    m_BaseScale = baseScale;
    m_Format = format;
    m_DebugName = debugName;

    // Persistent storage-writable + sampleable array. Mirrors the CSM moments
    // array: ForceArrayView so a sampler2DArray descriptor sees all layers, and
    // initialState ShaderResource so the sim's first ShaderResource ->
    // UnorderedAccess barrier is well-defined.
    TextureDesc td{};
    td.width = resolution;
    td.height = resolution;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = m_LodCount;
    td.format = static_cast<uint32>(format);
    td.usage = static_cast<uint32>(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource);
    td.sampleCount = 1;
    td.flags = TextureCreateFlags::ForceArrayView;
    td.persistent = true;
    td.initialState = ResourceState::ShaderResource;
    td.debugName = debugName;
    m_Texture = device->CreateTexture(td);
    if (!m_Texture.IsValid())
    {
        m_Device = nullptr;
        return false;
    }

    // Per-LOD View2D over a single array layer — bound as the storage image for
    // the compute write of that layer (a sampler2DArray imageStore by gl_Global
    // InvocationID.z is also possible, but per-layer views keep the ping-pong
    // dispatch a single full-array write, matching CSM's per-cascade views).
    for (uint32 lod = 0; lod < m_LodCount; ++lod)
    {
        TextureViewDesc vd{};
        vd.viewType = TextureViewType::View2D;
        vd.aspect = TextureAspect::Color;
        vd.baseLayer = lod;
        vd.layerCount = 1;
        vd.debugName = "OceanCascadeLayer";
        m_LayerViews[lod] = device->CreateTextureView(m_Texture, vd);
        if (!m_LayerViews[lod].IsValid())
        {
            Destroy();
            return false;
        }
    }

    // Default layout (origin 0) until the first SnapToCamera.
    for (uint32 lod = 0; lod < m_LodCount; ++lod)
    {
        const float texel = (baseScale * static_cast<float>(1u << lod)) / static_cast<float>(resolution);
        m_Layout.CascadeOriginScale[lod][0] = 0.0f;
        m_Layout.CascadeOriginScale[lod][1] = 0.0f;
        m_Layout.CascadeOriginScale[lod][2] = texel;
        m_Layout.CascadeOriginScale[lod][3] = static_cast<float>(1u << lod);
    }
    m_Layout.LodCount = m_LodCount;

    m_Ready = true;
    return true;
}

TextureViewHandle OceanCascadeArray::GetLayerView(uint32 lod) const
{
    if (lod >= m_LodCount)
        return {};
    return m_LayerViews[lod];
}

RenderGraph::RGTexture OceanCascadeArray::ImportRG(RenderGraph::RGFrame& frame) const
{
    if (!m_Ready || !m_Texture.IsValid())
        return {};
    // Real layer count so per-layer barrier cells cover the whole array (the
    // import defaults are mips=1, layers=1).
    return frame.ImportExternalTexture(m_DebugName.c_str(), m_Texture,
                                       ResourceState::ShaderResource, m_Format,
                                       1, m_LodCount);
}

bool OceanCascadeArray::SnapToCamera(float cameraX, float cameraZ)
{
    bool changed = m_Layout.LodCount != m_LodCount;
    for (uint32 lod = 0; lod < m_LodCount; ++lod)
    {
        if (m_CoarsestAnchored && lod + 1u == m_LodCount)
        {
            float* layer = m_Layout.CascadeOriginScale[lod];
            changed = changed || std::memcmp(layer, m_CoarsestAnchor, sizeof(m_CoarsestAnchor)) != 0;
            std::memcpy(layer, m_CoarsestAnchor, sizeof(m_CoarsestAnchor));
            continue;
        }
        const float scale = static_cast<float>(1u << lod);
        const float texel = (m_BaseScale * scale) / static_cast<float>(m_Resolution);
        const float extent = m_BaseScale * scale;
        const float originX = std::floor((cameraX - extent * 0.5f) / texel) * texel;
        const float originZ = std::floor((cameraZ - extent * 0.5f) / texel) * texel;
        changed = changed ||
                  m_Layout.CascadeOriginScale[lod][0] != originX ||
                  m_Layout.CascadeOriginScale[lod][1] != originZ ||
                  m_Layout.CascadeOriginScale[lod][2] != texel ||
                  m_Layout.CascadeOriginScale[lod][3] != scale;
        m_Layout.CascadeOriginScale[lod][0] = originX;
        m_Layout.CascadeOriginScale[lod][1] = originZ;
        m_Layout.CascadeOriginScale[lod][2] = texel;
        m_Layout.CascadeOriginScale[lod][3] = scale;
    }
    const uint32 coarsestAnchored = m_CoarsestAnchored ? 1u : 0u;
    changed = changed || m_Layout.CoarsestAnchored != coarsestAnchored;
    m_Layout.CoarsestAnchored = coarsestAnchored;
    m_Layout.LodCount = m_LodCount;
    return changed;
}

void OceanCascadeArray::AnchorCoarsestLevel(float minX, float minZ, float maxX, float maxZ)
{
    // The surface samples a layer only inside uv 0.04 to 0.96; the rectangle sits
    // in the middle of that band so its edges keep a few texels of margin.
    constexpr float kSampledBand = 0.88f;
    const float span = std::max(maxX - minX, maxZ - minZ);
    const float extent = std::max(span / kSampledBand, m_BaseScale);
    const float texel = extent / static_cast<float>(m_Resolution);
    m_CoarsestAnchor[0] = (minX + maxX) * 0.5f - extent * 0.5f;
    m_CoarsestAnchor[1] = (minZ + maxZ) * 0.5f - extent * 0.5f;
    m_CoarsestAnchor[2] = texel;
    m_CoarsestAnchor[3] = extent / m_BaseScale;
    m_CoarsestAnchored = true;
}

bool OceanCascadeArray::Resize(IDevice* device, uint32 resolution)
{
    if (!device || resolution == 0)
        return false;
    if (m_Ready && resolution == m_Resolution)
        return true;
    // Recreate at the new resolution, preserving LOD count / format / base scale
    // / debug name (the name is also the stable RG import key).
    const uint32 lodCount = m_LodCount;
    const TextureFormat format = m_Format;
    const float baseScale = m_BaseScale;
    const std::string debugName = m_DebugName;
    Destroy(); // frees textures + clears m_Ready so Initialize re-runs
    return Initialize(device, resolution, lodCount, format, baseScale, debugName.c_str());
}

} // namespace GameEngine::Ocean
