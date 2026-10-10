// Tests for ShaderVariantKey define generation and ShaderComposer source composition.

#include <gtest/gtest.h>

#include "Rendering/Materials/ShaderVariantKey.h"
#include "Rendering/Materials/ShaderComposer.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Geometry/VertexAttributeFlags.h"
#include "Rendering/Geometry/VertexLayoutBuilder.h"
#include "TestUtils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace GameEngine;
using namespace GameEngine::Rendering;

// Build context with the engine tree as the sole shader root (no packages).
static MaterialBuildContext MakeContext(const std::filesystem::path& adapterDir)
{
    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = adapterDir;
    return ctx;
}

// ---- ShaderVariantKey tests ----

TEST(ShaderVariantKeyTest, DefaultKey_GeneratesUnlitDefine)
{
    ShaderVariantKey key{};
    auto defs = GenerateDefines(key, "Unlit");

    auto has = [&](const std::string& d)
    { return std::find(defs.begin(), defs.end(), d) != defs.end(); };

    EXPECT_TRUE(has("LIGHTING_MODEL_UNLIT"));
    EXPECT_FALSE(has("LIGHTING_MODEL_STANDARD_PBR"));
}


TEST(ShaderVariantKeyTest, ShadowOnly_GeneratesCorrectDefine)
{
    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.lightingModel = LightingModel::kShadowOnly;

    auto defs = GenerateDefines(key, "ShadowOnly");

    auto has = [&](const std::string& d)
    { return std::find(defs.begin(), defs.end(), d) != defs.end(); };

    EXPECT_TRUE(has("LIGHTING_MODEL_SHADOW_ONLY"));
    EXPECT_TRUE(has("HAS_NORMAL"));
}

TEST(ShaderVariantKeyTest, StandardPBR_WithNormalUV_GeneratesCorrectDefines)
{
    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.lightingModel = LightingModel::kStandardPBR;

    auto defs = GenerateDefines(key, "StandardPBR");

    auto has = [&](const std::string& d)
    { return std::find(defs.begin(), defs.end(), d) != defs.end(); };

    EXPECT_TRUE(has("HAS_POSITION"));
    EXPECT_TRUE(has("HAS_NORMAL"));
    EXPECT_TRUE(has("HAS_UV0"));
    EXPECT_TRUE(has("LIGHTING_MODEL_STANDARD_PBR"));
    EXPECT_FALSE(has("HAS_TANGENT"));
    EXPECT_FALSE(has("SKINNED"));
    EXPECT_FALSE(has("GE_INSTANCED"));
}

TEST(ShaderVariantKeyTest, SkinnedInstanced_GeneratesAllExpectedDefines)
{
    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::SkinnedMeshWithTangent;
    key.materialKeywords = MaterialKeyword::Instanced | MaterialKeyword::AlphaTest;
    key.lightingModel = LightingModel::kStandardPBR;

    auto defs = GenerateDefines(key, "StandardPBR");

    auto has = [&](const std::string& d)
    { return std::find(defs.begin(), defs.end(), d) != defs.end(); };

    EXPECT_TRUE(has("HAS_POSITION"));
    EXPECT_TRUE(has("HAS_NORMAL"));
    EXPECT_TRUE(has("HAS_UV0"));
    EXPECT_TRUE(has("HAS_TANGENT"));
    EXPECT_TRUE(has("SKINNED"));
    EXPECT_TRUE(has("GE_INSTANCED"));
    EXPECT_TRUE(has("ALPHA_TEST"));
    EXPECT_TRUE(has("LIGHTING_MODEL_STANDARD_PBR"));
}

TEST(ShaderVariantKeyTest, HashIsDeterministic)
{
    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.lightingModel = LightingModel::kStandardPBR;

    ShaderVariantKey key2 = key;
    EXPECT_EQ(key.Hash(), key2.Hash());
}

TEST(ShaderVariantKeyTest, DifferentKeys_DifferentHashes)
{
    ShaderVariantKey a{};
    a.vertexFlags = VertexAttributeFlags::StandardMesh;
    a.lightingModel = LightingModel::kStandardPBR;

    ShaderVariantKey b{};
    b.vertexFlags = VertexAttributeFlags::StandardMesh;
    b.lightingModel = LightingModel::kUnlit;

    EXPECT_NE(a.Hash(), b.Hash());
}

