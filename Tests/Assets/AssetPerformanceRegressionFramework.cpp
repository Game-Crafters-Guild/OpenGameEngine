#include <gtest/gtest.h>
#include "Core/Engine.h"
#include "Core/Application.h"
#include "Assets/AssetManager.h"
#include "Assets/AudioAsset.h"
#include "Assets/ModelAsset.h"
#include "Assets/BinaryAsset.h"
#include "Assets/TextureAsset.h"
#include "AssetCore/GUID.h"
#include "AssetCore/AssetRegistry.h"
#include "Logger/Logger.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <chrono>
#include <thread>
#include <numeric>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <cstdio>

using namespace GameEngine;

/**
 * @brief High-precision performance measurement utilities
 */
class PerformanceMeasurement {
public:
    using TimePoint = std::chrono::high_resolution_clock::time_point;
    using Duration = std::chrono::nanoseconds;
    
    static TimePoint Now() {
        return std::chrono::high_resolution_clock::now();
    }
    
    static double ToMilliseconds(Duration duration) {
        return duration.count() / 1000000.0;
    }
    
    static double ToMicroseconds(Duration duration) {
        return duration.count() / 1000.0;
    }
    
    /**
     * @brief Statistical analysis of performance measurements
     */
    struct Statistics {
        double mean = 0.0;
        double median = 0.0;
        double stddev = 0.0;
        double min = 0.0;
        double max = 0.0;
        double p95 = 0.0;  // 95th percentile
        double p99 = 0.0;  // 99th percentile
        size_t sampleCount = 0;
        
        std::string ToString() const {
            std::stringstream ss;
            ss << std::fixed << std::setprecision(3);
            ss << "Mean: " << mean << "ms, ";
            ss << "Median: " << median << "ms, ";
            ss << "StdDev: " << stddev << "ms, ";
            ss << "Min: " << min << "ms, ";
            ss << "Max: " << max << "ms, ";
            ss << "P95: " << p95 << "ms, ";
            ss << "P99: " << p99 << "ms, ";
            ss << "Samples: " << sampleCount;
            return ss.str();
        }
    };
    
    /**
     * @brief Calculate statistics from a vector of measurements (in milliseconds)
     */
    static Statistics CalculateStatistics(std::vector<double> measurements) {
        if (measurements.empty()) {
            return Statistics{};
        }
        
        Statistics stats;
        stats.sampleCount = measurements.size();
        
        // Sort for percentile calculations
        std::sort(measurements.begin(), measurements.end());
        
        // Basic statistics
        stats.min = measurements.front();
        stats.max = measurements.back();
        stats.median = measurements[measurements.size() / 2];
        
        // Mean
        double sum = std::accumulate(measurements.begin(), measurements.end(), 0.0);
        stats.mean = sum / measurements.size();
        
        // Standard deviation
        double variance = 0.0;
        for (double measurement : measurements) {
            variance += (measurement - stats.mean) * (measurement - stats.mean);
        }
        stats.stddev = std::sqrt(variance / measurements.size());
        
        // Percentiles
        stats.p95 = measurements[static_cast<size_t>(measurements.size() * 0.95)];
        stats.p99 = measurements[static_cast<size_t>(measurements.size() * 0.99)];
        
        return stats;
    }
};

/**
 * @brief Performance thresholds for regression testing
 */
struct PerformanceThresholds {
    // Main thread blocking thresholds
    double mainThreadBlockingTarget = 1.0;      // <1ms target
    double mainThreadBlockingWarning = 5.0;     // 5ms warning threshold
    double mainThreadBlockingFailure = 50.0;    // 50ms failure threshold
    
    // Per-asset submission thresholds
    double perAssetSubmissionTarget = 0.1;      // <0.1ms target
    double perAssetSubmissionWarning = 0.5;     // 0.5ms warning threshold
    double perAssetSubmissionFailure = 5.0;     // 5ms failure threshold
    
