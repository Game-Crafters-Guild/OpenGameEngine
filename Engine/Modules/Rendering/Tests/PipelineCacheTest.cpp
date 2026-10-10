/**
 * @file VulkanPipelineCacheTest.cpp
 * @brief Unit tests for Pipeline Cache functionality
 */

#include <gtest/gtest.h>
#include "VulkanPipelineCache.h"
#include "TestTempDir.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace GameEngine::Rendering;

class VulkanPipelineCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a mock Vulkan device (VK_NULL_HANDLE for testing)
        m_Device = VK_NULL_HANDLE;
        
        // Use a test-specific cache directory, relative to the working directory every test
        // process of this build shares; the process id keeps a concurrent run's cleanup out of it.
        m_Config.CacheDirectory = "test_cache_" + std::to_string(GameEngine::TestUtils::GetProcessIdForTests()) + "/";
        m_Config.CacheFileName = "test_pipeline_cache.bin";
        
        // Clean up any existing test cache
        CleanupTestCache();
    }

    void TearDown() override {
        CleanupTestCache();
    }

    void CleanupTestCache() {
        try {
            std::filesystem::remove_all(m_Config.CacheDirectory);
        } catch (...) {
            // Ignore cleanup errors
        }
    }

    VkDevice m_Device;
    VulkanPipelineCache::Config m_Config;
};

TEST_F(VulkanPipelineCacheTest, InitializationWithNullDevice) {
    // Test that pipeline cache handles null device gracefully
    VulkanPipelineCache cache(m_Device, m_Config);
    
    // Should not crash, but may not fully initialize without valid device
    // This tests the error handling paths
    bool result = cache.Initialize();
    
    // With null device, initialization should fail gracefully
    EXPECT_FALSE(result);
}

TEST_F(VulkanPipelineCacheTest, ConfigurationSettings) {
    VulkanPipelineCache::Config config;
    config.CacheDirectory = "custom_cache/";
    config.CacheFileName = "custom_cache.bin";
    config.EnableCompression = false;
    config.EnableValidation = false;
    config.MaxCacheSize = 32 * 1024 * 1024; // 32MB
    config.CacheVersion = 2;

    VulkanPipelineCache cache(m_Device, config);
    
    // Verify configuration is stored correctly
    auto cachePath = cache.GetCacheFilePath();
    // `parent_path()` strips the trailing slash, so compare without it.
    EXPECT_EQ(cachePath.parent_path().string(), "custom_cache");
    EXPECT_EQ(cachePath.filename().string(), "custom_cache.bin");
}

TEST_F(VulkanPipelineCacheTest, CacheDirectoryCreation) {
    VulkanPipelineCache cache(m_Device, m_Config);
    
    // Initialize should create the cache directory
    cache.Initialize();
    
    // Verify directory was created
    EXPECT_TRUE(std::filesystem::exists(m_Config.CacheDirectory));
    EXPECT_TRUE(std::filesystem::is_directory(m_Config.CacheDirectory));
}

TEST_F(VulkanPipelineCacheTest, CacheFileOperations) {
    VulkanPipelineCache cache(m_Device, m_Config);
    cache.Initialize();
    
    // Test cache file path generation
    auto cachePath = cache.GetCacheFilePath();
    EXPECT_FALSE(cachePath.empty());
    EXPECT_EQ(cachePath.filename().string(), m_Config.CacheFileName);
    
    // Test cache clearing
    cache.ClearCache();
    EXPECT_FALSE(std::filesystem::exists(cachePath));
}

