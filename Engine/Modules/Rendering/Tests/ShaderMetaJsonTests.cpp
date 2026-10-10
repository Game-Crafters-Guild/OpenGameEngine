#include <gtest/gtest.h>
#include "Rendering/Materials/ShaderMeta.h"
#include "Rendering/Materials/ShaderMetaJson.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderLayoutShape.h"

#include <nlohmann/json.hpp>

using namespace GameEngine::Rendering;
using nlohmann::json;

TEST(ShaderMetaJson, UnsignedMaskSurvivesPackagingAndBothLayoutPaths) {
    DescriptorBindingMeta mask{};
    mask.Binding = 48;
    mask.Type = 5; // Combined image sampler; WGSL separates the texture.
    mask.StagesMask = 2;
    mask.Image.emplace();
    mask.Image->UnsignedInteger = true;
    const json packed = mask;
    const auto restored = packed.get<DescriptorBindingMeta>();
    ASSERT_TRUE(restored.Image.has_value());
    EXPECT_TRUE(restored.Image->UnsignedInteger);

    DescriptorSetMeta set{};
    set.Bindings.push_back(restored);
    auto material = MaterialBuilder::BuildSetLayout(set);
    ASSERT_EQ(material.bindings.size(), 1u);
    EXPECT_TRUE(material.bindings[0].imageIsUnsignedInteger);

    ShaderMeta meta{};
    meta.Sets.push_back(set);
    DescriptorSetLayoutDesc compute{};
    DescriptorBinding binding{};
    binding.binding = 48;
    binding.type = DescriptorType::CombinedImageSampler;
    compute.bindings.push_back(binding);
    ApplyMetaImageShapeToLayout(meta, compute);
    EXPECT_TRUE(compute.bindings[0].imageIsUnsignedInteger);

    json legacy = packed;
    legacy["image"].erase("unsignedInteger");
    EXPECT_FALSE(legacy.get<DescriptorBindingMeta>().Image->UnsignedInteger);
}

TEST(ShaderMetaJson, RoundTripComplexSchema) {
    ShaderMeta m{};
    m.Version = 1;
    m.EntryPoints["vs"] = "main";
    m.EntryPoints["fs"] = "main";

    // Push constants with structured members
    Member pcM0{ "time", TypeDesc{ TypeKind::Scalar, BaseType::Float }, 0, 4 };
    Member pcM1{ "padding", TypeDesc{ TypeKind::Vector, BaseType::Float, 1, 1, 4 }, 4, 16 };
    BlockLayout pcBlock{}; pcBlock.Size = 32; pcBlock.Members = { pcM0, pcM1 };
    {
        PushConstantRangeMeta pc{};
        pc.Name = "Globals";
        pc.Size = 32;
        pc.StagesMask = 3;
        pc.Block = pcBlock;
        m.PushConstants.push_back(pc);
    }

    // Set 0 with UBO having struct members
    Member uM0{ "proj", TypeDesc{ TypeKind::Matrix, BaseType::Float, 4, 4 }, 0, 64 };
    BlockLayout uBlock{}; uBlock.Size = 64; uBlock.Members = { uM0 };
    DescriptorBindingMeta ub{}; ub.Binding = 0; ub.Name = "PerFrame"; ub.Type = 0; ub.Count = 1; ub.StagesMask = 3; ub.Block = uBlock;
    DescriptorSetMeta set0{}; set0.Set = 0; set0.Bindings = { ub };
    m.Sets.push_back(set0);

    // Stage IO
    StageIO in0{}; in0.Location = 0; in0.Name = "POSITION";
    in0.Type = TypeDesc{ TypeKind::Vector, BaseType::Float, 1, 1, 3 };
    StageIO out0{}; out0.Location = 0; out0.Name = "TEXCOORD0";
    out0.Type = TypeDesc{ TypeKind::Vector, BaseType::Float, 1, 1, 2 };
    // Component-packed sibling: the second half of location 0's output slot.
    StageIO out1{}; out1.Location = 0; out1.Component = 2; out1.Name = "PACKED";
    out1.Type = TypeDesc{ TypeKind::Scalar, BaseType::UInt };
    StageMeta vs{}; vs.Inputs = { in0 }; vs.Outputs = { out0, out1 }; vs.EntryPoint = "main";
    m.Stages["vs"] = vs;

    // Spec constants
    SpecConstantMeta sc{}; sc.Id = 1; sc.Name = "USE_FOG"; sc.Type = TypeDesc{ TypeKind::Scalar, BaseType::Bool };
    m.SpecConstants.push_back(sc);

    // Serialize
    json j = m;
    std::string s = j.dump();

    // Deserialize
    ShaderMeta m2 = json::parse(s).get<ShaderMeta>();

    // Assertions
    EXPECT_EQ(m2.Version, 1u);
    EXPECT_EQ(m2.EntryPoints.at("vs"), "main");
    ASSERT_EQ(m2.PushConstants.size(), 1u);
    EXPECT_EQ(m2.PushConstants[0].Name, "Globals");
    ASSERT_EQ(m2.Sets.size(), 1u);
    ASSERT_EQ(m2.Sets[0].Bindings.size(), 1u);
    EXPECT_EQ(m2.Sets[0].Bindings[0].Name, "PerFrame");
    ASSERT_TRUE(m2.Stages.count("vs") > 0);
    ASSERT_EQ(m2.Stages["vs"].Inputs.size(), 1u);
    EXPECT_EQ(m2.Stages["vs"].Inputs[0].Name, "POSITION");
    EXPECT_EQ(m2.Stages["vs"].Inputs[0].Component, 0u);
    ASSERT_EQ(m2.Stages["vs"].Outputs.size(), 2u);
    EXPECT_EQ(m2.Stages["vs"].Outputs[0].Component, 0u);
    EXPECT_EQ(m2.Stages["vs"].Outputs[1].Component, 2u);
    ASSERT_EQ(m2.SpecConstants.size(), 1u);
    EXPECT_EQ(m2.SpecConstants[0].Name, "USE_FOG");
}



