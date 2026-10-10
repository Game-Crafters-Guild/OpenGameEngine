// The DDGI software lane shades from a CPU-baked record of each material. The
// parameters it needs — base colour, opacity, metallic, roughness, emission —
// have no fixed home in the parameter block: a surface that declares its
// properties packs them in declaration order. Reading fixed lanes shades a
// declared surface with whatever happened to land there, which is silent: a
// wrong bounce colour, no error, no crash.

#include "Engine/Rendering/DDGIUberMaterialBake.h"

#include "Engine/Rendering/DDGIEmissive.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/LegacyMaterialLanes.h"
#include "Rendering/Materials/MaterialParamsLayout.h"
#include "Rendering/Materials/ShaderPropertyTable.h"
#include "SceneBvh/UberMaterial.h"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using GameEngine::Rendering::BuildShaderPropertyTable;
using GameEngine::Rendering::ShaderPropertyOrigin;
using GameEngine::Rendering::ShaderPropertySource;
using GameEngine::Rendering::ShaderPropertyTable;
using GameEngine::Rendering::IDevice;

#include "TestDeviceHelper.h"

namespace
{

// The bake reads only a material's CPU parameter cache and its declared table,
// but Material is registry-constructed, so the registry is how a test gets one.
class DDGIUberMaterialBakeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        m_Registry.Initialize(m_Device.get());
        m_Registry.SeedTextureSlotDefaults(/*white*/ 1u, /*flatNormal*/ 2u, /*black*/ 3u);
    }

    void TearDown() override
    {
        m_Registry.Shutdown();
        if (m_Device)
            m_Device->Shutdown();
    }

    // A registered material with no authored properties; the test sets what it
    // needs by name afterwards, through the same setters the editor uses.
    Material* Register(const char* surface)
    {
        MaterialDocument doc{};
        doc.schemaVersion = 3;
        doc.materialName = surface;
        doc.lightingModel = "StandardPBR";
        doc.surfaceShader = surface;
        return m_Registry.Register(GUID::Generate(), doc);
    }

    std::unique_ptr<IDevice> m_Device;
    MaterialRegistry m_Registry;
};

void SetVector(Material& mat, const char* name, const std::vector<float>& values)
{
    mat.SetVector(HashStringId(name), values.data(), static_cast<uint32_t>(values.size()));
}

} // namespace

// The shipped waterfall surface: panSpeed takes lane 0 .xy, brightness 0.z and
// waterColor lane 1 .xyz. A fixed-lane reader takes base colour from lane 0 —
// the pan speed — and metallic from lane 1.x, which is the water colour's red.
TEST_F(DDGIUberMaterialBakeTest, DeclaredSurfaceWhoseLanesDoNotMatchTheLegacyConvention)
{
    const auto table = std::make_shared<const ShaderPropertyTable>(BuildShaderPropertyTable(
        {ShaderPropertySource{"// @property vec2  panSpeed   default=0.2,0\n"
                              "// @property float brightness default=1\n"
                              "// @property color waterColor default=0,0,1,1\n"
                              "// @property float metallic   default=0\n"
                              "// @property float roughness  default=0.5\n"
                              "// @property color baseColor  default=1,1,1,1\n",
                              "waterfall_fx.glsl", ShaderPropertyOrigin::Surface}}));
    ASSERT_FALSE(table->Rejected()) << table->Errors.front().Message;

    // The names the bake wants are NOT where the legacy convention puts them.
    const auto* baseColor = table->Find("baseColor");
    ASSERT_NE(baseColor, nullptr);
    EXPECT_NE(baseColor->ByteOffset, 0u)
        << "this test only proves anything while baseColor is off lane 0";

    Material* mat = Register("waterfall_fx.glsl");
    ASSERT_NE(mat, nullptr);
    mat->SetDeclaredProperties(table);
    SetVector(*mat, "baseColor", {0.25f, 0.5f, 0.75f, 0.8f});
    mat->SetFloat(HashStringId("metallic"), 0.125f);
    mat->SetFloat(HashStringId("roughness"), 0.375f);
    SetVector(*mat, "waterColor", {1.0f, 0.0f, 0.0f, 1.0f}); // what a fixed reader would steal

    const SceneBvh::UberMaterial baked = BakeUberMaterial(mat, nullptr);
    EXPECT_FLOAT_EQ(baked.BaseColorR, 0.25f);
    EXPECT_FLOAT_EQ(baked.BaseColorG, 0.5f);
    EXPECT_FLOAT_EQ(baked.BaseColorB, 0.75f);
    EXPECT_FLOAT_EQ(baked.Opacity, 0.8f);
    EXPECT_FLOAT_EQ(baked.Metalness, 0.125f) << "metallic came from waterColor.r";
    EXPECT_FLOAT_EQ(baked.Roughness, 0.375f);
}

