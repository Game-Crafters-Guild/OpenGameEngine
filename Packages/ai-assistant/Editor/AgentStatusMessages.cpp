#include "AgentStatusMessages.h"

#include "Conversation.h"
#include "ConversationMessage.h"
#include "Providers/CliSessionList.h"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace GameEngine
{
static_assert(Conversation::kMaxMessageBytes == 64 * 1024, "CopyReplyFeedback names the cap");

std::string CopyReplyFeedback(const ConversationMessage& reply)
{
    if (reply.Truncated)
        return "Reply copied: its first 64 KB, where it was truncated";
    return "Reply copied";
}

std::string PromptTooLongStatus(size_t promptBytes)
{
    constexpr size_t kBytesPerKb = 1024;
    return "The prompt is " + std::to_string((promptBytes + kBytesPerKb - 1) / kBytesPerKb) + " KB, over the " +
           std::to_string(Conversation::kMaxMessageBytes / kBytesPerKb) +
           " KB a message can hold: shorten it and send again.";
}

std::string SessionTimeText(std::chrono::system_clock::time_point time)
{
    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%d %H:%M");
    return out.str();
}

std::string ContinuedSessionText(const CliSessionSummary& session)
{
    if (session.Time == std::chrono::system_clock::time_point{})
        return "Continued session · earlier turns not shown";
    return "Continued session from " + SessionTimeText(session.Time) + " · " +
           std::to_string(session.PromptCount) + (session.PromptCount == 1 ? " earlier turn" : " earlier turns") +
           " not shown";
}
} // namespace GameEngine
