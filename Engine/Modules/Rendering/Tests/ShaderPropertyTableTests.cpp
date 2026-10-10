// Declared material properties: `// @property <type> <name> ["Display"] key=value flags`
// parsed by ParseDeclaredProperties and packed by BuildShaderPropertyTable. Grammar,
// greedy first-fit lane packing, the adapter/surface/vertex-modifier union and the
// error cases are pinned here; the composer's GE_Props emission is pinned in
// ShaderComposerPropertyEmissionTests.

#include <gtest/gtest.h>

#include "Rendering/Materials/MaterialParamsLayout.h"
#include "Rendering/Materials/ShaderPropertyTable.h"

#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

ShaderPropertySource Surface(const std::string& text, const char* file = "surface.glsl")
{
    return ShaderPropertySource{text, file, ShaderPropertyOrigin::Surface};
}

ShaderPropertySource Adapter(const std::string& text)
{
    return ShaderPropertySource{text, "adapter_forward.glsl", ShaderPropertyOrigin::Adapter};
}

ShaderPropertySource Modifier(const std::string& text)
{
    return ShaderPropertySource{text, "modifier.glsl", ShaderPropertyOrigin::VertexModifier};
}

std::string Errors(const ShaderPropertyTable& t)
{
    std::string out;
    for (const auto& e : t.Errors)
        out += e.Format("error") + "\n";
    return out;
}

const ShaderProperty& Prop(const ShaderPropertyTable& t, const char* name)
{
    const ShaderProperty* p = t.Find(name);
    if (!p)
        ADD_FAILURE() << "property '" << name << "' missing; errors: " << Errors(t);
    static const ShaderProperty kNull{};
    return p ? *p : kNull;
}

// The adapter's own four declarations, as shipped.
const char* kAdapterDecls =
    "// @property float alphaCutoff \"Alpha Cutoff\" default=0.5 range=0,1 visibleIf=alphaMode=Mask\n"
    "// @property float specularIor default=1.5 hidden\n"
    "// @property color transmissionColor default=1,1,1 hidden\n"
    "// @property float transmissionWeight default=0 hidden\n";

} // namespace

// ---- Grammar ----

TEST(ShaderPropertyGrammar, ParsesEveryScalarAndVectorType)
{
    const auto t = ParseDeclaredProperties(Surface(
        "// @property float f default=0.25\n"
        "// @property vec2 v2 default=1,2\n"
        "// @property vec3 v3 default=1,2,3\n"
        "// @property vec4 v4 default=1,2,3,4\n"
        "// @property color c default=0.1,0.2,0.3\n"
        "// @property color ca default=0.1,0.2,0.3,0.4\n"
        "// @property bool b default=true\n"
        "// @property int i default=7\n"
        "// @property enum e values=UV,WorldY,Radial default=WorldY\n"));
    ASSERT_FALSE(t.Rejected()) << Errors(t);
    ASSERT_EQ(t.Properties.size(), 9u);
    EXPECT_EQ(Prop(t, "f").Type, ShaderPropertyType::Float);
    EXPECT_FLOAT_EQ(Prop(t, "f").Default[0], 0.25f);
    EXPECT_EQ(Prop(t, "v2").ComponentCount(), 2u);
    EXPECT_FLOAT_EQ(Prop(t, "v2").Default[1], 2.0f);
    EXPECT_EQ(Prop(t, "v3").ComponentCount(), 3u);
    EXPECT_EQ(Prop(t, "v4").ComponentCount(), 4u);
    EXPECT_FLOAT_EQ(Prop(t, "v4").Default[3], 4.0f);
    EXPECT_EQ(Prop(t, "c").ComponentCount(), 3u);
    EXPECT_STREQ(Prop(t, "c").GlslType(), "vec3");
    EXPECT_EQ(Prop(t, "ca").ComponentCount(), 4u);
    EXPECT_STREQ(Prop(t, "ca").GlslType(), "vec4");
    EXPECT_TRUE(Prop(t, "ca").HasAlpha);
    EXPECT_FLOAT_EQ(Prop(t, "b").Default[0], 1.0f);
    EXPECT_STREQ(Prop(t, "b").GlslType(), "bool");
    EXPECT_FLOAT_EQ(Prop(t, "i").Default[0], 7.0f);
    EXPECT_STREQ(Prop(t, "i").GlslType(), "int");
    ASSERT_EQ(Prop(t, "e").EnumValues.size(), 3u);
    EXPECT_EQ(Prop(t, "e").EnumValues[2], "Radial");
    EXPECT_FLOAT_EQ(Prop(t, "e").Default[0], 1.0f) << "enum default is the label's index";
    EXPECT_STREQ(Prop(t, "e").GlslType(), "int");
}

