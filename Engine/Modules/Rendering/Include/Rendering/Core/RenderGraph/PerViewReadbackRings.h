#pragma once

#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGReadbackRing.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>

namespace GameEngine::Rendering::RenderGraph
{

// A per-view family of RGReadbackRings sharing one slot shape — the layer
// every multi-view readback consumer re-derived on top of the ring (CSM's
// SDSM depth bounds, the editor's exposure readback): lazy per-view ring
// creation, the FramesInFlight+2 sizing contract, the submit/retire fan-out,
// and teardown. The ring's completion contract is unchanged; see
// RGReadbackRing.h.
//
// The slot BufferDesc is fixed per instance (debugName is overridden with
// "<namePrefix><viewId>" at ring creation). Owners forward OnFrameSubmitted /
// OnFrameStreamRetired from their IRenderFeature hooks and call Destroy
// before device teardown.
template <class Payload>
class PerViewReadbackRings
{
  public:
    PerViewReadbackRings(const BufferDesc& slotDesc, std::string debugNamePrefix)
        : m_SlotDesc(slotDesc), m_DebugNamePrefix(std::move(debugNamePrefix))
    {
        // The per-view name is composed at ring creation; a caller-supplied
        // name pointer must not dangle inside the stored copy.
        m_SlotDesc.debugName = nullptr;
    }

    // Begins a ring write for this frame on the view's ring, creating it on
    // first use (FramesInFlight+2 slots — a reused slot's previous write has
    // always retired). Invalid handle when device is null or creation fails.
    BufferHandle BeginWrite(IDevice* device, const RGFrame& frame, ViewId viewId,
                            const Payload& payload = {})
    {
        if (!device)
            return {};
        auto& ring = m_Rings[viewId];
        if (!ring.IsInitialized())
        {
            BufferDesc desc = m_SlotDesc;
            const std::string debugName =
                m_DebugNamePrefix + std::to_string(static_cast<uint32_t>(viewId));
            desc.debugName = debugName.c_str();
            ring.Init(device, desc, std::max(1u, device->GetFramesInFlight()) + 2u);
        }
        return ring.BeginWrite(frame, payload);
    }

    // Record the view slot's GPU-side resolve — see RGReadbackRing::
    // RecordResolve. A no-op wherever BeginWrite already handed out the
    // mappable buffer, so callers record it unconditionally after the write.
    void RecordResolve(ViewId viewId, CommandList* cl, BufferHandle writeHandle)
    {
        const auto it = m_Rings.find(viewId);
        if (it != m_Rings.end())
            it->second.RecordResolve(cl, writeHandle);
    }

    // Stamp / purge every view's pendings (RGReadbackRing completion
    // contract) — forward from the owner's OnFrameSubmittedRG /
    // OnFrameStreamRetiredRG hooks.
    void OnFrameSubmitted(const RGFrame& frame, const IDevice::GpuSyncToken& token)
    {
        for (auto& [viewId, ring] : m_Rings)
        {
            (void)viewId;
            ring.OnFrameSubmitted(frame, token);
        }
    }

    void OnFrameStreamRetired(const RGFrame& frame)
    {
        for (auto& [viewId, ring] : m_Rings)
        {
            (void)viewId;
            ring.OnFrameStreamRetired(frame);
        }
    }

    // Copies the NEWEST signaled slot's first `bytes` into dst (consuming it
    // and everything older, per the ring's newest-wins resolve). False when
    // no slot is ready. `outPayload` receives the slot's schedule-time
    // metadata when non-null.
    bool ReadNewestInto(IDevice* device, ViewId viewId, void* dst, size_t bytes,
                        Payload* outPayload = nullptr)
    {
        const auto it = m_Rings.find(viewId);
        if (it == m_Rings.end())
            return false;
        const void* mapped = it->second.MapNewestReady(device, outPayload);
        if (!mapped)
            return false;
        std::memcpy(dst, mapped, bytes);
        it->second.Unmap(device);
        return true;
    }

    // Drop ONE view's pendings without touching its buffers or any other view:
    // that view's data contract was invalidated, so results already in flight must
    // not resolve as fresh afterwards. The per-view arm of
    // RGReadbackRing::DropPendings.
    //
    // Gating reads is NOT a substitute. The ring resolves newest-first and
    // BeginWrite only evicts the pending on the slot it reuses, so a completed
    // pending from before the invalidation survives and is what the first read
    // after the view resumes hands back.
    void DropPendings(ViewId viewId)
    {
        const auto it = m_Rings.find(viewId);
        if (it != m_Rings.end())
            it->second.DropPendings();
    }

    // Release one view's ring (device buffer destroys are timeline-deferred,
    // so in-flight writes are safe) or every ring before device teardown.
    void DestroyView(IDevice* device, ViewId viewId)
    {
        const auto it = m_Rings.find(viewId);
        if (it == m_Rings.end())
            return;
        it->second.Destroy(device);
        m_Rings.erase(it);
    }

    void Destroy(IDevice* device)
    {
        for (auto& [viewId, ring] : m_Rings)
        {
            (void)viewId;
            ring.Destroy(device);
        }
        m_Rings.clear();
    }

  private:
    BufferDesc m_SlotDesc;
    std::string m_DebugNamePrefix;
    std::unordered_map<ViewId, RGReadbackRing<Payload>> m_Rings;
};

} // namespace GameEngine::Rendering::RenderGraph
