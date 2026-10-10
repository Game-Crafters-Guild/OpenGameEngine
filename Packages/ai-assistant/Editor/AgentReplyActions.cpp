#include "AgentReplyActions.h"

#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{
std::vector<AgentReplyAction>& Actions()
{
    static std::vector<AgentReplyAction> actions;
    return actions;
}
} // namespace

void AgentReplyActions::Register(AgentReplyAction action)
{
    Actions().push_back(std::move(action));
}

std::span<const AgentReplyAction> AgentReplyActions::All()
{
    return Actions();
}
} // namespace GameEngine
