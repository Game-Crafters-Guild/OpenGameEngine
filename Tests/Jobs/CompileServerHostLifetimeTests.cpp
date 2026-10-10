// Issue #357: test-spawned CompileServerHost processes must die with the test
// run. Verifies both layers of the fix:
//   1. Hosts spawned through the engine's real launch path while
//      GE_COMPILE_SERVER_EPHEMERAL=1 is set land in the kill-on-close job
//      object (covers crashed/killed runs — the job dies with this process).
//   2. Fixture teardown reaps the host on orderly exit (__shutdown__, bounded
//      grace, then kill), so cleanup does not depend on the job object alone.
#include <gtest/gtest.h>
#include "CompileServerTestHost.h"
#include "Jobs/HotReloadTestHooks.h"
#include <chrono>
#include <thread>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

TEST(CompileServerHostLifetime, TestSpawnedHostIsJobBoundAndReapedByTeardown)
{
#ifdef _WIN32
    const std::string pipeName = CompileServerTestHost::UniquePipeName("Lifetime");

    if (!CompileServerTestHost::StartTestCompileServer(pipeName))
    {
        GTEST_SKIP() << "CompileServerHost not found or failed to start; skipping lifetime test";
    }

    // Cold start (JIT, AV scanning) can take several seconds; poll for the
    // host to come up and self-report its PID over __version__.
    uint32_t pid = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (pid == 0 && std::chrono::steady_clock::now() < deadline)
    {
        pid = CompileServerTestHost::QueryHostPid(pipeName);
        if (pid == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    ASSERT_NE(pid, 0u) << "Host never became reachable on pipe '" << pipeName << "'";

    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    ASSERT_NE(process, nullptr) << "OpenProcess failed for host pid=" << pid;

    // Layer 1: the host must be inside OUR ephemeral job object, not merely
    // "in some job" (test runners often wrap the test exe in their own job).
    HANDLE job = static_cast<HANDLE>(GameEngine::GetCompileServerEphemeralJobHandleForTests());
    ASSERT_NE(job, nullptr) << "Ephemeral job object was never created despite GE_COMPILE_SERVER_EPHEMERAL=1";
    BOOL inJob = FALSE;
    ASSERT_TRUE(IsProcessInJob(process, job, &inJob));
    EXPECT_TRUE(inJob) << "Host pid=" << pid << " is not in the kill-on-close job object";

    // Layer 2: teardown reaping. __shutdown__ + bounded grace must remove the
    // process; the kill fallback caps the wait.
    CompileServerTestHost::ReapHostsOnPipe(pipeName);
    EXPECT_EQ(WaitForSingleObject(process, 5000), WAIT_OBJECT_0)
        << "Host pid=" << pid << " still alive 5s after teardown reap";
    CloseHandle(process);

    // The pipe must be dead too — no other instance left behind.
    EXPECT_EQ(CompileServerTestHost::QueryHostPid(pipeName), 0u);
#else
    GTEST_SKIP() << "Job-object lifetime is Windows-only; POSIX uses PR_SET_PDEATHSIG (Linux)";
#endif
}
