#include "Providers/CancelToken.h"
#include "Providers/IAgentProvider.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace GameEngine
{
namespace
{
/// Answers every turn with the session id, the user's text in two deltas around a
/// tool call, and the result: the shape a streaming CLI session produces.
class FakeSessionProvider final : public IAgentProvider
{
public:
    std::string_view Id() const override { return "fake-session"; }

    ProviderCapabilities Capabilities() const override
    {
        return {.Streams = true, .OwnsTranscript = true, .NeedsKey = false, .CanActWithTools = false};
    }

    void RunTurn(const AgentTurnRequest& request, AgentTurnEvents& events, const CancelToken&) override
    {
        events.OnSessionId(request.SessionId.empty() ? "session-1" : request.SessionId);
        const std::string_view text = request.UserText;
        const size_t half = text.size() / 2;
        events.OnTextDelta(text.substr(0, half));
        events.OnToolActivity({.Name = "markup_list", .InputJson = "{}"});
        events.OnTextDelta(text.substr(half));

        TurnResult result;
        result.Outcome = TurnOutcome::Succeeded;
        result.Text = request.UserText;
        result.ModelRoundTrips = 2;
        events.OnFinished(result);
    }
};

class RecordingEvents final : public AgentTurnEvents
{
public:
    void OnTextDelta(std::string_view text) override { Log.push_back("delta:" + std::string(text)); }
    void OnToolActivity(const ToolActivity& activity) override { Log.push_back("tool:" + activity.Name); }
    void OnSessionId(std::string_view sessionId) override { Log.push_back("session:" + std::string(sessionId)); }
    void OnFinished(const TurnResult& result) override
    {
        Log.push_back("finished:" + result.Text);
        Result = result;
    }

    std::vector<std::string> Log;
    TurnResult Result;
};
} // namespace

TEST(AgentProviderTests, FakeProviderTurnReportsEventsInOrderThroughTheInterface)
{
    FakeSessionProvider fake;
    IAgentProvider& provider = fake;
    const std::vector<ConversationMessage> history = {
        {.Role = MessageRole::User, .Text = "first"},
        {.Role = MessageRole::Assistant, .Text = "reply", .ProviderId = "fake-session"},
    };
    AgentTurnRequest request;
    request.History = history;
    request.UserText = "Hello";
    request.SessionId = "session-7";
    RecordingEvents events;
    CancelToken cancel;

    provider.RunTurn(request, events, cancel);

    const std::vector<std::string> expected = {
        "session:session-7", "delta:He", "tool:markup_list", "delta:llo", "finished:Hello",
    };
    EXPECT_EQ(events.Log, expected);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Succeeded);
    EXPECT_EQ(events.Result.ModelRoundTrips, 2u);
    EXPECT_TRUE(provider.Capabilities().OwnsTranscript);
    EXPECT_EQ(provider.Id(), "fake-session");
}
} // namespace GameEngine
