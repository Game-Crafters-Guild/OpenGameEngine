#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace GameEngine::Editor
{

// The parameters of the `set_parallax_steps_view` debug command, resolved against what the Scene
// Views show now.
//
// `enable` sets the view on or off; leaving it out toggles from the current state, so a caller that
// only wants to flip the view does not have to read it first. Anything but a boolean is an error
// rather than a guess: a string "false" read as truthy would turn the view on.
class ParallaxStepsViewRequest
{
public:
    // `shownNow`: whether the Scene Views show the Parallax steps view at the time of the request.
    // Check Error() first.
    ParallaxStepsViewRequest(const nlohmann::json& params, bool shownNow);

    // Empty when the request was understood; otherwise a message naming the parameter and the
    // values it takes.
    const std::string& Error() const { return m_Error; }

    // Whether the Scene Views show the view after the request. Only meaningful while Error() is
    // empty.
    bool Shown() const { return m_Shown; }

private:
    bool m_Shown = false;
    std::string m_Error;
};

} // namespace GameEngine::Editor
