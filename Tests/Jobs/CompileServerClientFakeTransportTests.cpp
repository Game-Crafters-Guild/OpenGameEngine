#include <gtest/gtest.h>
#include "Jobs/CompileServerClient.h"
#include "Jobs/CompileServerTestDoubles.h" // Tests/Jobs
#include "Jobs/CompileServerVersion.h"
#include "Jobs/IHotReloadTransport.h"

using namespace GameEngine;

namespace {
// Speaks the CompileServerClient wire protocol: the client sends a __version__
// handshake (and __shutdown__ when recycling a stale server) before the compile
// request, so the fake must answer those control messages or the client treats
// the canned response as a version mismatch and recycles the "server" until it
// gives up (same pattern as HotReloadPipelineMultiInstanceIsolationTests).
class FakeTransport : public IHotReloadTransport {
public:
    explicit FakeTransport(std::string response, bool connectOk = true)
        : m_Response(std::move(response)), m_ConnectOk(connectOk) {}
    bool Connect() override { return m_ConnectOk; }
    bool SendRequest(const std::string& requestJson, std::string& outResponse) override {
        if (!m_ConnectOk) return false;
        if (requestJson == "__version__") {
            outResponse = std::string("{\"Version\":\"") + kExpectedCompileServerVersion + "\"}";
            return true;
        }
        if (requestJson == "__shutdown__") {
            outResponse = "{}";
            return true;
        }
        outResponse = m_Response;
        return true;
    }
    void Close() override {}
private:
    std::string m_Response;
    bool m_ConnectOk;
};

// The version a resident server from before the AllowUnsafe/ImplicitUsings/
// Nullable request fields reports; such a server would drop those fields.
constexpr const char* kPreviousCompileServerVersion = "2026.07.17.2";

struct StaleServerLog {
    bool ShutdownSent = false;
    bool CompiledBeforeShutdown = false;
    bool CompiledAfterShutdown = false;
};

// A resident server of the previous version until the client asks it to shut
// down; the restarted one reports the expected version.
class StaleServerTransport : public IHotReloadTransport {
public:
    explicit StaleServerTransport(StaleServerLog& log) : m_Log(log) {}
    bool Connect() override { return true; }
    bool SendRequest(const std::string& requestJson, std::string& outResponse) override {
        if (requestJson == "__version__") {
            const char* version = m_Log.ShutdownSent ? kExpectedCompileServerVersion : kPreviousCompileServerVersion;
            outResponse = std::string("{\"Version\":\"") + version + "\"}";
            return true;
        }
        if (requestJson == "__shutdown__") {
            m_Log.ShutdownSent = true;
            outResponse = "ok";
            return true;
        }
        (m_Log.ShutdownSent ? m_Log.CompiledAfterShutdown : m_Log.CompiledBeforeShutdown) = true;
        outResponse = R"({"Success": true})";
        return true;
    }
    void Close() override {}
private:
    StaleServerLog& m_Log;
};
}

TEST(CompileServerClientFakeTransport, RecyclesServerOfPreviousVersion) {
    StaleServerLog log;
    CompileServerClient client(std::make_unique<StaleServerTransport>(log), "FAKE_PIPE");
    CompileServerResponse resp;
    ASSERT_TRUE(client.Compile("{}", resp));
    EXPECT_TRUE(log.ShutdownSent);
    EXPECT_FALSE(log.CompiledBeforeShutdown);
    EXPECT_TRUE(log.CompiledAfterShutdown);
}

TEST(CompileServerClientFakeTransport, ParsesSuccessWithBytes) {
    // Response with success and small assembly byte payload
    std::string response = R"({
        "Success": true,
        "Warnings": [],
        "Errors": [],
        "AssemblyBytes": "AQIDBA==",
        "PdbBytes": ""
    })";

    auto transport = std::make_unique<FakeTransport>(response, /*connectOk*/true);
    CompileServerClient client(std::move(transport), "FAKE_PIPE");

    CompileServerResponse resp;
    bool ok = client.Compile("{}", resp);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(resp.Success);
    EXPECT_EQ(resp.AssemblyBytes.size(), 4u);
}