TEST(ShaderPropertyGrammar, ParsesEveryAttributeAndQuotedDisplayName)
{
    const auto t = ParseDeclaredProperties(Surface(
        "// @property float pulseSpeed \"Pulse Speed\" default=2.0 range=0,8 group=Pulse "
        "visibleIf=enablePulse tooltip=\"radians per second; 1.0 == full turn\"\n"
        "// @property bool enablePulse \"Enable Pulse\" default=false group=Pulse\n"
        "// @property color glow \"Glow\" default=2.4,0.9,0.2 hdr\n"
        "// @property float secret default=1 hidden\n"));
    ASSERT_FALSE(t.Rejected()) << Errors(t);
    const ShaderProperty& p = Prop(t, "pulseSpeed");
    EXPECT_EQ(p.DisplayName, "Pulse Speed");
    EXPECT_FLOAT_EQ(p.Default[0], 2.0f);
    EXPECT_TRUE(p.HasRange);
    EXPECT_FLOAT_EQ(p.RangeMin, 0.0f);
    EXPECT_FLOAT_EQ(p.RangeMax, 8.0f);
    EXPECT_EQ(p.Group, "Pulse");
    EXPECT_EQ(p.VisibleIf, "enablePulse");
    EXPECT_EQ(p.Tooltip, "radians per second; 1.0 == full turn");
    EXPECT_FLOAT_EQ(Prop(t, "enablePulse").Default[0], 0.0f);
    EXPECT_TRUE(Prop(t, "glow").Hdr) << "a bare flag after a key=value is still a flag";
    EXPECT_FLOAT_EQ(Prop(t, "glow").Default[0], 2.4f);
    EXPECT_FLOAT_EQ(Prop(t, "glow").Default[2], 0.2f);
    EXPECT_TRUE(Prop(t, "secret").Hidden);
}

TEST(ShaderPropertyGrammar, QuotedValueKeepsTrailingFlagWords)
{
    // A quoted value is verbatim: a flag word at its end is content, never a
    // flag. Unquoted values still shed trailing flags (the glow pin above).
    // "hidden" peeled out of a's tooltip would silently drop the row; "normal"
    // peeled out of b's would hard-reject the surface (texture-only flag).
    const auto t = ParseDeclaredProperties(Surface(
        "// @property float a tooltip=\"keep it hidden\" range=\"0,1\"\n"
        "// @property float b tooltip=\"looks normal\"\n"));
    ASSERT_FALSE(t.Rejected()) << Errors(t);
    EXPECT_EQ(Prop(t, "a").Tooltip, "keep it hidden");
    EXPECT_FALSE(Prop(t, "a").Hidden);
    EXPECT_TRUE(Prop(t, "a").HasRange) << "a quoted range= is still a range";
    EXPECT_FLOAT_EQ(Prop(t, "a").RangeMax, 1.0f);
    EXPECT_EQ(Prop(t, "b").Tooltip, "looks normal");
}

