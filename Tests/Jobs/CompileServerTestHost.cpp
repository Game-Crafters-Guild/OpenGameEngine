#include "CompileServerTestHost.h"
#include "Jobs/CompileServerClient.h"
#include "Jobs/PipeTransport.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace CompileServerTestHost {

namespace {

constexpr int kShutdownGraceMs = 2000;
// A pipe can serve multiple host instances (NamedPipeServerStream allows
// MaxAllowedServerInstances, and fixtures may spawn per compile call).
constexpr int kMaxHostsPerPipe = 8;

std::mutex& RegistryMutex()
{
    static std::mutex m;
    return m;
}

std::vector<std::string>& RegisteredPipes()
{
    static std::vector<std::string> pipes;
    return pipes;
}

uint32_t ParseProcessId(const std::string& versionJson)
{
    const auto key = versionJson.find("\"ProcessId\":");
    if (key == std::string::npos)
        return 0;
    size_t i = key + 12;
    while (i < versionJson.size() && (versionJson[i] == ' ' || versionJson[i] == '\t'))
        ++i;
    uint32_t pid = 0;
    while (i < versionJson.size() && versionJson[i] >= '0' && versionJson[i] <= '9')
    {
        pid = pid * 10 + static_cast<uint32_t>(versionJson[i] - '0');
        ++i;
    }
    return pid;
}

// Registers the ephemeral marker + teardown reaper for any test executable
// that links this file. SetUp runs before the first test, so implicit host
// spawns (pipeline / ScriptManager compiles) are job-bound too.
class CompileServerHostReaperEnvironment : public ::testing::Environment {
public:
    void SetUp() override { EnsureEphemeralMarker(); }
    void TearDown() override { ReapAllTestHosts(); }
};

const bool g_ReaperRegistered = []
{
    ::testing::AddGlobalTestEnvironment(new CompileServerHostReaperEnvironment());
    return true;
}();

} // namespace

void EnsureEphemeralMarker()
{
#ifdef _WIN32
    _putenv_s("GE_COMPILE_SERVER_EPHEMERAL", "1");
#else
    setenv("GE_COMPILE_SERVER_EPHEMERAL", "1", 1);
#endif
}

bool StartTestCompileServer(const std::string& pipeName)
{
    EnsureEphemeralMarker();
    if (!GameEngine::CompileServerClient::StartServerForPipe(pipeName))
        return false;

    std::lock_guard<std::mutex> lock(RegistryMutex());
    auto& pipes = RegisteredPipes();
    if (std::find(pipes.begin(), pipes.end(), pipeName) == pipes.end())
        pipes.push_back(pipeName);
    return true;
}

uint32_t QueryHostPid(const std::string& pipeName, int connectWaitMs)
{
    GameEngine::PipeTransport transport(pipeName);
    if (!transport.TryConnectOnce(connectWaitMs))
        return 0;
    std::string response;
    const bool ok = transport.SendRequest("__version__", response);
    transport.Close();
    return ok ? ParseProcessId(response) : 0;
}

void ReapHostsOnPipe(const std::string& pipeName)
{
    for (int i = 0; i < kMaxHostsPerPipe; ++i)
    {
        GameEngine::PipeTransport transport(pipeName);
        // The host tears down and recreates its pipe instance between clients,
        // and TryConnectOnce only waits out ERROR_PIPE_BUSY — a probe landing
        // in that gap sees "no pipe" while the host is alive. Retry briefly
        // before concluding no host is listening.
        bool connected = false;
        for (int probe = 0; probe < 6 && !connected; ++probe)
        {
            connected = transport.TryConnectOnce(100);
            if (!connected)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!connected)
            return; // no host (left) on this pipe

        // Same connection for __version__ and __shutdown__ so the PID we wait
        // on belongs to the instance we just told to exit.
        std::string versionJson;
        uint32_t pid = 0;
        if (transport.SendRequest("__version__", versionJson))
            pid = ParseProcessId(versionJson);

#ifdef _WIN32
        HANDLE process = pid != 0 ? OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid) : nullptr;
#endif
        std::string shutdownResponse;
        (void)transport.SendRequest("__shutdown__", shutdownResponse);
        transport.Close();

#ifdef _WIN32
        if (process)
        {
            if (WaitForSingleObject(process, kShutdownGraceMs) != WAIT_OBJECT_0)
            {
                TerminateProcess(process, 1);
                WaitForSingleObject(process, kShutdownGraceMs);
            }
            CloseHandle(process);
        }
        else
        {
            Sleep(200); // no PID: give the orderly shutdown a moment before re-probing
        }
#endif
    }
}

void ReapAllTestHosts()
{
    std::vector<std::string> pipes;
    {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        pipes = RegisteredPipes();
        RegisteredPipes().clear();
    }
    for (const auto& pipe : pipes)
        ReapHostsOnPipe(pipe);
}

std::string UniquePipeName(const char* tag)
{
    std::ostringstream oss;
#ifdef _WIN32
    oss << "GE_CompileServer_Test_" << tag << "_" << GetCurrentProcessId();
#else
    oss << "GE_CompileServer_Test_" << tag << "_" << getpid();
#endif
    return oss.str();
}

} // namespace CompileServerTestHost
