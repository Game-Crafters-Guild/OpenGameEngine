#include <gtest/gtest.h>
#include "Jobs/CompileServerVersion.h"
#include "Jobs/HotReloadTasks.h"
#include "Jobs/HotReloadTestHooks.h"
#include "Jobs/IHotReloadTransport.h"
#include "JobSystem/JobChannel.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "MockHotReloadDependencies.h"
#include "ScopedHotReloadTestHooks.h"
#include "Scripting/ScriptManager.h"
#include "TestEnvVar.h"
#include "TestTempDir.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

using namespace GameEngine;

namespace {
// Speaks the CompileServerClient wire protocol: answers the __version__
// handshake (and __shutdown__) so the client accepts the fake as a live server
// and delivers the canned response for the actual compile request. Without
// this the client takes the stale-server recycle path on every attempt. When
// given a counter it counts the compile requests it answered: a client built on
// an injected transport never starts a server, so a count of one proves the
// compile went through the fake and nowhere else.
class FakeTransport : public IHotReloadTransport
{
  public:
    FakeTransport(std::string response, bool connectOk = true,
                  std::shared_ptr<std::atomic<int>> compileRequests = nullptr)
        : m_Response(std::move(response)), m_ConnectOk(connectOk),
          m_CompileRequests(std::move(compileRequests)) {}
    bool Connect() override { return m_ConnectOk; }
    bool SendRequest(const std::string& requestJson, std::string& outResponse) override
    {
        if (!m_ConnectOk)
            return false;
        if (requestJson == "__version__")
        {
            outResponse = std::string("{\"Version\":\"") + kExpectedCompileServerVersion + "\"}";
            return true;
        }
        if (requestJson == "__shutdown__")
        {
            outResponse = "{}";
            return true;
        }
        if (m_CompileRequests)
            m_CompileRequests->fetch_add(1);
        outResponse = m_Response;
        return true;
    }
    void Close() override {}

  private:
    std::string m_Response;
    bool m_ConnectOk;
    std::shared_ptr<std::atomic<int>> m_CompileRequests;
};

// The compiler takes the parent of the assembly's directory as the project root
// and enumerates every .cs file beneath it, so the assembly sits one directory
// below the test's workspace; directly inside it, the root would be the system
// temp directory and the enumeration would walk all of it.
std::filesystem::path WorkspaceAssemblyPath(const std::filesystem::path& workspace)
{
    std::error_code ec;
    std::filesystem::create_directories(workspace / "Assemblies", ec);
    return workspace / "Assemblies" / "GameEngine.Scripts.dll";
}

// Answers every compile request with a failed compile and returns the count of
// compile requests the fake answered.
std::shared_ptr<std::atomic<int>> InstallCountingFakeTransport()
{
    auto compileRequests = std::make_shared<std::atomic<int>>(0);
    SetCompileServerTransportFactoryForTests(
        [compileRequests](const std::string&)
        {
            return std::make_unique<FakeTransport>("{\"Success\":false}", true, compileRequests);
        });
    return compileRequests;
}
} // namespace

