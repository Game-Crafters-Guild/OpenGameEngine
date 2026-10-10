#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include "Core/Application.h"
#include "Engine/Build/DotnetHost.h"
#include "Jobs/CompileServerUtil.h"
#include "Jobs/CompileServerClient.h"
#include <climits>
#ifdef _WIN32
#include "Jobs/PipeTransport.h"
#include "Jobs/CompileServerVersion.h"
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace GameEngine;

static std::filesystem::path MakeTempWorkspace() {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto base = std::filesystem::temp_directory_path() / ("GE_Test_WS_" + std::to_string(static_cast<long long>(now)));
    std::filesystem::create_directories(base);
    return base;
}

TEST(CompileServerUtilTests, IdenticalNamesForEquivalentPaths) {
    auto root = MakeTempWorkspace();

    auto p1 = root;
    auto p2 = (root / "").lexically_normal();
    auto p3 = std::filesystem::path(root.string() + "/");
#ifdef _WIN32
    auto p4 = std::filesystem::path(root.string());
    std::string up = root.string();
    if (!up.empty()) up[0] = (char)std::toupper((unsigned char)up[0]);
    auto p5 = std::filesystem::path(up);
#endif

    auto n1 = ComputeCompileServerPipeName(p1);
    auto n2 = ComputeCompileServerPipeName(p2);
    auto n3 = ComputeCompileServerPipeName(p3);
    EXPECT_EQ(n1, n2);
    EXPECT_EQ(n1, n3);
#ifdef _WIN32
    auto n4 = ComputeCompileServerPipeName(p4);
    auto n5 = ComputeCompileServerPipeName(p5);
    EXPECT_EQ(n1, n4);
    EXPECT_EQ(n1, n5);
#endif
}

static void WritePidFile(const std::filesystem::path& path, long long pid)
{
    std::ofstream ofs(path, std::ios::binary);
    ofs << pid << "\n" << "C:/Some/Tree/GameEngine.CompileServerHost.dll\n";
}

TEST(CompileServerUtilTests, PruneRemovesPidFilesOfExitedHostsOnly)
{
    const auto dir = MakeTempWorkspace();
#ifdef _WIN32
    const long long livePid = static_cast<long long>(GetCurrentProcessId());
#else
    const long long livePid = static_cast<long long>(getpid());
#endif
    // INT_MAX is never a live pid: Windows ids are multiples of 4, Linux and
    // macOS cap them far lower.
    const long long deadPid = INT_MAX;
    WritePidFile(dir / "GE_CompileServer_Live.pid", livePid);
    WritePidFile(dir / "GE_CompileServer_Dead.pid", deadPid);
    WritePidFile(dir / "GE_CompileServer_Dead.pid.42.tmp", deadPid);
    {
        // A file without the host path line names no host.
        std::ofstream ofs(dir / "GE_CompileServer_NoHostPath.pid", std::ios::binary);
        ofs << livePid << "\n";
    }

    PruneStaleCompileServerPidFiles(dir);

    EXPECT_TRUE(std::filesystem::exists(dir / "GE_CompileServer_Live.pid"));
    EXPECT_FALSE(std::filesystem::exists(dir / "GE_CompileServer_Dead.pid"));
    EXPECT_FALSE(std::filesystem::exists(dir / "GE_CompileServer_NoHostPath.pid"));
    // A host's in-progress write is not a pid file.
    EXPECT_TRUE(std::filesystem::exists(dir / "GE_CompileServer_Dead.pid.42.tmp"));
}

#ifdef _WIN32
static std::wstring Widen(const std::string& s) {
    std::wstring w; w.reserve(s.size());
    for (unsigned char c : s) w.push_back((wchar_t)c);
    return w;
}

// Staged beside the exe by this target's POST_BUILD step — the same layout the
// Editor stages and CompileServerClient::FindHostPath probes. Resolved from the
// executable directory only: climbing from the CWD into the source tree's
// Managed/ works on a dev machine and breaks the moment the build is relocated.
static std::filesystem::path StagedCompileServerHostDll()
{
    return PathUtils::GetExecutableDirectory() / "Managed" / "CompileServerHost" / "bin" /
           GE_TEST_CONFIG_NAME / "net10.0" / "GameEngine.CompileServerHost.dll";
}

