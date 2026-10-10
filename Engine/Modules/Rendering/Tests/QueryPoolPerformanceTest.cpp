/**
 * @file QueryPoolPerformanceTest.cpp
 * @brief Comprehensive performance testing for QueryPool system
 */

// Every test in this file is DISABLED_, so the RenderingQueryPoolPerformanceTests
// target reports green having run nothing and gates no behaviour. The query
// pool's contracts are pinned by MetalBackendHeadlessTests (MetalTimestampSpans)
// and RGProfilingTests.

#include <gtest/gtest.h>
#include "Rendering/Core/QueryPool.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include <chrono>
#include <thread>
#include <vector>
#include <iostream>
#include <iomanip>

using namespace GameEngine::Rendering;

class QueryPoolPerformanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create device via factory (Vulkan backend)
        DeviceDesc desc{};
        desc.preferredAPI = GraphicsAPI::Vulkan;
        m_Device = DeviceFactory::CreateDevice(desc);
        ASSERT_NE(m_Device, nullptr);
        ASSERT_TRUE(m_Device->Initialize(desc));

        m_QueryPool = m_Device->GetQueryPool();
        ASSERT_NE(m_QueryPool, nullptr);
        ASSERT_TRUE(m_QueryPool->IsValid());
    }

    void TearDown() override {
        m_QueryPool = nullptr;
        m_Device.reset();
    }

    std::unique_ptr<IDevice> m_Device;
    IQueryPool* m_QueryPool = nullptr;
};

/**
 * @brief Test GPU timing accuracy by comparing with CPU timing
 */
TEST_F(QueryPoolPerformanceTest, DISABLED_TimingAccuracy) {
    if (!m_QueryPool) {
        GTEST_SKIP() << "QueryPool not available for testing";
    }

    auto commandList = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_NE(commandList, nullptr);

    commandList->Begin();

    // Test multiple timing scenarios
    struct TimingTest {
        std::string name;
        std::chrono::microseconds expectedDuration;
        std::function<void()> workload;
    };

    std::vector<TimingTest> tests = {
        {"ShortWorkload", std::chrono::microseconds(100), []() {
            // Simulate short GPU work
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }},
        {"MediumWorkload", std::chrono::microseconds(1000), []() {
            // Simulate medium GPU work
            std::this_thread::sleep_for(std::chrono::microseconds(1000));
        }},
        {"LongWorkload", std::chrono::microseconds(5000), []() {
            // Simulate long GPU work
            std::this_thread::sleep_for(std::chrono::microseconds(5000));
        }}
    };

    for (const auto& test : tests) {
        std::cout << "\n=== Testing " << test.name << " ===" << std::endl;

        // CPU timing
        auto cpuStart = std::chrono::high_resolution_clock::now();
        
        // GPU timing (query IDs unused: we validate CPU-side timing only in this test)
        (void)m_QueryPool->WriteTimestamp(commandList.get(), TimestampPoint::SpanBegin, test.name + "_Start");

        // Execute workload
        test.workload();

        (void)m_QueryPool->WriteTimestamp(commandList.get(), TimestampPoint::SpanEnd, test.name + "_End");
        auto cpuEnd = std::chrono::high_resolution_clock::now();

        // Calculate CPU duration
        auto cpuDuration = std::chrono::duration_cast<std::chrono::microseconds>(cpuEnd - cpuStart);

        std::cout << "CPU Duration: " << cpuDuration.count() << " μs" << std::endl;
        std::cout << "Expected: " << test.expectedDuration.count() << " μs" << std::endl;

        // Verify timing accuracy (within 10% tolerance)
        double tolerance = 0.1;
        double expectedUs = static_cast<double>(test.expectedDuration.count());
        double actualUs = static_cast<double>(cpuDuration.count());
        double error = std::abs(actualUs - expectedUs) / expectedUs;

        EXPECT_LT(error, tolerance) << "Timing error too large: " << (error * 100) << "%";
    }

    commandList->End();
}

/**
 * @brief Benchmark query overhead in complex scenarios
 */
