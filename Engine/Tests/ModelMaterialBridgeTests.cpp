// Tests for ModelMaterialBridge: glTF material -> MaterialDocument conversion,
// stable GUID derivation, property mapping, alpha/double-sided flags.

#include <gtest/gtest.h>

#include <limits>

#include "Engine/Rendering/ModelMaterialBridge.h"
#include "Assets/ModelAsset.h"
#include "AssetCore/GUID.h"

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;

// Helper: create a default glTF-style PBR material.
static ImportedMaterialData MakeDefaultPBRMaterial()
{
    ImportedMaterialData m{};
    m.Name = "TestPBR";
    m.DiffuseColor[0] = 0.8f;
    m.DiffuseColor[1] = 0.2f;
    m.DiffuseColor[2] = 0.1f;
    m.DiffuseColor[3] = 1.0f;
    m.Metallic = 0.0f;
    m.Roughness = 0.5f;
    m.DoubleSided = false;
    m.AlphaMode = AlphaMode::Opaque;
    m.AlphaCutoff = 0.5f;
    return m;
}

// Basic conversion produces a valid MaterialDocument with correct properties.
TEST(ModelMaterialBridgeTest, Convert_DefaultPBR_ProducesValidDocument)
{
    GUID modelGuid = GUID::Generate();
    ImportedMaterialData mat = MakeDefaultPBRMaterial();

    auto result = ModelMaterialBridge::Convert(modelGuid, 0, mat);

    EXPECT_FALSE(result.derivedGuid.IsNull());
    EXPECT_EQ(result.materialIndex, 0u);
    EXPECT_EQ(result.document.schemaVersion, 3);
    EXPECT_EQ(result.document.lightingModel, "StandardPBR");
    EXPECT_FALSE(result.document.surfaceShader.empty());

    // Check baseColor property.
    auto it = result.document.properties.find("baseColor");
    ASSERT_NE(it, result.document.properties.end());
    const auto* bc = std::get_if<std::vector<float>>(&it->second);
    ASSERT_NE(bc, nullptr);
    ASSERT_EQ(bc->size(), 4u);
    EXPECT_NEAR((*bc)[0], 0.8f, 1e-5f);
    EXPECT_NEAR((*bc)[1], 0.2f, 1e-5f);
    EXPECT_NEAR((*bc)[2], 0.1f, 1e-5f);
    EXPECT_NEAR((*bc)[3], 1.0f, 1e-5f);

    // Check metallic and roughness.
    {
        auto itM = result.document.properties.find("metallic");
        ASSERT_NE(itM, result.document.properties.end());
        EXPECT_NEAR(std::get<float>(itM->second), 0.0f, 1e-5f);
    }
    {
        auto itR = result.document.properties.find("roughness");
        ASSERT_NE(itR, result.document.properties.end());
        EXPECT_NEAR(std::get<float>(itR->second), 0.5f, 1e-5f);
    }
}

// Document reflects the material render state and lighting model.
TEST(ModelMaterialBridgeTest, Convert_SetsCorrectDocumentFields)
{
    GUID modelGuid = GUID::Generate();
    ImportedMaterialData mat = MakeDefaultPBRMaterial();

    auto result = ModelMaterialBridge::Convert(modelGuid, 0, mat);

    EXPECT_EQ(result.document.lightingModel, "StandardPBR");
    EXPECT_EQ(result.document.alphaMode, MaterialAlphaMode::Opaque);
    EXPECT_FALSE(result.document.doubleSided);
}

// Double-sided material sets the document flag.
TEST(ModelMaterialBridgeTest, Convert_DoubleSided_SetsDocumentFlag)
{
    GUID modelGuid = GUID::Generate();
    ImportedMaterialData mat = MakeDefaultPBRMaterial();
    mat.DoubleSided = true;

    auto result = ModelMaterialBridge::Convert(modelGuid, 0, mat);

    EXPECT_TRUE(result.document.doubleSided);
}