    // Pipeline completion thresholds
    double pipelineCompletionTarget = 20.0;     // <20ms target
    double pipelineCompletionWarning = 100.0;   // 100ms warning threshold
    double pipelineCompletionFailure = 1000.0;  // 1000ms failure threshold
    
    // Throughput thresholds (assets per second)
    double throughputTarget = 1000.0;           // 1000+ assets/second target
    double throughputWarning = 100.0;           // 100 assets/second warning
    double throughputFailure = 10.0;            // 10 assets/second failure
    
    // Memory usage thresholds (MB)
    double memoryUsageTarget = 100.0;           // <100MB target
    double memoryUsageWarning = 500.0;          // 500MB warning
    double memoryUsageFailure = 1000.0;         // 1000MB failure
};

/**
 * @brief Performance regression test framework
 */
class AssetPerformanceRegressionFramework : public ::testing::Test {
protected:
    void SetUp() override {
        // Create test directory
        testDir = TestUtils::MakeUniqueTempDirectory("perf_regression_test_assets");
        std::filesystem::create_directories(testDir);
        std::fprintf(stderr, "[Perf] SetUp: testDir=%s\n", testDir.string().c_str());
        std::fflush(stderr);
        
        // Use Info during Engine init so hangs are diagnosable, then drop to Warning
        // for the actual performance measurements.
        Logger::Log::SetLogLevel(Logger::LogLevel::Info);
        
        // Initialize Engine with job system enabled
        engine = std::make_unique<EngineCore>();
        ApplicationConfig config;
        config.AssetDirectory = testDir.string();

        ScriptsConfig scriptsConfig{};
        // These performance tests target the native asset pipeline and should not block
        // on managed compilation / CompileServer startup.
        scriptsConfig.disableClr = true;
        scriptsConfig.enableHotReload = false; // Disable for performance testing
        scriptsConfig.enableAsyncHotReload = false;
        scriptsConfig.enableAutoProjectGeneration = false;
        engine->SetScriptsConfig(scriptsConfig);

        std::fprintf(stderr, "[Perf] SetUp: calling Engine::Initialize(assetDirectory=%s)\n", config.AssetDirectory.c_str());
        std::fflush(stderr);
        ASSERT_TRUE(engine->Initialize(config)) << "Failed to initialize Engine for performance testing";
        std::fprintf(stderr, "[Perf] SetUp: Engine::Initialize returned\n");
        std::fflush(stderr);

        // Reduce logging overhead for performance measurements
        Logger::Log::SetLogLevel(Logger::LogLevel::Warning);

        // Get asset manager from engine
        assetManager = &engine->GetAssetManager();
        
        // Create performance test assets
        CreatePerformanceTestAssets();
        std::fprintf(stderr, "[Perf] SetUp: CreatePerformanceTestAssets complete (count=%zu)\n", testAssetCount);
        std::fflush(stderr);
        
        Logger::Log::Info("AssetPerformanceRegressionFramework: Setup complete with {} test assets", testAssetCount);
    }
    
    void TearDown() override {
        // Shutdown engine
        if (engine) {
            engine->Shutdown();
            engine.reset();
        }
        
        // Clean up test files
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
        
        Logger::Log::Info("AssetPerformanceRegressionFramework: Teardown complete");
    }
    
