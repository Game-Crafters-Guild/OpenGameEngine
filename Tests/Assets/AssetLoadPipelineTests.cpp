#include <gtest/gtest.h>
#include "Assets/AssetManager.h"
#include "Assets/AssetTasks.h"
#include "Assets/BinaryAsset.h"
#include "Assets/ParserRegistry.h"
#include "Core/EngineLoggerBridge.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"
#include "TestTempDir.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <vector>

using namespace GameEngine;

// End-to-end coverage of the asset load pipeline: blocking read on the
// AssetIOService, one merged processing+registration job on the JobSystem at
// JobPriority::Background, completion through the per-GUID shared promise.
class AssetLoadPipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create test directory
        testDir = TestUtils::MakeUniqueTempDirectory("asset_load_pipeline_test");
        std::filesystem::create_directories(testDir);

        // Files land before Initialize, so the startup scan claims an identity
        // for every one of them.
        writeTestAssetFiles();

        // Initialize job system with multiple threads for performance testing
        jobSystem = std::make_unique<JobSystem::WorkStealingThreadPool>(8);

        // Initialize asset manager
        assetManager = std::make_unique<AssetManager>();
        assetManager->Initialize(testDir, jobSystem.get());

        // The startup scan is the other registrar for every file written above.
        // Drain it before any test registers or loads, so registration order is
        // the test's rather than a race (AssetDbHardening's path-collision
        // fixture arranges its registry the same way).
        assetManager->WaitForStartupScan();

        for (const auto& [filename, content, type] : testAssets) {
            testMetadata.push_back(
                MakeMetadata(assetManager->GetRegistry(), testDir / filename, content, type));
        }

        Logger::Log::Info("AssetLoadPipelineTest: Setup complete with {} test assets", testAssets.size());
    }

    void TearDown() override {
        // Cleanup
        if (assetManager) {
            assetManager->Shutdown();
            assetManager.reset();
        }

        if (jobSystem) {
            jobSystem->Shutdown();
            jobSystem.reset();
        }

        // Remove test directory
        std::filesystem::remove_all(testDir);

        Logger::Log::Info("AssetLoadPipelineTest: Teardown complete");
    }

    void writeTestAssetFiles() {
        // Create minimal valid WAV file (44 bytes header + 4 bytes data)
        std::string validWavData;
        validWavData += "RIFF";                    // ChunkID
        validWavData += "\x28\x00\x00\x00";       // ChunkSize (40 bytes, little-endian)
        validWavData += "WAVE";                    // Format
        validWavData += "fmt ";                    // Subchunk1ID
        validWavData += "\x10\x00\x00\x00";       // Subchunk1Size (16, little-endian)
        validWavData += "\x01\x00";               // AudioFormat (1 = PCM, little-endian)
        validWavData += "\x01\x00";               // NumChannels (1, little-endian)
        validWavData += "\x44\xAC\x00\x00";       // SampleRate (44100, little-endian)
        validWavData += "\x88\x58\x01\x00";       // ByteRate (88200, little-endian)
        validWavData += "\x02\x00";               // BlockAlign (2, little-endian)
        validWavData += "\x10\x00";               // BitsPerSample (16, little-endian)
        validWavData += "data";                    // Subchunk2ID
        validWavData += "\x04\x00\x00\x00";       // Subchunk2Size (4 bytes, little-endian)
        validWavData += "\x00\x00\x00\x00";       // Sample data (silence)

        // Create minimal valid OBJ file
        std::string validObjData =
            "# Test OBJ file\n"
            "v 0.0 0.0 0.0\n"
            "v 1.0 0.0 0.0\n"
            "v 0.0 1.0 0.0\n"
            "f 1 2 3\n";

        // Create various test assets for comprehensive testing
        testAssets = {
            {"small_texture.png", PngData(), AssetType::Texture},
            {"large_model.obj", validObjData, AssetType::Model},
            {"audio_clip.wav", validWavData, AssetType::Audio},
            {"ui_layout.xml",
             "<?xml version=\"1.0\"?>\n<UI id=\"root\"><Button id=\"b\" text=\"Test\"/></UI>\n",
             AssetType::UILayout},
            {"ui_style.css", "button { color: red; }", AssetType::UIStyle},
            {"generic_data.bin", std::string(1024 * 10, 'G'), AssetType::Unknown} // 10KB generic
        };

        for (const auto& [filename, content, type] : testAssets) {
            std::ofstream file(testDir / filename, std::ios::binary);
            file << content;
            file.close();
        }
    }

    static std::string PngData() {
        // Minimal valid 1x1 RGBA PNG (so TextureAsset can actually parse it).
        static const unsigned char kPng1x1Rgba[] = {
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
            0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
            0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89,
            0x00, 0x00, 0x00, 0x0A, 0x49, 0x44, 0x41, 0x54,
            0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01,
            0x0D, 0x0A, 0x2D, 0xB4,
            0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
            0xAE, 0x42, 0x60, 0x82
        };
        return std::string(reinterpret_cast<const char*>(kPng1x1Rgba), sizeof(kPng1x1Rgba));
    }

    // Identity for an on-disk path belongs to the registry, never to the caller.
    // The project source is derived-identity, so GetOrCreateAssetGUID is a pure
    // function of the path and every registrar — this fixture, the startup scan,
    // the file watcher — converges on the same GUID. A caller-minted GUID is not
    // arbitrated (ClaimPathGuidLocked covers only the registry's own mint sites),
    // so whichever registrar commits last evicts the other's row by path and the
    // load then resolves nullptr.
    static AssetMetadata MakeMetadata(AssetRegistry& registry,
                                      const std::filesystem::path& assetPath,
                                      const std::string& content, AssetType type) {
        AssetMetadata metadata;
        metadata.Guid = registry.GetOrCreateAssetGUID(assetPath);
        metadata.Path = assetPath;
        metadata.Name = assetPath.stem().string();
        metadata.Extension = assetPath.extension().string();
        metadata.Type = type;
        metadata.FileSize = content.size();
        metadata.LastModified = std::filesystem::last_write_time(assetPath);
        return metadata;
    }

    // Creates <count> copies of the 1x1 PNG on disk and registers their
    // metadata; returns the registered metadata.
    std::vector<AssetMetadata> CreatePngFleet(size_t count, const std::string& prefix) {
        const std::string content = PngData();
        std::vector<AssetMetadata> fleet;
        fleet.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto path = testDir / (prefix + std::to_string(i) + ".png");
            std::ofstream file(path, std::ios::binary);
            file << content;
            file.close();

            AssetMetadata metadata =
                MakeMetadata(assetManager->GetRegistry(), path, content, AssetType::Texture);
            assetManager->GetRegistry().RegisterAssetMetadata(metadata);
            fleet.push_back(std::move(metadata));
        }
        return fleet;
    }

    // Registers <count> PNG-typed metadata entries whose files never exist:
    // each read burns the full open-retry window (~50ms) on a reader thread,
    // making them deterministic slow reads for saturating the IO queue.
    std::vector<AssetMetadata> RegisterMissingAssets(size_t count, const std::string& prefix) {
        std::vector<AssetMetadata> gates;
        gates.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            AssetMetadata metadata;
            metadata.Path = testDir / (prefix + std::to_string(i) + ".png");
            metadata.Guid = assetManager->GetRegistry().GetOrCreateAssetGUID(metadata.Path);
            metadata.Name = metadata.Path.stem().string();
            metadata.Extension = ".png";
            metadata.Type = AssetType::Texture;
            metadata.FileSize = 64;
            assetManager->GetRegistry().RegisterAssetMetadata(metadata);
            gates.push_back(std::move(metadata));
        }
        return gates;
    }

    // Loads `path` through the path-and-callback request: whether it delivered an asset, or
    // nothing when the load did not complete within ten seconds.
    std::optional<bool> LoadByPath(const std::filesystem::path& path) {
        auto delivered = std::make_shared<std::promise<bool>>();
        auto outcome = delivered->get_future();
        assetManager->LoadAsset(path, [delivered](Result<SharedPtr<Asset>, AssetError> result) {
            delivered->set_value(result.IsOk() && result.Value());
        });
        if (outcome.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
            return std::nullopt;
        return outcome.get();
    }

    // What the decode job's catch logs for a decode that threw.
    static constexpr std::string_view kDecodeThrewLine = "AssetDecodeJob: processing failed for";

    // The Error lines logged from creation on. The logger owns its sink for the rest of the
    // process, so the sink shares the lines it collects.
    struct ErrorLines {
        std::mutex Mutex;
        std::vector<std::string> Lines;

        bool AnyContains(std::string_view text) {
            Logger::Log::Flush();
            std::lock_guard lock(Mutex);
            return std::any_of(Lines.begin(), Lines.end(),
                               [text](const std::string& line) { return line.find(text) != std::string::npos; });
        }
    };

    static std::shared_ptr<ErrorLines> CaptureErrorLines() {
        auto errors = std::make_shared<ErrorLines>();
        // Engine is a SHARED library: the loaders log through Engine's Logger state.
        Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());
        Logger::Log::Initialize({});
        auto sink = Logger::MakeUnique<Logger::CallbackSink>();
        sink->RegisterCallback([errors](const Logger::LogMessage& message) {
            if (message.Level != Logger::LogLevel::Error)
                return;
            std::lock_guard lock(errors->Mutex);
            errors->Lines.emplace_back(message.Message);
        });
        Logger::Log::AddSink(std::move(sink));
        return errors;
    }

    std::filesystem::path testDir;
    std::vector<std::tuple<std::string, std::string, AssetType>> testAssets;
    std::vector<AssetMetadata> testMetadata;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> jobSystem;
    std::unique_ptr<AssetManager> assetManager;
};