// The keyword field holds 64 bits: the operators keep a keyword at any of them,
// and two keys that differ only in it compare and hash apart. A 32-bit cast on
// that path drops the upper half and turns the two keys into one.
TEST(ShaderVariantKeyTest, EveryKeywordBitSurvivesTheOperatorsAndSplitsTheKey)
{
    constexpr uint32_t kKeywordBits = 64;
    ShaderVariantKey plain{};
    plain.vertexFlags = VertexAttributeFlags::StandardMesh;
    plain.lightingModel = LightingModel::kStandardPBR;

    for (uint32_t bit = 0; bit < kKeywordBits; ++bit)
    {
        const auto keyword = static_cast<MaterialKeyword>(uint64_t{1} << bit);
        EXPECT_TRUE(HasKeyword(MaterialKeyword::Instanced | keyword, keyword)) << "bit " << bit;
        EXPECT_TRUE(HasKeyword(~MaterialKeyword::None, keyword)) << "bit " << bit;

        ShaderVariantKey keyed = plain;
        keyed.materialKeywords |= keyword;
        EXPECT_FALSE(keyed == plain) << "bit " << bit;
        EXPECT_NE(keyed.Hash(), plain.Hash()) << "bit " << bit;
    }
}

// ---- ShaderComposer tests ----

TEST(ShaderComposerTest, GenerateDefinePreamble_ProducesValidGLSL)
{
    std::vector<std::string> defs = {"HAS_NORMAL", "HAS_UV0", "LIGHTING_MODEL_STANDARD_PBR"};
    std::string preamble = ShaderComposer::GenerateDefinePreamble(defs);

    EXPECT_NE(preamble.find("#define HAS_NORMAL"), std::string::npos);
    EXPECT_NE(preamble.find("#define HAS_UV0"), std::string::npos);
    EXPECT_NE(preamble.find("#define LIGHTING_MODEL_STANDARD_PBR"), std::string::npos);
}

TEST(ShaderComposerTest, GenerateDefinePreambleFromKey_MatchesExplicit)
{
    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::HasPosition | VertexAttributeFlags::HasNormal;
    key.lightingModel = LightingModel::kUnlit;

    std::string fromKey = ShaderComposer::GenerateDefinePreamble(key, "Unlit");
    std::string fromExplicit = ShaderComposer::GenerateDefinePreamble(GenerateDefines(key, "Unlit"));

    EXPECT_EQ(fromKey, fromExplicit);
}

TEST(ShaderComposerTest, Compose_WithValidAdapters_ProducesNonEmptySource)
{
    const auto adapterDir = GameEngine::Rendering::Tests::GetAdapterShaderDir();

    MaterialDocument doc{};
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_surface.glsl";

    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.lightingModel = LightingModel::kStandardPBR;

    std::vector<std::string> errors;
    auto result = ShaderComposer::Compose(doc, key, adapterDir, MakeContext(adapterDir), &errors);

    for (const auto& e : errors)
        ADD_FAILURE() << "Compose error: " << e;

    EXPECT_TRUE(result.IsValid()) << "Adapter source should be valid";
    EXPECT_FALSE(result.vertexSource.empty());
    EXPECT_FALSE(result.fragmentSource.empty());

    // The vertex source should contain the injected defines.
    EXPECT_NE(result.vertexSource.find("#define HAS_POSITION"), std::string::npos);
    EXPECT_NE(result.vertexSource.find("#define HAS_NORMAL"), std::string::npos);
    EXPECT_NE(result.vertexSource.find("#define HAS_UV0"), std::string::npos);
}

TEST(ShaderComposerTest, Compose_Unlit_UsesUnlitAdapters)
{
    const auto adapterDir = GameEngine::Rendering::Tests::GetAdapterShaderDir();

    MaterialDocument doc{};
    doc.lightingModel = "Unlit";
    doc.surfaceShader = "Surfaces/standard_surface.glsl";

    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.lightingModel = LightingModel::kUnlit;

    std::vector<std::string> errors;
    auto result = ShaderComposer::Compose(doc, key, adapterDir, MakeContext(adapterDir), &errors);

    EXPECT_TRUE(result.IsValid());
    EXPECT_NE(result.fragmentAdapterName.find("adapter_forward"), std::string::npos)
        << "Unlit material should use the unified fragment adapter";

    auto hasDefine = [&](const std::string& d)
    { return std::find(result.defines.begin(), result.defines.end(), d) != result.defines.end(); };
    EXPECT_TRUE(hasDefine("LIGHTING_MODEL_UNLIT"))
        << "Unlit variant should emit LIGHTING_MODEL_UNLIT define";
}

