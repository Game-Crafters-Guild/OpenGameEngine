#include "ClaudeApiStream.h"

#include "CancelToken.h"
#include "ClaudeApiProvider.h"
#include "JsonFields.h"

#include <chrono>
#include <exception>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{
// The stall limit (HttpClient::PostOptions::Timeout for a stream): a model may
// think for minutes before its first token, and a reply that keeps arriving is
// never cut off.
constexpr std::chrono::milliseconds kStreamStallTimeout = std::chrono::minutes(10);
constexpr size_t kBytesPerMiB = size_t{1024} * 1024;

// The event types OnEvent reads. Every other type is skipped before its data
// is parsed, so a type the API adds later cannot fail a turn.
bool IsMappedEvent(std::string_view type)
{
    return type == "message_start" || type == "content_block_start" || type == "content_block_delta" ||
           type == "message_delta" || type == "message_stop" || type == "error";
}

// The Note for a reply that ended for a `stop_reason` other than a finished answer.
std::string StopReasonNote(std::string_view stopReason)
{
    if (stopReason == "max_tokens")
        return "The reply stopped at the " + std::to_string(ClaudeApiProvider::kMaxReplyTokens) +
               "-token limit; ask the model to continue or for a shorter answer.";
    if (stopReason == "refusal")
        return "The model declined to answer this request.";
    return {};
}

// "overloaded_error: Overloaded" from the API's {"type":"error","error":{...}} shape.
std::string DescribeApiError(const nlohmann::json& payload)
{
    const nlohmann::json& error = JsonFields::Object(payload, "error");
    const std::string type = JsonFields::String(error, "type");
    const std::string message = JsonFields::String(error, "message");
    if (type.empty())
        return message;
    return message.empty() ? type : type + ": " + message;
}
} // namespace

void ClaudeApiStream::Run(const std::string& url,
                          const std::unordered_map<std::string, std::string>& headers,
                          const std::string& body,
                          AgentTurnEvents& events,
                          const CancelToken& cancel)
{
    HttpClient::PostOptions options;
    options.Timeout = kStreamStallTimeout;
    options.MaxResponseBytes = ClaudeApiStream::kMaxResponseBytes;
    options.Cancellation = &cancel.Flag();

    ClaudeApiStream stream(events);
    HttpClient::HttpResponse response;
    try
    {
        response = HttpClient::PostJsonStream(url, headers, body, options,
                                              [&stream](std::string_view chunk) { stream.Feed(chunk); });
    }
    catch (const std::exception& error)
    {
        response.error = error.what();
    }
    stream.Finish(response);
}

ClaudeApiStream::ClaudeApiStream(AgentTurnEvents& events) : m_Events(events)
{
}

void ClaudeApiStream::Feed(std::string_view bytes)
{
    m_Parser.Feed(bytes, [this](const SseEvent& event) { OnEvent(event); });
}

void ClaudeApiStream::OnEvent(const SseEvent& event)
{
    if (m_Failed || !IsMappedEvent(event.Type))
        return;
    const nlohmann::json payload = nlohmann::json::parse(event.Data, nullptr, false);
    if (!payload.is_object())
    {
        Fail("The Claude API sent a '" + event.Type + "' event that is not a JSON object.");
        return;
    }

    if (event.Type == "message_start")
    {
        const nlohmann::json& usage = JsonFields::Object(JsonFields::Object(payload, "message"), "usage");
        m_Result.InputTokens = JsonFields::Count(usage, "input_tokens") + JsonFields::Count(usage, "cache_creation_input_tokens") +
                               JsonFields::Count(usage, "cache_read_input_tokens");
        m_Result.OutputTokens = JsonFields::Count(usage, "output_tokens");
        m_Result.ModelRoundTrips = 1;
    }
    else if (event.Type == "content_block_start")
    {
        const nlohmann::json& block = JsonFields::Object(payload, "content_block");
        const std::string text = JsonFields::String(block, "text");
        if (JsonFields::String(block, "type") == "text" && !text.empty())
        {
            m_Result.Text += text;
            m_Events.OnTextDelta(text);
        }
    }
    else if (event.Type == "content_block_delta")
    {
        const nlohmann::json& delta = JsonFields::Object(payload, "delta");
        if (JsonFields::String(delta, "type") != "text_delta")
            return;
        const std::string text = JsonFields::String(delta, "text");
        m_Result.Text += text;
        m_Events.OnTextDelta(text);
    }
    else if (event.Type == "message_delta")
    {
        // output_tokens here is the turn's running total, not an increment.
        const nlohmann::json& usage = JsonFields::Object(payload, "usage");
        if (usage.contains("output_tokens"))
            m_Result.OutputTokens = JsonFields::Count(usage, "output_tokens");
        const std::string stopReason = JsonFields::String(JsonFields::Object(payload, "delta"), "stop_reason");
        if (!stopReason.empty())
            m_StopReason = stopReason;
    }
    else if (event.Type == "message_stop")
    {
        m_MessageStopped = true;
    }
    else if (event.Type == "error")
    {
        Fail("The Claude API ended the reply with " + DescribeApiError(payload) + ". Send the message again.");
    }
}

void ClaudeApiStream::Fail(std::string message)
{
    m_Failed = true;
    m_Result.Error = std::move(message);
}

void ClaudeApiStream::Finish(const HttpClient::HttpResponse& response)
{
    TurnResult result = std::move(m_Result);
    if (response.cancelled)
    {
        result.Outcome = TurnOutcome::Stopped;
        result.Error.clear();
    }
    else if (m_Failed)
    {
        result.Outcome = TurnOutcome::Failed;
    }
    else if (response.overflowed)
    {
        result.Outcome = TurnOutcome::Failed;
        result.Error = "The Claude API reply passed " + std::to_string(kMaxResponseBytes / kBytesPerMiB) +
                       " MiB, more than a " + std::to_string(ClaudeApiProvider::kMaxReplyTokens) +
                       "-token reply needs, so it was cut off. The text received so far is kept; send the "
                       "message again.";
    }
    else if (!response.success)
    {
        result.Outcome = TurnOutcome::Failed;
        if (response.statusCode >= 400)
        {
            const nlohmann::json payload = nlohmann::json::parse(response.body, nullptr, false);
            const std::string detail = payload.is_object() ? DescribeApiError(payload) : std::string{};
            result.Error = "The Claude API refused the request (HTTP " + std::to_string(response.statusCode) +
                           (detail.empty() ? std::string{} : ", " + detail) + ").";
            if (response.statusCode == 401)
                result.Error += " Check the key in " + std::string(ClaudeApiProvider::kApiKeyVariable) + ".";
        }
        else
        {
            result.Error = "The Claude API request failed: " +
                           (response.error.empty() ? "HTTP " + std::to_string(response.statusCode) : response.error);
        }
    }
    else if (!m_MessageStopped)
    {
        result.Outcome = TurnOutcome::Failed;
        result.Error = "The Claude API stream ended before the reply finished. Send the message again.";
    }
    else
    {
        result.Outcome = TurnOutcome::Succeeded;
        result.Note = StopReasonNote(m_StopReason);
    }
    m_Events.OnFinished(result);
}
} // namespace GameEngine
