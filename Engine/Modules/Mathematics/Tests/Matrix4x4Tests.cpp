#include "Mathematics/Matrix4x4.h"

#include <gtest/gtest.h>

using GameEngine::Mathematics::Matrix4x4;
using GameEngine::Mathematics::Vector3;

TEST(Matrix4x4Tests, FromColumnMajorReadsTranslationFromTheLastColumn)
{
    // Uniform scale 2, translation (5, 6, 7), column-major.
    const float columnMajor[16] = {
        2.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 2.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 2.0f, 0.0f,
        5.0f, 6.0f, 7.0f, 1.0f};
    const Vector3 p = Matrix4x4::FromColumnMajor(columnMajor).TransformPoint(Vector3(1.0f, 2.0f, 3.0f));
    EXPECT_FLOAT_EQ(p.x, 7.0f);
    EXPECT_FLOAT_EQ(p.y, 10.0f);
    EXPECT_FLOAT_EQ(p.z, 13.0f);
}
