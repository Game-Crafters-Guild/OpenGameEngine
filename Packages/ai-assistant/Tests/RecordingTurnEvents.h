#pragma once

#include "Providers/IAgentProvider.h"

#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
/// Records a turn's events as "kind:payload" lines in arrival order, and the result.
class RecordingTurnEvents : public AgentTurnEvents
{
public:
    void OnTextDelta(std::string_view text) override { Log.push_back("delta:" + std::string(text)); }
    void OnToolActivity(const ToolActivity& activity) override { Log.push_back("tool:" + activity.Name); }
    void OnToolFailed(const ToolFailure& failure) override
    {
        Log.push_back((failure.Refused ? "refused:" : "failed:") + failure.CallId + ":" + failure.Error);
    }
    void OnSessionId(std::string_view sessionId) override { Log.push_back("session:" + std::string(sessionId)); }
    void OnFinished(const TurnResult& result) override
    {
        Log.push_back("finished:" + result.Text);
        Result = result;
        ++FinishedCount;
    }

    std::vector<std::string> Log;
    TurnResult Result;
    int FinishedCount = 0;
};
} // namespace GameEngine