// A surface that declares none of them still bakes something sane rather than a
// black, fully-metallic hole in the bounce.
TEST_F(DDGIUberMaterialBakeTest, DeclaredSurfaceThatAuthorsNoneOfThemBakesTheNeutralRecord)
{
    const auto table = std::make_shared<const ShaderPropertyTable>(BuildShaderPropertyTable(
        {ShaderPropertySource{"// @property float panSpeed default=0.2\n", "fx.glsl",
                              ShaderPropertyOrigin::Surface}}));
    ASSERT_FALSE(table->Rejected());

    Material* mat = Register("fx.glsl");
    ASSERT_NE(mat, nullptr);
    mat->SetDeclaredProperties(table);

    const SceneBvh::UberMaterial baked = BakeUberMaterial(mat, nullptr);
    EXPECT_FLOAT_EQ(baked.BaseColorR, 1.0f);
    EXPECT_FLOAT_EQ(baked.BaseColorG, 1.0f);
    EXPECT_FLOAT_EQ(baked.BaseColorB, 1.0f);
    EXPECT_FLOAT_EQ(baked.Opacity, 1.0f);
    EXPECT_FLOAT_EQ(baked.Metalness, 0.0f);
    EXPECT_FLOAT_EQ(baked.Roughness, 0.5f);
    EXPECT_FLOAT_EQ(baked.EmissiveR, 0.0f);
}

// An adapter read the surface does not redeclare has no lane: the compiler folds
// it to the declaration's default, and that constant is what the raster surface
// shades with — so the bounce shades with it too, not with the bake's own
// neutral guess.
TEST_F(DDGIUberMaterialBakeTest, ADeclaredNameFoldedToAConstantBakesItsDefault)
{
    const auto table = std::make_shared<const ShaderPropertyTable>(BuildShaderPropertyTable(
        {ShaderPropertySource{"// @property float metallic default=0.9\n", "adapter_forward.glsl",
                              ShaderPropertyOrigin::Adapter},
         ShaderPropertySource{"// @property float panSpeed default=0.2\n", "fx.glsl",
                              ShaderPropertyOrigin::Surface}}));
    ASSERT_FALSE(table->Rejected()) << table->Errors.front().Message;
    const auto* metallic = table->Find("metallic");
    ASSERT_NE(metallic, nullptr);
    ASSERT_FALSE(metallic->HasLane) << "this test only proves anything while metallic has no lane";

    Material* mat = Register("fx.glsl");
    ASSERT_NE(mat, nullptr);
    mat->SetDeclaredProperties(table);

    const SceneBvh::UberMaterial baked = BakeUberMaterial(mat, nullptr);
    EXPECT_FLOAT_EQ(baked.Metalness, 0.9f) << "the folded default, not the neutral 0";
    EXPECT_FLOAT_EQ(baked.Roughness, 0.5f) << "not declared at all: the neutral fallback";
}

// The path every engine surface still takes: no declared table, so the names
// resolve through the shared legacy lane map — the same offsets the registry
// writes and the same lanes the hardware kernel indexes.
TEST_F(DDGIUberMaterialBakeTest, UndeclaredSurfaceReadsTheLegacyLanes)
{
    Material* mat = Register("Surfaces/standard_pbr.glsl");
    ASSERT_NE(mat, nullptr);
    SetVector(*mat, "baseColor", {0.1f, 0.2f, 0.3f, 0.4f});
    mat->SetFloat(HashStringId("metallic"), 0.6f);
    mat->SetFloat(HashStringId("roughness"), 0.7f);
    SetVector(*mat, "emissive", {1.0f, 0.5f, 0.25f});
    mat->SetFloat(HashStringId("emissionLuminance"), 203.0f);

    const SceneBvh::UberMaterial baked = BakeUberMaterial(mat, nullptr);
    EXPECT_FLOAT_EQ(baked.BaseColorR, 0.1f);
    EXPECT_FLOAT_EQ(baked.Opacity, 0.4f);
    EXPECT_FLOAT_EQ(baked.Metalness, 0.6f);
    EXPECT_FLOAT_EQ(baked.Roughness, 0.7f);

    // The same fold the raster surface and the hardware kernel apply.
    const DDGIEmissive expected = ComputeDDGIEmissive(1.0f, 0.5f, 0.25f, 203.0f);
    EXPECT_FLOAT_EQ(baked.EmissiveR, expected.Color[0]);

    // And those offsets are the legacy table's, not a second copy of them.
    const auto* lane = GameEngine::Rendering::FindLegacyMaterialLane("baseColor");
    ASSERT_NE(lane, nullptr);
    EXPECT_EQ(GameEngine::Rendering::LegacyLaneByteOffset(*lane), 0u);
}

TEST_F(DDGIUberMaterialBakeTest, NullMaterialBakesTheNeutralRecord)
{
    const SceneBvh::UberMaterial baked = BakeUberMaterial(nullptr, nullptr);
    const SceneBvh::UberMaterial neutral{};
    EXPECT_EQ(0, std::memcmp(&baked, &neutral, sizeof(baked)))
        << "a null material must bake the default record, not a read through null";
}
