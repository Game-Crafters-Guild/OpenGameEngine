#include "Providers/CancelToken.h"
#include "Providers/ClaudeApiProvider.h"
#include "Providers/ClaudeApiStream.h"
#include "RecordingTurnEvents.h"
#include "ScopedEnvironmentVariable.h"

#include "LoopbackServer.h"
#include "Platform/HttpClient.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{
using namespace std::chrono_literals;

// A reply in the Messages API's documented stream shape, written by hand: an
// (empty) thinking block the mapper must skip, then a text block in two deltas.
constexpr std::string_view kRecordedReply =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_fixture\",\"type\":\"message\",\"role\":"
    "\"assistant\",\"model\":\"claude-opus-5-5\",\"content\":[],\"stop_reason\":null,\"usage\":{\"input_tokens\":"
    "25,\"cache_creation_input_tokens\":0,\"cache_read_input_tokens\":5,\"output_tokens\":1}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"c2ln\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: ping\n"
    "data: {\"type\": \"ping\"}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\", world\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},"
    "\"usage\":{\"output_tokens\":12}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

const std::vector<std::string> kRecordedTurnLog = {"delta:Hello", "delta:, world", "finished:Hello, world"};

HttpClient::HttpResponse Completed()
{
    HttpClient::HttpResponse response;
    response.statusCode = 200;
    response.success = true;
    return response;
}

std::string ChunkedHead()
{
    return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n"
           "Connection: close\r\n\r\n";
}

std::string Chunk(std::string_view bytes)
{
    std::ostringstream size;
    size << std::hex << bytes.size();
    return size.str() + "\r\n" + std::string(bytes) + "\r\n";
}
} // namespace

TEST(ClaudeApiProviderTests, RecordedReplyYieldsTheTextDeltasInOrderAndTheAccounting)
{
    RecordingTurnEvents events;
    ClaudeApiStream stream(events);
    stream.Feed(kRecordedReply);
    stream.Finish(Completed());

    EXPECT_EQ(events.Log, kRecordedTurnLog);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Succeeded);
    EXPECT_EQ(events.Result.InputTokens, 30u);
    EXPECT_EQ(events.Result.OutputTokens, 12u);
    EXPECT_EQ(events.Result.ModelRoundTrips, 1u);
}

TEST(ClaudeApiProviderTests, ReplySplitAtEveryByteBoundaryYieldsTheSameTurn)
{
    for (size_t split = 0; split <= kRecordedReply.size(); ++split)
    {
        RecordingTurnEvents events;
        ClaudeApiStream stream(events);
        stream.Feed(kRecordedReply.substr(0, split));
        stream.Feed(kRecordedReply.substr(split));
        stream.Finish(Completed());
        ASSERT_EQ(events.Log, kRecordedTurnLog) << "split at byte " << split;
        ASSERT_EQ(events.Result.OutputTokens, 12u) << "split at byte " << split;
    }
}

TEST(ClaudeApiProviderTests, AnErrorEventEndsTheTurnWithItsMessage)
{
    const std::string_view reply =
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Partial\"}}\n\n"
        "event: error\n"
        "data: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}\n\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"late\"}}\n\n";

    RecordingTurnEvents events;
    ClaudeApiStream stream(events);
    stream.Feed(reply);
    stream.Finish(Completed());

    const std::vector<std::string> expected = {"delta:Partial", "finished:Partial"};
    EXPECT_EQ(events.Log, expected);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_NE(events.Result.Error.find("overloaded_error: Overloaded"), std::string::npos) << events.Result.Error;
}

TEST(ClaudeApiProviderTests, AStreamThatEndsBeforeMessageStopFailsKeepingTheText)
{
    const std::string_view stopFrame = "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
    ASSERT_TRUE(kRecordedReply.ends_with(stopFrame));

    RecordingTurnEvents events;
    ClaudeApiStream stream(events);
    stream.Feed(kRecordedReply.substr(0, kRecordedReply.size() - stopFrame.size()));
    stream.Finish(Completed());

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_EQ(events.Result.Text, "Hello, world");
    EXPECT_NE(events.Result.Error.find("ended before the reply finished"), std::string::npos) << events.Result.Error;
}

TEST(ClaudeApiProviderTests, AnUnknownEventIsSkippedWithoutReadingItsData)
{
    const std::string_view stopFrame = "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
    ASSERT_TRUE(kRecordedReply.ends_with(stopFrame));
    std::string reply(kRecordedReply.substr(0, kRecordedReply.size() - stopFrame.size()));
    reply += "event: future_event\ndata: not json\n\n";
    reply += stopFrame;

    RecordingTurnEvents events;
    ClaudeApiStream stream(events);
    stream.Feed(reply);
    stream.Finish(Completed());

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Succeeded) << events.Result.Error;
    EXPECT_EQ(events.Log, kRecordedTurnLog);
}

