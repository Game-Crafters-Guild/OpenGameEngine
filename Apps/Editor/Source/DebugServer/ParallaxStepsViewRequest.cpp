#include "DebugServer/ParallaxStepsViewRequest.h"

namespace GameEngine::Editor
{

ParallaxStepsViewRequest::ParallaxStepsViewRequest(const nlohmann::json& params, bool shownNow)
{
    if (!params.is_object() || !params.contains("enable") || params["enable"].is_null())
    {
        m_Shown = !shownNow;
        return;
    }
    if (!params["enable"].is_boolean())
    {
        m_Error = "'enable' must be true or false; leave it out to toggle the view";
        return;
    }
    m_Shown = params["enable"].get<bool>();
}

} // namespace GameEngine::Editor