static bool StartServerWithSpoofVersion(const std::string& pipe, const std::string& spoof)
{
    const std::filesystem::path host = StagedCompileServerHostDll();
    std::error_code ec;
    if (!std::filesystem::exists(host, ec)) {
        ADD_FAILURE() << "Staged CompileServerHost missing: " << host.string()
                      << " — the EngineCompileServerUtilTests POST_BUILD staging should have "
                         "copied it from Managed/CompileServerHost/bin/" << GE_TEST_CONFIG_NAME
                      << "/net10.0";
        return false;
    }

    // Same engine-wide dotnet discovery the real client uses (DOTNET_ROOT /
    // PATH / standard install locations) — agent shells often lack dotnet on PATH.
    const std::string& dotnetCmd = GameEngine::DotnetHostCommand();
    const std::wstring dotnetPathW = Widen(dotnetCmd);
    const wchar_t* app = (dotnetCmd != "dotnet") ? dotnetPathW.c_str() : nullptr;
    std::wstring cmd = L"\"" + dotnetPathW + L"\" \"" + host.wstring() + L"\" " + Widen(pipe) + L" --spoof-version " + Widen(spoof);
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end()); cmdBuf.push_back(L'\0');
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring wd = host.parent_path().wstring();
    if (!CreateProcessW(app, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, wd.c_str(), &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    // Give the server a moment
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return true;
}

static int ExtractPid(const std::string& json)
{
    auto pos = json.find("\"ProcessId\"");
    if (pos == std::string::npos) return -1;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return -1;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    int pid = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') { pid = pid*10 + (json[pos]-'0'); ++pos; }
    return pid;
}

static std::string ExtractVersion(const std::string& json)
{
    auto k = json.find("\"Version\"");
    if (k == std::string::npos) return {};
    auto colon = json.find(':', k);
    if (colon == std::string::npos) return {};
    auto q1 = json.find('"', colon);
    if (q1 == std::string::npos) return {};
    auto q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return json.substr(q1 + 1, q2 - q1 - 1);
}

TEST(CompileServerUtilTests, RecyclesServerOnVersionMismatch)
{
    auto ws = MakeTempWorkspace();
    auto pipe = ComputeCompileServerPipeName(ws);

    // Create a minimal source file so compile succeeds
    auto src = (ws / "Foo.cs").string();
    {
        std::ofstream ofs(src, std::ios::binary);
        ofs << "public class Foo { public static int X = 1; }\n";
    }

    ASSERT_TRUE(StartServerWithSpoofVersion(pipe, "bad.ver"));

    // Capture original PID
    PipeTransport t0(Widen(pipe));
    ASSERT_TRUE(t0.Connect());
    std::string s0; ASSERT_TRUE(t0.SendRequest("__status__", s0)); t0.Close();
    int pid0 = ExtractPid(s0);
    ASSERT_GT(pid0, 0);

    // Invoke client compile; this should detect mismatch, shut down old server, start a fresh one, and succeed
    CompileServerClient client(nullptr, pipe);
    std::string req = std::string("{\"ProjectRoot\":\"\",\"ChangedFiles\":[],\"AffectedFiles\":[],\"AllFiles\":[\"") + src + "\"],\"PreferredStrategy\":\"Full\",\"ForceFull\":true}";
    CompileServerResponse resp{};
    (void)client.Compile(req, resp); // may return false if compile fails; still triggers recycle on mismatch

    // New PID
    PipeTransport t1(Widen(pipe));
    ASSERT_TRUE(t1.Connect());
    std::string s1; ASSERT_TRUE(t1.SendRequest("__status__", s1)); t1.Close();
    int pid1 = ExtractPid(s1);
    ASSERT_GT(pid1, 0);
    EXPECT_NE(pid0, pid1);

    // Also ensure version now matches expected
    PipeTransport t2(Widen(pipe));
    ASSERT_TRUE(t2.Connect());
    std::string vj; ASSERT_TRUE(t2.SendRequest("__version__", vj)); t2.Close();
    auto ver = ExtractVersion(vj);
    EXPECT_EQ(ver, std::string(kExpectedCompileServerVersion));

    // Cleanup
    PipeTransport t3(Widen(pipe));
    if (t3.Connect()) { std::string o; t3.SendRequest("__shutdown__", o); t3.Close(); }
}

TEST(CompileServerUtilTests, HandlesMultipleConcurrentClients)
{
    auto ws = MakeTempWorkspace();
    auto pipe = ComputeCompileServerPipeName(ws);

    // Ensure server is up with correct version
    ASSERT_TRUE(StartServerWithSpoofVersion(pipe, std::string(kExpectedCompileServerVersion)));

    const int kThreads = 4;
    std::vector<std::thread> threads;
    std::atomic<int> successes{0};

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&]() {
            PipeTransport t(Widen(pipe));
            if (!t.Connect()) return;
            std::string vj; if (!t.SendRequest("__version__", vj)) { t.Close(); return; }
            t.Close();
            auto ver = ExtractVersion(vj);
            if (ver == std::string(kExpectedCompileServerVersion)) successes.fetch_add(1);
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(successes.load(), kThreads);

    // Shutdown server
    PipeTransport t(Widen(pipe));
    if (t.Connect()) { std::string o; t.SendRequest("__shutdown__", o); t.Close(); }
}

static double ExtractLastActivityAgoSeconds(const std::string& json)
{
    auto k = json.find("\"LastActivityAgoSeconds\"");
    if (k == std::string::npos) return -1.0;
    auto colon = json.find(':', k);
    if (colon == std::string::npos) return -1.0;
    size_t pos = colon + 1;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    size_t start = pos;
    while (pos < json.size() && ((json[pos] >= '0' && json[pos] <= '9') || json[pos] == '.')) ++pos;
    try { return std::stod(json.substr(start, pos - start)); } catch (...) { return -1.0; }
}

TEST(CompileServerUtilTests, StatusDoesNotExtendActivity)
{
    auto ws = MakeTempWorkspace();
    auto pipe = ComputeCompileServerPipeName(ws);

    // Ensure server is up with correct version
    ASSERT_TRUE(StartServerWithSpoofVersion(pipe, std::string(kExpectedCompileServerVersion)));

    PipeTransport t0(Widen(pipe));
    ASSERT_TRUE(t0.Connect());
    std::string s0; ASSERT_TRUE(t0.SendRequest("__status__", s0)); t0.Close();
    double a0 = ExtractLastActivityAgoSeconds(s0);
    ASSERT_GE(a0, 0.0);

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    PipeTransport t1(Widen(pipe));
    ASSERT_TRUE(t1.Connect());
    std::string s1; ASSERT_TRUE(t1.SendRequest("__status__", s1)); t1.Close();
    double a1 = ExtractLastActivityAgoSeconds(s1);
    ASSERT_GE(a1, 0.0);

    // Since __status__ no longer updates last-activity, a1 should be at least ~0.8s greater than a0.
    EXPECT_GE(a1, a0 + 0.8);

    // Cleanup
    PipeTransport t2(Widen(pipe));
    if (t2.Connect()) { std::string o; t2.SendRequest("__shutdown__", o); t2.Close(); }
}

// A child process this test owns; terminated on scope exit if still running.
class ChildProcess
{
public:
    ChildProcess() = default;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ~ChildProcess()
    {
        if (!m_Handle)
            return;
        TerminateProcess(m_Handle, 1);
        CloseHandle(m_Handle);
    }

    bool Launch(const std::wstring& commandLine, const std::filesystem::path& workingDirectory)
    {
        std::vector<wchar_t> cmdBuf(commandLine.begin(), commandLine.end());
        cmdBuf.push_back(L'\0');
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const std::wstring wd = workingDirectory.wstring();
        if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                            wd.empty() ? nullptr : wd.c_str(), &si, &pi))
            return false;
        CloseHandle(pi.hThread);
        m_Handle = pi.hProcess;
        m_Id = pi.dwProcessId;
        return true;
    }

    unsigned long Id() const { return m_Id; }

    bool IsRunning() const { return m_Handle && WaitForSingleObject(m_Handle, 0) == WAIT_TIMEOUT; }
    bool WaitForExit(DWORD timeoutMs) const { return m_Handle && WaitForSingleObject(m_Handle, timeoutMs) == WAIT_OBJECT_0; }

