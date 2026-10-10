#include "DebugServer/EditorDebugServer.h"
#include "DebugServer/DebugServerReply.h"
#include "Editor/Registries/DebugRequestGateRegistry.h"
#include "Platform/Capabilities.h"

#ifdef _WIN32
// Winsock fd_set holds 64 sockets by default; listen + wake + kMaxClients(64)
// exceeds that and FD_SET silently drops the overflow. Must precede winsock2.h.
#define FD_SETSIZE 130
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#endif

#include "Core/Application.h"

#include <Logger/Logger.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace GameEngine
{

// Portable socket aliases
#ifdef _WIN32
using SocketType = SOCKET;
static constexpr SocketType kInvalidSock = INVALID_SOCKET;
static inline int SocketClose(SocketType s) { return closesocket(s); }
static inline bool SocketError(int result) { return result == SOCKET_ERROR; }
#else
using SocketType = int;
static constexpr SocketType kInvalidSock = -1;
static inline int SocketClose(SocketType s) { return ::close(s); }
static inline bool SocketError(int result) { return result < 0; }
#endif

static constexpr int kSelectTimeoutMs = 50;
static constexpr int kRecvBufferSize = 4096;
static constexpr int kWakeDrainBufferSize = 16;
// Parallel automation (the integration harness fires concurrent IPC calls,
// and batch CLI sessions can hold connections open) legitimately exceeds
// a small cap. A rejected connect surfaces as an opaque ECONNRESET that
// callers misread as an editor crash, so keep this comfortably high.
static constexpr int kMaxClients = 64;
// listen + wake + kMaxClients sockets must fit the fd_set or FD_SET silently
// drops the overflow (the FD_SETSIZE define at the top of this file).
static_assert(kMaxClients + 2 <= FD_SETSIZE, "raise FD_SETSIZE alongside kMaxClients");
static constexpr auto kStartupTimeout = std::chrono::seconds(2);

static inline int LastSocketError()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

// select() has been observed failing in long-running editor sessions,
// which previously killed the network thread silently (the listener
// vanished while the editor kept running). Append the failure to a
// side-file so the evidence survives even if the process later dies
// before its buffered stdout log flushes.
static void AppendServerDiag(const char* what, int err, size_t clientCount)
{
    // Anchored to the exe, not the cwd — the editor may be launched from
    // anywhere and runtime artifacts must not land in the source tree.
    static const std::string s_DiagPath =
        (PathUtils::GetExecutableDirectory() / "EditorDebugServer-diag.txt").string();
    if (FILE* f = std::fopen(s_DiagPath.c_str(), "a"))
    {
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::fprintf(f, "%lld %s err=%d clients=%zu\n",
                     static_cast<long long>(now), what, err, clientCount);
        std::fclose(f);
    }
}

// A persistent select() failure must not pin a core or flood the log/diag
// file: report the first few occurrences, then recover quietly with a
// backoff each iteration.
static constexpr int kMaxSelectFailureReports = 8;

static void SetSocketNonBlocking(SocketType sock)
{
#ifdef _WIN32
    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);
#else
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
#endif
}

// Loopback UDP socket the network thread parks in its select() set. Responses
// are produced on the main thread while the network thread sleeps in select()
// with no inbound traffic (the client is waiting on us), so without a wake the
// response sits queued until the poll timeout expires — a flat latency tax on
// every request. A one-byte datagram to this socket wakes select() instantly.
static SocketType CreateWakeSocket(uint16_t& outPort)
{
    outPort = 0;
    SocketType sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalidSock)
        return kInvalidSock;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // ephemeral

    if (SocketError(bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))))
    {
        SocketClose(sock);
        return kInvalidSock;
    }

    sockaddr_in boundAddr{};
#ifdef _WIN32
    int addrLen = static_cast<int>(sizeof(boundAddr));
#else
    socklen_t addrLen = sizeof(boundAddr);