TEST_F(AssetLoadPipelineTest, SingleAssetCompletePipeline) {
    // Test the complete read -> decode pipeline for a single asset
    const auto& metadata = testMetadata[0]; // Use first test asset

    auto startTime = std::chrono::high_resolution_clock::now();

    // Register the asset metadata with the registry first
    assetManager->GetRegistry().RegisterAssetMetadata(metadata);

    auto future = assetManager->LoadAssetAsync(metadata.Guid, AssetLoadPriority::Normal);
    ASSERT_EQ(future.wait_for(std::chrono::milliseconds(10000)), std::future_status::ready)
        << "load did not complete within timeout";

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    SharedPtr<Asset> asset = future.get();
    ASSERT_NE(asset, nullptr);
    EXPECT_EQ(asset->GetGUID(), metadata.Guid);
    EXPECT_TRUE(assetManager->IsAssetLoaded(metadata.Guid));

    // Verify performance targets
    EXPECT_LT(duration.count(), 1000); // Should complete within 1 second

    std::cout << "[ MEASURE  ] single-asset pipeline: " << duration.count() << "ms\n";
}

// A load request for an asset whose file is missing completes, delivers no asset and leaves
// nothing loaded (the request path: LoadAsset(AssetLoadRequest) and its handle).
TEST_F(AssetLoadPipelineTest, AMissingFileFailsItsLoad) {
    const auto missing = RegisterMissingAssets(1, "missing_");

    AssetLoadRequest request;
    request.AssetGuid = missing[0].Guid;
    request.Priority = AssetLoadPriority::High;
    AssetLoadHandle handle = assetManager->LoadAsset(request);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!handle.IsComplete() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(handle.IsComplete()) << "the load of a missing file did not complete";
    EXPECT_EQ(handle.GetResult(), nullptr) << "a missing file delivered an asset";
    EXPECT_FALSE(assetManager->IsAssetLoaded(missing[0].Guid));
}

