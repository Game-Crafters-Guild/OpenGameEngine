#pragma once

#include "Mathematics/Matrix3x3.h"
#include "Mathematics/Vector3.h"
#include "Types/Types.h"

#include <cstdint>
#include <vector>

namespace GameEngine::Engine::Renderer
{
class ViewRegistry;

// "Is any camera that can see this world-space field moving right now?" — the
// signal DDGI's idle-gated solve mode holds its work on.
//
// DDGI is engine-shared and view-independent, so there is no single "the
// camera": the gate samples EVERY eligible registered view each tick and
// reports moving if any of them moved. Eligible means a view that a human is
// actually looking through — Game and EditorScene only. Preview and
// utility-capture views are deliberately excluded: a thumbnail bake or a
// reflection-probe capture re-points its camera every frame by design, and
// counting those would wedge the field at "always moving" and it would never
// solve at all.
//
// After the last moving view comes to rest the gate stays armed for
// kRestDebounceMs before reporting still, matching the reference
// implementation's GI_IDLE_MS quiet window (js/gi_probes.js: GI_IDLE_MS = 200,
// `moving = idleMs < GI_IDLE_MS`). The debounce is on the RELEASE side only —
// motion arms the gate on the tick it is first seen.
class DDGICameraIdleGate
{
  public:
    // Samples this tick's eligible views and advances the rest timer.
    // `worldId` is the volume's world (0 = accept any world's views);
    // `deltaTimeSeconds` is the frame's delta, so the debounce is measured in
    // the same clock the caller already has rather than a second wall clock.
    // Returns true while the field should hold: a view moved this tick, or the
    // rest debounce has not elapsed yet.
    bool UpdateAndIsMoving(const ViewRegistry& views, uint64 worldId, float deltaTimeSeconds);

  private:
    // World-space camera pose of one view, as of the last tick it was sampled.
    struct ViewPose
    {
        uint32 ViewId = 0;
        Mathematics::Vector3 PositionWS{};
        // View matrix's rotation basis (column-major columns 0..2).
        Mathematics::Matrix3x3 Basis{};
        uint64 SeenSerial = 0;
    };

    std::vector<ViewPose> m_Poses;
    // Ticks the gate has run, used to drop views that stopped being sampled
    // (released, or no longer eligible) instead of leaking their pose forever.
    uint64 m_Serial = 0;
    float m_RestMs = 0.0f;
};

}  // namespace GameEngine::Engine::Renderer