TEST(ShaderPropertyGrammar, DisplayNameDerivesFromCamelCaseWhenAbsent)
{
    const auto t = ParseDeclaredProperties(Surface(
        "// @property float fresnelRimCap\n"
        "// @property float roughness\n"
        "// @property float ao\n"));
    ASSERT_FALSE(t.Rejected()) << Errors(t);
    EXPECT_EQ(Prop(t, "fresnelRimCap").DisplayName, "Fresnel Rim Cap");
    EXPECT_EQ(Prop(t, "roughness").DisplayName, "Roughness");
    EXPECT_EQ(Prop(t, "ao").DisplayName, "AO");
}

TEST(ShaderPropertyGrammar, DefaultsWithSpacesAfterCommasParse)
{
    const auto t = ParseDeclaredProperties(Surface("// @property color tint default=1, 0.5, 0.2 hdr\n"));
    ASSERT_FALSE(t.Rejected()) << Errors(t);
    EXPECT_FLOAT_EQ(Prop(t, "tint").Default[1], 0.5f);
    EXPECT_TRUE(Prop(t, "tint").Hdr);
}

TEST(ShaderPropertyGrammar, IndentedDeclarationsAndOmittedDefaultsParse)
{
    const auto t = ParseDeclaredProperties(Surface(
        "    // @property float a\n"
        "\t// @property color c\n"));
    ASSERT_FALSE(t.Rejected()) << Errors(t);
    EXPECT_FLOAT_EQ(Prop(t, "a").Default[0], 0.0f);
    EXPECT_FLOAT_EQ(Prop(t, "c").Default[0], 1.0f) << "an undefaulted color is white";
    EXPECT_FALSE(Prop(t, "c").HasAlpha);
}

TEST(ShaderPropertyGrammar, MidProseMentionAndSuffixedTagAreNotDeclarations)
{
    const auto t = ParseDeclaredProperties(Surface(
        "// Declare parameters with // @property float foo default=1 lines.\n"
        "// @propertyX float bar\n"
        "// see @property for the grammar\n"
        "float notAComment; // @property float baz\n"));
    EXPECT_FALSE(t.Rejected()) << Errors(t);
    EXPECT_TRUE(t.Properties.empty());
}

TEST(ShaderPropertyGrammar, UnknownTypeIsAnErrorWithFileAndLine)
{
    const auto t = ParseDeclaredProperties(Surface("// header\n// @property flaot roughness default=0.5\n", "water.glsl"));
    ASSERT_TRUE(t.Rejected());
    ASSERT_EQ(t.Errors.size(), 1u);
    EXPECT_EQ(t.Errors[0].File, "water.glsl");
    EXPECT_EQ(t.Errors[0].Line, 2u);
    EXPECT_NE(t.Errors[0].Message.find("flaot"), std::string::npos);
    EXPECT_EQ(t.Errors[0].Format("error").rfind("water.glsl:2: error: ", 0), 0u);
}

TEST(ShaderPropertyGrammar, MalformedRangeUnknownAttributeAndBadTokenAreErrors)
{
    EXPECT_TRUE(ParseDeclaredProperties(Surface("// @property float a range=1\n")).Rejected());
    EXPECT_TRUE(ParseDeclaredProperties(Surface("// @property float a range=1,0\n")).Rejected());
    EXPECT_TRUE(ParseDeclaredProperties(Surface("// @property float a rnage=0,1\n")).Rejected());
    EXPECT_TRUE(ParseDeclaredProperties(Surface("// @property float a Unquoted Display\n")).Rejected());
    EXPECT_TRUE(ParseDeclaredProperties(Surface("// @property vec3 a default=1,2\n")).Rejected());
    EXPECT_TRUE(ParseDeclaredProperties(Surface("// @property float ge_secret\n")).Rejected());
    EXPECT_TRUE(ParseDeclaredProperties(Surface("// @property float 9lives\n")).Rejected());
    EXPECT_TRUE(ParseDeclaredProperties(Surface("// @property enum e default=A\n")).Rejected());
    EXPECT_TRUE(ParseDeclaredProperties(Surface("// @property float a hdr\n")).Rejected());
}

