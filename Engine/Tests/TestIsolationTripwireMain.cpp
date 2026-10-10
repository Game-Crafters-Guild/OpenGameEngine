// Custom gtest main for the render-services test executables (EngineMaterialTests
// and its siblings in Engine/CMakeLists.txt) with an isolation tripwire: after
// every test, verify no cross-test residue — debug-heap guard corruption,
// outstanding global JobSystem work, or engine threads that outlive the test
// that spawned them. Order-dependent flakes in these suites (e.g.
// MaterialRegistryTest.Register_InitializesParamCache failing once mid-run,
// 2026-07) are only diagnosable when the polluting test is NAMED at the moment
// it leaks, not when a later victim trips over the residue.
//
// Checks, in cost order:
//   1. _CrtCheckMemory (Debug CRT only, ~14 ms/test): full guard-byte sweep;
//      a test that scribbles past a heap block fails HERE, not 400 tests
//      later. Always fatal — heap corruption is never noise.
//   2. Global JobSystem residue: queued or in-flight work surviving the test
//      that submitted it. It reads the pool only while EngineCore is
//      initialized at test end, so it is free for tests that never start the
//      engine or shut it down before they return; a test that leaves the
//      engine running is held to the async-compile/prewarm teardown contract.
//   3. Thread stragglers (GE_TEST_ISOLATION_THREADS=1 only — Toolhelp thread
//      snapshots enumerate system-wide and cost ~100 ms/test on a busy box),
//      attributed by Win32 start address to the owning module. Engine-code
//      stragglers (exe / engine DLLs / CRT thread thunks) get a settle
//      window and are an offense if they persist — they can scribble freed
//      engine heap into the next test. Driver / OS pool / injected-hook
//      threads (nvoglv, ntdll TppWorker, OBS graphics-hook, ...) wind down
//      on their own schedule and are logged transiently without polling.
//      This is the forensic mode for order-dependent flake hunts.
//
// Modes:
//   default                    — heap + pool checks (~+6% wall); offenses log
//                                to stderr ("[ISOLATION] ..."); only heap
//                                corruption fails the test.
//   GE_TEST_ISOLATION_THREADS=1 — adds per-test thread-straggler attribution.
//   GE_TEST_ISOLATION_STRICT=1  — pool residue and persistent engine-thread
//                                 leaks also fail the offending test (CI).

#include <gtest/gtest.h>

#include "Core/Engine.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <winternl.h>
#ifdef _DEBUG
#include <crtdbg.h>
#endif

namespace
{

std::set<DWORD> SnapshotThreadIds()
{
    std::set<DWORD> ids;
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return ids;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te))
    {
        do
        {
            if (te.th32OwnerProcessID == pid)
                ids.insert(te.th32ThreadID);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return ids;
}

// ThreadQuerySetWin32StartAddress — stable since XP; not in winternl.h's enum.
constexpr THREADINFOCLASS kThreadQuerySetWin32StartAddress = static_cast<THREADINFOCLASS>(9);

using NtQueryInformationThreadFn = NTSTATUS(NTAPI*)(HANDLE, THREADINFOCLASS, PVOID, ULONG, PULONG);

// Threads whose start address lives in these modules run no engine code:
// they cannot scribble engine heap, and their lifetimes follow the driver /
// OS / injector, not our teardown. Everything NOT listed is treated as
// engine-side (fail-safe direction).
bool IsKnownNoiseModule(const char* moduleName)
{
    static const char* kNoise[] = {
        "ntdll.dll",           // OS threadpool workers (TppWorkerThread)
        "kernel32.dll",        //
        "crypt32.dll",         // certificate cache workers
        "nvoglv64.dll",        // NVIDIA GL/VK worker pool
        "nvcuda64.dll",        //
        "nvapi64.dll",         //
        "amdvlk64.dll",        // AMD ICD
        "atio6axx.dll",        //
        "amdihk64.dll",        // AMD input hook (injected)
        "igxelpicd64.dll",     // Intel ICD
        "graphics-hook64.dll", // OBS game-capture hook (injected)
    };
    for (const char* n : kNoise)
        if (_stricmp(moduleName, n) == 0)
            return true;
    return false;
}

struct ThreadOrigin
{
    DWORD tid = 0;
    char  module[MAX_PATH]{};
    bool  noise = false;
};

ThreadOrigin DescribeThread(DWORD tid)
{
    ThreadOrigin origin;
    origin.tid = tid;
    std::snprintf(origin.module, sizeof(origin.module), "<unknown>");

    HANDLE h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION,
                          FALSE, tid);
    if (!h)
        return origin; // already exited — treat as engine-side unknown

    void* startAddr = nullptr;
    static NtQueryInformationThreadFn ntQuery =
        reinterpret_cast<NtQueryInformationThreadFn>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    if (ntQuery)
        ntQuery(h, kThreadQuerySetWin32StartAddress, &startAddr, sizeof(startAddr), nullptr);
    CloseHandle(h);

    if (startAddr)
    {
        HMODULE mod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                   | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCWSTR>(startAddr), &mod)
            && mod)
        {
            char path[MAX_PATH]{};
            if (GetModuleFileNameA(mod, path, MAX_PATH))
            {
                const char* base = path;
                for (const char* p = path; *p; ++p)
                    if (*p == '\\' || *p == '/')
                        base = p + 1;
                std::snprintf(origin.module, sizeof(origin.module), "%s", base);
                origin.noise = IsKnownNoiseModule(base);
            }
        }
    }
    return origin;
}

