// Tests for MaterialAsset JSON parsing: v2/v3 schema compat, GUID fields,
// alpha mode, properties, textures, and error handling.

#include <gtest/gtest.h>

#include "Assets/MaterialAsset.h"
#include "AssetCore/GUID.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/SurfaceShaderTemplate.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using namespace GameEngine;

namespace {
bool ParseMaterialJson(const std::string& json, MaterialAsset& asset)
{
    std::vector<uint8_t> data(json.begin(), json.end());
    return asset.LoadFromData(data);
}
} // namespace

// ---- Schema version 2 (legacy) ----

TEST(MaterialAssetParsingTest, V2_BasicPBR_ParsesCorrectly)
{
    const std::string json = R"JSON({
        "schemaVersion": 2,
        "materialName": "TestPBR",
        "lightingModel": "StandardPBR",
        "surfaceShader": "Surfaces/standard_pbr.glsl",
        "properties": {
            "baseColor": [1.0, 0.5, 0.0, 1.0],
            "roughness": 0.5,
            "metallic": 0.0
        },
        "textures": {}
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));

    const auto& doc = asset.GetDocument();
    EXPECT_EQ(doc.schemaVersion, 2);
    EXPECT_EQ(doc.materialName, "TestPBR");
    EXPECT_EQ(doc.lightingModel, "StandardPBR");
    EXPECT_EQ(doc.surfaceShader, "Surfaces/standard_pbr.glsl");

    auto itBC = doc.properties.find("baseColor");
    ASSERT_NE(itBC, doc.properties.end());
    const auto* bc = std::get_if<std::vector<float>>(&itBC->second);
    ASSERT_NE(bc, nullptr);
    ASSERT_EQ(bc->size(), 4u);
    EXPECT_NEAR((*bc)[0], 1.0f, 1e-5f);
    EXPECT_NEAR((*bc)[1], 0.5f, 1e-5f);

    auto itR = doc.properties.find("roughness");
    ASSERT_NE(itR, doc.properties.end());
    EXPECT_NEAR(std::get<float>(itR->second), 0.5f, 1e-5f);
}

TEST(MaterialAssetParsingTest, V2_MissingSurfaceShader_Fails)
{
    const std::string json = R"JSON({
        "schemaVersion": 2,
        "materialName": "NoShader",
        "lightingModel": "StandardPBR",
        "properties": {}
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    EXPECT_FALSE(ParseMaterialJson(json, asset));
    EXPECT_FALSE(asset.GetErrors().empty());
}

TEST(MaterialAssetParsingTest, V2_DoesNotPopulateGuidFields)
{
    const std::string json = R"JSON({
        "schemaVersion": 2,
        "materialName": "Legacy",
        "lightingModel": "StandardPBR",
        "surfaceShader": "Surfaces/standard_pbr.glsl",
        "properties": {}
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));

    const auto& doc = asset.GetDocument();
    EXPECT_TRUE(doc.surfaceShaderGuid.empty());
    EXPECT_TRUE(doc.vertexModifierGuid.empty());
}

// ---- Schema version 3 ----

TEST(MaterialAssetParsingTest, V3_BasicPBR_ParsesCorrectly)
{
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "PBR_v3",
        "lightingModel": "StandardPBR",
        "surfaceShaderGuid": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
        "alphaMode": "Opaque",
        "doubleSided": false,
        "properties": {
            "baseColor": [1.0, 1.0, 1.0, 1.0],
            "roughness": 0.5,
            "metallic": 0.0,
            "emissive": [0.0, 0.0, 0.0],
            "emissionLuminance": 203.0,
            "ao": 1.0,
            "opacity": 1.0
        },
        "textures": {}
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));

    const auto& doc = asset.GetDocument();
    EXPECT_EQ(doc.schemaVersion, 3);
    EXPECT_EQ(doc.materialName, "PBR_v3");
    EXPECT_EQ(doc.lightingModel, "StandardPBR");
    EXPECT_EQ(doc.surfaceShaderGuid, "a1b2c3d4-e5f6-7890-abcd-ef1234567890");
    EXPECT_EQ(doc.alphaMode, MaterialAlphaMode::Opaque);
    EXPECT_FALSE(doc.doubleSided);

    // surfaceShader path not in this JSON; reconciliation fills it at compile time
    EXPECT_TRUE(doc.surfaceShader.empty());
}

TEST(MaterialAssetParsingTest, V3_NoSurfaceShader_Succeeds)
{
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "NoShader_v3",
        "lightingModel": "StandardPBR",
        "properties": {
            "baseColor": [1.0, 1.0, 1.0, 1.0]
        }
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));

    const auto& doc = asset.GetDocument();
    EXPECT_EQ(doc.schemaVersion, 3);
    EXPECT_TRUE(doc.surfaceShaderGuid.empty());
    EXPECT_TRUE(doc.surfaceShader.empty());
}

