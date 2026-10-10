// T1 — explicit blend authoring. Covers:
//   * PSO blend-state derivation (DeriveMaterialBlendState) for every mode.
//   * Default byte-identity: an unauthored Blend material derives the exact
//     pre-T1 hardwired equation, and serializes without a `blend`/`zWrite` key.
//   * Schema round-trip: an authored blend + zWrite survives serialize -> parse.

#include <gtest/gtest.h>

#include "Assets/MaterialAsset.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialAlphaMode.h"
#include "Rendering/Materials/MaterialBlend.h"
#include "Rendering/Materials/MaterialBlendDerive.h"
#include "Rendering/Materials/MaterialDocument.h"

#include <nlohmann/json.hpp>

#include <cstdint>
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

// ---- PSO blend-state derivation ----

TEST(MaterialBlendDerive, Opaque_NoBlend_DepthWrites)
{
    const auto d = Rendering::DeriveMaterialBlendState(MaterialAlphaMode::Opaque, std::nullopt, std::nullopt,
                                                       std::nullopt);
    EXPECT_FALSE(d.BlendEnable);
    EXPECT_TRUE(d.DepthWriteEnable);
}

TEST(MaterialBlendDerive, Mask_NoBlend_DepthWrites)
{
    const auto d = Rendering::DeriveMaterialBlendState(MaterialAlphaMode::Mask, std::nullopt, std::nullopt,
                                                       std::nullopt);
    EXPECT_FALSE(d.BlendEnable);
    EXPECT_TRUE(d.DepthWriteEnable);
}

// The load-bearing byte-identity check: an unauthored Blend material must derive
// the exact factors/ops the hardwired pre-T1 block set, and must not write depth.
TEST(MaterialBlendDerive, Blend_Default_MatchesPreT1Hardwired)
{
    const auto d = Rendering::DeriveMaterialBlendState(MaterialAlphaMode::Blend, std::nullopt, std::nullopt,
                                                       std::nullopt);
    EXPECT_TRUE(d.BlendEnable);
    EXPECT_TRUE(d.Attachment.blendEnable);
    EXPECT_EQ(d.Attachment.srcColorBlendFactor, Rendering::BlendFactor::SrcAlpha);
    EXPECT_EQ(d.Attachment.dstColorBlendFactor, Rendering::BlendFactor::OneMinusSrcAlpha);
    EXPECT_EQ(d.Attachment.colorBlendOp, Rendering::BlendOp::Add);
    EXPECT_EQ(d.Attachment.srcAlphaBlendFactor, Rendering::BlendFactor::One);
    EXPECT_EQ(d.Attachment.dstAlphaBlendFactor, Rendering::BlendFactor::OneMinusSrcAlpha);
    EXPECT_EQ(d.Attachment.alphaBlendOp, Rendering::BlendOp::Add);
    EXPECT_FALSE(d.DepthWriteEnable);
    EXPECT_TRUE(d.DepthTestEnable);
}

TEST(MaterialBlendDerive, Blend_Additive_IsOrderIndependentFactors)
{
    MaterialBlendState add{};
    add.SrcColorFactor = MaterialBlendFactor::One;
    add.DstColorFactor = MaterialBlendFactor::One;
    add.SrcAlphaFactor = MaterialBlendFactor::One;
    add.DstAlphaFactor = MaterialBlendFactor::One;
    const auto d = Rendering::DeriveMaterialBlendState(MaterialAlphaMode::Blend, add, std::nullopt, std::nullopt);
    EXPECT_TRUE(d.BlendEnable);
    EXPECT_EQ(d.Attachment.srcColorBlendFactor, Rendering::BlendFactor::One);
    EXPECT_EQ(d.Attachment.dstColorBlendFactor, Rendering::BlendFactor::One);
    EXPECT_EQ(d.Attachment.srcAlphaBlendFactor, Rendering::BlendFactor::One);
    EXPECT_EQ(d.Attachment.dstAlphaBlendFactor, Rendering::BlendFactor::One);
    EXPECT_FALSE(d.DepthWriteEnable);
}

