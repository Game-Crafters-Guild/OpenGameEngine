#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/Vector4.h"

#include <cstddef>

#include <gtest/gtest.h>

using GameEngine::Mathematics::Vector2;
using GameEngine::Mathematics::Vector3;
using GameEngine::Mathematics::Vector4;

namespace
{

template <typename VectorType, std::size_t ComponentCount>
void ExpectIndexAndNameAlias(VectorType& vector, float* const (&named)[ComponentCount])
{
    for (std::size_t component = 0; component < ComponentCount; ++component)
    {
        EXPECT_EQ(&vector[component], named[component]);
        EXPECT_EQ(&vector.v[component], named[component]);
        vector[component] = static_cast<float>(component + 1);
        EXPECT_FLOAT_EQ(*named[component], static_cast<float>(component + 1));
        *named[component] = static_cast<float>(10 * (component + 1));
        EXPECT_FLOAT_EQ(vector[component], static_cast<float>(10 * (component + 1)));
    }
}

} // namespace

TEST(VectorComponents, IndexAndNameAddressTheSameComponent)
{
    Vector2 vector2;
    Vector3 vector3;
    Vector4 vector4;
    ExpectIndexAndNameAlias(vector2, {&vector2.x, &vector2.y});
    ExpectIndexAndNameAlias(vector3, {&vector3.x, &vector3.y, &vector3.z});
    ExpectIndexAndNameAlias(vector4, {&vector4.x, &vector4.y, &vector4.z, &vector4.w});
}

TEST(VectorComponents, ConstIndexReadsTheConstructedComponents)
{
    const Vector2 vector2(2, 3);
    const Vector3 vector3(2, 3, 5);
    const Vector4 vector4(Vector3(2, 3, 5), 7);
    const float expected[] = {2, 3, 5, 7};
    for (std::size_t component = 0; component < 2; ++component)
        EXPECT_FLOAT_EQ(vector2[component], expected[component]);
    for (std::size_t component = 0; component < 3; ++component)
        EXPECT_FLOAT_EQ(vector3[component], expected[component]);
    for (std::size_t component = 0; component < 4; ++component)
        EXPECT_FLOAT_EQ(vector4[component], expected[component]);
}

TEST(VectorComponents, ArraysOfVectorsAreTightlyPackedFloats)
{
    // Upload paths copy arrays of vectors as flat float arrays; the union adds no padding.
    const Vector3 points[2] = {Vector3(1, 2, 3), Vector3(4, 5, 6)};
    const float* flat = &points[0].x;
    for (std::size_t component = 0; component < 6; ++component)
        EXPECT_FLOAT_EQ(flat[component], static_cast<float>(component + 1));
    EXPECT_EQ(sizeof(points), 6 * sizeof(float));
    EXPECT_EQ(sizeof(Vector2[3]), 6 * sizeof(float));
    EXPECT_EQ(sizeof(Vector4[2]), 8 * sizeof(float));
}

TEST(VectorComponents, EqualityComparesEveryComponent)
{
    EXPECT_EQ(Vector2(1, 2), Vector2(1, 2));
    EXPECT_NE(Vector2(1, 2), Vector2(1, 3));
    EXPECT_NE(Vector2(1, 2), Vector2(0, 2));
}
