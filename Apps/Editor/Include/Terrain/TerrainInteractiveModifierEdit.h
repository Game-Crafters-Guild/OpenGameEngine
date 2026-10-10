#pragma once

namespace GameEngine::Editor
{

// Holds the terrain preview-cadence throttle open for the duration of one
// Inspector drag gesture.
//
// A terrain modifier edit dirties every tile the modifier covers, and a
// GLOBAL-scoped one covers all of them. Dragging a control that writes such a
// modifier re-bakes the world on every mouse-move unless the modifier system is
// told a gesture is in progress: TerrainModifierSystem coalesces those frames to
// a bounded wall-clock cadence and settles once when the arm is released. The
// cadence and the settle both already ship; this type is only the editor half
// that arms them.
//
// It is a HELD object rather than a begin/end pair because the failure mode is
// asymmetric. A missed arm costs a stuttery drag; a missed release leaves the
// throttle armed for every later edit, so the terrain would never settle again.
// An Inspector rebuild destroys a widget's callbacks mid-drag — the mouse-up
// that would have released simply never arrives — so release has to ride on
// destruction, which is what capturing this in the widget's own callbacks gives.
//
// Arms are counted process-wide: two live arms both holding, then one releasing,
// must not clear a source the other still needs.
class TerrainInteractiveModifierEditArm
{
  public:
    TerrainInteractiveModifierEditArm() = default;
    ~TerrainInteractiveModifierEditArm();

    TerrainInteractiveModifierEditArm(const TerrainInteractiveModifierEditArm&) = delete;
    TerrainInteractiveModifierEditArm& operator=(const TerrainInteractiveModifierEditArm&) = delete;

    // Idempotent: a drag previews many times and only the first one arms.
    void Arm();

    // Idempotent: the commit releases, and destruction releases whatever a
    // commit that never came would have.
    void Release();

    bool IsArmed() const { return m_Armed; }

  private:
    bool m_Armed = false;
};

// Live arm count. The signal the terrain service carries is a single source bit
// shared by every Inspector arm, so the count is what decides when that bit
// clears — and a leaked arm is a count that never returns to zero.
int TerrainInteractiveModifierEditArmCount();

} // namespace GameEngine::Editor