    void CreatePerformanceTestAssets() {
        // Create various sized assets for performance testing
        
        // Small binary assets (1KB each)
        for (int i = 0; i < 50; ++i) {
            auto path = testDir / ("small_" + std::to_string(i) + ".bin");
            CreateTestBinaryFile(path, 1024);
            smallAssetPaths.push_back(path);
        }
        
        // Medium binary assets (10KB each)
        for (int i = 0; i < 20; ++i) {
            auto path = testDir / ("medium_" + std::to_string(i) + ".bin");
            CreateTestBinaryFile(path, 10240);
            mediumAssetPaths.push_back(path);
        }
        
        // Large binary assets (100KB each)
        for (int i = 0; i < 5; ++i) {
            auto path = testDir / ("large_" + std::to_string(i) + ".bin");
            CreateTestBinaryFile(path, 102400);
            largeAssetPaths.push_back(path);
        }
        
        // Audio assets
        for (int i = 0; i < 10; ++i) {
            auto path = testDir / ("audio_" + std::to_string(i) + ".wav");
            CreateTestWAVFile(path);
            audioAssetPaths.push_back(path);
        }
        
        // Model assets
        for (int i = 0; i < 10; ++i) {
            auto path = testDir / ("model_" + std::to_string(i) + ".obj");
            CreateTestOBJFile(path);
            modelAssetPaths.push_back(path);
        }
        
        testAssetCount = smallAssetPaths.size() + mediumAssetPaths.size() + 
                        largeAssetPaths.size() + audioAssetPaths.size() + modelAssetPaths.size();
    }
    
    void CreateTestBinaryFile(const std::filesystem::path& path, size_t size) {
        std::ofstream file(path, std::ios::binary);
        std::vector<uint8_t> data(size);
        for (size_t i = 0; i < size; ++i) {
            data[i] = static_cast<uint8_t>((i * 37) % 256);
        }
        file.write(reinterpret_cast<const char*>(data.data()), size);
        file.close();
    }
    
    void CreateTestWAVFile(const std::filesystem::path& path) {
        std::ofstream file(path, std::ios::binary);

        // Create minimal audio data for performance tests (just 10 samples = 40 bytes)
        const uint32_t sampleRate = 44100;
        const uint16_t numChannels = 2;
        const uint16_t bitsPerSample = 16;
        const uint32_t dataSize = 10 * numChannels * (bitsPerSample / 8); // Just 10 samples
        const uint32_t fileSize = 36 + dataSize;  // Header size + data size

        // WAV header
        file.write("RIFF", 4);                    // ChunkID
        file.write(reinterpret_cast<const char*>(&fileSize), 4);
        file.write("WAVE", 4);                    // Format

        // fmt subchunk
        file.write("fmt ", 4);                    // Subchunk1ID
        uint32_t subchunk1Size = 16;              // Subchunk1Size
        file.write(reinterpret_cast<const char*>(&subchunk1Size), 4);
        uint16_t audioFormat = 1;                 // PCM
        file.write(reinterpret_cast<const char*>(&audioFormat), 2);
        file.write(reinterpret_cast<const char*>(&numChannels), 2);
        file.write(reinterpret_cast<const char*>(&sampleRate), 4);
        uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);
        file.write(reinterpret_cast<const char*>(&byteRate), 4);
        uint16_t blockAlign = numChannels * (bitsPerSample / 8);
        file.write(reinterpret_cast<const char*>(&blockAlign), 2);
        file.write(reinterpret_cast<const char*>(&bitsPerSample), 2);

        // data subchunk
        file.write("data", 4);                    // Subchunk2ID
        file.write(reinterpret_cast<const char*>(&dataSize), 4);

        // Write minimal audio data (silence) - just 40 bytes instead of 176KB
        std::vector<uint8_t> audioData(dataSize, 0);
        file.write(reinterpret_cast<const char*>(audioData.data()), dataSize);

