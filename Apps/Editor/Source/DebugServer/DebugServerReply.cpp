#include "DebugServer/DebugServerReply.h"

#include "JobSystem/WorkStealingThreadPool.h"

#include <memory>
#include <utility>

namespace GameEngine::Editor
{

namespace
{

constexpr const char* kRefusalKey = "__refusal";
constexpr const char* kReasonKey = "reason";
constexpr const char* kDetailsKey = "details";
constexpr const char* kSerializedReplyKey = "__serialized";
constexpr const char* kGateResponseKey = "__gateResponse";
// The capture handlers' PNG as base64: the client's alone, never the gates'.
constexpr const char* kImagePayloadKey = "pngBase64";

// A reply a job serialized: the line the client receives and the gates' copy of the response.
struct SerializedHandlerReply
{
    std::string Line;
    nlohmann::json GateResponse;
};

} // namespace

nlohmann::json RefuseRequest(std::string reason, nlohmann::json details)
{
    return nlohmann::json{{kRefusalKey, {{kReasonKey, std::move(reason)}, {kDetailsKey, std::move(details)}}}};
}

bool IsRefusal(const nlohmann::json& result)
{
    return result.is_object() && result.contains(kRefusalKey);
}

nlohmann::json HandlerResponse(const std::string& id, const nlohmann::json& result)
{
    if (!IsRefusal(result))
        return nlohmann::json{{"id", id}, {"ok", true}, {"result", result}};

    const nlohmann::json& refusal = result.at(kRefusalKey);
    nlohmann::json response{{"id", id}, {"ok", false}, {"error", refusal.at(kReasonKey)}};
    const nlohmann::json& details = refusal.at(kDetailsKey);
    if (!details.empty())
        response["details"] = details;
    return response;
}

std::string SerializeHandlerResult(const std::string& id, const nlohmann::json& result)
{
    return HandlerResponse(id, result).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

nlohmann::json SerializedReply(std::string line, nlohmann::json gateResponse)
{
    nlohmann::json marker = nlohmann::json::object();
    marker[kSerializedReplyKey] = std::move(line);
    marker[kGateResponseKey] = std::move(gateResponse);
    return marker;
}

std::string* SerializedReplyLine(nlohmann::json& result)
{
    if (!result.is_object())
        return nullptr;
    const auto line = result.find(kSerializedReplyKey);
    if (line == result.end() || !line->is_string())
        return nullptr;
    return &line->get_ref<std::string&>();
}

const nlohmann::json* SerializedReplyGateResponse(const nlohmann::json& result)
{
    if (!result.is_object())
        return nullptr;
    const auto response = result.find(kGateResponseKey);
    return response == result.end() || !response->is_object() ? nullptr : &*response;
}

JobSystem::TaskHandle SubmitHandlerReplyJob(JobSystem::WorkStealingThreadPool& jobs,
                                           std::string requestId,
                                           std::function<nlohmann::json()> buildResult)
{
    return jobs.Submit([requestId = std::move(requestId), buildResult = std::move(buildResult)]
    {
        nlohmann::json response = HandlerResponse(requestId, buildResult());
        auto reply = std::make_shared<SerializedHandlerReply>();
        reply->Line = response.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
        if (const auto result = response.find("result"); result != response.end() && result->is_object())
            result->erase(kImagePayloadKey);
        reply->GateResponse = std::move(response);
        return reply;
    });
}

bool PollHandlerReplyJob(const JobSystem::TaskHandle& job, nlohmann::json& outResult,
                         std::string& error)
{
    if (!job.IsValid())
    {
        error = "the job system did not accept the reply work";
        return true;
    }
    if (!job.IsDone())
        return false;
    std::shared_ptr<SerializedHandlerReply> reply;
    if (!job.TryGetResult(reply) || !reply)
    {
        error = job.GetErrorMessage();
        if (error.empty())
            error = "the reply job finished without a serialized result";
        return true;
    }
    error.clear();
    outResult = SerializedReply(std::move(reply->Line), std::move(reply->GateResponse));
    return true;
}
} // namespace GameEngine::Editor
