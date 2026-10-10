#include "GlslShim.h"
#include <gtest/gtest.h>
#include <vector>

namespace ApplyShader
{
using namespace GameEngine::GlslShim;
using GameEngine::GlslShim::sqrt;
using GameEngine::GlslShim::abs;
using GameEngine::GlslShim::sin;
using GameEngine::GlslShim::cos;
using GameEngine::GlslShim::atan;
using GameEngine::GlslShim::length;
#include "GrassWindVolumesExtracted.h"
struct TerrainParamsEntry
{
    float GrassWindDirection = 0, GrassWindStrength = 0, GrassWindFlutterAmount = 0;
    float GrassWindGustSpeed = 1, GrassWindGustScale = 0.05f;
};
struct VolumeArray : std::vector<GrassWindVolume>
{
    int length() const { return static_cast<int>(size()); }
} grassWindVolumes;
#include "GrassWindApplyExtracted.h"
}

TEST(GrassWindVolumes, AppliesCalmAndAdditiveVolumesToGrassParameters)
{
    ApplyShader::TerrainParamsEntry params;
    params.GrassWindStrength = 1.0f;
    params.GrassWindFlutterAmount = 0.2f;
    ApplyShader::GrassWindVolume volume{};
    volume.CenterShape.w = 0.0f; // box at the blade root
    volume.AxisXExtent = ApplyShader::vec4(1, 0, 0, 2);
    volume.AxisYExtent = ApplyShader::vec4(0, 1, 0, 2);
    volume.AxisZExtent = ApplyShader::vec4(0, 0, 1, 2);
    volume.VelocityWeight.w = 1.0f;
    volume.Mode.x = 1.0f;
    volume.Gust = ApplyShader::vec4(0, 1, 20, 2);
    ApplyShader::grassWindVolumes = {{volume}};
    const auto calm = ApplyShader::grassApplyWindVolumes(params, ApplyShader::vec3(0, 0, 0));
    EXPECT_FLOAT_EQ(calm.GrassWindStrength, 0.0f);
    EXPECT_FLOAT_EQ(calm.GrassWindFlutterAmount, 0.0f);
    const auto outside = ApplyShader::grassApplyWindVolumes(params, ApplyShader::vec3(20, 0, 0));
    EXPECT_FLOAT_EQ(outside.GrassWindStrength, params.GrassWindStrength);

    volume.CenterShape.w = -1.0f;
    volume.Mode.x = 0.0f;
    volume.VelocityWeight = ApplyShader::vec4(2, 0, 4, 1);
    ApplyShader::grassWindVolumes = {{volume}};
    const auto additive = ApplyShader::grassApplyWindVolumes(params, ApplyShader::vec3(0, 0, 0));
    EXPECT_NEAR(additive.GrassWindStrength, 5.0f, 1e-5f);
    EXPECT_NEAR(additive.GrassWindDirection, std::atan2(4.0f, 3.0f), 1e-5f);
    ApplyShader::grassWindVolumes.clear();
    params.GrassWindStrength = 0.0f;
    params.GrassWindFlutterAmount = 0.0f;
    const auto still = ApplyShader::grassApplyWindVolumes(params, ApplyShader::vec3(0, 0, 0));
    EXPECT_FLOAT_EQ(still.GrassWindStrength, 0.0f);
    EXPECT_FLOAT_EQ(still.GrassWindFlutterAmount, 0.0f);
}