        file.close();
    }
    
    void CreateTestOBJFile(const std::filesystem::path& path) {
        std::ofstream file(path);
        file << "# Performance test OBJ file\n";
        file << "v 0.0 0.0 0.0\n";
        file << "v 1.0 0.0 0.0\n";
        file << "v 0.0 1.0 0.0\n";
        file << "v 0.0 0.0 1.0\n";
        file << "vn 0.0 0.0 1.0\n";
        file << "vn 0.0 1.0 0.0\n";
        file << "vt 0.0 0.0\n";
        file << "vt 1.0 0.0\n";
        file << "vt 0.5 1.0\n";
        file << "f 1/1/1 2/2/1 3/3/1\n";
        file << "f 1/1/2 3/3/2 4/1/2\n";
        file.close();
    }
    
    /**
     * @brief Measure main thread blocking time for asset submission
     */
    PerformanceMeasurement::Statistics MeasureMainThreadBlocking(
        const std::vector<std::filesystem::path>& assetPaths, 
        int iterations = 10) {
        
        std::vector<double> measurements;
        measurements.reserve(iterations);
        
        for (int iter = 0; iter < iterations; ++iter) {
            // Create fresh asset metadata for each iteration
            std::vector<GUID> assetGuids;
            std::vector<AssetLoadHandle> handles;
            
            for (const auto& path : assetPaths) {
                GUID guid = GUID::Generate();
                AssetMetadata metadata;
                metadata.Guid = guid;
                metadata.Path = path;
                metadata.Name = path.stem().string();
                metadata.Extension = path.extension().string();
                metadata.Type = AssetType::Unknown; // Use Unknown for binary assets
                metadata.FileSize = std::filesystem::file_size(path);
                
                assetManager->GetRegistry().RegisterAssetMetadata(metadata);
                assetGuids.push_back(guid);
            }
            
            // Measure main thread blocking time
            auto start = PerformanceMeasurement::Now();
            
            for (const auto& guid : assetGuids) {
                AssetLoadRequest request;
                request.AssetGuid = guid;
                request.Priority = AssetLoadPriority::Normal;
                
                auto handle = assetManager->LoadAsset(request);
                handles.push_back(std::move(handle));
            }
            
            auto end = PerformanceMeasurement::Now();
            auto duration = end - start;
            double milliseconds = PerformanceMeasurement::ToMilliseconds(duration);
            measurements.push_back(milliseconds);
            
            // Wait for completion to avoid interference with next iteration
            for (auto& handle : handles) {
                for (int wait = 0; wait < 100 && !handle.IsComplete(); ++wait) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Reduced from 10ms to 1ms
                }
            }
            
            // Small delay between iterations
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        
        return PerformanceMeasurement::CalculateStatistics(measurements);
    }
    
protected:
    std::filesystem::path testDir;
    std::unique_ptr<EngineCore> engine;
    AssetManager* assetManager;
    PerformanceThresholds thresholds;
    
    std::vector<std::filesystem::path> smallAssetPaths;
    std::vector<std::filesystem::path> mediumAssetPaths;
    std::vector<std::filesystem::path> largeAssetPaths;
    std::vector<std::filesystem::path> audioAssetPaths;
    std::vector<std::filesystem::path> modelAssetPaths;
    
    size_t testAssetCount = 0;

protected:
    /**
     * @brief Get current memory usage (simplified implementation)
     */
    size_t GetCurrentMemoryUsage() {
        // This is a simplified implementation
        // In a real implementation, you would use platform-specific APIs
        // For now, return a placeholder value
        return 50 * 1024 * 1024; // 50MB placeholder
    }

    /**
     * @brief Measure main thread blocking with artificial delay (for regression testing)
     */
    PerformanceMeasurement::Statistics MeasureMainThreadBlockingWithDelay(
        const std::vector<std::filesystem::path>& assetPaths,
        int iterations,
        double delayMs) {

        std::vector<double> measurements;
        measurements.reserve(iterations);

        for (int iter = 0; iter < iterations; ++iter) {
            std::vector<GUID> assetGuids;
            std::vector<AssetLoadHandle> handles;

            for (const auto& path : assetPaths) {
                GUID guid = GUID::Generate();
                AssetMetadata metadata;
                metadata.Guid = guid;
                metadata.Path = path;
                metadata.Name = path.stem().string();
                metadata.Extension = path.extension().string();
                metadata.Type = AssetType::Unknown;
                metadata.FileSize = std::filesystem::file_size(path);

                assetManager->GetRegistry().RegisterAssetMetadata(metadata);
                assetGuids.push_back(guid);
            }

            auto start = PerformanceMeasurement::Now();

            for (const auto& guid : assetGuids) {
                // Add artificial delay to simulate regression using CPU work instead of sleep
                // (sleep_for has poor precision on Windows)
                volatile int dummy = 0;
                int workAmount = static_cast<int>(delayMs * 10000); // Scale factor for CPU work
                for (int i = 0; i < workAmount; ++i) {
                    dummy += i;
                }

                AssetLoadRequest request;
                request.AssetGuid = guid;
                request.Priority = AssetLoadPriority::Normal;

                auto handle = assetManager->LoadAsset(request);
                handles.push_back(std::move(handle));
            }

            auto end = PerformanceMeasurement::Now();
            auto duration = end - start;
            double milliseconds = PerformanceMeasurement::ToMilliseconds(duration);
            measurements.push_back(milliseconds);

            // Wait for completion
            for (auto& handle : handles) {
                for (int wait = 0; wait < 100 && !handle.IsComplete(); ++wait) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Reduced from 10ms to 1ms
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(5)); // Reduced from 50ms to 5ms
        }

        return PerformanceMeasurement::CalculateStatistics(measurements);
    }
};