TEST(ShaderComposerTest, Compose_Instanced_UsesNewVertexAdapter)
{
    const auto adapterDir = GameEngine::Rendering::Tests::GetAdapterShaderDir();

    MaterialDocument doc{};
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_surface.glsl";

    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.materialKeywords = MaterialKeyword::Instanced;
    key.lightingModel = LightingModel::kStandardPBR;

    std::vector<std::string> errors;
    auto result = ShaderComposer::Compose(doc, key, adapterDir, MakeContext(adapterDir), &errors);

    EXPECT_TRUE(result.IsValid());
    EXPECT_NE(result.vertexAdapterName.find("adapter_vertex"), std::string::npos);
    EXPECT_NE(result.vertexSource.find("#define GE_INSTANCED"), std::string::npos);
}

TEST(ShaderVariantKeyTest, HasVertexMod_GeneratesDefine)
{
    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.materialKeywords = MaterialKeyword::Instanced | MaterialKeyword::HasVertexMod;
    key.lightingModel = LightingModel::kStandardPBR;

    auto defs = GenerateDefines(key, "StandardPBR");

    auto has = [&](const std::string& d)
    { return std::find(defs.begin(), defs.end(), d) != defs.end(); };

    EXPECT_TRUE(has("HAS_VERTEX_MODIFIER"));
    EXPECT_TRUE(has("GE_INSTANCED"));
}

TEST(ShaderVariantKeyTest, ProceduralVertexOutput_IsExplicitOptIn)
{
    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.materialKeywords = MaterialKeyword::HasVertexOutputMod;
    key.lightingModel = LightingModel::kStandardPBR;

    auto defs = GenerateDefines(key, "StandardPBR");

    auto has = [&](const std::string& d)
    { return std::find(defs.begin(), defs.end(), d) != defs.end(); };

    EXPECT_TRUE(has("HAS_VERTEX_OUTPUT_MODIFIER"));
    EXPECT_FALSE(has("GE_PROCEDURAL_VERTEX_OUTPUT"));

    key.materialKeywords |= MaterialKeyword::ProceduralVertexOutput;
    defs = GenerateDefines(key, "StandardPBR");

    EXPECT_TRUE(has("HAS_VERTEX_OUTPUT_MODIFIER"));
    EXPECT_TRUE(has("GE_PROCEDURAL_VERTEX_OUTPUT"));
}

// The device's interpolation functions reach only the programs that march, and never the compat
// arm, whose shading takes no pixel-centre footprint.
TEST(ShaderVariantKeyTest, InterpolationFunctions_ReachOnlyMarchingDesktopKeys)
{
    const bool previousAvailable = AreInterpolationFunctionsAvailable();
    const bool previousCompat = IsCompatShaderProfile();
    ShaderVariantKey relief{};
    relief.vertexFlags = VertexAttributeFlags::StandardMesh;
    relief.materialKeywords = MaterialKeyword::ForwardPlus | MaterialKeyword::Parallax;
    relief.lightingModel = LightingModel::kStandardPBR;
    ShaderVariantKey flat = relief;
    flat.materialKeywords = MaterialKeyword::ForwardPlus;
    const auto names = [](const ShaderVariantKey& key)
    {
        const std::vector<std::string> defs = GenerateDefines(key, "StandardPBR");
        return std::find(defs.begin(), defs.end(), "GE_INTERPOLATION_FUNCTIONS") != defs.end();
    };

    SetCompatShaderProfile(false);
    SetInterpolationFunctionsAvailable(true);
    EXPECT_TRUE(names(relief));
    EXPECT_FALSE(names(flat)) << "a program that does not march keeps its defines on every device";
    for (const MaterialKeyword coverageShape :
         {MaterialKeyword::DepthOnlyFragment, MaterialKeyword::MotionVectors, MaterialKeyword::DepthOnlyTransmissionColor})
    {
        ShaderVariantKey coverage = relief;
        coverage.materialKeywords = MaterialKeyword::Parallax | coverageShape;
        EXPECT_FALSE(names(coverage)) << "a Parallax key that decides coverage on the polygon does not march, "
                                         "so it keeps its defines on every device ("
                                      << static_cast<uint64_t>(coverageShape) << ")";
    }
    ShaderVariantKey depthOffsetPrepass = relief;
    depthOffsetPrepass.materialKeywords = MaterialKeyword::Parallax | MaterialKeyword::DepthOnlyFragment |
                                          MaterialKeyword::ParallaxDepthOffset;
    EXPECT_TRUE(names(depthOffsetPrepass)) << "the prepass that writes the relief's depth marches from the same "
                                              "pixel-centre footprint as the colour pass";
    SetInterpolationFunctionsAvailable(false);
    EXPECT_FALSE(names(relief));
    SetInterpolationFunctionsAvailable(true);
    SetCompatShaderProfile(true);
    EXPECT_FALSE(names(relief));

    SetCompatShaderProfile(previousCompat);
    SetInterpolationFunctionsAvailable(previousAvailable);
}

