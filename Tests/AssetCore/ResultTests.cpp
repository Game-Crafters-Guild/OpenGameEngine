#include <gtest/gtest.h>

#include "AssetCore/Result.h"

#include <memory>
#include <string>
#include <utility>

using namespace GameEngine;

namespace
{

struct MoveOnly
{
    explicit MoveOnly(int v) : Value(v) {}
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&&) = default;
    MoveOnly& operator=(MoveOnly&&) = default;
    int Value;
};

} // namespace

TEST(AssetResult, ConstructFromValueIsOk)
{
    Result<int> r(42);
    EXPECT_TRUE(r.IsOk());
    EXPECT_FALSE(r.IsErr());
    EXPECT_TRUE(static_cast<bool>(r));
    EXPECT_EQ(r.Value(), 42);
}

TEST(AssetResult, ConstructFromErrorIsErr)
{
    Result<int> r(AssetError::Missing);
    EXPECT_FALSE(r.IsOk());
    EXPECT_TRUE(r.IsErr());
    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_EQ(r.Error(), AssetError::Missing);
}

TEST(AssetResult, MoveOnlyValuePayloadIsSupported)
{
    Result<MoveOnly> r(MoveOnly(7));
    ASSERT_TRUE(r.IsOk());
    EXPECT_EQ(r.Value().Value, 7);

    Result<MoveOnly> moved = std::move(r);
    EXPECT_TRUE(moved.IsOk());
    EXPECT_EQ(moved.Value().Value, 7);
}

TEST(AssetResult, ValueOrReturnsFallbackOnError)
{
    Result<int> ok(123);
    Result<int> err(AssetError::ImportFailed);
    EXPECT_EQ(ok.ValueOr(-1), 123);
    EXPECT_EQ(err.ValueOr(-1), -1);
}

TEST(AssetResult, ValueOnConstResultReturnsConstReference)
{
    const Result<std::string> r(std::string("hello"));
    const std::string& s = r.Value();
    EXPECT_EQ(s, "hello");
}

TEST(AssetResult, ValueOnRvalueMoves)
{
    Result<std::unique_ptr<int>> r(std::make_unique<int>(99));
    auto ptr = std::move(r).Value();
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(*ptr, 99);
}

TEST(AssetResult, ToStringCoversEveryEnumValue)
{
    EXPECT_STREQ(ToString(AssetError::Missing), "Missing");
    EXPECT_STREQ(ToString(AssetError::ImportFailed), "ImportFailed");
    EXPECT_STREQ(ToString(AssetError::MountUnavailable), "MountUnavailable");
    EXPECT_STREQ(ToString(AssetError::Cancelled), "Cancelled");
}

TEST(AssetResult, CustomErrorTypeWorks)
{
    enum class IoErr { NotFound, Permission };
    Result<int, IoErr> r(IoErr::NotFound);
    EXPECT_TRUE(r.IsErr());
    EXPECT_EQ(r.Error(), IoErr::NotFound);
}

TEST(AssetResult, ResultIsCheapBySize)
{
    // Size of Result<int, AssetError> should be at most:
    //   sizeof(int) (4) + sizeof(AssetError) (4) + variant tag (~8 with alignment)
    // i.e. comparable to an inline tagged union — no heap, no large overhead.
    EXPECT_LE(sizeof(Result<int, AssetError>), 16u);
}