#endif
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&boundAddr), &addrLen) != 0)
    {
        SocketClose(sock);
        return kInvalidSock;
    }

    SetSocketNonBlocking(sock);
    outPort = ntohs(boundAddr.sin_port);
    return sock;
}

static unsigned long CurrentProcessId()
{
#ifdef _WIN32
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

// Bind `sock` to loopback:port, claiming the port EXCLUSIVELY where the platform
// can express that.
//
// SO_REUSEADDR exists so a restarted editor can rebind a port whose old accepted
// connections are still in TIME_WAIT (the listener itself never enters TIME_WAIT).
// On Windows it also lets a second LIVE process bind the same listening port: both
// binds succeed, both log "Listening", and the first binder answers every request,
// so the second editor's own tooling silently queries a stranger.
//
// SO_EXCLUSIVEADDRUSE is the option that separates the two. It refuses to bind over
// anything already holding the port AND refuses to let a later SO_REUSEADDR socket
// bind over us, which closes the hazard from both directions. It cannot rebind over
// TIME_WAIT remnants, which is what the SO_REUSEADDR retry below is for.
static bool BindLoopbackExclusive(SocketType sock, uint16_t port)
{
#ifdef _WIN32
    int optVal = 1;
    setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&optVal),
               sizeof(optVal));
#endif
    // POSIX SO_REUSEADDR does not permit stealing a live listening port, so a plain
    // bind is already exclusive there.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    return !SocketError(bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
}

// Fallback bind for the TIME_WAIT case only. Reached when the exclusive bind failed,
// which on a machine running only current builds means TIME_WAIT remnants rather than
// a live owner — an editor holding the port exclusively defeats this too, and that is
// the refusal we want.
static bool BindLoopbackReusing(SocketType sock, uint16_t port)
{
    int optVal = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&optVal),
               sizeof(optVal));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    return !SocketError(bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
}

static std::string JsonFieldToString(const nlohmann::json& msg, const char* key)
{
    const auto it = msg.find(key);
    if (it == msg.end() || it->is_null())
        return {};
    if (it->is_string())
        return it->get<std::string>();
    if (it->is_number_integer())
        return std::to_string(it->get<long long>());
    if (it->is_number_unsigned())
        return std::to_string(it->get<unsigned long long>());
    if (it->is_number_float())
        return std::to_string(it->get<double>());
    if (it->is_boolean())
        return it->get<bool>() ? "true" : "false";
    return {};
}

EditorDebugServer::EditorDebugServer(const Editor::DebugRequestGateRegistry& gates) : m_Gates(gates)
{
}

EditorDebugServer::~EditorDebugServer()
{
    Stop();
}

void EditorDebugServer::RegisterHandler(const std::string& method, Handler handler)
{
    m_Handlers[method] = std::move(handler);
}

bool EditorDebugServer::Start(uint16_t port)
{
    if (m_Running.load(std::memory_order_relaxed))
        return false;

    // The handlers are portable; only the listening transport is not. A
    // platform that cannot accept inbound connections gets no server (a browser
    // transport for the same handlers is a separate slice).
    if (!Platform::SupportsInboundSockets())
    {
        LOG_WARNING("EditorDebugServer: the debug/MCP server is not available on this "
                    "platform (it cannot accept inbound connections); it stays stopped.");
        return false;
    }

#ifdef _WIN32
    WSADATA wsaData{};
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0)
    {
        LOG_ERROR("EditorDebugServer: WSAStartup failed with error {}", result);
        return false;
    }
