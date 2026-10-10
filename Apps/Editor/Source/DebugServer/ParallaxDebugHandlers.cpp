#include "DebugServer/ParallaxDebugHandlers.h"

#include "Core/Engine.h"
#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "DebugServer/ParallaxStepsViewRequest.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewRegistry.h"

#include <nlohmann/json.hpp>

namespace GameEngine
{

using json = nlohmann::json;

namespace
{

// Whether the Scene Views show the steps view now: they are set together, so the first one answers.
bool SceneViewsShowStepsView(const Engine::Renderer::ViewRegistry& views)
{
    for (const Rendering::ViewDesc& view : views.GetViews())
    {
        if (view.purpose == Rendering::ViewPurpose::EditorScene)
            return views.GetViewParallaxStepsView(view.id);
    }
    return false;
}

json SetParallaxStepsView(const EditorDebugServer::RequestContext& ctx)
{
    auto* renderServices = EngineCore::GetInstance().GetRenderServices();
    if (!renderServices)
        return Editor::RefuseRequest("No RenderServices");
    Engine::Renderer::ViewRegistry& views = renderServices->Views();

    const Editor::ParallaxStepsViewRequest request(ctx.params, SceneViewsShowStepsView(views));
    if (!request.Error().empty())
        return Editor::RefuseRequest(request.Error());

    json sceneViews = json::array();
    for (const Rendering::ViewDesc& view : views.GetViews())
    {
        if (view.purpose != Rendering::ViewPurpose::EditorScene)
            continue;
        views.SetViewParallaxStepsView(view.id, request.Shown());
        sceneViews.push_back(static_cast<uint64_t>(view.id));
    }
    if (sceneViews.empty())
        return Editor::RefuseRequest("No Scene View is open; open one and ask again");

    return json{{"ok", true}, {"enabled", request.Shown()}, {"sceneViews", std::move(sceneViews)}};
}

} // namespace

void RegisterParallaxDebugHandlers(EditorDebugServer& server)
{
    // set_parallax_steps_view — every Scene View draws height-mapped materials as the height samples
    // their relief march took (blue none, red the desktop march's most), in place of their lit
    // colour. `enable` true/false; leave it out to toggle.
    server.RegisterHandler("set_parallax_steps_view", SetParallaxStepsView);
}

} // namespace GameEngine
