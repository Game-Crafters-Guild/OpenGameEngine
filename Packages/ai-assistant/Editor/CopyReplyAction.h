#pragma once

#include <string>

namespace GameEngine
{
struct ConversationMessage;

/// The Copy reply action: puts the reply's stored text on the clipboard and returns
/// CopyReplyFeedback(reply) for the panel's status line.
std::string CopyReply(const ConversationMessage& reply);
} // namespace GameEngine
