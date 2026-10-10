#pragma once

// CBTUpdateRestGate — whether the CBT update can be skipped because the tree is at rest.
//
// An update whose inputs (camera, terrain, classify) equal the previous update's, and which split
// nothing, merged nothing and left the pool-pressure and off-frustum keep steps unchanged, leaves
// every CBT buffer as it found them; the next update with the same inputs would too. The work-queue
// counters that say so reach the CPU a few frames late (CBTActivityReadback), tagged with the
// sequence number of the update they were copied from. CPU only: the caller owns the inputs, the
// readback and the decision to declare the update.

#include <cstdint>
#include <span>
#include <vector>

namespace GameEngine::CBTTerrain
{

// What one update did, from its work-queue counters (CBTLayout.h kWQ* slots).
struct CBTUpdateActivity
{
    int32_t SplitServed = 0;        // kWQAllocateCounter: allocations Split satisfied
    int32_t MergeServed = 0;        // kWQSimplifyCounter: merge groups PrepareSimplify built
    int32_t PressureStep = 0;       // kWQPressureStep: planar pool-pressure scale step
    int32_t OffFrustumKeepStep = 0; // kWQOffFrustumKeepStep
    int32_t VertexEvalCount = 0;    // kWQVertexEvalCounter: bisectors a gated VertexEval re-evaluated
};

class CBTUpdateRestGate
{
  public:
    // Quiet readings, from updates recorded after the inputs last changed, before the update is
    // skipped. Two, because a reading proves its steps unchanged only against the one before it.
    static constexpr uint32_t kQuietReadingsToRest = 2u;

    // True when this frame's update, with `inputs`, can be skipped. Inputs that differ from the
    // previous call's restart the count; their first update is the first one counted.
    bool CanSkip(std::span<const uint8_t> inputs);

    // An update was declared; returns the sequence number its reading carries. A forced update (a
    // full vertex re-evaluation after a re-seed, a retired heightmap or a planet change) moves
    // corners without a split or merge, so it restarts the count.
    uint64_t OnUpdateRecorded(bool forced);

    // A reading landed. Readings of updates from before the last restart are ignored.
    void OnActivityRead(uint64_t sequence, const CBTUpdateActivity& activity);

    // Forgets everything, as at construction (device rebuild).
    void Reset();

  private:
    void Restart();

    std::vector<uint8_t> m_Inputs;
    bool m_HasInputs = false;
    uint64_t m_NextSequence = 0;
    uint64_t m_FirstCountedSequence = 0;
    uint32_t m_QuietReadings = 0;
    bool m_HasSteps = false;
    int32_t m_PressureStep = 0;
    int32_t m_OffFrustumKeepStep = 0;
};

} // namespace GameEngine::CBTTerrain
