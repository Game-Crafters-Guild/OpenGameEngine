#include "ClaudeApiProvider.h"

#include "ClaudeApiStream.h"

#include <cstdlib>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{
constexpr const char* kApiVersion = "2023-06-01";
} // namespace

ProviderCapabilities ClaudeApiProvider::Capabilities() const
{
    return {.Streams = true, .OwnsTranscript = false, .NeedsKey = true, .CanActWithTools = false};
}

void ClaudeApiProvider::RunTurn(const AgentTurnRequest& request, AgentTurnEvents& events, const CancelToken& cancel)
{
    const std::string variable(kApiKeyVariable);
    const char* key = std::getenv(variable.c_str());
    if (key == nullptr || key[0] == '\0')
    {
        TurnResult result;
        result.Outcome = TurnOutcome::Failed;
        result.Error = "No Claude API key: set " + variable + " in the environment before starting the editor.";
        events.OnFinished(result);
        return;
    }

    ClaudeApiStream::Run(std::string(kEndpoint), BuildRequestHeaders(key), BuildRequestBody(request), events, cancel);
}

std::unordered_map<std::string, std::string> ClaudeApiProvider::BuildRequestHeaders(std::string_view key)
{
    return {
        {"x-api-key", std::string(key)},
        {"anthropic-version", kApiVersion},
        {"accept", "text/event-stream"},
    };
}

std::string ClaudeApiProvider::BuildRequestBody(const AgentTurnRequest& request)
{
    nlohmann::json messages = nlohmann::json::array();
    for (const ConversationMessage& message : request.History)
    {
        if (message.Text.empty())
            continue;
        messages.push_back({{"role", message.Role == MessageRole::User ? "user" : "assistant"},
                            {"content", message.Text}});
    }
    messages.push_back({{"role", "user"}, {"content", request.UserText}});

    nlohmann::json body = {
        {"model", request.Model.empty() ? std::string(kDefaultModel) : request.Model},
        {"max_tokens", kMaxReplyTokens},
        {"stream", true},
        {"messages", std::move(messages)},
    };
    if (!request.SystemPrompt.empty())
        body["system"] = request.SystemPrompt;
    // Invalid UTF-8 in typed text becomes U+FFFD instead of throwing out of the turn.
    return body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}
} // namespace GameEngine
