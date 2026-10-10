// visibleIf is the one piece of the declared-property inspector that is a small
// language: `name`, `name=value`, `name!=value`, resolved against the declared
// properties and then the document's own alphaMode / lightingModel. Getting it
// wrong hides an editable value with no error anywhere, so the rules are pinned
// here rather than left to a screenshot.

#include <gtest/gtest.h>

#include "Inspectors/DeclaredPropertyRowModel.h"
#include "Rendering/Materials/MaterialAlphaMode.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderPropertyTable.h"

#include <array>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{

Rendering::ShaderPropertyTable Table()
{
    return Rendering::BuildShaderPropertyTable(
        {{"// @property bool  enableFresnel default=true\n"
          "// @property float fresnelPower  default=3 range=0,16 visibleIf=enableFresnel\n"
          "// @property enum  blendMode     values=Add,Multiply,Screen default=1\n"
          "// @property float amount        default=0.25\n",
          "surface.glsl", Rendering::ShaderPropertyOrigin::Surface}});
}

MaterialDocument Doc()
{
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.lightingModel = "StandardPBR";
    doc.alphaMode = MaterialAlphaMode::Opaque;
    return doc;
}

} // namespace

TEST(DeclaredPropertyRowModel, AnEmptyExpressionIsNoCondition)
{
    const Rendering::ShaderPropertyTable table = Table();
    EXPECT_TRUE(Editor::EvaluateVisibleIf("", table, Doc()));
}

TEST(DeclaredPropertyRowModel, ABareNameFollowsTheBoolItNames)
{
    const Rendering::ShaderPropertyTable table = Table();
    MaterialDocument doc = Doc();
    EXPECT_TRUE(Editor::EvaluateVisibleIf("enableFresnel", table, doc))
        << "the declared default is true, so the row shows before anything is authored";
    doc.properties["enableFresnel"] = false;
    EXPECT_FALSE(Editor::EvaluateVisibleIf("enableFresnel", table, doc));
}

TEST(DeclaredPropertyRowModel, AnEnumComparesAgainstItsLabelNotItsIndex)
{
    const Rendering::ShaderPropertyTable table = Table();
    MaterialDocument doc = Doc();
    EXPECT_TRUE(Editor::EvaluateVisibleIf("blendMode=Multiply", table, doc)) << "default is index 1";
    EXPECT_FALSE(Editor::EvaluateVisibleIf("blendMode=Add", table, doc));
    doc.properties["blendMode"] = static_cast<int32_t>(2);
    EXPECT_TRUE(Editor::EvaluateVisibleIf("blendMode=Screen", table, doc));
    EXPECT_TRUE(Editor::EvaluateVisibleIf("blendMode!=Multiply", table, doc));
}

TEST(DeclaredPropertyRowModel, ComparisonIsCaseInsensitive)
{
    const Rendering::ShaderPropertyTable table = Table();
    EXPECT_TRUE(Editor::EvaluateVisibleIf("blendMode=multiply", table, Doc()));
}

TEST(DeclaredPropertyRowModel, DocumentFieldsResolveWhenNoPropertyOwnsTheName)
{
    const Rendering::ShaderPropertyTable table = Table();
    MaterialDocument doc = Doc();
    EXPECT_FALSE(Editor::EvaluateVisibleIf("alphaMode=Mask", table, doc));
    doc.alphaMode = MaterialAlphaMode::Mask;
    EXPECT_TRUE(Editor::EvaluateVisibleIf("alphaMode=Mask", table, doc));
    EXPECT_TRUE(Editor::EvaluateVisibleIf("lightingModel=StandardPBR", table, doc));
    EXPECT_FALSE(Editor::EvaluateVisibleIf("lightingModel=Unlit", table, doc));
}

// Hiding a row the author can no longer reach is worse than showing one that
// does nothing, so an expression naming nothing shows the row.
TEST(DeclaredPropertyRowModel, AnUnknownNameShowsTheRow)
{
    const Rendering::ShaderPropertyTable table = Table();
    EXPECT_TRUE(Editor::EvaluateVisibleIf("noSuchProperty", table, Doc()));
    EXPECT_TRUE(Editor::EvaluateVisibleIf("noSuchProperty=1", table, Doc()));
}

TEST(DeclaredPropertyRowModel, AFloatComparesByItsPrintedValue)
{
    const Rendering::ShaderPropertyTable table = Table();
    MaterialDocument doc = Doc();
    EXPECT_TRUE(Editor::EvaluateVisibleIf("amount=0.25", table, doc));
    doc.properties["amount"] = 0.5f;
    EXPECT_TRUE(Editor::EvaluateVisibleIf("amount!=0.25", table, doc));
    EXPECT_TRUE(Editor::EvaluateVisibleIf("amount", table, doc)) << "non-zero is truthy";
    doc.properties["amount"] = 0.0f;
    EXPECT_FALSE(Editor::EvaluateVisibleIf("amount", table, doc));
}

TEST(DeclaredPropertyRowModel, ReadDeclaredValueFallsBackToTheDeclaredDefault)
{
    const Rendering::ShaderPropertyTable table = Table();
    const Rendering::ShaderProperty* power = table.Find("fresnelPower");
    ASSERT_NE(power, nullptr);

    MaterialDocument doc = Doc();
    std::array<float, 4> value{};
    EXPECT_FALSE(Editor::ReadDeclaredValue(doc, *power, value)) << "nothing authored";
    EXPECT_FLOAT_EQ(value[0], 3.0f);

    doc.properties["fresnelPower"] = 7.5f;
    EXPECT_TRUE(Editor::ReadDeclaredValue(doc, *power, value));
    EXPECT_FLOAT_EQ(value[0], 7.5f);
}

// A document written against a wider declaration than the surface now has must
// not write past the property's components.
TEST(DeclaredPropertyRowModel, AnOverlongAuthoredVectorIsClampedToTheDeclaredWidth)
{
    const Rendering::ShaderPropertyTable table = Rendering::BuildShaderPropertyTable(
        {{"// @property vec2 scrollSpeed default=0,0.2\n", "surface.glsl",
          Rendering::ShaderPropertyOrigin::Surface}});
    const Rendering::ShaderProperty* scroll = table.Find("scrollSpeed");
    ASSERT_NE(scroll, nullptr);

    MaterialDocument doc = Doc();
    doc.properties["scrollSpeed"] = std::vector<float>{1.0f, 2.0f, 3.0f, 4.0f};
    std::array<float, 4> value{9.0f, 9.0f, 9.0f, 9.0f};
    EXPECT_TRUE(Editor::ReadDeclaredValue(doc, *scroll, value));
    EXPECT_FLOAT_EQ(value[0], 1.0f);
    EXPECT_FLOAT_EQ(value[1], 2.0f);
    EXPECT_FLOAT_EQ(value[2], 0.0f) << "the declared default's third component, not the document's";
}