TEST_F(VulkanPipelineCacheTest, StatisticsTracking) {
    VulkanPipelineCache cache(m_Device, m_Config);
    cache.Initialize();
    
    // Test initial statistics
    auto stats = cache.GetStatistics();
    EXPECT_EQ(stats.TotalPipelines, 0u);
    EXPECT_EQ(stats.CacheHits, 0u);
    EXPECT_EQ(stats.CacheMisses, 0u);
    EXPECT_EQ(stats.CacheSize, 0u);

    // Test recording pipeline creation
    cache.RecordPipelineCreation(5.0, false); // Cache miss
    cache.RecordPipelineCreation(1.0, true);  // Cache hit

    stats = cache.GetStatistics();
    EXPECT_EQ(stats.TotalPipelines, 2u);
    EXPECT_EQ(stats.CacheHits, 1u);
    EXPECT_EQ(stats.CacheMisses, 1u);
    EXPECT_DOUBLE_EQ(stats.AverageCreationTime, 5.0);
    EXPECT_DOUBLE_EQ(stats.AverageCachedCreationTime, 1.0);
}

TEST_F(VulkanPipelineCacheTest, PipelineCreationTimer) {
    VulkanPipelineCache cache(m_Device, m_Config);
    cache.Initialize();
    
    // Test RAII timer
    {
        PipelineCreationTimer timer(&cache, false);
        // Simulate some work
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    auto stats = cache.GetStatistics();
    EXPECT_EQ(stats.TotalPipelines, 1u);
    EXPECT_EQ(stats.CacheMisses, 1u);
    EXPECT_GT(stats.AverageCreationTime, 0.0);
}

TEST_F(VulkanPipelineCacheTest, ValidityChecks) {
    VulkanPipelineCache cache(m_Device, m_Config);
    
    // Before initialization
    EXPECT_FALSE(cache.IsValid());
    
    // After initialization (may still be false with null device)
    cache.Initialize();
    // With null device, cache won't be valid, but this tests the API
}

TEST_F(VulkanPipelineCacheTest, MultipleInstances) {
    // Test that multiple cache instances can coexist
    VulkanPipelineCache::Config config1 = m_Config;
    config1.CacheFileName = "cache1.bin";

    VulkanPipelineCache::Config config2 = m_Config;
    config2.CacheFileName = "cache2.bin";
    
    VulkanPipelineCache cache1(m_Device, config1);
    VulkanPipelineCache cache2(m_Device, config2);
    
    cache1.Initialize();
    cache2.Initialize();
    
    // Both should have different cache files
    EXPECT_NE(cache1.GetCacheFilePath(), cache2.GetCacheFilePath());
}

TEST_F(VulkanPipelineCacheTest, ErrorHandling) {
    // Test with invalid cache directory (read-only or invalid path)
    VulkanPipelineCache::Config invalidConfig = m_Config;
    invalidConfig.CacheDirectory = ""; // Invalid empty directory
    
    VulkanPipelineCache cache(m_Device, invalidConfig);
    
    // Should handle invalid configuration gracefully
    bool result = cache.Initialize();
    EXPECT_FALSE(result);
}

// Integration test that would require a real Vulkan device
// This is commented out as it requires actual Vulkan initialization
/*
TEST_F(VulkanPipelineCacheTest, DISABLED_RealVulkanIntegration) {
    // This test would require:
    // 1. Real Vulkan instance and device creation
    // 2. Actual pipeline creation
    // 3. Cache save/load verification
    // 4. Performance measurement
    
    // Would be run as part of integration test suite
}
*/

// Performance test for cache operations
TEST_F(VulkanPipelineCacheTest, CachePerformance) {
    VulkanPipelineCache cache(m_Device, m_Config);
    cache.Initialize();
    
    // Measure statistics update performance
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < 1000; ++i) {
        cache.RecordPipelineCreation(1.0, i % 2 == 0);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    // Statistics updates should be very fast (< 1ms for 1000 operations)
    EXPECT_LT(duration.count(), 1000);
    
    auto stats = cache.GetStatistics();
    EXPECT_EQ(stats.TotalPipelines, 1000u);
    EXPECT_EQ(stats.CacheHits, 500u);
    EXPECT_EQ(stats.CacheMisses, 500u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
