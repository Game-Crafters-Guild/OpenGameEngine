#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

namespace JobSystem
{
class TaskHandle;
class WorkStealingThreadPool;
}

namespace GameEngine::Editor
{

// How a debug-server handler (or a deferred poll) says no: it returns RefuseRequest(reason).
// The dispatcher sends that as {"id", "ok": false, "error": reason} and adds "details" when
// the refusal carries any — diagnostic fields for the caller, such as the candidates a lookup
// considered or the state that blocked the request.
//
// A refusal is recognised only by the reserved "__refusal" member RefuseRequest writes, never
// by a payload's own fields: a result that carries an "error" member of its own is still a
// success and goes out as ok:true.
nlohmann::json RefuseRequest(std::string reason, nlohmann::json details = nlohmann::json::object());

bool IsRefusal(const nlohmann::json& result);

// The response object for a handler's result: a refusal as ok:false with its reason (and
// details, when there are any), any other result as ok:true with the result unchanged.
nlohmann::json HandlerResponse(const std::string& id, const nlohmann::json& result);

// The ndjson line a handler result is sent as: HandlerResponse serialized, with any invalid
// UTF-8 replaced so an externally sourced string cannot fail the reply. Safe on any thread.
std::string SerializeHandlerResult(const std::string& id, const nlohmann::json& result);

// A deferred poll whose reply was serialized off the main thread (SerializeHandlerResult on a
// job) completes with SerializedReply(line, gateResponse); the server sends that line
// unchanged, so a multi-megabyte payload is never re-serialized on the frame, and hands the
// request gates `gateResponse`: the same response object without the result's PNG payload
// (`pngBase64`; `filePath` names the same bytes).
nlohmann::json SerializedReply(std::string line, nlohmann::json gateResponse);

// The line a SerializedReply marker carries, or null when `result` is not one.
std::string* SerializedReplyLine(nlohmann::json& result);

// The response a SerializedReply marker carries for the request gates, or null when `result`
// is not one.
const nlohmann::json* SerializedReplyGateResponse(const nlohmann::json& result);

/// Run an owned result builder and response serialization on the job system, with the
/// gates' copy of the response (SerializedReply). The builder must own everything it reads
/// after submission.
JobSystem::TaskHandle SubmitHandlerReplyJob(JobSystem::WorkStealingThreadPool& jobs,
                                           std::string requestId,
                                           std::function<nlohmann::json()> buildResult);

/// Never waits; false leaves both outputs unchanged while the job is pending.
/// True consumes a serialized reply, or sets error for a terminal failure.
bool PollHandlerReplyJob(const JobSystem::TaskHandle& job, nlohmann::json& outResult,
                         std::string& error);

} // namespace GameEngine::Editor
