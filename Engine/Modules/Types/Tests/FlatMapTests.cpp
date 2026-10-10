#include "Types/FlatMap.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using GameEngine::FlatMap;

TEST(FlatMapTests, EmptyOnConstruction)
{
    FlatMap<int, std::string> map;
    EXPECT_TRUE(map.Empty());
    EXPECT_EQ(map.Size(), 0u);
}

TEST(FlatMapTests, InsertAndFind)
{
    FlatMap<int, std::string> map;
    map.InsertOrAssign(3, "three");
    map.InsertOrAssign(1, "one");
    map.InsertOrAssign(2, "two");

    EXPECT_EQ(map.Size(), 3u);

    const std::string* v = map.Find(2);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, "two");

    EXPECT_EQ(map.Find(99), nullptr);
}

TEST(FlatMapTests, InsertOrAssignOverwrites)
{
    FlatMap<int, int> map;
    map.InsertOrAssign(5, 100);
    EXPECT_EQ(*map.Find(5), 100);

    map.InsertOrAssign(5, 200);
    EXPECT_EQ(map.Size(), 1u);
    EXPECT_EQ(*map.Find(5), 200);
}

TEST(FlatMapTests, GetOrInsertDefaultConstructs)
{
    FlatMap<int, int> map;
    int& ref = map.GetOrInsert(7);
    EXPECT_EQ(ref, 0);
    ref = 42;
    EXPECT_EQ(*map.Find(7), 42);
}

TEST(FlatMapTests, GetOrInsertExisting)
{
    FlatMap<int, int> map;
    map.InsertOrAssign(3, 99);
    int& ref = map.GetOrInsert(3);
    EXPECT_EQ(ref, 99);
}

TEST(FlatMapTests, Contains)
{
    FlatMap<int, int> map;
    map.InsertOrAssign(1, 10);
    EXPECT_TRUE(map.Contains(1));
    EXPECT_FALSE(map.Contains(2));
}

TEST(FlatMapTests, Erase)
{
    FlatMap<int, std::string> map;
    map.InsertOrAssign(1, "a");
    map.InsertOrAssign(2, "b");
    map.InsertOrAssign(3, "c");

    EXPECT_TRUE(map.Erase(2));
    EXPECT_EQ(map.Size(), 2u);
    EXPECT_EQ(map.Find(2), nullptr);
    EXPECT_NE(map.Find(1), nullptr);
    EXPECT_NE(map.Find(3), nullptr);
}

TEST(FlatMapTests, EraseNonExistent)
{
    FlatMap<int, int> map;
    map.InsertOrAssign(1, 10);
    EXPECT_FALSE(map.Erase(99));
    EXPECT_EQ(map.Size(), 1u);
}

TEST(FlatMapTests, ClearResetsSize)
{
    FlatMap<int, int> map;
    map.InsertOrAssign(1, 10);
    map.InsertOrAssign(2, 20);
    map.Clear();
    EXPECT_TRUE(map.Empty());
    EXPECT_EQ(map.Size(), 0u);
}

TEST(FlatMapTests, IterationIsSorted)
{
    FlatMap<int, std::string> map;
    map.InsertOrAssign(5, "five");
    map.InsertOrAssign(1, "one");
    map.InsertOrAssign(3, "three");

    std::vector<int> keys;
    for (const auto& [k, v] : map)
        keys.push_back(k);

    ASSERT_EQ(keys.size(), 3u);
    EXPECT_EQ(keys[0], 1);
    EXPECT_EQ(keys[1], 3);
    EXPECT_EQ(keys[2], 5);
}

TEST(FlatMapTests, ForEachVisitsAll)
{
    FlatMap<int, int> map;
    map.InsertOrAssign(10, 100);
    map.InsertOrAssign(20, 200);
    map.InsertOrAssign(30, 300);

    int sum = 0;
    map.ForEach([&](int /*key*/, int v) { sum += v; });
    EXPECT_EQ(sum, 600);
}

TEST(FlatMapTests, ConstFind)
{
    FlatMap<int, int> map;
    map.InsertOrAssign(1, 42);

    const auto& cmap = map;
    const int* v = cmap.Find(1);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, 42);
    EXPECT_EQ(cmap.Find(2), nullptr);
}

TEST(FlatMapTests, ReserveDoesNotChangeSize)
{
    FlatMap<int, int> map;
    map.Reserve(100);
    EXPECT_TRUE(map.Empty());
}

