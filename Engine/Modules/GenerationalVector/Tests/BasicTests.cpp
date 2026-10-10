#include <gtest/gtest.h>
#include <GenerationalVector/GenerationalVector.hpp>
#include <string>
#include <vector>
#include <algorithm>

// Simple test to verify the library compiles and basic functionality works
TEST(GenerationalVectorBasicTest, HandleCreationAndValidation) {
    // Test handle creation
    GenerationalVector::Handle handle1;
    EXPECT_FALSE(handle1.IsValid());

    GenerationalVector::Handle handle2(1, 1);
    EXPECT_TRUE(handle2.IsValid());
    EXPECT_EQ(handle2.Index(), 1u);
    EXPECT_EQ(handle2.Generation(), 1u);
}

TEST(GenerationalVectorBasicTest, BasicVectorOperations) {
    // Simple test data
    struct TestData {
        int value;
        TestData(int v = 0) : value(v) {}
    };

    GenerationalVector::GenerationalVector<TestData> container;

    // Test empty state
    EXPECT_TRUE(container.Empty());
    EXPECT_EQ(container.Size(), 0u);

    // Create an element
    auto handle = container.Create(42);
    EXPECT_TRUE(handle.IsValid());
    EXPECT_FALSE(container.Empty());
    EXPECT_EQ(container.Size(), 1u);

    // Access the element
    EXPECT_TRUE(container.IsValid(handle));
    EXPECT_EQ(container[handle].value, 42);

    // Destroy the element
    container.Destroy(handle);
    EXPECT_FALSE(container.IsValid(handle));
    EXPECT_TRUE(container.Empty());
    EXPECT_EQ(container.Size(), 0u);
}

TEST(GenerationalVectorBasicTest, HandleReuse) {
    struct TestData {
        int value;
        TestData(int v = 0) : value(v) {}
    };

    GenerationalVector::GenerationalVector<TestData> container;

    // Create and destroy an element
    auto handle1 = container.Create(1);
    auto index1 = handle1.Index();
    auto gen1 = handle1.Generation();

    container.Destroy(handle1);
    EXPECT_FALSE(container.IsValid(handle1));

    // Create another element (should reuse the slot)
    auto handle2 = container.Create(2);
    auto index2 = handle2.Index();
    auto gen2 = handle2.Generation();

    // Should reuse the same index but with incremented generation
    EXPECT_EQ(index1, index2);
    EXPECT_GT(gen2, gen1);

    // Old handle should still be invalid
    EXPECT_FALSE(container.IsValid(handle1));
    EXPECT_TRUE(container.IsValid(handle2));
}

namespace {

struct TestData {
    int value;
    TestData(int v = 0) : value(v) {}
};

std::vector<int> CollectValues(const GenerationalVector::GenerationalVector<TestData>& container) {
    std::vector<int> values;
    container.ForEach([&](GenerationalVector::Handle, const TestData& element) {
        values.push_back(element.value);
    });
    std::sort(values.begin(), values.end());
    return values;
}

} // namespace

TEST(GenerationalVectorForEachTest, VisitsElementInRecycledSlot) {
    GenerationalVector::GenerationalVector<TestData> container;

    container.Create(10);
    auto middle = container.Create(20);
    container.Create(30);

    // Freeing a slot advances its generation; the replacement lives in the same
    // index at a higher generation and must still be iterated.
    container.Destroy(middle);
    auto recycled = container.Create(99);
    EXPECT_GT(recycled.Generation(), 1u);

    EXPECT_EQ(container.Size(), 3u);
    EXPECT_EQ(CollectValues(container), (std::vector<int>{10, 30, 99}));
}

TEST(GenerationalVectorForEachTest, VisitsEveryElementAfterRepeatedRecycling) {
    GenerationalVector::GenerationalVector<TestData> container;

    std::vector<GenerationalVector::Handle> handles;
    for (int i = 0; i < 8; ++i) {
        handles.push_back(container.Create(i));
    }

    // Churn each slot several times so generations diverge across the container.
    for (int round = 0; round < 3; ++round) {
        for (auto& handle : handles) {
            container.Destroy(handle);
            handle = container.Create(static_cast<int>(handle.Index()) * 100 + round);
        }
    }

    size_t visited = 0;
    container.ForEach([&](GenerationalVector::Handle handle, TestData&) {
        EXPECT_TRUE(container.IsValid(handle));
        ++visited;
    });
    EXPECT_EQ(visited, container.Size());
    EXPECT_EQ(visited, handles.size());
}

TEST(GenerationalVectorForEachTest, YieldsHandlesThatResolveToTheVisitedElement) {
    GenerationalVector::GenerationalVector<TestData> container;

    auto first = container.Create(1);
    container.Create(2);
    container.Destroy(first);
    container.Create(3);

    container.ForEach([&](GenerationalVector::Handle handle, TestData& element) {
        ASSERT_TRUE(container.IsValid(handle));
        EXPECT_EQ(&container[handle], &element);
    });
}

TEST(GenerationalVectorForEachTest, ConstAndMutableOverloadsAgree) {
    GenerationalVector::GenerationalVector<TestData> container;

    auto first = container.Create(1);
    container.Create(2);
    container.Destroy(first);
    container.Create(3);

    std::vector<int> mutableValues;
    container.ForEach([&](GenerationalVector::Handle, TestData& element) {
        mutableValues.push_back(element.value);
    });
    std::sort(mutableValues.begin(), mutableValues.end());

    EXPECT_EQ(mutableValues, CollectValues(container));
    EXPECT_EQ(mutableValues.size(), container.Size());
}

TEST(GenerationalVectorForEachTest, ClearDestroysElementsInRecycledSlots) {
    static int liveCount = 0;
    struct Tracked {
        Tracked() noexcept { ++liveCount; }
        Tracked(const Tracked&) noexcept { ++liveCount; }
        ~Tracked() { --liveCount; }
    };

    liveCount = 0;
    {
        GenerationalVector::GenerationalVector<Tracked> container;

        auto first = container.Create(Tracked{});
        container.Create(Tracked{});
        container.Destroy(first);
        container.Create(Tracked{});

        ASSERT_EQ(liveCount, static_cast<int>(container.Size()));

        // Clear() runs destructors through ForEach, so a skipped recycled slot
        // would leak its element.
        container.Clear();
        EXPECT_EQ(liveCount, 0);
    }
    EXPECT_EQ(liveCount, 0);
}
