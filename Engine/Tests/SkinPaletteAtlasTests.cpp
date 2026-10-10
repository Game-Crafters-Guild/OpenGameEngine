// Tests for SkinPaletteAtlas: per-frame bone palette atlas SSBO for instanced
// skinned draws. Validates allocation, offset tracking, buffer reuse, and
// integration with PerFrameWritePool.

#include <gtest/gtest.h>

#include "Engine/Rendering/BonePaletteLayout.h"
#include "Engine/Rendering/SkinPaletteAtlas.h"
#include "Engine/Rendering/PerFrameWritePool.h"
#include "Rendering/Core/Device.h"

#include <cmath>
#include <cstring>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Rendering;

#include "TestDeviceHelper.h"

class SkinPaletteAtlasTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_device = CreateVulkanDeviceFast();
        if (!m_device)
            GTEST_SKIP() << "No Vulkan device available";
        PerFrameWritePoolConfig cfg{};
        cfg.FramesInFlight = 2;
        ASSERT_TRUE(m_pool.Initialize(m_device.get(), cfg));
    }

    void TearDown() override
    {
        m_pool.Shutdown();
        if (m_device)
            m_device->Shutdown();
    }

    // Create a test palette: boneCount identity matrices.
    std::vector<float> MakeIdentityPalette(uint32_t boneCount)
    {
        std::vector<float> data(boneCount * 16, 0.0f);
        for (uint32_t i = 0; i < boneCount; ++i)
        {
            data[i * 16 + 0] = 1.0f;  // m[0][0]
            data[i * 16 + 5] = 1.0f;  // m[1][1]
            data[i * 16 + 10] = 1.0f; // m[2][2]
            data[i * 16 + 15] = 1.0f; // m[3][3]
        }
        return data;
    }

    std::unique_ptr<IDevice> m_device;
    PerFrameWritePool m_pool;
    SkinPaletteAtlas m_atlas;
};

TEST_F(SkinPaletteAtlasTest, MultipleUploads_GetContiguousOffsets)
{
    m_pool.BeginFrame(0);
    m_atlas.BeginFrame(m_pool);

    auto pal1 = MakeIdentityPalette(32);
    auto pal2 = MakeIdentityPalette(64);

    auto alloc1 = m_atlas.Upload(pal1.data(), 32);
    auto alloc2 = m_atlas.Upload(pal2.data(), 64);

    EXPECT_TRUE(alloc1.valid);
    EXPECT_TRUE(alloc2.valid);

    // Second allocation should start after the first.
    EXPECT_GT(alloc2.offsetInFloats, alloc1.offsetInFloats);
    EXPECT_GE(alloc2.OffsetInBones(), alloc1.OffsetInBones() + 32u);

    EXPECT_EQ(m_atlas.GetCurrentFrameAllocCount(), 2u);
    EXPECT_EQ(m_atlas.GetCurrentFrameTotalBones(), 96u);
}

TEST_F(SkinPaletteAtlasTest, Upload_NullDataOrZeroBones_ReturnsInvalid)
{
    m_pool.BeginFrame(0);
    m_atlas.BeginFrame(m_pool);

    EXPECT_FALSE(m_atlas.Upload(nullptr, 64).valid);
    float dummy = 0.0f;
    EXPECT_FALSE(m_atlas.Upload(&dummy, 0).valid);
}

// =============================================================================
// BonePaletteLayout — CPU/GPU shared mat3x4 packing.
//
// These tests don't need the Vulkan device; they validate the host-side
// pack/unpack helpers that mirror Includes/bone_palette.glsl exactly.
// Any drift between the CPU pack and the GPU shader's row interpretation
// will surface here as a numerical mismatch.
// =============================================================================

namespace
{
// Apply 3 packed rows (12 floats) to a homogeneous point — mirrors
// ge_ApplyBonePalettePosition's math (with weights = (1,0,0,0) and only
// joint 0 considered, plus no atlas indexing).
void ApplyRowsToPoint(const float* rows, const float p[3], float out[3])
{
    for (int r = 0; r < 3; ++r)
    {
        out[r] = rows[r * 4 + 0] * p[0]
              + rows[r * 4 + 1] * p[1]
              + rows[r * 4 + 2] * p[2]
              + rows[r * 4 + 3]; // implicit *1 for the homogeneous w
    }
}

// Apply column-major mat4 (16 floats) to a vec3 (homogeneous w=1).
void ApplyMat4CmToPoint(const float* m, const float p[3], float out[3])
{
    for (int r = 0; r < 3; ++r)
    {
        out[r] = m[0 * 4 + r] * p[0]
              + m[1 * 4 + r] * p[1]
              + m[2 * 4 + r] * p[2]
              + m[3 * 4 + r];
    }
}
} // namespace