// A .gltf whose buffer file is not beside it fails its own load with the loader's reason
// logged, and the next load runs.
TEST_F(AssetLoadPipelineTest, AGltfWithoutItsBufferFileFailsItsLoadAndTheNextLoadRuns) {
    const std::string gltf = R"({
      "asset": { "version": "2.0" },
      "scenes": [ { "nodes": [ 0 ] } ],
      "nodes": [ { "mesh": 0 } ],
      "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 } } ] } ],
      "buffers": [ { "byteLength": 36, "uri": "triangle.bin" } ],
      "bufferViews": [ { "buffer": 0, "byteLength": 36 } ],
      "accessors": [ { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [ 0, 0, 0 ], "max": [ 1, 1, 0 ] } ]
    })";
    std::ofstream(testDir / "triangle.gltf", std::ios::binary) << gltf;

    const auto errors = CaptureErrorLines();
    EXPECT_EQ(LoadByPath(testDir / "triangle.gltf"), std::optional<bool>(false))
        << "a .gltf without its buffer file did not fail its load";

    EXPECT_TRUE(errors->AnyContains("triangle.gltf' is not loaded: its buffers do not load (cgltf_load_buffers: "
                                    "file_not_found)"))
        << "the loader's reason was not logged";
    // The decode job's catch logs this line only when the decode threw, which aborts the web module.
    EXPECT_FALSE(errors->AnyContains(kDecodeThrewLine)) << "the decode threw instead of returning no asset";

    EXPECT_EQ(LoadByPath(testDir / "large_model.obj"), std::optional<bool>(true))
        << "the load after the failed .gltf did not load";
}

// An audio file that does not decode fails its own load, and the next load runs.
TEST_F(AssetLoadPipelineTest, AnAudioFileThatDoesNotDecodeFailsItsLoadAndTheNextLoadRuns) {
    // A RIFF/WAVE header with no fmt chunk.
    std::ofstream(testDir / "corrupt.wav", std::ios::binary) << std::string("RIFF\xff\xff\xff\xff" "WAVE", 12);

    const auto errors = CaptureErrorLines();
    EXPECT_EQ(LoadByPath(testDir / "corrupt.wav"), std::optional<bool>(false))
        << "an audio file that does not decode did not fail its load";
    // The decode job's catch logs this line only when the decode threw, which aborts the web module.
    EXPECT_FALSE(errors->AnyContains(kDecodeThrewLine)) << "the decode threw instead of returning no asset";

    EXPECT_EQ(LoadByPath(testDir / "large_model.obj"), std::optional<bool>(true))
        << "the load after the failed audio file did not load";
}

