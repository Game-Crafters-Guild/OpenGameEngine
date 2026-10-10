#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace GameEngine::Editor
{

// The `view` parameter of the `focus_view` debug command: which of the editor's
// two viewport panels to bring to the front of its dock tab group, and what that
// view needs done with UI focus once it is there.
//
// The two views want opposite things from UI focus, which is the trap this
// encodes. Scene View camera movement is driven by the editor input action
// system, so a focused viewport makes UIManager consume W/A/S/D before those
// actions ever see them — focusing the Scene View means CLEARING UI focus.
// The Game View is the reverse: its viewport must hold UI focus for input to
// reach the running game.
class ViewFocusRequest
{
public:
    // Parses the request parameters. Check Error() before using the request.
    explicit ViewFocusRequest(const nlohmann::json& params);

    // Empty when the view was understood; otherwise a message naming the value
    // that matched nothing and the values that would.
    const std::string& Error() const { return m_Error; }

    // Dock panel id of the requested view. Only meaningful while Error() is empty.
    const char* PanelId() const { return m_PanelId; }

    // True when the view's viewport element should take UI focus, false when UI
    // focus must instead be cleared. Only meaningful while Error() is empty.
    bool WantsViewportUiFocus() const { return m_WantsViewportUiFocus; }

private:
    const char* m_PanelId = nullptr;
    bool m_WantsViewportUiFocus = false;
    std::string m_Error;
};

} // namespace GameEngine::Editor
