#pragma once

#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/RenderGraph/PerViewReadbackRings.h"
#include "TerrainGrass/GrassPlacementModel.h"
#include "Types/Types.h"

#include <unordered_map>
#include <vector>

namespace GameEngine::TerrainGrass
{

// Per-view truth about grass placement: what the compute PLACED, whether that number is still
// being refreshed, and how often the dispatch dropped a frame instead of running.
//
// Placement is stochastic and lives entirely on the GPU, so "are there blades here, and how many"
// has no CPU-side answer: the compute's counters are the only record and they land in the indirect
// args block. This copies that whole block into a readback ring slot per view per frame and latches
// the newest resolved sample, so the question is answered with a number instead of a screenshot.
//
// The counts are per VIEW, never per terrain. One dispatch covers every terrain params row and
// every terrain's blades land in one shared instance buffer behind one indirect draw per LOD, so
// the counts are the whole view's totals and cannot be attributed to a single terrain entity.

// One indirect draw record; mirrors DrawIndexedIndirectCommand in terrain_grass_place.comp and the
// Vulkan VkDrawIndexedIndirectCommand layout the draw consumes.
struct GrassIndirectDrawGPU
{
    uint32 IndexCount = 0;
    uint32 InstanceCount = 0;
    uint32 FirstIndex = 0;
    int32 VertexOffset = 0;
    uint32 FirstInstance = 0;
};
static_assert(sizeof(GrassIndirectDrawGPU) == 20);

// The per-view indirect args buffer in full: the draw records the GPU fills, then the placement
// counters. Draws and counters share ONE buffer so the readback copies a single contiguous range
// and can never report a count from a different dispatch than the draw it describes.
struct GrassIndirectBlockGPU
{
    GrassIndirectDrawGPU Draws[kGrassLodCount];
    // Slots the admitted cells reserve across both LODs — the pool's fill level, and the sum of
    // the two draw records. Written by the plan kernel, which also writes those records, so the
    // count and the draw it describes cannot disagree.
    uint32 CandidatesConsidered = 0;
    uint32 CellsVisible = 0; // cells that passed the frustum and stand on a grass terrain
    // Of those reserved slots, how many produced a blade. The gap is what the splat mask and the
    // growth floor rejected, standing in the pool as zero-size instances — so
    // AcceptedBlades / CandidatesConsidered is the pool efficiency on this content.
    uint32 AcceptedBlades = 0;
    // Radius scale the GPU budget fit landed on, as float bits. 1.0 means the pool cost this view
    // no range; below 1.0 the field ends that much closer than the CPU fit planned.
    uint32 FittedRangeScaleBits = 0;
    uint32 Capacity = 0;
    uint32 _Pad[3] = {};
};
static_assert(sizeof(GrassIndirectBlockGPU) == 20 * kGrassLodCount + 32);

class TerrainGrassPlacementStats
{
  public:
    TerrainGrassPlacementStats();

    // What the dispatch was ASKED to do, stamped into the slot at declare time. It travels with
    // the data (the ring's payload carriage) so a resolved count is always read against the
    // dispatch that produced it, never against whatever the current frame happens to be dispatching.
    struct Dispatch
    {
        uint32 Capacity = 0;            // instance slots in the pool both LODs draw from
        uint32 CellCount = 0;           // cells in the dispatched window (one workgroup each)
        uint32 PlannedCandidates = 0;   // candidates the CPU budget fit expected to survive culling
        uint32 TerrainParamsCount = 0;  // rows the dispatch covers
        uint32 ActiveGrassTerrains = 0; // rows with grass enabled
        float32 NearDensity = 0.0f;     // blades/m² at the camera, after the budget fit
        float32 Range = 0.0f;           // metres at which density reaches zero, after the budget fit
    };