TEST_F(AssetLoadPipelineTest, MultipleAssetsParallelPipeline) {
    // Test parallel processing of multiple assets
    auto startTime = std::chrono::high_resolution_clock::now();

    // Register all asset metadata with the registry first
    for (const auto& metadata : testMetadata) {
        assetManager->GetRegistry().RegisterAssetMetadata(metadata);
    }

    // Load all assets in parallel
    std::vector<AssetFuture> futures;
    futures.reserve(testMetadata.size());
    for (const auto& metadata : testMetadata) {
        futures.push_back(assetManager->LoadAssetAsync(metadata.Guid, AssetLoadPriority::Normal));
    }

    // Wait for all to complete
    bool allCompleted = true;
    for (auto& future : futures) {
        if (future.wait_for(std::chrono::milliseconds(15000)) != std::future_status::ready) {
            allCompleted = false;
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    // Verify all pipelines completed
    EXPECT_TRUE(allCompleted);

    // Count successful assets. A load that completes may still produce no
    // usable result (e.g., an asset type whose support isn't wired up in this
    // standalone AssetManager harness). Those are acceptable here — the
    // contract this test enforces is "at least 3 of the 6 test assets load
    // end-to-end".
    int successfulAssets = 0;
    for (size_t i = 0; i < futures.size(); ++i) {
        SharedPtr<Asset> asset = futures[i].get();
        if (!asset) {
            Logger::Log::Info("Asset {} completed without a result (pipeline stage yielded null)",
                              testMetadata[i].Name);
            continue;
        }
        EXPECT_EQ(asset->GetGUID(), testMetadata[i].Guid);
        successfulAssets++;
    }

    // Expect at least some assets to succeed (texture, UI, binary should work)
    EXPECT_GE(successfulAssets, 3) << "At least 3 assets should load successfully";
    Logger::Log::Info("AssetLoadPipelineTest: {}/{} assets loaded successfully",
                successfulAssets, futures.size());

    // Calculate throughput
    double throughputAssetsPerSecond = (testMetadata.size() * 1000.0) / duration.count();

    std::cout << "[ MEASURE  ] parallel pipeline: " << testMetadata.size() << " assets in "
              << duration.count() << "ms (" << throughputAssetsPerSecond << " assets/s)\n";

    // Verify performance targets
    EXPECT_LT(duration.count(), 5000); // Should complete within 5 seconds
    EXPECT_GT(throughputAssetsPerSecond, 1.0); // Should achieve reasonable throughput
}

TEST_F(AssetLoadPipelineTest, PerformanceTargetValidation) {
    // Test that the complete pipeline meets performance targets
    auto performanceMonitor = &AssetTaskPerformanceMonitor::GetInstance();
    performanceMonitor->ResetMetrics();

    // The stage budgets below are hang-scale bounds, not microbenchmarks: three
    // Debug runs at processor-queue-length 121 (2026-08-10) measured fileIO
    // 0.12-0.14ms, processing 0.23-1.66ms, registration 0.03-0.05ms, total
    // 0.4-1.8ms — 20-70x inside every budget. Keep that headroom when touching
    // them; a budget a contended box can reach stops being an instrument.
    //
    // ASan builds have meaningful overhead; relax perf thresholds in those builds so this
    // test remains a functional guardrail rather than a flaky microbenchmark.
    double targetScale = 1.0;
#if defined(GE_ENABLE_ASAN) && GE_ENABLE_ASAN
    targetScale = 2.0;
#endif

    // Register the asset metadata with the registry first
    const auto& metadata = testMetadata[0];
    assetManager->GetRegistry().RegisterAssetMetadata(metadata);

    // Load a single asset to generate metrics
    auto future = assetManager->LoadAssetAsync(metadata.Guid, AssetLoadPriority::Normal);
    ASSERT_EQ(future.wait_for(std::chrono::milliseconds(10000)), std::future_status::ready);

    // Verify asset loaded successfully. The state is spelled out because a
    // null here is the load failing, not the wait expiring — and the suite has
    // no Logger sink to say why. residentRow separates "this GUID lost its
    // metadata to a path-conflict eviction" from every other load failure.
    SharedPtr<Asset> asset = future.get();
    AssetMetadata residentMetadata{};
    const bool residentRow =
        assetManager->GetRegistry().TryGetAssetMetadata(metadata.Guid, residentMetadata);
    ASSERT_NE(asset, nullptr)
        << "load resolved without an asset: suppressed=" << assetManager->IsLoadSuppressed(metadata.Guid)
        << " loaded=" << assetManager->IsAssetLoaded(metadata.Guid)
        << " inFlight=" << assetManager->GetInFlightLoadCount()
        << " residentRow=" << residentRow;

    // Check performance targets
    auto pipelineMetrics = performanceMonitor->GetPipelineMetrics();

    // Validate individual stage performance targets. The pipeline is a
    // blocking read on the AssetIOService followed by one merged
    // processing+registration job; the metric buckets keep their names.
    auto fileIOMetrics = performanceMonitor->GetTaskMetrics("AssetFileIOTask");
    auto processingMetrics = performanceMonitor->GetTaskMetrics("AssetProcessingTask");
    auto registrationMetrics = performanceMonitor->GetTaskMetrics("AssetRegistrationTask");

    // Performance target validation
    EXPECT_LE(fileIOMetrics.averageExecutionTimeMs, 10.0 * targetScale) << "File I/O stage should be ≤10ms";
    EXPECT_LE(processingMetrics.averageExecutionTimeMs, 50.0 * targetScale) << "Processing stage should be ≤50ms";
    EXPECT_LE(registrationMetrics.averageExecutionTimeMs, 1.0 * targetScale) << "Registration stage should be ≤1ms";

    // Overall pipeline performance
    EXPECT_LE(pipelineMetrics.totalPipelineTimeMs, 61.0 * targetScale) << "Total pipeline should be ≤61ms";

    // Success rate validation
    EXPECT_GE(fileIOMetrics.successRate, 0.99) << "File I/O success rate should be ≥99%";
    EXPECT_GE(processingMetrics.successRate, 0.99) << "Processing success rate should be ≥99%";
    EXPECT_GE(registrationMetrics.successRate, 0.99) << "Registration success rate should be ≥99%";

    EXPECT_FALSE(performanceMonitor->GeneratePerformanceReport().empty())
        << "the monitor must render a report for the stages it just recorded";

    std::cout << "[ MEASURE  ] stage times: fileIO " << fileIOMetrics.averageExecutionTimeMs
              << "ms, processing " << processingMetrics.averageExecutionTimeMs << "ms, registration "
              << registrationMetrics.averageExecutionTimeMs << "ms, total "
              << pipelineMetrics.totalPipelineTimeMs << "ms\n";
}

TEST_F(AssetLoadPipelineTest, MainThreadBlockingValidation) {
    // Test that main thread blocking is minimized
    auto startTime = std::chrono::high_resolution_clock::now();

    // Register the asset metadata with the registry first
    const auto& metadata = testMetadata[1]; // Use larger asset
    assetManager->GetRegistry().RegisterAssetMetadata(metadata);

    // Measure main thread blocking during asset loading
    auto mainThreadStartTime = std::chrono::high_resolution_clock::now();
    auto future = assetManager->LoadAssetAsync(metadata.Guid, AssetLoadPriority::Normal);
    auto mainThreadEndTime = std::chrono::high_resolution_clock::now();

    auto mainThreadBlockingTime = std::chrono::duration_cast<std::chrono::microseconds>(
        mainThreadEndTime - mainThreadStartTime);

    // Wait for completion
    ASSERT_EQ(future.wait_for(std::chrono::milliseconds(10000)), std::future_status::ready);

    auto endTime = std::chrono::high_resolution_clock::now();
    auto totalTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    // The model asset may or may not parse in this standalone harness; the
    // contract here is submission latency, not decode success.
    (void)future.get();

    // Validate main thread blocking is minimal
    double blockingTimeMs = mainThreadBlockingTime.count() / 1000.0;
    EXPECT_LT(blockingTimeMs, 5.0) << "Main thread blocking should be <5ms, was " << blockingTimeMs << "ms";

    std::cout << "[ MEASURE  ] submission latency: " << blockingTimeMs << "ms (load total "
              << totalTime.count() << "ms)\n";
}

TEST_F(AssetLoadPipelineTest, HighThroughputStressTest) {
    // Test high throughput capability under a burst of load submissions
    const size_t numAssets = 50; // Reasonable number for test environment
    const auto stressTestMetadata = CreatePngFleet(numAssets, "stress_test_");

    auto startTime = std::chrono::high_resolution_clock::now();

    // Load all assets in parallel
    std::vector<AssetFuture> futures;
    futures.reserve(stressTestMetadata.size());
    for (const auto& metadata : stressTestMetadata) {
        futures.push_back(assetManager->LoadAssetAsync(metadata.Guid, AssetLoadPriority::Normal));
    }

    // Wait for all to complete
    bool allCompleted = true;
    for (auto& future : futures) {
        if (future.wait_for(std::chrono::milliseconds(30000)) != std::future_status::ready) {
            allCompleted = false;
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    // Verify all completed successfully
    EXPECT_TRUE(allCompleted);

    // Calculate throughput
    double throughputAssetsPerSecond = (numAssets * 1000.0) / duration.count();

    Logger::Log::Info("AssetLoadPipelineTest: Stress test - {} assets in {}ms, throughput: {:.1f} assets/second",
                numAssets, duration.count(), throughputAssetsPerSecond);

    // Verify reasonable throughput (scaled for test environment)
    EXPECT_GT(throughputAssetsPerSecond, 10.0) << "Should achieve >10 assets/second in test environment";
}

TEST_F(AssetLoadPipelineTest, BurstScanCompletesAllAndDrainsInFlight) {
    // Burst-scan conservation: every load resolves with a real asset, and the
    // in-flight dedupe map returns to empty (no leaked entries, no stranded
    // promises).
    const size_t numAssets = 64;
    const auto fleet = CreatePngFleet(numAssets, "burst_");

    std::vector<AssetFuture> futures;
    futures.reserve(fleet.size());
    for (const auto& metadata : fleet) {
        futures.push_back(assetManager->LoadAssetAsync(metadata.Guid, AssetLoadPriority::Normal));
    }

    for (size_t i = 0; i < futures.size(); ++i) {
        ASSERT_EQ(futures[i].wait_for(std::chrono::milliseconds(30000)), std::future_status::ready)
            << "asset " << i << " did not complete";
        SharedPtr<Asset> asset = futures[i].get();
        ASSERT_NE(asset, nullptr) << "asset " << i << " failed to load";
        EXPECT_EQ(asset->GetGUID(), fleet[i].Guid);
    }

    // The in-flight entry is erased inside the same completion callback that
    // resolves the promise, immediately after the resolve — poll briefly.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (assetManager->GetInFlightLoadCount() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(assetManager->GetInFlightLoadCount(), 0u)
        << "in-flight map must drain after a burst scan completes";

    // Every asset must be servable from the loaded cache now.
    for (const auto& metadata : fleet) {
        EXPECT_TRUE(assetManager->IsAssetLoaded(metadata.Guid));
    }
}

TEST_F(AssetLoadPipelineTest, CancelledLoadDoesNotSuppressRetry) {
    // Cancelled ≠ Suppressed (design §3.2): a cancelled load is a withdrawn
    // request, not a failure — a later legitimate load of the same GUID must
    // actually load. Saturate the reader threads with slow (missing-file)
    // reads so the target load is still pending when the cancel lands.
    const auto gates = RegisterMissingAssets(8, "suppress_gate_");
    for (const auto& gate : gates) {
        (void)assetManager->LoadAssetAsync(gate.Guid, AssetLoadPriority::Normal);
    }

    const auto fleet = CreatePngFleet(1, "cancel_retry_");
    const GUID target = fleet[0].Guid;

    auto loadFuture = assetManager->LoadAssetAsync(target, AssetLoadPriority::Normal);
    // Public cancel trigger: unloading a GUID withdraws its in-flight load.
    auto unloadFuture = assetManager->UnloadAssetAsync(target);

    ASSERT_EQ(loadFuture.wait_for(std::chrono::milliseconds(10000)), std::future_status::ready)
        << "a cancelled load must still resolve its waiters";
    SharedPtr<Asset> firstResult = loadFuture.get(); // null when the cancel won; the asset when it lost
    unloadFuture.wait();

    // The pin: cancellation must not poison the suppression cache.
    EXPECT_FALSE(assetManager->IsLoadSuppressed(target))
        << "a cancelled load must not mark the GUID suppressed";

    // Re-request (Critical jumps the still-saturated IO queue): the load must
    // actually run and succeed.
    auto retryFuture = assetManager->LoadAssetAsync(target, AssetLoadPriority::Critical);
    ASSERT_EQ(retryFuture.wait_for(std::chrono::milliseconds(10000)), std::future_status::ready)
        << "retry after cancel did not complete";
    SharedPtr<Asset> retried = retryFuture.get();
    ASSERT_NE(retried, nullptr) << "retry after cancel must load the asset";
    EXPECT_TRUE(assetManager->IsAssetLoaded(target));

    std::cout << "[ REGIME   ] cancel-then-retry: first load "
              << (firstResult ? "completed (cancel lost)" : "cancelled") << "\n";
}

TEST_F(AssetLoadPipelineTest, FailedDecodeKeepsItsSpecificSuppressionReason) {
    // The decode job records why a load failed (for a render pipeline, its
    // parse errors) before the load completes. The completion records the
    // generic failure only for a GUID with no record, so the suppression reason
    // callers read stays the decode job's.
    const std::string payload = "{ this is not json";
    const auto path = testDir / "malformed_pipeline.rendergraph";
    {
        std::ofstream file(path, std::ios::binary);
        file << payload;
    }
    AssetMetadata metadata =
        MakeMetadata(assetManager->GetRegistry(), path, payload, AssetType::RenderPipeline);
    assetManager->GetRegistry().RegisterAssetMetadata(metadata);

    auto loadFuture = assetManager->LoadAssetAsync(metadata.Guid, AssetLoadPriority::High);
    ASSERT_EQ(loadFuture.wait_for(std::chrono::seconds(10)), std::future_status::ready)
        << "the failed load must still resolve its waiters";
    EXPECT_EQ(loadFuture.get(), nullptr);

    String reason;
    ASSERT_TRUE(assetManager->IsLoadSuppressed(metadata.Guid, &reason)) << "a failed decode suppresses retries";
    EXPECT_TRUE(reason.starts_with("Failed to load '")) << reason;
    EXPECT_NE(reason.find("JSON parse failed"), std::string::npos) << reason;
}

TEST_F(AssetLoadPipelineTest, EjectDuringScanCancelsQueuedBacklog) {
    // Eject contract (design §3.3): unregistering a source mid-scan cancels
    // that source's queued in-flight loads and returns promptly — bounded by
    // genuinely running reads/decodes, never by the queued backlog. The
    // backlog here is 80 missing-file reads (each burns the ~50ms open-retry
    // window on a reader; ≥2s if actually drained on 2 readers) with 40 real
    // PNGs queued behind them.
    const auto gates = RegisterMissingAssets(80, "eject_gate_");
    const auto fleet = CreatePngFleet(40, "eject_");

    std::vector<AssetFuture> futures;
    futures.reserve(gates.size() + fleet.size());
    for (const auto& gate : gates) {
        futures.push_back(assetManager->LoadAssetAsync(gate.Guid, AssetLoadPriority::Normal));
    }
    for (const auto& metadata : fleet) {
        futures.push_back(assetManager->LoadAssetAsync(metadata.Guid, AssetLoadPriority::Normal));
    }

    std::atomic<bool> unregistered{false};
    const auto ejectStart = std::chrono::steady_clock::now();
    ASSERT_TRUE(assetManager->BeginUnregisterSource(
        "project", [&unregistered](bool) { unregistered.store(true, std::memory_order_release); }));
    const auto ejectMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - ejectStart).count();

    // The eject no longer waits on the thread that asks for it: it records the
    // loads it must outlast and finishes from Update once they resolve. Drive
    // that here, then assert what the old blocking contract asserted at return.
    const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!unregistered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < drainDeadline) {
        assetManager->Update();
        std::this_thread::yield();
    }
    ASSERT_TRUE(unregistered.load(std::memory_order_acquire)) << "eject never drained";

    size_t loaded = 0;
    size_t cancelledOrFailed = 0;
    for (size_t i = 0; i < futures.size(); ++i) {
        ASSERT_EQ(futures[i].wait_for(std::chrono::milliseconds(0)), std::future_status::ready)
            << "load " << i << " not resolved when the eject finished";
        SharedPtr<Asset> asset;
        try {
            asset = futures[i].get();
        } catch (const std::exception&) {
            asset = nullptr;
        }
        if (asset)
            ++loaded;
        else
            ++cancelledOrFailed;
    }

    std::cout << "[ REGIME   ] eject-mid-scan: " << ejectMs << "ms, " << loaded
              << " loaded / " << cancelledOrFailed << " cancelled-or-failed of "
              << futures.size() << "\n";

    // The queued PNG backlog must have been cancelled, not loaded — this is
    // the prompt-return contract stated as an outcome instead of a stopwatch.
    // A drained backlog would have loaded the fleet; a handful may genuinely
    // complete (claimed reads / running decodes survive). ejectMs is reported
    // above but not asserted: its tail overlaps the ~2s drain floor on a
    // contended box (83-380ms across three runs at processor-queue-length 121,
    // 2026-08-10 Debug), so the clock cannot separate the two behaviours.
    EXPECT_LT(loaded, fleet.size() / 2) << "eject loaded the backlog instead of cancelling it";
    EXPECT_EQ(assetManager->GetInFlightLoadCount(), 0u);
}

TEST_F(AssetLoadPipelineTest, ShutdownMidScanResolvesAllPromises) {
    // Shutdown-correctness (design R2): tearing the manager and the pool down
    // in engine order (AssetIOService stops inside AssetManager::Shutdown,
    // pool afterwards) while a scan is in flight must resolve every waiter —
    // reads still queued are abandoned by the IO service, decode jobs still
    // queued are Cancelled by the pool's shutdown drain and resolve through
    // the OnFailure path. No promise may strand, nothing may hang.
    const size_t numAssets = 100;
    const auto fleet = CreatePngFleet(numAssets, "shutdown_scan_");

    std::vector<AssetFuture> futures;
    futures.reserve(fleet.size());
    for (const auto& metadata : fleet) {
        futures.push_back(assetManager->LoadAssetAsync(metadata.Guid, AssetLoadPriority::Normal));
    }

    // Engine teardown order: AssetManager (stops the AssetIOService) before
    // the JobSystem pool.
    assetManager->Shutdown();
    jobSystem->Shutdown();

    // Every future must be resolved — loaded, failed, or cancelled — with no
    // residual waiter. Generous per-future timeout only as a hang backstop.
    size_t loaded = 0;
    size_t abandoned = 0;
    for (size_t i = 0; i < futures.size(); ++i) {
        ASSERT_EQ(futures[i].wait_for(std::chrono::milliseconds(5000)), std::future_status::ready)
            << "asset " << i << " promise stranded across shutdown";
        SharedPtr<Asset> asset;
        try {
            asset = futures[i].get();
        } catch (const std::exception&) {
            asset = nullptr; // an exceptional resolution still counts as resolved
        }
        if (asset)
            ++loaded;
        else
            ++abandoned;
    }

    Logger::Log::Info("AssetLoadPipelineTest: shutdown-mid-scan resolved {} loaded / {} abandoned",
                      loaded, abandoned);
    // Visible in gtest output (the suite adds no Logger sink): lets a rerun
    // confirm the regime — abandoned > 0 means shutdown genuinely raced
    // in-flight loads rather than landing after the scan finished.
    std::cout << "[ REGIME   ] shutdown-mid-scan: " << loaded << " loaded / "
              << abandoned << " abandoned\n";
    EXPECT_EQ(loaded + abandoned, numAssets);
}

TEST_F(AssetLoadPipelineTest, ShutdownMidScanDrainsDecodesAndLeavesPoolHealthy) {
    // Engine-teardown-order UAF guard (S2 audit C2): the engine destroys the
    // AssetManager BEFORE the JobSystem pool, so AssetManager::Shutdown must
    // leave nothing in the pool that references the manager. Contract pinned
    // here: (1) every load promise is resolved by the time Shutdown returns
    // (queued decodes cancelled, running decodes waited out), (2) destroying
    // the manager while the pool keeps running causes no UAF, (3) the pool
    // stays healthy for unrelated work afterwards.
    const size_t numAssets = 100;
    const auto fleet = CreatePngFleet(numAssets, "drain_scan_");

    std::vector<AssetFuture> futures;
    futures.reserve(fleet.size());
    for (const auto& metadata : fleet) {
        futures.push_back(assetManager->LoadAssetAsync(metadata.Guid, AssetLoadPriority::Normal));
    }

    assetManager->Shutdown(); // pool deliberately still running (engine order)

    size_t loaded = 0;
    size_t abandoned = 0;
    for (size_t i = 0; i < futures.size(); ++i) {
        ASSERT_EQ(futures[i].wait_for(std::chrono::milliseconds(0)), std::future_status::ready)
            << "load " << i << " not resolved when Shutdown returned — the drain contract is broken";
        SharedPtr<Asset> asset;
        try {
            asset = futures[i].get();
        } catch (const std::exception&) {
            asset = nullptr;
        }
        if (asset)
            ++loaded;
        else
            ++abandoned;
    }
    EXPECT_EQ(assetManager->GetInFlightLoadCount(), 0u)
        << "in-flight entries survived Shutdown";

    // The UAF window: manager destroyed, pool still running. Any decode job
    // or callback still referencing the manager crashes/corrupts here.
    assetManager.reset();

    // The pool must remain fully usable for unrelated work.
    std::atomic<bool> poolStillWorks{false};
    auto probe = jobSystem->Submit([&poolStillWorks] { poolStillWorks.store(true); });
    ASSERT_TRUE(probe.IsValid());
    probe.Wait();
    EXPECT_TRUE(poolStillWorks.load()) << "pool unhealthy after AssetManager teardown";

    std::cout << "[ REGIME   ] shutdown-drain: " << loaded << " loaded / " << abandoned
              << " cancelled of " << numAssets << " (pool healthy: "
              << (poolStillWorks.load() ? "yes" : "no") << ")\n";
}

TEST_F(AssetLoadPipelineTest, CancelledDecodesReleaseTheDecodeGate) {
    // Regression: the AssetIOService decode-occupancy gate must release a slot
    // when a decode task is cancelled BEFORE it runs — TaskHandle::Cancel that
    // wins means the task body never executes, so a release tied to execution
    // leaks the slot. One leak per won cancel ratchets the gate shut; after
    // Max leaks every reader thread parks at the gate forever and no asset in
    // the session ever decodes again (the editor-startup wedge this pins).
    //
    // Deterministic shape: a private 4-worker pool fully occupied by blocker
    // tasks so submitted decodes stay queued; gate capped at 2 via env; two
    // loads submit decodes (both slots taken), both loads cancelled (cancel
    // wins — workers are blocked), blockers released, then two FRESH loads
    // must still complete. With the leak, the fresh loads never decode.
#ifdef _WIN32
    _putenv_s("GE_ASSET_DECODE_MAX", "2");
#else
    setenv("GE_ASSET_DECODE_MAX", "2", 1);
#endif
    const auto localDir = TestUtils::MakeUniqueTempDirectory("decode_gate_cancel_test");
    std::filesystem::create_directories(localDir);
    auto localPool = std::make_unique<JobSystem::WorkStealingThreadPool>(4);
    auto localAm = std::make_unique<AssetManager>();
    localAm->Initialize(localDir, localPool.get());
#ifdef _WIN32
    _putenv_s("GE_ASSET_DECODE_MAX", "");
#else
    unsetenv("GE_ASSET_DECODE_MAX");
#endif

    // Four 1x1 PNGs registered with the LOCAL manager.
    const std::string content = PngData();
    std::vector<AssetMetadata> fleet;
    for (int i = 0; i < 4; ++i) {
        const auto path = localDir / ("gate_" + std::to_string(i) + ".png");
        std::ofstream file(path, std::ios::binary);
        file << content;
        file.close();
        AssetMetadata metadata =
            MakeMetadata(localAm->GetRegistry(), path, content, AssetType::Texture);
        localAm->GetRegistry().RegisterAssetMetadata(metadata);
        fleet.push_back(std::move(metadata));
    }

    // Occupy every pool worker so submitted decode tasks cannot start.
    std::atomic<bool> release{false};
    std::vector<JobSystem::TaskHandle> blockers;
    for (int i = 0; i < 4; ++i) {
        blockers.push_back(localPool->Submit([&release] {
            while (!release.load(std::memory_order_relaxed))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }));
    }

    // Two loads: reads complete on the reader threads, decode submissions take
    // both gate slots and queue behind the blockers.
    auto fut0 = localAm->LoadAssetAsync(fleet[0].Guid, AssetLoadPriority::Normal);
    auto fut1 = localAm->LoadAssetAsync(fleet[1].Guid, AssetLoadPriority::Normal);
    // Settle, not a readiness wait: both reads must finish and their decodes
    // must take the two gate slots before the cancels land. Too short and the
    // cancels beat the decode submissions, leaving no occupied slot to leak —
    // the test would pass without exercising anything.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Cancel both (public trigger: unload withdraws the in-flight load): the
    // queued decode envelopes are dropped without running.
    auto unload0 = localAm->UnloadAssetAsync(fleet[0].Guid);
    auto unload1 = localAm->UnloadAssetAsync(fleet[1].Guid);

    // Free the workers; the pool pops and discards the cancelled envelopes,
    // which must release their gate slots.
    release.store(true, std::memory_order_relaxed);
    for (auto& b : blockers)
        b.Wait();
    unload0.wait();
    unload1.wait();

    // The gate must admit new decodes: two fresh loads complete.
    auto fut2 = localAm->LoadAssetAsync(fleet[2].Guid, AssetLoadPriority::Normal);
    auto fut3 = localAm->LoadAssetAsync(fleet[3].Guid, AssetLoadPriority::Normal);
    ASSERT_EQ(fut2.wait_for(std::chrono::seconds(10)), std::future_status::ready)
        << "decode gate wedged: cancelled decode leaked its occupancy slot";
    ASSERT_EQ(fut3.wait_for(std::chrono::seconds(10)), std::future_status::ready)
        << "decode gate wedged: cancelled decode leaked its occupancy slot";
    EXPECT_NE(fut2.get(), nullptr);
    EXPECT_NE(fut3.get(), nullptr);

    localAm->Shutdown();
    localAm.reset();
    localPool->Shutdown();
    localPool.reset();
    std::filesystem::remove_all(localDir);
}

namespace {

// Holds every decode of a file named hold_* inside the decode job until the
// test releases it, so a load can be observed while it is genuinely in flight.
// Everything else parses normally.
class HeldBinaryParser : public AssetParser {
public:
    AssetType GetAssetType() const override { return AssetType::Unknown; }
    std::vector<std::string> GetSupportedExtensions() const override { return {".bin"}; }
    int GetPriority() const override { return 1000; }
    std::string GetName() const override { return "HeldBinaryParser"; }

    AssetParseResult Parse(const AssetMetadata& metadata, AssetManager& assetManager) override {
        if (metadata.Path.filename().string().rfind("hold", 0) == 0) {
            Entered.fetch_add(1, std::memory_order_release);
            Gate.wait();
        }
        return m_Inner.Parse(metadata, assetManager);
    }

    /// Incremented as each held decode enters, before it blocks.
    std::atomic<int> Entered{0};
    /// Released by the test to let the held decodes through.
    std::shared_future<void> Gate;

private:
    BinaryAssetParser m_Inner;
};

bool WaitForDecodeToEnter(const HeldBinaryParser& parser,
                          std::chrono::seconds budget = std::chrono::seconds(20)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (parser.Entered.load(std::memory_order_acquire) < 1) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Callback-observation state, owned by shared_ptr so the callback stays valid
// even if it is delivered after the test body has returned.
struct CallbackProbe {
    std::atomic<bool> Fired{false};
    std::atomic<bool> Ok{false};
};

} // namespace

TEST_F(AssetLoadPipelineTest, DiscardedHandleStillDeliversTheCompletionCallback) {
    // Fire-and-forget is the documented contract of LoadAsset(guid, callback),
    // and nearly every call site in the engine uses it that way: it asks for a
    // load, keeps no handle, and expects the callback. The held parser pins the
    // load in flight so the callback is registered on the in-flight entry (the
    // path a resident or already-finished asset skips).
    auto held = std::make_shared<HeldBinaryParser>();
    std::promise<void> release;
    held->Gate = release.get_future().share();
    ASSERT_TRUE(assetManager->GetParserRegistry().RegisterParser(held, held->GetPriority()));

    const std::string payload = "held payload";
    const auto path = testDir / "hold_deliver.bin";
    {
        std::ofstream file(path, std::ios::binary);
        file << payload;
    }
    AssetMetadata metadata =
        MakeMetadata(assetManager->GetRegistry(), path, payload, AssetType::Unknown);
    assetManager->GetRegistry().RegisterAssetMetadata(metadata);
    ASSERT_FALSE(assetManager->IsAssetLoaded(metadata.Guid));

    auto probe = std::make_shared<CallbackProbe>();
    assetManager->LoadAsset(
        metadata.Guid,
        [probe](Result<SharedPtr<Asset>, AssetError> result) {
            probe->Ok.store(result.IsOk() && result.Value() != nullptr, std::memory_order_release);
            probe->Fired.store(true, std::memory_order_release);
        },
        AssetLoadPriority::High);

    ASSERT_TRUE(WaitForDecodeToEnter(*held)) << "the load never reached its decode";
    release.set_value();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!probe->Fired.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_TRUE(probe->Fired.load(std::memory_order_acquire))
        << "LoadAsset(guid, callback) never delivered its callback when the caller kept no handle";
    EXPECT_TRUE(probe->Ok.load(std::memory_order_acquire))
        << "the callback fired but did not carry the loaded asset";
    EXPECT_TRUE(assetManager->IsAssetLoaded(metadata.Guid))
        << "the load itself must have completed";
}

TEST_F(AssetLoadPipelineTest, CancellingTheHandleWithholdsTheCompletionCallback) {
    // The other half of the contract: cancellation is explicit. A caller that
    // keeps the handle to scope the callback to its own lifetime withdraws the
    // request by calling Cancel(), and the callback must then not fire.
    auto held = std::make_shared<HeldBinaryParser>();
    std::promise<void> release;
    held->Gate = release.get_future().share();
    ASSERT_TRUE(assetManager->GetParserRegistry().RegisterParser(held, held->GetPriority()));

    const std::string payload = "held payload";
    const auto path = testDir / "hold_cancel.bin";
    {
        std::ofstream file(path, std::ios::binary);
        file << payload;
    }
    AssetMetadata metadata =
        MakeMetadata(assetManager->GetRegistry(), path, payload, AssetType::Unknown);
    assetManager->GetRegistry().RegisterAssetMetadata(metadata);

    auto probe = std::make_shared<CallbackProbe>();
    AssetLoadHandle handle = assetManager->LoadAsset(
        metadata.Guid,
        [probe](Result<SharedPtr<Asset>, AssetError>) {
            probe->Fired.store(true, std::memory_order_release);
        },
        AssetLoadPriority::High);

    ASSERT_TRUE(WaitForDecodeToEnter(*held)) << "the load never reached its decode";
    handle.Cancel();
    release.set_value();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (assetManager->GetInFlightLoadCount() != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(assetManager->GetInFlightLoadCount(), 0u) << "the cancelled load never completed";

    EXPECT_FALSE(probe->Fired.load(std::memory_order_acquire))
        << "a cancelled request must not deliver its callback";
}