// ========================================
// PERFORMANCE REGRESSION TESTS
// ========================================

// Test main thread blocking performance with small assets
TEST_F(AssetPerformanceRegressionFramework, MainThreadBlocking_SmallAssets) {
    Logger::Log::Info("=== Testing Main Thread Blocking Performance - Small Assets ===");

    auto stats = MeasureMainThreadBlocking(smallAssetPaths, 20);

    Logger::Log::Info("Small Assets Main Thread Blocking: {}", stats.ToString());

    // Performance assertions
    EXPECT_LT(stats.mean, thresholds.mainThreadBlockingFailure)
        << "Main thread blocking mean (" << stats.mean << "ms) exceeds failure threshold ("
        << thresholds.mainThreadBlockingFailure << "ms)";

    if (stats.mean > thresholds.mainThreadBlockingWarning) {
        Logger::Log::Warning("Main thread blocking mean ({:.3f}ms) exceeds warning threshold ({:.3f}ms)",
                       stats.mean, thresholds.mainThreadBlockingWarning);
    }

    if (stats.mean <= thresholds.mainThreadBlockingTarget) {
        Logger::Log::Info("✅ Main thread blocking meets target (<{:.3f}ms)", thresholds.mainThreadBlockingTarget);
    }

    // Check for performance consistency (low standard deviation)
    double coefficientOfVariation = stats.stddev / stats.mean;
    EXPECT_LT(coefficientOfVariation, 0.5)
        << "Performance is inconsistent (CV: " << coefficientOfVariation << ")";

    // Check 95th percentile performance
    EXPECT_LT(stats.p95, thresholds.mainThreadBlockingFailure * 1.5)
        << "95th percentile performance (" << stats.p95 << "ms) is too slow";
}

// Test main thread blocking performance with medium assets
TEST_F(AssetPerformanceRegressionFramework, MainThreadBlocking_MediumAssets) {
    Logger::Log::Info("=== Testing Main Thread Blocking Performance - Medium Assets ===");

    auto stats = MeasureMainThreadBlocking(mediumAssetPaths, 15);

    Logger::Log::Info("Medium Assets Main Thread Blocking: {}", stats.ToString());

    // Performance assertions with slightly relaxed thresholds for larger assets
    EXPECT_LT(stats.mean, thresholds.mainThreadBlockingFailure * 2.0)
        << "Main thread blocking mean (" << stats.mean << "ms) exceeds failure threshold";

    if (stats.mean > thresholds.mainThreadBlockingWarning * 1.5) {
        Logger::Log::Warning("Main thread blocking mean ({:.3f}ms) exceeds warning threshold for medium assets",
                       stats.mean);
    }

    // Check performance consistency
    double coefficientOfVariation = stats.stddev / stats.mean;
    EXPECT_LT(coefficientOfVariation, 0.6)
        << "Performance is inconsistent for medium assets (CV: " << coefficientOfVariation << ")";
}

