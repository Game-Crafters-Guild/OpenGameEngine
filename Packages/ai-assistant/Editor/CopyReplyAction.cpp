#include "CopyReplyAction.h"

#include "AgentStatusMessages.h"
#include "ConversationMessage.h"

#include "Platform/Clipboard.h"

namespace GameEngine
{
std::string CopyReply(const ConversationMessage& reply)
{
    Platform::SetClipboardText(reply.Text.c_str());
    return CopyReplyFeedback(reply);
}
} // namespace GameEngine
