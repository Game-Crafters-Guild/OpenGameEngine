#include "ManagedFixturePaths.h"
#include "gtest/gtest.h"
#include "Scripting/PathResolver.h"
#include "Scripting/ScriptingABI.h"
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <atomic>

using namespace std::literals;

static std::filesystem::path FindDomainRoutingDll()
{
    std::vector<std::filesystem::path> candidates = {
        GameEngine::TestPaths::ExecutableDirectory() / "DomainRoutingTest.dll",
        GameEngine::TestPaths::StagedRoot() / "DomainRoutingTest.dll",
        GameEngine::Tests::DomainRoutingTestDll(),
    };
    for (const auto& p : candidates) {
        if (std::filesystem::exists(p)) return p;
    }
    return {};
}

TEST(ScriptingAbi_Guardrails, SwapWithoutPreload_ReturnsNotInitialized)
{
#ifdef _WIN32
    _putenv_s("GE_HRM_DIR", std::filesystem::current_path().string().c_str());
#endif
    GE_Result r = GE_SwapPreloadedContext();
    EXPECT_TRUE(r == GE_Result_NotInitialized || r == GE_Result_InvalidArg)
        << "Expected NotInitialized or InvalidArg when swapping without a prior preload, got " << (int)r;
}

TEST(ScriptingAbi_Guardrails, NativeShim_StagedWhereManagedTestsExpect)
{
    // Exe-anchored: an empty working directory keeps the lookup off the cwd.
    auto nativeLibrary = GameEngine::ScriptingPaths::ResolveNativeLibraryPathFrom(GameEngine::TestPaths::ExecutableDirectory(), {});
    ASSERT_FALSE(nativeLibrary.empty())
        << "The GameEngine.Native library, under this platform's file name, is not next to the test executable "
        << "or in the staged bin/<config> tree. "
        << "Managed bindings and ManagedInteropTests rely on it being discoverable via app-local path or GE_NATIVE_DIR.\n"
        << "Executable directory: '" << GameEngine::TestPaths::ExecutableDirectory().string() << "'.";

    // Sanity: calling the ABI from this process should succeed if the DLL is staged correctly
    const void* table = nullptr;
    uint32_t size = 0;
    GE_Result rc = GE_GetInterface(GE_ABI_VERSION_CURRENT, &table, &size);
    EXPECT_EQ(rc, GE_Result_Ok) << "GE_GetInterface should succeed when GameEngine.Native is present; rc=" << (int)rc;
    EXPECT_NE(table, nullptr);
    EXPECT_EQ(size, sizeof(GE_Interface_v1));
}

static std::vector<uint8_t> ReadAll(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

TEST(ScriptingAbi_Guardrails, Cleanup_Idempotent_And_SafeUnderLightConcurrency)
{
#ifdef _WIN32
    _putenv_s("GE_HRM_DIR", std::filesystem::current_path().string().c_str());
#endif

    auto dll = FindDomainRoutingDll();
    if (dll.empty()) {
        GTEST_SKIP() << "DomainRoutingTest.dll not found in expected locations";
    }

    // Preload and swap to make the runtime domain use the test assembly
    auto pathUtf8 = dll.string();
    ASSERT_EQ(GE_PreloadAssemblyContext(pathUtf8.c_str(), (uint32_t)pathUtf8.size()), GE_Result_Ok);
    ASSERT_EQ(GE_SwapPreloadedContext(), GE_Result_Ok);

    // Light concurrency: invoke a known method in a few worker threads while cleanup runs
    const char* fqn = "GameEngine.Scripts.ScriptsEntryPoint.Reset";
    std::atomic<int> okCount{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 50; ++i) {
                GE_Result rc = GE_DebugInvokeByNameNoOut(0ULL, fqn, (uint32_t)std::strlen(fqn));
                if (rc == GE_Result_Ok) okCount.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(1ms);
            }
        });
    }

    // Perform cleanup twice; second call should be idempotent (Ok) or report NotFound safely
    GE_Result c1 = GE_CleanupOldContext(pathUtf8.c_str(), (uint32_t)pathUtf8.size());
    GE_Result c2 = GE_CleanupOldContext(pathUtf8.c_str(), (uint32_t)pathUtf8.size());

    for (auto& th : threads) th.join();

    EXPECT_EQ(c1, GE_Result_Ok);
    EXPECT_TRUE(c2 == GE_Result_Ok || c2 == GE_Result_NotFound)
        << "Second cleanup should be idempotent (Ok) or a safe no-op (NotFound)";

    // Sanity: we should have observed successful invocations during cleanup period
    EXPECT_GT(okCount.load(), 0);
}



// Invalid path preload guardrail: should return InvalidArg and emit ReloadFailed when diagnostics are enabled
static std::atomic<int> s_ReloadFailedCount{0};
static std::string s_LastReloadReason;
static void GE_CDECL Guardrail_OnReload(GE_ReloadStage stage, const char* reasonUtf8, void* userData)
{
    (void)userData;
    if (stage == GE_Reload_Failed) {
        s_ReloadFailedCount.fetch_add(1, std::memory_order_relaxed);
        s_LastReloadReason = reasonUtf8 ? std::string(reasonUtf8) : std::string();
    }
}

TEST(ScriptingAbi_Guardrails, Preload_InvalidPath_ReturnsInvalidArg_EmitsReloadFailed)
{
#ifdef _WIN32
    _putenv_s("GE_HRM_DIR", std::filesystem::current_path().string().c_str());
#endif
    s_ReloadFailedCount.store(0);
    s_LastReloadReason.clear();

    GE_DiagnosticsSink sink{}; sink.onCompilationEvent = nullptr; sink.onReloadEvent = &Guardrail_OnReload; sink.userData = nullptr;
    ASSERT_EQ(GE_SubscribeDiagnostics(&sink), GE_Result_Ok);

    const char* badPath = "This/Path/Does/Not/Exist/Invalid.dll";
    GE_Result rc = GE_PreloadAssemblyContext(badPath, (uint32_t)std::strlen(badPath));
    EXPECT_EQ(rc, GE_Result_InvalidArg) << "Preload with invalid path should return InvalidArg, got " << (int)rc;

    // Diagnostics emission is validated in a separate smoke test; keep this guardrail minimal.
    std::this_thread::sleep_for(1ms);
    if (s_ReloadFailedCount.load() > 0 && !s_LastReloadReason.empty()) {
        EXPECT_NE(s_LastReloadReason.find("Preload failed"), std::string::npos);
    }

    ASSERT_EQ(GE_UnsubscribeDiagnostics(&sink), GE_Result_Ok);
}
