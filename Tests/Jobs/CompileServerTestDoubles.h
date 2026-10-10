#pragma once

// Test doubles for code that compiles through CompileServerClient without a
// real server: a transport that refuses to connect, and a log of the
// notifications the client raises for each request.

#include "Jobs/CompileServerClient.h"
#include "Jobs/IHotReloadTransport.h"

#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine::Tests
{

// Refuses every connection, so a compile fails fast and never starts a server.
class RefusingTransport : public IHotReloadTransport
{
  public:
    bool Connect() override { return false; }
    bool SendRequest(const std::string&, std::string&) override { return false; }
    void Close() override {}
};

// Records the compile-started and diagnostics-batch notifications in arrival
// order, as "started <Assembly>" and "batch <Assembly> <count>". The observers
// run on the thread that compiles: read the entries after that compile returns.
class CompileNotificationLog
{
  public:
    CompileNotificationLog()
    {
        m_StartedId = CompileServerClient::RegisterCompileStartedObserver(
            [this](const std::string& assemblyName) { m_Entries.push_back("started " + assemblyName); });
        m_BatchId = CompileServerClient::RegisterDiagnosticsBatchObserver(
            [this](const std::string& assemblyName, const std::vector<CompileServerDiagnostic>& diagnostics) {
                m_Entries.push_back("batch " + assemblyName + " " + std::to_string(diagnostics.size()));
            });
    }
    ~CompileNotificationLog()
    {
        CompileServerClient::UnregisterCompileStartedObserver(m_StartedId);
        CompileServerClient::UnregisterDiagnosticsBatchObserver(m_BatchId);
    }
    CompileNotificationLog(const CompileNotificationLog&) = delete;
    CompileNotificationLog& operator=(const CompileNotificationLog&) = delete;

    const std::vector<std::string>& Entries() const { return m_Entries; }

  private:
    uint64_t m_StartedId = 0;
    uint64_t m_BatchId = 0;
    std::vector<std::string> m_Entries;
};

} // namespace GameEngine::Tests
