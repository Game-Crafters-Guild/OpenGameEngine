#pragma once

#include "IAgentProvider.h"

#include <string>
#include <string_view>
#include <unordered_map>

namespace GameEngine
{
/// The Claude Messages API, streamed. Stateless: every turn resends the system
/// prompt, the history and the new user message. The key is read from the
/// ANTHROPIC_API_KEY environment variable when the turn starts and from nowhere
/// else, and goes only to the fixed endpoint below: there is no key setting and
/// no endpoint setting.
class ClaudeApiProvider final : public IAgentProvider
{
public:
    /// The provider's id (Id()).
    static constexpr std::string_view kId = "claude-api";
    /// The only URL this provider posts to.
    static constexpr std::string_view kEndpoint = "https://api.anthropic.com/v1/messages";
    /// The environment variable the key is read from.
    static constexpr std::string_view kApiKeyVariable = "ANTHROPIC_API_KEY";
    /// The model a turn uses when AgentTurnRequest::Model is empty.
    static constexpr std::string_view kDefaultModel = "claude-opus-5-5";
    /// The request's max_tokens: the longest reply a turn can produce.
    static constexpr int kMaxReplyTokens = 16384;

    std::string_view Id() const override { return kId; }
    ProviderCapabilities Capabilities() const override;

    /// Refuses with the fix when ANTHROPIC_API_KEY is unset or empty; otherwise
    /// streams the turn from kEndpoint (ClaudeApiStream).
    void RunTurn(const AgentTurnRequest& request, AgentTurnEvents& events, const CancelToken& cancel) override;

    /// The request body: model, max_tokens, stream, the system prompt when there
    /// is one, and messages = the history (messages with empty text skipped, as
    /// the API refuses them) followed by the user's message.
    static std::string BuildRequestBody(const AgentTurnRequest& request);

    /// The request headers: x-api-key = `key`, anthropic-version and accept
    /// (an event stream). The key goes in that one header and nowhere else.
    static std::unordered_map<std::string, std::string> BuildRequestHeaders(std::string_view key);
};
} // namespace GameEngine