TEST(ShaderPropertyGrammar, NonFiniteNumbersAreErrors)
{
    EXPECT_TRUE(ParseDeclaredProperties(Surface("// @property float a range=nan,inf\n")).Rejected());
    EXPECT_TRUE(ParseDeclaredProperties(Surface("// @property float a default=inf\n")).Rejected());
    EXPECT_TRUE(ParseDeclaredProperties(Surface("// @property vec2 a default=1,nan\n")).Rejected());
    EXPECT_TRUE(ParseDeclaredProperties(Surface("// @property color a default=1,1,1e39\n")).Rejected());
    EXPECT_FALSE(ParseDeclaredProperties(Surface("// @property float a default=-1e30 range=-1e30,1e30\n")).Rejected());
}

TEST(ShaderPropertyGrammar, NamesTheComposedProgramDefinesAreErrors)
{
    for (const char* name : {"Props", "Mat", "uBaseColor", "uParams3", "uParams24", "uUser0", "uUserColor"})
    {
        const auto t = ParseDeclaredProperties(Surface(std::string("// @property float ") + name + "\n"));
        ASSERT_TRUE(t.Rejected()) << name;
        EXPECT_NE(t.Errors[0].Message.find("reserved"), std::string::npos) << name;
    }
    // Macros are case-sensitive and a prefix is the whole word.
    EXPECT_FALSE(ParseDeclaredProperties(Surface("// @property float props\n")).Rejected());
    EXPECT_FALSE(ParseDeclaredProperties(Surface("// @property float uParticles\n")).Rejected());
}

TEST(ShaderPropertyGrammar, TexturePropertiesAreRejectedUntilTheyExist)
{
    const auto t = ParseDeclaredProperties(Surface("// @property texture2D albedoMap srgb\n"));
    ASSERT_TRUE(t.Rejected());
    EXPECT_NE(t.Errors[0].Message.find("@texture"), std::string::npos);
}

TEST(ShaderPropertyGrammar, DuplicateNameInOneFileIsAnError)
{
    const auto t = ParseDeclaredProperties(Surface(
        "// @property float a\n"
        "// @property float a\n"));
    ASSERT_TRUE(t.Rejected());
    EXPECT_EQ(t.Errors[0].Line, 2u);
}

// ---- Packing ----

TEST(ShaderPropertyPacking, DeclarationOrderFirstFitWithVec3ScalarSharingAndVec2Alignment)
{
    const auto t = BuildShaderPropertyTable({Surface(
        "// @property color tint default=1,1,1\n"      // lane 0 .xyz
        "// @property float power default=1\n"         // lane 0 .w
        "// @property vec2 scroll default=0,0\n"       // lane 1 .xy
        "// @property float a\n"                       // lane 1 .z
        "// @property vec2 tiling default=1,1\n"       // lane 2 .xy (pair aligned; .w of lane 1 is a single slot)
        "// @property float b\n"                       // lane 1 .w
        "// @property vec4 rect default=0,0,1,1\n"     // lane 3
        "// @property bool flag\n")});                 // lane 2 .z
    ASSERT_FALSE(t.Rejected()) << Errors(t);
    EXPECT_TRUE(t.HasSurfaceDeclarations);

    EXPECT_EQ(Prop(t, "tint").Lane, 0u);
    EXPECT_EQ(Prop(t, "tint").Component, 0u);
    EXPECT_EQ(Prop(t, "tint").ByteOffset, 0u);
    EXPECT_EQ(Prop(t, "tint").ByteSize, 12u);
    EXPECT_EQ(Prop(t, "power").Lane, 0u);
    EXPECT_EQ(Prop(t, "power").Component, 3u) << "a scalar takes the vec3's free .w";
    EXPECT_EQ(Prop(t, "power").ByteOffset, 12u);
    EXPECT_EQ(Prop(t, "scroll").Lane, 1u);
    EXPECT_EQ(Prop(t, "scroll").Component, 0u);
    EXPECT_EQ(Prop(t, "a").Lane, 1u);
    EXPECT_EQ(Prop(t, "a").Component, 2u);
    EXPECT_EQ(Prop(t, "tiling").Lane, 2u) << "vec2 needs an 8-byte-aligned pair; lane 1 has only .w free";
    EXPECT_EQ(Prop(t, "tiling").Component, 0u);
    EXPECT_EQ(Prop(t, "b").Lane, 1u);
    EXPECT_EQ(Prop(t, "b").Component, 3u);
    EXPECT_EQ(Prop(t, "rect").Lane, 3u) << "vec4 waits for a whole lane";
    EXPECT_EQ(Prop(t, "rect").ByteOffset, 48u);
    EXPECT_EQ(Prop(t, "rect").ByteSize, 16u);
    EXPECT_EQ(Prop(t, "flag").Lane, 2u);
    EXPECT_EQ(Prop(t, "flag").Component, 2u);
    EXPECT_EQ(t.LanesUsed, 4u);
}

