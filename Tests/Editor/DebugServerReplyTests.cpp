// How a debug-server handler's result becomes the response a client reads.
//
// A handler refuses with Editor::RefuseRequest, and HandlerResponse sends that as ok:false.
// The refusal is recognised by the reserved member RefuseRequest writes, never by a payload's
// own fields, so a result that legitimately carries an "error" member — a sub-operation's
// outcome, a per-row status — still answers ok:true.
//
// The source scan pins the other half. With the refusal keyed on RefuseRequest, a handler that
// spells a refusal as a result object whose first member is "error" answers ok:true, and the
// caller reads a success. No debug-server source may build one.
#include <gtest/gtest.h>

#include "DebugServer/DebugServerReply.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace GameEngine;
using nlohmann::json;

namespace
{

constexpr std::string_view kErrorKey = "\"error\"";

// These build the wire response itself, so they write the response's own "error" member.
const char* const kWireResponseSources[] = {"EditorDebugServer.cpp", "DebugServerReply.cpp"};

bool IsWireResponseSource(const std::filesystem::path& path)
{
    for (const char* name : kWireResponseSources)
        if (path.filename() == name)
            return true;
    return false;
}

std::string ReadSource(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool IsSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

size_t SkipSpaceBackward(const std::string& src, size_t end)
{
    while (end > 0 && IsSpace(src[end - 1]))
        --end;
    return end;
}

size_t SkipSpaceForward(const std::string& src, size_t at)
{
    while (at < src.size() && IsSpace(src[at]))
        ++at;
    return at;
}

bool EndsWith(const std::string& src, size_t end, const std::string& suffix)
{
    return end >= suffix.size() && src.compare(end - suffix.size(), suffix.size(), suffix) == 0;
}

// `json{{"error", ...}, ...}`: "error" as the first member of an object literal.
bool IsErrorFirstMember(const std::string& src, size_t key)
{
    const size_t afterKey = SkipSpaceForward(src, key + kErrorKey.size());
    if (afterKey >= src.size() || src[afterKey] != ',')
        return false;
    const size_t pairBrace = SkipSpaceBackward(src, key);
    if (pairBrace == 0 || src[pairBrace - 1] != '{')
        return false;
    const size_t objectBrace = SkipSpaceBackward(src, pairBrace - 1);
    if (objectBrace == 0 || src[objectBrace - 1] != '{')
        return false;
    return EndsWith(src, SkipSpaceBackward(src, objectBrace - 1), "json");
}

// `result["error"] = ...`: an "error" member assigned onto a result.
bool IsErrorMemberAssignment(const std::string& src, size_t key)
{
    const size_t open = SkipSpaceBackward(src, key);
    if (open == 0 || src[open - 1] != '[')
        return false;
    const size_t close = SkipSpaceForward(src, key + kErrorKey.size());
    if (close >= src.size() || src[close] != ']')
        return false;
    const size_t assign = SkipSpaceForward(src, close + 1);
    return assign + 1 < src.size() && src[assign] == '=' && src[assign + 1] != '=';
}

size_t LineOfOffset(const std::string& src, size_t offset)
{
    size_t line = 1;
    for (size_t i = 0; i < offset && i < src.size(); ++i)
        if (src[i] == '\n')
            ++line;
    return line;
}

std::vector<size_t> ErrorMemberRefusals(const std::string& src)
{
    std::vector<size_t> lines;
    for (size_t at = src.find(kErrorKey); at != std::string::npos; at = src.find(kErrorKey, at + 1))
        if (IsErrorFirstMember(src, at) || IsErrorMemberAssignment(src, at))
            lines.push_back(LineOfOffset(src, at));
    return lines;
}

} // namespace

TEST(DebugServerReplyTests, RefusalIsSentAsOkFalseWithItsReason)
{
    const json response = Editor::HandlerResponse("7", Editor::RefuseRequest("No world available"));

    EXPECT_EQ(response.value("id", std::string()), "7");
    EXPECT_EQ(response.value("ok", true), false);
    EXPECT_EQ(response.value("error", std::string()), "No world available");
    EXPECT_FALSE(response.contains("result")) << response.dump();
    EXPECT_FALSE(response.contains("details")) << "a refusal without details sends none: " << response.dump();
}

TEST(DebugServerReplyTests, RefusalDetailsTravelWithTheError)
{
    const json refusal = Editor::RefuseRequest("Panel not found: Foo", json{{"availablePanels", {"Hierarchy", "Inspector"}}});
    const json response = Editor::HandlerResponse("1", refusal);

    EXPECT_EQ(response.value("ok", true), false);
    EXPECT_EQ(response.value("error", std::string()), "Panel not found: Foo");
    ASSERT_TRUE(response.contains("details")) << response.dump();
    EXPECT_EQ(response["details"], (json{{"availablePanels", {"Hierarchy", "Inspector"}}}));
}

// A deferred poll that serialized its reply off the main thread hands the server the line,
// and the server sends it unchanged. That line must be byte-identical to the one the server
// would have serialized from the result itself, for a success and for a refusal alike, and a
// marker whose line is not a string is not taken.
TEST(DebugServerReplyTests, SerializedReplyCarriesTheLineTheServerWouldSend)
{
    for (const json& result : {json{{"width", 8}, {"method", "rendergraph"}, {"note", "caf\xc3\xa9"}},
                               Editor::RefuseRequest("PNG encoding failed", json{{"fallbackReason", "x"}})})
    {
        const std::string direct =
            Editor::HandlerResponse("42", result).dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
        json marker = Editor::SerializedReply(Editor::SerializeHandlerResult("42", result), json::object());
        const std::string* line = Editor::SerializedReplyLine(marker);
        ASSERT_NE(line, nullptr) << marker.dump();
        EXPECT_EQ(*line, direct);
    }

    json result{{"width", 8}};
    EXPECT_EQ(Editor::SerializedReplyLine(result), nullptr);
    json notAString{{"__serialized", 3}};
    EXPECT_EQ(Editor::SerializedReplyLine(notAString), nullptr);
}

// A payload whose own data has a top-level string "error" is a result, not a refusal, and
// reaches the caller whole.
TEST(DebugServerReplyTests, ResultCarryingItsOwnErrorMemberIsASuccess)
{
    const json result{{"error", "sub-operation timed out"}, {"attempts", 3}};
    ASSERT_FALSE(Editor::IsRefusal(result));

    const json response = Editor::HandlerResponse("2", result);
    EXPECT_EQ(response.value("ok", false), true);
    EXPECT_FALSE(response.contains("error")) << response.dump();
    EXPECT_EQ(response.value("result", json()), result);
}

TEST(DebugServerReplyTests, NonObjectResultsAreSuccesses)
{
    for (const json& result : {json::array({1, 2}), json(), json("text"), json(42)})
    {
        const json response = Editor::HandlerResponse("3", result);
        EXPECT_EQ(response.value("ok", false), true) << result.dump();
        EXPECT_EQ(response.value("result", json()), result);
    }
}

TEST(DebugServerReplyTests, NoDebugServerSourceSpellsARefusalAsAnErrorMember)
{
    const std::filesystem::path dir = std::filesystem::path(GE_EDITOR_SOURCE_DIR) / "Source" / "DebugServer";
    ASSERT_TRUE(std::filesystem::is_directory(dir)) << "debug-server sources not found at " << dir.string();

    size_t scannedFiles = 0;
    std::ostringstream offenders;
    for (const auto& entry : std::filesystem::directory_iterator(dir))
    {
        const std::filesystem::path& path = entry.path();
        if (!entry.is_regular_file() || (path.extension() != ".cpp" && path.extension() != ".h") ||
            IsWireResponseSource(path))
            continue;
        const std::string src = ReadSource(path);
        ASSERT_FALSE(src.empty()) << "could not read " << path.string();
        ++scannedFiles;
        for (size_t line : ErrorMemberRefusals(src))
            offenders << "\n  " << path.filename().string() << ":" << line;
    }

    // A scan that matched no files would pass for the wrong reason.
    EXPECT_GT(scannedFiles, 0u) << "no debug-server sources scanned";
    EXPECT_TRUE(offenders.str().empty())
        << "these build a result with an \"error\" member, which is sent as ok:true; "
           "refuse with Editor::RefuseRequest(reason, details) instead:"
        << offenders.str();
}

// The scanner is the instrument, so prove it sees the refusal idiom before trusting a zero,
// and that it leaves an "error" data member that is not the first member alone.
TEST(DebugServerReplyTests, TheScannerFindsTheErrorMemberIdiomAndNothingElse)
{
    const std::string sample =
        "return json{{\"error\", \"No world\"}};\n"
        "state->error = json{\n"
        "    {\"error\", \"no match\"}, {\"candidates\", c}};\n"
        "result[\"error\"] = \"did not start\";\n"
        "json lod{{\"indexCount\", n}, {\"error\", e.lodError[k]}};\n"
        "if (result[\"error\"] == x) {}\n"
        "{\"severity\", isError ? \"error\" : \"warning\"},\n";

    EXPECT_EQ(ErrorMemberRefusals(sample), (std::vector<size_t>{1, 3, 4}));
}

TEST(DebugServerReplyTests, ReplyJobBuildsAndSerializesItsOwnedResultOffTheCallerThread)
{
    JobSystem::WorkStealingThreadPool jobs(1);
    const auto caller = std::this_thread::get_id();
    for (const json& expected : {json{{"width", 8}, {"pngBase64", "pixels"}},
                                 Editor::RefuseRequest("PNG encoding failed")})
    {
        // Promise destruction releases the held worker on an assertion exit.
        std::promise<void> release;
        auto gate = release.get_future().share();
        auto entered = std::make_shared<std::promise<std::thread::id>>();
        auto thread = entered->get_future();
        auto job = Editor::SubmitHandlerReplyJob(jobs, "42",
            [reply = expected, gate, entered, caller]
            {
                const auto worker = std::this_thread::get_id();
                entered->set_value(worker);
                // The inline-work counterfactual must fail, not deadlock the test.
                if (worker != caller)
                    gate.wait();
                return reply;
            });
        ASSERT_EQ(thread.wait_for(std::chrono::seconds(10)), std::future_status::ready);
        EXPECT_NE(thread.get(), caller);
        json output{{"untouched", true}};
        std::string error;
        EXPECT_FALSE(Editor::PollHandlerReplyJob(job, output, error));
        EXPECT_EQ(output, (json{{"untouched", true}}));
        EXPECT_TRUE(error.empty());
        release.set_value();
        job.Wait();
        ASSERT_TRUE(Editor::PollHandlerReplyJob(job, output, error));
        ASSERT_TRUE(error.empty()) << error;
        const auto* line = Editor::SerializedReplyLine(output);
        ASSERT_NE(line, nullptr);
        EXPECT_EQ(*line, Editor::SerializeHandlerResult("42", expected));
    }
}

TEST(DebugServerReplyTests, ReplyJobFailureAndShutdownEndThePollWithAReason)
{
    JobSystem::WorkStealingThreadPool jobs(1);
    auto failed = Editor::SubmitHandlerReplyJob(jobs, "failed", []() -> json
    {
        throw std::runtime_error("encode operation failed");
    });
    failed.Wait();
    json output{{"untouched", true}};
    std::string error;
    EXPECT_TRUE(Editor::PollHandlerReplyJob(failed, output, error));
    EXPECT_NE(error.find("encode operation failed"), std::string::npos) << error;
    EXPECT_EQ(output, (json{{"untouched", true}}));

    jobs.Shutdown();
    bool called = false;
    auto refused = Editor::SubmitHandlerReplyJob(jobs, "stopped", [&called]
    {
        called = true;
        return json::object();
    });
    EXPECT_FALSE(refused.IsValid());
    error.clear();
    EXPECT_TRUE(Editor::PollHandlerReplyJob(refused, output, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(called);
}

TEST(DebugServerReplyTests, AReplyJobCanceledBeforeEncodingEndsThePoll)
{
    JobSystem::WorkStealingThreadPool jobs(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto entered = std::make_shared<std::promise<void>>();
    auto ready = entered->get_future();
    auto blocker = jobs.Submit([gate, entered]
    {
        entered->set_value();
        gate.wait();
    });
    ASSERT_EQ(ready.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    auto called = std::make_shared<std::atomic<bool>>(false);
    auto job = Editor::SubmitHandlerReplyJob(jobs, "canceled", [called]
    {
        called->store(true);
        return json::object();
    });
    ASSERT_TRUE(job.Cancel());
    json output;
    std::string error;
    EXPECT_TRUE(Editor::PollHandlerReplyJob(job, output, error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(output.is_null());
    EXPECT_FALSE(called->load());
    release.set_value();
    blocker.Wait();
}