// Alpha mask material sets correct alpha mode on document.
TEST(ModelMaterialBridgeTest, Convert_AlphaMask_SetsDocumentAlphaMode)
{
    GUID modelGuid = GUID::Generate();
    ImportedMaterialData mat = MakeDefaultPBRMaterial();
    mat.AlphaMode = AlphaMode::Mask;
    mat.AlphaCutoff = 0.3f;

    auto result = ModelMaterialBridge::Convert(modelGuid, 0, mat);

    EXPECT_EQ(result.document.alphaMode, MaterialAlphaMode::Mask);

    auto itCut = result.document.properties.find("alphaCutoff");
    ASSERT_NE(itCut, result.document.properties.end());
    EXPECT_NEAR(std::get<float>(itCut->second), 0.3f, 1e-5f);
}

// Alpha blend material sets correct alpha mode on document.
TEST(ModelMaterialBridgeTest, Convert_AlphaBlend_SetsDocumentAlphaMode)
{
    GUID modelGuid = GUID::Generate();
    ImportedMaterialData mat = MakeDefaultPBRMaterial();
    mat.AlphaMode = AlphaMode::Blend;

    auto result = ModelMaterialBridge::Convert(modelGuid, 0, mat);

    EXPECT_EQ(result.document.alphaMode, MaterialAlphaMode::Blend);
}

// A source material that never reads vertex colour (FBX) converts to a document
// that ignores the mesh's colour stream; the default (glTF) keeps it.
TEST(ModelMaterialBridgeTest, Convert_IgnoresVertexColor_SetsDocumentFlag)
{
    ImportedMaterialData mat = MakeDefaultPBRMaterial();
    EXPECT_FALSE(ModelMaterialBridge::Convert(GUID::Generate(), 0, mat).document.ignoreVertexColor);

    mat.IgnoresVertexColor = true;
    EXPECT_TRUE(ModelMaterialBridge::Convert(GUID::Generate(), 0, mat).document.ignoreVertexColor);
}

// Textures are mapped correctly.
TEST(ModelMaterialBridgeTest, Convert_WithTextures_MapsCorrectly)
{
    GUID modelGuid = GUID::Generate();
    ImportedMaterialData mat = MakeDefaultPBRMaterial();
    mat.DiffuseTexture = "Textures/albedo.png";
    mat.NormalTexture = "Textures/normal.png";
    mat.SpecularTexture = "Textures/metallic_roughness.png";

    auto result = ModelMaterialBridge::Convert(modelGuid, 0, mat);

    EXPECT_EQ(result.document.textures["albedoMap"], "Textures/albedo.png");
    EXPECT_EQ(result.document.textures["normalMap"], "Textures/normal.png");
    EXPECT_EQ(result.document.textures["metallicRoughnessMap"], "Textures/metallic_roughness.png");
}

// The imported emissive color (an FBX emission color times its factor) becomes the material's emission: a
// [0, 1] tint and its brightness in nits, a peak of 1 being reference white (203 nits).
TEST(ModelMaterialBridgeTest, Convert_EmissiveColor_SplitsIntoTintAndLuminance)
{
    ImportedMaterialData mat = MakeDefaultPBRMaterial();
    mat.EmissiveColor[0] = 2.0f;
    mat.EmissiveColor[1] = 0.5f;
    mat.EmissiveColor[2] = 0.25f;

    const auto result = ModelMaterialBridge::Convert(GUID::Generate(), 0, mat);

    const auto tint = result.document.properties.find("emissive");
    ASSERT_NE(tint, result.document.properties.end()) << "the emissive color was dropped";
    const auto* rgb = std::get_if<std::vector<float>>(&tint->second);
    ASSERT_NE(rgb, nullptr);
    ASSERT_EQ(rgb->size(), 3u);
    EXPECT_NEAR((*rgb)[0], 1.0f, 1e-6f);
    EXPECT_NEAR((*rgb)[1], 0.25f, 1e-6f);
    EXPECT_NEAR((*rgb)[2], 0.125f, 1e-6f);
    const auto luminance = result.document.properties.find("emissionLuminance");
    ASSERT_NE(luminance, result.document.properties.end());
    EXPECT_NEAR(std::get<float>(luminance->second), 406.0f, 1e-3f);
}