TEST(MaterialAssetParsingTest, UnityMat_ConvertsToPbrDocument)
{
    const std::string unityMat = R"MAT(%YAML 1.1
--- !u!21 &2100000
Material:
  m_Name: Pterodactyl
  stringTagMap:
    RenderType: Opaque
  m_SavedProperties:
    m_TexEnvs:
    - _BaseMap:
        m_Texture: {fileID: 2800000, guid: 16e117e2a1488024d8229adaad6c0e63, type: 3}
        m_Scale: {x: 1, y: 1}
        m_Offset: {x: 0, y: 0}
    - _BumpMap:
        m_Texture: {fileID: 2800000, guid: 796454f5d1b896c4d99e31c51f57b5da, type: 3}
        m_Scale: {x: 1, y: 1}
        m_Offset: {x: 0, y: 0}
    m_Floats:
    - _Glossiness: 0.25
    - _Metallic: 0
    m_Colors:
    - _Color: {r: 0.6, g: 0.5, b: 0.4, a: 1}
)MAT";

    MaterialAsset asset(GUID::Generate(), "Pterodactyl.mat");
    ASSERT_TRUE(ParseMaterialJson(unityMat, asset)) << asset.GetErrors().front();

    const auto& doc = asset.GetDocument();
    EXPECT_EQ(doc.schemaVersion, 3);
    EXPECT_EQ(doc.materialName, "Pterodactyl");
    EXPECT_EQ(doc.surfaceShader, "Surfaces/standard_pbr.glsl");
    EXPECT_EQ(doc.alphaMode, MaterialAlphaMode::Opaque);

    auto baseIt = doc.textures.find("albedoMap");
    ASSERT_NE(baseIt, doc.textures.end());
    EXPECT_EQ(baseIt->second, "16e117e2-a148-8024-d822-9adaad6c0e63");

    auto normalIt = doc.textures.find("normalMap");
    ASSERT_NE(normalIt, doc.textures.end());
    EXPECT_EQ(normalIt->second, "796454f5-d1b8-96c4-d99e-31c51f57b5da");

    auto roughnessIt = doc.properties.find("roughness");
    ASSERT_NE(roughnessIt, doc.properties.end());
    EXPECT_NEAR(std::get<float>(roughnessIt->second), 0.75f, 1e-5f);
}

TEST(MaterialAssetParsingTest, UnityMat_TransparentRenderTypeUsesBlend)
{
    const std::string unityMat = R"MAT(%YAML 1.1
--- !u!21 &2100000
Material:
  m_Name: Eye Shine
  stringTagMap:
    RenderType: Transparent
  m_SavedProperties:
    m_TexEnvs:
    - _MainTex:
        m_Texture: {fileID: 2800000, guid: 26667450ec0f65549916b07e332c0689, type: 3}
        m_Scale: {x: 1, y: 1}
        m_Offset: {x: 0, y: 0}
    m_Floats:
    - _Mode: 2
    m_Colors:
    - _Color: {r: 1, g: 1, b: 1, a: 0.5}
)MAT";

    MaterialAsset asset(GUID::Generate(), "Eye Shine.mat");
    ASSERT_TRUE(ParseMaterialJson(unityMat, asset)) << asset.GetErrors().front();

    const auto& doc = asset.GetDocument();
    EXPECT_EQ(doc.materialName, "Eye Shine");
    EXPECT_EQ(doc.alphaMode, MaterialAlphaMode::Blend);
    EXPECT_EQ(doc.textures.at("albedoMap"), "26667450-ec0f-6554-9916-b07e332c0689");
}

TEST(MaterialAssetParsingTest, UnityMat_EmissionMapEmitsAtReferenceWhite)
{
    const std::string unityMat = R"MAT(%YAML 1.1
--- !u!21 &2100000
Material:
  m_Name: Lantern Glow
  m_SavedProperties:
    m_TexEnvs:
    - _EmissionMap:
        m_Texture: {fileID: 2800000, guid: 26667450ec0f65549916b07e332c0689, type: 3}
        m_Scale: {x: 1, y: 1}
        m_Offset: {x: 0, y: 0}
)MAT";

    MaterialAsset asset(GUID::Generate(), "Lantern Glow.mat");
    ASSERT_TRUE(ParseMaterialJson(unityMat, asset)) << asset.GetErrors().front();

    const auto& doc = asset.GetDocument();
    EXPECT_EQ(doc.textures.at("emissiveMap"), "26667450-ec0f-6554-9916-b07e332c0689");
    EXPECT_EQ(std::get<std::vector<float>>(doc.properties.at("emissive")), (std::vector<float>{1.0f, 1.0f, 1.0f}));
    EXPECT_FLOAT_EQ(std::get<float>(doc.properties.at("emissionLuminance")), 203.0f);
}