#endif

    m_ShutdownRequested.store(false, std::memory_order_relaxed);
    {
        std::lock_guard lock(m_StartupMutex);
        m_StartupComplete = false;
        m_StartupSucceeded = false;
    }

    // Persistent sender for WakeNetworkThread. Shared by the main thread and
    // the network thread — per-datagram sendto on one UDP socket is
    // kernel-atomic, so no lock is needed.
    m_WakeSendSocket.store(static_cast<intptr_t>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)),
                           std::memory_order_relaxed);
    m_WakePending.store(false, std::memory_order_relaxed);

    m_Running.store(true, std::memory_order_relaxed);
    m_NetworkThread = std::thread(&EditorDebugServer::NetworkThreadFunc, this, port);

    bool started = false;
    {
        std::unique_lock lock(m_StartupMutex);
        started = m_StartupCv.wait_for(lock, kStartupTimeout, [this] { return m_StartupComplete; }) &&
                  m_StartupSucceeded;
    }

    if (!started)
    {
        Stop();
        return false;
    }

    return true;
}

void EditorDebugServer::Stop()
{
    m_ShutdownRequested.store(true, std::memory_order_relaxed);

    // Drop all deferred responses to release any GPU resources they hold, and every held
    // request unanswered: its connection closes with the server.
    m_DeferredResponses.clear();
    m_HeldRequests.clear();

    // Gate on the thread, not m_Running: NetworkThreadFunc clears m_Running on its
    // own exit (socket error / loop end), so keying off it would skip the join and
    // leave a still-joinable std::thread to std::terminate() in the destructor.
    // joinable() is also correctly idempotent across a second Stop()/~dtor call.
    const bool hadThread = m_NetworkThread.joinable();
    if (hadThread)
        m_NetworkThread.join();

    const intptr_t wakeSend = m_WakeSendSocket.exchange(-1, std::memory_order_relaxed);
    if (wakeSend >= 0)
        SocketClose(static_cast<SocketType>(wakeSend));

    m_Running.store(false, std::memory_order_relaxed);
#ifdef _WIN32
    if (hadThread)
        WSACleanup(); // balance the WSAStartup paired with the thread launch in Start()
#endif
}

void EditorDebugServer::FlushPendingRequests()
{
    std::vector<PendingRequest> requests;
    std::vector<uint32_t> closedClients;
    {
        std::lock_guard lock(m_RequestMutex);
        requests.swap(m_PendingRequests);
        closedClients.swap(m_ClosedClients);
    }

    // A held request of a connection that closed is dropped unanswered, never run.
    DropHeldRequests(closedClients);
    std::vector<PendingRequest> held;
    held.swap(m_HeldRequests);
    // Connections with a request held this frame; their later requests wait behind it.
    std::vector<uint32_t> waiting;
    for (PendingRequest& req : held)
        Offer(req, waiting);
    for (PendingRequest& req : requests)
    {
        req.requestId = ++m_LastRequestId;
        Offer(req, waiting);
    }
    m_CurrentRequestClientId = 0;
    m_CurrentRequestId = 0;
    m_CurrentRequestMethod = {};
    DropHeldRequests(closedClients);
    for (const uint32_t clientId : closedClients)
        m_Gates.NotifyClientClosed(clientId);

    // Poll deferred responses (e.g. async GPU readback).
    for (auto it = m_DeferredResponses.begin(); it != m_DeferredResponses.end(); )
    {
        const Editor::DebugRequestGateContext context{it->method, it->clientId, nullptr, it->requestId, m_Undo};
        nlohmann::json result;
        if (it->poll(result))
        {
            if (std::string* line = Editor::SerializedReplyLine(result))
            {
                EnqueueResponse(it->clientId, std::move(*line));
                m_Gates.NotifyAnswered(context, Editor::SerializedReplyGateResponse(result));
            }
            else
            {
                Answer(context, it->id, result);
            }
            it = m_DeferredResponses.erase(it);
        }
        else if (--it->framesRemaining <= 0)
        {
            Answer(context, it->id, Editor::RefuseRequest("Deferred operation timed out"));
            it = m_DeferredResponses.erase(it);
        }
        else
        {
            ++it;
        }
    }
    m_Gates.NotifyUpdate(m_Undo);
}

