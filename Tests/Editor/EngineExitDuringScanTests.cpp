#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "Core/Engine.h"
#include "TestTempDir.h"

// A process that exits with the engine initialized ends while the startup scan
// still runs on job workers, and static destruction takes the tables the scan
// reads (the asset extension map) out from under them: the worker faults inside
// GetCompoundExtensionFromPath. This executable links EngineShutdownEnvironment,
// which shuts the engine down before main returns; without it the process
// exits with SIGSEGV. Only the exit code carries the result, so the test body
// leaves the engine running on purpose.
//
// Verified on macOS. The scan must outlast the tail of EngineCore::Initialize,
// which takes a directory of tens of thousands of files.
namespace
{

namespace fs = std::filesystem;

constexpr int kDirectoryCount = 300;
constexpr int kFileCount = 30000;
constexpr useconds_t kExitWindowMicroseconds = 300000;

void PopulateAssetDirectory(const fs::path& root)
{
    for (int i = 0; i < kFileCount; ++i)
    {
        const fs::path directory = root / ("d" + std::to_string(i % kDirectoryCount));
        if (i < kDirectoryCount)
            fs::create_directories(directory);
        std::ofstream(directory / ("f" + std::to_string(i) + ".txt")) << 'x';
    }
}

// Constructed before any engine static, so destroyed after all of them: it
// holds the process open between static destruction and termination, which
// makes a worker's read of a destroyed table fault every time instead of once
// in many runs. It also removes the directory once no worker can still scan it.
class ProcessExitGuard
{
  public:
    ~ProcessExitGuard()
    {
        usleep(kExitWindowMicroseconds);
        std::error_code ec;
        fs::remove_all(m_Root, ec);
    }

    void SetRoot(fs::path root) { m_Root = std::move(root); }

  private:
    fs::path m_Root;
};

ProcessExitGuard g_ProcessExitGuard;

} // namespace

TEST(EngineExitDuringScan, TheProcessLeavesWhileTheStartupScanRuns)
{
    const fs::path root = GameEngine::TestUtils::MakeUniqueTempDirectory("GameEngine_EngineExitDuringScan");
    fs::create_directories(root);
    g_ProcessExitGuard.SetRoot(root);
    PopulateAssetDirectory(root);

    GameEngine::ApplicationConfig config{};
    config.AssetDirectory = root;
    config.WorkspaceDirectory = root;
    config.EnableEditor = true;
    ASSERT_TRUE(GameEngine::EngineCore::GetInstance().Initialize(config));
}