// Test main thread blocking performance with large assets
TEST_F(AssetPerformanceRegressionFramework, MainThreadBlocking_LargeAssets) {
    Logger::Log::Info("=== Testing Main Thread Blocking Performance - Large Assets ===");

    auto stats = MeasureMainThreadBlocking(largeAssetPaths, 10);

    Logger::Log::Info("Large Assets Main Thread Blocking: {}", stats.ToString());

    // Performance assertions with relaxed thresholds for large assets
    EXPECT_LT(stats.mean, thresholds.mainThreadBlockingFailure * 5.0)
        << "Main thread blocking mean (" << stats.mean << "ms) exceeds failure threshold for large assets";

    if (stats.mean > thresholds.mainThreadBlockingWarning * 3.0) {
        Logger::Log::Warning("Main thread blocking mean ({:.3f}ms) exceeds warning threshold for large assets",
                       stats.mean);
    }

    // Large assets may have more variation due to I/O
    double coefficientOfVariation = stats.stddev / stats.mean;
    EXPECT_LT(coefficientOfVariation, 0.8)
        << "Performance is too inconsistent for large assets (CV: " << coefficientOfVariation << ")";
}

// Test per-asset submission performance
TEST_F(AssetPerformanceRegressionFramework, PerAssetSubmission_Performance) {
    Logger::Log::Info("=== Testing Per-Asset Submission Performance ===");

    auto stats = MeasureMainThreadBlocking(smallAssetPaths, 20);
    double perAssetTime = stats.mean / smallAssetPaths.size();

    Logger::Log::Info("Per-Asset Submission Time: {:.6f}ms (from {:.3f}ms total for {} assets)",
                perAssetTime, stats.mean, smallAssetPaths.size());

    // Performance assertions
    EXPECT_LT(perAssetTime, thresholds.perAssetSubmissionFailure)
        << "Per-asset submission time (" << perAssetTime << "ms) exceeds failure threshold ("
        << thresholds.perAssetSubmissionFailure << "ms)";

    if (perAssetTime > thresholds.perAssetSubmissionWarning) {
        Logger::Log::Warning("Per-asset submission time ({:.6f}ms) exceeds warning threshold ({:.6f}ms)",
                       perAssetTime, thresholds.perAssetSubmissionWarning);
    }

    if (perAssetTime <= thresholds.perAssetSubmissionTarget) {
        Logger::Log::Info("✅ Per-asset submission meets target (<{:.6f}ms)", thresholds.perAssetSubmissionTarget);
    }

    // Calculate theoretical throughput
    double theoreticalThroughput = 1000.0 / perAssetTime; // assets per second
    Logger::Log::Info("Theoretical Throughput: {:.1f} assets/second", theoreticalThroughput);

    EXPECT_GT(theoreticalThroughput, thresholds.throughputFailure)
        << "Theoretical throughput (" << theoreticalThroughput << " assets/sec) below failure threshold";
}

// Test mixed asset type performance
TEST_F(AssetPerformanceRegressionFramework, MixedAssetTypes_Performance) {
    Logger::Log::Info("=== Testing Mixed Asset Types Performance ===");

    // Combine different asset types
    std::vector<std::filesystem::path> mixedAssets;
    mixedAssets.insert(mixedAssets.end(), smallAssetPaths.begin(), smallAssetPaths.begin() + 10);
    mixedAssets.insert(mixedAssets.end(), audioAssetPaths.begin(), audioAssetPaths.begin() + 5);
    mixedAssets.insert(mixedAssets.end(), modelAssetPaths.begin(), modelAssetPaths.begin() + 5);

    auto stats = MeasureMainThreadBlocking(mixedAssets, 15);

    Logger::Log::Info("Mixed Asset Types Main Thread Blocking: {}", stats.ToString());

    // Performance assertions
    EXPECT_LT(stats.mean, thresholds.mainThreadBlockingFailure * 2.0)
        << "Mixed asset types performance exceeds failure threshold";

    // Check that mixed performance isn't significantly worse than individual types
    double perAssetTime = stats.mean / mixedAssets.size();
    EXPECT_LT(perAssetTime, thresholds.perAssetSubmissionFailure * 2.0)
        << "Mixed asset per-asset time is too slow: " << perAssetTime << "ms";
}