void EditorDebugServer::Offer(PendingRequest& req, std::vector<uint32_t>& waiting)
{
    const bool behind = std::find(waiting.begin(), waiting.end(), req.clientId) != waiting.end();
    if (!behind && !Dispatch(req))
        return;
    if (!behind)
        waiting.push_back(req.clientId);
    m_HeldRequests.push_back(std::move(req));
}

void EditorDebugServer::DropHeldRequests(const std::vector<uint32_t>& closedClients)
{
    std::erase_if(m_HeldRequests, [&closedClients](const PendingRequest& req) {
        return std::find(closedClients.begin(), closedClients.end(), req.clientId) != closedClients.end();
    });
}

bool EditorDebugServer::Dispatch(PendingRequest& req)
{
    const auto handler = m_Handlers.find(req.method);
    if (handler == m_Handlers.end())
    {
        SendErrorResponse(req.clientId, req.id, "Unknown method: " + req.method);
        return false;
    }

    const Editor::DebugRequestGateContext gateContext{req.method, req.clientId, &req.params, req.requestId, m_Undo};
    nlohmann::json result;
    try
    {
        m_CurrentRequestClientId = req.clientId;
        m_CurrentRequestId = req.requestId;
        m_CurrentRequestMethod = req.method;
        Editor::DebugRequestGateRegistry::RequestScope gates(m_Gates, gateContext);
        const Editor::DebugRequestVerdict verdict = gates.Admit();
        if (verdict.Decision == Editor::DebugRequestDecision::Park)
            return true;
        if (verdict.Decision == Editor::DebugRequestDecision::Refuse)
        {
            result = Editor::RefuseRequest(verdict.Refusal);
        }
        else
        {
            // A copy: the gates' context still reads the held request's parameters.
            const RequestContext ctx{req.id, req.params};
            result = handler->second(ctx);
        }
    }
    catch (const std::exception& e)
    {
        result = Editor::RefuseRequest(std::string("Handler exception: ") + e.what());
    }
    catch (...)
    {
        result = Editor::RefuseRequest("Handler threw a non-standard exception");
    }
    // A deferred handler has called EnqueueDeferredResponse, whose poll answers later.
    if (!IsDeferred(result))
        Answer(gateContext, req.id, result);
    return false;
}

