#include "Engine/Rendering/LodCrossfadeLiveness.h"

namespace GameEngine
{
namespace Engine
{
namespace Renderer
{

bool ResolveCrossfadeLiveness(float durationSeconds,
                              const LodCrossfadeTailObservation& observation,
                              uint64_t currentRecomputeCount, uint64_t frameIndex,
                              LodCrossfadeLivenessState& state)
{
    // One verdict per frame: the spine asks once per scatter call, and a call
    // that recomputes must not flip the answer the earlier calls acted on.
    if (frameIndex == state.ResolvedFrame)
        return state.Verdict;
    state.ResolvedFrame = frameIndex;

    if (durationSeconds <= 0.0f)
    {
        state.Verdict = false;
    }
    else if (!observation.Valid || observation.RecomputeCount != currentRecomputeCount)
    {
        // No trustworthy reading: either none has been reduced yet (or the last
        // was structurally invalidated), or a dispatch has recomputed since this
        // one was taken and may have started a transition the reading predates.
        // Suppressing keeps this frame's scatter dispatching, which is what
        // produces the next reading.
        state.Verdict = true;
    }
    else
    {
        state.Verdict = observation.TailRecords != 0u;
    }
    return state.Verdict;
}

} // namespace Renderer
} // namespace Engine
} // namespace GameEngine