TEST(MaterialBlendDerive, Blend_Multiply_Factors)
{
    MaterialBlendState mul{};
    mul.SrcColorFactor = MaterialBlendFactor::DstColor;
    mul.DstColorFactor = MaterialBlendFactor::Zero;
    const auto d = Rendering::DeriveMaterialBlendState(MaterialAlphaMode::Blend, mul, std::nullopt, std::nullopt);
    EXPECT_EQ(d.Attachment.srcColorBlendFactor, Rendering::BlendFactor::DstColor);
    EXPECT_EQ(d.Attachment.dstColorBlendFactor, Rendering::BlendFactor::Zero);
}

TEST(MaterialBlendDerive, ZWriteOverride_WinsBothDirections)
{
    // Force a Blend material to write depth.
    const auto blendForced = Rendering::DeriveMaterialBlendState(MaterialAlphaMode::Blend, std::nullopt, true,
                                                                 std::nullopt);
    EXPECT_TRUE(blendForced.BlendEnable);
    EXPECT_TRUE(blendForced.DepthWriteEnable);

    // Force an Opaque material to NOT write depth.
    const auto opaqueForced = Rendering::DeriveMaterialBlendState(MaterialAlphaMode::Opaque, std::nullopt, false,
                                                                  std::nullopt);
    EXPECT_FALSE(opaqueForced.BlendEnable);
    EXPECT_FALSE(opaqueForced.DepthWriteEnable);
}

TEST(MaterialBlendDerive, ZTestOverride_DisablesDepthTest)
{
    const auto off = Rendering::DeriveMaterialBlendState(
        MaterialAlphaMode::Blend, std::nullopt, std::nullopt, false);
    EXPECT_TRUE(off.BlendEnable);
    EXPECT_FALSE(off.DepthWriteEnable);
    EXPECT_FALSE(off.DepthTestEnable);

    const auto on = Rendering::DeriveMaterialBlendState(
        MaterialAlphaMode::Opaque, std::nullopt, std::nullopt, true);
    EXPECT_TRUE(on.DepthTestEnable);
}

TEST(MaterialBlendDerive, CustomOps_MapThrough)
{
    MaterialBlendState st{};
    st.ColorOp = MaterialBlendOp::ReverseSubtract;
    st.AlphaOp = MaterialBlendOp::Max;
    const auto d = Rendering::DeriveMaterialBlendState(MaterialAlphaMode::Blend, st, std::nullopt, std::nullopt);
    EXPECT_EQ(d.Attachment.colorBlendOp, Rendering::BlendOp::ReverseSubtract);
    EXPECT_EQ(d.Attachment.alphaBlendOp, Rendering::BlendOp::Max);
}

// ---- Default byte-identity (serialization) ----

TEST(MaterialBlendSerialize, UnauthoredBlend_OmitsBlendAndZWrite)
{
    MaterialDocument doc;
    doc.schemaVersion = 3;
    doc.materialName = "GlassBlend";
    doc.lightingModel = "Unlit";
    doc.alphaMode = MaterialAlphaMode::Blend;
    // No blend / no zWrite authored.

    const nlohmann::json j = SerializeMaterialDocument(doc);
    EXPECT_EQ(j.value("alphaMode", std::string()), "Blend");
    EXPECT_FALSE(j.contains("blend"));
    EXPECT_FALSE(j.contains("zWrite"));
    EXPECT_FALSE(j.contains("zTest"));
}

TEST(MaterialBlendSerialize, PresentButAllDefault_CollapsesToOmitted)
{
    MaterialDocument doc;
    doc.schemaVersion = 3;
    doc.materialName = "DefaultValued";
    doc.lightingModel = "Unlit";
    doc.alphaMode = MaterialAlphaMode::Blend;
    doc.blend = MaterialBlendState{}; // present, all default

    const nlohmann::json j = SerializeMaterialDocument(doc);
    // A present-but-all-default block is semantically the hardwired equation, so
    // it normalizes to omitted — the material stays byte-identical to a legacy one.
    EXPECT_FALSE(j.contains("blend"));
}

// ---- Schema round-trip ----