void EditorDebugServer::Answer(const Editor::DebugRequestGateContext& context, const std::string& id,
                               const nlohmann::json& result)
{
    const nlohmann::json response = Editor::HandlerResponse(id, result);
    EnqueueResponse(context.ClientId,
                    response.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
    m_Gates.NotifyAnswered(context, &response);
}

void EditorDebugServer::SignalStartupComplete(bool succeeded)
{
    {
        std::lock_guard lock(m_StartupMutex);
        if (m_StartupComplete)
            return;

        m_StartupComplete = true;
        m_StartupSucceeded = succeeded;
    }

    m_StartupCv.notify_all();
}

// ---------------------------------------------------------------------------
// Network thread: single select() loop over listen socket + all clients
// ---------------------------------------------------------------------------

struct ClientState
{
    SocketType socket = kInvalidSock;
    uint32_t id = 0;
    std::string recvBuffer;
};

void EditorDebugServer::NetworkThreadFunc(uint16_t port)
try
{
    SocketType listenSock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == kInvalidSock)
    {
        LOG_ERROR("EditorDebugServer: Failed to create listen socket");
        m_Running.store(false, std::memory_order_relaxed);
        SignalStartupComplete(false);
        return;
    }

    // Claim the port outright first. Failing that, a TIME_WAIT remnant of this
    // editor's own previous run is the expected cause, and SO_REUSEADDR is what
    // clears it — but the option cannot be applied to a socket that already tried
    // an exclusive bind, so start from a fresh one.
    //
    // Both attempts failing means something live owns the port and will not share
    // it. Binding anyway is how two editors end up splitting one port with the
    // first binder answering everything, so refuse and say so.
    bool bound = BindLoopbackExclusive(listenSock, port);
    if (!bound)
    {
        SocketClose(listenSock);
        listenSock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == kInvalidSock)
        {
            LOG_ERROR("EditorDebugServer: Failed to create listen socket");
            m_Running.store(false, std::memory_order_relaxed);
            SignalStartupComplete(false);
            return;
        }
        bound = BindLoopbackReusing(listenSock, port);
        if (bound)
            LOG_WARNING("EditorDebugServer: port {} was not free to claim exclusively; rebound it "
                        "(expected after a restart, while the previous run's connections drain).",
                        port);
    }

    if (!bound)
    {
        LOG_ERROR("EditorDebugServer: bind failed on port {} — another process holds it and will "
                  "not share. Two editors on one port silently split it, with the first binder "
                  "answering everything, so this editor (pid {}, {}) runs on WITHOUT a debug "
                  "server. Give it its own port via GE_EDITOR_DEBUG_PORT, and find the current "
                  "owner with `netstat -ano | findstr :{}`.",
                  port, CurrentProcessId(), PathUtils::GetExecutableDirectory().string(), port);
        SocketClose(listenSock);
        m_Running.store(false, std::memory_order_relaxed);
        SignalStartupComplete(false);
        return;
    }

    if (SocketError(listen(listenSock, SOMAXCONN)))
    {
        LOG_ERROR("EditorDebugServer: listen failed");
        SocketClose(listenSock);
        m_Running.store(false, std::memory_order_relaxed);
        SignalStartupComplete(false);
        return;
    }

    // Provenance on the hello line: several editors share this machine, and the
    // first question about any of them is which process and which staged tree.
    LOG_INFO("EditorDebugServer: Listening on port {} (pid {}, {})", port, CurrentProcessId(),
             PathUtils::GetExecutableDirectory().string());

    uint16_t wakePort = 0;
    SocketType wakeSock = CreateWakeSocket(wakePort);
    if (wakeSock == kInvalidSock)
        LOG_WARNING("EditorDebugServer: wake socket unavailable — responses flush on the {}ms poll", kSelectTimeoutMs);
    m_WakePort.store(wakePort, std::memory_order_relaxed);

    SignalStartupComplete(true);

    std::vector<ClientState> clients;
    uint32_t nextClientId = 1;
    int consecutiveSelectFailures = 0;
    char recvBuf[kRecvBufferSize];

    while (!m_ShutdownRequested.load(std::memory_order_relaxed))
    {
        // Build fd_set: listen socket + wake socket + all client sockets.
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSock, &readSet);
        int nfds = static_cast<int>(listenSock) + 1;
        if (wakeSock != kInvalidSock)
        {
            FD_SET(wakeSock, &readSet);
            if (static_cast<int>(wakeSock) + 1 > nfds)
                nfds = static_cast<int>(wakeSock) + 1;
        }
        for (auto& c : clients)
        {
            FD_SET(c.socket, &readSet);
            if (static_cast<int>(c.socket) + 1 > nfds)
                nfds = static_cast<int>(c.socket) + 1;
        }

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = kSelectTimeoutMs * 1000;

        int selectResult = select(nfds, &readSet, nullptr, nullptr, &timeout);
        if (SocketError(selectResult))
        {
            const int err = LastSocketError();
#ifndef _WIN32
            // EINTR is routine under profilers/timers/debuggers — retry
            // without touching clients.
            if (err == EINTR)
                continue;
#endif
            // A dying client socket (or an external component interfering with
            // the process's socket state) can fail select(). Breaking here used
            // to silently kill the server for the rest of the session — instead
            // drop all clients, revive the listener if needed, and keep serving.
            ++consecutiveSelectFailures;
            if (consecutiveSelectFailures <= kMaxSelectFailureReports)
            {
                LOG_ERROR("EditorDebugServer: select failed (err={}, clients={}, streak={}) — resetting clients",
                          err, clients.size(), consecutiveSelectFailures);
                AppendServerDiag("select-failed", err, clients.size());
            }

            for (auto& c : clients)
                SocketClose(c.socket);
            clients.clear();
            m_ClientCount.store(0, std::memory_order_relaxed);

            int soType = 0;
#ifdef _WIN32
            int optLen = static_cast<int>(sizeof(soType));
#else
            socklen_t optLen = sizeof(soType);
#endif
            const bool listenDead =
                getsockopt(listenSock, SOL_SOCKET, SO_TYPE,
                           reinterpret_cast<char*>(&soType), &optLen) != 0;
            if (listenDead)
            {
                SocketClose(listenSock);
                listenSock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                bool revived = listenSock != kInvalidSock;
                if (revived)
                {
                    // Recovery, not startup: this process already owned the port, so
                    // reuse is the point — drained connections from the sockets just
                    // dropped are exactly what would otherwise block the rebind.
                    revived = BindLoopbackReusing(listenSock, port) &&
                              !SocketError(listen(listenSock, SOMAXCONN));
                }
                if (!revived)
                {
                    const int reviveErr = LastSocketError();
                    LOG_ERROR("EditorDebugServer: listen socket unrecoverable (err={}); server stopping",
                              reviveErr);
                    AppendServerDiag("listen-unrecoverable", reviveErr, 0);
                    break;
                }
                LOG_WARNING("EditorDebugServer: listen socket recreated after select failure");
                AppendServerDiag("listen-recreated", 0, 0);
            }
            // The wake socket may equally be the fd that poisoned select() —
            // rebuild it too so a recovered server keeps its low-latency path.
            if (wakeSock != kInvalidSock)
                SocketClose(wakeSock);
            wakeSock = CreateWakeSocket(wakePort);
            m_WakePort.store(wakePort, std::memory_order_relaxed);
            // select() returns immediately on error (the timeout is not
            // consumed) — without this a persistent failure spins the thread.
            std::this_thread::sleep_for(std::chrono::milliseconds(kSelectTimeoutMs));
            continue;
        }
        consecutiveSelectFailures = 0;

        // Drain wake datagrams — their only job was to interrupt select().
        if (wakeSock != kInvalidSock && selectResult > 0 && FD_ISSET(wakeSock, &readSet))
        {
            char drainBuf[kWakeDrainBufferSize];
            while (recv(wakeSock, drainBuf, sizeof(drainBuf), 0) > 0)
            {
            }
        }
        // Cleared every iteration (not just when a datagram arrived): if a
        // wake was lost, the next response's wake sends again instead of
        // being coalesced away behind a flag nobody will reset.
        m_WakePending.store(false, std::memory_order_relaxed);

        // Accept new connections.
        if (selectResult > 0 && FD_ISSET(listenSock, &readSet))
        {
            SocketType newSock = accept(listenSock, nullptr, nullptr);
            if (newSock != kInvalidSock)
            {
                if (static_cast<int>(clients.size()) >= kMaxClients)
                {
                    LOG_WARNING("EditorDebugServer: Max clients reached, rejecting connection");
                    SocketClose(newSock);
                }
                else
                {
                    // Disable Nagle for low latency.
                    int noDelay = 1;
                    setsockopt(newSock, IPPROTO_TCP, TCP_NODELAY,
                               reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
#ifdef SO_NOSIGPIPE
                    int noSigPipe = 1;
                    setsockopt(newSock, SOL_SOCKET, SO_NOSIGPIPE,
                               reinterpret_cast<const char*>(&noSigPipe), sizeof(noSigPipe));
#endif

                    uint32_t cid = nextClientId++;
                    clients.push_back({newSock, cid, {}});
                    m_ClientCount.store(static_cast<int>(clients.size()), std::memory_order_relaxed);
                    // Debug, not info: one-shot CLI clients connect per request
                    // and would flood the info-level log tail.
                    LOG_DEBUG("EditorDebugServer: Client {} connected ({} total)", cid, clients.size());
                }
            }
        }

        // Read from clients.
        for (size_t i = 0; i < clients.size(); )
        {
            auto& c = clients[i];
            if (selectResult > 0 && FD_ISSET(c.socket, &readSet))
            {
                auto bytesRead = recv(c.socket, recvBuf, kRecvBufferSize, 0);
                if (bytesRead <= 0)
                {
                    if (bytesRead < 0)
                    {
                        // Abnormal close (reset/abort) — keep visible in every
                        // build; it is primary evidence when MCP goes quiet.
                        LOG_WARNING("EditorDebugServer: Client {} recv failed (err={}), disconnecting",
                                    c.id, LastSocketError());
                    }
                    else
                    {
                        // Clean close — routine for one-shot CLI clients,
                        // debug-only to keep the info tail readable.
                        LOG_DEBUG("EditorDebugServer: Client {} disconnected", c.id);
                    }
                    SocketClose(c.socket);
                    QueueClientClosed(c.id);
                    clients.erase(clients.begin() + static_cast<ptrdiff_t>(i));
                    m_ClientCount.store(static_cast<int>(clients.size()), std::memory_order_relaxed);
                    continue;
                }

                c.recvBuffer.append(recvBuf, static_cast<size_t>(bytesRead));

                // Process complete ndjson lines.
                size_t pos = 0;
                while (true)
                {
                    size_t newline = c.recvBuffer.find('\n', pos);
                    if (newline == std::string::npos)
                        break;

                    std::string line = c.recvBuffer.substr(pos, newline - pos);
                    pos = newline + 1;

                    if (!line.empty())
                        ProcessLine(c.id, line);
                }

                if (pos > 0)
                    c.recvBuffer.erase(0, pos);
            }
            ++i;
        }

        // Send queued responses to their respective clients.
        std::vector<PendingResponse> responses;
        {
            std::lock_guard lock(m_ResponseMutex);
            responses.swap(m_PendingResponses);
        }

        for (auto& resp : responses)
        {
            // Find the target client.
            SocketType targetSock = kInvalidSock;
            size_t targetIdx = 0;
            for (size_t i = 0; i < clients.size(); ++i)
            {
                if (clients[i].id == resp.clientId)
                {
                    targetSock = clients[i].socket;
                    targetIdx = i;
                    break;
                }
            }

            if (targetSock == kInvalidSock)
                continue; // Client already disconnected.

            const char* data = resp.json.c_str();
            int remaining = static_cast<int>(resp.json.size());
            bool sendFailed = false;

            while (remaining > 0)
            {
                auto sent = send(targetSock, data, remaining, 0);
                if (sent <= 0)
                {
                    LOG_WARNING("EditorDebugServer: send to client {} failed, disconnecting", resp.clientId);
                    SocketClose(targetSock);
                    QueueClientClosed(resp.clientId);
                    clients.erase(clients.begin() + static_cast<ptrdiff_t>(targetIdx));
                    m_ClientCount.store(static_cast<int>(clients.size()), std::memory_order_relaxed);
                    sendFailed = true;
                    break;
                }
                data += sent;
                remaining -= static_cast<int>(sent);
            }

            if (sendFailed)
                continue;
        }
    }

    // Close all client sockets.
    for (auto& c : clients)
        SocketClose(c.socket);
    clients.clear();
    m_ClientCount.store(0, std::memory_order_relaxed);

    m_WakePort.store(0, std::memory_order_relaxed);
    if (wakeSock != kInvalidSock)
        SocketClose(wakeSock);
    SocketClose(listenSock);
}
catch (const std::exception& e)
{
    LOG_ERROR("EditorDebugServer: network thread threw: {}", e.what());
    AppendServerDiag(e.what(), 0, 0);
    // Locals (sockets) are out of scope in a function-try-block handler; at
    // least stop WakeNetworkThread from firing datagrams at the dead socket.
    m_WakePort.store(0, std::memory_order_relaxed);
    m_Running.store(false, std::memory_order_relaxed);
    SignalStartupComplete(false);
}
catch (...)
{
    LOG_ERROR("EditorDebugServer: network thread threw unknown exception");
    AppendServerDiag("unknown-exception", 0, 0);
    m_WakePort.store(0, std::memory_order_relaxed);
    m_Running.store(false, std::memory_order_relaxed);
    SignalStartupComplete(false);
}

