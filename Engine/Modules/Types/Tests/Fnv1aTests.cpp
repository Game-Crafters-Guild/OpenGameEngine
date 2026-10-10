#include "Types/Fnv1a.h"
#include "Types/StringId.h"

#include <gtest/gtest.h>

#include <array>
#include <string_view>

namespace
{

using GameEngine::Hashing::Fnv1a64;
using GameEngine::Hashing::kFnv1a64OffsetBasis;

static_assert(Fnv1a64(std::string_view{}) == 0xcbf29ce484222325ull);
static_assert(Fnv1a64("a") == 0xaf63dc4c8601ec8cull);
static_assert(Fnv1a64("foobar") == 0x85944171f73967e8ull);
static_assert(GameEngine::HashStringId("foobar") == 0x85944171f73967e8ull);

TEST(Fnv1aTests, MatchesStandardKnownVectors)
{
    EXPECT_EQ(Fnv1a64(std::string_view{}), 0xcbf29ce484222325ull);
    EXPECT_EQ(Fnv1a64("a"), 0xaf63dc4c8601ec8cull);
    EXPECT_EQ(Fnv1a64("foobar"), 0x85944171f73967e8ull);
}

TEST(Fnv1aTests, ByteHashSupportsChaining)
{
    constexpr std::array<std::uint8_t, 6> bytes{'f', 'o', 'o', 'b', 'a', 'r'};
    const std::uint64_t first = Fnv1a64(bytes.data(), 3, kFnv1a64OffsetBasis);
    const std::uint64_t chained = Fnv1a64(bytes.data() + 3, 3, first);

    EXPECT_EQ(chained, Fnv1a64(bytes.data(), bytes.size(), kFnv1a64OffsetBasis));
    EXPECT_EQ(chained, 0x85944171f73967e8ull);
}

} // namespace