TEST(ShaderMetaJson, NestedBlocksAndSSBO_RoundTrip) {
    ShaderMeta m{}; m.Version = 1;
    // Nested struct: Light { vec3 color; float intensity; } inside UBO
    Member lightColor{ "color", TypeDesc{ TypeKind::Vector, BaseType::Float, 1, 1, 3 }, 0, 12 };
    Member lightIntensity{ "intensity", TypeDesc{ TypeKind::Scalar, BaseType::Float }, 16, 4 };
    TypeDesc lightType{}; lightType.Kind = TypeKind::Struct; lightType.Base = BaseType::Unknown;
    lightType.StructMembers = { lightColor, lightIntensity };

    Member lightMember{ "light", lightType, 0, 32 };
    BlockLayout ubo{}; ubo.Size = 32; ubo.Members = { lightMember };

    DescriptorBindingMeta ub{}; ub.Binding = 0; ub.Name = "PerFrame"; ub.Type = 0; ub.Count = 1; ub.StagesMask = 3; ub.Block = ubo;
    DescriptorSetMeta set0{}; set0.Set = 0; set0.Bindings = { ub };

    // SSBO with array of vec4
    TypeDesc v4{}; v4.Kind = TypeKind::Vector; v4.Base = BaseType::Float; v4.VecSize = 4;
    Member ssboElem{ "data", v4, 0, 16 }; ssboElem.ArrayStride = 16u;
    BlockLayout ssbo{}; ssbo.Size = 0; ssbo.Members = { ssboElem };

    DescriptorBindingMeta sb{}; sb.Binding = 1; sb.Name = "OutData"; sb.Type = 3; sb.Count = 1; sb.StagesMask = 3; sb.Block = ssbo; // pretend type 3 = storage buffer

    m.Sets = { set0, DescriptorSetMeta{1, { sb }} };

    // Spec constants
    SpecConstantMeta sc{}; sc.Id = 7; sc.Name = "ENABLE_EXTRA"; sc.Type = TypeDesc{ TypeKind::Scalar, BaseType::Bool };
    m.SpecConstants.push_back(sc);

    nlohmann::json j = m; std::string s = j.dump();
    ShaderMeta m2 = nlohmann::json::parse(s).get<ShaderMeta>();

    ASSERT_EQ(m2.Sets.size(), 2u);
    ASSERT_EQ(m2.Sets[0].Bindings.size(), 1u);
    ASSERT_TRUE(m2.Sets[0].Bindings[0].Block.has_value());
    ASSERT_EQ(m2.Sets[0].Bindings[0].Block->Members.size(), 1u);
    ASSERT_EQ(m2.Sets[0].Bindings[0].Block->Members[0].Type.StructMembers.size(), 2u);
    ASSERT_EQ(m2.Sets[1].Bindings[0].Name, "OutData");
    ASSERT_TRUE(m2.Sets[1].Bindings[0].Block.has_value());
    ASSERT_EQ(m2.SpecConstants.size(), 1u);
    EXPECT_EQ(m2.SpecConstants[0].Id, 7u);
}

