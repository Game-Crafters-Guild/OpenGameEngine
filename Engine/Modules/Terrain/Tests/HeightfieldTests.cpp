#include "Terrain/Heightfield.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>

using namespace GameEngine;
using namespace GameEngine::Terrain;

TEST(HeightfieldTests, DefaultConstruct_IsEmpty)
{
    HeightfieldData hf;
    EXPECT_TRUE(hf.IsEmpty());
    EXPECT_EQ(hf.GetWidth(), 0u);
    EXPECT_EQ(hf.GetHeight(), 0u);
}

TEST(HeightfieldTests, Construct_WithDimensions)
{
    HeightfieldData hf(33, 33, 0.5f);
    EXPECT_FALSE(hf.IsEmpty());
    EXPECT_EQ(hf.GetWidth(), 33u);
    EXPECT_EQ(hf.GetHeight(), 33u);
    EXPECT_EQ(hf.GetSampleCount(), 33u * 33u);
    EXPECT_FLOAT_EQ(hf.GetSample(0, 0), 0.5f);
    EXPECT_FLOAT_EQ(hf.GetSample(16, 16), 0.5f);
}

TEST(HeightfieldTests, SetSample_ReadBack)
{
    HeightfieldData hf(5, 5, 0.0f);
    hf.SetSample(2, 3, 42.0f);
    EXPECT_FLOAT_EQ(hf.GetSample(2, 3), 42.0f);
    EXPECT_FLOAT_EQ(hf.GetSample(0, 0), 0.0f);
}

TEST(HeightfieldTests, SampleBilinear_CenterOfSample)
{
    HeightfieldData hf(3, 3, 0.0f);
    hf.SetSample(1, 1, 1.0f);

    // UV (0.5, 0.5) maps to the center sample (1,1) in a 3x3 grid
    float value = hf.SampleBilinear(0.5f, 0.5f);
    EXPECT_FLOAT_EQ(value, 1.0f);
}

TEST(HeightfieldTests, SampleBilinear_Interpolation)
{
    HeightfieldData hf(2, 2, 0.0f);
    // 2x2 grid: corners at UV (0,0), (1,0), (0,1), (1,1)
    hf.SetSample(0, 0, 0.0f);
    hf.SetSample(1, 0, 1.0f);
    hf.SetSample(0, 1, 0.0f);
    hf.SetSample(1, 1, 1.0f);

    // Mid-X should give 0.5
    float mid = hf.SampleBilinear(0.5f, 0.0f);
    EXPECT_NEAR(mid, 0.5f, 0.01f);
}

TEST(HeightfieldTests, SampleBilinear_ClampEdges)
{
    HeightfieldData hf(3, 3, 1.0f);
    // Values outside [0,1] should clamp
    EXPECT_FLOAT_EQ(hf.SampleBilinear(-0.1f, 0.5f), 1.0f);
    EXPECT_FLOAT_EQ(hf.SampleBilinear(1.1f, 0.5f), 1.0f);
}

namespace
{
std::filesystem::path WriteTempRawU16(const char* name, const uint16* samples, std::size_t count)
{
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(samples),
              static_cast<std::streamsize>(count * sizeof(uint16)));
    return path;
}
} // namespace

TEST(HeightfieldTests, LoadFromRawUInt16_NormalizesSamples)
{
    // Row-major (z * width + x): (0,0)=0, (1,0)=65535, (0,1)=32768, (1,1)=16384.
    const uint16 samples[4] = {0, 65535, 32768, 16384};
    const auto path = WriteTempRawU16("ge_heightfield_rawu16_load.r16", samples, 4);

    HeightfieldData hf;
    ASSERT_TRUE(hf.LoadFromRawUInt16(path, 2, 2));
    EXPECT_EQ(hf.GetWidth(), 2u);
    EXPECT_EQ(hf.GetHeight(), 2u);
    EXPECT_FLOAT_EQ(hf.GetSample(0, 0), 0.0f);
    EXPECT_FLOAT_EQ(hf.GetSample(1, 0), 1.0f);
    EXPECT_FLOAT_EQ(hf.GetSample(0, 1), 32768.0f / 65535.0f);
    EXPECT_FLOAT_EQ(hf.GetSample(1, 1), 16384.0f / 65535.0f);

    std::filesystem::remove(path);
}

TEST(HeightfieldTests, LoadFromRawUInt16_ShortFileFails)
{
    const uint16 samples[2] = {1, 2};
    const auto path = WriteTempRawU16("ge_heightfield_rawu16_short.r16", samples, 2);

    HeightfieldData hf;
    EXPECT_FALSE(hf.LoadFromRawUInt16(path, 2, 2)); // needs 4 samples, file has 2
    EXPECT_TRUE(hf.IsEmpty());

    std::filesystem::remove(path);
}

