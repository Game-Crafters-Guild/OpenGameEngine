#include "DebugServer/MarkupDebugHandlers.h"

#include "Core/Engine.h"
#include "DebugServer/DebugHandlers.h"
#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "EditorApplication.h"
#include "Markups/MarkupEditorBridge.h"
#include "Markups/MarkupPresentation.h"
#include "Markups/MarkupRequests.h"
#include "PlayMode/PlayModeManager.h"
#include "SceneViewController.h"

#include <nlohmann/json.hpp>

namespace GameEngine
{

using json = nlohmann::json;

void RegisterMarkupDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
    const auto MakeContext = [](EditorApplication& editor) {
        Editor::MarkupRequestContext context;
        context.World = EngineCore::GetInstance().GetPrimaryWorld();
        context.Bridge = editor.m_MarkupBridge.get();
        context.Undo = editor.m_UndoRedo.get();
        context.Notifications = editor.m_ChangeNotifications.get();
        context.PlayMode = editor.m_PlayMode && editor.m_PlayMode->IsPlayingOrPaused();
        context.FrameTanHalfFov = Editor::MarkupFrameTanHalfFov(
            (!editor.m_Windows.empty() && editor.m_Windows[0]) ? editor.m_Windows[0]->scene.get() : nullptr);
        return context;
    };

    server.RegisterHandler("markup_list", [&app, MakeContext](const EditorDebugServer::RequestContext& ctx) -> json {
        return Editor::ListMarkups(MakeContext(app), ctx.params);
    });
    server.RegisterHandler("markup_get", [&app, MakeContext](const EditorDebugServer::RequestContext& ctx) -> json {
        return Editor::GetMarkup(MakeContext(app), ctx.params);
    });
    server.RegisterHandler("markup_create", [&app, MakeContext](const EditorDebugServer::RequestContext& ctx) -> json {
        return Editor::CreateMarkup(MakeContext(app), ctx.params);
    });
    server.RegisterHandler("markup_update", [&app, MakeContext](const EditorDebugServer::RequestContext& ctx) -> json {
        return Editor::UpdateMarkup(MakeContext(app), ctx.params);
    });
    server.RegisterHandler("markup_contains", [&app, MakeContext](const EditorDebugServer::RequestContext& ctx) -> json {
        return Editor::ContainsInMarkup(MakeContext(app), ctx.params);
    });
    server.RegisterHandler("markup_comment", [&app, MakeContext](const EditorDebugServer::RequestContext& ctx) -> json {
        return Editor::CommentOnMarkup(MakeContext(app), ctx.params);
    });
    // Frames the mark-up's bounding sphere with look_at's pose math; never focus_view,
    // which would ask the operating system for the foreground.
    server.RegisterHandler("markup_frame", [&app, MakeContext](const EditorDebugServer::RequestContext& ctx) -> json {
        SceneViewCameraPose pose{};
        json result = Editor::FrameMarkup(MakeContext(app), ctx.params, pose);
        if (Editor::IsRefusal(result))
            return result;
        if (!ApplyMainSceneViewPose(app, pose))
            return Editor::RefuseRequest("No Scene View to frame in; open one first");
        return result;
    });
    server.RegisterHandler("markup_set_visible", [&app, MakeContext](const EditorDebugServer::RequestContext& ctx) -> json {
        return Editor::SetMarkupsVisible(MakeContext(app), ctx.params);
    });
}

} // namespace GameEngine