private:
    HANDLE m_Handle = nullptr;
    unsigned long m_Id = 0;
};

// Points TMP and TEMP at a directory for this process and the children it
// starts, so hosts write their pid files there; restores both on scope exit.
class ScopedTempDirectory
{
public:
    explicit ScopedTempDirectory(const std::filesystem::path& dir)
        : m_Tmp(Read(L"TMP")), m_Temp(Read(L"TEMP"))
    {
        SetEnvironmentVariableW(L"TMP", dir.wstring().c_str());
        SetEnvironmentVariableW(L"TEMP", dir.wstring().c_str());
    }
    ~ScopedTempDirectory()
    {
        SetEnvironmentVariableW(L"TMP", m_Tmp.c_str());
        SetEnvironmentVariableW(L"TEMP", m_Temp.c_str());
    }

private:
    static std::wstring Read(const wchar_t* name)
    {
        wchar_t buffer[MAX_PATH * 2] = {};
        const DWORD length = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
        return std::wstring(buffer, length < std::size(buffer) ? length : 0);
    }

    std::wstring m_Tmp;
    std::wstring m_Temp;
};

static bool WaitForFile(const std::filesystem::path& path, std::chrono::seconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!std::filesystem::exists(path))
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
}

static bool LaunchHost(ChildProcess& process, const std::filesystem::path& hostDll, const std::string& pipe)
{
    const std::wstring dotnet = Widen(GameEngine::DotnetHostCommand());
    return process.Launch(L"\"" + dotnet + L"\" \"" + hostDll.wstring() + L"\" " + Widen(pipe), hostDll.parent_path());
}

