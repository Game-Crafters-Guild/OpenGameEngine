#include "SceneView/SceneViewEvents.h"

#include "Core/Engine.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"

namespace GameEngine::Editor::SceneTools
{

void PopulateScenePointerMods(ScenePointerEvent& ev, std::uint32_t mods)
{
    ev.alt   = (mods & Input::kModAlt) != 0;
    ev.ctrl  = (mods & Input::kModControl) != 0;
    ev.shift = (mods & Input::kModShift) != 0;
    ev.grave = false;
    ev.rotateSnap45 = false;
    if (Input::InputSystem* input = EngineCore::GetInstance().GetInputSystem())
    {
        ev.grave = input->IsKeyDown(static_cast<Input::KeyCode>(Input::kKeyCode_GraveAccent));
        ev.rotateSnap45 = input->IsKeyDown(static_cast<Input::KeyCode>(Input::kKeyCode_S));
    }
}

} // namespace GameEngine::Editor::SceneTools