// An imported emission is physical: the document carries the default exposure weight beside it,
// whatever the source file declares (glTF KHR_materials_emissive_strength included).
TEST(ModelMaterialBridgeTest, Convert_ImportedEmissionIsPhysical)
{
    ImportedMaterialData mat = MakeDefaultPBRMaterial();
    mat.EmissiveColor[0] = 9.85f;
    const auto result = ModelMaterialBridge::Convert(GUID::Generate(), 0, mat);
    const auto it = result.document.properties.find("emissiveExposureWeight");
    ASSERT_NE(it, result.document.properties.end());
    EXPECT_EQ(std::get<float>(it->second), kDefaultEmissiveExposureWeight);
}

// The surface multiplies the emissive texture by the emission color, so a textured emitter keeps its color
// and luminance beside the texture, and stays on the standard surface with its metallic-roughness map.
TEST(ModelMaterialBridgeTest, Convert_EmissiveTexture_KeepsTheColorAndLuminanceBesideIt)
{
    ImportedMaterialData textured = MakeDefaultPBRMaterial();
    textured.EmissiveColor[0] = 1.0f;
    textured.EmissiveColor[1] = 0.5f;
    textured.EmissiveColor[2] = 0.25f;
    textured.EmissiveTexture = "Textures/emission.png";
    textured.SpecularTexture = "Textures/metallic_roughness.png";
    const auto result = ModelMaterialBridge::Convert(GUID::Generate(), 0, textured);

    EXPECT_EQ(result.document.textures.at("emissiveMap"), "Textures/emission.png");
    const auto* rgb = std::get_if<std::vector<float>>(&result.document.properties.at("emissive"));
    ASSERT_NE(rgb, nullptr);
    EXPECT_EQ(*rgb, (std::vector<float>{1.0f, 0.5f, 0.25f}));
    EXPECT_NEAR(std::get<float>(result.document.properties.at("emissionLuminance")), 203.0f, 1e-3f);
    EXPECT_EQ(result.document.surfaceShader, "Surfaces/standard_pbr.glsl");
    EXPECT_EQ(result.document.textures.at("metallicRoughnessMap"), "Textures/metallic_roughness.png");
}

// A black emissive color writes nothing, with or without a texture: the defaults (luminance 0) emit nothing.
TEST(ModelMaterialBridgeTest, Convert_EmissiveColor_WritesNothingForABlackEmitter)
{
    ImportedMaterialData textured = MakeDefaultPBRMaterial();
    textured.EmissiveTexture = "Textures/emission.png";
    for (const ImportedMaterialData& material : {MakeDefaultPBRMaterial(), textured})
    {
        const auto black = ModelMaterialBridge::Convert(GUID::Generate(), 0, material);
        EXPECT_FALSE(black.document.properties.contains("emissive"));
        EXPECT_FALSE(black.document.properties.contains("emissionLuminance"));
    }
}

// Negative, NaN and infinite components from a file read 0; the rest of the color still converts.
TEST(ModelMaterialBridgeTest, Convert_EmissiveColor_CleansNegativeAndNonFiniteComponents)
{
    ImportedMaterialData mat = MakeDefaultPBRMaterial();
    mat.EmissiveColor[0] = std::numeric_limits<float>::quiet_NaN();
    mat.EmissiveColor[1] = 0.5f;
    mat.EmissiveColor[2] = -3.0f;
    const auto mixed = ModelMaterialBridge::Convert(GUID::Generate(), 0, mat);
    const auto* rgb = std::get_if<std::vector<float>>(&mixed.document.properties.at("emissive"));
    ASSERT_NE(rgb, nullptr);
    EXPECT_EQ(*rgb, (std::vector<float>{0.0f, 1.0f, 0.0f}));
    EXPECT_NEAR(std::get<float>(mixed.document.properties.at("emissionLuminance")), 101.5f, 1e-3f);

    mat.EmissiveColor[1] = std::numeric_limits<float>::infinity();
    const auto unusable = ModelMaterialBridge::Convert(GUID::Generate(), 0, mat);
    EXPECT_FALSE(unusable.document.properties.contains("emissive"));
    EXPECT_FALSE(unusable.document.properties.contains("emissionLuminance"));
}

