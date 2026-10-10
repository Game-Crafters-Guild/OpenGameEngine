#pragma once

#include <chrono>
#include <cstddef>
#include <string>

namespace GameEngine
{
struct CliSessionSummary;
struct ConversationMessage;

/// What the panel's status line says after a turn's session could not be continued
/// (AgentConversationController::SessionWasLost).
inline constexpr const char* kSessionLostStatus =
    "Reply failed: the session could not be continued, so Retry starts a new session";

/// What the panel's status line says after Copy: "Reply copied", or, for a reply
/// truncated at Conversation::kMaxMessageBytes, that only its first 64 KB was copied.
std::string CopyReplyFeedback(const ConversationMessage& reply);
/// What the panel's status line says when a prompt of `promptBytes` is refused for
/// being over Conversation::kMaxMessageBytes: its size in KB (rounded up) and the limit.
std::string PromptTooLongStatus(size_t promptBytes);
/// A session's time as the session list and the "Continued session" row show it: the
/// local date and time to the minute ("2026-10-07 14:05").
std::string SessionTimeText(std::chrono::system_clock::time_point time);
/// The first row of a conversation that continues `session`: "Continued session from
/// <SessionTimeText> · N earlier turns not shown", or "Continued session · earlier
/// turns not shown" for a session the list did not hold (a zero Time).
std::string ContinuedSessionText(const CliSessionSummary& session);
} // namespace GameEngine