TEST(ShaderPropertyPacking, AppendingADeclarationKeepsEveryEarlierOffset)
{
    const std::string base =
        "// @property color tint\n"
        "// @property float power\n"
        "// @property vec2 scroll\n"
        "// @property float a\n";
    const auto before = BuildShaderPropertyTable({Surface(base)});
    const auto after = BuildShaderPropertyTable({Surface(base + "// @property vec4 extra\n// @property float tail\n")});
    ASSERT_FALSE(before.Rejected()) << Errors(before);
    ASSERT_FALSE(after.Rejected()) << Errors(after);
    ASSERT_EQ(after.Properties.size(), before.Properties.size() + 2);
    for (size_t i = 0; i < before.Properties.size(); ++i)
    {
        EXPECT_EQ(after.Properties[i].Name, before.Properties[i].Name);
        EXPECT_EQ(after.Properties[i].ByteOffset, before.Properties[i].ByteOffset) << before.Properties[i].Name;
        EXPECT_EQ(after.Properties[i].ByteSize, before.Properties[i].ByteSize) << before.Properties[i].Name;
    }
}

TEST(ShaderPropertyPacking, BudgetOverflowIsAnErrorNamingTheSurface)
{
    std::string src;
    for (uint32_t i = 0; i < kMaterialParamLaneCount; ++i)
        src += "// @property vec4 v" + std::to_string(i) + "\n";
    ASSERT_FALSE(BuildShaderPropertyTable({Surface(src)}).Rejected()) << "exactly the budget fits";
    src += "// @property float one_too_many\n";
    const auto t = BuildShaderPropertyTable({Surface(src, "big.glsl")});
    ASSERT_TRUE(t.Rejected());
    EXPECT_EQ(t.Errors[0].File, "big.glsl");
    EXPECT_EQ(t.Errors[0].Line, kMaterialParamLaneCount + 1);
    EXPECT_NE(t.Errors[0].Message.find("one_too_many"), std::string::npos);
    EXPECT_NE(t.Errors[0].Message.find(std::to_string(kMaterialParamLaneCount * 4)), std::string::npos);
}

TEST(ShaderPropertyPacking, FragmentationRejectionNamesThePackingNotTheBudget)
{
    // One vec3 per lane plus one more: fewer floats than the block holds, but a
    // vec3 must start a lane, so placement — not the float budget — is what fails.
    std::string src;
    for (uint32_t i = 0; i < kMaterialParamLaneCount + 1; ++i)
        src += "// @property vec3 v" + std::to_string(i) + "\n";
    const auto t = BuildShaderPropertyTable({Surface(src, "fragmented.glsl")});
    ASSERT_TRUE(t.Rejected());
    EXPECT_NE(t.Errors[0].Message.find("vec3"), std::string::npos) << t.Errors[0].Message;
    EXPECT_NE(t.Errors[0].Message.find("floats free"), std::string::npos) << t.Errors[0].Message;
    EXPECT_EQ(t.Errors[0].Message.find("more than"), std::string::npos)
        << "fragmentation is not a budget overflow\n" << t.Errors[0].Message;
}

