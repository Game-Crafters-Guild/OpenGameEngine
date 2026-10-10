#include "TerrainGrass/TerrainGrassPlacementStats.h"

#include <algorithm>
#include <cstring>

namespace GameEngine::TerrainGrass
{

namespace
{
// Same shape as IDevice::CreateReadbackBuffer (host-cached copy dest,
// persistently mapped); the ring owns creation so it can size and rotate slots.
Rendering::BufferDesc ReadbackSlotDesc()
{
    Rendering::BufferDesc bd{};
    bd.size = TerrainGrassPlacementStats::kSlotBytes;
    bd.usage = static_cast<uint32>(Rendering::BufferUsage::TransferDst);
    bd.memoryUsage = Rendering::BufferMemoryUsage::Readback;
    bd.flags = Rendering::BufferCreateFlags::PersistentlyMapped;
    return bd;
}
} // namespace

TerrainGrassPlacementStats::TerrainGrassPlacementStats()
    : m_Rings(ReadbackSlotDesc(), "TerrainGrass.PlacedCount.View")
{
}

Rendering::BufferHandle TerrainGrassPlacementStats::AcquireSlotRG(
    Rendering::IDevice* device, const Rendering::RenderGraph::RGFrame& frame,
    Rendering::ViewId viewId, const Dispatch& dispatched)
{
    if (!device)
        return {};
    // Stamped whether or not the ring hands back a slot: what this records is that the node ran
    // for this view, which is exactly the fact a view that stops being declared stops producing.
    ViewState& state = m_Views[viewId];
    state.LastDeclaredFrame = m_SubmittedFrames;
    const Rendering::BufferHandle slot = m_Rings.BeginWrite(device, frame, viewId, dispatched);
    if (slot.IsValid())
    {
        // A poll target from the first write, before any resolve. Declaring again
        // clears idle: this view is placing grass once more.
        state.Idle = false;
    }
    return slot;
}

void TerrainGrassPlacementStats::RecordDropout(Rendering::ViewId viewId, Dropout kind)
{
    ViewState& state = m_Views[viewId];
    if (kind == Dropout::StaleArgs)
        ++state.StaleArgFrames;
    else
        ++state.ZeroGrassFrames;
    state.LastDropoutFrame = m_SubmittedFrames;
}

void TerrainGrassPlacementStats::RecordIdle(Rendering::ViewId viewId)
{
    ViewState& state = m_Views[viewId];
    state.Idle = true;
    state.Latest = {};
    // A measured zero, not an absence: the placement compute provably did not run for this
    // view, so nothing was placed.
    state.HasSample = true;
    // Drop what is already in flight, do not merely stop reading it. A slot is
    // written every frame while Poll runs only when the payload is asked for, so a
    // completed-but-unconsumed pending at this boundary is the normal case; the
    // ring resolves newest-first, so on resume that stale pending would be served
    // ahead of the not-yet-complete new one — carrying its own dispatch shape, and
    // therefore self-consistently wrong and undetectable.
    m_Rings.DropPendings(viewId);
}

void TerrainGrassPlacementStats::OnFrameSubmitted(const Rendering::RenderGraph::RGFrame& frame,
                                                  const Rendering::IDevice::GpuSyncToken& token)
{
    m_Rings.OnFrameSubmitted(frame, token);
    // The object's clock. On frames a stream actually submits, the feature fan-out reaches every
    // registered feature whether or not grass declared anything (a stream that skipped its declare
    // this incarnation returns before the fan-out) — enough to tell a view that stopped being
    // declared apart from one that is merely between samples.
    ++m_SubmittedFrames;
}

void TerrainGrassPlacementStats::OnFrameStreamRetired(const Rendering::RenderGraph::RGFrame& frame)
{
    m_Rings.OnFrameStreamRetired(frame);
}

void TerrainGrassPlacementStats::OnDeviceRebuilt(Rendering::IDevice* device)
{
    // The ring's slots are persistently-mapped Readback buffers the rebuild
    // freed; their cached mapped pointers now dangle (generational handles do
    // NOT guard that). Drop the rings so the next AcquireSlotRG re-creates them.
    m_Rings.Destroy(device);
    // Unlike the exposure readback — which latches a last-good scale so the image
    // does not pop — a diagnostic that keeps reporting a pre-rebuild count as
    // current is a false instrument. After a rebuild nothing has been measured
    // yet, and saying so for the few frames until the next sample resolves is the
    // honest answer.
    m_Views.clear();
}

void TerrainGrassPlacementStats::Destroy(Rendering::IDevice* device)
{
    m_Rings.Destroy(device);
    m_Views.clear();
}

void TerrainGrassPlacementStats::Poll(Rendering::IDevice* device)
{
    if (!device)
        return;
    for (auto& [viewId, state] : m_Views)
    {
        // A view that stopped declaring keeps its measured zero: a slot still in
        // flight from before the stop describes a dispatch that no longer happens.
        if (state.Idle)
            continue;
        GrassIndirectBlockGPU block{};
        Dispatch dispatched{};
        // ReadNewestInto resolves the newest completed slot and drops everything
        // older, so one call per poll already yields the freshest sample.
        if (!m_Rings.ReadNewestInto(device, viewId, &block, sizeof(block), &dispatched))
            continue;

        Placement placement{};
        placement.Dispatched = dispatched;
        for (uint32 lod = 0; lod < kGrassLodCount; ++lod)
        {
            placement.PlacedPerLod[lod] = block.Draws[lod].InstanceCount;
            placement.PlacedInstances += block.Draws[lod].InstanceCount;
        }
        placement.CandidatesConsidered = block.CandidatesConsidered;
        placement.CellsVisible = block.CellsVisible;
        placement.AcceptedBlades = block.AcceptedBlades;
        // The GPU writes the scale as float bits so it can share the counter block with the
        // integer tallies; memcpy rather than a reinterpret so no strict-aliasing rule is bent.
        std::memcpy(&placement.FittedRangeScale, &block.FittedRangeScaleBits,
                    sizeof(placement.FittedRangeScale));
        // Nothing is ever refused now — the budget is spent by shortening the field, not by
        // dropping candidates — so a full pool shows up here and only here.
        placement.RangeReducedByBudget = placement.FittedRangeScale < 1.0f;

        state.Latest = placement;
        state.HasSample = true;
    }
}

std::vector<TerrainGrassPlacementStats::ViewPlacement>
TerrainGrassPlacementStats::LatchedPlacements() const
{
    std::vector<ViewPlacement> out;
    out.reserve(m_Views.size());
    for (const auto& [viewId, state] : m_Views)
    {
        const bool hasDropouts = state.ZeroGrassFrames != 0 || state.StaleArgFrames != 0;
        if (!state.HasSample && !hasDropouts)
            continue;

        ViewPlacement row{};
        row.View = viewId;
        row.Placed = state.Latest;
        row.HasSample = state.HasSample;
        row.Idle = state.Idle;
        row.LastDeclaredFrameIndex = state.LastDeclaredFrame;
        row.FramesSinceDeclared = m_SubmittedFrames - state.LastDeclaredFrame;
        row.IsCurrent = row.FramesSinceDeclared <= kCurrentWithinFrames;
        row.ZeroGrassFrames = state.ZeroGrassFrames;
        row.StaleArgFrames = state.StaleArgFrames;
        row.LastDropoutFrameIndex = state.LastDropoutFrame;
        out.push_back(row);
    }
    // Hash-map order is not reproducible; a diagnostic payload that reorders
    // between calls is unreadable when two captures are diffed.
    std::sort(out.begin(), out.end(),
              [](const ViewPlacement& a, const ViewPlacement& b) { return a.View < b.View; });
    return out;
}

} // namespace GameEngine::TerrainGrass
