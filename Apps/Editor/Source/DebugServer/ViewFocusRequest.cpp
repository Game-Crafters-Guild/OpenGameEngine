#include "DebugServer/ViewFocusRequest.h"

#include "EditorPanelIds.h"

namespace GameEngine::Editor
{

ViewFocusRequest::ViewFocusRequest(const nlohmann::json& params)
{
    if (!params.contains("view") || params["view"].is_null())
    {
        m_Error = "Missing 'view' (scene or game)";
        return;
    }
    if (!params["view"].is_string())
    {
        m_Error = "'view' must be a string (scene or game)";
        return;
    }

    const std::string view = params["view"].get<std::string>();

    if (view == "scene")
    {
        m_PanelId = EditorPanelIds::SceneView;
        m_WantsViewportUiFocus = false;
        return;
    }

    if (view == "game")
    {
        m_PanelId = EditorPanelIds::GameView;
        m_WantsViewportUiFocus = true;
        return;
    }

    m_Error = "unknown view '" + view + "'; expected scene or game";
}

} // namespace GameEngine::Editor