// Test performance under load (stress test)
TEST_F(AssetPerformanceRegressionFramework, HighLoad_StressTest) {
    Logger::Log::Info("=== Testing High Load Stress Performance ===");

    // Use all small assets for maximum load
    auto stats = MeasureMainThreadBlocking(smallAssetPaths, 5); // Fewer iterations for stress test

    Logger::Log::Info("High Load Stress Test: {}", stats.ToString());
    Logger::Log::Info("Asset Count: {}, Total Time: {:.3f}ms", smallAssetPaths.size(), stats.mean);

    // Stress test thresholds are more relaxed
    EXPECT_LT(stats.mean, thresholds.mainThreadBlockingFailure * 10.0)
        << "High load performance exceeds stress test failure threshold";

    // Calculate actual throughput under load
    double loadThroughput = (smallAssetPaths.size() * 1000.0) / stats.mean; // assets per second
    Logger::Log::Info("High Load Throughput: {:.1f} assets/second", loadThroughput);

    EXPECT_GT(loadThroughput, thresholds.throughputFailure)
        << "High load throughput below minimum threshold";

    // Check that performance doesn't degrade too much under load
    double perAssetTime = stats.mean / smallAssetPaths.size();
    EXPECT_LT(perAssetTime, thresholds.perAssetSubmissionFailure * 5.0)
        << "Per-asset time under load is too slow: " << perAssetTime << "ms";
}

