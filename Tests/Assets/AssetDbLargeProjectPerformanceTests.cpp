#include <gtest/gtest.h>

#include "Core/Application.h"
#include "Core/Engine.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Logger/Logger.h"

#include "TestTempDir.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine;

namespace
{
namespace fs = std::filesystem;

static void WriteTextFile(const fs::path& p, const std::string& content)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);

    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

static void WriteBinaryFile(const fs::path& p, size_t sizeBytes, uint32_t seed)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);

    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(0, 255);

    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    std::vector<uint8_t> buf;
    buf.resize(4096);

    size_t remaining = sizeBytes;
    while (remaining > 0)
    {
        const size_t chunk = std::min(remaining, buf.size());
        for (size_t i = 0; i < chunk; ++i)
        {
            buf[i] = static_cast<uint8_t>(dist(rng));
        }
        out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(chunk));
        remaining -= chunk;
    }
}

static void WritePngStub(const fs::path& p)
{
    // Minimal PNG signature + IHDR chunk stub (not a valid image, but enough to be treated as a binary file).
    // We avoid real image generation to keep the test lightweight and dependency-free.
    static const uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);

    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(kPngSig), 8);
    // Some padding bytes.
    uint8_t pad[32] = {};
    out.write(reinterpret_cast<const char*>(pad), sizeof(pad));
}

static bool FileContainsSubstring(const fs::path& p, const std::string& needle)
{
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open())
        return false;

    std::string line;
    while (std::getline(in, line))
    {
        if (line.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

static size_t CountFilesWithExtensionRecursive(const fs::path& root, const std::string& extLower)
{
    size_t count = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec))
    {
        if (ec)
        {
            ec.clear();
            continue;
        }
        const auto& e = *it;
        if (!e.is_regular_file(ec))
            continue;

        std::string ext = e.path().extension().string();
        for (auto& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == extLower)
            ++count;
    }
    return count;
}

} // namespace

