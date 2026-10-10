#include <gtest/gtest.h>
#include <GenerationalVector/GenerationalVector.hpp>

#include <algorithm>
#include <numeric>
#include <vector>

namespace {

// Heap-owning payload: the container must relocate these by move-construction
// when its storage grows. A bytewise relocation leaves each std::vector's
// debug-iterator proxy pointing into the freed old buffer, so the iterator
// walk below asserts under MSVC debug iterators and reads freed memory
// elsewhere.
struct HeapPayload {
    std::vector<float> Values;

    explicit HeapPayload(int seed = 0) : Values(16) {
        std::iota(Values.begin(), Values.end(), static_cast<float>(seed));
    }
};

// Construction/destruction ledger: every element the container ever
// constructs (including move-relocations during growth) must be destroyed
// exactly once by the time the container dies.
struct BalanceCounted {
    static int Constructed;
    static int Destroyed;

    std::vector<int> Payload; // heap-owning, so leaks are real leaks

    explicit BalanceCounted(int value = 0) : Payload(4, value) { ++Constructed; }
    BalanceCounted(const BalanceCounted& other) : Payload(other.Payload) { ++Constructed; }
    BalanceCounted(BalanceCounted&& other) noexcept : Payload(std::move(other.Payload)) {
        ++Constructed;
    }
    ~BalanceCounted() { ++Destroyed; }
};

int BalanceCounted::Constructed = 0;
int BalanceCounted::Destroyed = 0;

} // namespace

TEST(GenerationalVectorLifetime, GrowthRelocatesHeapOwningElementsByMove) {
    GenerationalVector::GenerationalVector<HeapPayload> container;

    std::vector<GenerationalVector::Handle> handles;
    for (int i = 0; i < 9; ++i) {
        handles.push_back(container.Create(HeapPayload(i * 100)));
    }

    for (int i = 0; i < 9; ++i) {
        const HeapPayload* payload = container.Get(handles[static_cast<size_t>(i)]);
        ASSERT_NE(payload, nullptr);
        // Iterator-based walk on purpose: begin()/end() comparison is what
        // MSVC debug iterators validate against the owning container.
        const auto found =
            std::find(payload->Values.begin(), payload->Values.end(), static_cast<float>(i * 100));
        ASSERT_NE(found, payload->Values.end());
        EXPECT_EQ(found, payload->Values.begin());
        EXPECT_FLOAT_EQ(payload->Values.back(), static_cast<float>(i * 100 + 15));
    }
}

TEST(GenerationalVectorLifetime, ForEachVisitsRecycledSlots) {
    GenerationalVector::GenerationalVector<HeapPayload> container;

    const auto first = container.Create(HeapPayload(1000));
    const auto second = container.Create(HeapPayload(2000));
    container.Destroy(first);
    const auto recycled = container.Create(HeapPayload(3000)); // first's slot, generation >= 2

    ASSERT_EQ(recycled.Index(), first.Index());
    ASSERT_GT(recycled.Generation(), first.Generation());

    std::vector<float> visited;
    std::vector<GenerationalVector::Handle> visitedHandles;
    container.ForEach([&](GenerationalVector::Handle handle, const HeapPayload& payload) {
        visitedHandles.push_back(handle);
        visited.push_back(payload.Values.front());
    });

    ASSERT_EQ(visited.size(), 2u);
    EXPECT_NE(std::find(visited.begin(), visited.end(), 2000.0f), visited.end());
    EXPECT_NE(std::find(visited.begin(), visited.end(), 3000.0f), visited.end());
    // The handle passed to the visitor must be the slot's LIVE handle — usable
    // for a later Get — not a guessed generation.
    EXPECT_NE(std::find(visitedHandles.begin(), visitedHandles.end(), recycled),
              visitedHandles.end());
    EXPECT_NE(std::find(visitedHandles.begin(), visitedHandles.end(), second),
              visitedHandles.end());
}

TEST(GenerationalVectorLifetime, DestructionBalancesConstructionAcrossGrowthAndRecycling) {
    BalanceCounted::Constructed = 0;
    BalanceCounted::Destroyed = 0;
    {
        GenerationalVector::GenerationalVector<BalanceCounted> container;
        std::vector<GenerationalVector::Handle> handles;
        for (int i = 0; i < 6; ++i) {
            handles.push_back(container.Create(BalanceCounted(i)));
        }
        container.Destroy(handles[1]);
        container.Create(BalanceCounted(42)); // recycled slot
        container.Destroy(handles[4]);
        // The container dies with five live elements, one of them in a
        // recycled slot; its destructor must destroy every one of them.
    }
    EXPECT_EQ(BalanceCounted::Constructed, BalanceCounted::Destroyed)
        << "every element the container constructed (including move-relocations "
           "during growth) must be destroyed exactly once";
    EXPECT_GT(BalanceCounted::Constructed, 0);
}

TEST(GenerationalVectorLifetime, ClearDestroysRecycledSlotElements) {
    BalanceCounted::Constructed = 0;
    BalanceCounted::Destroyed = 0;

    GenerationalVector::GenerationalVector<BalanceCounted> container;
    const auto a = container.Create(BalanceCounted(1));
    container.Create(BalanceCounted(2));
    container.Destroy(a);
    container.Create(BalanceCounted(3)); // recycled slot, generation >= 2

    container.Clear();
    EXPECT_EQ(container.Size(), 0u);
    EXPECT_EQ(BalanceCounted::Constructed, BalanceCounted::Destroyed);
}