TEST(ShaderMetaJson, RoundTripDeclaredProperties) {
    ShaderMeta m{};
    ShaderProperty tint{};
    tint.Name = "tint";
    tint.DisplayName = "Tint";
    tint.Type = ShaderPropertyType::Color;
    tint.Origin = ShaderPropertyOrigin::Surface;
    tint.Default = {1.0f, 0.55f, 0.2f, 1.0f};
    tint.Group = "Look";
    tint.VisibleIf = "enableTint";
    tint.Tooltip = "scene-linear";
    tint.Hdr = true;
    tint.HasLane = true;
    tint.Lane = 2;
    tint.Component = 0;
    tint.ByteOffset = 32;
    tint.ByteSize = 12;
    tint.SourceFile = "water.glsl";
    tint.SourceLine = 7;
    ShaderProperty axis{};
    axis.Name = "axis";
    axis.Type = ShaderPropertyType::Enum;
    axis.EnumValues = {"UV", "WorldY"};
    axis.Default = {1.0f, 0.0f, 0.0f, 0.0f};
    axis.HasRange = false;
    ShaderProperty cutoff{};
    cutoff.Name = "alphaCutoff";
    cutoff.Origin = ShaderPropertyOrigin::Adapter;
    cutoff.HasRange = true;
    cutoff.RangeMin = 0.0f;
    cutoff.RangeMax = 1.0f;
    cutoff.Hidden = true; // exercises the flag; a constant read has no lane
    m.DeclaredProperties = {tint, axis, cutoff};

    const ShaderMeta m2 = json::parse(json(m).dump()).get<ShaderMeta>();
    ASSERT_EQ(m2.DeclaredProperties.size(), 3u);
    const ShaderProperty& t = m2.DeclaredProperties[0];
    EXPECT_EQ(t.Name, "tint");
    EXPECT_EQ(t.DisplayName, "Tint");
    EXPECT_EQ(t.Type, ShaderPropertyType::Color);
    EXPECT_EQ(t.Origin, ShaderPropertyOrigin::Surface);
    EXPECT_FLOAT_EQ(t.Default[1], 0.55f);
    EXPECT_EQ(t.Group, "Look");
    EXPECT_EQ(t.VisibleIf, "enableTint");
    EXPECT_EQ(t.Tooltip, "scene-linear");
    EXPECT_TRUE(t.Hdr);
    EXPECT_TRUE(t.HasLane);
    EXPECT_EQ(t.Lane, 2u);
    EXPECT_EQ(t.ByteOffset, 32u);
    EXPECT_EQ(t.ByteSize, 12u);
    EXPECT_EQ(t.SourceFile, "water.glsl");
    EXPECT_EQ(t.SourceLine, 7u);
    EXPECT_FALSE(t.HasRange);
    const ShaderProperty& a = m2.DeclaredProperties[1];
    EXPECT_EQ(a.Type, ShaderPropertyType::Enum);
    ASSERT_EQ(a.EnumValues.size(), 2u);
    EXPECT_EQ(a.EnumValues[1], "WorldY");
    EXPECT_FLOAT_EQ(a.Default[0], 1.0f);
    const ShaderProperty& c = m2.DeclaredProperties[2];
    EXPECT_EQ(c.Origin, ShaderPropertyOrigin::Adapter);
    EXPECT_TRUE(c.HasRange);
    EXPECT_FLOAT_EQ(c.RangeMax, 1.0f);
    EXPECT_TRUE(c.Hidden);
    EXPECT_FALSE(c.HasLane);

    // A meta without declarations stays byte-identical to the pre-declaration schema.
    EXPECT_FALSE(json(ShaderMeta{}).contains("declaredProperties"));
}
