#include "AssistantActionLedger.h"

#include <algorithm>
#include <utility>

namespace GameEngine
{
namespace
{
// The last revision any ledger took, so two ledgers never share a value and a view that
// follows one conversation's ledger after another still sees the change.
uint64_t s_LastRevision = 0;

template <typename Actions>
auto FindAction(Actions& actions, uint64_t requestId)
{
    // Recent calls are the ones asked about.
    return std::find_if(actions.rbegin(), actions.rend(),
                        [requestId](const AssistantAction& action) { return action.RequestId == requestId; });
}
} // namespace

void AssistantActionLedger::Add(AssistantAction action)
{
    m_Actions.push_back(std::move(action));
    Touch();
    m_Actions.back().Revision = m_Revision;
}

AssistantAction* AssistantActionLedger::Edit(uint64_t requestId)
{
    const auto it = FindAction(m_Actions, requestId);
    if (it == m_Actions.rend())
        return nullptr;
    Touch();
    it->Revision = m_Revision;
    return &*it;
}

const AssistantAction* AssistantActionLedger::Find(uint64_t requestId) const
{
    const auto it = FindAction(m_Actions, requestId);
    return it == m_Actions.rend() ? nullptr : &*it;
}

void AssistantActionLedger::Touch()
{
    m_Revision = ++s_LastRevision;
}
} // namespace GameEngine