TEST(ClaudeApiProviderTests, AReplyCutAtTheTokenLimitOrDeclinedEndsWithANote)
{
    const std::string_view endTurn = "\"stop_reason\":\"end_turn\"";
    const size_t at = kRecordedReply.find(endTurn);
    ASSERT_NE(at, std::string_view::npos);

    const std::pair<std::string, std::string> cases[] = {{"max_tokens", "16384-token limit"},
                                                         {"refusal", "declined"}};
    for (const auto& [stopReason, noteText] : cases)
    {
        std::string reply(kRecordedReply);
        reply.replace(at, endTurn.size(), "\"stop_reason\":\"" + stopReason + "\"");

        RecordingTurnEvents events;
        ClaudeApiStream stream(events);
        stream.Feed(reply);
        stream.Finish(Completed());

        EXPECT_EQ(events.Result.Outcome, TurnOutcome::Succeeded) << stopReason;
        EXPECT_EQ(events.Result.Text, "Hello, world") << stopReason;
        EXPECT_NE(events.Result.Note.find(noteText), std::string::npos) << stopReason << ": " << events.Result.Note;
    }

    RecordingTurnEvents complete;
    ClaudeApiStream stream(complete);
    stream.Feed(kRecordedReply);
    stream.Finish(Completed());
    EXPECT_TRUE(complete.Result.Note.empty()) << complete.Result.Note;
}

TEST(ClaudeApiProviderTests, TheKeyTravelsInTheXApiKeyHeaderAndNoOther)
{
    const std::string key = "sk-ant-dummy-header-test";
    const std::unordered_map<std::string, std::string> expected = {
        {"x-api-key", key},
        {"anthropic-version", "2023-06-01"},
        {"accept", "text/event-stream"},
    };

    const std::unordered_map<std::string, std::string> headers = ClaudeApiProvider::BuildRequestHeaders(key);

    EXPECT_EQ(headers, expected);
    for (const auto& [name, value] : headers)
    {
        if (name != "x-api-key")
            EXPECT_EQ(value.find(key), std::string::npos) << name;
    }
}

TEST(ClaudeApiProviderTests, AReplyOneByteOverTheCapFailsWithTheProvidersMessageAndKeepsTheText)
{
    const std::string_view delta =
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"abcd\"}}\n\n";
    const std::string_view ending =
        "event: message_delta\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":64}}\n\n"
        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n\n";
    constexpr int kDeltaCount = 64;

    // The deltas, then comment lines the parser skips, then a complete ending,
    // in exactly one byte more than the cap: without the cap the turn succeeds.
    std::string body;
    std::string expectedText;
    for (int i = 0; i < kDeltaCount; ++i)
    {
        body += delta;
        expectedText += "abcd";
    }
    constexpr size_t kPaddingLine = 1024;
    size_t padding = ClaudeApiStream::kMaxResponseBytes + 1 - body.size() - ending.size();
    while (padding >= 2 * kPaddingLine)
    {
        body += ":" + std::string(kPaddingLine - 2, 'x') + "\n";
        padding -= kPaddingLine;
    }
    body += ":" + std::string(padding - 2, 'x') + "\n";
    body += ending;
    ASSERT_EQ(body.size(), ClaudeApiStream::kMaxResponseBytes + 1);

    LoopbackServer server([&](LoopbackServer&, SocketHandle client) {
        LoopbackServer::Send(client, ChunkedHead() + Chunk(body) + "0\r\n\r\n");
    });
    RecordingTurnEvents events;
    CancelToken cancel;

    ClaudeApiStream::Run(server.Url(), {}, "{}", events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_EQ(events.Result.Text, expectedText);
    EXPECT_NE(events.Result.Error.find("passed 8 MiB"), std::string::npos) << events.Result.Error;
    EXPECT_EQ(events.Result.Error.find("PostOptions"), std::string::npos) << events.Result.Error;
}

TEST(ClaudeApiProviderTests, TheEndpointIsTheFixedHttpsMessagesApi)
{
    EXPECT_EQ(ClaudeApiProvider::kEndpoint, "https://api.anthropic.com/v1/messages");
    EXPECT_FALSE(ClaudeApiProvider().Capabilities().OwnsTranscript);
    EXPECT_TRUE(ClaudeApiProvider().Capabilities().NeedsKey);
}

