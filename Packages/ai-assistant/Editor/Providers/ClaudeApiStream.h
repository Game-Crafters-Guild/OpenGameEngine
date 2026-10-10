#pragma once

#include "ClaudeApiProvider.h"
#include "IAgentProvider.h"
#include "SseParser.h"

#include "Platform/HttpClient.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace GameEngine
{
class CancelToken;

/// One Messages API turn as it streams: maps the reply's server-sent events onto
/// a turn's events. `content_block_delta` text deltas become OnTextDelta in
/// arrival order; `message_start` and `message_delta` carry the token counts;
/// `message_delta`'s `stop_reason` notes a reply cut at the token limit or
/// declined; `message_stop` marks the reply complete; an `error` event fails
/// the turn with its message. A mapped event whose data is not a JSON object
/// fails the turn; `ping`, `content_block_stop`, other block types and event
/// types the API adds later are ignored without reading their data.
class ClaudeApiStream
{
public:
    /// The response body cap, sized from the reply's token limit. A reply
    /// streams at least one token per `text_delta` event and each event's
    /// envelope (event line, data prefix, the JSON around the text) is about
    /// 115 bytes, so 512 bytes per token holds the envelope and the token's
    /// text even when every character is JSON-escaped (6 bytes for \uXXXX):
    /// 16,384 tokens x 512 bytes = 8 MiB, half HttpClient's 16 MiB default.
    static constexpr size_t kMaxResponseBytes = size_t{ClaudeApiProvider::kMaxReplyTokens} * 512;

    /// Posts `body` to `url` with `headers`, streams the reply into `events` and
    /// ends with exactly one OnFinished. ClaudeApiProvider passes its fixed
    /// endpoint; nothing else names a URL. `cancel` stops the transfer.
    static void Run(const std::string& url,
                    const std::unordered_map<std::string, std::string>& headers,
                    const std::string& body,
                    AgentTurnEvents& events,
                    const CancelToken& cancel);

    explicit ClaudeApiStream(AgentTurnEvents& events);

    /// Consumes the next response bytes, split anywhere.
    void Feed(std::string_view bytes);

    /// Ends the turn with one OnFinished: Stopped when the request was
    /// cancelled, Failed on an `error` event, a refused request, a reply over
    /// kMaxResponseBytes, a transport failure or a stream that ended before
    /// `message_stop`, else Succeeded (with a Note when `stop_reason` was
    /// `max_tokens` or `refusal`). Text keeps what arrived in every case.
    void Finish(const HttpClient::HttpResponse& response);

private:
    void OnEvent(const SseEvent& event);
    void Fail(std::string message);

    AgentTurnEvents& m_Events;
    SseParser m_Parser;
    TurnResult m_Result;
    std::string m_StopReason;
    bool m_MessageStopped = false;
    bool m_Failed = false;
};
} // namespace GameEngine
