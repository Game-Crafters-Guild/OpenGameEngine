#include "Engine/Rendering/ExposureReadbackFeature.h"

#include <cmath>

namespace GameEngine::Engine::Renderer
{

namespace
{
// Same shape as IDevice::CreateReadbackBuffer (host-cached copy dest,
// persistently mapped); the ring owns creation so it can size and rotate the
// slots itself.
Rendering::BufferDesc ReadbackSlotDesc()
{
    Rendering::BufferDesc bd{};
    bd.size = ExposureReadbackFeature::kStateBytes;
    bd.usage = static_cast<uint32_t>(Rendering::BufferUsage::TransferDst);
    bd.memoryUsage = Rendering::BufferMemoryUsage::Readback;
    bd.flags = Rendering::BufferCreateFlags::PersistentlyMapped;
    return bd;
}
} // namespace

ExposureReadbackFeature::ExposureReadbackFeature()
    : m_Rings(ReadbackSlotDesc(), "AutoExposure.Readback.View")
{
}

ExposureReadbackFeature::~ExposureReadbackFeature()
{
    m_Rings.Destroy(m_Device);
}

void ExposureReadbackFeature::SetReadbackEnabled(Rendering::ViewId viewId, bool enabled)
{
    if (enabled)
    {
        m_Views[viewId].Enabled = true;
        return;
    }
    m_Rings.DestroyView(m_Device, viewId);
    m_Views.erase(viewId);
}

void ExposureReadbackFeature::OnDeviceRebuilt(Rendering::IDevice* device)
{
    if (!device)
        return;
    m_Device = device;
    // The readback ring's per-view slots are persistently-mapped Readback buffers the
    // rebuild freed; their cached mapped pointers now dangle (ReadNewestInto/BeginWrite
    // through them is a UAF that generational handles do NOT guard). Drop the rings so
    // AcquireSlotRG lazily re-creates + re-maps them (design §4). Device buffer
    // destroys inside are generational no-ops on the dead handles. The per-view CPU
    // state (enabled flag + last-good latched scale) survives in m_Views.
    m_Rings.Destroy(device);
}

bool ExposureReadbackFeature::IsReadbackEnabled(Rendering::ViewId viewId) const
{
    const auto it = m_Views.find(viewId);
    return it != m_Views.end() && it->second.Enabled;
}

Rendering::BufferHandle ExposureReadbackFeature::AcquireSlotRG(
    Rendering::IDevice* device, const Rendering::RenderGraph::RGFrame& frame,
    Rendering::ViewId viewId)
{
    if (!device)
        return {};
    m_Device = device;
    return m_Rings.BeginWrite(device, frame, viewId, SlotTag{frame.FrameIndex()});
}

void ExposureReadbackFeature::OnFrameSubmittedRG(Rendering::RenderGraph::RGFrame& frame,
                                                 const Rendering::IDevice::GpuSyncToken& token)
{
    m_Rings.OnFrameSubmitted(frame, token);
}

void ExposureReadbackFeature::OnFrameStreamRetiredRG(Rendering::RenderGraph::RGFrame& frame)
{
    m_Rings.OnFrameStreamRetired(frame);
}

bool ExposureReadbackFeature::TryResolveAdaptedExposure(Rendering::ViewId viewId,
                                                        float& outLinearScale, uint64_t* outFrameIndex)
{
    const auto it = m_Views.find(viewId);
    if (it == m_Views.end())
        return false;
    ViewState& view = it->second;
    State state{};
    SlotTag tag{};
    if (m_Rings.ReadNewestInto(m_Device, viewId, &state, sizeof(state), &tag))
    {
        // The resolve shader stamps valid=1 on its first write; a
        // zero-initialized slot (view metered for less than a ring
        // revolution) must not read as "scale 0".
        if (state.Valid != 0u && std::isfinite(state.Scale) && state.Scale > 0.0f)
        {
            view.LastScale = state.Scale;
            view.LastFrameIndex = tag.FrameIndex;
            view.HasScale = true;
        }
    }
    if (!view.HasScale)
        return false;
    outLinearScale = view.LastScale;
    if (outFrameIndex)
        *outFrameIndex = view.LastFrameIndex;
    return true;
}

} // namespace GameEngine::Engine::Renderer
