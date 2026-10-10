#include <gtest/gtest.h>
#include <GenerationalVector/GenerationalVector.hpp>
#include <chrono>
#include <vector>

// Simple performance test
TEST(GenerationalVectorPerformanceTest, BasicPerformance) {
    struct TestData {
        int value;
        TestData(int v = 0) : value(v) {}
    };

    GenerationalVector::GenerationalVector<TestData> container;
    const size_t TEST_SIZE = 10000;

    // Test creation performance
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<GenerationalVector::Handle> handles;
    handles.reserve(TEST_SIZE);

    for (size_t i = 0; i < TEST_SIZE; ++i) {
        handles.push_back(container.Create(static_cast<int>(i)));
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    EXPECT_EQ(container.Size(), TEST_SIZE);

    // Should be reasonably fast
    EXPECT_LT(duration.count(), 100); // Less than 100ms for 10k elements

    std::cout << "Created " << TEST_SIZE << " elements in " << duration.count() << "ms\n";
}
