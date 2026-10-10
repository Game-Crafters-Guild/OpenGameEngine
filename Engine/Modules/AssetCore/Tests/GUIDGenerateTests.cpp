// Tests for GUID::Generate — the random (RFC 4122 version 4) identity that asset
// registration mints from scan workers and that editor tools mint on the main
// thread, concurrently.

#include "AssetCore/GUID.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <latch>
#include <thread>
#include <vector>

namespace {

using GameEngine::GUID;

bool HasVersionFourLayout(const GUID& guid)
{
    const auto& bytes = guid.GetData();
    return !guid.IsNull() && (bytes[6] & 0xF0) == 0x40 && (bytes[8] & 0xC0) == 0x80;
}

size_t CountDuplicates(std::vector<GUID>& guids)
{
    std::sort(guids.begin(), guids.end());
    size_t duplicates = 0;
    for (size_t i = 1; i < guids.size(); ++i)
    {
        duplicates += guids[i] == guids[i - 1];
    }
    return duplicates;
}

TEST(GUIDGenerate, HasVersionFourLayoutAndRoundTripsThroughText)
{
    constexpr size_t kSampleCount = 1024;
    std::vector<GUID> guids(kSampleCount);
    for (GUID& guid : guids)
    {
        guid = GUID::Generate();
        ASSERT_TRUE(HasVersionFourLayout(guid)) << guid.ToString();
        EXPECT_EQ(GUID(guid.ToString()), guid);
    }
    EXPECT_EQ(CountDuplicates(guids), 0u);
}

// Eight threads start together and each mints a batch; every run therefore
// covers concurrent first use (per-thread seeding) as well as steady-state
// draws. With 122 random bits per GUID the chance of an honest collision in
// 2^17 draws is about 2^-89, so a duplicate means shared or identically seeded
// generator state: an unsynchronized shared generator produced 7,000-10,000
// duplicates at this size, and identical per-thread seeds would repeat every
// GUID once per thread.
TEST(GUIDGenerate, ConcurrentCallsProduceDistinctGuids)
{
    constexpr size_t kThreadCount = 8;
    constexpr size_t kGuidsPerThread = 16384;

    std::vector<std::vector<GUID>> batches(kThreadCount, std::vector<GUID>(kGuidsPerThread));
    std::latch ready(kThreadCount);
    std::latch start(1);
    std::vector<std::thread> workers;
    workers.reserve(kThreadCount);
    for (size_t thread = 0; thread < kThreadCount; ++thread)
    {
        workers.emplace_back([&, thread] {
            ready.count_down();
            start.wait();
            for (GUID& guid : batches[thread])
            {
                guid = GUID::Generate();
            }
        });
    }
    ready.wait();
    start.count_down();
    for (std::thread& worker : workers)
    {
        worker.join();
    }

    std::vector<GUID> all;
    all.reserve(kThreadCount * kGuidsPerThread);
    size_t invalid = 0;
    for (const auto& batch : batches)
    {
        for (const GUID& guid : batch)
        {
            invalid += !HasVersionFourLayout(guid);
            all.push_back(guid);
        }
    }
    EXPECT_EQ(invalid, 0u);
    EXPECT_EQ(CountDuplicates(all), 0u);
}

} // namespace
