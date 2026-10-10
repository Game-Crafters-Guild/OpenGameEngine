#include "gtest/gtest.h"
#include "Scripting/ScriptingABI.h"
#include <atomic>
#include <string>
#include <vector>

#include <filesystem>
#include <chrono>
#include <thread>

#include <fstream>
static std::vector<uint8_t> ReadAll(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

static std::atomic<int> g_compStarted{0};
static std::atomic<int> g_compCompleted{0};
static std::atomic<int> g_reloadFailed{0};
static std::atomic<int> g_reloadCompleted{0};
static std::string g_lastReason;
static std::vector<GE_Diagnostic> g_lastDiags;

static void GE_CDECL Test_OnCompilationEvent(GE_CompilationStage stage,
                                             float progress01,
                                             const GE_Diagnostic* diags,
                                             uint32_t diagCount,
                                             void* userData)
{
    (void)userData;
    if (stage == GE_Comp_Started) g_compStarted.fetch_add(1);
    if (stage == GE_Comp_Completed) {
        g_compCompleted.fetch_add(1);
        g_lastDiags.assign(diags, diags + diagCount);
    }
}

static void GE_CDECL Test_OnReloadEvent(GE_ReloadStage stage,
                                        const char* reasonUtf8,
                                        void* userData)
{
    (void)userData;
    if (stage == GE_Reload_Failed) {
        g_reloadFailed.fetch_add(1);
        g_lastReason = reasonUtf8 ? std::string(reasonUtf8) : std::string();
    }

    if (stage == GE_Reload_Completed) {
        g_reloadCompleted.fetch_add(1);
        g_lastReason = reasonUtf8 ? std::string(reasonUtf8) : std::string();
    }
}

TEST(ScriptingAbi_Diagnostics, SubscribeAndNotifyCompilationEvent)
{
    g_compStarted.store(0); g_compCompleted.store(0); g_lastDiags.clear();

    GE_DiagnosticsSink sink{}; sink.onCompilationEvent = &Test_OnCompilationEvent; sink.onReloadEvent = &Test_OnReloadEvent; sink.userData = nullptr;
    ASSERT_EQ(GE_SubscribeDiagnostics(&sink), GE_Result_Ok);

    // Build one error diagnostic
    const char* msg = "CS1001: Test error";
    const char* file = "Scripts/Foo.cs";
    GE_Diagnostic d{}; d.severity = GE_Diag_Error; d.code = 1001; d.message = { msg, (uint32_t)strlen(msg) }; d.file = { file, (uint32_t)strlen(file) }; d.line = 42; d.column = 7;

    ASSERT_EQ(GE_NotifyCompilationEvent(GE_Comp_Started, 0.0f, nullptr, 0), GE_Result_Ok);
    ASSERT_EQ(GE_NotifyCompilationEvent(GE_Comp_Completed, 1.0f, &d, 1), GE_Result_Ok);

    EXPECT_GE(g_compStarted.load(), 1);
    EXPECT_GE(g_compCompleted.load(), 1);
    ASSERT_EQ(g_lastDiags.size(), 1u);
    EXPECT_EQ(g_lastDiags[0].severity, GE_Diag_Error);
    EXPECT_EQ(g_lastDiags[0].code, 1001);
    EXPECT_EQ(std::string(g_lastDiags[0].message.data, g_lastDiags[0].message.length), std::string(msg));
    EXPECT_EQ(std::string(g_lastDiags[0].file.data, g_lastDiags[0].file.length), std::string(file));
    EXPECT_EQ(g_lastDiags[0].line, 42);
    EXPECT_EQ(g_lastDiags[0].column, 7);

    ASSERT_EQ(GE_UnsubscribeDiagnostics(&sink), GE_Result_Ok);
}

TEST(ScriptingAbi_Diagnostics, SubscribeAndNotifyReloadFailed)
{
    g_reloadFailed.store(0); g_lastReason.clear();

    GE_DiagnosticsSink sink{}; sink.onCompilationEvent = &Test_OnCompilationEvent; sink.onReloadEvent = &Test_OnReloadEvent; sink.userData = nullptr;
    ASSERT_EQ(GE_SubscribeDiagnostics(&sink), GE_Result_Ok);

    const char* reason = "unit-test failure";
    ASSERT_EQ(GE_NotifyReloadEvent(GE_Reload_Failed, reason), GE_Result_Ok);

    EXPECT_GE(g_reloadFailed.load(), 1);
    EXPECT_EQ(g_lastReason, std::string(reason));


    ASSERT_EQ(GE_UnsubscribeDiagnostics(&sink), GE_Result_Ok);
}

TEST(ScriptingAbi_Diagnostics, Smoke_PreloadInvalidPath_EmitsReloadFailed)
{
#ifdef _WIN32
    _putenv_s("GE_HRM_DIR", std::filesystem::current_path().string().c_str());
#endif
    g_reloadFailed.store(0);
    g_lastReason.clear();

    GE_DiagnosticsSink sink{}; sink.onCompilationEvent = &Test_OnCompilationEvent; sink.onReloadEvent = &Test_OnReloadEvent; sink.userData = nullptr;
    ASSERT_EQ(GE_SubscribeDiagnostics(&sink), GE_Result_Ok);

    const char* badPath = "This/Path/Does/Not/Exist/Invalid.dll";
    GE_Result rc = GE_PreloadAssemblyContext(badPath, (uint32_t)strlen(badPath));
    EXPECT_EQ(rc, GE_Result_InvalidArg) << "Expected InvalidArg for invalid preload path";
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_GE(g_reloadFailed.load(), 1) << "Expected GE_Reload_Failed event for invalid path preload";
    if (!g_lastReason.empty()) {
        EXPECT_NE(g_lastReason.find("Preload failed"), std::string::npos);
    }

    ASSERT_EQ(GE_UnsubscribeDiagnostics(&sink), GE_Result_Ok);
}

TEST(ScriptingAbi_Diagnostics, Smoke_UnloadEvent_EmitsReloadCompleted)
{
    g_reloadCompleted.store(0); g_lastReason.clear();

    GE_DiagnosticsSink sink{}; sink.onCompilationEvent = &Test_OnCompilationEvent; sink.onReloadEvent = &Test_OnReloadEvent; sink.userData = nullptr;
    ASSERT_EQ(GE_SubscribeDiagnostics(&sink), GE_Result_Ok);

    const char* reason = "Unload completed (test)";
    ASSERT_EQ(GE_NotifyReloadEvent(GE_Reload_Completed, reason), GE_Result_Ok);

    EXPECT_GE(g_reloadCompleted.load(), 1);
    EXPECT_NE(g_lastReason.find("Unload"), std::string::npos);

    ASSERT_EQ(GE_UnsubscribeDiagnostics(&sink), GE_Result_Ok);
}