TEST(MaterialBlendRoundTrip, AuthoredAdditivePlusZWrite_Survives)
{
    MaterialDocument doc;
    doc.schemaVersion = 3;
    doc.materialName = "AdditiveFX";
    doc.lightingModel = "Unlit";
    doc.alphaMode = MaterialAlphaMode::Blend;

    MaterialBlendState add{};
    add.SrcColorFactor = MaterialBlendFactor::One;
    add.DstColorFactor = MaterialBlendFactor::One;
    add.ColorOp = MaterialBlendOp::Add;
    add.SrcAlphaFactor = MaterialBlendFactor::One;
    add.DstAlphaFactor = MaterialBlendFactor::One;
    add.AlphaOp = MaterialBlendOp::Add;
    doc.blend = add;
    doc.zWrite = true;

    const nlohmann::json j = SerializeMaterialDocument(doc);
    ASSERT_TRUE(j.contains("blend"));
    ASSERT_TRUE(j.contains("zWrite"));

    MaterialAsset asset(GUID::Generate(), "additive.material");
    ASSERT_TRUE(ParseMaterialJson(j.dump(), asset));
    const MaterialDocument& parsed = asset.GetDocument();

    EXPECT_EQ(parsed.alphaMode, MaterialAlphaMode::Blend);
    ASSERT_TRUE(parsed.blend.has_value());
    EXPECT_EQ(*parsed.blend, add);
    ASSERT_TRUE(parsed.zWrite.has_value());
    EXPECT_TRUE(*parsed.zWrite);
}

TEST(MaterialBlendRoundTrip, AuthoredZTestFalse_Survives)
{
    MaterialDocument doc;
    doc.schemaVersion = 3;
    doc.materialName = "GroundStamp";
    doc.lightingModel = "Unlit";
    doc.alphaMode = MaterialAlphaMode::Blend;
    doc.zTest = false;

    const nlohmann::json j = SerializeMaterialDocument(doc);
    ASSERT_TRUE(j.contains("zTest"));
    EXPECT_FALSE(j["zTest"].get<bool>());
    EXPECT_FALSE(j.contains("zWrite"));

    MaterialAsset asset(GUID::Generate(), "stamp.material");
    ASSERT_TRUE(ParseMaterialJson(j.dump(), asset));
    ASSERT_TRUE(asset.GetDocument().zTest.has_value());
    EXPECT_FALSE(*asset.GetDocument().zTest);
}

TEST(MaterialBlendRoundTrip, PartialAuthoring_UnsetFieldsKeepDefault)
{
    // Author only the color factors; alpha factors/ops must fall back to default.
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "PartialBlend",
        "lightingModel": "Unlit",
        "alphaMode": "Blend",
        "blend": { "srcColor": "One", "dstColor": "One" }
    })JSON";

    MaterialAsset asset(GUID::Generate(), "partial.material");
    ASSERT_TRUE(ParseMaterialJson(json, asset));
    const MaterialDocument& doc = asset.GetDocument();

    ASSERT_TRUE(doc.blend.has_value());
    EXPECT_EQ(doc.blend->SrcColorFactor, MaterialBlendFactor::One);
    EXPECT_EQ(doc.blend->DstColorFactor, MaterialBlendFactor::One);
    // Unset -> default (premultiplied-style alpha accumulation).
    EXPECT_EQ(doc.blend->SrcAlphaFactor, MaterialBlendFactor::One);
    EXPECT_EQ(doc.blend->DstAlphaFactor, MaterialBlendFactor::OneMinusSrcAlpha);
    EXPECT_EQ(doc.blend->ColorOp, MaterialBlendOp::Add);
}

TEST(MaterialBlendRoundTrip, UnknownFactor_IsNonFatalAndKeepsDefault)
{
    const std::string json = R"JSON({
        "schemaVersion": 3,
        "materialName": "BadFactor",
        "lightingModel": "Unlit",
        "alphaMode": "Blend",
        "blend": { "srcColor": "NotAFactor" }
    })JSON";

    MaterialAsset asset(GUID::Generate(), "bad.material");
    // Load succeeds (non-fatal); the bad field keeps its default.
    ASSERT_TRUE(ParseMaterialJson(json, asset));
    const MaterialDocument& doc = asset.GetDocument();
    ASSERT_TRUE(doc.blend.has_value());
    EXPECT_EQ(doc.blend->SrcColorFactor, MaterialBlendFactor::SrcAlpha);
}
