#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace GameEngine::Editor
{
class DebugRequestGateRegistry;
struct DebugRequestGateContext;
class UndoRedoService;
} // namespace GameEngine::Editor

namespace GameEngine
{

// TCP debug server for AI agent introspection via MCP.
// Listens on a configurable port (default 9999), accepts multiple clients,
// speaks newline-delimited JSON (ndjson) request/response protocol.
//
// Thread model:
//   - Network thread: select() loop over listen + all client sockets
//   - Main thread: handler callbacks execute via FlushPendingRequests()
//
// Every request passes the gates of the registry the server was built over
// (Editor::DebugRequestGateRegistry) before its handler runs; a refusing gate answers it.
// A parked request is held and offered again at each FlushPendingRequests, and every
// later request from its connection is held behind it, so a connection's requests run
// in the order it sent them.
//
// Usage:
//   server.RegisterHandler("get_editor_state", [](const RequestContext&) -> json { ... });
//   // a handler refuses with `return Editor::RefuseRequest("reason");` (DebugServerReply.h)
//   server.Start(9999);
//   // each frame:
//   server.FlushPendingRequests();
//   // on shutdown:
//   server.Stop();
class EditorDebugServer
{
  public:
    struct RequestContext
    {
        std::string id;                // Unique request ID (for deferred responses)
        nlohmann::json params;         // Client-provided parameters
    };

    using Handler = std::function<nlohmann::json(const RequestContext& ctx)>;

    // `gates` is consulted for every request and must outlive the server; the editor's
    // server is built over Editor::DebugRequestGateRegistry::Get().
    explicit EditorDebugServer(const Editor::DebugRequestGateRegistry& gates);
    ~EditorDebugServer();

    // Register a named handler. Must be called before Start().
    void RegisterHandler(const std::string& method, Handler handler);

    // Start listening. Spawns the network thread and waits for bind/listen to complete.
    bool Start(uint16_t port = 9999);

    // Stop listening and disconnect all clients. Blocks until thread joins.
    void Stop();

    // Called once per frame on the main thread. Offers held requests to the gates again,
    // executes queued request handlers, sends responses back to the correct client, then
    // gives the gates their once-per-update call.
    void FlushPendingRequests();

    // The undo history the gates receive with each request (DebugRequestGateContext::Undo);
    // must outlive the server or be cleared first. Null (the default) for none.
    void SetUndoRedoService(Editor::UndoRedoService* undo) { m_Undo = undo; }

    bool IsRunning() const { return m_Running.load(std::memory_order_relaxed); }
    bool HasClient() const { return m_ClientCount.load(std::memory_order_relaxed) > 0; }

    // Deferred response support: allows handlers to delay their response by
    // returning a special "deferred" JSON marker. The handler stores a callback
    // via EnqueueDeferredResponse that is polled each frame until it produces
    // a result. Used for async operations like GPU readback screenshots.
    static nlohmann::json DeferredMarker() { return nlohmann::json{{"__deferred", true}}; }

    // The gates this server consults for every request.
    const Editor::DebugRequestGateRegistry& Gates() const { return m_Gates; }
    static bool IsDeferred(const nlohmann::json& j) { return j.is_object() && j.contains("__deferred"); }

    using DeferredPollFn = std::function<bool(nlohmann::json& outResult)>;
    void EnqueueDeferredResponse(const std::string& requestId, DeferredPollFn pollFn,
                                 int pollBudget = 30);

    // Render-phase callbacks: enqueued during FlushPendingRequests, executed
    // during Render after BeginFrame (when the render graph is fresh).
    using RenderCallback = std::function<void()>;
    void EnqueueRenderCallback(RenderCallback cb);
    void RunRenderCallbacks();
    // Any pending callback at all — the pure frame driver force-creates the
    // FinalLinear composite when a capture might want to read it.
    bool HasAnyPendingRenderCallbacks() const { return !m_RenderCallbacks.empty(); }

  private:
    // Each request carries the originating client ID so responses route back.
    struct PendingRequest
    {
        uint32_t clientId;
        std::string id;
        std::string method;
        nlohmann::json params;
        // Assigned on the main thread when the request is first taken (gate context).
        uint64_t requestId = 0;
    };