TEST(AssetDatabaseLargeProjectPerf, IgnoresToolTreesAndStaysResponsive)
{
    // Reduce logging overhead for this perf/stress test.
    Logger::Log::SetLogLevel(Logger::LogLevel::Warning);

    const fs::path testRoot = GameEngine::TestUtils::MakeUniqueTempDirectory("assetdb_large_project");
    const GameEngine::TestUtils::ScopedTempDir scoped(testRoot);

    // Per-project ignore: simulate a user accidentally committing/generated a venv/tool tree under Assets.
    WriteTextFile(testRoot / ".assetignore", "Icons/icon_env/**\n");

    // Create a reasonably large project file set.
    // Keep it bounded so CI doesn't take ages, but large enough to catch O(N^2) behaviors.
    size_t expectedIndexedFiles = 0;

    // Binary blobs (fast-path; no dependency extraction).
    for (int i = 0; i < 3000; ++i)
    {
        const fs::path p = testRoot / "Data" / ("blob_" + std::to_string(i) + ".bin");
        WriteBinaryFile(p, 1024, static_cast<uint32_t>(i));
        ++expectedIndexedFiles;
    }

    // PNG stubs
    for (int i = 0; i < 1000; ++i)
    {
        const fs::path p = testRoot / "Textures" / ("tex_" + std::to_string(i) + ".png");
        WritePngStub(p);
        ++expectedIndexedFiles;
    }

    // UI/text assets (limited count; triggers dependency extraction for small files).
    for (int i = 0; i < 400; ++i)
    {
        const fs::path p = testRoot / "UI" / ("panel_" + std::to_string(i) + ".uxml");
        WriteTextFile(p, "<ui><label text=\"Hello\" /></ui>\n");
        ++expectedIndexedFiles;
    }
    for (int i = 0; i < 400; ++i)
    {
        const fs::path p = testRoot / "UI" / ("style_" + std::to_string(i) + ".css");
        WriteTextFile(p, "body { color: #fff; }\n");
        ++expectedIndexedFiles;
    }

    // A few larger binary files to exercise sparse-hash path.
    for (int i = 0; i < 20; ++i)
    {
        const fs::path p = testRoot / "Big" / ("big_" + std::to_string(i) + ".bin");
        WriteBinaryFile(p, 1024 * 1024, static_cast<uint32_t>(100000 + i));
        ++expectedIndexedFiles;
    }

    // Junk tool tree that must be ignored (including files that are NOT covered by default ignore dir names).
    // This is similar to the Mac report where a Python venv landed under Assets.
    for (int i = 0; i < 1500; ++i)
    {
        const fs::path p = testRoot / "Icons" / "icon_env" / "misc" / ("file_" + std::to_string(i) + ".txt");
        WriteTextFile(p, "junk\n");
        // Intentionally NOT counted: must be ignored by .assetignore prefix rule.
    }

    // Initialize engine from a clean run (no pre-existing AssetDatabase.assetdb).
    auto engine = std::make_unique<EngineCore>();
    ApplicationConfig config;
    config.WorkspaceDirectory = testRoot.string();
    config.AssetDirectory = testRoot.string();
    config.AssetDatabaseFile = "AssetDatabase.assetdb";
    config.AssetDatabaseCacheDirectory = ".Cache/AssetDatabase";

    ScriptsConfig scriptsConfig{};
    scriptsConfig.disableClr = true;
    scriptsConfig.enableHotReload = false;
    scriptsConfig.enableAsyncHotReload = false;
    scriptsConfig.enableAutoProjectGeneration = false;
    engine->SetScriptsConfig(scriptsConfig);

    const auto initStart = std::chrono::high_resolution_clock::now();
    ASSERT_TRUE(engine->Initialize(config));
    const auto initEnd = std::chrono::high_resolution_clock::now();
    const double initMs =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(initEnd - initStart).count();

    // Non-blocking requirement: initialization should not wait for full asset scan completion.
    EXPECT_LT(initMs, 2000.0) << "Engine::Initialize took too long; asset scanning should be async";

    AssetManager& am = engine->GetAssetManager();
    AssetRegistry& reg = am.GetRegistry();

    // While scan is running, keep ticking the asset manager (persistence tick) and measure responsiveness.
    std::vector<double> updateMs;
    updateMs.reserve(2000);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto t0 = std::chrono::high_resolution_clock::now();
        am.Update();
        const auto t1 = std::chrono::high_resolution_clock::now();
        updateMs.push_back(std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(t1 - t0).count());

        const size_t count = reg.GetAssetCount();
        if (count >= expectedIndexedFiles)
            break;

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const size_t finalCount = reg.GetAssetCount();
    EXPECT_GE(finalCount, expectedIndexedFiles)
        << "Background scan did not complete in time (finalCount=" << finalCount
        << ", expected=" << expectedIndexedFiles << ")";

    // Hard stall guard: we should not see massive main-thread stalls during background scanning.
    double worstUpdate = 0.0;
    for (double ms : updateMs)
        worstUpdate = std::max(worstUpdate, ms);
    EXPECT_LT(worstUpdate, 500.0) << "AssetManager::Update stalled too long during scanning";

    // Ensure no .meta sidecars were created.
    EXPECT_EQ(CountFilesWithExtensionRecursive(testRoot, ".meta"), 0u);

    // Let debounced persistence flush at least once.
    for (int i = 0; i < 50; ++i)
    {
        am.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const fs::path dbFile = testRoot / "AssetDatabase.assetdb";
    {
        std::error_code ec;
        EXPECT_TRUE(fs::exists(dbFile, ec)) << "Expected AssetDatabase.assetdb to be created";
    }

    // Ensure ignored prefix doesn't leak into the authoritative DB.
    EXPECT_FALSE(FileContainsSubstring(dbFile, "\"path\":\"Icons/icon_env/"))
        << "Ignored tool tree was persisted into the authoritative asset database";

    // Capture a few GUIDs for stability check across restart.
    const fs::path sample1 = testRoot / "Textures" / "tex_0.png";
    const fs::path sample2 = testRoot / "UI" / "panel_0.uxml";
    const GUID guid1 = reg.GetAssetGUID(sample1);
    const GUID guid2 = reg.GetAssetGUID(sample2);
    EXPECT_FALSE(guid1.IsNull());
    EXPECT_FALSE(guid2.IsNull());

    engine->Shutdown();
    engine.reset();

    // Restart with existing DB; GUIDs should be stable (no `.meta` dependency).
    auto engine2 = std::make_unique<EngineCore>();
    engine2->SetScriptsConfig(scriptsConfig);
    ASSERT_TRUE(engine2->Initialize(config));

    AssetRegistry& reg2 = engine2->GetAssetManager().GetRegistry();
    EXPECT_EQ(reg2.GetAssetGUID(sample1), guid1);
    EXPECT_EQ(reg2.GetAssetGUID(sample2), guid2);

    engine2->Shutdown();
}






