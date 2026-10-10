#pragma once

#include "Conversation.h"
#include "Providers/CliSessionList.h"
#include "Providers/IAgentProvider.h"

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
/// Everything one turn reported between two drains: the text deltas joined in order,
/// the tool calls in order, and the result once the turn ended.
struct TurnUpdate
{
    Conversation::TurnId Turn = 0;
    std::string Text;
    std::vector<ToolActivity> ToolActivities;
    /// The calls that ended in an error, in the order reported.
    std::vector<ToolFailure> FailedToolCalls;
    std::optional<TurnResult> Result;
};

/// A session list a read finished with, for the connection it was read for.
struct SessionListUpdate
{
    std::string ProviderId;
    CliSessionList List;
};

/// Carries a turn's reports from the turn thread, and a session list from the thread
/// that read it, to the UI thread. Reports coalesce
/// per turn: however many deltas and tool calls arrive between two drains, the UI
/// receives one TurnUpdate per turn, so nothing is ever dropped and the mailbox never
/// holds more than the turn in flight (turns run one at a time). Thread-safe.
class TurnMailbox
{
public:
    /// Appends a text delta to `turn`'s pending update.
    void PostText(Conversation::TurnId turn, std::string_view text);
    /// Appends a tool call to `turn`'s pending update.
    void PostToolActivity(Conversation::TurnId turn, const ToolActivity& activity);
    /// Records that a tool call of `turn` ended in an error.
    void PostToolFailed(Conversation::TurnId turn, const ToolFailure& failure);
    /// Records `turn`'s result; nothing of the turn follows it.
    void PostFinished(Conversation::TurnId turn, TurnResult result);
    /// Takes every pending update, in the order the turns first reported.
    std::vector<TurnUpdate> Take();

    /// Records the session list a read finished with, replacing one not yet taken.
    void PostSessionList(SessionListUpdate update);
    /// Takes the session list posted since the last call; nullopt when none was.
    std::optional<SessionListUpdate> TakeSessionList();

private:
    TurnUpdate& PendingFor(Conversation::TurnId turn);

    std::mutex m_Mutex;
    std::vector<TurnUpdate> m_Pending;
    std::optional<SessionListUpdate> m_SessionList;
};
} // namespace GameEngine
