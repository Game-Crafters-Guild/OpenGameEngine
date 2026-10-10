#include "Types/FalseSharing.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace
{

using GameEngine::FalseSharingPadded;
using GameEngine::kFalseSharingSeparation;

static_assert(kFalseSharingSeparation == 128, "128 covers the Apple Silicon line and the x86 adjacent-line pair");

static_assert(alignof(FalseSharingPadded<std::atomic<std::uint64_t>>) == 128);
static_assert(sizeof(FalseSharingPadded<std::atomic<std::uint64_t>>) == 128);
static_assert(alignof(FalseSharingPadded<char>) == 128);
static_assert(sizeof(FalseSharingPadded<char>) == 128);
static_assert(alignof(FalseSharingPadded<std::mutex>) == 128);
static_assert(sizeof(FalseSharingPadded<std::mutex>) == 128);
static_assert(alignof(FalseSharingPadded<std::condition_variable>) == 128);
static_assert(sizeof(FalseSharingPadded<std::condition_variable>) == 128);
static_assert(sizeof(FalseSharingPadded<std::array<char, 129>>) == 256);

std::uintptr_t AddressOf(const void* pointer)
{
    return reinterpret_cast<std::uintptr_t>(pointer);
}

TEST(FalseSharingTests, ArrayElementsStartOneSeparationApart)
{
    std::array<FalseSharingPadded<std::atomic<std::uint32_t>>, 4> counters{};

    for (std::size_t index = 0; index < counters.size(); ++index)
    {
        EXPECT_EQ(AddressOf(&counters[index].Value) % kFalseSharingSeparation, 0u) << "element " << index;
    }
    EXPECT_EQ(AddressOf(&counters[1].Value) - AddressOf(&counters[0].Value), kFalseSharingSeparation);
}

TEST(FalseSharingTests, HeapStorageKeepsTheSeparationAlignment)
{
    const auto single = std::make_unique<FalseSharingPadded<std::mutex>>();
    EXPECT_EQ(AddressOf(single.get()) % kFalseSharingSeparation, 0u);

    std::vector<FalseSharingPadded<std::uint64_t>> slots(3);
    EXPECT_EQ(AddressOf(slots.data()) % kFalseSharingSeparation, 0u);
}

TEST(FalseSharingTests, ValueIsValueInitialisedAndAggregateInitialisable)
{
    const FalseSharingPadded<std::uint64_t> zeroed;
    EXPECT_EQ(zeroed.Value, 0u);

    FalseSharingPadded<std::atomic<std::uint32_t>> counter{7};
    EXPECT_EQ(counter.Value.load(std::memory_order_relaxed), 7u);
}

} // namespace
