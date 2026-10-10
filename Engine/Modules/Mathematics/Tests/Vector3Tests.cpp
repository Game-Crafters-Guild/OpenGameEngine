#include "Mathematics/Vector3.h"

#include <gtest/gtest.h>

using GameEngine::Mathematics::Vector3;

TEST(Vector3Tests, LengthSquaredIsTheDotWithItself)
{
    const Vector3 v(1.0f, -2.0f, 3.0f);
    EXPECT_FLOAT_EQ(v.LengthSquared(), 14.0f);
    EXPECT_FLOAT_EQ(v.LengthSquared(), Vector3::Dot(v, v));
}

TEST(Vector3Tests, NormalizeOrZeroReturnsAUnitVector)
{
    const Vector3 n = Vector3(0.0f, 3.0f, 4.0f).NormalizeOrZero();
    EXPECT_FLOAT_EQ(n.x, 0.0f);
    EXPECT_FLOAT_EQ(n.y, 0.6f);
    EXPECT_FLOAT_EQ(n.z, 0.8f);
}

TEST(Vector3Tests, NormalizeOrZeroKeepsTheZeroVector)
{
    const Vector3 n = Vector3().NormalizeOrZero();
    EXPECT_EQ(n.x, 0.0f);
    EXPECT_EQ(n.y, 0.0f);
    EXPECT_EQ(n.z, 0.0f);
}