    struct Placement
    {
        Dispatch Dispatched{};
        uint32 PlacedPerLod[kGrassLodCount] = {};
        uint32 PlacedInstances = 0;      // sum over LODs — the pool slots the draws cover
        // GPU truth, not a CPU estimate: the slots the admitted cells reserved, after the GPU
        // re-fit. Bounded by Capacity by construction, so it measures how much of the pool this
        // view spends rather than how much it wanted.
        uint32 CandidatesConsidered = 0;
        uint32 CellsVisible = 0;
        // Of the reserved slots, how many produced a blade; the rest are zero-size instances the
        // splat mask or the growth floor rejected. AcceptedBlades / PlacedInstances is the pool
        // efficiency, and it is the number that says what dead slots cost on real content.
        uint32 AcceptedBlades = 0;
        // Where the field actually ends, as a fraction of the range the CPU fit planned. Below 1
        // the per-view budget fit shortened it; nothing is ever refused, so this is the only place
        // a full pool shows up.
        float32 FittedRangeScale = 1.0f;
        bool RangeReducedByBudget = false;
    };

    // Which side of the indirect-args zero-fill the placement dispatch bailed out on. The two
    // classes look nothing alike on screen or in the counts, so one undifferentiated "it stopped"
    // tally would hide the half that matters.
    enum class Dropout : uint8
    {
        // Bailed BEFORE the fill. The per-(view, frame slot) args buffer still holds what the same
        // slot was given a full ring ago, so the draw runs against a placement made for a camera
        // several frames stale — at a perfectly healthy instance count. No count- or
        // threshold-shaped detector can see this one.
        StaleArgs,
        // Bailed AFTER the fill, so InstanceCount stays 0: this view's grass is simply absent for
        // the frame.
        ZeroGrass,
    };

    // One row of the per-view payload: the latched sample, plus the liveness and dropout history
    // that say whether it may be read as current.
    struct ViewPlacement
    {
        Rendering::ViewId View{};
        Placement Placed{};
        // Placed carries a resolved sample. False means the row exists only because dropouts were
        // counted for a view that has never resolved one, so its counts are not measured.
        bool HasSample = false;
        // The node declared no grass for this view, so Placed is a measured zero (RecordIdle).
        bool Idle = false;
        uint32 LastDeclaredFrameIndex = 0;
        // Frames since this view last declared grass, in FrameIndex() units. A view that stops
        // being declared at all is invisible to the node's idle guard — that guard is a scope
        // guard inside the declare, so it only runs when the node runs — and Poll leaves an
        // unrefreshed view's sample untouched. Without this the row serves its last healthy count
        // forever, which is how a view that rendered nothing reported 191406 blades.
        uint32 FramesSinceDeclared = 0;
        bool IsCurrent = false;
        // Monotonic per class, never reset while the device lives. Always on: every dropout path
        // used to be visible only under GE_TERRAIN_GRASS_DEBUG and only once per process, so a
        // shipped editor recorded nothing at all when grass dropped a frame.
        uint32 ZeroGrassFrames = 0;
        uint32 StaleArgFrames = 0;
        uint32 LastDropoutFrameIndex = 0;
    };

    // Ring write for this frame's dispatch; the caller copies InstanceCount into
    // the returned slot. Invalid handle when the device is null or the ring could
    // not be created. Also stamps this view as declared for the current frame,
    // which is what FramesSinceDeclared counts from.
    Rendering::BufferHandle AcquireSlotRG(Rendering::IDevice* device,
                                          const Rendering::RenderGraph::RGFrame& frame,
                                          Rendering::ViewId viewId,
                                          const Dispatch& dispatched);

    // This view's placement dispatch returned without dispatching. Called from the placement
    // pass's exec — the same thread that declares and polls, because the pass carries no
    // attachments and so is never forked into a record worker (RGFrame::RecordSecondariesParallel
    // takes only attachment-bearing passes).
    //
    // Costs nothing in the steady state: the early returns are the exceptional paths, and a frame
    // that dispatches touches none of this.
    void RecordDropout(Rendering::ViewId viewId, Dropout kind);