// ---- Forward+ tests ----

TEST(ShaderVariantKeyTest, ForwardPlus_GeneratesDefine)
{
    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.materialKeywords = MaterialKeyword::ForwardPlus;
    key.lightingModel = LightingModel::kStandardPBR;

    auto defs = GenerateDefines(key, "StandardPBR");

    auto has = [&](const std::string& d)
    { return std::find(defs.begin(), defs.end(), d) != defs.end(); };

    EXPECT_TRUE(has("FORWARD_PLUS"));
    EXPECT_TRUE(has("LIGHTING_MODEL_STANDARD_PBR"));
}

TEST(ShaderVariantKeyTest, Transmission_GeneratesDefine)
{
    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.lightingModel = LightingModel::kStandardPBR;

    auto has = [](const std::vector<std::string>& defs, const std::string& d)
    { return std::find(defs.begin(), defs.end(), d) != defs.end(); };

    // Off by default — the refractive-glass lobe must not compile into ordinary PBR materials.
    EXPECT_FALSE(has(GenerateDefines(key, "StandardPBR"), "GE_TRANSMISSION_ENABLED"));

    // Setting the keyword bit emits the define that gates the in-shader transmission term.
    key.materialKeywords = MaterialKeyword::Transmission;
    EXPECT_TRUE(has(GenerateDefines(key, "StandardPBR"), "GE_TRANSMISSION_ENABLED"));
}

TEST(ShaderComposerTest, Compose_ForwardPlus_InjectsDefine)
{
    const auto adapterDir = GameEngine::Rendering::Tests::GetAdapterShaderDir();

    MaterialDocument doc{};
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_surface.glsl";

    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.materialKeywords = MaterialKeyword::ForwardPlus;
    key.lightingModel = LightingModel::kStandardPBR;

    std::vector<std::string> errors;
    auto result = ShaderComposer::Compose(doc, key, adapterDir, MakeContext(adapterDir), &errors);

    EXPECT_TRUE(result.IsValid());
    // The fragment source should contain the FORWARD_PLUS define.
    EXPECT_NE(result.fragmentSource.find("#define FORWARD_PLUS"), std::string::npos);
    // And the StandardPBR lighting model define.
    EXPECT_NE(result.fragmentSource.find("#define LIGHTING_MODEL_STANDARD_PBR"), std::string::npos);
}

// ---- Vertex modifier injection tests ----

// ---- customVertexShader (F2 / CBT foundation) tests ----

TEST(ShaderVariantKeyTest, CustomVertexShader_EmitsDefineOnlyWhenSet)
{
    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::None;
    key.materialKeywords = MaterialKeyword::HasVertexOutputMod;
    key.lightingModel = LightingModel::kUnlit;

    auto has = [](const std::vector<std::string>& d, const std::string& s)
    { return std::find(d.begin(), d.end(), s) != d.end(); };

    EXPECT_FALSE(has(GenerateDefines(key, "Unlit"), "CUSTOM_VERTEX_SHADER"));

    key.materialKeywords |= MaterialKeyword::CustomVertexShader;
    EXPECT_TRUE(has(GenerateDefines(key, "Unlit"), "CUSTOM_VERTEX_SHADER"));
}

