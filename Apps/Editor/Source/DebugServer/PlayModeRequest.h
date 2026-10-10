#pragma once

#include "PlayMode/PlayModeManager.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace GameEngine::Editor
{

// Wire spelling of a play-mode state. `get_editor_state`'s "playMode" field and
// `set_play_mode`'s response share it, so the two can never disagree about what
// the editor is doing.
const char* PlayModeStateToString(PlayModeState state);

// The `action` parameter of the `set_play_mode` debug command, resolved against
// the state the editor is actually in.
//
// A transition is legal from exactly one state, and anything else is an error
// rather than a silent no-op: entering while already playing, or exiting while
// already editing, would otherwise answer "ok" to a caller whose next step
// assumes the transition happened. The editor's own Play/Pause/Stop buttons
// perform the accepted transitions through UI-replay commands, so this picks
// the command and never implements a transition itself.
//
// Pause and resume are the two directions of the editor's single TogglePause
// action, split so a caller states the outcome it wants instead of toggling
// blind. There is no frame-step action in the editor, so none is offered.
class PlayModeRequest
{
public:
    // `current` is the state the editor is in right now. Check Error() first.
    PlayModeRequest(const nlohmann::json& params, PlayModeState current);

    // Empty when the action was understood and is legal from `current`;
    // otherwise a message naming the action, the state that refused it, and the
    // state it would be legal from.
    const std::string& Error() const { return m_Error; }

    // The UiReplayCommandIds value that performs the transition. Only meaningful
    // while Error() is empty.
    std::uint32_t CommandId() const { return m_CommandId; }

    // True when the request was an entry into play mode. The caller uses it to
    // tell a refused entry (state unchanged) from a successful one.
    bool IsEnter() const { return m_IsEnter; }

    // True when the caller asked for the Game View to be brought to the front of
    // its tab group before the transition runs.
    //
    // It exists because the Game View is the only thing that publishes the
    // gameplay UI host, so nothing mounts a UI document while some other tab is
    // active: C# `Ui.FindElement` answers null and `[UiElement]` fields never
    // bind, which reads as a broken scripting ABI rather than as a HUD nobody is
    // compositing. Absent or null is false, which leaves the dock layout alone:
    // raising a view is a change to what the user is looking at, so it is opt-in.
    bool ActivateGameView() const { return m_ActivateGameView; }

private:
    std::uint32_t m_CommandId = 0;
    bool m_IsEnter = false;
    bool m_ActivateGameView = false;
    std::string m_Error;
};

} // namespace GameEngine::Editor