TEST(CompileServerClientFakeTransport, MalformedJsonReturnsFalse) {
    std::string response = "not a json";
    auto transport = std::make_unique<FakeTransport>(response, /*connectOk*/true);
    CompileServerClient client(std::move(transport), "FAKE_PIPE");

    CompileServerResponse resp;
    bool ok = client.Compile("{}", resp);
    EXPECT_FALSE(ok);
}



TEST(CompileServerClientFakeTransport, ConnectFailsReturnsFalse) {
    std::string response = R"({"Success":true})";
    auto transport = std::make_unique<FakeTransport>(response, /*connectOk*/false);
    CompileServerClient client(std::move(transport), "FAKE_PIPE");
    CompileServerResponse resp;
    bool ok = client.Compile("{}", resp);
    EXPECT_FALSE(ok);
}

TEST(CompileServerClientFakeTransport, ParsesSuccessWithOutputPath) {
    std::string response = R"({
        "Success": true,
        "OutputPath": "C:/temp/fake.dll"
    })";
    auto transport = std::make_unique<FakeTransport>(response, /*connectOk*/true);
    CompileServerClient client(std::move(transport), "FAKE_PIPE");
    CompileServerResponse resp;
    bool ok = client.Compile("{}", resp);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(resp.Success);
    EXPECT_EQ(resp.OutputPathUtf8, std::string("C:/temp/fake.dll"));
}


TEST(CompileServerClientFakeTransport, SuccessWithoutBytesOrPath) {
    std::string response = R"({"Success": true})";
    auto transport = std::make_unique<FakeTransport>(response, /*connectOk*/true);
    CompileServerClient client(std::move(transport), "FAKE_PIPE");
    CompileServerResponse resp;
    bool ok = client.Compile("{}", resp);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(resp.Success);
    EXPECT_TRUE(resp.AssemblyBytes.empty());
    EXPECT_TRUE(resp.OutputPathUtf8.empty());
}

namespace {
const char* kFailingResponse =
    R"({"Success":false,"Errors":[{"Code":"CS1002","FileUtf8":"C:/p/A.cs","Line":3,"Column":5,"MessageUtf8":"; expected"}]})";

void CompileFailing(const std::string& requestJson) {
    CompileServerClient client(std::make_unique<FakeTransport>(kFailingResponse), "FAKE_PIPE");
    CompileServerResponse resp;
    EXPECT_FALSE(client.Compile(requestJson, resp));
}
}

// Every request's notifications carry the assembly it compiles, and a retried
// request starts again before its batch, so a consumer replaces that
// assembly's rows rather than doubling them.
TEST(CompileServerClientFakeTransport, NotificationsCarryTheRequestsAssemblyName) {
    Tests::CompileNotificationLog log;
    CompileFailing(R"({"AssemblyName":"WatchedPack"})");
    CompileFailing(R"({"AssemblyName":"GameEngine.Editor"})");
    CompileFailing(R"({"AssemblyName":"WatchedPack"})");
    EXPECT_EQ(log.Entries(), (std::vector<std::string>{
                                 "started WatchedPack", "batch WatchedPack 1",
                                 "started GameEngine.Editor", "batch GameEngine.Editor 1",
                                 "started WatchedPack", "batch WatchedPack 1"}));
}

// The hot-reload pipeline's project-scripts request names no assembly.
TEST(CompileServerClientFakeTransport, AnUnnamedRequestIsTheScriptsAssembly) {
    Tests::CompileNotificationLog log;
    CompileFailing(R"({"ProjectRoot":"C:/p"})");
    EXPECT_EQ(log.Entries(), (std::vector<std::string>{"started GameEngine.Scripts", "batch GameEngine.Scripts 1"}));
}