    // The view declared no grass this frame — grass disabled, or no terrain — so
    // the placement compute will not run and nothing can be placed. Latches a measured zero
    // and stops this view resolving further slots until it declares again.
    //
    // Without this the instrument LIES. The node's early-return path writes no
    // slot, so the newest previously-resolved sample keeps being served as if it
    // were current: switching grass off left the last healthy blade count on
    // display, observed exactly that way against a live editor.
    //
    // Its reach ends at "the node RAN and returned early" — the only caller is a scope guard
    // inside the declare, so a view the node stops being called for at all can never arrive here.
    // ViewPlacement::FramesSinceDeclared is what covers that boundary.
    void RecordIdle(Rendering::ViewId viewId);

    void OnFrameSubmitted(const Rendering::RenderGraph::RGFrame& frame,
                          const Rendering::IDevice::GpuSyncToken& token);
    void OnFrameStreamRetired(const Rendering::RenderGraph::RGFrame& frame);
    void OnDeviceRebuilt(Rendering::IDevice* device);
    void Destroy(Rendering::IDevice* device);

    // Consume every slot whose submission has completed and latch it. Maps the
    // slots, so main thread only (the debug-server handler path).
    //
    // A pending resolves only while the frame stream that declared it is still
    // alive — which is the normal case, because a frame stream is per WINDOW and
    // is re-begun every frame, not constructed per frame. Poll after the stream
    // dies and the sample is simply lost, silently.
    void Poll(Rendering::IDevice* device);

    // One row per view that has either resolved a sample or recorded a dropout, ascending by view
    // id. The placement counts are as of the last Poll; the liveness fields are derived here, so
    // they are current whether or not Poll ran.
    std::vector<ViewPlacement> LatchedPlacements() const;

    // Frames this object has seen submitted — the unit LastDeclaredFrameIndex and
    // LastDropoutFrameIndex are stamped in, and the only clock available to it. IDevice's frame
    // index is a slot in [0, framesInFlight) and wraps, so it cannot serve. One step per submitted
    // render-graph frame stream, so a multi-window editor steps it once per window per app frame.
    uint32 FrameIndex() const { return m_SubmittedFrames; }

    // The readback copies the whole indirect block, starting at its first byte. Lives here rather
    // than on the render feature because it IS the readback's contract with the buffer; the node
    // and the tests both read it, so a wrong size or offset reds the suite instead of quietly
    // reporting an index count as a blade count.
    static constexpr uint64 kSlotBytes = sizeof(GrassIndirectBlockGPU);
    static constexpr uint64 kIndirectArgsCopyOffset = 0;

    // A row still reads as current this many frames after its last declare. Wide enough that the
    // declare/submit/poll ordering skew, and several window streams stepping the clock within one
    // app frame, cannot make a healthy view look stale; narrow enough that a view which stopped
    // being declared is flagged fast. The clock steps once per window stream per app frame, so
    // the bound is derived from the engine's own stream ceiling (2 * kMaxRGFrameStreams = 16 in
    // FrameOrchestrator.h) rather than sampled from a one-window run: a healthy view under the
    // maximum supported window count stays comfortably inside it, and a dead view is flagged in
    // ~32 steps (about half a second at 60 fps with one window).
    static constexpr uint32 kCurrentWithinFrames = 32;

  private:
    struct ViewState
    {
        Placement Latest{};
        bool HasSample = false;
        // Set while the view is declaring no grass. Gates Poll so an in-flight
        // slot from before the stop cannot resolve and overwrite the zero with the
        // count that was true a few frames ago.
        bool Idle = false;
        uint32 LastDeclaredFrame = 0;
        uint32 ZeroGrassFrames = 0;
        uint32 StaleArgFrames = 0;
        uint32 LastDropoutFrame = 0;
    };

    Rendering::RenderGraph::PerViewReadbackRings<Dispatch> m_Rings;
    std::unordered_map<Rendering::ViewId, ViewState> m_Views;
    uint32 m_SubmittedFrames = 0;
};

} // namespace GameEngine::TerrainGrass