TEST(BonePaletteLayoutTests, PackUnpackRoundTrip_PreservesAffineRows)
{
    using namespace BonePaletteLayout;

    // Sample affine mat4 (column-major). Translation+rotation+scale.
    // Row 3 = (0,0,0,1) — affine invariant.
    float m[16] = {
        // col 0
        2.0f, 0.0f, 0.0f, 0.0f,
        // col 1
        0.0f, 1.5f, 0.5f, 0.0f,
        // col 2
        0.0f, -0.5f, 1.5f, 0.0f,
        // col 3 (translation)
        10.0f, 20.0f, 30.0f, 1.0f
    };

    float rows[kFloatsPerBone];
    PackMat4ToRows(m, rows);

    float reconstructed[16];
    UnpackRowsToMat4(rows, reconstructed);

    // Top 3 rows preserved exactly.
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 3; ++r)
            EXPECT_FLOAT_EQ(reconstructed[c * 4 + r], m[c * 4 + r]) << "col " << c << " row " << r;

    // Implied bottom row reconstructed as (0, 0, 0, 1).
    EXPECT_FLOAT_EQ(reconstructed[0 * 4 + 3], 0.0f);
    EXPECT_FLOAT_EQ(reconstructed[1 * 4 + 3], 0.0f);
    EXPECT_FLOAT_EQ(reconstructed[2 * 4 + 3], 0.0f);
    EXPECT_FLOAT_EQ(reconstructed[3 * 4 + 3], 1.0f);
}

TEST(BonePaletteLayoutTests, RowApply_EquivalentToMat4Apply)
{
    using namespace BonePaletteLayout;

    float m[16] = {
        0.7071f, 0.7071f, 0.0f, 0.0f, // col 0: rotate Z
        -0.7071f, 0.7071f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        5.0f, -3.0f, 2.0f, 1.0f
    };

    float rows[kFloatsPerBone];
    PackMat4ToRows(m, rows);

    const float p[3] = {1.0f, 0.0f, 0.0f};
    float fromMat[3];
    float fromRows[3];
    ApplyMat4CmToPoint(m, p, fromMat);
    ApplyRowsToPoint(rows, p, fromRows);

    EXPECT_NEAR(fromRows[0], fromMat[0], 1e-5f);
    EXPECT_NEAR(fromRows[1], fromMat[1], 1e-5f);
    EXPECT_NEAR(fromRows[2], fromMat[2], 1e-5f);
}

TEST(BonePaletteLayoutTests, IdentityMatrixPacksToCanonicalRows)
{
    using namespace BonePaletteLayout;

    float identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    float rows[kFloatsPerBone];
    PackMat4ToRows(identity, rows);

    // Row 0 should be (1, 0, 0, 0); row 1 = (0, 1, 0, 0); row 2 = (0, 0, 1, 0).
    EXPECT_FLOAT_EQ(rows[0 * 4 + 0], 1.0f);
    EXPECT_FLOAT_EQ(rows[0 * 4 + 1], 0.0f);
    EXPECT_FLOAT_EQ(rows[0 * 4 + 2], 0.0f);
    EXPECT_FLOAT_EQ(rows[0 * 4 + 3], 0.0f);
    EXPECT_FLOAT_EQ(rows[1 * 4 + 0], 0.0f);
    EXPECT_FLOAT_EQ(rows[1 * 4 + 1], 1.0f);
    EXPECT_FLOAT_EQ(rows[1 * 4 + 2], 0.0f);
    EXPECT_FLOAT_EQ(rows[1 * 4 + 3], 0.0f);
    EXPECT_FLOAT_EQ(rows[2 * 4 + 0], 0.0f);
    EXPECT_FLOAT_EQ(rows[2 * 4 + 1], 0.0f);
    EXPECT_FLOAT_EQ(rows[2 * 4 + 2], 1.0f);
    EXPECT_FLOAT_EQ(rows[2 * 4 + 3], 0.0f);
}

TEST(BonePaletteLayoutTests, ConstantsMatchSharedSpec)
{
    // Mirrors the GLSL constants in Includes/bone_palette.glsl.
    EXPECT_EQ(BonePaletteLayout::kFloatsPerBone, 12u);
    EXPECT_EQ(BonePaletteLayout::kVec4sPerBone, 3u);
}