TEST(HeightfieldTests, LoadFromRawUInt16_MissingFileFails)
{
    HeightfieldData hf;
    const auto path = std::filesystem::temp_directory_path() / "ge_heightfield_rawu16_missing.r16";
    std::filesystem::remove(path);
    EXPECT_FALSE(hf.LoadFromRawUInt16(path, 2, 2));
}

TEST(HeightfieldTests, ComputeNormal_FlatTerrain)
{
    HeightfieldData hf(5, 5, 0.0f);
    auto normal = hf.ComputeNormal(2, 2, 1.0f, 1.0f);
    // Flat terrain should have normal pointing straight up
    EXPECT_NEAR(normal.x, 0.0f, 0.01f);
    EXPECT_NEAR(normal.y, 1.0f, 0.01f);
    EXPECT_NEAR(normal.z, 0.0f, 0.01f);
}

TEST(HeightfieldTests, ComputeNormal_Slope)
{
    HeightfieldData hf(5, 5, 0.0f);
    // Create a slope along X: each column increases
    for (uint32 z = 0; z < 5; ++z)
        for (uint32 x = 0; x < 5; ++x)
            hf.SetSample(x, z, static_cast<float32>(x) * 0.5f);

    auto normal = hf.ComputeNormal(2, 2, 1.0f, 1.0f);
    // Normal should tilt away from the slope (negative X component)
    EXPECT_LT(normal.x, 0.0f);
    EXPECT_GT(normal.y, 0.0f);
}

TEST(HeightfieldTests, GetMinMax_Region)
{
    HeightfieldData hf(5, 5, 0.0f);
    hf.SetSample(1, 1, 10.0f);
    hf.SetSample(2, 2, -5.0f);

    float32 minH = 0, maxH = 0;
    hf.GetMinMax(0, 0, 3, 3, minH, maxH);
    EXPECT_FLOAT_EQ(minH, -5.0f);
    EXPECT_FLOAT_EQ(maxH, 10.0f);
}

TEST(HeightfieldTests, FillWithNoise_ProducesVariation)
{
    HeightfieldData hf(33, 33, 0.0f);
    hf.FillWithNoise(0.02f, 1.0f, 4, 42);

    float minVal = 1e9f, maxVal = -1e9f;
    for (uint32 z = 0; z < 33; ++z)
    {
        for (uint32 x = 0; x < 33; ++x)
        {
            float v = hf.GetSample(x, z);
            minVal = std::min(minVal, v);
            maxVal = std::max(maxVal, v);
        }
    }
    // Noise should produce variation
    EXPECT_LT(minVal, maxVal);
    // Values should be in [0,1] range
    EXPECT_GE(minVal, 0.0f);
    EXPECT_LE(maxVal, 1.0f);
}

TEST(HeightfieldTests, FillRegionWithNoise_MatchesFullFillInRegion)
{
    HeightfieldData full(33, 33, 0.0f);
    full.FillWithNoise(4.0f, 1.0f, 5, 42);

    constexpr int32 kMinX = 8, kMinZ = 5, kMaxX = 20, kMaxZ = 17;
    HeightfieldData region(33, 33, -1.0f);
    region.FillRegionWithNoise(4.0f, 1.0f, kMinX, kMinZ, kMaxX, kMaxZ, 5, 42);

    for (uint32 z = 0; z < 33; ++z)
    {
        for (uint32 x = 0; x < 33; ++x)
        {
            const bool inRegion = static_cast<int32>(x) >= kMinX && static_cast<int32>(x) <= kMaxX &&
                                  static_cast<int32>(z) >= kMinZ && static_cast<int32>(z) <= kMaxZ;
            if (inRegion)
                EXPECT_EQ(region.GetSample(x, z), full.GetSample(x, z))
                    << "region fill diverged from full fill at (" << x << ", " << z << ")";
            else
                EXPECT_EQ(region.GetSample(x, z), -1.0f)
                    << "region fill touched sample outside region at (" << x << ", " << z << ")";
        }
    }
}

TEST(HeightfieldTests, FillRegionWithNoise_ClampsOutOfRangeRegion)
{
    HeightfieldData full(17, 17, 0.0f);
    full.FillWithNoise(4.0f, 1.0f, 4, 7);

    HeightfieldData region(17, 17, 0.0f);
    region.FillRegionWithNoise(4.0f, 1.0f, -10, -10, 100, 100, 4, 7);

    for (uint32 z = 0; z < 17; ++z)
        for (uint32 x = 0; x < 17; ++x)
            EXPECT_EQ(region.GetSample(x, z), full.GetSample(x, z));
}
