#include "TurnMailbox.h"

#include <algorithm>
#include <cassert>
#include <utility>

namespace GameEngine
{
namespace
{
// One turn runs at a time and the next starts only after the UI drained the last
// one's result, so the mailbox only ever holds the running turn.
constexpr size_t kMaxPendingTurns = 1;
} // namespace

void TurnMailbox::PostText(Conversation::TurnId turn, std::string_view text)
{
    std::lock_guard lock(m_Mutex);
    PendingFor(turn).Text.append(text);
}

void TurnMailbox::PostToolActivity(Conversation::TurnId turn, const ToolActivity& activity)
{
    std::lock_guard lock(m_Mutex);
    PendingFor(turn).ToolActivities.push_back(activity);
}

void TurnMailbox::PostToolFailed(Conversation::TurnId turn, const ToolFailure& failure)
{
    std::lock_guard lock(m_Mutex);
    PendingFor(turn).FailedToolCalls.push_back(failure);
}

void TurnMailbox::PostFinished(Conversation::TurnId turn, TurnResult result)
{
    std::lock_guard lock(m_Mutex);
    PendingFor(turn).Result = std::move(result);
}

std::vector<TurnUpdate> TurnMailbox::Take()
{
    std::lock_guard lock(m_Mutex);
    return std::exchange(m_Pending, {});
}

void TurnMailbox::PostSessionList(SessionListUpdate update)
{
    std::lock_guard lock(m_Mutex);
    m_SessionList = std::move(update);
}

std::optional<SessionListUpdate> TurnMailbox::TakeSessionList()
{
    std::lock_guard lock(m_Mutex);
    return std::exchange(m_SessionList, std::nullopt);
}

TurnUpdate& TurnMailbox::PendingFor(Conversation::TurnId turn)
{
    const auto it = std::find_if(m_Pending.begin(), m_Pending.end(),
                                 [turn](const TurnUpdate& update) { return update.Turn == turn; });
    if (it != m_Pending.end())
        return *it;
    assert(m_Pending.size() < kMaxPendingTurns && "turns run one at a time; a drain was skipped");
    TurnUpdate& added = m_Pending.emplace_back();
    added.Turn = turn;
    return added;
}
} // namespace GameEngine
