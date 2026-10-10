#pragma once

#include <cstdint>
#include "Mathematics/Ray.h"
#include "Mathematics/Vector3.h"

namespace GameEngine {
namespace Editor {
namespace SceneTools {

enum class PointerButton : std::uint8_t
{
    None,
    Left,
    Right,
    Middle
};

enum class PointerPhase : std::uint8_t
{
    Down,
    Up,
    Move
};

// Editor-side alias for the shared engine ray type so call sites remain
// descriptive in the tools/gizmos context while reusing a generic 3D ray.
using GizmoRay = ::GameEngine::Mathematics::Ray3D;

struct ScenePointerEvent
{
    float         viewX = 0.0f;
    float         viewY = 0.0f;
    float         viewW = 0.0f;
    float         viewH = 0.0f;
    PointerButton button = PointerButton::None;
    PointerPhase  phase  = PointerPhase::Move;

    bool alt   = false;
    bool ctrl  = false;
    bool shift = false;
    // Raw key poll (not a modifier bit): true while the ` / Grave key is held.
    bool grave = false;
    // Raw key poll: true while S is held (rotate gizmo: snap to 45° increments).
    bool rotateSnap45 = false;

    GizmoRay ray;

    // Camera state in world space, written by
    // SceneViewController::PopulatePointerCameraState. Project a world point to
    // view pixels through SceneTools::ProjectWorldToView (SceneViewProjection.h).
    Mathematics::Vector3 cameraPos{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 cameraRight{1.0f, 0.0f, 0.0f};
    Mathematics::Vector3 cameraUp{0.0f, 1.0f, 0.0f};
    Mathematics::Vector3 cameraForward{0.0f, 0.0f, 1.0f};
    // Perspective: tan(vertical FOV / 2), orthoHeight == 0.
    // Orthographic: full vertical extent in world units, tanHalfFovY == 0.
    float tanHalfFovY = 0.0f;
    float orthoHeight = 0.0f;
};

struct SceneKeyEvent
{
    std::uint32_t keyCode = 0;
    bool          pressed = false;

    bool alt   = false;
    bool ctrl  = false;
    bool shift = false;
};

void PopulateScenePointerMods(ScenePointerEvent& ev, std::uint32_t mods);

} // namespace SceneTools
} // namespace Editor
} // namespace GameEngine

