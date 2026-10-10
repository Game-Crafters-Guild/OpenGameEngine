#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine
{
/// Who wrote a conversation message. The system prompt is not a message: it travels
/// on AgentTurnRequest::SystemPrompt.
enum class MessageRole : uint8_t
{
    User,
    Assistant,
};

/// Where a message stands. An assistant message is Queued while an earlier turn runs,
/// InProgress while its turn streams, and ends Complete, Failed, Stopped (the user
/// stopped its turn) or Cancelled (Stop cleared it from the queue before it ran).
enum class MessageStatus : uint8_t
{
    Queued,
    InProgress,
    Complete,
    Failed,
    Stopped,
    Cancelled,
};

/// How a reported tool call ended.
enum class ToolCallEnd : uint8_t
{
    /// No error was reported (it ran, or the provider reports no results).
    Unknown,
    /// Its result was an error.
    Failed,
    /// The CLI refused it itself: it never reached the tool.
    Refused,
};

/// One tool call an assistant message made, as the provider reported it. The editor's
/// own record of a call (AssistantActionLedger) is what its row shows; a reported call
/// the editor never received gets a row of its own (AgentCallRowModel::For).
struct ToolCall
{
    /// The tool's name as the provider reported it.
    std::string Name;
    /// The call's input, as the JSON text the provider sent.
    std::string InputJson;
    /// The provider's id for the call; empty when it reports none.
    std::string Id;
    /// How the call ended, as the provider reported it.
    ToolCallEnd End = ToolCallEnd::Unknown;
    /// The first line of the error the call's result carried (ClaudeSessionProvider::FirstErrorLine),
    /// on a Failed call and on a Refused one alike; a later report with no text keeps it. Empty
    /// when no report carried one.
    std::string Error;
};

/// One message of a conversation in the AI Assistant panel.
struct ConversationMessage
{
    /// Who wrote the message.
    MessageRole Role = MessageRole::User;
    /// The message text: what the user sent, or the assistant's reply so far; at most
    /// Conversation::kMaxMessageBytes.
    std::string Text;
    /// The reply ran past Conversation::kMaxMessageBytes and Text holds its first part.
    bool Truncated = false;
    /// The reply failed and its prompt was sent again as a later turn (Retry).
    bool Retried = false;
    /// IAgentProvider::Id() of the provider that answered (empty on a user message).
    std::string ProviderId;
    /// The model the reply asked for, as named in settings; empty for the provider's
    /// default and on a user message.
    std::string Model;
    /// The effort the reply asked for (an AiAssistantSettings::EffortChoices() id);
    /// empty for the provider's default and on a user message.
    std::string Effort;
    /// The model that answered, as the provider reported it (TurnResult::Model); empty
    /// until the turn finishes and when the provider reports none.
    std::string AnsweredModel;
    /// When the message was created.
    std::chrono::system_clock::time_point Time;
    /// Where the message stands; a user message is always Complete.
    MessageStatus Status = MessageStatus::Complete;
    /// The tool calls the assistant made while writing this message, in order.
    std::vector<ToolCall> ToolCalls;
    /// On a finished assistant message: what the turn said beside its text (the
    /// failure and its fix, or why a reply ended short); empty otherwise.
    std::string Notice;
    /// The provider's cost estimate for the turn that wrote the message, in US
    /// dollars; shown, never summed. Zero where the provider reports none.
    double CostUsd = 0.0;
    /// Tokens the model read for the turn, cached ones included.
    uint64_t InputTokens = 0;
    /// Tokens the model wrote for the turn.
    uint64_t OutputTokens = 0;
};
} // namespace GameEngine