struct PoolSnapshot
{
    bool   engineInitialized = false;
    size_t pendingApprox     = 0;
    size_t queueApprox       = 0;
};

PoolSnapshot SnapshotGlobalPool()
{
    PoolSnapshot s{};
    auto& engine = GameEngine::EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return s;
    s.engineInitialized = true;
    auto& pool      = engine.GetJobSystem();
    s.pendingApprox = pool.GetPendingTasksApprox();
    s.queueApprox   = pool.GetApproximateQueueSize();
    return s;
}

class IsolationTripwireListener : public ::testing::EmptyTestEventListener
{
  public:
    void OnTestStart(const ::testing::TestInfo&) override
    {
        if (m_ThreadCheck)
            m_ThreadsAtStart = SnapshotThreadIds();
    }

    void OnTestEnd(const ::testing::TestInfo& info) override
    {
#ifdef _DEBUG
        // Debug-CRT guard-byte sweep of every live heap block: names the test
        // that corrupted the heap AT that test, instead of a later victim
        // failing on the reused block. This is the only deterministic handle
        // on "earlier test scribbles, later test flakes" single-thread bugs.
        if (!_CrtCheckMemory())
        {
            std::fprintf(stderr, "[ISOLATION-HEAP] after %s.%s: _CrtCheckMemory FAILED\n",
                         info.test_suite_name(), info.name());
            std::fflush(stderr);
            ADD_FAILURE() << "isolation tripwire: debug heap corrupted during this test";
        }
#endif

        const PoolSnapshot pool = SnapshotGlobalPool();
        const bool poolBusy     = pool.engineInitialized
                                  && (pool.pendingApprox > 0 || pool.queueApprox > 0);

        std::set<DWORD> now;
        std::vector<ThreadOrigin> engineStragglers;
        std::vector<ThreadOrigin> noiseStragglers;
        if (m_ThreadCheck)
        {
            now = SnapshotThreadIds();
            for (DWORD tid : now)
            {
                if (m_ThreadsAtStart.count(tid))
                    continue;
                ThreadOrigin origin = DescribeThread(tid);
                (origin.noise ? noiseStragglers : engineStragglers).push_back(origin);
            }
        }

        // Engine-code stragglers get a settle window: one that exits within
        // it still outlived the test (logged), one that persists is an
        // offense — it is alive and running engine code inside the NEXT test.
        long settleMs = -1;
        if (!engineStragglers.empty())
        {
            const auto t0       = std::chrono::steady_clock::now();
            const auto deadline = t0 + std::chrono::milliseconds(500);
            while (std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                now = SnapshotThreadIds();
                bool anyAlive = false;
                for (const ThreadOrigin& o : engineStragglers)
                    if (now.count(o.tid))
                        anyAlive = true;
                if (!anyAlive)
                {
                    settleMs = static_cast<long>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count());
                    break;
                }
            }
        }
        size_t persistent = 0;
        for (const ThreadOrigin& o : engineStragglers)
            if (now.count(o.tid))
                ++persistent;

        if (!poolBusy && engineStragglers.empty())
            return; // noise-module churn alone isn't worth a line

        std::fprintf(stderr,
                     "[ISOLATION%s] after %s.%s: pool(init=%d pending=%zu queue=%zu) "
                     "engineStragglers=%zu (settleMs=%ld persistent=%zu) noise=%zu\n",
                     (persistent == 0 && !poolBusy) ? "-TRANSIENT" : "",
                     info.test_suite_name(), info.name(), pool.engineInitialized ? 1 : 0,
                     pool.pendingApprox, pool.queueApprox, engineStragglers.size(),
                     settleMs, persistent, noiseStragglers.size());
        for (const ThreadOrigin& o : engineStragglers)
            std::fprintf(stderr, "[ISOLATION]   straggler tid=%lu module=%s%s\n", o.tid,
                         o.module, now.count(o.tid) ? " (still running)" : " (settled)");
        std::fflush(stderr);

        if (m_Strict && (poolBusy || persistent > 0))
        {
            ADD_FAILURE() << "isolation tripwire: async residue (pool pending="
                          << pool.pendingApprox << " queue=" << pool.queueApprox
                          << " persistent engine threads=" << persistent << ")";
        }
    }

    void SetStrict(bool strict) { m_Strict = strict; }
    void SetThreadCheck(bool enabled) { m_ThreadCheck = enabled; }

  private:
    std::set<DWORD> m_ThreadsAtStart;
    bool            m_Strict      = false;
    bool            m_ThreadCheck = false;
};

} // namespace

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    auto* listener = new IsolationTripwireListener();
    const char* strict = std::getenv("GE_TEST_ISOLATION_STRICT");
    listener->SetStrict(strict && strict[0] == '1');
    const char* threads = std::getenv("GE_TEST_ISOLATION_THREADS");
    listener->SetThreadCheck(threads && threads[0] == '1');
    ::testing::UnitTest::GetInstance()->listeners().Append(listener);
    return RUN_ALL_TESTS();
}