static void WriteHostPidFile(const std::filesystem::path& path, unsigned long pid, const std::filesystem::path& hostDll)
{
    std::ofstream ofs(path, std::ios::binary);
    ofs << pid << "\n" << hostDll.string() << "\n";
}

// A copy of the staged host in a directory of its own: stands for another tree's
// build or an editor's staged copy.
static std::filesystem::path CopyHost(const std::filesystem::path& ownHost, const std::filesystem::path& dir)
{
    std::filesystem::create_directories(dir);
    std::filesystem::copy(ownHost.parent_path(), dir, std::filesystem::copy_options::recursive);
    return dir / ownHost.filename();
}

// The host build's last resort on Windows, StopHostRunningFromOutput.ps1, must stop the
// host running from the build output and nothing else: not a host running from another
// copy (another tree, an editor's staged copy), not a process whose command line merely
// names the path, not a non-dotnet process a pid file names, not a process that reused a
// dead host's pid, and not a dead pid.
TEST(CompileServerUtilTests, HostBuildStopsOnlyTheHostRunningFromItsOutput)
{
    const std::filesystem::path ownHost = StagedCompileServerHostDll();
    const std::filesystem::path script = PathUtils::GetExecutableDirectory() / "StopHostRunningFromOutput.ps1";
    ASSERT_TRUE(std::filesystem::exists(ownHost)) << ownHost.string();
    ASSERT_TRUE(std::filesystem::exists(script)) << script.string();

    const auto root = MakeTempWorkspace();
    const std::filesystem::path otherHost = CopyHost(ownHost, root / "OtherTree" / "net10.0");

    ScopedTempDirectory temp(root);
    const std::filesystem::path pidDir = root / "GE_CompileServer";
    const std::string suffix = std::to_string(GetCurrentProcessId());
    const std::string ownPipe = "GE_CompileServer_StopOwn_" + suffix;
    const std::string otherPipe = "GE_CompileServer_StopOther_" + suffix;
    const std::string reusedPipe = "GE_CompileServer_StopReused_" + suffix;

    ChildProcess own;
    ChildProcess other;
    ChildProcess shell;
    ASSERT_TRUE(LaunchHost(own, ownHost, ownPipe));
    ASSERT_TRUE(LaunchHost(other, otherHost, otherPipe));
    ASSERT_TRUE(shell.Launch(L"powershell -NoProfile -NonInteractive -Command \"Start-Sleep 120 # " + ownHost.wstring() + L"\"", {}));
    ASSERT_TRUE(WaitForFile(pidDir / (ownPipe + ".pid"), std::chrono::seconds(20)));
    ASSERT_TRUE(WaitForFile(pidDir / (otherPipe + ".pid"), std::chrono::seconds(20)));

    // A host killed earlier left its file; its pid is now nobody's.
    WriteHostPidFile(pidDir / "GE_CompileServer_StopDead.pid", INT_MAX, ownHost);
    // A pid file naming the build output and a process that is not a host.
    WriteHostPidFile(pidDir / "GE_CompileServer_StopShell.pid", shell.Id(), ownHost);
    // A host killed earlier left its file, and a dotnet process started afterwards took its pid.
    const std::filesystem::path reusedPidFile = pidDir / "GE_CompileServer_StopReused.pid";
    WriteHostPidFile(reusedPidFile, INT_MAX, ownHost);
    const auto deadHostWriteTime = std::filesystem::last_write_time(reusedPidFile);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ChildProcess reused;
    ASSERT_TRUE(LaunchHost(reused, otherHost, reusedPipe));
    ASSERT_TRUE(WaitForFile(pidDir / (reusedPipe + ".pid"), std::chrono::seconds(20)));
    WriteHostPidFile(reusedPidFile, reused.Id(), ownHost);
    std::filesystem::last_write_time(reusedPidFile, deadHostWriteTime);

    ChildProcess stop;
    ASSERT_TRUE(stop.Launch(L"powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + script.wstring() +
                                L"\" -HostPath \"" + ownHost.wstring() + L"\"",
                            {}));
    ASSERT_TRUE(stop.WaitForExit(60000));

    EXPECT_TRUE(own.WaitForExit(10000)) << "the host running from the build output must be stopped";
    EXPECT_FALSE(std::filesystem::exists(pidDir / (ownPipe + ".pid")));
    EXPECT_TRUE(other.IsRunning()) << "a host running from another copy must survive";
    EXPECT_TRUE(shell.IsRunning()) << "a process that is not a host must survive";
    EXPECT_TRUE(reused.IsRunning()) << "a process that reused a dead host's pid must survive";
}

