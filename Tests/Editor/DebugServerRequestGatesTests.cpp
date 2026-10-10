// EditorDebugServer's request gates over a real loopback connection, with the mark-ups'
// attribution gate as the editor registers it: a request runs inside the Agent scope and
// the scope closes when the handler returns or throws, a deferred handler included; the
// input-injection methods run outside it (issue #3289); a refusing gate keeps the handler
// from running and answers the client with its reason; a parked request waits, with its
// connection's later requests behind it, and is dropped when the connection closes; the
// gates hear each response, a deferred one included, and a reply serialized on a job as the
// client's response without its PNG payload; an assistant's binding is admitted only by a
// gate that claims it.

#include "DebugServer/AssistantSessionHandlers.h"
#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"

#include "Components/Markup/Markup.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Editor/Registries/DebugRequestGateRegistry.h"
#include "EditorChangeNotifications.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "MarkupECS/MarkupService.h"
#include "Markups/MarkupEditorBridge.h"
#include "Markups/MarkupRequestGate.h"

#include <gtest/gtest.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine;

namespace
{

#ifdef _WIN32
using SocketType = SOCKET;
constexpr SocketType kInvalidSocket = INVALID_SOCKET;
void CloseSocket(SocketType socket) { closesocket(socket); }
#else
using SocketType = int;
constexpr SocketType kInvalidSocket = -1;
void CloseSocket(SocketType socket) { ::close(socket); }
#endif

constexpr auto kDeadline = std::chrono::seconds(10);

sockaddr_in Loopback(uint16_t port)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    return address;
}

// A port the socket stack just handed out and released.
uint16_t FreeLoopbackPort()
{
    const SocketType socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kInvalidSocket)
        return 0;
    sockaddr_in address = Loopback(0);
#ifdef _WIN32
    int length = static_cast<int>(sizeof(address));
#else
    socklen_t length = sizeof(address);
#endif
    uint16_t port = 0;
    if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
        getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) == 0)
        port = ntohs(address.sin_port);
    CloseSocket(socket);
    return port;
}

class DebugServerRequestGatesTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
#ifdef _WIN32
        WSADATA wsa{};
        ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &wsa), 0);
