#include "DebugServer/AssistantSessionHandlers.h"

#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "Editor/Registries/DebugRequestGateRegistry.h"

#include <nlohmann/json.hpp>

#include <string>

namespace GameEngine
{
namespace
{
// The answer to a claimed method's request that reached its handler: a claiming gate
// admitted it, or no gate claims the method and it is refused.
nlohmann::json AnswerClaimedMethod(const EditorDebugServer& server, const std::string& method, const char* admitted)
{
    if (!server.Gates().IsClaimed(method))
        return Editor::RefuseRequest(method + ": this editor has no AI Assistant to answer it; open the project "
                                              "whose AI Assistant panel started the session");
    return nlohmann::json{{admitted, true}};
}
} // namespace

void RegisterAssistantSessionHandlers(EditorDebugServer& server)
{
    server.RegisterHandler("assistant_bind", [&server](const EditorDebugServer::RequestContext&) {
        return AnswerClaimedMethod(server, "assistant_bind", "bound");
    });
    server.RegisterHandler("assistant_authorize", [&server](const EditorDebugServer::RequestContext&) {
        return AnswerClaimedMethod(server, "assistant_authorize", "authorized");
    });
}

} // namespace GameEngine