// Derived GUID is stable: same inputs produce same GUID.
TEST(ModelMaterialBridgeTest, DerivedGuid_IsStable)
{
    GUID modelGuid = GUID::Generate();

    GUID derived1 = ModelMaterialBridge::DeriveMaterialGuid(modelGuid, 0);
    GUID derived2 = ModelMaterialBridge::DeriveMaterialGuid(modelGuid, 0);

    EXPECT_EQ(derived1, derived2);
    EXPECT_FALSE(derived1.IsNull());
}

// Derived GUIDs for different material indices are different.
TEST(ModelMaterialBridgeTest, DerivedGuid_DiffersByIndex)
{
    GUID modelGuid = GUID::Generate();

    GUID derived0 = ModelMaterialBridge::DeriveMaterialGuid(modelGuid, 0);
    GUID derived1 = ModelMaterialBridge::DeriveMaterialGuid(modelGuid, 1);

    EXPECT_NE(derived0, derived1);
}

// Derived GUIDs for different models are different.
TEST(ModelMaterialBridgeTest, DerivedGuid_DiffersByModel)
{
    GUID modelA = GUID::Generate();
    GUID modelB = GUID::Generate();

    GUID derivedA = ModelMaterialBridge::DeriveMaterialGuid(modelA, 0);
    GUID derivedB = ModelMaterialBridge::DeriveMaterialGuid(modelB, 0);

    EXPECT_NE(derivedA, derivedB);
}

// ConvertAll handles multiple materials.
TEST(ModelMaterialBridgeTest, ConvertAll_MultiMaterial)
{
    GUID modelGuid = GUID::Generate();
    std::vector<ImportedMaterialData> materials;

    ImportedMaterialData m0 = MakeDefaultPBRMaterial();
    m0.Name = "Body";
    m0.Metallic = 0.0f;
    materials.push_back(m0);

    ImportedMaterialData m1 = MakeDefaultPBRMaterial();
    m1.Name = "Metal";
    m1.Metallic = 1.0f;
    m1.Roughness = 0.2f;
    materials.push_back(m1);

    ImportedMaterialData m2 = MakeDefaultPBRMaterial();
    m2.Name = "Glass";
    m2.AlphaMode = AlphaMode::Blend;
    materials.push_back(m2);

    auto results = ModelMaterialBridge::ConvertAll(modelGuid, materials);

    ASSERT_EQ(results.size(), 3u);

    EXPECT_EQ(results[0].materialIndex, 0u);
    EXPECT_EQ(results[1].materialIndex, 1u);
    EXPECT_EQ(results[2].materialIndex, 2u);

    EXPECT_EQ(results[0].document.materialName, "Body");
    EXPECT_EQ(results[1].document.materialName, "Metal");
    EXPECT_EQ(results[2].document.materialName, "Glass");

    // All derived GUIDs should be unique.
    EXPECT_NE(results[0].derivedGuid, results[1].derivedGuid);
    EXPECT_NE(results[1].derivedGuid, results[2].derivedGuid);

    EXPECT_EQ(results[2].document.alphaMode, MaterialAlphaMode::Blend);
}

// Material with no name gets a generated name.
TEST(ModelMaterialBridgeTest, Convert_NoName_GeneratesName)
{
    GUID modelGuid = GUID::Generate();
    ImportedMaterialData mat = MakeDefaultPBRMaterial();
    mat.Name = "";

    auto result = ModelMaterialBridge::Convert(modelGuid, 3, mat);

    EXPECT_FALSE(result.document.materialName.empty());
    EXPECT_NE(result.document.materialName.find("3"), std::string::npos)
        << "Generated name should contain the material index";
}
