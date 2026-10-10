#pragma once

#include "Mathematics/Vector3.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::Rendering
{
struct ViewDesc;
}

namespace GameEngine::Engine::Renderer
{
class ViewRegistry;

// "Which cameras does DDGI care about?" — one answer, shared by everything that
// has to ask.
//
// DDGI is engine-shared and view-independent, so there is no single "the
// camera". Eligible means a view a human is actually looking through: Game and
// EditorScene only. Preview and utility-capture views are deliberately excluded
// because a thumbnail bake or a reflection-probe capture re-points its camera
// every frame by design — counting those would wedge the idle gate at "always
// moving", and would make a following volume chase a camera nobody is using.
//
// `worldId` 0 on either side means unscoped: a view that accepts any world, or
// a volume that has not been given one.
bool IsDDGIEligibleView(const Rendering::ViewDesc& view, uint64 worldId);

// Picks the camera a DDGIVolumeFit::FollowCamera volume should track, with no
// host-supplied signal: it follows the eligible camera that MOVED most
// recently, observed from the view registry itself.
//
// Motion is the arbitration because it is the only honest one available. An
// editor renders Scene and Game views simultaneously, both eligible — but the
// camera the user is working through is the one they are driving, and driving
// is observable as movement. Fly the Scene View and the grid follows the
// editor camera; play the game and it follows the game camera; nobody had to
// tell the renderer which "mode" the user is in, and multiple live views need
// no special case. While every camera is at rest the previous choice is held —
// re-centring is destructive enough (see DDGIVolumeSystem::ApplyCameraFit)
// that a tie must never flip the target.
//
// Before any motion has ever been seen, a Game view wins over an EditorScene
// view: a Player has only Game views, and a freshly opened editor centres on
// whatever the scene camera shows soon enough for the first mouse movement to
// take over.
//
// Known residual: a SCRIPTED Game camera (a cutscene panning during play) is
// still an eligible mover and will win the election over an idle Scene View —
// usually the right call, since the travelling game camera is what is being
// lit. If a project needs a camera that must never drive shared effects, the
// extension point is eligibility (a per-view opt-out on ViewDesc), not a
// host-declared mode.
class DDGIFollowCameraTracker
{
  public:
    // Samples this tick's eligible views. Returns true and writes the followed
    // camera's world-space position, or false when no eligible view exists
    // this tick (nothing rendering yet, or every view is a capture) — callers
    // hold their current centre in that case rather than inventing one.
    bool Update(const ViewRegistry& views, uint64 worldId, Mathematics::Vector3& outPositionWS);

  private:
    struct ViewSample
    {
        uint32 ViewId = 0;
        Mathematics::Vector3 PositionWS{};
        uint64 SeenSerial = 0;
    };

    std::vector<ViewSample> m_Samples;
    // Ticks the tracker has run; stale entries (views that stopped being
    // sampled) are dropped by serial, same discipline as DDGICameraIdleGate.
    uint64 m_Serial = 0;
    // View whose camera moved most recently; 0 until any eligible view moves.
    uint32 m_FollowedViewId = 0;
};

}  // namespace GameEngine::Engine::Renderer