TEST(ShaderVariantKeyTest, CustomVertexShaderClamp_ForcesNoneLayoutAndOutputModifier)
{
    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMeshWithTangent;
    key.materialKeywords = MaterialKeyword::HasVertexMod | MaterialKeyword::ForwardPlus;
    key.lightingModel = LightingModel::kStandardPBR;

    ApplyCustomVertexShaderClamp(key);

    EXPECT_EQ(key.vertexFlags, VertexAttributeFlags::None);
    EXPECT_TRUE(HasKeyword(key.materialKeywords, MaterialKeyword::CustomVertexShader));
    EXPECT_TRUE(HasKeyword(key.materialKeywords, MaterialKeyword::HasVertexOutputMod));
    // Simple-form bit cleared so both modifier defines never co-emit (POC fix 3).
    EXPECT_FALSE(HasKeyword(key.materialKeywords, MaterialKeyword::HasVertexMod));
    // Unrelated pipeline keywords are preserved.
    EXPECT_TRUE(HasKeyword(key.materialKeywords, MaterialKeyword::ForwardPlus));

    auto defs = GenerateDefines(key, "StandardPBR");
    auto has = [&](const std::string& d)
    { return std::find(defs.begin(), defs.end(), d) != defs.end(); };
    EXPECT_TRUE(has("CUSTOM_VERTEX_SHADER"));
    EXPECT_TRUE(has("HAS_VERTEX_OUTPUT_MODIFIER"));
    EXPECT_FALSE(has("HAS_VERTEX_MODIFIER"));
    EXPECT_FALSE(has("HAS_POSITION")); // no vertex buffer -> no mesh-attribute defines
}

TEST(ShaderVariantKeyTest, CustomVertexShader_HashDistinctFromStandard)
{
    ShaderVariantKey standard{};
    standard.vertexFlags = VertexAttributeFlags::StandardMesh;
    standard.materialKeywords = MaterialKeyword::HasVertexOutputMod;
    standard.lightingModel = LightingModel::kStandardPBR;

    ShaderVariantKey custom = standard;
    ApplyCustomVertexShaderClamp(custom);

    EXPECT_NE(standard.Hash(), custom.Hash())
        << "customVertexShader must select a distinct shader variant / cache slot";
}

TEST(ShaderVariantKeyTest, SimpleVertexModifier_UnaffectedWhenNotCustom)
{
    // A CDLOD-style material with a simple vertex modifier but NOT
    // customVertexShader keeps its mesh layout and simple-form modifier: the
    // custom-vertex path must never leak into ordinary vertex-modifier materials.
    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.materialKeywords = MaterialKeyword::HasVertexMod;
    key.lightingModel = LightingModel::kStandardPBR;

    auto defs = GenerateDefines(key, "StandardPBR");
    auto has = [&](const std::string& d)
    { return std::find(defs.begin(), defs.end(), d) != defs.end(); };
    EXPECT_TRUE(has("HAS_VERTEX_MODIFIER"));
    EXPECT_FALSE(has("CUSTOM_VERTEX_SHADER"));
    EXPECT_FALSE(has("HAS_VERTEX_OUTPUT_MODIFIER"));
    EXPECT_TRUE(has("HAS_POSITION"));
}

TEST(VertexLayoutBuilderTest, NoneFlags_ProducesEmptyVertexInput)
{
    GraphicsPipelineDesc pd{};
    const uint32_t bindings = BuildVertexLayoutFromFlags(VertexAttributeFlags::None, pd);
    EXPECT_EQ(bindings, 0u);
    EXPECT_TRUE(pd.VertexBindings.empty());
    EXPECT_TRUE(pd.VertexAttributes.empty());
}

TEST(VertexLayoutBuilderTest, StandardMeshFlags_StillProducesCoreBinding)
{
    GraphicsPipelineDesc pd{};
    const uint32_t bindings = BuildVertexLayoutFromFlags(VertexAttributeFlags::StandardMesh, pd);
    EXPECT_GT(bindings, 0u);
    EXPECT_FALSE(pd.VertexBindings.empty());

    bool hasPosition = false;
    for (const auto& attr : pd.VertexAttributes)
        if (attr.location == VertexLocation::Position)
            hasPosition = true;
    EXPECT_TRUE(hasPosition) << "standard mesh layout must still bind the position attribute";
}