void EditorDebugServer::ProcessLine(uint32_t clientId, const std::string& line)
{
    // allow_exceptions=false — return json::value_t::discarded on parse failure
    // instead of throwing. The throwing overload triggers first-chance exception
    // breaks in attached debuggers and stalls the network thread, which made
    // MCP requests time out whenever a client sent malformed input.
    nlohmann::json msg = nlohmann::json::parse(line, /*cb=*/nullptr,
                                               /*allow_exceptions=*/false,
                                               /*ignore_comments=*/false);
    if (msg.is_discarded())
    {
        LOG_WARNING("EditorDebugServer: Failed to parse JSON line ({} bytes)", line.size());
        return;
    }

    std::string id = JsonFieldToString(msg, "id");
    std::string method = JsonFieldToString(msg, "method");
    nlohmann::json params = msg.contains("params") ? msg["params"] : nlohmann::json::object();

    if (method.empty())
    {
        SendErrorResponse(clientId, id, "Missing 'method' field");
        return;
    }

    std::lock_guard lock(m_RequestMutex);
    m_PendingRequests.push_back({clientId, std::move(id), std::move(method), std::move(params)});
}

void EditorDebugServer::QueueClientClosed(uint32_t clientId)
{
    std::lock_guard lock(m_RequestMutex);
    m_ClosedClients.push_back(clientId);
}