// The surface multiplies the emissive texture by the emission color and luminance, so a committed
// material that binds the texture under a black color or a zero luminance draws no emission at all.
TEST(MaterialAssetParsingTest, CommittedMaterialsWithAnEmissiveMapEmit)
{
#ifndef GE_RENDERER_REPO_ROOT
    GTEST_SKIP() << "GE_RENDERER_REPO_ROOT not defined (dev-only source anchor)";
#else
    namespace fs = std::filesystem;
    size_t materials = 0;
    size_t emissiveMaps = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(GE_RENDERER_REPO_ROOT, ec); it != fs::recursive_directory_iterator();
         it.increment(ec))
    {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec) && (name.starts_with("build") || name.starts_with(".") || name == "dependencies" ||
                                     name == "node_modules"))
        {
            it.disable_recursion_pending();
            continue;
        }
        if (it->path().extension() != ".material")
            continue;
        std::ifstream in(it->path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        MaterialAsset asset(GUID::Generate(), it->path());
        ASSERT_TRUE(ParseMaterialJson(text, asset)) << it->path().string();
        ++materials;
        const MaterialDocument& doc = asset.GetDocument();
        if (!doc.textures.contains("emissiveMap") || doc.textures.at("emissiveMap").empty())
            continue;
        ++emissiveMaps;
        const auto color = doc.properties.find("emissive");
        const auto* rgb = color != doc.properties.end() ? std::get_if<std::vector<float>>(&color->second) : nullptr;
        EXPECT_TRUE(rgb == nullptr || std::any_of(rgb->begin(), rgb->end(), [](float c) { return c > 0.0f; }))
            << it->path().string() << " binds an emissive map under a black emission color";
        const auto luminance = doc.properties.find("emissionLuminance");
        const float* nits = luminance != doc.properties.end() ? std::get_if<float>(&luminance->second) : nullptr;
        EXPECT_TRUE(nits != nullptr && *nits > 0.0f)
            << it->path().string() << " binds an emissive map at zero emission luminance";
    }
    EXPECT_GT(materials, 50u) << "the sweep found too few committed materials under " << GE_RENDERER_REPO_ROOT;
    EXPECT_GE(emissiveMaps, 1u) << "no committed material binds an emissive map; the sweep checks nothing";
#endif
}

TEST(MaterialAssetParsingTest, V3_AllGuidFields)
{
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "FullGuids",
        "lightingModel": "StandardPBR",
        "surfaceShaderGuid": "aaaa-bbbb",
        "vertexModifierGuid": "cccc-dddd",
        "properties": {}
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));

    const auto& doc = asset.GetDocument();
    EXPECT_EQ(doc.surfaceShaderGuid, "aaaa-bbbb");
    EXPECT_EQ(doc.vertexModifierGuid, "cccc-dddd");
}

// ---- Alpha mode ----

TEST(MaterialAssetParsingTest, AlphaMode_Blend)
{
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "BlendMat",
        "lightingModel": "StandardPBR",
        "alphaMode": "Blend",
        "properties": {}
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));
    EXPECT_EQ(asset.GetDocument().alphaMode, MaterialAlphaMode::Blend);
}

TEST(MaterialAssetParsingTest, AlphaMode_Mask)
{
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "MaskMat",
        "lightingModel": "StandardPBR",
        "alphaMode": "Mask",
        "properties": {}
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));
    EXPECT_EQ(asset.GetDocument().alphaMode, MaterialAlphaMode::Mask);
}

TEST(MaterialAssetParsingTest, AlphaMode_DefaultsToOpaque)
{
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "DefaultAlpha",
        "lightingModel": "StandardPBR",
        "properties": {}
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));
    EXPECT_EQ(asset.GetDocument().alphaMode, MaterialAlphaMode::Opaque);
}

// ---- Mask preservation at load ----
//
// Mask demotion is a RUNTIME decision (MaterialSystem::RegisterMaterialFromDocument
// demotes a registration-local copy). The parsed document must keep the
// authored Mask verbatim: editor save paths serialize m_Doc, so a load-time
// demotion would bake Opaque into the asset on the next save — with no
// self-heal when the albedo texture later gains alpha.

namespace {
std::string ProvablyOpaqueMaskJson()
{
    return R"JSON({
        "schemaVersion": 3,
        "materialName": "MaskPreserve",
        "lightingModel": "StandardPBR",
        "surfaceShader": "Surfaces/standard_pbr.glsl",
        "ignoreVertexColor": true,
        "alphaMode": "Mask",
        "properties": { "baseColor": [1, 1, 1, 1], "alphaCutoff": 0.5 }})JSON";
}
} // namespace

TEST(MaterialAssetParsingTest, Mask_ProvablyOpaqueDocumentKeepsAuthoredMaskAtLoad)
{
    // This document satisfies every runtime demotion gate (builtin surface,
    // vertex color ignored, opaque baseColor, default cutoff, no albedo
    // texture) — and must STILL parse as Mask.
    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(ProvablyOpaqueMaskJson(), asset));
    EXPECT_EQ(asset.GetDocument().alphaMode, MaterialAlphaMode::Mask);
}

TEST(MaterialAssetParsingTest, Mask_SurvivesSerializeRoundTrip)
{
    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(ProvablyOpaqueMaskJson(), asset));

    const nlohmann::json serialized = SerializeMaterialDocument(asset.GetDocument());
    ASSERT_TRUE(serialized.contains("alphaMode"));
    EXPECT_EQ(serialized["alphaMode"].get<std::string>(), "Mask");

    MaterialAsset reparsed(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(serialized.dump(), reparsed));
    EXPECT_EQ(reparsed.GetDocument().alphaMode, MaterialAlphaMode::Mask);
}

