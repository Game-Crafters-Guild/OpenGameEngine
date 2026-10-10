// The archetype match a Query<Ts...> derives from its type list and its With<>/Without<>
// filters: which components an archetype must have, must lack, and may lack.
#include <gtest/gtest.h>
#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/ECSTemplates.h"
#include "DeathTestChild.h"
#include "TestComponents.h"

using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;

namespace
{
// a: Position; b: Position+Velocity; c: Position+Velocity+Health; d: Position+Velocity+Health+Temporary.
void PopulateSignatureFixture(World& w)
{
    auto a = w.Create(); a.Set(Position{1, 2, 3});
    auto b = w.Create(); b.Set(Position{4, 5, 6}); b.Set(Velocity{1, 0, 0});
    auto c = w.Create(); c.Set(Position{7, 8, 9}); c.Set(Velocity{0, 1, 0}); c.Set(Health{10, 10});
    auto d = w.Create(); d.Set(Position{0, 0, 0}); d.Set(Velocity{0, 0, 1}); d.Set(Health{5, 5}); d.Set(Temporary{0.5f});
    w.ProcessCommands();
}

template <typename QueryT>
std::size_t CountMatches(QueryT& query)
{
    return query.Count();
}
} // namespace

// Read<> and Write<> types are both required; Optional<> types are not.
TEST(QuerySignatureTest, WriteQualifiedTypesAreRequiredLikeReadOnes)
{
    World w(nullptr);
    PopulateSignatureFixture(w);

    EXPECT_EQ((w.Query<Read<Position>, Write<Velocity>>().Count()), 3u);
    EXPECT_EQ((w.Query<Write<Position>, Read<Velocity>, Write<Health>>().Count()), 2u);
    EXPECT_EQ((w.Query<Read<Position>, Optional<Velocity>>().Count()), 4u);
}

TEST(QuerySignatureTest, WithIncludesExtraRequiredComponents)
{
    World w(nullptr);
    PopulateSignatureFixture(w);

    std::size_t count = 0;
    w.Query<Read<Position>, Read<Velocity>>().With<Health>().Each(
        [&](EntityHandle, const Position&, const Velocity&) { ++count; });
    EXPECT_EQ(count, 2u);
}

TEST(QuerySignatureTest, WithNamingTwoTypesRequiresBoth)
{
    World w(nullptr);
    PopulateSignatureFixture(w);

    EXPECT_EQ((w.Query<Read<Position>>().With<Health, Temporary>().Count()), 1u);
}

TEST(QuerySignatureTest, WithAndWithoutCombine)
{
    World w(nullptr);
    PopulateSignatureFixture(w);

    EXPECT_EQ((w.Query<Read<Position>, Read<Velocity>>().With<Health>().Without<Temporary>().Count()), 1u);
}

// A retained query re-applies its filter every frame; naming the same pack again is the
// same filter and stays allowed.
TEST(QuerySignatureTest, ReapplyingTheSamePackKeepsTheFilter)
{
    World w(nullptr);
    PopulateSignatureFixture(w);

    Query<Read<Position>, Read<Velocity>> included(&w);
    included.With<Health>();
    EXPECT_EQ(CountMatches(included), 2u);
    included.With<Health>();
    EXPECT_EQ(CountMatches(included), 2u);

    Query<Read<Position>, Read<Velocity>> excluded(&w);
    excluded.Without<Temporary>();
    EXPECT_EQ(CountMatches(excluded), 2u);
    excluded.Without<Temporary>();
    EXPECT_EQ(CountMatches(excluded), 2u);
}

#if !defined(NDEBUG)
// A second With<> or Without<> naming another pack replaces the filter rather than adding
// to it, so the first pack would silently stop applying; dev builds assert instead.
TEST(QuerySignatureTest, SecondWithNamingAnotherPackAssertsInDevBuilds)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            SuppressCrtDialogsInDeathTestChild();
            World w(nullptr);
            Query<Read<Position>> query(&w);
            query.With<Health>();
            query.With<Temporary>();
        },
        "name every included type in one call");
}

TEST(QuerySignatureTest, SecondWithoutNamingAnotherPackAssertsInDevBuilds)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            SuppressCrtDialogsInDeathTestChild();
            World w(nullptr);
            Query<Read<Position>> query(&w);
            query.Without<Temporary>();
            query.Without<Health>();
        },
        "name every excluded type in one call");
}
#endif
