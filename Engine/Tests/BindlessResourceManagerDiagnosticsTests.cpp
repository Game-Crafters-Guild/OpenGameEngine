// The manager's diagnostics are process-scoped: the capacity is reported once per
// process, not once per manager. A test that wants to see that happen has to be the
// process's first manager build, which rules out the umbrella suite — its RenderServices
// fixtures construct a manager through TextureService long before any logging test runs,
// and by then the one report is spent whatever the code does. Hence a binary of its own.

#include <gtest/gtest.h>

#include "Rendering/Core/BindlessResourceManager.h"

#include "Core/EngineLoggerBridge.h"
#include "EngineLogCapture.h"
#include "Logger/LogLevel.h"
#include "Logger/Logger.h"
#include "ScopedStdStreamCapture.h"

#include <cstdint>
#include <string>
#include <vector>

using GameEngine::Rendering::BindlessResourceManager;
using GameEngine::TestLog::CountLinesContaining;
using GameEngine::TestLog::ScopedEngineLogCapture;
using GameEngine::TestLog::ScopedStdStreamCapture;

namespace
{

constexpr uint32_t kTextures = 64;
constexpr uint32_t kBuffers = 64;
constexpr const char* kCapacityLine = "BindlessResourceManager: ready with";
constexpr const char* kSinkLiveLine = "BindlessResourceManagerDiagnosticsTests: capture sink live";

// The manager needs no device to construct, initialize or shut down, so a test can build
// one wherever it likes.
void BuildManager()
{
    BindlessResourceManager manager(nullptr);
    EXPECT_TRUE(manager.Initialize(kTextures, kBuffers));
    manager.Shutdown();
}

// Two API misuses that need no device and no valid resource: a create before Initialize,
// and a destroy of a handle the manager never issued.
void MisuseTheManager()
{
    BindlessResourceManager beforeInitialize(nullptr);
    GameEngine::Rendering::BindlessTextureDesc desc{};
    beforeInitialize.CreateBindlessTexture(desc);

    BindlessResourceManager initialized(nullptr);
    initialized.Initialize(kTextures, kBuffers);
    initialized.DestroyBindlessTexture(GameEngine::Rendering::kInvalidBindlessTexture);
    initialized.Shutdown();
}

} // namespace

// The capacity is one number per process, so one manager reports it at Info and the rest
// report at Debug. Spending that one report is only worth anything if someone can read
// it: a logger still below Info drops the record, and a flag spent on a dropped record
// leaves the capacity carried at no level at all for the rest of the process.
TEST(BindlessResourceManagerDiagnostics, CapacityReportIsNotSpentOnAHostThatCannotCarryItYet)
{
    // Engine is a shared library and Logger a static one: the manager logs through
    // Engine.dll's logger state, so read and write that state rather than this
    // executable's copy.
    Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());
    ASSERT_EQ(Logger::LogLevel::Off, Logger::Log::GetLogLevel())
        << "something configured the logger before this test, so the report may already be spent";

    // A host below Info cannot carry the report, so building here must leave it unspent.
    BuildManager();

    std::vector<std::string> infoLines;
    {
        ScopedEngineLogCapture capture(&infoLines, Logger::LogLevel::Info);
        // A capture that sees nothing is indistinguishable from a line that was never
        // emitted: prove the sink live before reading a count out of it.
        Logger::Log::Info(kSinkLiveLine);
        BuildManager();
        Logger::Log::Flush();
    }
    ASSERT_EQ(1u, CountLinesContaining(infoLines, kSinkLiveLine)) << "the Info capture was not live";
    EXPECT_EQ(1u, CountLinesContaining(infoLines, kCapacityLine))
        << "the capacity report was spent by a build the logger could not carry";

    // And it stays spent: the second Info reader sees none.
    std::vector<std::string> laterInfoLines;
    {
        ScopedEngineLogCapture capture(&laterInfoLines, Logger::LogLevel::Info);
        Logger::Log::Info(kSinkLiveLine);
        BuildManager();
        Logger::Log::Flush();
    }
    ASSERT_EQ(1u, CountLinesContaining(laterInfoLines, kSinkLiveLine)) << "the Info capture was not live";
    EXPECT_EQ(0u, CountLinesContaining(laterInfoLines, kCapacityLine))
        << "the capacity report repeated at Info, once per manager";

    // Every later manager still says it, one level down, so the number is recoverable.
    std::vector<std::string> debugLines;
    {
        ScopedEngineLogCapture capture(&debugLines, Logger::LogLevel::Debug);
        Logger::Log::Debug(kSinkLiveLine);
        BuildManager();
        Logger::Log::Flush();
    }
    ASSERT_EQ(1u, CountLinesContaining(debugLines, kSinkLiveLine)) << "the Debug capture was not live";
    EXPECT_EQ(1u, CountLinesContaining(debugLines, kCapacityLine))
        << "a later manager's capacity line is not recoverable at Debug";
}

// The manager's failure diagnostics are logger records and nothing else. That is the
// module's standard - 322 Logger::Log::Error call sites against zero conditional stderr
// fallbacks - and it means their visibility is the host's answer, not each subsystem's:
// the editor, the player and Application all configure the logger before any device
// exists. A test executable does not, so a misuse there is dropped, and the fix for that
// belongs in the test host rather than in a stderr mirror per call site.
//
// What must not happen is a second channel appearing in one place and not another, which
// is how the same report ends up printed twice in the editor and once in a test.
TEST(BindlessResourceManagerDiagnostics, MisuseGoesThroughTheLoggerAndNowhereElse)
{
    Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());

    // A host that never configured the logger: the records are dropped at the call site,
    // and no stream carries them behind its back.
    Logger::Log::ClearSinks();
    ASSERT_EQ(0u, Logger::Log::GetSinkCount()) << "the logger still has a sink, so this is not that host";
    ASSERT_EQ(Logger::LogLevel::Off, Logger::Log::GetLogLevel());
    std::string streamOutput;
    {
        // Nothing to flush: with the level at Off the records are dropped at the call
        // site and never reach the queue. Anything in the capture got there some other
        // way, which is exactly what this reads for.
        ScopedStdStreamCapture streams;
        MisuseTheManager();
        streamOutput = streams.Text();
    }
    EXPECT_EQ(std::string::npos, streamOutput.find("BindlessResourceManager"))
        << "a diagnostic reached stdout or stderr, where no log level reaches it:\n"
        << streamOutput;

    // Configure the logger and the same misuse is reported, at Error.
    std::vector<std::string> errorLines;
    {
        ScopedEngineLogCapture capture(&errorLines, Logger::LogLevel::Error);
        Logger::Log::Error(kSinkLiveLine);
        MisuseTheManager();
        Logger::Log::Flush();
    }
    ASSERT_EQ(1u, CountLinesContaining(errorLines, kSinkLiveLine)) << "the Error capture was not live";
    EXPECT_EQ(1u, CountLinesContaining(errorLines, "CreateBindlessTexture before Initialize"));
    EXPECT_EQ(1u, CountLinesContaining(errorLines, "DestroyBindlessTexture got unknown handle"));
}
