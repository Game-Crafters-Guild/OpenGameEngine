#pragma once

// Optional Editor IPC helper (header-only) for posting overlay messages from Engine code.
// Windows-only minimal TCP client that talks to the Editor's IPCServer (localhost:9999).
// Guarded by env var GE_EDITOR_IPC=1 to avoid unintended traffic.
// Uses MSVC's #pragma comment(lib, "Ws2_32.lib") to avoid build script changes.

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#endif

#include <cstdlib>
#include <sstream>
#include <string>

namespace GameEngine
{
namespace EditorIPC
{

inline bool IsEnabled()
{
    const char* env = std::getenv("GE_EDITOR_IPC");
    return env && std::string(env) == "1";
}

#ifdef _WIN32
inline bool SendEditorAIMessage(const std::string& text,
                                const std::string& type = "warning",
                                float timeToLive = 6.0f,
                                bool dismissible = true)
{
    if (!IsEnabled())
        return false;

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        return false;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET)
    {
        WSACleanup();
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9999);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        closesocket(sock);
        WSACleanup();
        return false;
    }

    // Build minimal JSON line expected by IPCServer
    std::ostringstream oss;
    oss << "{\"type\":\"display_ai_message\",\"data\":{"
        << "\"text\":\"";

    // Escape quotes and backslashes in text
    for (char c : text)
    {
        if (c == '"' || c == '\\')
            oss << '\\' << c;
        else if (c == '\n')
            oss << "\\n";
        else
            oss << c;
    }

    oss << "\",\"type\":\"" << type << "\",\"timeToLive\":\"" << timeToLive
        << "\",\"dismissible\":\"" << (dismissible ? "true" : "false") << "\"}}\n";

    const std::string payload = oss.str();
    int sent = send(sock, payload.c_str(), static_cast<int>(payload.size()), 0);

    closesocket(sock);
    WSACleanup();
    return sent == static_cast<int>(payload.size());
}

#ifdef _WIN32
inline bool SendEditorCompileDiagnostic(bool success,
                                        const std::string& strategy,
                                        int changed,
                                        int affected,
                                        int warnings,
                                        int errors,
                                        float ms /* -1 if unknown */)
{
    if (!IsEnabled())
        return false;

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        return false;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET)
    {
        WSACleanup();
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9999);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        closesocket(sock);
        WSACleanup();
        return false;
    }

    std::ostringstream oss;
    oss << "{\"type\":\"add_compile_diag\",\"data\":{"
        << "\"success\":\"" << (success ? "true" : "false") << "\","
        << "\"strategy\":\"";

    // escape strategy
    for (char c : strategy)
    {
        if (c == '"' || c == '\\')
            oss << '\\' << c;
        else
            oss << c;
    }

    oss << "\",\"changed\":\"" << changed
        << "\",\"affected\":\"" << affected
        << "\",\"warnings\":\"" << warnings
        << "\",\"errors\":\"" << errors
        << "\",\"ms\":\"" << ms << "\"}}\n";

    const std::string payload = oss.str();
    int sent = send(sock, payload.c_str(), static_cast<int>(payload.size()), 0);

    closesocket(sock);
    WSACleanup();
    return sent == static_cast<int>(payload.size());
}
#else
inline bool SendEditorCompileDiagnostic(bool, const std::string&, int, int, int, int, float)
{
    return false;
}
#endif

#ifdef _WIN32
inline bool ToggleEditorCompileDiagnosticsPane()
{
    if (!IsEnabled())
        return false;
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return false;
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET)
    {
        WSACleanup();
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9999);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        closesocket(sock);
        WSACleanup();
        return false;
    }
    const char* payload = "{\"type\":\"toggle_compile_diag_pane\"}\n";
    int sent = send(sock, payload, (int)strlen(payload), 0);
    closesocket(sock);
    WSACleanup();
    return sent == (int)strlen(payload);
}
inline bool ClearEditorCompileDiagnostics()
{
    if (!IsEnabled())
        return false;
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return false;
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET)
    {
        WSACleanup();
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9999);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        closesocket(sock);
        WSACleanup();
        return false;
    }
    const char* payload = "{\"type\":\"clear_compile_diag\"}\n";
    int sent = send(sock, payload, (int)strlen(payload), 0);
    closesocket(sock);
    WSACleanup();
    return sent == (int)strlen(payload);
}
#else
inline bool ToggleEditorCompileDiagnosticsPane()
{
    return false;
}
inline bool ClearEditorCompileDiagnostics()
{
    return false;
}
#endif

#else
inline bool SendEditorAIMessage(const std::string&, const std::string& = "warning", float = 6.0f, bool = true)
{
    return false; // Not implemented on non-Windows
}
inline bool SendEditorCompileDiagnostic(bool, const std::string&, int, int, int, int, float)
{
    return false; // Not implemented on non-Windows
}
inline bool ToggleEditorCompileDiagnosticsPane()
{
    return false;
}
inline bool ClearEditorCompileDiagnostics()
{
    return false;
}
#endif

inline void NotifyCompileServerFallbackOnce()
{
    // Keep message concise; users can disable via GE_EDITOR_IPC=0
    (void)SendEditorAIMessage(
        "CompileServer fallback to CLI. Check CompileServer host/pipe. Set GE_DISABLE_COMPILE_SERVER=1 to opt-out.",
        "warning", 7.0f, true);
}

} // namespace EditorIPC
} // namespace GameEngine