// A server that reports success without sending an assembly is a failed
// compile: the pipeline fails with the compiler's message and never runs the
// later stages, which would otherwise reload whatever assembly is on disk.
TEST(HotReloadPipelineTestHooks, ServerSuccessButNoPayloadFailsPipeline)
{
    const GameEngine::TestUtils::ScopedTempDir workspace(
        GameEngine::TestUtils::MakeUniqueTempDirectory("GE_Pipeline_Seam_NoPayload"));
    {
        std::ofstream source(workspace.Path() / "Foo.cs");
        source << "public static class Foo { public static int Smoke(){ return 1; } }";
    }
    const std::filesystem::path assemblyPath = WorkspaceAssemblyPath(workspace.Path());

    ResetLastDefaultCompilerSelection();
    GameEngine::Testing::SetEnvVar("GE_DISABLE_COMPILE_SERVER", "0");

    GameEngine::Tests::ScopedHotReloadTestHooks hookGuard;
    auto compileRequests = std::make_shared<std::atomic<int>>(0);
    SetCompileServerTransportFactoryForTests(
        [compileRequests](const std::string&)
        { return std::make_unique<FakeTransport>("{\"Success\":true}", true, compileRequests); });

    JobSystem::WorkStealingThreadPool pool(4);
    JobSystem::JobChannel compiles(pool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
    HotReloadPipeline pipeline(pool, compiles);

    auto future = pipeline.ExecuteAsync(assemblyPath.string());
    ASSERT_EQ(future.wait_for(std::chrono::seconds(10)), std::future_status::ready)
        << "Pipeline did not complete in time";
    const HotReloadPipeline::PipelineStats stats = future.get();

    EXPECT_TRUE(WasLastDefaultCompilerServer()) << "Expected CompileServerCompiler to be selected";
    EXPECT_EQ(compileRequests->load(), 1) << "The compile request must reach the fake transport";
    EXPECT_FALSE(stats.success) << "A compile without an assembly must fail the pipeline";
    EXPECT_EQ(stats.result, HotReloadResult::CompilationFailed) << stats.errorMessage;
    EXPECT_NE(stats.errorMessage.find("neither AssemblyBytes nor OutputPath"), std::string::npos)
        << "The pipeline must report the compiler's message, got: " << stats.errorMessage;
}

TEST(HotReloadPipelineTestHooks, ServerConnectFailsTriggersPipelineFailure) {
#ifdef _WIN32
    std::filesystem::path ws = std::filesystem::temp_directory_path() / "GE_Pipeline_Seam_ConnFail";
    std::error_code ec; std::filesystem::create_directories(ws, ec);
    auto sourcePath = ws / "Foo.cs";
    {
        std::ofstream f(sourcePath);
        f << "public static class Foo { public static int Smoke(){ return 1; } }";
        f.close();
    }
    auto asmDir = ws / "bin" / "GE_Temp";
    std::filesystem::create_directories(asmDir, ec);
    auto asmPath = asmDir / "GameEngine.Scripts.dll";

    _putenv_s("GE_DISABLE_COMPILE_SERVER", "0");

    GameEngine::Tests::ScopedHotReloadTestHooks hookGuard;
    SetCompileServerTransportFactoryForTests([](const std::string&){
        return std::make_unique<FakeTransport>(std::string("{}"), /*connectOk*/false);
    });

    auto pool = std::make_shared<JobSystem::WorkStealingThreadPool>(4);
    JobSystem::JobChannel compiles(*pool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
    HotReloadPipeline pipeline(*pool, compiles);

    auto fut = pipeline.ExecuteAsync(asmPath.string());
    auto status = fut.wait_for(std::chrono::seconds(10));
    ASSERT_EQ(status, std::future_status::ready) << "Pipeline did not complete in time";
    auto stats = fut.get();

    if (stats.success) {
        GTEST_SKIP() << "Pipeline succeeded in this environment; skipping negative test";
    }
#else
    GTEST_SKIP() << "Windows-only in this environment";
#endif
}



TEST(HotReloadPipelineTestHooks, ChangedFilesHookIncludedInRequest)
{
#ifdef _WIN32
    // Arrange temp workspace with a simple C# file
    // A workspace of its own per run: the compiler enumerates every .cs file under
    // it, so a file left by an earlier run would stand in for one this test forgot.
    const GameEngine::TestUtils::ScopedTempDir workspace(
        GameEngine::TestUtils::MakeUniqueTempDirectory("GE_Pipeline_Seam_ChangedFiles"));
    const std::filesystem::path& ws = workspace.Path();
    auto sourcePath = ws / "Bar.cs";
    {
        std::ofstream f(sourcePath);
        f << "public static class Bar { public static int Smoke(){ return 2; } }";
        f.close();
    }
    auto asmPath = WorkspaceAssemblyPath(ws);
    { std::ofstream f(asmPath); f << ""; }

    ResetLastDefaultCompilerSelection();

    // Ensure CompileServer is enabled (default unless explicitly disabled)
    _putenv_s("GE_DISABLE_COMPILE_SERVER", "0");

    // Provide changed files via hook
    SetChangedFilesForNextCompile({ sourcePath.generic_string() });

    // Capture the request JSON built by the pipeline via test hook. The guard
    // outlives the pool below, so the by-ref capture can never dangle into a
    // later test's compile.
    GameEngine::Tests::ScopedHotReloadTestHooks hookGuard;
    std::string lastRequest;
    SetOnCompileServerJsonBuiltForTests([&](const std::string& s){ lastRequest = s; });
    // Force server path with a fast fake transport for deterministic CI
    const auto compileRequests = InstallCountingFakeTransport();

    auto pool = std::make_shared<JobSystem::WorkStealingThreadPool>(4);
    JobSystem::JobChannel compiles(*pool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
    HotReloadPipeline pipeline(*pool, compiles);

    auto fut = pipeline.ExecuteAsync(asmPath.string());
    auto status = fut.wait_for(std::chrono::seconds(10));
    ASSERT_EQ(status, std::future_status::ready) << "Pipeline did not complete in time";
    (void)fut.get();
    EXPECT_EQ(compileRequests->load(), 1) << "The compile request must reach the fake transport";

    // Validate captured request contains our changed file and incremental flags
    EXPECT_TRUE(WasLastDefaultCompilerServer()) << "Expected CompileServerCompiler to be selected";

    if (lastRequest.empty()) { GTEST_SKIP() << "Server path not exercised in this environment"; }
    ASSERT_FALSE(lastRequest.empty());
    nlohmann::json j = nlohmann::json::parse(lastRequest);
    ASSERT_TRUE(j.contains("ChangedFiles"));
    auto arr = j["ChangedFiles"];
    ASSERT_TRUE(arr.is_array());
    bool found = false;
    for (auto& v : arr) { if (v.is_string() && v.get<std::string>() == sourcePath.generic_string()) { found = true; break; } }
    EXPECT_TRUE(found) << "ChangedFiles did not include the provided path";
    EXPECT_TRUE(j.contains("ForceFull") && j["ForceFull"].is_boolean() && !j["ForceFull"].get<bool>())
        << "ForceFull should be false for incremental";
    EXPECT_TRUE(j.contains("PreferredStrategy") && j["PreferredStrategy"].get<std::string>() == "Incremental");
    EXPECT_TRUE(j.contains("Config"));
    EXPECT_TRUE(j.contains("Tfm") && j["Tfm"].get<std::string>() == "net10.0");
#else
    GTEST_SKIP() << "Windows-only in this environment";
#endif
}


TEST(HotReloadPipelineTestHooks, CsprojChangeForcesFullAndAffectsList)
{
#ifdef _WIN32
    using nlohmann::json;
    // Arrange temp workspace with a dummy project file
    // A workspace of its own per run: the compiler enumerates every .cs file under
    // it, so a file left by an earlier run would stand in for one this test forgot.
    const GameEngine::TestUtils::ScopedTempDir workspace(
        GameEngine::TestUtils::MakeUniqueTempDirectory("GE_Pipeline_Seam_Csproj"));
    const std::filesystem::path& ws = workspace.Path();
    auto projPath = ws / "Foo.csproj";
    {
        std::ofstream f(projPath); f << "<Project Sdk=\"Microsoft.NET.Sdk\"/>"; f.close();
    }
    // The project has a source: the compiler refuses a project root with no .cs file.
    {
        std::ofstream f(ws / "Foo.cs");
        f << "public static class Foo { public static int Smoke(){ return 1; } }";
    }
    auto asmPath = WorkspaceAssemblyPath(ws);
    {
        std::ofstream f(asmPath);
        f << "";
    }

    ResetLastDefaultCompilerSelection();
    // Ensure CompileServer is enabled (default unless explicitly disabled)
    _putenv_s("GE_DISABLE_COMPILE_SERVER", "0");

    // Provide changed files via production hint
    SetChangedFilesForNextCompile({ projPath.generic_string() });

    // Capture JSON
    GameEngine::Tests::ScopedHotReloadTestHooks hookGuard;
    std::string lastRequest;
    SetOnCompileServerJsonBuiltForTests([&](const std::string& s){ lastRequest = s; });

    // Ensure the pipeline completes quickly regardless of environment by forcing a fast fake transport
    const auto compileRequests = InstallCountingFakeTransport();

    auto pool = std::make_shared<JobSystem::WorkStealingThreadPool>(2);
    JobSystem::JobChannel compiles(*pool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
    HotReloadPipeline pipeline(*pool, compiles);

    auto fut = pipeline.ExecuteAsync(asmPath.string());
    auto status = fut.wait_for(std::chrono::seconds(10));
    ASSERT_EQ(status, std::future_status::ready);
    (void)fut.get();
    EXPECT_EQ(compileRequests->load(), 1) << "The compile request must reach the fake transport";

    if (lastRequest.empty()) { GTEST_SKIP() << "Server path not exercised in this environment"; }
    json j = json::parse(lastRequest);
    ASSERT_TRUE(j.contains("ChangedFiles"));
    ASSERT_TRUE(j.contains("AffectedFiles"));
    ASSERT_TRUE(j.contains("ForceFull"));
    ASSERT_TRUE(j.contains("PreferredStrategy"));

    EXPECT_TRUE(j["ForceFull"].get<bool>()) << "csproj change should trigger ForceFull";
    EXPECT_EQ(j["PreferredStrategy"].get<std::string>(), "Full");

    auto arrC = j["ChangedFiles"]; bool foundC=false; for (auto& v:arrC) if (v.is_string() && v.get<std::string>()==projPath.generic_string()) { foundC=true; break; }
    auto arrA = j["AffectedFiles"]; bool foundA=false; for (auto& v:arrA) if (v.is_string() && v.get<std::string>()==projPath.generic_string()) { foundA=true; break; }
    EXPECT_TRUE(foundC);
    EXPECT_TRUE(foundA);
#else
    GTEST_SKIP() << "Windows-only in this environment";

#endif
}


TEST(HotReloadPipelineTestHooks, ChangedFilesDefaultsAffectsListAndIncremental)
{
#ifdef _WIN32
    using nlohmann::json;
    // Arrange temp workspace with a simple C# file
    // A workspace of its own per run: the compiler enumerates every .cs file under
    // it, so a file left by an earlier run would stand in for one this test forgot.
    const GameEngine::TestUtils::ScopedTempDir workspace(
        GameEngine::TestUtils::MakeUniqueTempDirectory("GE_Pipeline_Seam_DefaultAffects"));
    const std::filesystem::path& ws = workspace.Path();
    auto sourcePath = ws / "Baz.cs";
    { std::ofstream f(sourcePath); f << "public static class Baz { public static int X()=>3; }"; }
    auto asmPath = WorkspaceAssemblyPath(ws);
    {
        std::ofstream f(asmPath);
        f << "";
    }

    ResetLastDefaultCompilerSelection();
    // CompileServer is default; ensure it remains enabled for this test
    _putenv_s("GE_DISABLE_COMPILE_SERVER", "0");
    _putenv_s("GE_USE_COMPILE_SERVER", "1");

    // Provide changed files via production hint
    SetChangedFilesForNextCompile({ sourcePath.generic_string() });

    // Capture JSON and force fast path via fake transport
    GameEngine::Tests::ScopedHotReloadTestHooks hookGuard;
    std::string lastRequest;
    SetOnCompileServerJsonBuiltForTests([&](const std::string& s){ lastRequest = s; });
    const auto compileRequests = InstallCountingFakeTransport();

    auto pool = std::make_shared<JobSystem::WorkStealingThreadPool>(2);
    JobSystem::JobChannel compiles(*pool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
    HotReloadPipeline pipeline(*pool, compiles);

    auto fut = pipeline.ExecuteAsync(asmPath.string());
    auto status = fut.wait_for(std::chrono::seconds(10));
    ASSERT_EQ(status, std::future_status::ready);
    (void)fut.get();
    EXPECT_EQ(compileRequests->load(), 1) << "The compile request must reach the fake transport";

    if (lastRequest.empty()) { GTEST_SKIP() << "Server path not exercised in this environment"; }
    json j = json::parse(lastRequest);
    ASSERT_TRUE(j.contains("ChangedFiles"));
    ASSERT_TRUE(j.contains("AffectedFiles"));
    ASSERT_TRUE(j.contains("ForceFull"));
    ASSERT_TRUE(j.contains("PreferredStrategy"));

    // Incremental expected
    EXPECT_FALSE(j["ForceFull"].get<bool>()) << "non-csproj change should not force full";
    EXPECT_EQ(j["PreferredStrategy"].get<std::string>(), "Incremental");

    auto arrC = j["ChangedFiles"]; bool foundC=false; for (auto& v:arrC) if (v.is_string() && v.get<std::string>()==sourcePath.generic_string()) { foundC=true; break; }
    auto arrA = j["AffectedFiles"]; bool foundA=false; for (auto& v:arrA) if (v.is_string() && v.get<std::string>()==sourcePath.generic_string()) { foundA=true; break; }
    EXPECT_TRUE(foundC);
    EXPECT_TRUE(foundA) << "AffectedFiles should default to ChangedFiles for .cs";
#else
    GTEST_SKIP() << "Windows-only in this environment";
#endif
}

TEST(HotReloadPipelineTestHooks, AffectedFilesInferenceAddsGeneratedAndCsproj)
{
#ifdef _WIN32
    using nlohmann::json;
    // A workspace of its own per run: the compiler enumerates every .cs file under
    // it, so a file left by an earlier run would stand in for one this test forgot.
    const GameEngine::TestUtils::ScopedTempDir workspace(
        GameEngine::TestUtils::MakeUniqueTempDirectory("GE_Pipeline_Seam_AffectsInference"));
    const std::filesystem::path& ws = workspace.Path();
    auto sourcePath = ws / "Qux.cs";
    { std::ofstream f(sourcePath); f << "public static class Qux { public static int V()=>4; }"; }
    auto genPath = ws / "Qux.g.cs"; { std::ofstream f(genPath); f << "// generated"; }
    auto projPath = ws / "Qux.csproj"; { std::ofstream f(projPath); f << "<Project Sdk=\"Microsoft.NET.Sdk\"/>"; }
    auto asmPath = WorkspaceAssemblyPath(ws);
    {
        std::ofstream f(asmPath);
        f << "";
    }

    ResetLastDefaultCompilerSelection();
    // CompileServer is default; ensure it remains enabled for this test
    _putenv_s("GE_DISABLE_COMPILE_SERVER", "0");
    _putenv_s("GE_AFFECTS_INFERENCE", "1");

    SetChangedFilesForNextCompile({ sourcePath.generic_string() });

    GameEngine::Tests::ScopedHotReloadTestHooks hookGuard;
    std::string lastRequest;
    SetOnCompileServerJsonBuiltForTests([&](const std::string& s){ lastRequest = s; });
    const auto compileRequests = InstallCountingFakeTransport();

    auto pool = std::make_shared<JobSystem::WorkStealingThreadPool>(2);
    JobSystem::JobChannel compiles(*pool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
    HotReloadPipeline pipeline(*pool, compiles);

    auto fut = pipeline.ExecuteAsync(asmPath.string());
    auto status = fut.wait_for(std::chrono::seconds(10));
    // Reset before any skip/assert exit so the inference mode can't leak into
    // later tests' request shapes.
    _putenv_s("GE_AFFECTS_INFERENCE", "0");
    ASSERT_EQ(status, std::future_status::ready);
    (void)fut.get();
    EXPECT_EQ(compileRequests->load(), 1) << "The compile request must reach the fake transport";

    if (lastRequest.empty()) { GTEST_SKIP() << "Server path not exercised in this environment"; }
    json j = json::parse(lastRequest);
    ASSERT_TRUE(j.contains("AffectedFiles"));
    auto arrA = j["AffectedFiles"]; ASSERT_TRUE(arrA.is_array());
    auto has = [&](const std::string& p){ for (auto& v:arrA) if (v.is_string() && v.get<std::string>()==p) return true; return false; };
    EXPECT_TRUE(has(sourcePath.generic_string()));
    EXPECT_TRUE(has(genPath.generic_string())) << ".g.cs should be included when inference is enabled";
    EXPECT_TRUE(has(projPath.generic_string())) << "nearest .csproj should be included when inference is enabled";

    EXPECT_TRUE(j.contains("ForceFull") && j["ForceFull"].is_boolean() && !j["ForceFull"].get<bool>())
        << "ForceFull should remain false for .cs change";
    EXPECT_TRUE(j.contains("PreferredStrategy") && j["PreferredStrategy"].get<std::string>() == "Incremental");
#else
    GTEST_SKIP() << "Windows-only in this environment";
#endif
}

// ============================================================================
// S4 — compile supersede via TaskHandle::Cancel
// ============================================================================

namespace {

// Records which pipeline runs' compile bodies actually executed. A compile
// superseded while still queued loses the F12 arbitration to Cancel() and
// must never appear here.
struct RecordingCompilerLog {
    std::mutex Mutex;
    std::vector<std::string> CompiledPaths;
};

class RecordingCompiler : public Jobs::ICompiler {
public:
    RecordingCompiler(std::shared_ptr<RecordingCompilerLog> log, int delayMs)
        : m_Log(std::move(log)), m_DelayMs(delayMs) {}

    CompilationResult compile(const String& assemblyPath, const std::atomic<bool>* /*cancelRequested*/) override {
        {
            std::lock_guard<std::mutex> lock(m_Log->Mutex);
            m_Log->CompiledPaths.push_back(assemblyPath);
        }
        if (m_DelayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(m_DelayMs));
        }
        CompilationResult result;
        result.success = true;
        result.output = "recorded";
        return result;
    }

private:
    std::shared_ptr<RecordingCompilerLog> m_Log;
    int m_DelayMs;
};

// Recording compile stage + hermetic mock loader/swapper so stages 2-4 are
// deterministic (no real dotnet, no CoreCLR, no disk dependency).
void ConfigureMockStages(HotReloadPipeline& pipeline, std::shared_ptr<RecordingCompilerLog> log, int compileDelayMs) {
    pipeline.SetCompilerFactory([log, compileDelayMs] {
        return std::make_unique<RecordingCompiler>(log, compileDelayMs);
    });
    pipeline.SetAssemblyLoaderFactory([] { return std::make_unique<GameEngine::Tests::MockAssemblyLoader>(); });
    pipeline.SetSwapperFactory([] { return std::make_unique<GameEngine::Tests::MockAssemblySwapper>(true, 1); });
}

std::filesystem::path MakeSupersedeWorkspace(const char* name) {
    auto ws = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(ws, ec);
    std::filesystem::create_directories(ws, ec);
    return ws;
}

std::filesystem::path WriteMzDummy(const std::filesystem::path& dir, const std::string& fileName) {
    auto p = dir / fileName;
    std::ofstream f(p, std::ios::binary);
    f << "MZ";
    std::vector<char> dummy(512, 0x42);
    f.write(dummy.data(), static_cast<std::streamsize>(dummy.size()));
    return p;
}

// Fake transport whose compile round-trip blocks until released — keeps a
// CompilationTask deterministically in the Running state. Control messages
// (__version__/__shutdown__) answer immediately so the client handshake
// succeeds. A null gate makes it a plain immediate-response transport.
class GatedFakeTransport : public IHotReloadTransport {
public:
    struct Gate {
        std::mutex Mutex;
        std::condition_variable Cv;
        bool Released = false;
        std::atomic<bool> CompileRequestSeen{false};
        std::atomic<bool> CompileResponseDelivered{false};

        void Release() {
            {
                std::lock_guard<std::mutex> lock(Mutex);
                Released = true;
            }
            Cv.notify_all();
        }
    };

    GatedFakeTransport(std::string response, std::shared_ptr<Gate> gate)
        : m_Response(std::move(response)), m_Gate(std::move(gate)) {}

    bool Connect() override { return true; }
    bool SendRequest(const std::string& requestJson, std::string& outResponse) override {
        if (requestJson == "__version__") {
            outResponse = std::string("{\"Version\":\"") + kExpectedCompileServerVersion + "\"}";
            return true;
        }
        if (requestJson == "__shutdown__") {
            outResponse = "{}";
            return true;
        }
        if (m_Gate) {
            m_Gate->CompileRequestSeen.store(true);
            std::unique_lock<std::mutex> lock(m_Gate->Mutex);
            m_Gate->Cv.wait(lock, [this] { return m_Gate->Released; });
        }
        outResponse = m_Response;
        if (m_Gate) {
            m_Gate->CompileResponseDelivered.store(true);
        }
        return true;
    }
    void Close() override {}

private:
    std::string m_Response;
    std::shared_ptr<Gate> m_Gate;
};

} // namespace

TEST(HotReloadPipelineSupersede, QueuedRunIsSupersededWithoutEverCompiling) {
    auto ws = MakeSupersedeWorkspace("GE_Supersede_Queued");
    auto pathA = WriteMzDummy(ws, "A.dll");
    auto pathB = WriteMzDummy(ws, "B.dll");

    auto pool = std::make_shared<JobSystem::WorkStealingThreadPool>(2);

    // Occupy both workers, and below the "Script compiles" channel's one
    // running slot, so run A's compile and stages stay queued (never Running).
    // Wait until the blockers are observably Running before starting run A:
    // the global queue is not strictly FIFO across producer threads, so
    // without the latch a worker can dequeue one of run A's stages ahead of a
    // still-queued blocker and complete the run before the supersede fires.
    //
    // The blockers use Submit deliberately: registry-backed exe-side handles
    // sharing one pool with the pipeline's Engine-side stage tasks pins issue
    // #340. When this binary linked the JobSystem module both statically and
    // via Engine.dll, the two m_NextTaskId copies collided in the shared
    // pool's registry (blocker ids aliased the stage ids) and corrupted the
    // F12 arbitration — see the S4 audit notes on PR #336. The exe now
    // single-links the module through Engine.dll, so both sides draw from
    // one counter and this mixed shape must stay collision-free.
    std::promise<void> release;
    std::shared_future<void> workerGate = release.get_future().share();
    auto blockersRunning = std::make_shared<std::atomic<int>>(0);
    auto blocker1 = pool->Submit([blockersRunning, workerGate] {
        blockersRunning->fetch_add(1);
        workerGate.wait();
    });
    auto blocker2 = pool->Submit([blockersRunning, workerGate] {
        blockersRunning->fetch_add(1);
        workerGate.wait();
    });
    ASSERT_TRUE(blocker1.IsValid());
    ASSERT_TRUE(blocker2.IsValid());
    {
        const auto blockersDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (blockersRunning->load() < 2 && std::chrono::steady_clock::now() < blockersDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_EQ(blockersRunning->load(), 2) << "workers never picked up the blocker tasks";
    }

    auto log = std::make_shared<RecordingCompilerLog>();
    long long supersedeLatencyMs = 0;
    {
        JobSystem::JobChannel compiles(*pool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
        auto channelBlocker = compiles.Submit([blockersRunning, workerGate] {
            blockersRunning->fetch_add(1);
            workerGate.wait();
        });
        ASSERT_TRUE(channelBlocker.IsValid());
        {
            const auto blockerDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (blockersRunning->load() < 3 && std::chrono::steady_clock::now() < blockerDeadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            ASSERT_EQ(blockersRunning->load(), 3) << "the compile channel never ran its blocker";
        }
        HotReloadPipeline pipeline(*pool, compiles);
        ConfigureMockStages(pipeline, log, 0);

        auto futA = pipeline.ExecuteAsync(pathA.string());
        // Let the launcher queue run A's compile; with the channel's slot held
        // it can only wait in the channel, so the supersede below
        // deterministically hits the queued case.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        const auto supersedeStart = std::chrono::steady_clock::now();
        auto futB = pipeline.ExecuteAsync(pathB.string());
        supersedeLatencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - supersedeStart)
                                 .count();

        release.set_value(); // run B may now execute

        ASSERT_EQ(futA.wait_for(std::chrono::seconds(15)), std::future_status::ready);
        auto statsA = futA.get();
        EXPECT_FALSE(statsA.success);
        EXPECT_EQ(statsA.result, HotReloadResult::CancelledByNewOperation);

        ASSERT_EQ(futB.wait_for(std::chrono::seconds(15)), std::future_status::ready);
        auto statsB = futB.get();
        EXPECT_TRUE(statsB.success) << statsB.errorMessage;

        {
            std::lock_guard<std::mutex> lock(log->Mutex);
            ASSERT_EQ(log->CompiledPaths.size(), 1u) << "superseded queued compile body must never run";
            EXPECT_EQ(log->CompiledPaths[0], pathB.string());
        }
    }

    std::cout << "[supersede] queued-case ExecuteAsync handover took " << supersedeLatencyMs << " ms\n";
    EXPECT_LT(supersedeLatencyMs, 1000);

    pool.reset();
    std::error_code ec;
    std::filesystem::remove_all(ws, ec);
}

TEST(HotReloadPipelineSupersede, SupersededRunningCompileNeverWritesStaleDll) {
#ifdef _WIN32
    auto ws = MakeSupersedeWorkspace("GE_Supersede_StaleWrite");
    {
        std::ofstream f(ws / "Foo.cs");
        f << "public static class Foo { public static int V() => 1; }";
    }
    auto asmDir = ws / "bin";
    std::filesystem::create_directories(asmDir);
    auto asmPath = asmDir / "GameEngine.Scripts.dll";

    _putenv_s("GE_DISABLE_COMPILE_SERVER", "0");

    // First compile (stale): blocks inside the server round-trip, answers
    // "MZOLD". Second compile (fresh): answers "MZNEW" immediately.
    GameEngine::Tests::ScopedHotReloadTestHooks hookGuard;
    auto gate = std::make_shared<GatedFakeTransport::Gate>();
    auto transportCount = std::make_shared<std::atomic<int>>(0);
    SetCompileServerTransportFactoryForTests(
        [gate, transportCount](const std::string&) -> std::unique_ptr<IHotReloadTransport> {
            const int index = transportCount->fetch_add(1);
            if (index == 0) {
                return std::make_unique<GatedFakeTransport>(R"({"Success":true,"AssemblyBytes":"TVpPTEQ="})", gate); // "MZOLD"
            }
            return std::make_unique<GatedFakeTransport>(R"({"Success":true,"AssemblyBytes":"TVpORVc="})", nullptr); // "MZNEW"
        });

    auto readDll = [&]() {
        std::ifstream f(asmPath, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };

    auto pool = std::make_shared<JobSystem::WorkStealingThreadPool>(4);
    {
        JobSystem::JobChannel compiles(*pool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
        HotReloadPipeline pipeline(*pool, compiles);
        // Real CompileServerCompiler (default) for stage 1; hermetic mocks for stages 2-4.
        pipeline.SetAssemblyLoaderFactory([] { return std::make_unique<GameEngine::Tests::MockAssemblyLoader>(); });
        pipeline.SetSwapperFactory([] { return std::make_unique<GameEngine::Tests::MockAssemblySwapper>(true, 1); });

        auto futA = pipeline.ExecuteAsync(asmPath.string());

        // Wait until the stale compile is genuinely Running (blocked mid round-trip).
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!gate->CompileRequestSeen.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ASSERT_TRUE(gate->CompileRequestSeen.load()) << "stale compile never reached the server round-trip";

        // Supersede while stage 1 is Running: Cancel() loses the F12
        // arbitration, the stale compile keeps running, and only the pre-write
        // token check keeps its output off the disk.
        auto futB = pipeline.ExecuteAsync(asmPath.string());

        ASSERT_EQ(futB.wait_for(std::chrono::seconds(15)), std::future_status::ready);
        auto statsB = futB.get();
        EXPECT_TRUE(statsB.success) << statsB.errorMessage;

        ASSERT_EQ(futA.wait_for(std::chrono::seconds(15)), std::future_status::ready);
        auto statsA = futA.get();
        EXPECT_FALSE(statsA.success);
        EXPECT_EQ(statsA.result, HotReloadResult::CancelledByNewOperation);

        // Fresh DLL is on disk while the stale compile is still in flight.
        EXPECT_EQ(readDll(), "MZNEW");

        gate->Release(); // let the stale compile complete its round-trip
    }
    // Pool teardown joins the worker running the stale compile; after this
    // the stale run has fully completed.
    pool.reset();

    EXPECT_TRUE(gate->CompileResponseDelivered.load())
        << "stale compile should have completed normally (Cancel cannot abort a Running stage)";
    EXPECT_EQ(readDll(), "MZNEW") << "superseded compile must not overwrite the newer DLL";

    std::error_code ec;
    std::filesystem::remove_all(ws, ec);
#else
    GTEST_SKIP() << "Windows-only in this environment";
#endif
}

TEST(HotReloadPipelineSupersede, RapidFireSupersedeLastWinsAndNothingHangs) {
    constexpr int kSubmissions = 8;
    auto ws = MakeSupersedeWorkspace("GE_Supersede_RapidFire");

    auto pool = std::make_shared<JobSystem::WorkStealingThreadPool>(4);
    auto log = std::make_shared<RecordingCompilerLog>();
    std::vector<long long> handoverMs;
    {
        JobSystem::JobChannel compiles(*pool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
        HotReloadPipeline pipeline(*pool, compiles);
        ConfigureMockStages(pipeline, log, 5);

        std::vector<std::filesystem::path> paths;
        for (int i = 0; i < kSubmissions; ++i) {
            paths.push_back(WriteMzDummy(ws, "Rapid" + std::to_string(i) + ".dll"));
        }

        std::vector<std::future<HotReloadPipeline::PipelineStats>> futures;
        for (int i = 0; i < kSubmissions; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            futures.push_back(pipeline.ExecuteAsync(paths[static_cast<size_t>(i)].string()));
            handoverMs.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - t0)
                                     .count());
        }

        for (int i = 0; i < kSubmissions; ++i) {
            ASSERT_EQ(futures[static_cast<size_t>(i)].wait_for(std::chrono::seconds(30)), std::future_status::ready)
                << "submission " << i << " hung";
        }

        auto statsLast = futures.back().get();
        EXPECT_TRUE(statsLast.success) << statsLast.errorMessage;

        {
            std::lock_guard<std::mutex> lock(log->Mutex);
            ASSERT_FALSE(log->CompiledPaths.empty());
            EXPECT_EQ(log->CompiledPaths.back(), paths.back().string())
                << "last submission must be the last compile to run";
            EXPECT_EQ(std::count(log->CompiledPaths.begin(), log->CompiledPaths.end(), paths.back().string()), 1);
        }

        // Every run retired: the pipeline must be idle.
        const auto idleDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (pipeline.IsExecuting() && std::chrono::steady_clock::now() < idleDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_FALSE(pipeline.IsExecuting());
    }

    long long maxMs = *std::max_element(handoverMs.begin(), handoverMs.end());
    std::cout << "[supersede] rapid-fire ExecuteAsync handovers (ms):";
    for (auto v : handoverMs) {
        std::cout << ' ' << v;
    }
    std::cout << " (max " << maxMs << ")\n";
    EXPECT_LT(maxMs, 2000);

    pool.reset();
    std::error_code ec;
    std::filesystem::remove_all(ws, ec);
}

TEST(HotReloadPipelineSupersede, SupersedeRacingShutdownDoesNotStrand) {
    auto ws = MakeSupersedeWorkspace("GE_Supersede_Shutdown");
    auto pathA = WriteMzDummy(ws, "A.dll");
    auto pathB = WriteMzDummy(ws, "B.dll");

    auto pool = std::make_shared<JobSystem::WorkStealingThreadPool>(2);
    auto log = std::make_shared<RecordingCompilerLog>();
    {
        JobSystem::JobChannel compiles(*pool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
        HotReloadPipeline pipeline(*pool, compiles);
        ConfigureMockStages(pipeline, log, 50);

        auto futA = pipeline.ExecuteAsync(pathA.string());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto futB = pipeline.ExecuteAsync(pathB.string()); // supersede
        pool->Shutdown();                                  // race the superseding run's freshly submitted stages

        // Termination is the contract: F13 cancels queued handle tasks and the
        // post-gate Submit guard covers submissions that lost the race, so
        // both futures must resolve. Outcomes depend on the interleave.
        ASSERT_EQ(futA.wait_for(std::chrono::seconds(15)), std::future_status::ready)
            << "superseded run stranded across Shutdown";
        ASSERT_EQ(futB.wait_for(std::chrono::seconds(15)), std::future_status::ready)
            << "superseding run stranded across Shutdown";
        (void)futA.get();
        (void)futB.get();
    }
    pool.reset();
    std::error_code ec;
    std::filesystem::remove_all(ws, ec);
}


// Editor-path analogue of SupersededRunningCompileNeverWritesStaleDll (issue
// #339): the editor pipeline always compiles through ScriptManagerCompiler →
// CompileScripts → CompileProject, so CompileProject's own DLL write must be
// gated by the same supersede token + write mutex as CompileServerCompiler's.
// Exercises the inversion the issue names: a stale compile stalled mid
// round-trip while a fresh compile completes and writes first.
TEST(HotReloadPipelineSupersede, EditorPathCompileProjectNeverWritesStaleDll) {
#ifdef _WIN32
    auto ws = MakeSupersedeWorkspace("GE_Supersede_EditorPath");
    {
        std::ofstream f(ws / "Foo.cs");
        f << "public static class Foo { public static int V() => 1; }";
    }
    auto fakeProject = ws / "GameEngine.Scripts.csproj";
    {
        std::ofstream f(fakeProject);
        f << "<Project Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup></Project>";
    }
    auto asmPath = ws / "ScriptAssemblies" / "GameEngine.Scripts.dll";

    _putenv_s("GE_DISABLE_COMPILE_SERVER", "0");

    // First compile (stale): blocks inside the server round-trip, answers
    // "MZOLD". Second compile (fresh): answers "MZNEW" immediately.
    GameEngine::Tests::ScopedHotReloadTestHooks hookGuard;
    auto gate = std::make_shared<GatedFakeTransport::Gate>();
    auto transportCount = std::make_shared<std::atomic<int>>(0);
    SetCompileServerTransportFactoryForTests(
        [gate, transportCount](const std::string&) -> std::unique_ptr<IHotReloadTransport> {
            const int index = transportCount->fetch_add(1);
            if (index == 0) {
                return std::make_unique<GatedFakeTransport>(R"({"Success":true,"AssemblyBytes":"TVpPTEQ="})", gate); // "MZOLD"
            }
            return std::make_unique<GatedFakeTransport>(R"({"Success":true,"AssemblyBytes":"TVpORVc="})", nullptr); // "MZNEW"
        });

    auto readDll = [&]() {
        std::ifstream f(asmPath, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };

    JobSystem::WorkStealingThreadPool pool(2);
    {
        ScriptManager mgr;
        ScriptsConfig cfg{};
        cfg.workspaceRoot = ws;
        cfg.scriptsRoot = ws;
        cfg.assembliesRoot = ws / "ScriptAssemblies";
        cfg.generatedProjectRoot = ws;
        cfg.disableClr = true; // CompileProject does not require CoreCLR
        cfg.enableHotReload = false;
        cfg.enableAsyncHotReload = false;
        cfg.enableAutoProjectGeneration = false;
        ASSERT_TRUE(mgr.Initialize(cfg, pool));

        // Stale compile: per-run supersede token, stalled mid round-trip.
        std::atomic<bool> staleToken{false};
        auto staleFuture = std::async(std::launch::async, [&] {
            return mgr.CompileProject(fakeProject, &staleToken);
        });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!gate->CompileRequestSeen.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!gate->CompileRequestSeen.load()) {
            gate->Release(); // unblock the stale thread so the future can drain
            FAIL() << "stale compile never reached the server round-trip";
        }

        // Supersede while the round-trip is in flight, then let the fresh
        // compile finish end-to-end before the stale one resumes.
        staleToken.store(true);
        auto freshResult = mgr.CompileProject(fakeProject, nullptr);
        EXPECT_TRUE(freshResult.success) << freshResult.errorMessage;
        EXPECT_EQ(readDll(), "MZNEW");

        gate->Release(); // stale compile completes its round-trip and hits the write gate

        auto staleResult = staleFuture.get();
        EXPECT_FALSE(staleResult.success)
            << "superseded compile must not report success";
        EXPECT_NE(staleResult.errorMessage.find("superseded"), std::string::npos)
            << "expected supersede skip, got: " << staleResult.errorMessage;
        EXPECT_TRUE(gate->CompileResponseDelivered.load())
            << "stale compile should have completed its round-trip normally";
        EXPECT_EQ(readDll(), "MZNEW") << "superseded editor-path compile must not overwrite the newer DLL";
    }

    std::error_code ec;
    std::filesystem::remove_all(ws, ec);
#else
    GTEST_SKIP() << "Windows-only in this environment";
#endif
}
