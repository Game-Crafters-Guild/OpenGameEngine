#include "Jobs/CompileServerClient.h"
#include "Engine/Build/DotnetHost.h"
#include "Jobs/PipeTransport.h"
#include "Jobs/WideStringUtil.h"
#include "Logger/Logger.h"
#include "Core/Application.h"
#include "Platform/Shell.h"
#include "Scripting/ScriptTargetFramework.h"
#include <atomic>
#include <filesystem>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <chrono>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#ifdef __linux__
#include <csignal>
#include <sys/prctl.h>
#endif
#endif

#include "Jobs/CompileServerVersion.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <unordered_map>
namespace GameEngine
{
static std::atomic<int> g_SuccessCount{0};
static std::atomic<int> g_FallbackCount{0};

namespace {

struct ObserverStore {
    template<typename T>
    struct Entry { uint64_t Id; T Callback; };

    std::mutex Mutex;
    std::vector<Entry<CompileServerClient::DiagnosticsObserver>> DiagnosticsObservers;
    std::vector<Entry<CompileServerClient::DiagnosticsBatchObserver>> DiagnosticsBatchObservers;
    std::vector<Entry<CompileServerClient::CompileStartedObserver>> CompileStartedObservers;
    uint64_t NextId = 1;
};

ObserverStore& GetObserverStore()
{
    static ObserverStore store;
    return store;
}

// The project-scripts request the hot-reload pipeline builds names no assembly;
// CompileScripts names the same assembly explicitly.
constexpr const char* kUnnamedRequestAssemblyName = "GameEngine.Scripts";

std::string RequestAssemblyName(const std::string& requestJson)
{
    const nlohmann::json request = nlohmann::json::parse(requestJson, nullptr, /*allow_exceptions=*/false);
    if (request.is_object())
    {
        const auto it = request.find("AssemblyName");
        if (it != request.end() && it->is_string() && !it->get_ref<const std::string&>().empty())
            return it->get<std::string>();
    }
    return kUnnamedRequestAssemblyName;
}

} // namespace

uint64_t CompileServerClient::RegisterDiagnosticsObserver(DiagnosticsObserver callback)
{
    auto& store = GetObserverStore();
    std::lock_guard<std::mutex> lock(store.Mutex);
    const uint64_t id = store.NextId++;
    store.DiagnosticsObservers.push_back({id, std::move(callback)});
    return id;
}

void CompileServerClient::UnregisterDiagnosticsObserver(uint64_t id)
{
    auto& store = GetObserverStore();
    std::lock_guard<std::mutex> lock(store.Mutex);
    auto& v = store.DiagnosticsObservers;
    v.erase(std::remove_if(v.begin(), v.end(), [id](const auto& e) { return e.Id == id; }), v.end());
}

uint64_t CompileServerClient::RegisterDiagnosticsBatchObserver(DiagnosticsBatchObserver callback)
{
    auto& store = GetObserverStore();
    std::lock_guard<std::mutex> lock(store.Mutex);
    const uint64_t id = store.NextId++;
    store.DiagnosticsBatchObservers.push_back({id, std::move(callback)});
    return id;
}

void CompileServerClient::UnregisterDiagnosticsBatchObserver(uint64_t id)
{
    auto& store = GetObserverStore();
    std::lock_guard<std::mutex> lock(store.Mutex);
    auto& v = store.DiagnosticsBatchObservers;
    v.erase(std::remove_if(v.begin(), v.end(), [id](const auto& e) { return e.Id == id; }), v.end());
}

uint64_t CompileServerClient::RegisterCompileStartedObserver(CompileStartedObserver callback)
{
    auto& store = GetObserverStore();
    std::lock_guard<std::mutex> lock(store.Mutex);
    const uint64_t id = store.NextId++;
    store.CompileStartedObservers.push_back({id, std::move(callback)});
    return id;
}

void CompileServerClient::UnregisterCompileStartedObserver(uint64_t id)
{
    auto& store = GetObserverStore();
    std::lock_guard<std::mutex> lock(store.Mutex);
    auto& v = store.CompileStartedObservers;
    v.erase(std::remove_if(v.begin(), v.end(), [id](const auto& e) { return e.Id == id; }), v.end());
}

void CompileServerClient::NotifyCompileStarted(const std::string& assemblyName)
{
    auto& store = GetObserverStore();
    std::lock_guard<std::mutex> lock(store.Mutex);
    for (const auto& entry : store.CompileStartedObservers)
    {
        entry.Callback(assemblyName);
    }
}

void CompileServerClient::NotifyDiagnostics(const std::string& assemblyName, const CompileServerResponse& response)
{
    auto& store = GetObserverStore();
    std::lock_guard<std::mutex> lock(store.Mutex);
    if ((response.Warnings.empty() && response.Errors.empty()) ||
        (store.DiagnosticsObservers.empty() && store.DiagnosticsBatchObservers.empty()))
    {
        return;
    }

    if (!store.DiagnosticsBatchObservers.empty())
    {
        std::vector<CompileServerDiagnostic> diagnostics;
        diagnostics.reserve(response.Warnings.size() + response.Errors.size());
        diagnostics.insert(diagnostics.end(), response.Warnings.begin(), response.Warnings.end());
        diagnostics.insert(diagnostics.end(), response.Errors.begin(), response.Errors.end());

        for (const auto& entry : store.DiagnosticsBatchObservers)
        {
            entry.Callback(assemblyName, diagnostics);
        }
    }

    if (store.DiagnosticsObservers.empty())
    {
        return;
    }

    for (const auto& w : response.Warnings)
    {
        for (const auto& entry : store.DiagnosticsObservers)
        {
            entry.Callback(w);
        }
    }
    for (const auto& e : response.Errors)
    {
        for (const auto& entry : store.DiagnosticsObservers)
        {
            entry.Callback(e);
        }
    }
}

static constexpr int kFastPipeWaitMs = 50;
// Prevent duplicate server launches. The dotnet cold-start can take 5-7 seconds,
// so the throttle window must be longer than the startup time.
static constexpr auto kServerLaunchThrottleWindow = std::chrono::milliseconds(8000);
static constexpr int kMaxHostSearchDepth = 12;

// Outcome-aware launch gate. Prevents accidental duplicate host launches for the same
// pipe (startup races where the pipe exists but the version handshake isn't ready yet),
// and remembers whether the most recent launch attempt actually produced a server so
// callers never sit in the full readiness timeout waiting for a server that provably
// failed to start (e.g. dotnet missing from PATH, broken host DLL).
enum class ServerLaunchGate
{
    Launch,   // no recent attempt for this pipe: launch now, then record the outcome
    Wait,     // a recent launch is in flight (or succeeded): wait for readiness
    FailFast, // the recent launch attempt failed: there is no server to wait for
};

struct ServerLaunchRecord
{
    std::chrono::steady_clock::time_point Time;
    bool Failed = false;
};

static std::mutex& ServerLaunchRegistryMutex()
{
    static std::mutex mutex;
    return mutex;
}

static std::unordered_map<std::string, ServerLaunchRecord>& ServerLaunchRegistry()
{
    static std::unordered_map<std::string, ServerLaunchRecord> registry;
    return registry;
}

static ServerLaunchGate GateServerLaunchForPipe(const std::string& pipeNameUtf8)
{
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(ServerLaunchRegistryMutex());

    auto& registry = ServerLaunchRegistry();
    auto it = registry.find(pipeNameUtf8);
    if (it != registry.end() && (now - it->second.Time) < kServerLaunchThrottleWindow)
    {
        return it->second.Failed ? ServerLaunchGate::FailFast : ServerLaunchGate::Wait;
    }

    // Claim the launch slot optimistically so concurrent callers wait instead of
    // double-launching; the caller records a failure if the launch doesn't pan out.
    registry[pipeNameUtf8] = {now, false};
    return ServerLaunchGate::Launch;
}

static void RecordServerLaunchFailure(const std::string& pipeNameUtf8)
{
    std::lock_guard<std::mutex> lock(ServerLaunchRegistryMutex());
    ServerLaunchRegistry()[pipeNameUtf8] = {std::chrono::steady_clock::now(), true};
}

// A launch for this pipe happened recently and has not been recorded as
// failed: the host process is (very likely) alive but still cold-starting
// (dotnet JIT, Roslyn warmup) — the window where the pipe may accept a
// connection while the version handshake still stalls. Compile() extends its
// retry budget in exactly that window instead of failing after ~450 ms and
// pushing callers onto stale output.
static constexpr auto kFreshLaunchObservationWindow = std::chrono::milliseconds(15000);

static bool ServerLaunchInFlightForPipe(const std::string& pipeNameUtf8)
{
    std::lock_guard<std::mutex> lock(ServerLaunchRegistryMutex());
    const auto& registry = ServerLaunchRegistry();
    const auto it = registry.find(pipeNameUtf8);
    return it != registry.end() && !it->second.Failed &&
           (std::chrono::steady_clock::now() - it->second.Time) < kFreshLaunchObservationWindow;
}

// Fast connect probe for the common PipeTransport case. For other transports,
// fall back to their Connect() implementation.
static bool ConnectFast(IHotReloadTransport& transport)
{
    if (auto* pipe = dynamic_cast<PipeTransport*>(&transport))
    {
        return pipe->TryConnectOnce(/*waitMs=*/kFastPipeWaitMs);
    }
    return transport.Connect();
}

// Cross-platform sleep helper
static void SleepMs(int ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
}

static int GetEnvIntCompileClient(const char* key, int defv)
{
    const char* v = std::getenv(key);
    if (!v || v[0] == '\0')
        return defv;
    try
    {
        return std::max(1, std::stoi(v));
    }
    catch (...)
    {
        return defv;
    }
}

// Wait for the managed CompileServerHost to start listening on the pipe/unix socket.
static bool WaitForServerReady(const std::string& pipeNameUtf8)
{
    // The managed host may take a while to cold-start (JIT, AV scanning, first-run caching).
    // Keep this reasonably bounded, but long enough to avoid "start -> timeout -> start again"
    // loops that stall Editor startup even more.
    const int timeoutMs = GetEnvIntCompileClient("GE_COMPILE_SERVER_READY_TIMEOUT_MS", 30000);
    constexpr int pollMs = 100;
    const int attempts = std::max(1, timeoutMs / pollMs);

    for (int i = 0; i < attempts; ++i)
    {
        PipeTransport retry(pipeNameUtf8);
        // Important: this must be a *fast* single-attempt probe; PipeTransport::Connect()
        // has its own retry loop (env-tunable) and can block for seconds, which would
        // turn this 10s readiness wait into minutes.
        if (retry.TryConnectOnce(/*waitMs=*/kFastPipeWaitMs))
        {
            retry.Close();
            return true;
        }
        SleepMs(pollMs);
    }
    return false;
}

// The host is a resident service by design: its pipe name is keyed to the
// workspace (ComputeCompileServerPipeName), not to the spawning process, so a
// host deliberately survives its Editor's exit and serves the next session
// with warm Roslyn caches. Test runs must opt out of that persistence: a test
// process that crashes or is killed would otherwise strand hosts that hold
// locks on the managed DLLs and break subsequent builds (issue #357). Test
// harnesses set GE_COMPILE_SERVER_EPHEMERAL=1 before the first spawn; every
// host launched by that process then dies with it.
static bool IsEphemeralHostRequested()
{
    const char* v = std::getenv("GE_COMPILE_SERVER_EPHEMERAL");
    return v && v[0] == '1' && v[1] == '\0';
}

#ifdef _WIN32
// Job object that ephemeral hosts are assigned to. KILL_ON_JOB_CLOSE means the
// OS terminates every host in the job when the last handle closes — which
// happens when this process exits for any reason, including crashes and
// TerminateProcess. The handle is intentionally kept open for the lifetime of
// the process.
static HANDLE EphemeralHostJobObject()
{
    static HANDLE job = []() -> HANDLE
    {
        HANDLE h = CreateJobObjectW(nullptr, nullptr);
        if (!h)
        {
            Logger::Log::Warning("[CompileServerClient] CreateJobObject failed (err={}); ephemeral hosts will rely on fixture teardown only", (unsigned)GetLastError());
            return nullptr;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(h, JobObjectExtendedLimitInformation, &info, sizeof(info)))
        {
            Logger::Log::Warning("[CompileServerClient] SetInformationJobObject failed (err={}); ephemeral hosts will rely on fixture teardown only", (unsigned)GetLastError());
            CloseHandle(h);
            return nullptr;
        }
        return h;
    }();
    return job;
}
#endif

void* GetCompileServerEphemeralJobHandleForTests()
{
#ifdef _WIN32
    return IsEphemeralHostRequested() ? EphemeralHostJobObject() : nullptr;
#else
    return nullptr;
#endif
}

// Launch the managed CompileServerHost process for the given pipe name.
static bool LaunchCompileServerProcess(const std::string& hostDll, const std::string& pipeNameUtf8)
{
#ifdef _WIN32
    // Shared engine-wide dotnet discovery (DOTNET_ROOT / PATH / standard install
    // locations); bare "dotnet" only as a last resort for CreateProcess's own lookup.
    const std::string& dotnetCmd = DotnetHostCommand();
    const bool resolved = dotnetCmd != "dotnet";
    std::wstring dotnetPathW = ToWide(dotnetCmd);
    const wchar_t* app = resolved ? dotnetPathW.c_str() : nullptr;
    std::wstring cmd = L"\"" + dotnetPathW + L"\" \"" + ToWide(hostDll) + L"\" " + ToWide(pipeNameUtf8);
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring wd = std::filesystem::path(hostDll).parent_path().wstring();
    Logger::Log::Debug("[CompileServerClient] Launching server: dotnet={} host={} pipe={}",
                       dotnetCmd, hostDll, pipeNameUtf8);

    // Ephemeral hosts start suspended so they are inside the kill-on-close job
    // before their first instruction; a host that raced ahead of the
    // assignment could outlive a crashing test run.
    HANDLE job = IsEphemeralHostRequested() ? EphemeralHostJobObject() : nullptr;
    DWORD creationFlags = CREATE_NO_WINDOW | (job ? CREATE_SUSPENDED : 0);

    if (!CreateProcessW(app, cmdBuf.data(), nullptr, nullptr, FALSE, creationFlags, nullptr, wd.c_str(), &si, &pi))
    {
        DWORD err = GetLastError();
        Logger::Log::Error("[CompileServerClient] Failed to start CompileServerHost process (err={})", (unsigned)err);
        return false;
    }
    if (job)
    {
        if (!AssignProcessToJobObject(job, pi.hProcess))
        {
            Logger::Log::Warning("[CompileServerClient] AssignProcessToJobObject failed (err={}); host pid={} will rely on fixture teardown only",
                                 (unsigned)GetLastError(), (unsigned)pi.dwProcessId);
        }
        ResumeThread(pi.hThread);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    const bool ephemeral = IsEphemeralHostRequested();
    // Resolve in the parent: only async-signal-safe calls are allowed between
    // fork and exec, which rules out the resolver's getenv/filesystem probing.
    const std::string dotnetCmd = DotnetHostCommand();
    pid_t pid = fork();
    if (pid < 0)
    {
        Logger::Log::Error("[CompileServerClient] fork() failed while starting CompileServerHost (errno={})", errno);
        return false;
    }
    if (pid == 0)
    {
#ifdef __linux__
        if (ephemeral)
        {
            // Linux analogue of the Windows kill-on-close job: die with the parent.
            ::prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (::getppid() == 1)
                _exit(127); // parent already gone between fork and prctl
        }
#else
        (void)ephemeral; // macOS has no parent-death signal; fixture teardown reaps
#endif
        namespace fs = std::filesystem;

        fs::path hostPath(hostDll);
        fs::path hostDir = hostPath.parent_path();

        std::string wd = hostDir.string();
        if (!wd.empty())
            ::chdir(wd.c_str());

        // Finder/Dock launches often have a minimal PATH that does not include dotnet.
        // Prefer running the apphost executable (produced by dotnet build for OutputType=Exe)
        // directly when present next to the managed host files.
        //
        // Common outputs:
        // - GameEngine.CompileServerHost.dll (managed entry)
        // - GameEngine.CompileServerHost (native apphost executable)
        // Prefer an apphost that matches the selected host DLL basename.
        // If hostDll is ".../CompileServerHost.dll", try ".../CompileServerHost" first.
        // If hostDll is ".../GameEngine.CompileServerHost.dll", try ".../GameEngine.CompileServerHost" first.
        fs::path derivedApphost = hostPath;
        derivedApphost.replace_extension();

        fs::path apphostCandidate1 = hostDir / "GameEngine.CompileServerHost";
        fs::path apphostCandidate2 = hostDir / "CompileServerHost";

        auto tryExecApphost = [&](const fs::path& exePath)
        {
            std::error_code ec;
            if (!fs::exists(exePath, ec))
                return;
            // Must be executable. If it's not, just skip and fall back to dotnet.
            // (We avoid stat() / permission checks here; execv will fail with 13 if needed.)
            const std::string exeUtf8 = exePath.string();
            const char* argv[] = {exeUtf8.c_str(), pipeNameUtf8.c_str(), nullptr};
            ::execv(exeUtf8.c_str(), const_cast<char* const*>(argv));
        };

        // Best: basename-matched apphost
        tryExecApphost(derivedApphost);

        tryExecApphost(apphostCandidate1);
        tryExecApphost(apphostCandidate2);

        // Fallback: run the framework-dependent host via the parent-resolved
        // dotnet, then a bare PATH lookup as the last resort.
        const char* argvResolved[] = {dotnetCmd.c_str(), hostDll.c_str(), pipeNameUtf8.c_str(), nullptr};
        ::execv(dotnetCmd.c_str(), const_cast<char* const*>(argvResolved));
        const char* dotnet = "dotnet";
        const char* argv[] = {dotnet, hostDll.c_str(), pipeNameUtf8.c_str(), nullptr};
        ::execvp(dotnet, const_cast<char* const*>(argv));
        _exit(127);
    }

    return true;
#endif
}

static bool DecodeBase64(const std::string& in, std::vector<uint8_t>& out)
{
    static const int8_t kDec[256] = {
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        62,
        -1,
        -1,
        -1,
        63,
        52,
        53,
        54,
        55,
        56,
        57,
        58,
        59,
        60,
        61,
        -1,
        -1,
        -1,
        -2,
        -1,
        -1,
        -1,
        0,
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        9,
        10,
        11,
        12,
        13,
        14,
        15,
        16,
        17,
        18,
        19,
        20,
        21,
        22,
        23,
        24,
        25,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        26,
        27,
        28,
        29,
        30,
        31,
        32,
        33,
        34,
        35,
        36,
        37,
        38,
        39,
        40,
        41,
        42,
        43,
        44,
        45,
        46,
        47,
        48,
        49,
        50,
        51,
        -1,
        -1,
        -1,
        -1,
        -1,
        // rest initialized to -1
    };
    // Initialize rest to -1
    static bool init = false;
    static int8_t table[256];
    if (!init)
    {
        for (int i = 0; i < 256; ++i)
            table[i] = -1;
        for (int i = 0; i < 128; ++i)
            table[i] = kDec[i];
        init = true;
    }
    out.clear();
    out.reserve(in.size() * 3 / 4);
    int val = 0, valb = -8;
    for (unsigned char c : in)
    {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t')
            continue;
        int8_t d = table[c];
        if (d < 0)
            return false;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0)
        {
            out.push_back((uint8_t)((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return true;
}

#ifdef _WIN32
// True when process pid started after the pid file was last written, so it is not the host
// that wrote it but a process that reused a dead host's id.
static bool ProcessStartedAfter(int pid, std::filesystem::file_time_type writeTime)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process)
        return false;
    FILETIME creation{}, exitTime{}, kernel{}, user{};
    const bool known = GetProcessTimes(process, &creation, &exitTime, &kernel, &user) != 0;
    CloseHandle(process);
    if (!known)
        return false;
    // file_time_type on MSVC counts FILETIME's 100 ns ticks since 1601, the same epoch.
    const auto creationTicks = (static_cast<int64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
    return creationTicks > writeTime.time_since_epoch().count();
}
#endif

// A pid file is stale when it is not in the current format (line 1 the pid, line 2 the
// host's assembly path), when its process has exited, or (Windows) when its pid now belongs
// to a process that started after the file was written.
static bool IsStaleCompileServerPidFile(const std::filesystem::path& pidPath)
{
    int pid = 0;
    std::string hostPath;
    {
        std::ifstream in(pidPath);
        if (!(in >> pid) || pid <= 0)
            return true;
        in >> std::ws;
        std::getline(in, hostPath);
    }
    if (hostPath.empty() || !Platform::IsProcessRunning(pid))
        return true;
#ifdef _WIN32
    std::error_code ec;
    const auto writeTime = std::filesystem::last_write_time(pidPath, ec);
    if (!ec && ProcessStartedAfter(pid, writeTime))
        return true;
#endif
    return false;
}

void PruneStaleCompileServerPidFiles(const std::filesystem::path& pidDir)
{
    std::error_code ec;
    for (std::filesystem::directory_iterator it(pidDir, ec), end; !ec && it != end; it.increment(ec))
    {
        const std::filesystem::path& pidPath = it->path();
        if (pidPath.extension() != ".pid" || !IsStaleCompileServerPidFile(pidPath))
            continue;
        std::error_code removeEc;
        std::filesystem::remove(pidPath, removeEc);
    }
}

static void PruneStaleCompileServerPidFilesOnce()
{
    static std::once_flag sPruned;
    std::call_once(sPruned, [] {
        std::error_code ec;
        const std::filesystem::path tempDir = std::filesystem::temp_directory_path(ec);
        if (!ec)
            PruneStaleCompileServerPidFiles(tempDir / "GE_CompileServer");
    });
}

CompileServerClient::CompileServerClient(std::unique_ptr<IHotReloadTransport> transport, std::string pipeNameUtf8)
    : m_Transport(std::move(transport)), m_PipeNameUtf8(std::move(pipeNameUtf8)),
      m_ExternalTransport(m_Transport != nullptr) {}

bool CompileServerClient::FindHostPath(std::string& outPath)
{
    // Candidate build configurations to probe. We prefer the current native build
    // config but also fall back to other common multi-config names so that:
    // - Debug-built Editor can find Debug-managed host
    // - Release-built Editor can find Release-managed host
    // - developers can still run RelWithDebInfo/MinSizeRel builds without special flags
#if defined(_DEBUG)
    constexpr const char* kPreferredCfg = "Debug";
#elif defined(NDEBUG)
    constexpr const char* kPreferredCfg = "Release";
#else
    // DebugFast is this engine's only config defining neither _DEBUG nor NDEBUG.
    // Preferring it makes the exe-anchored probe hit the staged
    // Managed/CompileServerHost/bin/DebugFast host beside a DebugFast editor
    // instead of walking up into repo layouts of the wrong config.
    constexpr const char* kPreferredCfg = "DebugFast";
#endif

    const char* cfgCandidates[] = {
        kPreferredCfg,
        "DebugFast",
        "RelWithDebInfo",
        "Debug",
        "Release",
        "MinSizeRel",
    };

    // Managed host assembly name: the csproj outputs `GameEngine.CompileServerHost.dll`.
    // (The "GameEngine.CompileServerHost" file is an apphost executable on some platforms.)
    const char* hostAssemblyCandidates[] = {
        "GameEngine.CompileServerHost.dll",
        "CompileServerHost.dll", // legacy/older naming (fallback)
    };

    auto check = [&](const std::filesystem::path& base) -> bool
    {
        std::filesystem::path cur = base;
        for (int up = 0; up < kMaxHostSearchDepth && !cur.empty(); ++up)
        {
            for (const char* cfg : cfgCandidates)
            {
                if (!cfg || cfg[0] == '\0')
                    continue;
                for (const char* hostName : hostAssemblyCandidates)
                {
                    if (!hostName || hostName[0] == '\0')
                        continue;
                    auto candidate = cur / "Managed" / "CompileServerHost" / "bin" / cfg / kScriptTargetFramework / hostName;
                    if (std::filesystem::exists(candidate))
                    {
                        outPath = candidate.string();
                        return true;
                    }
                }
            }
            cur = cur.parent_path();
        }
        return false;
    };
    try
    {
        static std::atomic<bool> sLoggedOnce{false};
        auto logFound = [&](const char* where)
        {
            if (!sLoggedOnce.exchange(true))
            {
                Logger::Log::Debug("[CompileServerClient] Found host {}: {}", where,
                                   std::filesystem::weakly_canonical(outPath).string());
            }
        };

        // A macOS app bundle stages the host as a resource, beside the other managed assemblies:
        //   Editor.app/Contents/Resources/Managed/CompileServerHost/bin/<Config>/net10.0
        // (Contents/MacOS stays code-only for codesign). Empty outside a bundle.
        const std::filesystem::path bundleResources =
            GameEngine::PathUtils::GetBundleResourcesDirectory(GameEngine::PathUtils::GetExecutableDirectory());
        if (!bundleResources.empty() && check(bundleResources))
        {
            logFound("under bundle Resources");
            return true;
        }

        // Fall back to scanning upward from current_path and executable directory.
        // This is useful for repo-dev layouts where the host lives under <repo>/Managed/.
        if (check(std::filesystem::current_path()))
        {
            logFound("under current_path");
            return true;
        }

        auto exeDir2 = GameEngine::PathUtils::GetExecutableDirectory();
        if (!exeDir2.empty() && check(exeDir2))
        {
            logFound("near executable directory");
            return true;
        }
    }
    catch (const std::exception& ex)
    {
        Logger::Log::Warning("[CompileServerClient] Exception probing host path: {}", ex.what());
    }
    Logger::Log::Warning("[CompileServerClient] CompileServerHost not found");
    return false;
}

bool CompileServerClient::StartServerIfNeeded()
{
    // If already listening, succeed
    {
        PipeTransport probe(m_PipeNameUtf8);
        if (probe.TryConnectOnce(/*waitMs=*/kFastPipeWaitMs))
        {
            probe.Close();
            return true;
        }
    }

    // Otherwise, start the server for this pipe name
    std::string hostDll;
    if (!FindHostPath(hostDll))
    {
        Logger::Log::Error("[CompileServerClient] CompileServerHost not found; expected Managed/CompileServerHost/bin/<Config>/{}/GameEngine.CompileServerHost.dll (or CompileServerHost.dll)",
                           kScriptTargetFramework);
        return false;
    }

    switch (GateServerLaunchForPipe(m_PipeNameUtf8))
    {
    case ServerLaunchGate::FailFast:
        // The last launch attempt for this pipe failed moments ago. Waiting on
        // readiness would burn the full timeout on a server that is not coming;
        // fail immediately so callers can fall back (retry happens naturally once
        // the throttle window expires).
        return false;
    case ServerLaunchGate::Launch:
        PruneStaleCompileServerPidFilesOnce();
        if (!LaunchCompileServerProcess(hostDll, m_PipeNameUtf8))
        {
            RecordServerLaunchFailure(m_PipeNameUtf8);
            return false;
        }
        break;
    case ServerLaunchGate::Wait:
        break;
    }

    if (!WaitForServerReady(m_PipeNameUtf8))
    {
        // The process launched but never started accepting connections (e.g. the
        // managed host crashed on startup). Record it so back-to-back attempts
        // fail fast instead of stacking additional full readiness timeouts.
        RecordServerLaunchFailure(m_PipeNameUtf8);
        Logger::Log::Warning("[CompileServerClient] Timeout waiting for CompileServerHost to accept connections (pipe='{}')", m_PipeNameUtf8);
        return false;
    }

    return true;
}

bool CompileServerClient::StartServerForPipe(const std::string& pipeNameUtf8)
{
    std::string hostDll;
    if (!FindHostPath(hostDll))
        return false;
    return LaunchCompileServerProcess(hostDll, pipeNameUtf8);
}

bool CompileServerClient::CompileJson(const std::string& requestJson, bool /*incremental*/)
{
    CompileServerResponse parsed;
    return Compile(requestJson, parsed) && parsed.Success;
}

bool CompileServerClient::Compile(const std::string& requestJson, CompileServerResponse& outResponse)
{
    if (!m_Transport)
        m_Transport = std::make_unique<PipeTransport>(m_PipeNameUtf8);

    const std::string assemblyName = RequestAssemblyName(requestJson);
    NotifyCompileStarted(assemblyName);

    // Retry connect with server auto-start/backoff. The fixed 3-attempt budget
    // (~450 ms) applies when no server is coming; while a server we JUST
    // spawned is still booting (pipe up, handshake stalling on JIT/Roslyn
    // warmup), retries extend — bounded — until the fresh-launch budget runs
    // out, so a first editor open doesn't fall back to stale output 1.6 s
    // into a 6 s cold start.
    constexpr int kMaxAttempts = 3;
    constexpr int kBackoffMs[kMaxAttempts] = {0, 150, 300};
    constexpr int kFreshLaunchBackoffMs = 500;
    constexpr auto kFreshLaunchRetryBudget = std::chrono::milliseconds(10000);
    const auto retryStart = std::chrono::steady_clock::now();

    auto shouldRetry = [&](int attemptIndex) {
        if (attemptIndex < kMaxAttempts)
            return true;
        if (!ServerLaunchInFlightForPipe(m_PipeNameUtf8))
            return false;
        return (std::chrono::steady_clock::now() - retryStart) < kFreshLaunchRetryBudget;
    };
    auto backoffFor = [&](int attempt) {
        return attempt < kMaxAttempts ? kBackoffMs[attempt] : kFreshLaunchBackoffMs;
    };

    for (int attempt = 0;; ++attempt)
    {
        const int attemptIndex = attempt + 1;

        if (!ConnectFast(*m_Transport))
        {
            if (m_ExternalTransport || !StartServerIfNeeded())
            {
                if (shouldRetry(attemptIndex))
                {
                    SleepMs(backoffFor(attempt));
                    continue;
                }
                Logger::Log::Warning("[CompileServerClient] Failed to start CompileServerHost or connect (attempt={} pipe='{}')",
                                     attemptIndex, m_PipeNameUtf8);
                return false;
            }
            if (!ConnectFast(*m_Transport))
            {
                if (shouldRetry(attemptIndex))
                {
                    SleepMs(backoffFor(attempt));
                    continue;
                }
                Logger::Log::Warning("[CompileServerClient] Could not connect to CompileServerHost after start (attempt={} pipe='{}')",
                                     attemptIndex, m_PipeNameUtf8);
                return false;
            }
        }

        // Ensure server version compatibility (query and potentially recycle stale server)
        {
            std::string verResp;
            const bool okVer = m_Transport->SendRequest("__version__", verResp);
            if (!okVer || verResp.empty())
            {
                m_Transport->Close();
                if (shouldRetry(attemptIndex))
                {
                    SleepMs(backoffFor(attempt));
                    continue;
                }
                Logger::Log::Warning("[CompileServerClient] Version handshake failed (ok={} len={} attempt={} pipe='{}')",
                                     okVer, verResp.size(), attemptIndex, m_PipeNameUtf8);
                return false;
            }
            try
            {
                auto jv = nlohmann::json::parse(verResp);
                std::string serverVer = jv.value("Version", std::string());
                if (serverVer != kExpectedCompileServerVersion)
                {
                    Logger::Log::Warning("[CompileServerClient] Version mismatch: server='{}' expected='{}' (pipe='{}')",
                                         serverVer, kExpectedCompileServerVersion, m_PipeNameUtf8);
                    // Ask old server to shutdown and retry (will auto-start a fresh one)
                    std::string shutResp;
                    (void)m_Transport->SendRequest("__shutdown__", shutResp);
                    m_Transport->Close();
                    if (!m_ExternalTransport && !StartServerIfNeeded())
                    {
                        if (shouldRetry(attemptIndex))
                        {
                            SleepMs(backoffFor(attempt));
                            continue;
                        }
                        Logger::Log::Warning("[CompileServerClient] Failed to restart CompileServerHost after version mismatch (pipe='{}')",
                                             m_PipeNameUtf8);
                        return false;
                    }
                    // next attempt will reconnect and retry
                    if (shouldRetry(attemptIndex))
                    {
                        SleepMs(backoffFor(attempt));
                        continue;
                    }
                    Logger::Log::Warning("[CompileServerClient] Exhausted attempts while recycling server for version mismatch (pipe='{}')",
                                         m_PipeNameUtf8);
                    return false;
                }
            }
            catch (...)
            {
                Logger::Log::Warning("[CompileServerClient] Exception parsing version response: '{}'", verResp);
                m_Transport->Close();
                if (shouldRetry(attemptIndex))
                {
                    SleepMs(backoffFor(attempt));
                    continue;
                }
                return false;
            }
        }

        // Multi-message protocol: send the compile request on the same connection as
        // the version handshake (server supports multiple messages per connection).
        // If the send fails (old server that disconnects after __version__), fall back
        // to reconnecting with polling.
        std::string response;
        bool ok = m_Transport->SendRequest(requestJson, response);
        if (!ok || response.empty())
        {
            // Fallback: old server may have disconnected after version handshake.
            // Close and reconnect with retry polling.
            m_Transport->Close();
            bool reconnected = false;
            constexpr int kReconnectAttempts = 10;
            constexpr int kReconnectPollMs = 100;
            for (int r = 0; r < kReconnectAttempts; ++r)
            {
                if (ConnectFast(*m_Transport))
                {
                    if (r > 0)
                        Logger::Log::Debug("[CompileServerClient] Reconnected for compile request after {} polls (legacy server)", r + 1);
                    reconnected = true;
                    break;
                }
                SleepMs(kReconnectPollMs);
            }
            if (reconnected)
            {
                ok = m_Transport->SendRequest(requestJson, response);
            }
        }
        m_Transport->Close();
        if (!ok || response.empty())
        {
            if (shouldRetry(attemptIndex))
            {
                SleepMs(backoffFor(attempt));
                continue;
            }
            Logger::Log::Warning("[CompileServerClient] Compile request failed: ok={} len={} attempt={} pipe='{}'",
                                 ok, response.size(), attemptIndex, m_PipeNameUtf8);
            return false;
        }
        const bool parsed = ParseResponse(response, outResponse);
        if (!parsed)
        {
            if (shouldRetry(attemptIndex))
            {
                SleepMs(backoffFor(attempt));
                continue;
            }
            Logger::Log::Warning("[CompileServerClient] Failed to parse server response");
            return false;
        }
        NotifyDiagnostics(assemblyName, outResponse);
        if (!outResponse.Success)
        {
            // Summarize diagnostics; treat as failure for caller
            std::ostringstream oss;
            oss << "Managed compile reported errors (" << outResponse.Errors.size() << ")";
            for (const auto& e : outResponse.Errors)
            {
                oss << "\n  " << e.FileUtf8 << ":" << e.Line << "," << e.Column << " " << e.Code << ": " << e.MessageUtf8;
            }
            Logger::Log::Warning("[CompileServerClient] {}", oss.str());
            return false;
        }
        // Success
        g_SuccessCount.fetch_add(1);
        return true;
    }
    // Unreachable: every failure path above returns once shouldRetry() says stop.
}

bool CompileServerClient::ParseResponse(const std::string& jsonText, CompileServerResponse& outResponse)
{
    outResponse = CompileServerResponse{};
    try
    {
        auto j = nlohmann::json::parse(jsonText);
        outResponse.Success = j.value("Success", false);
        if (j.contains("Warnings"))
        {
            for (auto& w : j["Warnings"])
            {
                CompileServerDiagnostic d;
                d.Severity = "Warning";
                d.Code = w.value("Code", "");
                d.FileUtf8 = w.value("FileUtf8", "");
                d.Line = w.value("Line", 0);
                d.Column = w.value("Column", 0);
                d.MessageUtf8 = w.value("MessageUtf8", "");
                outResponse.Warnings.push_back(std::move(d));
            }
        }
        if (j.contains("Errors"))
        {
            for (auto& e : j["Errors"])
            {
                CompileServerDiagnostic d;
                d.Severity = "Error";
                d.Code = e.value("Code", "");
                d.FileUtf8 = e.value("FileUtf8", "");
                d.Line = e.value("Line", 0);
                d.Column = e.value("Column", 0);
                d.MessageUtf8 = e.value("MessageUtf8", "");
                outResponse.Errors.push_back(std::move(d));
            }
        }
        if (j.contains("AssemblyBytes") && j["AssemblyBytes"].is_string())
        {
            std::string b64 = j["AssemblyBytes"].get<std::string>();
            DecodeBase64(b64, outResponse.AssemblyBytes);
        }
        if (j.contains("PdbBytes") && j["PdbBytes"].is_string())
        {
            std::string b64 = j["PdbBytes"].get<std::string>();
            DecodeBase64(b64, outResponse.PdbBytes);
        }
        if (j.contains("OutputPath") && j["OutputPath"].is_string())
        {
            outResponse.OutputPathUtf8 = j["OutputPath"].get<std::string>();
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

int CompileServerClient::GetCompileServerSuccessCount()
{
    return g_SuccessCount.load();
}
int CompileServerClient::GetCompileServerFallbackCount()
{
    return g_FallbackCount.load();
}
void CompileServerClient::NoteFallback()
{
    g_FallbackCount.fetch_add(1);
}
void CompileServerClient::ResetCounters()
{
    g_SuccessCount.store(0);
    g_FallbackCount.store(0);
}

} // namespace GameEngine
