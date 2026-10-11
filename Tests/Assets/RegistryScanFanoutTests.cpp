// The registry scan's metadata fan-out: a unit (256 files) that throws reaches
// the scan's future as that exception, through the stage's own promise, never
// as an abandoned promise.

#include <gtest/gtest.h>

#include "Assets/AssetRegistry.h"
#include "Assets/ParserRegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>

using namespace GameEngine;

namespace
{

// The exception the probe parser throws, a type the scan has no handler of
// its own for.
class ScanProbeFailure : public std::runtime_error
{
  public:
    ScanProbeFailure() : std::runtime_error("scan probe failed on throws_here.scanprobe") {}
};

// Classifies .scanprobe files and throws while classifying one of them, as a
// parser sniffing a corrupt file might.
class ThrowingProbeParser final : public AssetParser
{
  public:
    AssetType GetAssetType() const override { return AssetType::Unknown; }
    std::vector<std::string> GetSupportedExtensions() const override { return {".scanprobe"}; }
    bool CanParse(const std::filesystem::path& filePath) const override
    {
        if (filePath.filename() == "throws_here.scanprobe")
            throw ScanProbeFailure();
        return true;
    }
    AssetParseResult Parse(const AssetMetadata&, AssetManager&) override { return AssetParseResult(false); }
    std::string GetName() const override { return "ThrowingProbeParser"; }
};

void WriteFile(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << "probe";
}

} // namespace

// More than 256 files, so the stage forks; the throwing file sits in a unit
// past the first, so a helper as well as the stage's own thread may take it.
TEST(RegistryScan, AChunkThatThrowsResolvesTheStagePromise)
{
    namespace fs = std::filesystem;
    const TestUtils::ScopedTempDir root(TestUtils::MakeUniqueTempDirectory("ge_registry_scan_throw"));
    constexpr int kFiles = 700;
    for (int i = 0; i < kFiles; ++i)
        WriteFile(root.Path() / ("probe_" + std::to_string(i) + ".scanprobe"));
    WriteFile(root.Path() / "throws_here.scanprobe");

    ParserRegistry parsers;
    ASSERT_TRUE(parsers.RegisterParser(std::make_shared<ThrowingProbeParser>()));

    JobSystem::WorkStealingThreadPool pool(4);
    AssetRegistry registry;
    ASSERT_TRUE(registry.Initialize(&pool));
    registry.SetParserRegistry(&parsers);

    std::future<size_t> scan = registry.ScanDirectoryAsync(root.Path(), true);
    ASSERT_EQ(scan.wait_for(std::chrono::seconds(60)), std::future_status::ready);
    try
    {
        (void)scan.get();
        ADD_FAILURE() << "the scan completed although a file's metadata extraction threw";
    }
    catch (const ScanProbeFailure& e)
    {
        EXPECT_STREQ(e.what(), "scan probe failed on throws_here.scanprobe");
    }
    catch (const std::exception& e)
    {
        ADD_FAILURE() << "the scan ended with \"" << e.what() << "\" in place of the unit's exception";
    }

    registry.SetParserRegistry(nullptr);
    registry.Shutdown();
}