TEST(ShaderPropertyGrammar, KeywordAttributeIsRejectedUntilItIsWired)
{
    const auto t = ParseDeclaredProperties(Surface("// @property bool dissolve keyword=DISSOLVE\n"));
    ASSERT_TRUE(t.Rejected());
    EXPECT_NE(t.Errors[0].Message.find("GE_USER_DISSOLVE"), std::string::npos);
}

// ---- Union of producers ----

TEST(ShaderPropertyUnion, AdapterDeclarationsPackFirstOnlyWhenTheSurfaceDeclaresThem)
{
    const auto t = BuildShaderPropertyTable({Adapter(kAdapterDecls),
                                             Surface("// @property color tint\n"
                                                     "// @property float alphaCutoff default=0.3\n")});
    ASSERT_FALSE(t.Rejected()) << Errors(t);
    ASSERT_EQ(t.Properties.size(), 5u) << "four adapter reads + tint; alphaCutoff merges into one slot";
    EXPECT_EQ(t.Properties[0].Name, "alphaCutoff") << "the shared slot sits at the adapter's position";
    EXPECT_TRUE(Prop(t, "alphaCutoff").HasLane);
    EXPECT_EQ(Prop(t, "alphaCutoff").Lane, 0u);
    EXPECT_EQ(Prop(t, "alphaCutoff").Component, 0u);
    EXPECT_FLOAT_EQ(Prop(t, "alphaCutoff").Default[0], 0.3f) << "the surface's declaration owns the metadata";
    EXPECT_FALSE(Prop(t, "specularIor").HasLane) << "an adapter read nobody stores is a constant";
    EXPECT_FALSE(Prop(t, "transmissionColor").HasLane);
    EXPECT_FALSE(Prop(t, "transmissionWeight").HasLane);
    EXPECT_FLOAT_EQ(Prop(t, "specularIor").Default[0], 1.5f);
    EXPECT_EQ(Prop(t, "tint").Lane, 1u) << "the vec3 starts the next lane; .w of lane 0 stays free";
}

TEST(ShaderPropertyUnion, AdapterOnlyDeclarationsMakeALegacyTable)
{
    const auto t = BuildShaderPropertyTable({Adapter(kAdapterDecls), Surface("SurfaceOutput EvaluateSurface(SurfaceInput s) {}\n")});
    ASSERT_FALSE(t.Rejected()) << Errors(t);
    EXPECT_FALSE(t.HasSurfaceDeclarations);
    EXPECT_EQ(t.Properties.size(), 4u);
    for (const auto& p : t.Properties)
        EXPECT_FALSE(p.HasLane) << p.Name;
    EXPECT_EQ(t.LanesUsed, 0u);
}

TEST(ShaderPropertyUnion, VertexModifierDeclarationsFollowTheSurface)
{
    const auto t = BuildShaderPropertyTable({Adapter(kAdapterDecls),
                                             Surface("// @property float a\n"),
                                             Modifier("// @property vec4 wind default=0,0,0,0\n// @property float a\n")});
    ASSERT_FALSE(t.Rejected()) << Errors(t);
    EXPECT_EQ(Prop(t, "a").Lane, 0u);
    EXPECT_EQ(Prop(t, "wind").Lane, 1u);
    EXPECT_EQ(Prop(t, "wind").Origin, ShaderPropertyOrigin::VertexModifier);
    EXPECT_EQ(t.Properties.size(), 6u) << "a re-declared name merges into one slot";
}

TEST(ShaderPropertyUnion, SameNameDifferentTypeIsAnErrorAtTheSecondDeclaration)
{
    const auto t = BuildShaderPropertyTable({Adapter(kAdapterDecls),
                                             Surface("// header\n// @property vec3 alphaCutoff\n", "s.glsl")});
    ASSERT_TRUE(t.Rejected());
    EXPECT_EQ(t.Errors[0].File, "s.glsl");
    EXPECT_EQ(t.Errors[0].Line, 2u);
    EXPECT_NE(t.Errors[0].Message.find("adapter_forward.glsl:1"), std::string::npos);
}
