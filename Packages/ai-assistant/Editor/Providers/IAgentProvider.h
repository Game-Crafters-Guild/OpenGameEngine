#pragma once

#include "ConversationMessage.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine
{
class CancelToken;

/// The editor's MCP server, attached to a turn so the model can call the editor's
/// tools. The provider adds it to the CLI's launch; the server lives as long as the turn.
struct AgentToolAttachment
{
    /// The server's name in the CLI's configuration (AssistantTools::kServerName).
    std::string ServerName;
    /// The Node.js executable and the staged server script it runs.
    std::string Node;
    std::string ServerScript;
    /// The server's environment: the editor's port, its tool set, the session token
    /// and the request time limit.
    std::vector<std::pair<std::string, std::string>> Environment;
    /// The tools the CLI may call, by their bare names.
    std::vector<std::string> ToolNames;
    /// The configuration file holding the server's definition, written by the editor
    /// for this turn; a CLI that reads a file reads this one.
    std::filesystem::path ConfigFile;
    /// The editor's debug port, which the server reaches.
    uint16_t Port = 0;
};

/// What one turn sends to a provider. A provider reads the half that fits its shape:
/// a message API (OwnsTranscript false) resends History plus UserText; a CLI session
/// (OwnsTranscript true) sends only UserText and continues SessionId.
struct AgentTurnRequest
{
    /// Instructions the panel adds to the provider's own system prompt.
    std::string SystemPrompt;
    /// The conversation before this turn, oldest first. Must outlive RunTurn().
    std::span<const ConversationMessage> History;
    /// The user's new message.
    std::string UserText;
    /// The model to answer with, as named in settings; empty selects the provider's default.
    std::string Model;
    /// The reasoning effort to answer with, one of the levels the provider offers
    /// (ClaudeSessionProvider::kEffortLevels); empty selects the provider's default. Only
    /// providers with effort levels read it.
    std::string Effort;
    /// The provider session to continue; empty starts a new one. Only providers that
    /// own their transcript read it.
    std::string SessionId;
    /// The editor's tools for this turn; null for a conversation without them. Must
    /// outlive RunTurn(). Only providers that can act with tools read it.
    const AgentToolAttachment* Tools = nullptr;
};

/// A tool call the provider reports while a turn runs.
struct ToolActivity
{
    /// The tool's name as the provider reported it.
    std::string Name;
    /// The call's input, as the JSON text the provider sent.
    std::string InputJson;
    /// The provider's id for the call, which its result names; empty when it reports none.
    std::string Id;
};

/// How a turn ended.
enum class TurnOutcome : uint8_t
{
    /// The provider finished its reply.
    Succeeded,
    /// The provider could not answer; TurnResult::Error says why and how to fix it.
    Failed,
    /// The CancelToken stopped the turn before the provider finished.
    Stopped,
};

/// The end of a turn, reported once through AgentTurnEvents::OnFinished(). The
/// accounting fields are the provider's own figures, shown to the user and never
/// summed into a bill; zero where the provider reports none.
struct TurnResult
{
    /// How the turn ended.
    TurnOutcome Outcome = TurnOutcome::Failed;
    /// The whole reply text; equals the concatenation of the turn's text deltas. A
    /// provider that does not stream delivers the reply as one delta before OnFinished.
    std::string Text;
    /// On Failed: what went wrong and the fix, in one user-facing message. Text
    /// keeps whatever arrived before the failure.
    std::string Error;
    /// On Succeeded: why the reply ended short of a complete answer (the token
    /// limit, a refusal), in one user-facing sentence; empty for a complete reply.
    std::string Note;
    /// The model that answered, as the provider reported it (a full model id); empty
    /// when the provider reports none.
    std::string Model;
    /// The provider's cost estimate for the turn, in US dollars.
    double CostUsd = 0.0;
    /// Model round trips the provider made for the turn (more than one when it calls tools).
    uint32_t ModelRoundTrips = 0;
    /// Tokens the model read for the turn, cached ones included.
    uint64_t InputTokens = 0;
    /// Tokens the model wrote for the turn.
    uint64_t OutputTokens = 0;
    /// Tool calls the provider's permission rules refused during the turn.
    uint32_t PermissionDenials = 0;
    /// On Failed: the provider ran and said the session AgentTurnRequest::SessionId
    /// named does not exist (any other failure, a refused login check included,
    /// leaves it false), so continuing that session can never succeed.
    bool SessionNotFound = false;
};

/// What a provider can do; the panel adapts its controls to it.
struct ProviderCapabilities
{
    /// The reply arrives as text deltas while it is generated.
    bool Streams = false;
    /// The provider keeps the conversation itself and continues it by session id.
    bool OwnsTranscript = false;
    /// The provider needs a key the user supplies.
    bool NeedsKey = false;
    /// The provider can call the editor's tools.
    bool CanActWithTools = false;
};

/// How a tool call the provider reported ended in an error.
struct ToolFailure
{
    /// ToolActivity::Id of the call.
    std::string CallId;
    /// The CLI refused the call itself (not in its allowed list): it never reached the tool.
    bool Refused = false;
    /// The first line of the error the call's result carried; empty when it had none.
    std::string Error;
};

/// Receives a turn's progress. RunTurn() calls these on its own thread, in order:
/// OnSessionId (when the provider reports one) before the first OnTextDelta, deltas
/// and tool activity in the order the provider produced them, and OnFinished exactly
/// once, last.
class AgentTurnEvents
{
public:
    virtual ~AgentTurnEvents() = default;

    /// The next piece of the reply text.
    virtual void OnTextDelta(std::string_view text) = 0;
    /// A tool call the provider made.
    virtual void OnToolActivity(const ToolActivity& activity) = 0;
    /// A tool call ended in an error (ToolFailure). A later report for the same call
    /// replaces an earlier one (a CLI reports a permission denial after the call's error).
    /// A provider that reports no results never calls it.
    virtual void OnToolFailed(const ToolFailure& /*failure*/) {}
    /// The provider's session id for this conversation; pass it as
    /// AgentTurnRequest::SessionId to continue the conversation.
    virtual void OnSessionId(std::string_view sessionId) = 0;
    /// The turn ended; nothing follows.
    virtual void OnFinished(const TurnResult& result) = 0;
};

/// A connection the AI Assistant can talk to: a message API or a local CLI session.
/// One provider runs one turn at a time.
class IAgentProvider
{
public:
    virtual ~IAgentProvider() = default;

    /// Stable identifier, persisted in settings and on conversation messages.
    virtual std::string_view Id() const = 0;
    /// What this provider can do; constant for the provider's lifetime.
    virtual ProviderCapabilities Capabilities() const = 0;

    /// Runs one turn and blocks until it ends, reporting through `events`; every
    /// outcome, failure included, ends with one OnFinished(). Polls `cancel` and ends
    /// with TurnOutcome::Stopped once it is cancelled. Never call it on a JobSystem
    /// worker: a turn can last minutes.
    virtual void RunTurn(const AgentTurnRequest& request, AgentTurnEvents& events, const CancelToken& cancel) = 0;
};
} // namespace GameEngine