// ---- Double sided ----

TEST(MaterialAssetParsingTest, DoubleSided_True)
{
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "DS",
        "lightingModel": "StandardPBR",
        "doubleSided": true,
        "properties": {}
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));
    EXPECT_TRUE(asset.GetDocument().doubleSided);
}

TEST(MaterialAssetParsingTest, DoubleSided_DefaultsFalse)
{
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "NotDS",
        "lightingModel": "StandardPBR",
        "properties": {}
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));
    EXPECT_FALSE(asset.GetDocument().doubleSided);
}

// ---- Properties: various types ----

TEST(MaterialAssetParsingTest, Properties_BoolIntFloat)
{
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "TypeTest",
        "lightingModel": "StandardPBR",
        "properties": {
            "useDetail": true,
            "layerCount": 3,
            "roughness": 0.42
        }
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));

    const auto& props = asset.GetDocument().properties;

    auto itBool = props.find("useDetail");
    ASSERT_NE(itBool, props.end());
    EXPECT_TRUE(std::get<bool>(itBool->second));

    auto itInt = props.find("layerCount");
    ASSERT_NE(itInt, props.end());
    EXPECT_EQ(std::get<int32_t>(itInt->second), 3);

    auto itFloat = props.find("roughness");
    ASSERT_NE(itFloat, props.end());
    EXPECT_NEAR(std::get<float>(itFloat->second), 0.42f, 1e-5f);
}

TEST(MaterialAssetParsingTest, Properties_FloatArray)
{
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "ArrayTest",
        "lightingModel": "StandardPBR",
        "properties": {
            "emissive": [0.5, 0.6, 0.7]
        }
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));

    auto it = asset.GetDocument().properties.find("emissive");
    ASSERT_NE(it, asset.GetDocument().properties.end());
    const auto* arr = std::get_if<std::vector<float>>(&it->second);
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->size(), 3u);
    EXPECT_NEAR((*arr)[0], 0.5f, 1e-5f);
    EXPECT_NEAR((*arr)[1], 0.6f, 1e-5f);
    EXPECT_NEAR((*arr)[2], 0.7f, 1e-5f);
}

// ---- opacity / baseColor.a (two spellings of one quantity) ----
//
// StandardPBR docs get the full property set seeded so the inspector shows every
// control. `opacity` is the one seeded key that duplicates authored data
// (baseColor.a), so its default must be derived, never the factory 1.0 —
// otherwise the seed overwrites the authored alpha at registration
// (SyncMaterialOpacityFromDocument) and blinds the Mask->Opaque demotion gate.

namespace {
float ParsedOpacity(const MaterialAsset& asset)
{
    auto it = asset.GetDocument().properties.find("opacity");
    EXPECT_NE(it, asset.GetDocument().properties.end());
    if (it == asset.GetDocument().properties.end())
        return -1.0f;
    const float* v = std::get_if<float>(&it->second);
    EXPECT_NE(v, nullptr);
    return v ? *v : -1.0f;
}
} // namespace

TEST(MaterialAssetParsingTest, Opacity_AbsentTakesAuthoredBaseColorAlpha)
{
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "MaskFade",
        "lightingModel": "StandardPBR",
        "alphaMode": "Mask",
        "properties": {
            "baseColor": [1.0, 1.0, 1.0, 0.0705882],
            "alphaCutoff": 0.5
        }
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));

    EXPECT_NEAR(ParsedOpacity(asset), 0.0705882f, 1e-6f);
}

// An authored integer spelling of the relief depth (0 turns the march off) stays a float, like every
// other float slider: JSON cannot tell 0 from 0.0.
TEST(MaterialAssetParsingTest, ReliefDepth_IntegerSpellingParsesAsFloat)
{
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "FlatRelief",
        "lightingModel": "StandardPBR",
        "surfaceShader": "Surfaces/standard_pbr.glsl",
        "properties": {
            "reliefDepth": 0
        }
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));
    const auto it = asset.GetDocument().properties.find("reliefDepth");
    ASSERT_NE(it, asset.GetDocument().properties.end());
    const float* depth = std::get_if<float>(&it->second);
    ASSERT_NE(depth, nullptr) << "reliefDepth parsed as a non-float";
    EXPECT_EQ(*depth, 0.0f);
}