void EditorDebugServer::SendErrorResponse(uint32_t clientId, const std::string& id, const std::string& error)
{
    nlohmann::json resp;
    resp["id"] = id;
    resp["ok"] = false;
    resp["error"] = error;

    EnqueueResponse(clientId,
                    resp.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
}

void EditorDebugServer::EnqueueResponse(uint32_t clientId, std::string serialized)
{
    {
        std::lock_guard lock(m_ResponseMutex);
        m_PendingResponses.push_back({clientId, std::move(serialized)});
    }
    // Wake OUTSIDE the lock: the network thread reacts by taking the same
    // mutex to drain, so waking under it would just stall the handoff.
    WakeNetworkThread();
}

void EditorDebugServer::WakeNetworkThread()
{
    const uint16_t wakePort = m_WakePort.load(std::memory_order_relaxed);
    const intptr_t sendSock = m_WakeSendSocket.load(std::memory_order_relaxed);
    if (wakePort == 0 || sendSock < 0)
        return; // Wake path unavailable — the 50ms select() poll picks it up.

    // Coalesce bursts (FlushPendingRequests can emit many responses per
    // frame): one in-flight datagram flushes the whole queue. The network
    // thread clears the flag every iteration, so nothing can wedge.
    if (m_WakePending.exchange(true, std::memory_order_acq_rel))
        return;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(wakePort);

    const char wakeByte = 1;
    sendto(static_cast<SocketType>(sendSock), &wakeByte, 1, 0,
           reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
}

void EditorDebugServer::EnqueueDeferredResponse(const std::string& requestId, DeferredPollFn pollFn,
                                                int pollBudget)
{
    m_DeferredResponses.push_back(
        {m_CurrentRequestClientId, requestId, std::move(pollFn), std::max(1, pollBudget),
         std::string(m_CurrentRequestMethod), m_CurrentRequestId});
}

void EditorDebugServer::EnqueueRenderCallback(RenderCallback cb)
{
    m_RenderCallbacks.push_back({std::move(cb)});
}

void EditorDebugServer::RunRenderCallbacks()
{
    if (m_RenderCallbacks.empty())
        return;

    auto callbacks = std::move(m_RenderCallbacks);
    m_RenderCallbacks.clear();
    for (auto& entry : callbacks)
        entry.Fn();
}

} // namespace GameEngine