#endif
        MarkupECS::MarkupService::Initialize();
        m_Bridge = std::make_unique<Editor::MarkupEditorBridge>(
            m_Notifications, []() { return int64{1000}; }, []() { return std::optional<std::filesystem::path>(); });
        Editor::MarkupEditorBridge::Install(m_Bridge.get());
        m_Lake = m_World.CreateEntity();
        m_World.AddComponentImmediate(m_Lake, Components::Markup{});
        ASSERT_TRUE(MarkupECS::MarkupService::Get().BeginMarkup(m_World, m_Lake, Components::MarkupAuthor::User, 1000));
        Editor::RegisterMarkupRequestGate(m_Gates);
    }

    void TearDown() override
    {
        if (m_Client != kInvalidSocket)
            CloseSocket(m_Client);
        m_Server.Stop();
        m_Bridge.reset();
        MarkupECS::MarkupService::Shutdown();
#ifdef _WIN32
        WSACleanup();
#endif
    }

    void StartAndConnect()
    {
        m_Port = FreeLoopbackPort();
        ASSERT_NE(m_Port, 0);
        ASSERT_TRUE(m_Server.Start(m_Port));
        m_Client = Connect();
        ASSERT_NE(m_Client, kInvalidSocket);
    }

    // A second, non-blocking connection to the started server; the caller closes it.
    SocketType Connect()
    {
        const SocketType client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (client == kInvalidSocket)
            return client;
        sockaddr_in address = Loopback(m_Port);
        if (connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        {
            CloseSocket(client);
            return kInvalidSocket;
        }
#ifdef _WIN32
        u_long nonBlocking = 1;
        ioctlsocket(client, FIONBIO, &nonBlocking);
#else
        fcntl(client, F_SETFL, fcntl(client, F_GETFL, 0) | O_NONBLOCK);
#endif
        return client;
    }

    void Send(const std::string& id, const std::string& method) { SendOn(m_Client, id, method); }

    void SendOn(SocketType client, const std::string& id, const std::string& method)
    {
        const std::string request = "{\"id\":\"" + id + "\",\"method\":\"" + method + "\",\"params\":{}}\n";
        ASSERT_EQ(send(client, request.data(), static_cast<int>(request.size()), 0),
                  static_cast<int>(request.size()));
    }

    // Runs the server's frames until `done` holds or the deadline passes.
    bool FlushUntil(const std::function<bool()>& done)
    {
        const auto deadline = std::chrono::steady_clock::now() + kDeadline;
        while (!done() && std::chrono::steady_clock::now() < deadline)
        {
            m_Server.FlushPendingRequests();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return done();
    }

    // The next reply line, parsed (discarded when the deadline passes first); the server's
    // frames run while it waits, since they are what sends the reply.
    nlohmann::json ReadReply()
    {
        std::string line;
        const auto deadline = std::chrono::steady_clock::now() + kDeadline;
        while (std::chrono::steady_clock::now() < deadline)
        {
            m_Server.FlushPendingRequests();
            char c = 0;
            while (recv(m_Client, &c, 1, 0) == 1)
            {
                if (c == '\n')
                    return nlohmann::json::parse(line, nullptr, false);
                line.push_back(c);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return nlohmann::json::parse(line, nullptr, false);
    }

    // A user's edit to the lake mark-up outside any request, and the author it was stamped with.
    Components::MarkupAuthor UserEditAuthor()
    {
        m_Notifications.NotifyComponentChange<Components::Markup>(&m_World, m_Lake,
                                                                  Editor::EditorChangeNotifications::ChangeKind::Commit);
        return m_World.GetComponent<Components::Markup>(m_Lake)->UpdatedBy;
    }

    Editor::EditorChangeNotifications m_Notifications;
    std::unique_ptr<Editor::MarkupEditorBridge> m_Bridge;
    ECS::World m_World;
    ECS::EntityHandle m_Lake;
    Editor::DebugRequestGateRegistry m_Gates;
    EditorDebugServer m_Server{m_Gates};
    SocketType m_Client = kInvalidSocket;
    uint16_t m_Port = 0;
};

} // namespace

TEST_F(DebugServerRequestGatesTest, AThrowingHandlerStillClosesTheAgentScope)
{
    bool handled = false;
    m_Server.RegisterHandler("markup_comment", [&](const EditorDebugServer::RequestContext&) -> nlohmann::json {
        handled = true;
        EXPECT_EQ(m_Bridge->CurrentAuthor(), Components::MarkupAuthor::Agent);
        throw std::runtime_error("handler failure");
    });
    StartAndConnect();
    Send("1", "markup_comment");

    ASSERT_TRUE(FlushUntil([&] { return handled; })) << "the request never reached the handler";
    EXPECT_EQ(m_Bridge->CurrentAuthor(), Components::MarkupAuthor::User);
    EXPECT_EQ(UserEditAuthor(), Components::MarkupAuthor::User);
}

// Issue #3289: input_text (like send_key) routes its event inside the request, so the edit
// it causes is made during the handler; it is the user's. A port edit stays the agent's.
TEST_F(DebugServerRequestGatesTest, InjectedInputIsTheUsersAndAPortEditIsTheAgents)
{
    std::optional<Components::MarkupAuthor> typed;
    std::optional<Components::MarkupAuthor> commented;
    const auto editTheLake = [this](std::optional<Components::MarkupAuthor>& stamped) {
        m_Notifications.NotifyComponentChange<Components::Markup>(&m_World, m_Lake,
                                                                  Editor::EditorChangeNotifications::ChangeKind::Commit);
        stamped = m_World.GetComponent<Components::Markup>(m_Lake)->UpdatedBy;
        return nlohmann::json::object();
    };
    m_Server.RegisterHandler("input_text",
                             [&](const EditorDebugServer::RequestContext&) { return editTheLake(typed); });
    m_Server.RegisterHandler("markup_comment",
                             [&](const EditorDebugServer::RequestContext&) { return editTheLake(commented); });
    StartAndConnect();
    Send("1", "input_text");
    Send("2", "markup_comment");

    ASSERT_TRUE(FlushUntil([&] { return typed && commented; }));
    EXPECT_EQ(*typed, Components::MarkupAuthor::User);
    EXPECT_EQ(*commented, Components::MarkupAuthor::Agent);
}

// The scope closes when a deferred handler returns, not when its response is sent: the
// user's edits in the frames between are the user's.
TEST_F(DebugServerRequestGatesTest, ADeferredHandlerClosesTheAgentScopeBeforeItsResponse)
{
    bool handled = false;
    bool ready = false;
    m_Server.RegisterHandler("take_screenshot", [&](const EditorDebugServer::RequestContext& ctx) {
        handled = true;
        EXPECT_EQ(m_Bridge->CurrentAuthor(), Components::MarkupAuthor::Agent);
        m_Server.EnqueueDeferredResponse(ctx.id, [&](nlohmann::json& result) {
            result = nlohmann::json{{"captured", true}};
            return ready;
        });
        return EditorDebugServer::DeferredMarker();
    });
    StartAndConnect();
    Send("1", "take_screenshot");

    ASSERT_TRUE(FlushUntil([&] { return handled; }));
    m_Server.FlushPendingRequests();
    EXPECT_EQ(UserEditAuthor(), Components::MarkupAuthor::User);
    ready = true;
    const nlohmann::json reply = ReadReply();
    EXPECT_EQ(reply.value("ok", false), true) << reply.dump();
}

TEST_F(DebugServerRequestGatesTest, ARefusingGateAnswersWithItsReasonAndTheHandlerNeverRuns)
{
    Editor::DebugRequestGate refuses;
    refuses.GateId = "refuses";
    refuses.Before = [](const Editor::DebugRequestGateContext&) {
        return Editor::DebugRequestVerdict{Editor::DebugRequestDecision::Refuse, "Stop play mode, then retry."};
    };
    m_Gates.Register(std::move(refuses));
    bool handled = false;
    m_Server.RegisterHandler("save_scene", [&](const EditorDebugServer::RequestContext&) {
        handled = true;
        return nlohmann::json::object();
    });
    StartAndConnect();
    Send("1", "save_scene");

    const nlohmann::json reply = ReadReply();
    EXPECT_FALSE(handled);
    EXPECT_EQ(reply.value("ok", true), false) << reply.dump();
    EXPECT_EQ(reply.value("error", std::string()), "Stop play mode, then retry.");
    EXPECT_EQ(m_Bridge->CurrentAuthor(), Components::MarkupAuthor::User);
}

// A gate hears that a connection closed, after the requests it sent.
TEST_F(DebugServerRequestGatesTest, AGateHearsThatAConnectionClosed)
{
    std::vector<std::string> log;
    Editor::DebugRequestGate gate;
    gate.GateId = "listens";
    gate.Before = [&log](const Editor::DebugRequestGateContext& context) {
        log.push_back("before:" + std::to_string(context.ClientId));
        return Editor::DebugRequestVerdict{};
    };
    gate.ClientClosed = [&log](uint32_t clientId) { log.push_back("closed:" + std::to_string(clientId)); };
    m_Gates.Register(std::move(gate));
    m_Server.RegisterHandler("get_log", [](const EditorDebugServer::RequestContext&) { return nlohmann::json::object(); });
    StartAndConnect();
    Send("1", "get_log");
    ASSERT_TRUE(FlushUntil([&] { return !log.empty(); }));
    CloseSocket(m_Client);
    m_Client = kInvalidSocket;

    ASSERT_TRUE(FlushUntil([&] { return log.size() == 2; })) << "the gate never heard the close";
    EXPECT_EQ(log[0].substr(0, 7), "before:");
    EXPECT_EQ(log[1], "closed:" + log[0].substr(7));
}

// With no gate claiming assistant_bind (no AI Assistant loaded), the binding is refused,
// so the assistant's MCP server fails closed; a claiming gate's admission binds.
TEST_F(DebugServerRequestGatesTest, AnAssistantBindingIsAdmittedOnlyByAClaimingGate)
{
    RegisterAssistantSessionHandlers(m_Server);
    StartAndConnect();
    Send("1", "assistant_bind");
    const nlohmann::json unclaimed = ReadReply();
    EXPECT_EQ(unclaimed.value("ok", true), false) << unclaimed.dump();

    Editor::DebugRequestGate assistant;
    assistant.GateId = "assistant";
    assistant.Before = [](const Editor::DebugRequestGateContext&) { return Editor::DebugRequestVerdict{}; };
    assistant.ClaimedMethods = {"assistant_bind"};
    m_Gates.Register(std::move(assistant));
    Send("2", "assistant_bind");
    const nlohmann::json claimed = ReadReply();
    EXPECT_EQ(claimed.value("ok", false), true) << claimed.dump();
}

namespace
{
// A gate that parks `parkedMethod` while `hold` is set and logs every Before as
// "<method>#<request id>".
Editor::DebugRequestGate ParkingGate(const std::string& parkedMethod, const bool& hold, std::vector<std::string>& log)
{
    Editor::DebugRequestGate gate;
    gate.GateId = "parks";
    gate.Before = [parkedMethod, &hold, &log](const Editor::DebugRequestGateContext& context) {
        log.push_back(std::string(context.Method) + "#" + std::to_string(context.RequestId));
        if (hold && context.Method == parkedMethod)
            return Editor::DebugRequestVerdict{Editor::DebugRequestDecision::Park, ""};
        return Editor::DebugRequestVerdict{};
    };
    return gate;
}
} // namespace

// A parked request is offered again each frame under its own id; its connection's later
// request (a read) waits behind it and runs after it, while another connection's runs.
TEST_F(DebugServerRequestGatesTest, AParkedRequestHoldsItsConnectionsLaterRequestsInOrder)
{
    bool hold = true;
    std::vector<std::string> gateLog;
    m_Gates.Register(ParkingGate("save_scene", hold, gateLog));
    std::vector<std::string> ran;
    for (const char* method : {"save_scene", "get_log", "get_camera"})
        m_Server.RegisterHandler(method, [&ran, method](const EditorDebugServer::RequestContext&) {
            ran.push_back(method);
            return nlohmann::json::object();
        });
    StartAndConnect();
    const SocketType other = Connect();
    ASSERT_NE(other, kInvalidSocket);
    Send("1", "save_scene");
    Send("2", "get_log");
    ASSERT_TRUE(FlushUntil([&] { return gateLog.size() >= 3; })) << "the parked request was not offered again";
    SendOn(other, "3", "get_camera");
    ASSERT_TRUE(FlushUntil([&] { return ran.size() == 1; }));
    EXPECT_EQ(ran, (std::vector<std::string>{"get_camera"}));

    hold = false;
    ASSERT_TRUE(FlushUntil([&] { return ran.size() == 3; }));
    CloseSocket(other);
    EXPECT_EQ(ran, (std::vector<std::string>{"get_camera", "save_scene", "get_log"}));
    EXPECT_EQ(gateLog[0], gateLog[1]) << "a parked request keeps its id when offered again";
    EXPECT_EQ(std::count(gateLog.begin(), gateLog.end(), std::string("get_log#2")), 1)
        << "a request behind a parked one is offered once, after it";
    const nlohmann::json first = ReadReply();
    const nlohmann::json second = ReadReply();
    EXPECT_EQ(first.value("id", std::string()), "1") << first.dump();
    EXPECT_EQ(second.value("id", std::string()), "2") << second.dump();
}

TEST_F(DebugServerRequestGatesTest, AParkedRequestIsDroppedUnansweredWhenItsConnectionCloses)
{
    const bool hold = true;
    std::vector<std::string> gateLog;
    Editor::DebugRequestGate gate = ParkingGate("save_scene", hold, gateLog);
    bool closed = false;
    int answered = 0;
    gate.ClientClosed = [&closed](uint32_t) { closed = true; };
    gate.After = [&answered](const Editor::DebugRequestGateContext&, const nlohmann::json*) { ++answered; };
    m_Gates.Register(std::move(gate));
    bool handled = false;
    m_Server.RegisterHandler("save_scene", [&handled](const EditorDebugServer::RequestContext&) {
        handled = true;
        return nlohmann::json::object();
    });
    StartAndConnect();
    Send("1", "save_scene");
    ASSERT_TRUE(FlushUntil([&] { return gateLog.size() >= 2; }));
    CloseSocket(m_Client);
    m_Client = kInvalidSocket;

    ASSERT_TRUE(FlushUntil([&] { return closed; })) << "the gate never heard the close";
    const size_t offers = gateLog.size();
    m_Server.FlushPendingRequests();
    EXPECT_EQ(gateLog.size(), offers) << "a dropped request is not offered again";
    EXPECT_FALSE(handled);
    EXPECT_EQ(answered, 0);
}

// After hears each response under the request's id: a refusal at once, a deferred
// response when it is sent.
TEST_F(DebugServerRequestGatesTest, TheGatesHearEachResponseADeferredOneWhenItIsSent)
{
    std::vector<uint64_t> admitted;
    std::vector<std::string> answered;
    Editor::DebugRequestGate gate;
    gate.GateId = "listens";
    gate.Before = [&admitted](const Editor::DebugRequestGateContext& context) {
        admitted.push_back(context.RequestId);
        return context.Method == "save_scene"
                   ? Editor::DebugRequestVerdict{Editor::DebugRequestDecision::Refuse, "Not now."}
                   : Editor::DebugRequestVerdict{};
    };
    gate.After = [&answered](const Editor::DebugRequestGateContext& context, const nlohmann::json* response) {
        answered.push_back(std::string(context.Method) + "#" + std::to_string(context.RequestId) + ":" +
                           (response ? (response->value("ok", false) ? "true" : "false") : "null"));
    };
    m_Gates.Register(std::move(gate));
    bool ready = false;
    m_Server.RegisterHandler("take_screenshot", [&](const EditorDebugServer::RequestContext& ctx) {
        m_Server.EnqueueDeferredResponse(ctx.id, [&ready](nlohmann::json& result) {
            result = nlohmann::json{{"captured", true}};
            return ready;
        });
        return EditorDebugServer::DeferredMarker();
    });
    m_Server.RegisterHandler("save_scene", [](const EditorDebugServer::RequestContext&) { return nlohmann::json::object(); });
    StartAndConnect();
    Send("1", "take_screenshot");
    Send("2", "save_scene");

    ASSERT_TRUE(FlushUntil([&] { return answered.size() == 1; }));
    ASSERT_EQ(admitted.size(), 2u);
    EXPECT_EQ(answered[0], "save_scene#" + std::to_string(admitted[1]) + ":false");
    ready = true;
    ASSERT_TRUE(FlushUntil([&] { return answered.size() == 2; }));
    EXPECT_EQ(answered[1], "take_screenshot#" + std::to_string(admitted[0]) + ":true");
}

// A capture reply serialized on a job goes to the client whole, and the gates hear it as the
// same response without the PNG's base64: the file path and size a gate records stay.
TEST_F(DebugServerRequestGatesTest, TheGatesHearAReplySerializedOnAJobWithoutItsImagePayload)
{
    std::optional<nlohmann::json> heard;
    Editor::DebugRequestGate gate;
    gate.GateId = "listens";
    gate.Before = [](const Editor::DebugRequestGateContext&) { return Editor::DebugRequestVerdict{}; };
    gate.After = [&heard](const Editor::DebugRequestGateContext&, const nlohmann::json* response) {
        heard = response ? *response : nlohmann::json(nullptr);
    };
    m_Gates.Register(std::move(gate));
    JobSystem::WorkStealingThreadPool jobs(1);
    const nlohmann::json capture{{"filePath", "C:/captures/screenshot_viewport_1.png"},
                                 {"pngBase64", "iVBORw0KGgo="},
                                 {"width", 640},
                                 {"height", 360}};
    m_Server.RegisterHandler("take_screenshot", [&](const EditorDebugServer::RequestContext& ctx) {
        auto job = std::make_shared<JobSystem::TaskHandle>(
            Editor::SubmitHandlerReplyJob(jobs, ctx.id, [capture] { return capture; }));
        m_Server.EnqueueDeferredResponse(ctx.id, [job](nlohmann::json& result) {
            std::string error;
            return Editor::PollHandlerReplyJob(*job, result, error);
        });
        return EditorDebugServer::DeferredMarker();
    });
    StartAndConnect();
    Send("1", "take_screenshot");

    const nlohmann::json reply = ReadReply();
    EXPECT_EQ(reply.value("result", nlohmann::json()), capture) << "the client receives the reply whole";
    ASSERT_TRUE(FlushUntil([&] { return heard.has_value(); }));
    nlohmann::json expected = reply;
    expected["result"].erase("pngBase64");
    EXPECT_EQ(*heard, expected);
}