TEST(FlatMapTests, StringKeys)
{
    FlatMap<std::string, int> map;
    map.InsertOrAssign("beta", 2);
    map.InsertOrAssign("alpha", 1);
    map.InsertOrAssign("gamma", 3);

    EXPECT_EQ(*map.Find("alpha"), 1);
    EXPECT_EQ(*map.Find("beta"), 2);
    EXPECT_EQ(*map.Find("gamma"), 3);

    std::vector<std::string> keys;
    for (const auto& [k, v] : map)
        keys.push_back(k);
    EXPECT_EQ(keys[0], "alpha");
    EXPECT_EQ(keys[1], "beta");
    EXPECT_EQ(keys[2], "gamma");
}

TEST(FlatMapTests, LargeInsertionStaysSorted)
{
    FlatMap<int, int> map;
    for (int i = 100; i >= 0; --i)
        map.InsertOrAssign(i, i * 10);

    EXPECT_EQ(map.Size(), 101u);

    int prev = -1;
    for (const auto& [k, v] : map)
    {
        EXPECT_GT(k, prev);
        EXPECT_EQ(v, k * 10);
        prev = k;
    }
}

TEST(FlatMapTests, EraseAllLeavesEmpty)
{
    FlatMap<int, int> map;
    map.InsertOrAssign(1, 10);
    map.InsertOrAssign(2, 20);
    map.InsertOrAssign(3, 30);

    EXPECT_TRUE(map.Erase(2));
    EXPECT_TRUE(map.Erase(1));
    EXPECT_TRUE(map.Erase(3));
    EXPECT_TRUE(map.Empty());
    EXPECT_EQ(map.Find(1), nullptr);
}

TEST(FlatMapTests, InsertAfterEraseReusesSortPosition)
{
    FlatMap<int, int> map;
    map.InsertOrAssign(1, 10);
    map.InsertOrAssign(3, 30);
    map.InsertOrAssign(5, 50);
    EXPECT_TRUE(map.Erase(3));

    map.InsertOrAssign(3, 99);
    EXPECT_EQ(map.Size(), 3u);
    EXPECT_EQ(*map.Find(3), 99);

    std::vector<int> keys;
    for (const auto& [k, v] : map)
        keys.push_back(k);
    ASSERT_EQ(keys.size(), 3u);
    EXPECT_EQ(keys[0], 1);
    EXPECT_EQ(keys[1], 3);
    EXPECT_EQ(keys[2], 5);
}

TEST(FlatMapTests, EnumKeyType)
{
    enum class Id : uint16_t { A = 1, B = 5, C = 10 };
    FlatMap<Id, float> map;
    map.InsertOrAssign(Id::C, 3.0f);
    map.InsertOrAssign(Id::A, 1.0f);
    map.InsertOrAssign(Id::B, 2.0f);

    EXPECT_EQ(map.Size(), 3u);
    EXPECT_FLOAT_EQ(*map.Find(Id::B), 2.0f);

    std::vector<Id> keys;
    for (const auto& [k, v] : map)
        keys.push_back(k);
    EXPECT_EQ(keys[0], Id::A);
    EXPECT_EQ(keys[1], Id::B);
    EXPECT_EQ(keys[2], Id::C);
}

TEST(FlatMapTests, MoveOnlyValues)
{
    FlatMap<int, std::unique_ptr<int>> map;
    map.InsertOrAssign(1, std::make_unique<int>(42));
    map.InsertOrAssign(2, std::make_unique<int>(99));

    EXPECT_EQ(map.Size(), 2u);
    ASSERT_NE(map.Find(1), nullptr);
    EXPECT_EQ(**map.Find(1), 42);

    map.InsertOrAssign(1, std::make_unique<int>(7));
    EXPECT_EQ(**map.Find(1), 7);
}

TEST(FlatMapTests, EraseIfKeepsTheRestInOrder)
{
    FlatMap<int, int> map;
    for (int key = 1; key <= 6; ++key)
        map.InsertOrAssign(key, key * 10);

    EXPECT_EQ(map.EraseIf([](int key, int value) { return key % 2 == 0 || value == 50; }), 4u);
    std::vector<int> keys;
    for (const auto& [key, value] : map)
        keys.push_back(key);
    EXPECT_EQ(keys, (std::vector<int>{1, 3}));
    EXPECT_EQ(*map.Find(3), 30);
    EXPECT_EQ(map.EraseIf([](int, int) { return false; }), 0u);
}
