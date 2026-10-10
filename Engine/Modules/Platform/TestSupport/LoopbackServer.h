#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace GameEngine
{

#ifdef _WIN32
using SocketHandle = SOCKET;
inline constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
inline constexpr int kShutdownBoth = SD_BOTH;
inline constexpr int kSendFlags = 0;
inline void CloseSocket(SocketHandle socketHandle)
{
    closesocket(socketHandle);
}
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
inline constexpr int kShutdownBoth = SHUT_RDWR;
#ifdef MSG_NOSIGNAL
inline constexpr int kSendFlags = MSG_NOSIGNAL;
#else
inline constexpr int kSendFlags = 0;
#endif
inline void CloseSocket(SocketHandle socketHandle)
{
    close(socketHandle);
}
#endif

// An HTTP server on 127.0.0.1 and an ephemeral port, so no test reaches past
// this machine. It serves one connection at a time: reads the request, then
// hands the socket to the test's responder, which writes the reply. Test
// targets that use it link PlatformTestSupport.
class LoopbackServer
{
public:
    using Responder = std::function<void(LoopbackServer& server, SocketHandle client)>;

    explicit LoopbackServer(Responder responder) : m_Responder(std::move(responder))
    {
#ifdef _WIN32
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
        m_Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        bind(m_Listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        listen(m_Listener, 4);
        socklen_t length = sizeof(address);
        getsockname(m_Listener, reinterpret_cast<sockaddr*>(&address), &length);
        m_Port = ntohs(address.sin_port);
        m_Thread = std::thread([this] { AcceptLoop(); });
    }

    ~LoopbackServer()
    {
        {
            std::lock_guard lock(m_Mutex);
            m_Stopping = true;
        }
        m_Signalled.notify_all();
        shutdown(m_Listener, kShutdownBoth);
        CloseSocket(m_Listener);
        m_Thread.join();
#ifdef _WIN32
        WSACleanup();
#endif
    }

    std::string Url(std::string_view path = "/") const
    {
        return "http://127.0.0.1:" + std::to_string(m_Port) + std::string(path);
    }

    int RequestCount() const { return m_RequestCount.load(); }

    static void Send(SocketHandle client, std::string_view bytes)
    {
        while (!bytes.empty())
        {
            const int sent = send(client, bytes.data(), static_cast<int>(bytes.size()), kSendFlags);
            if (sent <= 0)
                return;
            bytes.remove_prefix(static_cast<size_t>(sent));
        }
    }

    // Called by the test (any thread) to let a waiting responder go on.
    void Signal()
    {
        {
            std::lock_guard lock(m_Mutex);
            ++m_Signals;
        }
        m_Signalled.notify_all();
    }

    // Blocks the responder until Signal has been called `count` times in total.
    // False when the limit passes first or the server is stopping.
    bool WaitForSignals(int count, std::chrono::milliseconds limit)
    {
        std::unique_lock lock(m_Mutex);
        m_Signalled.wait_for(lock, limit, [&] { return m_Stopping || m_Signals >= count; });
        return !m_Stopping && m_Signals >= count;
    }

private:
    void AcceptLoop()
    {
        for (;;)
        {
            const SocketHandle client = accept(m_Listener, nullptr, nullptr);
            if (client == kInvalidSocket)
                return;
            ReadRequest(client);
            ++m_RequestCount;
            m_Responder(*this, client);
            CloseSocket(client);
        }
    }

    // Consumes the request head and its Content-Length body.
    static void ReadRequest(SocketHandle client)
    {
        std::string request;
        char buffer[4096];
        size_t headEnd = std::string::npos;
        size_t contentLength = 0;
        for (;;)
        {
            if (headEnd != std::string::npos && request.size() >= headEnd + 4 + contentLength)
                return;
            const int received = recv(client, buffer, sizeof(buffer), 0);
            if (received <= 0)
                return;
            request.append(buffer, static_cast<size_t>(received));
            if (headEnd == std::string::npos && (headEnd = request.find("\r\n\r\n")) != std::string::npos)
            {
                const size_t field = request.find("Content-Length:");
                if (field != std::string::npos && field < headEnd)
                    contentLength = std::stoul(request.substr(field + 15));
            }
        }
    }

    Responder m_Responder;
    SocketHandle m_Listener = kInvalidSocket;
    int m_Port = 0;
    std::atomic<int> m_RequestCount{0};
    std::mutex m_Mutex;
    std::condition_variable m_Signalled;
    int m_Signals = 0;
    bool m_Stopping = false;
    std::thread m_Thread;
};

} // namespace GameEngine
