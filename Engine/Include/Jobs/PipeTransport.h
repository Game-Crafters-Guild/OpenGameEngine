#pragma once

#include "Jobs/IHotReloadTransport.h"
#include <string>

namespace GameEngine
{

class PipeTransport : public IHotReloadTransport
{
  public:
    PipeTransport();
    explicit PipeTransport(const std::string& pipeNameUtf8);
#if defined(PLATFORM_WINDOWS)
    explicit PipeTransport(const std::wstring& pipeName);
#endif
    ~PipeTransport() override;

    bool Connect() override;
    // Single-attempt, low-latency connect probe (no internal retries, no timeout logging).
    // Useful for readiness polling and "is server up?" checks.
    bool TryConnectOnce(int waitMs = 0);
    bool SendRequest(const std::string& requestJson, std::string& outResponse) override;
    void Close() override;

#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
    // Diagnostics/testing helper: populated during Connect().
    const std::string& DebugGetSocketPath() const { return m_SocketPath; }
#endif

  private:
#if defined(PLATFORM_WINDOWS)
    void* m_Handle;          // HANDLE (void* to avoid windows.h in header)
    std::wstring m_PipeName; // L"\\.\\pipe\\GE_CompileServer_<WorkspaceId>"
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
    int m_SocketFd;               // Unix domain socket fd
    std::string m_PipeNameUtf8;   // Logical pipe name (e.g., GE_CompileServer_<WorkspaceId>)
    std::string m_SocketPath;     // Full path to the Unix domain socket (for diagnostics)
#else
#   error "PipeTransport is not supported on this platform"
#endif
};

} // namespace GameEngine