// The emission's exposure weight survives a save and a reload, the integer spelling of its 0 end
// included, and a document that authors none reads as physical.
TEST(MaterialAssetParsingTest, EmissiveExposureWeight_RoundTripsAndDefaultsToPhysical)
{
    const auto parsedWeight = [](const std::string& json) {
        MaterialAsset asset(GUID::Generate(), "test.material");
        EXPECT_TRUE(ParseMaterialJson(json, asset));
        const auto it = asset.GetDocument().properties.find("emissiveExposureWeight");
        const float* weight = it == asset.GetDocument().properties.end() ? nullptr : std::get_if<float>(&it->second);
        return weight ? std::optional<float>(*weight) : std::nullopt;
    };
    const std::string authored = R"JSON({"schemaVersion": 3, "materialName": "Sign", "lightingModel": "StandardPBR",
        "surfaceShader": "Surfaces/standard_pbr.glsl", "properties": {"emissiveExposureWeight": 0}})JSON";
    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(authored, asset));
    EXPECT_EQ(parsedWeight(authored), std::optional<float>(0.0f)) << "the integer 0 parsed as a non-float";
    EXPECT_EQ(parsedWeight(SerializeMaterialDocument(asset.GetDocument()).dump()), std::optional<float>(0.0f));

    const std::string unauthored = R"JSON({"schemaVersion": 3, "materialName": "Lamp", "lightingModel": "StandardPBR",
        "surfaceShader": "Surfaces/standard_pbr.glsl", "properties": {}})JSON";
    EXPECT_EQ(parsedWeight(unauthored), std::optional<float>(kDefaultEmissiveExposureWeight));
}

TEST(MaterialAssetParsingTest, Opacity_AuthoredWinsOverBaseColorAlpha)
{
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "BlendAuthored",
        "lightingModel": "StandardPBR",
        "alphaMode": "Blend",
        "properties": {
            "baseColor": [1.0, 1.0, 1.0, 0.25],
            "opacity": 0.75
        }
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));

    EXPECT_NEAR(ParsedOpacity(asset), 0.75f, 1e-6f);
}

TEST(MaterialAssetParsingTest, Opacity_ThreeComponentBaseColorDefaultsToOne)
{
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "NoAlphaChannel",
        "lightingModel": "StandardPBR",
        "properties": {
            "baseColor": [0.2, 0.4, 0.6]
        }
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));

    EXPECT_NEAR(ParsedOpacity(asset), 1.0f, 1e-6f);
}

TEST(MaterialAssetParsingTest, Opacity_OpaqueAlphaOneIsUnchanged)
{
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "TheNinetyNinePercent",
        "lightingModel": "StandardPBR",
        "properties": {
            "baseColor": [0.8, 0.8, 0.8, 1.0]
        }
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));

    EXPECT_NEAR(ParsedOpacity(asset), 1.0f, 1e-6f);
}

// ---- FillMissingStandardPBRDefaults ----
//
// The inspector seeds a document through this when the user changes Lighting Model,
// and that path SAVES. Seeding a constant 1.0 there would destroy an authored alpha
// on a document that never spelled it `opacity` — the same defect as the parse path,
// reached by a different trigger. These pin the seeding rule directly, independent of
// the parser, so dropping the derivation from either caller fails a test.

namespace {
float DocOpacity(const MaterialDocument& doc)
{
    auto it = doc.properties.find("opacity");
    EXPECT_NE(it, doc.properties.end());
    if (it == doc.properties.end())
        return -1.0f;
    const float* v = std::get_if<float>(&it->second);
    EXPECT_NE(v, nullptr);
    return v ? *v : -1.0f;
}
} // namespace

TEST(MaterialDocumentDefaultsTest, FillDerivesOpacityFromAuthoredBaseColorAlpha)
{
    MaterialDocument doc{};
    doc.lightingModel = "StandardPBR";
    doc.properties["baseColor"] = std::vector<float>{1.0f, 1.0f, 1.0f, 0.25f};

    doc.FillMissingStandardPBRDefaults();

    EXPECT_NEAR(DocOpacity(doc), 0.25f, 1e-6f);
}

TEST(MaterialDocumentDefaultsTest, FillPreservesAuthoredOpacity)
{
    MaterialDocument doc{};
    doc.lightingModel = "StandardPBR";
    doc.properties["baseColor"] = std::vector<float>{1.0f, 1.0f, 1.0f, 0.25f};
    doc.properties["opacity"] = 0.75f;

    doc.FillMissingStandardPBRDefaults();

    EXPECT_NEAR(DocOpacity(doc), 0.75f, 1e-6f);
}

// A doc arriving from Unlit / ShadowOnly / MaterialX carries baseColor.a but never
// spells `opacity`; switching it to StandardPBR must not invent 1.0 over that alpha.
TEST(MaterialDocumentDefaultsTest, FillOnLightingModelSwitchKeepsForeignAlpha)
{
    MaterialDocument doc{};
    doc.lightingModel = "StandardPBR"; // just switched from Unlit by the inspector
    doc.properties["baseColor"] = std::vector<float>{0.2f, 0.4f, 0.6f, 0.4f};

    doc.FillMissingStandardPBRDefaults();

    EXPECT_NEAR(DocOpacity(doc), 0.4f, 1e-6f);
    // ...and the rest of the control set is now present
    EXPECT_NE(doc.properties.find("roughness"), doc.properties.end());
    EXPECT_NE(doc.properties.find("specularIor"), doc.properties.end());
}