// The host build first asks the host running from its output to exit
// (--shutdown-running-from, run by the previous build). Only a host running from that path
// exits: the request reaches every host whose pid file names the path, and a host whose
// pid file is wrong about it refuses.
TEST(CompileServerUtilTests, ShutdownRunningFromStopsOnlyTheHostRunningFromThatPath)
{
    const std::filesystem::path ownHost = StagedCompileServerHostDll();
    ASSERT_TRUE(std::filesystem::exists(ownHost)) << ownHost.string();

    const auto root = MakeTempWorkspace();
    const std::filesystem::path otherHost = CopyHost(ownHost, root / "OtherTree" / "net10.0");

    ScopedTempDirectory temp(root);
    const std::filesystem::path pidDir = root / "GE_CompileServer";
    const std::string suffix = std::to_string(GetCurrentProcessId());
    const std::string ownPipe = "GE_CompileServer_ShutdownOwn_" + suffix;
    const std::string otherPipe = "GE_CompileServer_ShutdownOther_" + suffix;

    ChildProcess own;
    ChildProcess other;
    ASSERT_TRUE(LaunchHost(own, ownHost, ownPipe));
    ASSERT_TRUE(LaunchHost(other, otherHost, otherPipe));
    ASSERT_TRUE(WaitForFile(pidDir / (ownPipe + ".pid"), std::chrono::seconds(20)));
    ASSERT_TRUE(WaitForFile(pidDir / (otherPipe + ".pid"), std::chrono::seconds(20)));
    // The other host's pid file claims the own path, so the request reaches it too.
    WriteHostPidFile(pidDir / (otherPipe + ".pid"), other.Id(), ownHost);

    const std::wstring dotnet = Widen(GameEngine::DotnetHostCommand());
    ChildProcess shutdown;
    ASSERT_TRUE(shutdown.Launch(L"\"" + dotnet + L"\" \"" + ownHost.wstring() + L"\" --shutdown-running-from \"" +
                                    ownHost.wstring() + L"\"",
                                ownHost.parent_path()));
    ASSERT_TRUE(shutdown.WaitForExit(60000));

    EXPECT_TRUE(own.WaitForExit(10000)) << "the host running from the path must exit";
    EXPECT_FALSE(std::filesystem::exists(pidDir / (ownPipe + ".pid")));
    EXPECT_TRUE(other.IsRunning()) << "a host running from another copy must refuse";
}

// Windows-specific prune cases: a process that has exited while someone still holds its
// handle is not running, and a pid file whose pid belongs to a process started after the
// file was written names a reused pid, not its host.
TEST(CompileServerUtilTests, PruneRemovesPidFilesOfExitedOrReusedProcesses)
{
    const auto dir = MakeTempWorkspace();
    const std::filesystem::path hostDll = "C:/Some/Tree/GameEngine.CompileServerHost.dll";

    ChildProcess exited;
    ASSERT_TRUE(exited.Launch(L"cmd.exe /c exit 0", {}));
    ASSERT_TRUE(exited.WaitForExit(10000));
    WriteHostPidFile(dir / "GE_CompileServer_ExitedHeld.pid", exited.Id(), hostDll);

    const std::filesystem::path reusedPidFile = dir / "GE_CompileServer_Reused.pid";
    WriteHostPidFile(reusedPidFile, GetCurrentProcessId(), hostDll);
    std::filesystem::last_write_time(reusedPidFile,
                                     std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 365));

    PruneStaleCompileServerPidFiles(dir);

    EXPECT_FALSE(std::filesystem::exists(dir / "GE_CompileServer_ExitedHeld.pid"));
    EXPECT_FALSE(std::filesystem::exists(reusedPidFile));
}

#endif