TEST_F(QueryPoolPerformanceTest, DISABLED_QueryOverheadBenchmark) {
    if (!m_QueryPool) {
        GTEST_SKIP() << "QueryPool not available for testing";
    }

    auto commandList = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_NE(commandList, nullptr);

    // Test different query densities
    std::vector<uint32_t> queryCounts = {1, 10, 50, 100, 500, 1000};

    for (uint32_t queryCount : queryCounts) {
        std::cout << "\n=== Benchmarking " << queryCount << " queries ===" << std::endl;

        commandList->Begin();

        // Baseline: measure time without queries
        auto baselineStart = std::chrono::high_resolution_clock::now();
        
        // Simulate rendering work without queries
        for (uint32_t i = 0; i < queryCount; ++i) {
            // Simulate minimal GPU work
            commandList->SetMarker(("Work_" + std::to_string(i)).c_str());
        }
        
        auto baselineEnd = std::chrono::high_resolution_clock::now();
        auto baselineDuration = std::chrono::duration_cast<std::chrono::microseconds>(baselineEnd - baselineStart);

        // Test: measure time with queries
        auto testStart = std::chrono::high_resolution_clock::now();
        
        std::vector<uint32_t> queryIndices;
        queryIndices.reserve(queryCount);

        for (uint32_t i = 0; i < queryCount; ++i) {
            uint32_t queryIndex = m_QueryPool->WriteTimestamp(commandList.get(), TimestampPoint::SpanBegin, "Query_" + std::to_string(i));
            queryIndices.push_back(queryIndex);
            
            // Simulate minimal GPU work
            commandList->SetMarker(("Work_" + std::to_string(i)).c_str());
        }
        
        auto testEnd = std::chrono::high_resolution_clock::now();
        auto testDuration = std::chrono::duration_cast<std::chrono::microseconds>(testEnd - testStart);

        commandList->End();

        // Calculate overhead
        auto overhead = testDuration - baselineDuration;
        double overheadPerQuery = static_cast<double>(overhead.count()) / queryCount;

        std::cout << "Baseline: " << baselineDuration.count() << " μs" << std::endl;
        std::cout << "With queries: " << testDuration.count() << " μs" << std::endl;
        std::cout << "Overhead: " << overhead.count() << " μs" << std::endl;
        std::cout << "Per query: " << std::fixed << std::setprecision(2) << overheadPerQuery << " μs" << std::endl;

        // Verify overhead is reasonable (< 1μs per query)
        EXPECT_LT(overheadPerQuery, 1.0) << "Query overhead too high: " << overheadPerQuery << " μs per query";
    }
}

/**
 * @brief Test frame time impact of different query types
 */
TEST_F(QueryPoolPerformanceTest, DISABLED_QueryTypeImpactTest) {
    if (!m_QueryPool) {
        GTEST_SKIP() << "QueryPool not available for testing";
    }

    auto commandList = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_NE(commandList, nullptr);

    struct QueryTypeTest {
        std::string name;
        std::function<uint32_t()> startQuery;
        std::function<void(uint32_t)> endQuery;
    };

    std::vector<QueryTypeTest> queryTypes = {
        {"Timestamp", 
         [&]() { return m_QueryPool->WriteTimestamp(commandList.get(), TimestampPoint::SpanBegin, "TimestampTest"); },
         [&](uint32_t) { /* Timestamps don't need end */ }
        },
        {"Occlusion",
         [&]() { return m_QueryPool->BeginOcclusionQuery(commandList.get(), "OcclusionTest"); },
         [&](uint32_t idx) { m_QueryPool->EndOcclusionQuery(commandList.get(), idx); }
        },
        {"PipelineStats",
         [&]() { return m_QueryPool->BeginPipelineStats(commandList.get(), "PipelineStatsTest"); },
         [&](uint32_t idx) { m_QueryPool->EndPipelineStats(commandList.get(), idx); }
        }
    };

    const uint32_t queriesPerType = 100;

    for (const auto& queryType : queryTypes) {
        std::cout << "\n=== Testing " << queryType.name << " queries ===" << std::endl;

        commandList->Begin();

        auto start = std::chrono::high_resolution_clock::now();

        for (uint32_t i = 0; i < queriesPerType; ++i) {
            uint32_t queryIndex = queryType.startQuery();
            
            // Simulate some GPU work
            commandList->SetMarker(("Work_" + std::to_string(i)).c_str());
            
            queryType.endQuery(queryIndex);
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

        commandList->End();

        double timePerQuery = static_cast<double>(duration.count()) / queriesPerType;

        std::cout << "Total time: " << duration.count() << " μs" << std::endl;
        std::cout << "Per query: " << std::fixed << std::setprecision(2) << timePerQuery << " μs" << std::endl;

        // Verify reasonable performance
        EXPECT_LT(timePerQuery, 2.0) << queryType.name << " queries too slow: " << timePerQuery << " μs per query";
    }
}

/**
 * @brief Stress test with maximum query load
 */
TEST_F(QueryPoolPerformanceTest, DISABLED_MaximumLoadStressTest) {
    if (!m_QueryPool) {
        GTEST_SKIP() << "QueryPool not available for testing";
    }

    auto commandList = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_NE(commandList, nullptr);

    commandList->Begin();

    std::cout << "\n=== Maximum Load Stress Test ===" << std::endl;

    // Try to use maximum number of queries
    const uint32_t maxQueries = 1000; // Should be less than maxTimestampQueries
    
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<uint32_t> queryIndices;
    queryIndices.reserve(maxQueries);

    for (uint32_t i = 0; i < maxQueries; ++i) {
        uint32_t queryIndex = m_QueryPool->WriteTimestamp(commandList.get(), TimestampPoint::SpanBegin, "StressTest_" + std::to_string(i));
        queryIndices.push_back(queryIndex);

        // Verify query was created successfully
        EXPECT_NE(queryIndex, UINT32_MAX) << "Failed to create query " << i;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    commandList->End();

    std::cout << "Created " << queryIndices.size() << " queries in " << duration.count() << " μs" << std::endl;
    std::cout << "Average: " << std::fixed << std::setprecision(2) 
              << (static_cast<double>(duration.count()) / queryIndices.size()) << " μs per query" << std::endl;

    // Verify all queries were created
    EXPECT_EQ(queryIndices.size(), maxQueries);
    
    // Verify no query creation failed
    for (uint32_t queryIndex : queryIndices) {
        EXPECT_NE(queryIndex, UINT32_MAX);
    }
}