TEST(MaterialDocumentDefaultsTest, FillSkipsPbrScalarsForUnlit)
{
    MaterialDocument doc{};
    doc.lightingModel = "Unlit";
    doc.properties["baseColor"] = std::vector<float>{1.0f, 1.0f, 1.0f, 0.5f};

    doc.FillMissingStandardPBRDefaults();

    EXPECT_EQ(doc.properties.find("roughness"), doc.properties.end());
    EXPECT_EQ(doc.properties.find("metallic"), doc.properties.end());
    EXPECT_EQ(doc.properties.find("ao"), doc.properties.end());
    EXPECT_NEAR(DocOpacity(doc), 0.5f, 1e-6f);
}

TEST(MaterialDocumentDefaultsTest, FreshDefaultDocumentIsFullyOpaque)
{
    const MaterialDocument doc = MaterialDocument::CreateDefaultPBR("New");
    EXPECT_NEAR(DocOpacity(doc), 1.0f, 1e-6f);
    EXPECT_NEAR(doc.DefaultOpacity(), 1.0f, 1e-6f);
}

// ---- Textures ----

TEST(MaterialAssetParsingTest, Textures_StringAndNull)
{
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "TexTest",
        "lightingModel": "StandardPBR",
        "properties": {},
        "textures": {
            "albedoMap": "some-guid-string",
            "normalMap": null
        }
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));

    const auto& tex = asset.GetDocument().textures;
    ASSERT_EQ(tex.count("albedoMap"), 1u);
    EXPECT_EQ(tex.at("albedoMap"), "some-guid-string");
    ASSERT_EQ(tex.count("normalMap"), 1u);
    EXPECT_TRUE(tex.at("normalMap").empty());
}

// ---- Bindings ----

TEST(MaterialAssetParsingTest, Bindings_Parsed)
{
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "BindTest",
        "lightingModel": "StandardPBR",
        "properties": {},
        "bindings": {
            "uWindSpeed": "windSpeed",
            "uSwayAmount": "swayAmount"
        }
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));

    const auto& b = asset.GetDocument().bindings;
    EXPECT_EQ(b.at("uWindSpeed"), "windSpeed");
    EXPECT_EQ(b.at("uSwayAmount"), "swayAmount");
}

// ---- Error handling ----

TEST(MaterialAssetParsingTest, InvalidJson_Fails)
{
    const std::string json = "not valid json {{{";

    MaterialAsset asset(GUID::Generate(), "test.material");
    EXPECT_FALSE(ParseMaterialJson(json, asset));
    EXPECT_FALSE(asset.GetErrors().empty());
}

TEST(MaterialAssetParsingTest, UnsupportedSchemaVersion_Fails)
{
    const std::string json = R"JSON({
        "schemaVersion": 99,
        "materialName": "Future",
        "properties": {}
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    EXPECT_FALSE(ParseMaterialJson(json, asset));
}

TEST(MaterialAssetParsingTest, EmptyInput_Fails)
{
    MaterialAsset asset(GUID::Generate(), "test.material");
    EXPECT_FALSE(ParseMaterialJson("", asset));
}

// ---- Lighting model casing ----

TEST(MaterialAssetParsingTest, LightingModel_PreservesCasing)
{
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "CaseTest",
        "lightingModel": "unlit",
        "properties": {}
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));
    EXPECT_EQ(asset.GetDocument().lightingModel, "unlit");
}

// ---- V3 dual serialization: both GUID and path fields ----

TEST(MaterialAssetParsingTest, V3_BothGuidAndPath_Parsed)
{
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "DualRef",
        "lightingModel": "StandardPBR",
        "surfaceShaderGuid": "aaaa-bbbb",
        "surfaceShader": "Surfaces/standard_pbr.glsl",
        "vertexModifierGuid": "cccc-dddd",
        "vertexModifier": "Surfaces/tree_wind.glsl",
        "properties": {}
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));

    const auto& doc = asset.GetDocument();
    EXPECT_EQ(doc.surfaceShaderGuid, "aaaa-bbbb");
    EXPECT_EQ(doc.surfaceShader, "Surfaces/standard_pbr.glsl");
    EXPECT_EQ(doc.vertexModifierGuid, "cccc-dddd");
    EXPECT_EQ(doc.vertexModifier, "Surfaces/tree_wind.glsl");
}

TEST(MaterialAssetParsingTest, V3_GuidOnly_PathEmpty)
{
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "GuidOnly",
        "lightingModel": "StandardPBR",
        "surfaceShaderGuid": "aaaa-bbbb",
        "properties": {}
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));

    const auto& doc = asset.GetDocument();
    EXPECT_EQ(doc.surfaceShaderGuid, "aaaa-bbbb");
    EXPECT_TRUE(doc.surfaceShader.empty());
}

TEST(MaterialAssetParsingTest, V3_PathOnly_GuidEmpty)
{
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "PathOnly",
        "lightingModel": "StandardPBR",
        "surfaceShader": "Surfaces/standard_pbr.glsl",
        "properties": {}
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));

    const auto& doc = asset.GetDocument();
    EXPECT_TRUE(doc.surfaceShaderGuid.empty());
    EXPECT_EQ(doc.surfaceShader, "Surfaces/standard_pbr.glsl");
}