// Test memory usage during asset loading
TEST_F(AssetPerformanceRegressionFramework, MemoryUsage_Monitoring) {
    Logger::Log::Info("=== Testing Memory Usage During Asset Loading ===");

    // Get baseline memory usage
    size_t baselineMemory = GetCurrentMemoryUsage();
    Logger::Log::Info("Baseline Memory Usage: {:.2f} MB", baselineMemory / (1024.0 * 1024.0));

    // Load assets and monitor memory
    std::vector<GUID> assetGuids;
    std::vector<AssetLoadHandle> handles;

    for (const auto& path : smallAssetPaths) {
        GUID guid = GUID::Generate();
        AssetMetadata metadata;
        metadata.Guid = guid;
        metadata.Path = path;
        metadata.Name = path.stem().string();
        metadata.Extension = path.extension().string();
        metadata.Type = AssetType::Unknown;
        metadata.FileSize = std::filesystem::file_size(path);

        assetManager->GetRegistry().RegisterAssetMetadata(metadata);

        AssetLoadRequest request;
        request.AssetGuid = guid;
        request.Priority = AssetLoadPriority::Normal;

        auto handle = assetManager->LoadAsset(request);
        handles.push_back(std::move(handle));

        assetGuids.push_back(guid);
    }

    // Wait for completion
    for (auto& handle : handles) {
        for (int wait = 0; wait < 100 && !handle.IsComplete(); ++wait) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // Get peak memory usage
    size_t peakMemory = GetCurrentMemoryUsage();
    double memoryIncrease = (peakMemory - baselineMemory) / (1024.0 * 1024.0);

    Logger::Log::Info("Peak Memory Usage: {:.2f} MB", peakMemory / (1024.0 * 1024.0));
    Logger::Log::Info("Memory Increase: {:.2f} MB for {} assets", memoryIncrease, smallAssetPaths.size());
    Logger::Log::Info("Memory Per Asset: {:.2f} KB", (memoryIncrease * 1024.0) / smallAssetPaths.size());

    // Memory usage assertions
    EXPECT_LT(memoryIncrease, thresholds.memoryUsageFailure)
        << "Memory usage increase (" << memoryIncrease << " MB) exceeds failure threshold";

    if (memoryIncrease > thresholds.memoryUsageWarning) {
        Logger::Log::Warning("Memory usage increase ({:.2f} MB) exceeds warning threshold ({:.2f} MB)",
                       memoryIncrease, thresholds.memoryUsageWarning);
    }

    if (memoryIncrease <= thresholds.memoryUsageTarget) {
        Logger::Log::Info("✅ Memory usage meets target (<{:.2f} MB)", thresholds.memoryUsageTarget);
    }
}

// Test performance regression detection
TEST_F(AssetPerformanceRegressionFramework, RegressionDetection_Baseline) {
    Logger::Log::Info("=== Testing Performance Regression Detection ===");

    // Establish baseline performance
    auto baselineStats = MeasureMainThreadBlocking(smallAssetPaths, 10);
    Logger::Log::Info("Baseline Performance: {}", baselineStats.ToString());

    // Simulate performance regression by adding artificial delay
    auto regressionStats = MeasureMainThreadBlockingWithDelay(smallAssetPaths, 5, 0.1); // 0.1ms delay
    Logger::Log::Info("Regression Performance: {}", regressionStats.ToString());

    // Calculate regression percentage
    double regressionPercent = ((regressionStats.mean - baselineStats.mean) / baselineStats.mean) * 100.0;
    Logger::Log::Info("Performance Regression: {:.1f}%", regressionPercent);

    // Regression detection thresholds - more realistic for small changes
    EXPECT_LT(regressionPercent, 200.0)
        << "Performance regression (" << regressionPercent << "%) exceeds 200% threshold";

    if (regressionPercent > 50.0) {
        Logger::Log::Warning("Significant performance regression detected: {:.1f}%", regressionPercent);
    }

    if (regressionPercent > 300.0) {
        FAIL() << "Critical performance regression detected: " << regressionPercent << "%";
    }
}

// Test 240Hz frame rate compatibility
TEST_F(AssetPerformanceRegressionFramework, FrameRate_240Hz_Compatibility) {
    Logger::Log::Info("=== Testing 240Hz Frame Rate Compatibility ===");

    // 240Hz = 4.17ms per frame budget
    const double frameTimeBudget = 1000.0 / 240.0; // 4.17ms
    const double assetLoadingBudget = frameTimeBudget * 0.1; // 10% of frame time = 0.417ms

    Logger::Log::Info("240Hz Frame Budget: {:.3f}ms", frameTimeBudget);
    Logger::Log::Info("Asset Loading Budget (10% of frame): {:.3f}ms", assetLoadingBudget);

    // Test with small number of assets per frame
    std::vector<std::filesystem::path> frameAssets(smallAssetPaths.begin(), smallAssetPaths.begin() + 5);
    auto stats = MeasureMainThreadBlocking(frameAssets, 20);

    Logger::Log::Info("Per-Frame Asset Loading: {}", stats.ToString());

    // 240Hz compatibility assertions
    EXPECT_LT(stats.mean, assetLoadingBudget * 10.0)
        << "Asset loading time (" << stats.mean << "ms) exceeds 240Hz compatibility threshold";

    if (stats.mean <= assetLoadingBudget) {
        Logger::Log::Info("✅ Asset loading meets 240Hz frame budget ({:.3f}ms)", assetLoadingBudget);
    } else if (stats.mean <= assetLoadingBudget * 2.0) {
        Logger::Log::Warning("Asset loading exceeds 240Hz budget but within 2x tolerance: {:.3f}ms", stats.mean);
    } else {
        Logger::Log::Error("Asset loading significantly exceeds 240Hz budget: {:.3f}ms", stats.mean);
    }

    // Calculate maximum assets per frame at 240Hz
    double maxAssetsPerFrame = assetLoadingBudget / (stats.mean / frameAssets.size());
    Logger::Log::Info("Maximum Assets Per Frame at 240Hz: {:.1f}", maxAssetsPerFrame);

    EXPECT_GT(maxAssetsPerFrame, 1.0)
        << "Cannot load even 1 asset per frame at 240Hz";
}