    struct PendingResponse
    {
        uint32_t clientId;
        std::string json; // pre-serialized ndjson line
    };

    struct DeferredEntry
    {
        uint32_t clientId;
        std::string id;
        DeferredPollFn poll;
        int framesRemaining = 30; // max frames to wait before timeout
        // The request the response answers, for the gates' After.
        std::string method;
        uint64_t requestId = 0;
    };

    void NetworkThreadFunc(uint16_t port);
    void SignalStartupComplete(bool succeeded);
    void ProcessLine(uint32_t clientId, const std::string& line);
    // Dispatches `req` unless an earlier request of its connection is held this frame
    // (`waiting`); holds it when it waits or a gate parks it.
    void Offer(PendingRequest& req, std::vector<uint32_t>& waiting);
    // Gates and, when admitted, runs one request; true when a gate parked it (the request
    // is left intact to be offered again).
    bool Dispatch(PendingRequest& req);
    // Drops, unanswered, the held requests of the connections in `closedClients`.
    void DropHeldRequests(const std::vector<uint32_t>& closedClients);
    // Sends a handler's (or deferred poll's) result to the request `context` describes: a
    // refusal built by Editor::RefuseRequest as ok:false, anything else as ok:true
    // (DebugServerReply.h); then tells the gates (After).
    void Answer(const Editor::DebugRequestGateContext& context, const std::string& id,
                const nlohmann::json& result);
    // Queues the gates' notice that `clientId` closed (network thread).
    void QueueClientClosed(uint32_t clientId);
    void SendErrorResponse(uint32_t clientId, const std::string& id, const std::string& error);
    // Queues a serialized ndjson line for the network thread and wakes it.
    void EnqueueResponse(uint32_t clientId, std::string serialized);
    // Wakes the network thread out of its select() wait so a freshly queued
    // response is sent immediately instead of after the poll timeout.
    void WakeNetworkThread();

    std::unordered_map<std::string, Handler> m_Handlers;
    const Editor::DebugRequestGateRegistry& m_Gates;

    std::thread m_NetworkThread;
    std::atomic<bool> m_Running{false};
    std::atomic<bool> m_ShutdownRequested{false};
    std::atomic<int> m_ClientCount{0};
    // UDP port of the network thread's self-wake socket (0 = not available).
    std::atomic<uint16_t> m_WakePort{0};
    // Persistent loopback sender for WakeNetworkThread (-1 = none). intptr_t
    // holds both POSIX fds and Windows SOCKET handles; INVALID_SOCKET maps to
    // -1 through the cast.
    std::atomic<intptr_t> m_WakeSendSocket{-1};
    // True while a wake datagram is in flight — coalesces a burst of
    // per-response wakes into one. Cleared by the network thread every loop
    // iteration, so a lost datagram degrades to the poll timeout, not a wedge.
    std::atomic<bool> m_WakePending{false};

    std::mutex m_StartupMutex;
    std::condition_variable m_StartupCv;
    bool m_StartupComplete = false;
    bool m_StartupSucceeded = false;

    // Requests queued by network thread, consumed by main thread.
    std::vector<PendingRequest> m_PendingRequests;
    // Connections the network thread closed, in order; the gates hear of each after the
    // requests the connection sent before it closed (both queues are taken together).
    std::vector<uint32_t> m_ClosedClients;
    std::mutex m_RequestMutex;

    // Responses queued by main thread, consumed by network thread.
    std::vector<PendingResponse> m_PendingResponses;
    std::mutex m_ResponseMutex;

    std::vector<DeferredEntry> m_DeferredResponses;
    struct RenderCallbackEntry
    {
        RenderCallback Fn;
    };
    std::vector<RenderCallbackEntry> m_RenderCallbacks;

    // Requests a gate parked and the later requests of their connections, in arrival
    // order; main thread only.
    std::vector<PendingRequest> m_HeldRequests;
    uint64_t m_LastRequestId = 0;
    Editor::UndoRedoService* m_Undo = nullptr;

    // Tracks the request currently executing so that deferred responses can be routed
    // back to the right socket and reported to the gates.
    uint32_t m_CurrentRequestClientId = 0;
    uint64_t m_CurrentRequestId = 0;
    std::string_view m_CurrentRequestMethod;
};

} // namespace GameEngine