// ---- V2 backward compat: existing assets still load ----

TEST(MaterialAssetParsingTest, V2_UnlitWhite_Pattern)
{
    const std::string json = R"JSON({
        "schemaVersion": 2,
        "materialName": "Unlit White (M0)",
        "lightingModel": "unlit",
        "surfaceShader": "Surfaces/unlit_solid.glsl",
        "properties": {
            "baseColor": [1.0, 1.0, 1.0, 1.0]
        },
        "textures": {}
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));

    const auto& doc = asset.GetDocument();
    EXPECT_EQ(doc.schemaVersion, 2);
    EXPECT_EQ(doc.lightingModel, "unlit");
    EXPECT_EQ(doc.surfaceShader, "Surfaces/unlit_solid.glsl");
    EXPECT_TRUE(doc.surfaceShaderGuid.empty());
}

// ---- ignoreVertexColor (statue vertex-color fix) ----

TEST(MaterialAssetParsingTest, IgnoreVertexColor_ParsesTrue)
{
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "StatueBody",
        "lightingModel": "StandardPBR",
        "surfaceShader": "Surfaces/standard_pbr.glsl",
        "ignoreVertexColor": true
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));
    EXPECT_TRUE(asset.GetDocument().ignoreVertexColor);
}

TEST(MaterialAssetParsingTest, IgnoreVertexColor_DefaultsFalseWhenAbsent)
{
    // Absent key must decode to false so existing .material assets are unchanged.
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "Plain",
        "lightingModel": "StandardPBR",
        "surfaceShader": "Surfaces/standard_pbr.glsl"
    })JSON";

    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));
    EXPECT_FALSE(asset.GetDocument().ignoreVertexColor);
}

TEST(MaterialAssetParsingTest, IgnoreVertexColor_SerializeRoundTrip)
{
    MaterialDocument doc = MaterialDocument::CreateDefaultPBR("Statue");
    doc.ignoreVertexColor = true;

    const std::string text = SerializeMaterialDocument(doc).dump();
    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(text, asset));
    EXPECT_TRUE(asset.GetDocument().ignoreVertexColor);
}

TEST(MaterialAssetParsingTest, IgnoreVertexColor_AbsentFromJsonWhenFalse)
{
    // When false the key is omitted entirely, so a material that never sets it
    // serializes byte-identically to the pre-feature output.
    MaterialDocument doc = MaterialDocument::CreateDefaultPBR("Plain");
    doc.ignoreVertexColor = false;

    const nlohmann::json j = SerializeMaterialDocument(doc);
    EXPECT_FALSE(j.contains("ignoreVertexColor"));
}

// ---- User keywords (slice N): parse / sanitize / cap / serialize ----

TEST(MaterialAssetParsingTest, Keywords_SanitizeAndDedup)
{
    const std::string json = R"JSON({
        "schemaVersion": 3, "materialName": "Kw", "lightingModel": "StandardPBR",
        "surfaceShader": "s.glsl",
        "keywords": ["FLOW_MODE", "flow_mode", "HIGH_DETAIL"]
    })JSON";
    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));
    const auto& kw = asset.GetDocument().keywords;
    // "flow_mode" sanitizes to "FLOW_MODE" -> a duplicate, dropped.
    ASSERT_EQ(kw.size(), 2u);
    EXPECT_EQ(kw[0], "FLOW_MODE");
    EXPECT_EQ(kw[1], "HIGH_DETAIL");
}

TEST(MaterialAssetParsingTest, Keywords_RejectsInvalidAndReserved)
{
    const std::string json = R"JSON({
        "schemaVersion": 3, "materialName": "Kw", "lightingModel": "StandardPBR",
        "surfaceShader": "s.glsl",
        "keywords": ["ALPHA_TEST", "flow-mode", "", "9BAD", "FLOW_MODE"]
    })JSON";
    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));
    const auto& kw = asset.GetDocument().keywords;
    // ALPHA_TEST reserved, flow-mode has a hyphen, "" empty, 9BAD starts with a
    // digit -> only FLOW_MODE survives.
    ASSERT_EQ(kw.size(), 1u);
    EXPECT_EQ(kw[0], "FLOW_MODE");
}

TEST(MaterialAssetParsingTest, Keywords_CapAtEight)
{
    const std::string json = R"JSON({
        "schemaVersion": 3, "materialName": "Kw", "lightingModel": "StandardPBR",
        "surfaceShader": "s.glsl",
        "keywords": ["K0","K1","K2","K3","K4","K5","K6","K7","K8","K9"]
    })JSON";
    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));
    EXPECT_EQ(asset.GetDocument().keywords.size(), 8u);
}

TEST(MaterialAssetParsingTest, Keywords_SerializeRoundTrip)
{
    MaterialDocument doc = MaterialDocument::CreateDefaultPBR("Kw");
    doc.keywords = {"FLOW_MODE", "HIGH_DETAIL"};

    const std::string text = SerializeMaterialDocument(doc).dump();
    MaterialAsset asset(GUID::Generate(), "test.material");
    ASSERT_TRUE(ParseMaterialJson(text, asset));
    EXPECT_EQ(asset.GetDocument().keywords, doc.keywords);
}