TEST(ShaderComposerTest, Compose_InstancedWithVertexModifier_InjectsModifierPath)
{
    const auto adapterDir = GameEngine::Rendering::Tests::GetAdapterShaderDir();

    namespace fs = std::filesystem;
    const fs::path modifierPath = adapterDir / "test_modifier_temp.glsl";
    {
        std::ofstream out(modifierPath, std::ios::trunc);
        out << "vec3 ModifyVertex(vec3 pos, InstanceData inst) { return pos; }\n";
    }

    MaterialDocument doc{};
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_surface.glsl";
    doc.vertexModifier = "test_modifier_temp.glsl";

    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.materialKeywords = MaterialKeyword::Instanced | MaterialKeyword::HasVertexMod;
    key.lightingModel = LightingModel::kStandardPBR;

    std::vector<std::string> errors;
    auto result = ShaderComposer::Compose(doc, key, adapterDir, MakeContext(adapterDir), &errors);

    // Clean up temp file.
    std::error_code ec;
    fs::remove(modifierPath, ec);

    for (const auto& e : errors)
        ADD_FAILURE() << "Compose error: " << e;

    EXPECT_TRUE(result.IsValid());
    EXPECT_NE(result.vertexSource.find("#define HAS_VERTEX_MODIFIER"), std::string::npos)
        << "Vertex source should contain HAS_VERTEX_MODIFIER define";
    // The raw #include GE_VERTEX_MODIFIER_PATH marker should have been replaced.
    EXPECT_EQ(result.vertexSource.find("#include GE_VERTEX_MODIFIER_PATH"), std::string::npos)
        << "Raw vertex modifier marker should have been replaced";
    // The replaced include should reference the temp file.
    EXPECT_NE(result.vertexSource.find("test_modifier_temp.glsl"), std::string::npos)
        << "Vertex source should include the modifier file path";
}

// ---- User keyword hash lane (slice N) ----

TEST(UserKeywordHash, OrderIndependent)
{
    const uint64_t a = HashStringId("FLOW_MODE");
    const uint64_t b = HashStringId("HIGH_DETAIL");
    EXPECT_EQ(CombineUserKeywordHash({a, b}), CombineUserKeywordHash({b, a}));
}

TEST(UserKeywordHash, DeduplicatesAndIsNonCancelling)
{
    const uint64_t a = HashStringId("A");
    const uint64_t b = HashStringId("B");
    const uint64_t c = HashStringId("C");
    // Duplicates collapse to the set: {A,A} == {A}, {A,B,C,C} == {A,B,C}.
    EXPECT_EQ(CombineUserKeywordHash({a, a}), CombineUserKeywordHash({a}));
    EXPECT_EQ(CombineUserKeywordHash({a, b, c, c}), CombineUserKeywordHash({a, b, c}));
    // Non-cancelling: an even repeat does NOT vanish (XOR would give {A,A}=={}).
    EXPECT_NE(CombineUserKeywordHash({a, a}), CombineUserKeywordHash({}));
    EXPECT_NE(CombineUserKeywordHash({a, b}), CombineUserKeywordHash({}));
    // Distinct sets hash distinctly.
    EXPECT_NE(CombineUserKeywordHash({a}), CombineUserKeywordHash({b}));
    EXPECT_NE(CombineUserKeywordHash({a, b}), CombineUserKeywordHash({a}));
}

TEST(UserKeywordHash, EmptySetIsZero)
{
    EXPECT_EQ(CombineUserKeywordHash({}), 0ull);
}

TEST(UserKeywordHash, VariantKeyEqualityAndHashIncludeTheLane)
{
    ShaderVariantKey a{};
    ShaderVariantKey b{};
    b.userKeywordHash = 0xABCDEF12ull;
    EXPECT_FALSE(a == b);
    EXPECT_NE(a.Hash(), b.Hash());

    // Same lane value -> equal keys (order-independence already covered above).
    ShaderVariantKey c = b;
    EXPECT_TRUE(b == c);
    EXPECT_EQ(b.Hash(), c.Hash());
}
