#include "DebugServer/PlayModeRequest.h"

#include "Automation/UiReplayCommandIds.h"

namespace GameEngine::Editor
{

const char* PlayModeStateToString(PlayModeState state)
{
    switch (state)
    {
    case PlayModeState::Edit:
        return "editing";
    case PlayModeState::EnteringPlay:
        return "entering_play";
    case PlayModeState::Play:
        return "playing";
    case PlayModeState::Paused:
        return "paused";
    case PlayModeState::ExitingPlay:
        return "exiting_play";
    case PlayModeState::ChangeReview:
        return "change_review";
    }
    return "unknown";
}

PlayModeRequest::PlayModeRequest(const nlohmann::json& params, PlayModeState current)
{
    if (!params.contains("action") || params["action"].is_null())
    {
        m_Error = "Missing 'action' (enter, exit, pause or resume)";
        return;
    }
    if (!params["action"].is_string())
    {
        m_Error = "'action' must be a string (enter, exit, pause or resume)";
        return;
    }

    // Strictly a boolean. The handler is reachable over raw IPC with anything,
    // and a permissive read of the string "false" is truthy — the one misreading
    // that would rearrange the caller's dock layout after it asked not to.
    if (params.contains("activateGameView") && !params["activateGameView"].is_null())
    {
        if (!params["activateGameView"].is_boolean())
        {
            m_Error = "'activateGameView' must be a boolean (true or false)";
            return;
        }
        m_ActivateGameView = params["activateGameView"].get<bool>();
    }

    const std::string action = params["action"].get<std::string>();
    const std::string from = std::string("; the editor is '") + PlayModeStateToString(current) + "'";

    if (action == "enter")
    {
        // ChangeReview lands here too: its pending play-mode edits must be
        // resolved by an exit before a new session can be snapshotted.
        if (current != PlayModeState::Edit)
        {
            m_Error = "cannot 'enter' play mode" + from + ", not 'editing' — exit play mode first";
            return;
        }
        m_CommandId = UiReplayCommandIds::PlayEnter;
        m_IsEnter = true;
        return;
    }

    if (action == "exit")
    {
        if (current == PlayModeState::Edit)
        {
            m_Error = "cannot 'exit' play mode" + from + ", which is not a running state";
            return;
        }
        m_CommandId = UiReplayCommandIds::PlayStop;
        return;
    }

    if (action == "pause")
    {
        if (current != PlayModeState::Play)
        {
            m_Error = "cannot 'pause'" + from + ", not 'playing'";
            return;
        }
        m_CommandId = UiReplayCommandIds::PlayTogglePause;
        return;
    }

    if (action == "resume")
    {
        if (current != PlayModeState::Paused)
        {
            m_Error = "cannot 'resume'" + from + ", not 'paused'";
            return;
        }
        m_CommandId = UiReplayCommandIds::PlayTogglePause;
        return;
    }

    m_Error = "unknown action '" + action + "'; expected enter, exit, pause or resume";
}

} // namespace GameEngine::Editor