TEST(MaterialAssetParsingTest, Keywords_AbsentFromJsonWhenEmpty)
{
    // No keywords -> the field is omitted, so keyword-free materials stay
    // byte-identical to the pre-feature output.
    MaterialDocument doc = MaterialDocument::CreateDefaultPBR("Plain");
    const nlohmann::json j = SerializeMaterialDocument(doc);
    EXPECT_FALSE(j.contains("keywords"));
}

// ---- Create → Surface Shader companion material ----

TEST(MaterialAssetParsingTest, SurfaceShaderTemplateMaterial_SerializeRoundTrip)
{
    // The editor writes the companion .material via SerializeMaterialDocument;
    // everything the template shader consumes (surface reference, keyword,
    // texture keys) must survive the disk round-trip, and the file stores no
    // property values — every parameter starts at its @property default.
    MaterialDocument doc = Rendering::MakeSurfaceShaderTemplateMaterial("NewSurface");
    doc.surfaceShaderGuid = GUID::Generate().ToString();

    const nlohmann::json serialized = SerializeMaterialDocument(doc);
    // The serializer drops an empty properties object; either spelling means
    // "no overrides".
    EXPECT_TRUE(!serialized.contains("properties") || serialized["properties"].empty())
        << serialized["properties"].dump();

    const std::string text = serialized.dump(2);
    MaterialAsset asset(GUID::Generate(), "NewSurface.material");
    ASSERT_TRUE(ParseMaterialJson(text, asset));

    const MaterialDocument& parsed = asset.GetDocument();
    EXPECT_EQ(parsed.surfaceShader, "NewSurface.glsl");
    EXPECT_EQ(parsed.surfaceShaderGuid, doc.surfaceShaderGuid);
    ASSERT_EQ(parsed.keywords.size(), 1u);
    EXPECT_EQ(parsed.keywords[0], "PULSE");
    EXPECT_NE(parsed.textures.find("albedoMap"), parsed.textures.end());
    EXPECT_NE(parsed.textures.find("accentMask"), parsed.textures.end());
}

// ---- Minimal walkthrough examples: smallest document that parses, and one
// ---- mutation of it that the parser rejects with a real error message.

namespace {
const char* kMinimalMaterialJson = R"JSON({
    "schemaVersion": 3,
    "materialName": "MinimalPBR",
    "lightingModel": "StandardPBR"
})JSON";
}

TEST(MaterialAssetParsingTest, MinimalExample_V3Material_Parses)
{
    MaterialAsset asset(GUID::Generate(), "minimal.material");
    ASSERT_TRUE(ParseMaterialJson(kMinimalMaterialJson, asset))
        << (asset.GetErrors().empty() ? "" : asset.GetErrors().front());

    const auto& doc = asset.GetDocument();
    EXPECT_EQ(doc.schemaVersion, 3);
    EXPECT_EQ(doc.materialName, "MinimalPBR");
    EXPECT_EQ(doc.lightingModel, "StandardPBR");
    // No surface authored: v3 allows this; the composer falls back to the
    // default surface, and the StandardPBR gap-fill exposes the full control set.
    EXPECT_TRUE(doc.surfaceShader.empty());
    EXPECT_FALSE(doc.properties.empty());
    std::cout << "[minimal .material] parsed OK; gap-filled property count = "
              << doc.properties.size() << "\n";
}

TEST(MaterialAssetParsingTest, MinimalExample_BadSchemaVersion_FailsWithRealError)
{
    std::string mutated = kMinimalMaterialJson;
    const std::string from = "\"schemaVersion\": 3";
    const size_t pos = mutated.find(from);
    ASSERT_NE(pos, std::string::npos);
    mutated.replace(pos, from.size(), "\"schemaVersion\": 4");

    MaterialAsset asset(GUID::Generate(), "minimal.material");
    EXPECT_FALSE(ParseMaterialJson(mutated, asset));
    ASSERT_FALSE(asset.GetErrors().empty());
    std::cout << "[mutated .material] parser error: " << asset.GetErrors().front() << "\n";
    EXPECT_NE(asset.GetErrors().front().find("unsupported schemaVersion"), std::string::npos);
}

TEST(MaterialAssetParsingTest, MinimalExample_V2MissingSurfaceShader_FailsWithRealError)
{
    const std::string json = R"JSON({
    "schemaVersion": 2,
    "materialName": "MinimalPBR",
    "lightingModel": "StandardPBR"
})JSON";

    MaterialAsset asset(GUID::Generate(), "minimal.material");
    EXPECT_FALSE(ParseMaterialJson(json, asset));
    ASSERT_FALSE(asset.GetErrors().empty());
    std::cout << "[v2 no-surface .material] parser error: " << asset.GetErrors().front() << "\n";
    EXPECT_NE(asset.GetErrors().front().find("surfaceShader"), std::string::npos);
}