TEST(ClaudeApiProviderTests, RequestBodyStreamsTheSystemPromptHistoryAndUserTurn)
{
    const std::vector<ConversationMessage> history = {
        {.Role = MessageRole::User, .Text = "first"},
        {.Role = MessageRole::Assistant, .Text = ""},
        {.Role = MessageRole::Assistant, .Text = "reply"},
    };
    AgentTurnRequest request;
    request.SystemPrompt = "Be brief.";
    request.History = history;
    request.UserText = "second";

    const nlohmann::json body = nlohmann::json::parse(ClaudeApiProvider::BuildRequestBody(request));

    EXPECT_EQ(body["model"], "claude-opus-5-5");
    EXPECT_EQ(body["max_tokens"], 16384);
    EXPECT_EQ(body["stream"], true);
    EXPECT_EQ(body["system"], "Be brief.");
    const nlohmann::json expectedMessages = nlohmann::json::parse(
        R"([{"role":"user","content":"first"},{"role":"assistant","content":"reply"},{"role":"user","content":"second"}])");
    EXPECT_EQ(body["messages"], expectedMessages);
    EXPECT_FALSE(body.contains("temperature"));
}

TEST(ClaudeApiProviderTests, WithoutTheEnvironmentKeyTheTurnIsRefusedNamingTheVariable)
{
    const ScopedEnvironmentVariable noKey("ANTHROPIC_API_KEY", nullptr);
    AgentTurnRequest request;
    request.UserText = "Hello";
    RecordingTurnEvents events;
    CancelToken cancel;

    ClaudeApiProvider().RunTurn(request, events, cancel);

    EXPECT_EQ(events.FinishedCount, 1);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_NE(events.Result.Error.find("set ANTHROPIC_API_KEY"), std::string::npos) << events.Result.Error;
}

TEST(ClaudeApiProviderTests, RecordedReplyStreamedFromALoopbackServerYieldsTheSameTurn)
{
    // Three uneven pieces, so event boundaries and chunk boundaries disagree.
    LoopbackServer server([](LoopbackServer&, SocketHandle client) {
        LoopbackServer::Send(client, ChunkedHead());
        LoopbackServer::Send(client, Chunk(kRecordedReply.substr(0, 101)));
        LoopbackServer::Send(client, Chunk(kRecordedReply.substr(101, 700)));
        LoopbackServer::Send(client, Chunk(kRecordedReply.substr(801)));
        LoopbackServer::Send(client, "0\r\n\r\n");
    });
    RecordingTurnEvents events;
    CancelToken cancel;

    ClaudeApiStream::Run(server.Url("/v1/messages"), {{"anthropic-version", "2023-06-01"}}, "{}", events, cancel);

    EXPECT_EQ(events.Log, kRecordedTurnLog);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Succeeded) << events.Result.Error;
    EXPECT_EQ(server.RequestCount(), 1);
}

TEST(ClaudeApiProviderTests, ARefusedRequestFailsWithTheApiMessageAndTheFix)
{
    const std::string errorBody =
        R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})";
    LoopbackServer server([&](LoopbackServer&, SocketHandle client) {
        LoopbackServer::Send(client, "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: " +
                                         std::to_string(errorBody.size()) + "\r\nConnection: close\r\n\r\n" +
                                         errorBody);
    });
    RecordingTurnEvents events;
    CancelToken cancel;

    ClaudeApiStream::Run(server.Url(), {}, "{}", events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_NE(events.Result.Error.find("HTTP 401, authentication_error: invalid x-api-key"), std::string::npos)
        << events.Result.Error;
    EXPECT_NE(events.Result.Error.find("ANTHROPIC_API_KEY"), std::string::npos) << events.Result.Error;
}

TEST(ClaudeApiProviderTests, CancellingMidStreamStopsTheTurnWithTheTextSoFar)
{
    // The server sends the first delta, then goes quiet until the test ends:
    // only the cancel token can end the turn.
    const std::string_view firstDelta =
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}\n\n";
    LoopbackServer server([&](LoopbackServer& self, SocketHandle client) {
        LoopbackServer::Send(client, ChunkedHead() + Chunk(firstDelta));
        self.WaitForSignals(1, 30s);
    });

    class CancelOnFirstDelta final : public RecordingTurnEvents
    {
    public:
        explicit CancelOnFirstDelta(CancelToken& token) : m_Token(token) {}
        void OnTextDelta(std::string_view text) override
        {
            RecordingTurnEvents::OnTextDelta(text);
            m_Token.Cancel();
        }

    private:
        CancelToken& m_Token;
    };
    CancelToken cancel;
    CancelOnFirstDelta events(cancel);

    const auto start = std::chrono::steady_clock::now();
    ClaudeApiStream::Run(server.Url(), {}, "{}", events, cancel);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    const std::vector<std::string> expected = {"delta:Hello", "finished:Hello"};
    EXPECT_EQ(events.Log, expected);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Stopped);
    EXPECT_TRUE(events.Result.Error.empty()) << events.Result.Error;
    EXPECT_LT(elapsed, 10s);
}
} // namespace GameEngine
